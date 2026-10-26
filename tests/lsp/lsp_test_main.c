/* tests/lsp/lsp_test_main.c — Harness in-process pour le LSP OWL.
 *
 * Lance via 'make test-lsp'. Drive directement les fonctions LSP
 * (handle_completion, etc.) sans IPC ni JSON-RPC stdio. Build separe
 * via la cible CMake 'owl-lsp-test'.
 *
 * Cible les regressions remontees lors du dev :
 *   - 'FEN_X..' propose les props Fenetre (pas Date)
 *   - 'SAI_X..' propose les props Saisie
 *   - Body Fenetre/Bouton/Libelle/Saisie : Ctrl+Espace = props du kind
 *   - Filtre des props deja utilisees
 *   - Noms de Fenetres/Champs proposes en autocomplete standard
 *
 * Chaque test affiche [OK]/[FAIL]. Code retour 0 si tous OK, 1 sinon.
 */

#include "lsp_server.h"
#include "lsp_json.h"
#include "owl_catalog.h"
#include "formatter_canon.h"
#include "lsp_proc.h"  /*  : lsp_run_check_bounded, teste directement */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>  /* _fullpath via stdlib deja inclus */
#else
#include <limits.h>   /* PATH_MAX */
#include <time.h>     /* clock_gettime pour les mesures de duree */
#endif

/* Horloge monotone en ms, pour chiffrer les durees dans les messages d'echec
 * des tests qui spawnent un process ( : un echec doit dire combien de
 * temps ca a pris, pas seulement qu'il a echoue). */
static long long lsp_test_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ================================================================
 *  Test infrastructure
 * ================================================================ */

static int g_pass = 0;
static int g_fail = 0;

static void setup_doc(const char* uri, const char* source) {
    /* Reset l'etat LSP. */
    for (int i = 0; i < lspServer.documentCount; i++) {
        free(lspServer.documents[i].content);
    }
    lspServer.documentCount = 1;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri),
             "%s", uri);
    lspServer.documents[0].content = strdup(source);
    lspServer.documents[0].contentLen = (int)strlen(source);
    lspServer.documents[0].version = 1;
}


static JsonValue* make_completion_params(const char* uri, int line, int character) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    return params;
}

/*  — variante avec `context` LSP (CompletionContext) : triggerKind
 * 2 = TriggerCharacter, triggerCharacter = le caractere declencheur. Sert a
 * simuler l'auto-popup sur '(' (par opposition au Ctrl-Space = Invoked). */
static JsonValue* make_completion_params_ctx(const char* uri, int line, int character,
                                             int triggerKind, const char* triggerChar) {
    JsonValue* params = make_completion_params(uri, line, character);
    JsonValue* ctx = json_new_object();
    json_object_set(ctx, "triggerKind", json_new_int(triggerKind));
    if (triggerChar)
        json_object_set(ctx, "triggerCharacter", json_new_string(triggerChar));
    json_object_set(params, "context", ctx);
    return params;
}

static JsonValue* make_definition_params(const char* uri, int line, int character) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    return params;
}

/* Verifie que handle_definition renvoie une Location dont range.start.line == expLine.
 * En mode TDD (avant impl), le handler renvoie json_new_null() → FAIL attendu. */
#define ASSERT_DEF_LINE(result, expLine, test_name) do { \
    JsonValue* _r = (result); \
    if (_r && _r->type == JSON_OBJECT) { \
        JsonValue* _rng = json_get(_r, "range"); \
        JsonValue* _st  = _rng ? json_get(_rng, "start") : NULL; \
        int _got = (int)json_get_int(_st, "line", -999); \
        if (_got == (expLine)) { \
            g_pass++; \
            printf("  [OK]   %s : Location.line = %d\n", test_name, expLine); \
        } else { \
            g_fail++; \
            printf("  [FAIL] %s : attendu line=%d, obtenu line=%d%s\n", \
                   test_name, expLine, _got, \
                   (_r->type == JSON_NULL || _got == -999) ? " (null → handler non impl)" : ""); \
        } \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu Location{line=%d}, obtenu null (handler non impl)\n", \
               test_name, expLine); \
    } \
} while (0)

/* Verifie que handle_definition renvoie null propre (pas de crash, pas de Location). */
#define ASSERT_DEF_NULL(result, test_name) do { \
    JsonValue* _r = (result); \
    bool _isNull = (!_r || _r->type == JSON_NULL); \
    if (_isNull) { \
        g_pass++; \
        printf("  [OK]   %s : null propre (pas de crash)\n", test_name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu null, obtenu Location\n", test_name); \
    } \
} while (0)

/*  Lot 1 — forge des params textDocument/references.
 * context.includeDeclaration suit la spec LSP standard. */
static JsonValue* make_references_params(const char* uri, int line, int character,
                                          bool includeDeclaration) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    JsonValue* ctx = json_new_object();
    json_object_set(ctx, "includeDeclaration", json_new_bool(includeDeclaration));
    json_object_set(params, "context", ctx);
    return params;
}

/* Verifie que handle_references renvoie un tableau JSON de N Location.
 * En mode TDD (avant impl), le stub renvoie [] → FAIL attendu (0 vs N). */
#define ASSERT_REFS_COUNT(result, expCount, test_name) do { \
    JsonValue* _r = (result); \
    int _got = (_r && _r->type == JSON_ARRAY) ? _r->as.arrayVal.count : -1; \
    if (_got == (expCount)) { \
        g_pass++; \
        printf("  [OK]   %s : %d reference(s)\n", test_name, expCount); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu %d reference(s), obtenu %d%s\n", \
               test_name, (expCount), _got, \
               (_got <= 0) ? " (stub non implemente)" : ""); \
    } \
} while (0)

/* Verifie qu'aucune Location du tableau ne pointe la ligne donnee
 * (isolation des soeurs — verrou central du Lot 1). */
static bool refs_contains_line(JsonValue* result, int line) {
    if (!result || result->type != JSON_ARRAY) return false;
    for (int i = 0; i < result->as.arrayVal.count; i++) {
        JsonValue* loc = result->as.arrayVal.items[i];
        JsonValue* rng = json_get(loc, "range");
        JsonValue* st  = rng ? json_get(rng, "start") : NULL;
        int gotLine = (int)json_get_int(st, "line", -999);
        if (gotLine == line) return true;
    }
    return false;
}

#define ASSERT_REFS_EXCLUDES_LINE(result, forbiddenLine, test_name) do { \
    if (!refs_contains_line((result), (forbiddenLine))) { \
        g_pass++; \
        printf("  [OK]   %s : ligne %d absente (isolation soeurs)\n", test_name, (forbiddenLine)); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : ligne %d presente a tort (fuite cross-scope)\n", test_name, (forbiddenLine)); \
    } \
} while (0)

/*  Lot 2a — forge des params textDocument/rename et
 * textDocument/prepareRename. */
static JsonValue* make_rename_params(const char* uri, int line, int character,
                                      const char* newName) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    json_object_set(params, "newName", json_new_string(newName));
    return params;
}

static JsonValue* make_prepare_rename_params(const char* uri, int line, int character) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    return params;
}

/* Recupere le tableau de TextEdit pour un uri donne dans un WorkspaceEdit
 * { "changes": { uri: [ TextEdit... ] } }. NULL si absent. */
static JsonValue* rename_edits_for_uri(JsonValue* result, const char* uri) {
    if (!result || result->type != JSON_OBJECT) return NULL;
    JsonValue* changes = json_get(result, "changes");
    if (!changes || changes->type != JSON_OBJECT) return NULL;
    return json_get(changes, uri);
}

/* Verifie le nombre de TextEdit pour un uri dans le WorkspaceEdit. */
#define ASSERT_RENAME_EDIT_COUNT(result, uri, expCount, test_name) do { \
    JsonValue* _edits = rename_edits_for_uri((result), (uri)); \
    int _got = (_edits && _edits->type == JSON_ARRAY) ? _edits->as.arrayVal.count : -1; \
    if (_got == (expCount)) { \
        g_pass++; \
        printf("  [OK]   %s : %d TextEdit\n", test_name, expCount); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu %d TextEdit, obtenu %d%s\n", \
               test_name, (expCount), _got, \
               (_got <= 0) ? " (stub non implemente)" : ""); \
    } \
} while (0)

/* Verifie qu'aucun TextEdit du tableau (uri donne) ne cible la ligne
 * interdite (isolation des soeurs — verrou central rename). */
static bool rename_edits_contains_line(JsonValue* result, const char* uri, int line) {
    JsonValue* edits = rename_edits_for_uri(result, uri);
    if (!edits || edits->type != JSON_ARRAY) return false;
    for (int i = 0; i < edits->as.arrayVal.count; i++) {
        JsonValue* edit = edits->as.arrayVal.items[i];
        JsonValue* rng = json_get(edit, "range");
        JsonValue* st  = rng ? json_get(rng, "start") : NULL;
        int gotLine = (int)json_get_int(st, "line", -999);
        if (gotLine == line) return true;
    }
    return false;
}

#define ASSERT_RENAME_EXCLUDES_LINE(result, uri, forbiddenLine, test_name) do { \
    if (!rename_edits_contains_line((result), (uri), (forbiddenLine))) { \
        g_pass++; \
        printf("  [OK]   %s : ligne %d absente (isolation soeurs)\n", test_name, (forbiddenLine)); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : ligne %d presente a tort (fuite cross-scope)\n", test_name, (forbiddenLine)); \
    } \
} while (0)

/* Verifie que TOUS les TextEdit du tableau (uri donne) portent le
 * newText exact attendu (verbatim, pas de canonicalisation). */
#define ASSERT_RENAME_NEWTEXT_ALL(result, uri, expText, test_name) do { \
    JsonValue* _edits = rename_edits_for_uri((result), (uri)); \
    bool _allOk = (_edits && _edits->type == JSON_ARRAY && _edits->as.arrayVal.count > 0); \
    if (_allOk) { \
        for (int _i = 0; _i < _edits->as.arrayVal.count; _i++) { \
            const char* _nt = json_get_string(_edits->as.arrayVal.items[_i], "newText"); \
            if (!_nt || strcmp(_nt, (expText)) != 0) { _allOk = false; break; } \
        } \
    } \
    if (_allOk) { \
        g_pass++; \
        printf("  [OK]   %s : newText verbatim = \"%s\" sur tous les edits\n", test_name, (expText)); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : newText attendu verbatim \"%s\" absent ou divergent (stub non implemente ?)\n", \
               test_name, (expText)); \
    } \
} while (0)

/* Verifie que handle_prepare_rename renvoie un Range non-null (mot renommable). */
#define ASSERT_PREPARE_RENAME_NOT_NULL(result, test_name) do { \
    JsonValue* _r = (result); \
    bool _hasRange = (_r && _r->type == JSON_OBJECT && json_get(_r, "start") != NULL); \
    if (_hasRange) { \
        g_pass++; \
        printf("  [OK]   %s : Range non-null (renommable)\n", test_name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu Range, obtenu null (stub non implemente)\n", test_name); \
    } \
} while (0)

/* Verifie que handle_prepare_rename renvoie null (mot non-renommable). */
#define ASSERT_PREPARE_RENAME_NULL(result, test_name) do { \
    JsonValue* _r = (result); \
    bool _isNull = (!_r || _r->type == JSON_NULL); \
    if (_isNull) { \
        g_pass++; \
        printf("  [OK]   %s : null propre (refus attendu)\n", test_name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu null, obtenu Range\n", test_name); \
    } \
} while (0)

/* Unwrap CompletionList { isIncomplete, items } → items array, ou
 * retourne tel quel si deja un array. */
static JsonValue* unwrap_items(JsonValue* r) {
    if (!r) return NULL;
    if (r->type == JSON_ARRAY) return r;
    if (r->type == JSON_OBJECT) return json_get(r, "items");
    return NULL;
}

/* Cherche un item avec ce label dans le tableau d'items. */
static bool has_label(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return false;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0) return true;
    }
    return false;
}

static int count_items(JsonValue* r) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return 0;
    return items->as.arrayVal.count;
}

/*  — nombre d'items portant exactement `label`. Sert a prouver
 * l'absence de doublon de nom hors-portee (une locale valide + une locale
 * homonyme d'une autre procedure proposee a tort : 2 avant fix, 1 apres). */
static int count_label(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return 0;
    int n = 0;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        const char* lbl = json_get_string(items->as.arrayVal.items[i], "label");
        if (lbl && strcmp(lbl, label) == 0) n++;
    }
    return n;
}

/*  — dump des labels obtenus, pour qu'un echec de cardinal dise
 * QUELS items sont en trop plutot que juste combien. */
static void dump_labels(JsonValue* r) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) { printf("(aucun)"); return; }
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        const char* lbl = json_get_string(items->as.arrayVal.items[i], "label");
        printf("%s%s", i ? ", " : "", lbl ? lbl : "?");
    }
}

/* assert helpers */
#define ASSERT_HAS(items, label, test_name) do { \
    if (has_label(items, label)) { \
        g_pass++; \
        printf("  [OK]   %s : '%s' present\n", test_name, label); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : '%s' MANQUANT (total items=%d)\n", \
               test_name, label, count_items(items)); \
    } \
} while (0)

#define ASSERT_NOT_HAS(items, label, test_name) do { \
    if (!has_label(items, label)) { \
        g_pass++; \
        printf("  [OK]   %s : '%s' absent (filtre OK)\n", test_name, label); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : '%s' PRESENT alors qu'attendu absent\n", \
               test_name, label); \
    } \
} while (0)

/*  — assertion sur le NOMBRE exact d'items. Une liste filtree par
 * type se mesure a son cardinal : « la bonne propriete est proposee » ne dit
 * rien de celles qui n'auraient pas du l'etre. En cas d'echec on dump les
 * labels obtenus, sinon le chiffre seul ne dit pas ou est la fuite. */
#define ASSERT_COUNT(items, expected, test_name) do { \
    int _got = count_items(items); \
    if (_got == (expected)) { \
        g_pass++; \
        printf("  [OK]   %s : %d item(s)\n", test_name, _got); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu %d item(s), obtenu %d -> ", \
               test_name, (expected), _got); \
        dump_labels(items); \
        printf("\n"); \
    } \
} while (0)

/* Cherche un item par label et verifie que son champ `detail` commence
 * par le prefixe attendu. Utilise pour  (harmonisation prefixe
 * `Lifecycle :` entre snippets lifecycle widget et classe). */
static bool item_detail_starts_with(JsonValue* r, const char* label,
                                    const char* prefix) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return false;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (!lbl || strcmp(lbl, label) != 0) continue;
        const char* det = json_get_string(item, "detail");
        if (!det) return false;
        return strncmp(det, prefix, strlen(prefix)) == 0;
    }
    return false;
}

#define ASSERT_DETAIL_PREFIX(items, label, prefix, test_name) do { \
    if (item_detail_starts_with(items, label, prefix)) { \
        g_pass++; \
        printf("  [OK]   %s : '%s' detail prefixe par '%s'\n", \
               test_name, label, prefix); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : '%s' detail NE commence PAS par '%s'\n", \
               test_name, label, prefix); \
    } \
} while (0)

/* Cherche un item par label et verifie que son champ `documentation`
 * (string brute, ou objet markdown {kind,value}) contient `needle`.
 * Tolere les deux formes : certains items posent documentation en
 * string simple, d'autres en objet markdown. */
static bool item_doc_contains(JsonValue* r, const char* label,
                              const char* needle) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return false;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (!lbl || strcmp(lbl, label) != 0) continue;
        JsonValue* doc = json_get(item, "documentation");
        if (!doc) return false;
        const char* val = NULL;
        if (doc->type == JSON_STRING) val = doc->as.stringVal.str;
        else if (doc->type == JSON_OBJECT) val = json_get_string(doc, "value");
        return val && strstr(val, needle) != NULL;
    }
    return false;
}

#define ASSERT_DOC_CONTAINS(items, label, needle, test_name) do { \
    if (item_doc_contains(items, label, needle)) { \
        g_pass++; \
        printf("  [OK]   %s : '%s' doc contient \"%s\"\n", \
               test_name, label, needle); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : '%s' doc NE contient PAS \"%s\"\n", \
               test_name, label, needle); \
    } \
} while (0)

/* Renvoie l'insertText d'un item identifie par son label (NULL sinon). */
static const char* item_insert_text(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0)
            return json_get_string(item, "insertText");
    }
    return NULL;
}

/* Renvoie insertTextFormat d'un item identifie par son label (-1 sinon). */
static int item_insert_format(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return -1;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0)
            return (int)json_get_int(item, "insertTextFormat", -1);
    }
    return -1;
}

/* Verifie que l'item `label` porte insertTextFormat=2 (snippet LSP). */
#define ASSERT_INSERT_FORMAT_SNIPPET(items, label, test_name) do { \
    int _f = item_insert_format((items), (label)); \
    if (_f == 2) { \
        g_pass++; \
        printf("  [OK]   %s : insertTextFormat=2 (snippet)\n", test_name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : insertTextFormat=%d (attendu 2)\n", \
               test_name, _f); \
    } \
} while (0)

/* Verifie que l'insertText de `label` contient `needle`. */
#define ASSERT_INSERT_CONTAINS(items, label, needle, test_name) do { \
    const char* _it = item_insert_text((items), (label)); \
    if (_it && strstr(_it, (needle)) != NULL) { \
        g_pass++; \
        printf("  [OK]   %s : insertText '%s' contient \"%s\"\n", \
               test_name, label, needle); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : insertText '%s' NE contient PAS \"%s\" (got='%s')\n", \
               test_name, label, needle, _it ? _it : "(null)"); \
    } \
} while (0)

/* Verifie que l'insertText de `label` ne contient PAS `needle`. */
#define ASSERT_INSERT_NOT_CONTAINS(items, label, needle, test_name) do { \
    const char* _it = item_insert_text((items), (label)); \
    if (_it && strstr(_it, (needle)) == NULL) { \
        g_pass++; \
        printf("  [OK]   %s : insertText '%s' sans \"%s\"\n", \
               test_name, label, needle); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : insertText '%s' contient \"%s\" (attendu absent, got='%s')\n", \
               test_name, label, needle, _it ? _it : "(null)"); \
    } \
} while (0)

/* Verifie que l'insertText de `label` vaut EXACTEMENT `expected`. */
#define ASSERT_INSERT_EQUALS(items, label, expected, test_name) do { \
    const char* _it = item_insert_text((items), (label)); \
    if (_it && strcmp(_it, (expected)) == 0) { \
        g_pass++; \
        printf("  [OK]   %s : insertText '%s' == \"%s\"\n", \
               test_name, label, expected); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : insertText '%s' == \"%s\" attendu (got='%s')\n", \
               test_name, label, expected, _it ? _it : "(null)"); \
    } \
} while (0)

/* Renvoie le sortText d'un item identifie par son label (NULL si absent
 * ou si le champ sortText n'est pas pose). */
static const char* item_sort_text(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0)
            return json_get_string(item, "sortText");
    }
    return NULL;
}

/*  — filterText d'un item identifie par son label (NULL sinon).
 * Sert a verifier que le match exact (rang 0) porte un filterText mono-token
 * desaccentue au lieu de la forme bilingue ("LOOP" au lieu de "LOOP BOUCLE"). */
static const char* item_filter_text(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0)
            return json_get_string(item, "filterText");
    }
    return NULL;
}

/*  — vrai si l'item `label` porte preselect=true (VSCode le focus par
 * defaut). Utilise pour verifier que le match exact d'un fermeur (END/FIN) est
 * preselectionne face a ses homonymes prefixes (EndsWith...). */
static bool item_preselect(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return false;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0)
            return json_get_bool(item, "preselect", false);
    }
    return false;
}

/*  -- verifie que l'item `labelPrefix` (une correspondance
 * "commence par" le fragment tape) est classe AVANT l'item `labelContains`
 * (une correspondance "contient seulement") via sortText. Les DEUX items
 * doivent poser un sortText non-NULL, et sortText(prefix) < sortText(contains)
 * au sens strcmp (ordre lexicographique, c'est la comparaison que fait
 * VSCode pour trier les completions). */
#define ASSERT_SORT_BEFORE(items, labelPrefix, labelContains, test_name) do { \
    const char* _sp = item_sort_text((items), (labelPrefix)); \
    const char* _sc = item_sort_text((items), (labelContains)); \
    if (_sp && _sc && strcmp(_sp, _sc) < 0) { \
        g_pass++; \
        printf("  [OK]   %s : sortText('%s')=\"%s\" < sortText('%s')=\"%s\"\n", \
               test_name, labelPrefix, _sp, labelContains, _sc); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : sortText('%s')=\"%s\" pas < sortText('%s')=\"%s\"\n", \
               test_name, labelPrefix, _sp ? _sp : "(null)", \
               labelContains, _sc ? _sc : "(null)"); \
    } \
} while (0)

/*  -- un litteral (sortText pose) doit trier AVANT une native.
 * Les natives n'ont pas de sortText -> VSCode retombe sur le label ; on
 * compare donc sortText(litteral) < cle-effective(native) = son label.
 * Le litteral DOIT porter un sortText non-NULL. */
#define ASSERT_SORT_LIT_BEFORE_NATIVE(items, litLabel, nativeLabel, test_name) do { \
    const char* _sl = item_sort_text((items), (litLabel)); \
    const char* _sn = item_sort_text((items), (nativeLabel)); \
    const char* _keyN = _sn ? _sn : (nativeLabel); \
    if (_sl && strcmp(_sl, _keyN) < 0) { \
        g_pass++; \
        printf("  [OK]   %s : sortText('%s')=\"%s\" < cle('%s')=\"%s\"\n", \
               test_name, litLabel, _sl, nativeLabel, _keyN); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : sortText('%s')=\"%s\" pas < cle('%s')=\"%s\"\n", \
               test_name, litLabel, _sl ? _sl : "(null)", nativeLabel, _keyN); \
    } \
} while (0)

/* ================================================================
 *   -- Helpers documentSymbol / foldingRange
 * ================================================================ */

/* Forge les params textDocument/documentSymbol ou textDocument/foldingRange
 * (structure identique : { textDocument: { uri } }). */
static JsonValue* make_text_document_params(const char* uri) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    return params;
}

/* Cherche un DocumentSymbol par son champ "name" dans un tableau. */
static JsonValue* find_sym(JsonValue* arr, const char* name) {
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < arr->as.arrayVal.count; i++) {
        JsonValue* sym = arr->as.arrayVal.items[i];
        const char* n = json_get_string(sym, "name");
        if (n && strcmp(n, name) == 0) return sym;
    }
    return NULL;
}

/* Retourne le tableau "children" d'un DocumentSymbol (NULL si absent). */
static JsonValue* sym_children(JsonValue* sym) {
    if (!sym) return NULL;
    return json_get(sym, "children");
}

/* Retourne le nombre d'enfants d'un DocumentSymbol. */
static int sym_children_count(JsonValue* sym) {
    JsonValue* ch = sym_children(sym);
    if (!ch || ch->type != JSON_ARRAY) return 0;
    return ch->as.arrayVal.count;
}

/* Retourne range.start.line d'un symbole (-1 si absent). */
static int sym_start_line(JsonValue* sym) {
    if (!sym) return -1;
    JsonValue* rng = json_get(sym, "range");
    JsonValue* st  = rng ? json_get(rng, "start") : NULL;
    return st ? (int)json_get_int(st, "line", -1) : -1;
}

/* Retourne selectionRange.start.line d'un symbole (-1 si absent). */
static int sym_sel_start_line(JsonValue* sym) {
    if (!sym) return -1;
    JsonValue* rng = json_get(sym, "selectionRange");
    JsonValue* st  = rng ? json_get(rng, "start") : NULL;
    return st ? (int)json_get_int(st, "line", -1) : -1;
}

/* Retourne range.end.line d'un symbole (-1 si absent). */
static int sym_end_line(JsonValue* sym) {
    if (!sym) return -1;
    JsonValue* rng = json_get(sym, "range");
    JsonValue* en  = rng ? json_get(rng, "end") : NULL;
    return en ? (int)json_get_int(en, "line", -1) : -1;
}

/* Retourne range.end.character d'un symbole (-1 si absent).
 *  : verifie que end.character = longueur de la ligne FIN. */
static int sym_end_char(JsonValue* sym) {
    if (!sym) return -1;
    JsonValue* rng = json_get(sym, "range");
    JsonValue* en  = rng ? json_get(rng, "end") : NULL;
    return en ? (int)json_get_int(en, "character", -1) : -1;
}

/* Verifie qu'un FoldingRange {startLine, endLine} est present dans le tableau. */
static bool has_fold(JsonValue* arr, int sl, int el) {
    if (!arr || arr->type != JSON_ARRAY) return false;
    for (int i = 0; i < arr->as.arrayVal.count; i++) {
        JsonValue* fr = arr->as.arrayVal.items[i];
        int got_sl = (int)json_get_int(fr, "startLine", -999);
        int got_el = (int)json_get_int(fr, "endLine",   -999);
        if (got_sl == sl && got_el == el) return true;
    }
    return false;
}

/* Macros assertions documentSymbol ------------------------------------ */

/* Verifie que result est un tableau avec exactement N symboles. */
#define ASSERT_SYM_ARRAY_COUNT(arr, expected, test_name) do { \
    int _cnt = ((arr) && (arr)->type == JSON_ARRAY) \
               ? (arr)->as.arrayVal.count : -1; \
    if (_cnt == (expected)) { \
        g_pass++; \
        printf("  [OK]   %s : symbols count=%d\n", test_name, expected); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu %d symbole(s), obtenu %d" \
               " (null ou handler non impl)\n", \
               test_name, expected, _cnt); \
    } \
} while (0)

/* Verifie qu'un symbole nomme "name" est present dans le tableau. */
#define ASSERT_SYM_EXISTS(arr, name, test_name) do { \
    JsonValue* _s = find_sym(arr, name); \
    if (_s) { \
        g_pass++; \
        printf("  [OK]   %s : symbole '%s' present\n", test_name, name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : symbole '%s' ABSENT (handler non impl)\n", \
               test_name, name); \
    } \
} while (0)

/* Verifie le nombre d'enfants d'un symbole. */
#define ASSERT_SYM_CHILDREN_COUNT(sym, expected, test_name) do { \
    int _cnt = sym_children_count(sym); \
    if (_cnt == (expected)) { \
        g_pass++; \
        printf("  [OK]   %s : children count=%d\n", test_name, expected); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu %d enfants, obtenu %d\n", \
               test_name, expected, _cnt); \
    } \
} while (0)

/* Verifie range.start.line et range.end.line d'un symbole. */
#define ASSERT_SYM_LINE(sym, exp_start, exp_end, test_name) do { \
    int _sl = sym_start_line(sym); \
    int _el = sym_end_line(sym); \
    if (_sl == (exp_start) && _el == (exp_end)) { \
        g_pass++; \
        printf("  [OK]   %s : range [%d..%d]\n", test_name, exp_start, exp_end); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu [%d..%d], obtenu [%d..%d]\n", \
               test_name, exp_start, exp_end, _sl, _el); \
    } \
} while (0)

/* Verifie range.end.character d'un symbole ( -- longueur ligne FIN).
 * Rouge attendu avant impl : make_symbol passe eChar=0 en dur. */
#define ASSERT_SYM_END_CHAR(sym, exp_char, test_name) do { \
    int _ec = sym_end_char(sym); \
    if (_ec == (exp_char)) { \
        g_pass++; \
        printf("  [OK]   %s : end.character=%d\n", test_name, exp_char); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu end.character=%d, obtenu %d%s\n", \
               test_name, exp_char, _ec, \
               (_ec == 0) ? " (0=eChar non calcule, voir make_symbol)" : ""); \
    } \
} while (0)

/* Macros assertions foldingRange -------------------------------------- */

/* Verifie que result est un tableau JSON valide (non NULL). */
#define ASSERT_FOLD_ARRAY_VALID(arr, test_name) do { \
    if ((arr) && (arr)->type == JSON_ARRAY) { \
        g_pass++; \
        printf("  [OK]   %s : foldingRange array valide (%d elements)\n", \
               test_name, (arr)->as.arrayVal.count); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : foldingRange null/invalide (handler non impl)\n", \
               test_name); \
    } \
} while (0)

/* Verifie qu'une plage [startLine..endLine] est presente dans le tableau. */
#define ASSERT_HAS_FOLD(arr, sl, el, test_name) do { \
    if (has_fold(arr, sl, el)) { \
        g_pass++; \
        printf("  [OK]   %s : foldingRange [%d..%d] present\n", \
               test_name, sl, el); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : foldingRange [%d..%d] ABSENT (handler non impl)\n", \
               test_name, sl, el); \
    } \
} while (0)

/* Verifie le nombre total de plages repliables. */
#define ASSERT_FOLD_COUNT(arr, expected, test_name) do { \
    int _cnt = ((arr) && (arr)->type == JSON_ARRAY) \
               ? (arr)->as.arrayVal.count : -1; \
    if (_cnt == (expected)) { \
        g_pass++; \
        printf("  [OK]   %s : foldingRange count=%d\n", test_name, expected); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu %d plages, obtenu %d (handler non impl)\n", \
               test_name, expected, _cnt); \
    } \
} while (0)

/* LSP SymbolKind values numeriques (cf. LSP spec 3.17).
 * Utilises dans ASSERT_SYM_KIND pour verrouiller les kinds emis. */
#define LSP_KIND_MODULE     2
#define LSP_KIND_NAMESPACE  3
#define LSP_KIND_CLASS      5
#define LSP_KIND_METHOD     6
#define LSP_KIND_PROPERTY   7
#define LSP_KIND_FIELD      8
#define LSP_KIND_ENUM      10
#define LSP_KIND_VARIABLE  13
#define LSP_KIND_FUNCTION  12
#define LSP_KIND_CONSTANT  14
#define LSP_KIND_ENUMMEMBER 22
#define LSP_KIND_STRUCT    23
#define LSP_KIND_EVENT     24

/* Verifie le champ "kind" (entier) d'un DocumentSymbol. */
#define ASSERT_SYM_KIND(sym, expected_kind, test_name) do { \
    int _k = (sym) ? (int)json_get_int(sym, "kind", -1) : -1; \
    if (_k == (expected_kind)) { \
        g_pass++; \
        printf("  [OK]   %s : kind=%d\n", test_name, expected_kind); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu kind=%d, obtenu %d\n", \
               test_name, expected_kind, _k); \
    } \
} while (0)

/* Retourne le champ "detail" d'un DocumentSymbol (NULL si absent). */
static const char* sym_detail(JsonValue* sym) {
    if (!sym) return NULL;
    return json_get_string(sym, "detail");
}

/* Verifie le champ "detail" (chaine) d'un DocumentSymbol. */
#define ASSERT_SYM_DETAIL(sym, expected, test_name) do { \
    const char* _d = sym_detail(sym); \
    if (_d && strcmp(_d, (expected)) == 0) { \
        g_pass++; \
        printf("  [OK]   %s : detail=\"%s\"\n", test_name, expected); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : attendu detail=\"%s\", obtenu \"%s\"\n", \
               test_name, expected, _d ? _d : "(null)"); \
    } \
} while (0)

/* Verifie qu'aucun symbole "name" n'est present dans le tableau (assertion negative). */
#define ASSERT_SYM_NOT_EXISTS(arr, name, test_name) do { \
    JsonValue* _s = find_sym(arr, name); \
    if (!_s) { \
        g_pass++; \
        printf("  [OK]   %s : symbole '%s' absent (attendu)\n", test_name, name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : symbole '%s' PRESENT alors qu'il ne devrait pas etre la\n", \
               test_name, name); \
    } \
} while (0)

/* ================================================================
 *  Tests
 * ================================================================ */

#define URI "file:///c:/test/foo.owl"

static void test_body_fenetre_completion(void) {
    printf("\n[TEST] Body Fenetre : Ctrl+Espace propose props Fenetre\n");
    /* Ligne 1 (0-indexed) = ligne vide entre Titre= et FIN. Cursor a tab. */
    const char* src =
        "FEN_X est une Fenetre\n"
        "\t\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Titre",       "Body Fenetre");
    ASSERT_HAS(items, "Largeur",     "Body Fenetre");
    ASSERT_HAS(items, "Hauteur",     "Body Fenetre");
    ASSERT_HAS(items, "Visible",     "Body Fenetre");
    json_free(items);
    json_free(params);
}

static void test_body_bouton_completion(void) {
    printf("\n[TEST] Body Bouton : Ctrl+Espace propose props Bouton\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_OK est un Bouton\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Libell\xc3\xa9",     "Body Bouton");
    ASSERT_HAS(items, "TypeBouton",         "Body Bouton");
    ASSERT_HAS(items, "ToucheRaccourci",    "Body Bouton");
    ASSERT_HAS(items, "Bulle",              "Body Bouton");
    json_free(items);
    json_free(params);
}

/*  — 2nd terme d'une indirection `{ "nom", | }` : propose UNIQUEMENT les
 * constantes de scope (categorie "Indirection"), pas le catalogue general. */
static void test_completion_indirection_scope(void) {
    printf("\n[TEST] fix/1285 : scope d'indirection { nom, | } -> constantes indXxx seules\n");
    /* Ligne 0 : n est un entier = {"score", }  — curseur juste avant le '}'. */
    const char* src = "n est un entier = {\"score\", }\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 28);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "indChamp",    "1285 : indChamp proposee");
    ASSERT_HAS(items, "indVariable", "1285 : indVariable proposee");
    /* Liste scopee : une native du catalogue general est absente. */
    ASSERT_NOT_HAS(items, "Trace",   "1285 : pas de bruit (native hors scope)");
    json_free(items);
    json_free(params);
}

/*  — ',' auto-trigger : en scope d'indirection -> liste indXxx ;
 * hors contexte focalise -> vide (pas de spam), parite avec '('. */
static void test_completion_comma_trigger_indirection(void) {
    printf("\n[TEST] fix/1297 : ',' auto-trigger -> indXxx en scope, vide ailleurs\n");
    /* En scope d'indirection : ',' remonte la liste indXxx (auto-popup). */
    const char* src = "n est un entier = {\"score\", }\n";
    setup_doc(URI, src);
    JsonValue* p1 = make_completion_params_ctx(URI, 0, 28, 2, ",");
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS(i1, "indVariable", "1297 : ',' en scope -> indVariable");
    ASSERT_NOT_HAS(i1, "Trace",   "1297 : ',' en scope -> pas de bruit");
    json_free(i1); json_free(p1);
    /* Hors scope (arg de fonction quelconque) : ',' -> vide, pas de spam. */
    const char* src2 = "Trace(\"a\", )\n";
    setup_doc(URI, src2);
    JsonValue* p2 = make_completion_params_ctx(URI, 0, 11, 2, ",");
    JsonValue* i2 = handle_completion(p2);
    ASSERT_NOT_HAS(i2, "Trace",       "1297 : ',' hors scope -> pas de natives");
    ASSERT_NOT_HAS(i2, "indVariable", "1297 : ',' hors scope -> pas d'indXxx");
    json_free(i2); json_free(p2);
}

/*  — support #415 : la completion ne doit proposer une variable LOCALE
 * que si la procedure qui la declare contient le curseur. Deux procedures sœurs
 * declarant chacune ses locales (dont un nom DOUBLE) : au curseur dans procA,
 * seules les locales de procA sortent ; celles de procB (homonyme compris) sont
 * masquees, sans doublon de nom. Globales / procedures restent proposees. */
static void test_completion_local_scope_filter(void) {
    printf("\n[TEST] fix/2239 : completion filtre les variables locales hors portee\n");
    /* L.0 PROCEDURE procA(), L.1-2 ses locales, L.3 curseur (statement `va`),
     * L.4 FIN. L.5 PROCEDURE procB(), L.6-7 ses locales (dont `partage` homonyme
     * de la L.2), L.8 FIN. L.9 globale top-level. */
    const char* src =
        "PROCEDURE procA()\n"      /* 0 */
        "\tvarLettreA est un entier = 1\n"   /* 1 */
        "\tpartage est un entier = 2\n"      /* 2 */
        "\t\n"                     /* 3 : curseur ici (prefixe vide) */
        "FIN\n"                    /* 4 */
        "PROCEDURE procB()\n"      /* 5 */
        "\tvarLettreB est un entier = 3\n"   /* 6 */
        "\tpartage est un entier = 4\n"      /* 7 */
        "FIN\n"                    /* 8 */
        "gTotal est un entier = 0\n";        /* 9 */
    setup_doc(URI, src);
    /* Curseur ligne 3, apres le seul tab : prefixe vide -> liste complete (pas
     * de filtre prefixe ), toutes categories emises. */
    JsonValue* params = make_completion_params(URI, 3, 1);
    JsonValue* items = handle_completion(params);

    /* Locales de procA (dans la portee du curseur) : proposees. */
    ASSERT_HAS(items, "varLettreA", "2239 : locale de procA proposee");
    ASSERT_HAS(items, "partage",    "2239 : locale 'partage' de procA proposee");

    /* Locale de procB (hors portee) : masquee. */
    ASSERT_NOT_HAS(items, "varLettreB", "2239 : locale de procB masquee (hors portee)");

    /* Pas de doublon hors-portee : 'partage' existe dans procA ET procB, mais
     * seule celle de procA est en portee -> exactement 1 occurrence. */
    int nPartage = count_label(items, "partage");
    if (nPartage == 1) {
        g_pass++;
        printf("  [OK]   2239 : 'partage' propose exactement 1 fois (pas de doublon hors-portee)\n");
    } else {
        g_fail++;
        printf("  [FAIL] 2239 : 'partage' propose %d fois (attendu 1 — doublon hors-portee)\n", nPartage);
    }

    /* Non-regression : procedures (portee fichier) et globale top-level restent
     * proposees — le filtre ne touche QUE les variables locales. */
    ASSERT_HAS(items, "procB",  "2239 : procedure procB toujours proposee (non-regression)");
    ASSERT_HAS(items, "gTotal", "2239 : globale top-level toujours proposee (non-regression)");

    json_free(items);
    json_free(params);
}

/*  — support #208 : en mode EN, les litteraux booleens de la completion
 * doivent etre True/False (pas Vrai/Faux). 3 sites les emettaient en dur sans
 * consulter wantEN. */
static void test_completion_bool_literals_lang(void) {
    printf("\n[TEST] fix/1286 : litteraux booleens de completion suivent la langue\n");
    const char* src = "bLocalBoolean is a boolean = \n";
    setup_doc(URI, src);
    /* EN : True/False proposes, Vrai/Faux absents. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* p1 = make_completion_params(URI, 0, 29);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS(i1, "False",     "1286 EN : False propose");
    ASSERT_HAS(i1, "True",      "1286 EN : True propose");
    ASSERT_NOT_HAS(i1, "Faux",  "1286 EN : Faux (FR) absent");
    json_free(i1); json_free(p1);
    /* FR (defaut) : Vrai/Faux proposes, True/False absents. */
    lspServer.language[0] = '\0';
    JsonValue* p2 = make_completion_params(URI, 0, 29);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS(i2, "Faux",      "1286 FR : Faux propose");
    ASSERT_NOT_HAS(i2, "False", "1286 FR : False (EN) absent");
    json_free(i2); json_free(p2);
    lspServer.language[0] = '\0';
}

/*  — support #208 (regression post ) : le cas prefixe VIDE
 * ("= " curseur juste apres) proposait deja True/False (cf. test ci-dessus),
 * mais avec un prefixe PARTIEL ("= f", "= F", "= t") le chemin de completion
 * ne matche plus que les natives fXxx/tXxx et n'emet plus les litteraux
 * booleens du tout, meme filtres par le prefixe. Positions de curseur :
 * "bLocalBoolean is a boolean = " fait 29 caracteres (verifie : meme valeur
 * que le test  ci-dessus pour le prefixe vide), + 1 caractere de
 * prefixe ("f"/"F"/"t") = curseur en position 30. */
static void test_completion_bool_literals_partial_prefix(void) {
    printf("\n[TEST] fix/1955 : litteraux booleens de completion avec prefixe partiel\n");

    /* Temoin non-regression : prefixe VIDE reste vert (cf. ). */
    const char* srcEmpty = "bLocalBoolean is a boolean = \n";
    setup_doc(URI, srcEmpty);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* p0 = make_completion_params(URI, 0, 29);
    JsonValue* i0 = handle_completion(p0);
    ASSERT_HAS(i0, "False", "1955 temoin : prefixe vide EN -> False toujours propose");
    ASSERT_HAS(i0, "True",  "1955 temoin : prefixe vide EN -> True toujours propose");
    json_free(i0); json_free(p0);

    /* EN, prefixe "f" minuscule -> False doit etre propose. */
    const char* srcF = "bLocalBoolean is a boolean = f\n";
    setup_doc(URI, srcF);
    JsonValue* p1 = make_completion_params(URI, 0, 30);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS(i1, "False",     "1955 EN prefixe 'f' : False propose");
    ASSERT_NOT_HAS(i1, "Vrai",  "1955 EN prefixe 'f' : Vrai (FR) absent en mode EN");
    json_free(i1); json_free(p1);

    /* EN, prefixe "F" majuscule -> False propose (match casse-insensible). */
    const char* srcFMaj = "bLocalBoolean is a boolean = F\n";
    setup_doc(URI, srcFMaj);
    JsonValue* p2 = make_completion_params(URI, 0, 30);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS(i2, "False", "1955 EN prefixe 'F' : False propose (casse-insensible)");
    json_free(i2); json_free(p2);

    /* EN, prefixe "t" -> True propose. */
    const char* srcT = "bLocalBoolean is a boolean = t\n";
    setup_doc(URI, srcT);
    JsonValue* p3 = make_completion_params(URI, 0, 30);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_HAS(i3, "True", "1955 EN prefixe 't' : True propose");
    json_free(i3); json_free(p3);

    /* FR (defaut), prefixe "f" -> Faux propose, False (EN) absent. */
    lspServer.language[0] = '\0';
    const char* srcFrF = "bLocalVariableBooleenne est un booleen = f\n";
    setup_doc(URI, srcFrF);
    JsonValue* p4 = make_completion_params(URI, 0, (int)strlen(srcFrF) - 1);
    JsonValue* i4 = handle_completion(p4);
    ASSERT_HAS(i4, "Faux",      "1955 FR prefixe 'f' : Faux propose");
    ASSERT_NOT_HAS(i4, "False", "1955 FR prefixe 'f' : False (EN) absent");
    json_free(i4); json_free(p4);

    lspServer.language[0] = '\0';
}

/*  (suite) — support #208 : le declarant a precise que le symptome
 * survient quand la ligne est collee dans une PROCEDURE onclick() d'un
 * bouton (fenetre + projet), PAS au top-level. Le test precedent (top-level
 * pur) est vert sur HEAD des le  initial. On tente ici les contextes
 * REELS avant de conclure : procedure top-level nommee onClick, puis le
 * contexte le plus proche du declarant (Fenetre + Bouton + PROCEDURE
 * onClick()). Positions : le corps est indente (tabs), chaque tab compte
 * pour 1 caractere ; "bLocalBoolean is a boolean = " fait 29 caracteres
 * (verifie ci-dessus), + 1 caractere de prefixe "f" = +1, + N tabs
 * d'indentation. */
static void test_completion_bool_literals_partial_prefix_procedure_context(void) {
    printf("\n[TEST] fix/1955 (suite) : prefixe booleen partiel dans une PROCEDURE (top-level puis Fenetre/Bouton)\n");

    /* Contexte 1 : PROCEDURE top-level nommee onClick (pas dans une fenetre),
     * EN, prefixe "f" -> False attendu. Ligne 1 (0-indexed), indent 1 tab :
     * position = 1 (tab) + 29 + 1 (f) = 31. */
    const char* srcProcTop =
        "PROCEDURE onClick()\n"
        "\tbLocalBoolean is a boolean = f\n"
        "END\n";
    setup_doc(URI, srcProcTop);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* p1 = make_completion_params(URI, 1, 31);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS(i1, "False", "1955 suite : PROCEDURE onClick() top-level, prefixe 'f' -> False propose");
    json_free(i1); json_free(p1);

    /* Contexte 2 : le plus proche du declarant -> Fenetre + Bouton +
     * PROCEDURE onClick(). Ligne 3 (0-indexed : 0=FEN_X, 1=BTN_A,
     * 2=PROCEDURE onClick(), 3=corps), indent 3 tabs :
     * position = 3 (tabs) + 29 + 1 (f) = 33. */
    const char* srcWin =
        "FEN_X is a Window\n"
        "\tBTN_A is a Control Button\n"
        "\t\tPROCEDURE onClick()\n"
        "\t\t\tbLocalBoolean is a boolean = f\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, srcWin);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* p2 = make_completion_params(URI, 3, 33);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS(i2, "False", "1955 suite : PROCEDURE onClick() de bouton (Fenetre/Bouton), prefixe 'f' -> False propose");
    json_free(i2); json_free(p2);

    /* Contexte 3a : prefixe plus long "fa" au top-level pur (temoin de
     * comparaison). Position = 29 + 2 (fa) = 31. */
    const char* srcTopFa = "bLocalBoolean is a boolean = fa\n";
    setup_doc(URI, srcTopFa);
    JsonValue* p3 = make_completion_params(URI, 0, 31);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_HAS(i3, "False", "1955 suite : top-level, prefixe 'fa' -> False propose");
    json_free(i3); json_free(p3);

    /* Contexte 3b : prefixe plus long "fa" dans la PROCEDURE onClick() du
     * bouton. Position = 3 (tabs) + 29 + 2 (fa) = 34. */
    const char* srcWinFa =
        "FEN_X is a Window\n"
        "\tBTN_A is a Control Button\n"
        "\t\tPROCEDURE onClick()\n"
        "\t\t\tbLocalBoolean is a boolean = fa\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, srcWinFa);
    JsonValue* p4 = make_completion_params(URI, 3, 34);
    JsonValue* i4 = handle_completion(p4);
    ASSERT_HAS(i4, "False", "1955 suite : PROCEDURE onClick() de bouton, prefixe 'fa' -> False propose");
    json_free(i4); json_free(p4);
    lspServer.language[0] = '\0';

    /* Contexte 4 : variante FR du contexte procedure (Fenetre/Bouton FR,
     * surClic), prefixe "f" -> Faux attendu. Ligne 3 (0-indexed), indent
     * 3 tabs ; "bLocalVariableBooleenne est un booleen = " fait 43
     * caracteres (idem test  initial, ligne top-level ou
     * strlen(srcFrF)-1 valait 43) : position = 3 (tabs) + 43 + 1 (f) = 47. */
    const char* srcWinFr =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Bouton\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tbLocalVariableBooleenne est un booleen = f\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcWinFr);
    JsonValue* p5 = make_completion_params(URI, 3, 47);
    JsonValue* i5 = handle_completion(p5);
    ASSERT_HAS(i5, "Faux", "1955 suite : PROCEDURE surClic() de bouton (FR), prefixe 'f' -> Faux propose");
    json_free(i5); json_free(p5);
}

/*  — support #208 (follow-up ) : dans `b est un booleen = `,
 * la completion dumpait TOUT le catalogue de constantes (ordre alpha, AlignH &
 * co) au lieu de restreindre aux SEULS litteraux booleens.  validait
 * seulement « Vrai/Faux presents » ; ici on verrouille le critere fort « SEULS
 * les litteraux booleens » cote contribution CONSTANTES. Bornes : natives et
 * variables assignables (categorie distincte) restent proposees ; un contexte
 * NON-booleen (autre type / assignation libre) n'est PAS sur-filtre. */
static void test_completion_bool_decl_assign_filters_catalog(void) {
    printf("\n[TEST] fix/1988 : decl-assign booleen restreint la contribution constantes aux litteraux\n");

    /* FR, prefixe vide : Vrai/Faux proposes, TAB (temoin catalogue) absent. */
    lspServer.language[0] = '\0';
    const char* srcFr = "b est un booleen = \n";
    setup_doc(URI, srcFr);
    JsonValue* p1 = make_completion_params(URI, 0, 19);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS(i1, "Vrai",      "1988 FR : Vrai propose");
    ASSERT_HAS(i1, "Faux",      "1988 FR : Faux propose");
    ASSERT_NOT_HAS(i1, "TAB",   "1988 FR : catalogue constantes filtre (TAB absent)");
    ASSERT_NOT_HAS(i1, "CRLF",  "1988 FR : catalogue constantes filtre (CRLF absent)");
    json_free(i1); json_free(p1);

    /* FR, prefixe partiel 'f' : Faux propose, catalogue absent. */
    const char* srcFrF = "b est un booleen = f\n";
    setup_doc(URI, srcFrF);
    JsonValue* p2 = make_completion_params(URI, 0, 20);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS(i2, "Faux",      "1988 FR prefixe 'f' : Faux propose");
    ASSERT_NOT_HAS(i2, "TAB",   "1988 FR prefixe 'f' : catalogue filtre (TAB absent)");
    json_free(i2); json_free(p2);

    /* EN, prefixe vide : True/False proposes, catalogue absent. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* srcEn = "b is a boolean = \n";
    setup_doc(URI, srcEn);
    JsonValue* p3 = make_completion_params(URI, 0, 17);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_HAS(i3, "True",      "1988 EN : True propose");
    ASSERT_HAS(i3, "False",     "1988 EN : False propose");
    ASSERT_NOT_HAS(i3, "TAB",   "1988 EN : catalogue constantes filtre (TAB absent)");
    json_free(i3); json_free(p3);
    lspServer.language[0] = '\0';

    /* Non-regression : contexte NON-booleen (`s est une chaine = `) -> la
     * contribution constantes reste complete (TAB propose). Sans borne, le
     * filtre aurait pu deborder sur tout decl-assign. */
    const char* srcStr = "s est une chaine = \n";
    setup_doc(URI, srcStr);
    JsonValue* p4 = make_completion_params(URI, 0, 19);
    JsonValue* i4 = handle_completion(p4);
    ASSERT_HAS(i4, "TAB",       "1988 non-reg : contexte chaine garde le catalogue (TAB present)");
    json_free(i4); json_free(p4);
}

/*  — suite  : le filtre booleen s'arretait au top-level. Dans un
 * bloc widget (`FEN_X est une Fenetre ... FIN`, ou `est un Conteneur`), la
 * branche kindStr dumpait en plus les NOMS de props (AlignH/AlignV & co) par
 * dessus Vrai/Faux, car la boucle OWL_DSL_PROP_CATALOG ne consultait pas
 * boolLiteralOnly. Ce test verrouille : decl-assign booleen en bloc widget ->
 * Vrai/Faux SEULS, pas de props. Bornes : la vraie position nom-de-prop (debut
 * de ligne dans le bloc, pas un decl-assign) garde les props. */
static void test_completion_bool_decl_assign_block_widget_no_props(void) {
    printf("\n[TEST] fix/1997 : decl-assign booleen en bloc widget ne dumpe pas les noms de props\n");

    /* FR, dans un bloc Fenetre. Ligne 1 = `\tb est un booleen = `.
     * character = 1 (tab) + 19 (longueur de "b est un booleen = ") = 20. */
    lspServer.language[0] = '\0';
    const char* srcFen =
        "FEN_X est une Fenetre\n"
        "\tb est un booleen = \n"
        "FIN\n";
    setup_doc(URI, srcFen);
    JsonValue* p1 = make_completion_params(URI, 1, 20);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS    (i1, "Vrai",   "1997 FR Fenetre : Vrai propose");
    ASSERT_HAS    (i1, "Faux",   "1997 FR Fenetre : Faux propose");
    ASSERT_NOT_HAS(i1, "AlignH", "1997 FR Fenetre : nom de prop AlignH absent");
    ASSERT_NOT_HAS(i1, "AlignV", "1997 FR Fenetre : nom de prop AlignV absent");
    ASSERT_NOT_HAS(i1, "Titre",  "1997 FR Fenetre : nom de prop Titre absent");
    json_free(i1); json_free(p1);

    /* FR, dans un bloc Conteneur. */
    const char* srcCont =
        "CONT_X est un Conteneur\n"
        "\tb est un booleen = \n"
        "FIN\n";
    setup_doc(URI, srcCont);
    JsonValue* p2 = make_completion_params(URI, 1, 20);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS    (i2, "Vrai",       "1997 FR Conteneur : Vrai propose");
    ASSERT_HAS    (i2, "Faux",       "1997 FR Conteneur : Faux propose");
    ASSERT_NOT_HAS(i2, "AlignH",     "1997 FR Conteneur : nom de prop AlignH absent");
    ASSERT_NOT_HAS(i2, "Alignement", "1997 FR Conteneur : nom de prop Alignement absent");
    json_free(i2); json_free(p2);

    /* EN, `b is a boolean = ` en bloc Fenetre. character = 1 (tab) + 17 = 18. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* srcEn =
        "FEN_X est une Fenetre\n"
        "\tb is a boolean = \n"
        "FIN\n";
    setup_doc(URI, srcEn);
    JsonValue* p3 = make_completion_params(URI, 1, 18);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_HAS    (i3, "True",   "1997 EN Fenetre : True propose");
    ASSERT_HAS    (i3, "False",  "1997 EN Fenetre : False propose");
    ASSERT_NOT_HAS(i3, "AlignH", "1997 EN Fenetre : nom de prop AlignH absent");
    json_free(i3); json_free(p3);
    lspServer.language[0] = '\0';

    /* Non-regression : vraie position nom-de-prop (debut de ligne, pas un
     * decl-assign) dans un bloc Fenetre -> les props restent proposees. */
    const char* srcProp =
        "FEN_X est une Fenetre\n"
        "\t\n"
        "FIN\n";
    setup_doc(URI, srcProp);
    JsonValue* p4 = make_completion_params(URI, 1, 1);
    JsonValue* i4 = handle_completion(p4);
    ASSERT_HAS(i4, "Titre",   "1997 non-reg : nom de prop Titre toujours propose");
    ASSERT_HAS(i4, "Largeur", "1997 non-reg : nom de prop Largeur toujours propose");
    json_free(i4); json_free(p4);
}

/*  — suite /1997 : en position valeur d'un decl-assign
 * (`<var> est un <type> = <curseur>`), la completion proposait encore des
 * mots-cles (`ALORS`, `SINON`, `SI`, `FIN`…) et des noms de types (`entier`,
 * `chaine`, `Fenetre`…). Aucun des deux n'est une valeur assignable. Ce test
 * verrouille leur absence, top-level ET en bloc widget, tout en preservant les
 * litteraux Vrai/Faux (/1997). Bornes : debut d'instruction (ou les
 * mots-cles SONT valides) garde les mots-cles ; vraie position nom-de-prop
 * garde les props. */
static void test_completion_decl_assign_value_no_keywords_no_types(void) {
    printf("\n[TEST] fix/1999 : decl-assign value position exclut mots-cles + noms de types\n");
    lspServer.language[0] = '\0';

    /* Top-level : `b est un booleen = ` (col 19). */
    const char* srcTop = "b est un booleen = \n";
    setup_doc(URI, srcTop);
    JsonValue* p1 = make_completion_params(URI, 0, 19);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_HAS    (i1, "Vrai",    "1999 top : Vrai propose");
    ASSERT_HAS    (i1, "Faux",    "1999 top : Faux propose");
    ASSERT_NOT_HAS(i1, "ALORS",   "1999 top : mot-cle ALORS absent");
    ASSERT_NOT_HAS(i1, "SINON",   "1999 top : mot-cle SINON absent");
    ASSERT_NOT_HAS(i1, "SI",      "1999 top : mot-cle SI absent");
    ASSERT_NOT_HAS(i1, "FIN",     "1999 top : mot-cle FIN absent");
    ASSERT_NOT_HAS(i1, "entier",  "1999 top : nom de type entier absent");
    ASSERT_NOT_HAS(i1, "chaîne",  "1999 top : nom de type chaine absent");
    ASSERT_NOT_HAS(i1, "Fenêtre", "1999 top : nom de type Fenetre absent");
    json_free(i1); json_free(p1);

    /* En bloc widget Fenetre : `\tb est un booleen = ` (ligne 1, col 20). */
    const char* srcFen =
        "FEN_X est une Fenetre\n"
        "\tb est un booleen = \n"
        "FIN\n";
    setup_doc(URI, srcFen);
    JsonValue* p2 = make_completion_params(URI, 1, 20);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS    (i2, "Vrai",    "1999 widget : Vrai propose");
    ASSERT_HAS    (i2, "Faux",    "1999 widget : Faux propose");
    ASSERT_NOT_HAS(i2, "ALORS",   "1999 widget : mot-cle ALORS absent");
    ASSERT_NOT_HAS(i2, "SI",      "1999 widget : mot-cle SI absent");
    ASSERT_NOT_HAS(i2, "entier",  "1999 widget : nom de type entier absent");
    ASSERT_NOT_HAS(i2, "Fenêtre", "1999 widget : nom de type Fenetre absent");
    json_free(i2); json_free(p2);

    /* Decl-assign d'un type NON booleen (`n est un entier = `) : la position
     * value exclut aussi mots-cles + types (fondation etape B). Les natives et
     * constantes restent (etape B les filtrera). */
    const char* srcInt = "n est un entier = \n";
    setup_doc(URI, srcInt);
    JsonValue* p3 = make_completion_params(URI, 0, 18);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_NOT_HAS(i3, "ALORS",   "1999 entier : mot-cle ALORS absent");
    ASSERT_NOT_HAS(i3, "chaîne",  "1999 entier : nom de type chaine absent");
    /* Temoin : le catalogue de constantes reste (non filtre par type en A). */
    ASSERT_HAS    (i3, "TAB",     "1999 entier : catalogue constantes preserve (TAB present)");
    json_free(i3); json_free(p3);

    /* Non-regression : debut d'instruction dans une procedure (ligne vide) ->
     * les mots-cles restent proposes (position ou ils sont valides). */
    const char* srcProc =
        "PROCEDURE Test()\n"
        "\t\n"
        "FIN\n";
    setup_doc(URI, srcProc);
    JsonValue* p4 = make_completion_params(URI, 1, 1);
    JsonValue* i4 = handle_completion(p4);
    ASSERT_HAS(i4, "SI",   "1999 non-reg : mot-cle SI propose en debut d'instruction");
    ASSERT_HAS(i4, "POUR", "1999 non-reg : mot-cle POUR propose en debut d'instruction");
    json_free(i4); json_free(p4);

    /* Non-regression : vraie position nom-de-prop en bloc widget -> props
     * proposees. */
    const char* srcProp =
        "FEN_X est une Fenetre\n"
        "\t\n"
        "FIN\n";
    setup_doc(URI, srcProp);
    JsonValue* p5 = make_completion_params(URI, 1, 1);
    JsonValue* i5 = handle_completion(p5);
    ASSERT_HAS(i5, "Titre",   "1999 non-reg : nom de prop Titre propose");
    ASSERT_HAS(i5, "Largeur", "1999 non-reg : nom de prop Largeur propose");
    json_free(i5); json_free(p5);
}

/* ================================================================
 *   — completion consciente du type en position valeur
 *  d'un decl-assign (`<var> est un <type> = <curseur>`).
 *
 *  /1997/1999 ont nettoye le bruit non-valeur (mots-cles, noms
 *  de type, props widget). Il reste que TOUTES les natives et TOUTES
 *  les variables/champs en scope sont encore proposes, sans regard au
 *  type du LHS (`declAssignType`, capture par  mais pas encore
 *  exploite). Ces tests verrouillent le comportement attendu par
 *  categorie : seul ce qui est ASSIGNABLE au type LHS doit rester.
 *
 *  Temoins natives (return_type verifie via `owl explain <Native>
 *  --json`, signature(s) sans ambiguite de retour) :
 *    - DateValide(date) -> booleen (signature unique)
 *    - Taille(chaine)   -> entier  (signature unique)
 *    - Racine(numerique)-> reel    (signature unique)
 *    - Majuscule(chaine)-> chaine  (signature unique)
 *    - Gauche(chaine,entier) -> chaine (2 sigs, meme retour, non ambigu)
 *
 *  RED-FIRST : avant impl, aucune boucle (natives, userSymbols) ne
 *  consulte declAssignType -> les ASSERT_NOT_HAS ci-dessous echouent
 *  (tout est encore propose). Apres impl -> verts. Les tests de surete
 *  (fallback) et de non-regression sont verts des le depart : ce sont
 *  des invariants a NE JAMAIS casser, pas des comportements a ajouter.
 *  Exception : le critere 4 (enum LHS) est deja couvert par un mecanisme
 *  pre-existant independant de declAssignType (cf. commentaire dedie sur
 *  test_completion_declassign_type_filter_enum) -> VERT des le depart.
 * ================================================================ */

/* Critere 1 : `b est un booleen = ` -> Vrai/Faux + native retour
 * booleen presents ; natives retour entier/chaine absentes. */
static void test_completion_declassign_type_filter_boolean(void) {
    printf("\n[TEST] feat/2001 : decl-assign booleen filtre par type de retour (RED avant impl)\n");
    lspServer.language[0] = '\0';

    /* "b est un booleen = " fait 19 caracteres -> curseur col 19. */
    const char* src = "b est un booleen = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 19);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Vrai",       "2001 booleen : Vrai propose");
    ASSERT_HAS    (items, "Faux",       "2001 booleen : Faux propose");
    ASSERT_HAS    (items, "DateValide", "2001 booleen : native retour booleen proposee (temoin)");
    ASSERT_NOT_HAS(items, "Taille",     "2001 booleen : native retour entier absente");
    ASSERT_NOT_HAS(items, "Majuscule",  "2001 booleen : native retour chaine absente");
    json_free(items);
    json_free(params);
}

/* Critere 2 : `n est un entier = ` -> natives retour entier presentes,
 * ET natives retour reel presentes (hierarchie numerique : un reel se
 * tronque a l'affectation dans un entier, cf. 01_types_et_coercions.md
 * §3 `nT est un entier = 3.7 -> 3`). Natives chaine/booleen absentes. */
static void test_completion_declassign_type_filter_entier(void) {
    printf("\n[TEST] feat/2001 : decl-assign entier filtre par type de retour + hierarchie numerique (RED avant impl)\n");
    lspServer.language[0] = '\0';

    /* "n est un entier = " fait 18 caracteres -> curseur col 18. */
    const char* src = "n est un entier = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 18);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Taille",     "2001 entier : native retour entier proposee");
    ASSERT_HAS    (items, "Racine",     "2001 entier : native retour reel proposee (hierarchie numerique)");
    ASSERT_NOT_HAS(items, "Majuscule",  "2001 entier : native retour chaine absente");
    ASSERT_NOT_HAS(items, "DateValide", "2001 entier : native retour booleen absente");
    json_free(items);
    json_free(params);
}

/* Critere 3 : `s est une chaine = ` -> natives retour chaine presentes,
 * natives entier/booleen absentes. Plus coercion chaineMultilingue ->
 * chaine (, automatique a l'usage) : une variable en scope
 * typee chaineMultilingue reste proposee pour un LHS chaine. */
static void test_completion_declassign_type_filter_chaine(void) {
    printf("\n[TEST] feat/2001 : decl-assign chaine filtre par type de retour + coercion chaineMultilingue (RED avant impl)\n");
    lspServer.language[0] = '\0';

    /* "s est une chaine = " fait 19 caracteres -> curseur col 19. */
    const char* src = "s est une chaine = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 19);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Majuscule",  "2001 chaine : native retour chaine proposee");
    ASSERT_HAS    (items, "Gauche",     "2001 chaine : native retour chaine proposee (2)");
    ASSERT_NOT_HAS(items, "Taille",     "2001 chaine : native retour entier absente");
    ASSERT_NOT_HAS(items, "DateValide", "2001 chaine : native retour booleen absente");
    json_free(items);
    json_free(params);

    /* Coercion  : `m` typee chaineMultilingue en scope doit
     * rester proposee comme valeur d'un LHS chaine. */
    const char* srcCoerce =
        "m est une chaineMultilingue = { langueAnglais: \"x\" }\n"
        "s est une chaine = \n";
    setup_doc(URI, srcCoerce);
    JsonValue* p2 = make_completion_params(URI, 1, 19);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_HAS(i2, "m", "2001 chaine : variable chaineMultilingue proposee (coercion ADR_0026)");
    json_free(i2); json_free(p2);
}

/* Critere 4 : enum LHS (`e est un <MonEnum> = `) -> les VALEURS de
 * l'enum proposees, pas le catalogue general.
 *
 * VERT DES LE DEPART (pas RED) : verification a l'ecriture de ce test,
 * un mecanisme PRE-EXISTANT (walk-back "Cas decl-and-assign", ~L5585-5601
 * de lsp_handlers.c) traite deja ce cas — quand le mot juste avant `=`
 * est un type enum connu (user_enum_lookup / owl_enum_info_find), il
 * arme simpleAssignContext + simpleAssignVarType et le bloc L7478-7499
 * renvoie STRICTEMENT les valeurs de l'enum (return items anticipe,
 * avant meme la boucle natives). Le critere 4 du ticket est donc deja
 * couvert independamment du filtre generalise vise par  ;
 * ce test verrouille ce comportement existant (non-regression), il ne
 * documente pas un ajout d'impl. Signale au coordinator (needs_input
 * potentiel sur le perimetre exact du ticket vs l'existant). */
static void test_completion_declassign_type_filter_enum(void) {
    printf("\n[TEST] feat/2001 : decl-assign enum user propose les valeurs de l'enum (VERT — mecanisme deja existant)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "Statut est une Enumeration\n"
        "\tactif\n"
        "\tinactif\n"
        "FIN\n"
        "\n"
        "e est un Statut = \n";
    setup_doc(URI, src);
    /* Curseur en fin de derniere ligne, apres "e est un Statut = ". */
    int col = (int)strlen("e est un Statut = ");
    JsonValue* params = make_completion_params(URI, 5, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "actif",     "2001 enum : valeur actif proposee");
    ASSERT_HAS    (items, "inactif",   "2001 enum : valeur inactif proposee");
    /* Strict : le catalogue general de natives ne doit pas fuiter. */
    ASSERT_NOT_HAS(items, "Trace",     "2001 enum : catalogue natives filtre (Trace absente)");
    json_free(items);
    json_free(params);
}

/* Critere 5 : type objet LHS -> sous-type (heritage) propose, scalaire
 * incompatible absent. cChien Herite de cAnimal (baseClass trackee,
 * ) : une variable cChien doit rester assignable a un LHS
 * cAnimal ; une variable entier ne l'est pas. */
static void test_completion_declassign_type_filter_objet_heritage(void) {
    printf("\n[TEST] feat/2001 : decl-assign type objet propose sous-type herite, pas scalaire (RED avant impl)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "cAnimal est une Classe\n"
        "FIN\n"
        "\n"
        "cChien est une Classe Herite de cAnimal\n"
        "FIN\n"
        "\n"
        "aAnimal est un cAnimal\n"
        "unChien est un cChien\n"
        "nAutre est un entier\n"
        "\n"
        "x est un cAnimal = \n";
    setup_doc(URI, src);
    int col = (int)strlen("x est un cAnimal = ");
    JsonValue* params = make_completion_params(URI, 10, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "aAnimal",  "2001 objet : meme type propose");
    ASSERT_HAS    (items, "unChien",  "2001 objet : sous-type herite propose");
    ASSERT_NOT_HAS(items, "nAutre",   "2001 objet : scalaire incompatible absent");
    json_free(items);
    json_free(params);
}

/* Critere 6 : variables/champs en scope filtres par type declare
 * compatible. Une variable booleenne et une entiere avant le curseur :
 * sur `b est un booleen = `, seule la booleenne est proposee. */
static void test_completion_declassign_type_filter_variables_scope(void) {
    printf("\n[TEST] feat/2001 : decl-assign filtre les variables en scope par type declare (RED avant impl)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "bFlag est un booleen = Vrai\n"
        "nCount est un entier = 5\n"
        "\n"
        "b est un booleen = \n";
    setup_doc(URI, src);
    int col = (int)strlen("b est un booleen = ");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "bFlag",  "2001 scope : variable booleenne compatible proposee");
    ASSERT_NOT_HAS(items, "nCount", "2001 scope : variable entiere incompatible absente");
    json_free(items);
    json_free(params);
}

/* Critere 7 : SURETE (fallback) — type LHS non reconnu par le predicat
 * (struct/type inconnu du registry) -> tout candidat non PROUVE
 * incompatible reste PROPOSE (faux negatif = pire que faux positif,
 * cf. ticket §Risque). Ce test est VERT des le depart ET apres impl :
 * c'est un invariant de securite, pas un comportement a ajouter. */
static void test_completion_declassign_type_filter_fallback_surete(void) {
    printf("\n[TEST] feat/2001 : type LHS non reconnu -> fallback propose tout (VERT invariant)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "nExistant est un entier = 5\n"
        "\n"
        "x est un TrucInconnu = \n";
    setup_doc(URI, src);
    int col = (int)strlen("x est un TrucInconnu = ");
    JsonValue* params = make_completion_params(URI, 2, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Trace",      "2001 surete : native generique proposee malgre type LHS inconnu");
    ASSERT_HAS(items, "nExistant",  "2001 surete : variable existante proposee malgre type LHS inconnu");
    json_free(items);
    json_free(params);
}

/* Critere 8 : NON-REGRESSION — hors position valeur d'un decl-assign
 * (reaffectation `x = ` sur une variable existante), la completion
 * reste INCHANGEE : aucun filtrage par type, meme si x est entier. */
static void test_completion_declassign_type_filter_non_regression_reassign(void) {
    printf("\n[TEST] feat/2001 : non-regression reaffectation (x = ) -> pas de filtrage par type\n");
    lspServer.language[0] = '\0';

    const char* src =
        "x est un entier\n"
        "x = \n";
    setup_doc(URI, src);
    int col = (int)strlen("x = ");
    JsonValue* params = make_completion_params(URI, 1, col);
    JsonValue* items = handle_completion(params);
    /* Majuscule retourne une chaine, incompatible avec x (entier) : si la
     * completion etait filtree ici comme en decl-assign, elle serait
     * absente. Elle doit rester presente (hors-scope assume au ticket). */
    ASSERT_HAS(items, "Majuscule", "2001 non-reg : reaffectation garde le catalogue complet");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — filtrage AGRESSIF pour LHS scalaire + tri litteraux en tete
 *
 *   restait permissif : sa regle de surete gardait toute native
 *  dont le retour n'etait pas un scalaire propre (categorie VT_OTHER :
 *  tableau, tout, rien/void, date, coerce...).  distingue, DANS
 *  VT_OTHER, le CONNU-non-scalaire (a exclure pour un LHS scalaire) du
 *  VRAIMENT-inconnu / variant `tout` (a garder, option A validee dev).
 *
 *  RED avant impl : `Ajoute`(tableau)/`Trace`(rien) restent proposes pour
 *  un LHS booleen -> ASSERT_NOT_HAS echouent ; les litteraux Vrai/Faux
 *  n'ont pas de sortText -> ASSERT_SORT_LIT_BEFORE_NATIVE echoue.
 * ================================================================ */

/* Volet 1+2 : `b est un booleen = ` -> tableau/rien/autre-scalaire EXCLUS,
 * booleen + `tout` GARDES, litteraux Vrai/Faux en tete via sortText. */
static void test_completion_declassign_scalaire_agressif_boolean(void) {
    printf("\n[TEST] feat/2003 : LHS booleen filtre agressif (tableau/rien exclus, tout garde) + tri litteraux (RED avant impl)\n");
    lspServer.language[0] = '\0';

    const char* src = "b est un booleen = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 19);
    JsonValue* items = handle_completion(params);
    /* GARDES : meme categorie scalaire + variant `tout`. */
    ASSERT_HAS    (items, "Vrai",             "2003 booleen : litteral Vrai propose");
    ASSERT_HAS    (items, "Faux",             "2003 booleen : litteral Faux propose");
    ASSERT_HAS    (items, "DateValide",       "2003 booleen : native retour booleen gardee");
    ASSERT_HAS    (items, "AnnéeBissextile",  "2003 booleen : native retour booleen gardee (2)");
    ASSERT_HAS    (items, "Agrège",           "2003 booleen : native retour `tout`/variant gardee (option A)");
    /* EXCLUS : connu non-scalaire (tableau, rien/void, autre scalaire). */
    ASSERT_NOT_HAS(items, "Ajoute",           "2003 booleen : native retour tableau exclue");
    ASSERT_NOT_HAS(items, "Trace",            "2003 booleen : native retour `rien`/void exclue");
    ASSERT_NOT_HAS(items, "Taille",           "2003 booleen : native retour entier exclue");
    ASSERT_NOT_HAS(items, "Majuscule",        "2003 booleen : native retour chaine exclue");
    /* Tri : les litteraux du type remontent avant les natives. */
    ASSERT_SORT_LIT_BEFORE_NATIVE(items, "Vrai", "DateValide", "2003 booleen : Vrai avant natives");
    ASSERT_SORT_LIT_BEFORE_NATIVE(items, "Faux", "DateValide", "2003 booleen : Faux avant natives");
    json_free(items);
    json_free(params);
}

/* Volet 1 : `n est un entier = ` -> numeriques gardees, tableau/chaine/
 * booleen exclus, `tout` garde. */
static void test_completion_declassign_scalaire_agressif_entier(void) {
    printf("\n[TEST] feat/2003 : LHS entier filtre agressif (tableau/chaine/booleen exclus, tout garde) (RED avant impl)\n");
    lspServer.language[0] = '\0';

    const char* src = "n est un entier = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 18);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Taille",     "2003 entier : native retour entier gardee");
    ASSERT_HAS    (items, "Racine",     "2003 entier : native retour reel gardee (hierarchie numerique)");
    ASSERT_HAS    (items, "Agrège",     "2003 entier : native retour `tout`/variant gardee (option A)");
    ASSERT_NOT_HAS(items, "Ajoute",     "2003 entier : native retour tableau exclue");
    ASSERT_NOT_HAS(items, "Majuscule",  "2003 entier : native retour chaine exclue");
    ASSERT_NOT_HAS(items, "DateValide", "2003 entier : native retour booleen exclue");
    ASSERT_NOT_HAS(items, "Trace",      "2003 entier : native retour `rien`/void exclue");
    json_free(items);
    json_free(params);
}

/* Volet 1 SURETE : un candidat de type NON RESOLU (chaine non vide inconnue
 * du registry) reste PROPOSE meme pour un LHS scalaire. On ne masque QUE le
 * connu-incompatible ; l'inconnu reste propose (faux negatif > faux positif). */
static void test_completion_declassign_scalaire_agressif_surete(void) {
    printf("\n[TEST] feat/2003 : LHS scalaire, candidat type non resolu -> propose (surete, VERT invariant)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "vMystere est un TrucInconnu\n"
        "\n"
        "b est un booleen = \n";
    setup_doc(URI, src);
    int col = (int)strlen("b est un booleen = ");
    JsonValue* params = make_completion_params(URI, 2, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "vMystere", "2003 surete : variable de type non resolu proposee (jamais masquee)");
    json_free(items);
    json_free(params);
}

/* Volet 1 borne : LHS NON scalaire (classe user) -> comportement permissif
 * INCHANGE. Une native a retour tableau (Ajoute) reste proposee (pas de
 * sur-filtrage sur les types non-scalaires). */
static void test_completion_declassign_scalaire_agressif_lhs_non_scalaire(void) {
    printf("\n[TEST] feat/2003 : LHS non scalaire (classe) -> permissif inchange (native tableau gardee) (VERT invariant)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "cAnimal est une Classe\n"
        "FIN\n"
        "\n"
        "x est un cAnimal = \n";
    setup_doc(URI, src);
    int col = (int)strlen("x est un cAnimal = ");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Ajoute", "2003 borne : LHS classe garde les natives non-scalaires (permissif)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — LHS scalaire : exclure EN BLOC champs + procedures
 *
 *  /2003 filtrent natives (par retour) et variables kind 6
 *  (par type declare). Restaient le bruit des CHAMPS (userControls[])
 *  et des PROCEDURES (userSymbols kind 3), dont ni le type-valeur ni
 *  le type de retour ne sont captures au scan LSP. Filtrage fin =
 *  hors-scope. Decision dev : quand le LHS est scalaire (BOOL/NUM/STR),
 *  on exclut EN BLOC ces deux categories en position valeur decl-assign
 *  (comme les mots-cles/types de ). LHS non-scalaire : permissif
 *  inchange. Hors position valeur : inchange.
 *
 *  RED avant impl : champs + procedure encore proposes pour un LHS
 *  booleen/entier -> ASSERT_NOT_HAS echouent. Les bornes (LHS classe,
 *  hors position valeur) sont VERTES des le depart : invariants a ne
 *  jamais casser.
 * ================================================================ */

/* Critere 1 : `b est un booleen = ` -> champs + procedure ABSENTS ;
 * Vrai/Faux + variable booleenne PRESENTS. */
static void test_completion_2028_exclut_champs_procedures_booleen(void) {
    printf("\n[TEST] feat/2028 : LHS booleen exclut champs + procedures (RED avant impl)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_Nom est une Saisie\n"
        "\tFIN\n"
        "\tLBL_Titre est un Libelle\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE GenereFacture()\n"
        "FIN\n"
        "\n"
        "bFlag est un booleen = Vrai\n"
        "\n"
        "b est un booleen = \n";
    setup_doc(URI, src);
    int col = (int)strlen("b est un booleen = ");
    JsonValue* params = make_completion_params(URI, 12, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Vrai",          "2028 booleen : litteral Vrai propose");
    ASSERT_HAS    (items, "Faux",          "2028 booleen : litteral Faux propose");
    ASSERT_HAS    (items, "bFlag",         "2028 booleen : variable booleenne proposee (feat/2001 inchange)");
    ASSERT_NOT_HAS(items, "SAI_Nom",       "2028 booleen : champ Saisie exclu");
    ASSERT_NOT_HAS(items, "LBL_Titre",     "2028 booleen : champ Libelle exclu");
    ASSERT_NOT_HAS(items, "GenereFacture", "2028 booleen : procedure exclue");
    json_free(items);
    json_free(params);
}

/* Critere 2 : `n est un entier = ` -> idem, champs + procedure absents. */
static void test_completion_2028_exclut_champs_procedures_entier(void) {
    printf("\n[TEST] feat/2028 : LHS entier exclut champs + procedures (RED avant impl)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_Nom est une Saisie\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE GenereFacture()\n"
        "FIN\n"
        "\n"
        "n est un entier = \n";
    setup_doc(URI, src);
    int col = (int)strlen("n est un entier = ");
    JsonValue* params = make_completion_params(URI, 8, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Taille",        "2028 entier : native retour entier proposee (temoin)");
    ASSERT_NOT_HAS(items, "SAI_Nom",       "2028 entier : champ Saisie exclu");
    ASSERT_NOT_HAS(items, "GenereFacture", "2028 entier : procedure exclue");
    json_free(items);
    json_free(params);
}

/* Critere 3 borne : LHS non scalaire (classe user) -> champ + procedure
 * TOUJOURS proposes (permissif inchange, un champ/procedure peut y etre
 * une valeur legitime). VERT invariant. */
static void test_completion_2028_lhs_non_scalaire_permissif(void) {
    printf("\n[TEST] feat/2028 : LHS classe garde champs + procedures (permissif) (VERT invariant)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "cAnimal est une Classe\n"
        "FIN\n"
        "\n"
        "FEN_X est une Fenetre\n"
        "\tSAI_Nom est une Saisie\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE GenereFacture()\n"
        "FIN\n"
        "\n"
        "o est un cAnimal = \n";
    setup_doc(URI, src);
    int col = (int)strlen("o est un cAnimal = ");
    JsonValue* params = make_completion_params(URI, 11, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "SAI_Nom",       "2028 borne : LHS classe garde le champ (permissif)");
    ASSERT_HAS(items, "GenereFacture", "2028 borne : LHS classe garde la procedure (permissif)");
    json_free(items);
    json_free(params);
}

/* Critere 4 borne : hors position valeur decl-assign (statement start
 * top-level) -> champ + procedure proposes comme avant. VERT invariant. */
static void test_completion_2028_hors_position_valeur_permissif(void) {
    printf("\n[TEST] feat/2028 : hors position valeur garde champs + procedures (VERT invariant)\n");
    lspServer.language[0] = '\0';

    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_Nom est une Saisie\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE GenereFacture()\n"
        "FIN\n"
        "\n"
        "\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 8, 0);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "SAI_Nom",       "2028 non-reg : champ propose hors position valeur");
    ASSERT_HAS(items, "GenereFacture", "2028 non-reg : procedure proposee hors position valeur");
    json_free(items);
    json_free(params);
}

/*  lot 1 — RC et CR ont quitte le registre des constantes : ils
 * cessent d'etre des symboles du langage, donc la completion ne doit plus les
 * proposer. LF et CRLF, seules constantes de fin de ligne restantes, doivent
 * rester proposees. Sans ce test, rien n'empeche une reintroduction
 * silencieuse via le catalogue genere. */
static void test_completion_eol_constants(void) {
    printf("\n[TEST] fix/1314 : completion des constantes de fin de ligne (RC/CR retires)\n");
    const char* src = "s est une chaîne = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 19);
    JsonValue* items = handle_completion(params);
    /* Les deux constantes canon restent proposees. */
    ASSERT_HAS(items, "LF",       "1314 : LF proposee");
    ASSERT_HAS(items, "CRLF",     "1314 : CRLF proposee");
    /* Temoin : le reste des « Constantes diverses » n'a pas bouge. */
    ASSERT_HAS(items, "TAB",      "1314 : TAB toujours proposee (temoin)");
    /* Les formes heritees sont invisibles a la completion. */
    ASSERT_NOT_HAS(items, "RC",   "1314 : RC absente (hors registre)");
    ASSERT_NOT_HAS(items, "CR",   "1314 : CR absente (hors registre)");
    json_free(items);
    json_free(params);
}

/*  — le dump generique des constantes du catalogue emet un sortText
 * deterministe (alphabetique sur le label affiche). Sans lui, VSCode retombe
 * sur l'ordre de declaration du catalogue et les langues sortent incoherentes
 * (Da/No/Fi/Gr/Tu/Ru/... au lieu de l'ordre alpha). On verrouille sur deux
 * paires dont l'ordre catalogue est INVERSE de l'ordre alpha :
 *   - langueFinnois declaree APRES langueNorvegien -> alpha Finnois < Norvegien
 *   - langueRusse   declaree APRES langueTurc       -> alpha Russe   < Turc     */
static void test_completion_2107_const_sorttext_alpha(void) {
    printf("\n[TEST] fix/2107 : dump generique constantes trie par sortText alphabetique\n");
    lspServer.language[0] = '\0';
    const char* src = "s est une chaîne = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 19);
    JsonValue* items = handle_completion(params);
    /* Valeur exacte : sortText = label en minuscule (insensible a la casse). */
    const char* stFin = item_sort_text(items, "langueFinnois");
    if (stFin && strcmp(stFin, "languefinnois") == 0) {
        g_pass++;
        printf("  [OK]   2107 : sortText('langueFinnois')=\"%s\"\n", stFin);
    } else {
        g_fail++;
        printf("  [FAIL] 2107 : sortText('langueFinnois')=\"%s\" attendu \"languefinnois\"\n",
               stFin ? stFin : "(null)");
    }
    /* Ordre alpha malgre l'ordre catalogue inverse (les 2 paires hors-ordre). */
    ASSERT_SORT_BEFORE(items, "langueFinnois", "langueNorv\xc3\xa9gien",
                       "2107 catalogue-inverse : Finnois < Norvegien");
    ASSERT_SORT_BEFORE(items, "langueRusse", "langueTurc",
                       "2107 catalogue-inverse : Russe < Turc");
    json_free(items);
    json_free(params);
}

/*  L1 — champs de type compose (Style/Cadre) FR-only en mode EN.
 * OwlComposedTypeField n'a pas de nameEN : l'emission (litteral `{ }`) ignore
 * wantEN et sort toujours les noms FR. 3 niveaux verifies : le champ de haut
 * niveau styleConteneur -> styleContainer (CadreExterieur/OuterFrame), le
 * sous-bloc OuterFrame lui-meme (CouleurFond/BackgroundColor), et le type
 * compose Cadre/Frame pris isolement (Couleur/Epaisseur -> Color/Thickness). */
static void test_completion_composed_field_lang_l1(void) {
    printf("\n[TEST] fix/1341 L1 : champs de type compose bilingues (litteral { })\n");

    /* --- Niveau 1 : styleConteneur, champ CadreExterieur/OuterFrame.
     * Source ECRITE EN FR dans les deux passes (seule lspServer.language
     * change) : le LHS ecrit "styleConteneur" matche le typeName FR du
     * catalogue (Cas 1 de la resolution brace-literal) quelle que soit la
     * langue de completion active — on force donc reellement le chemin
     * OWL_COMPOSED_TYPE_CATALOG vise par le bug, pas un contournement via
     * le registry bilingue generique. */
    const char* srcTop =
        "STY est un styleConteneur = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, srcTop);
    JsonValue* pTopFR = make_completion_params(URI, 1, 1);
    JsonValue* iTopFR = handle_completion(pTopFR);
    /* Le catalogue de fields stocke le fieldName accent-strippe (constat
     * deja documente ) : le label emis en FR est "CadreExterieur"
     * (ASCII), pas la forme accentuee. */
    ASSERT_HAS(iTopFR, "CadreExterieur", "L1 FR : CadreExterieur proposee (temoin)");
    json_free(iTopFR); json_free(pTopFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pTopEN = make_completion_params(URI, 1, 1);
    JsonValue* iTopEN = handle_completion(pTopEN);
    ASSERT_HAS    (iTopEN, "OuterFrame",     "L1 EN : OuterFrame proposee (styleConteneur)");
    ASSERT_NOT_HAS(iTopEN, "CadreExterieur", "L1 EN : CadreExterieur (FR) absente");
    json_free(iTopEN); json_free(pTopEN);
    lspServer.language[0] = '\0';

    /* --- Niveau 2 : sous-bloc CadreExterieur/OuterFrame, champ
     * CouleurFond/BackgroundColor. Meme discipline : LHS "CadreExterieur"
     * ecrit en FR dans les deux passes (matche le fieldName FR du Cas 2). */
    const char* srcSub =
        "STY est un styleConteneur = {\n"
        "\tCadreExt\xc3\xa9rieur = {\n"
        "\t\t\n"
        "\t}\n"
        "}\n";
    setup_doc(URI, srcSub);
    JsonValue* pSubFR = make_completion_params(URI, 2, 2);
    JsonValue* iSubFR = handle_completion(pSubFR);
    ASSERT_HAS(iSubFR, "CouleurFond", "L1 FR : CouleurFond proposee (temoin, sous-bloc)");
    json_free(iSubFR); json_free(pSubFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pSubEN = make_completion_params(URI, 2, 2);
    JsonValue* iSubEN = handle_completion(pSubEN);
    ASSERT_HAS    (iSubEN, "BackgroundColor", "L1 EN : BackgroundColor proposee (sous-bloc CadreExterieur)");
    ASSERT_NOT_HAS(iSubEN, "CouleurFond",     "L1 EN : CouleurFond (FR) absente (sous-bloc)");
    json_free(iSubEN); json_free(pSubEN);
    lspServer.language[0] = '\0';

    /* --- Type compose Cadre isole : Couleur/Epaisseur -> Color/Thickness.
     * LHS "Cadre" (FR, Cas 1) dans les deux passes. */
    const char* srcCadre =
        "monCadre est un Cadre = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, srcCadre);
    JsonValue* pCadreFR = make_completion_params(URI, 1, 1);
    JsonValue* iCadreFR = handle_completion(pCadreFR);
    ASSERT_HAS(iCadreFR, "Couleur", "L1 FR : Couleur proposee (temoin, type Cadre)");
    json_free(iCadreFR); json_free(pCadreFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pCadreEN = make_completion_params(URI, 1, 1);
    JsonValue* iCadreEN = handle_completion(pCadreEN);
    ASSERT_HAS    (iCadreEN, "Color",     "L1 EN : Color proposee (type Cadre)");
    ASSERT_HAS    (iCadreEN, "Thickness", "L1 EN : Thickness proposee (type Cadre)");
    ASSERT_NOT_HAS(iCadreEN, "Couleur",   "L1 EN : Couleur (FR) absente (type Cadre)");
    ASSERT_NOT_HAS(iCadreEN, "Epaisseur", "L1 EN : Epaisseur (FR) absente (type Cadre)");
    json_free(iCadreEN); json_free(pCadreEN);
    lspServer.language[0] = '\0';
}

/*  L2 — props via acces membre `..` sur une instance FR-only en mode
 * EN (alors que les MEMES props sont deja bilingues dans le bloc PROPRIETES).
 * Reprend le contexte de test_dot_dot_fenetre. */
static void test_completion_dotdot_props_lang_l2(void) {
    printf("\n[TEST] fix/1341 L2 : props via '..' bilingues (instance)\n");
    const char* src =
        "FEN_Login est une Fenetre\n"
        "\tTitre = \"x\"\n"
        "FIN\n"
        "\n"
        "x = FEN_Login..\n";
    setup_doc(URI, src);
    JsonValue* pFR = make_completion_params(URI, 4, 15);
    JsonValue* iFR = handle_completion(pFR);
    ASSERT_HAS(iFR, "Largeur", "L2 FR : Largeur proposee (temoin)");
    json_free(iFR); json_free(pFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN = make_completion_params(URI, 4, 15);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_HAS    (iEN, "Width",   "L2 EN : Width proposee (FEN_Login..)");
    ASSERT_NOT_HAS(iEN, "Largeur", "L2 EN : Largeur (FR) absente");
    json_free(iEN); json_free(pEN);
    lspServer.language[0] = '\0';
}

/*  Lot 2 — mise en forme conditionnelle par cellule de Table.
 * `TABLE_X[i][j]..` doit proposer Couleur/CouleurFond (+ formes EN Color/
 * BackgroundColor). Canon WL : NomTable[NumLigne][NumColonne].Propriete,
 * exactement 2 proprietes documentees (fiche Proprietes/Visuelles/2510071
 * + 2510022). RED attendu avant impl : le double-indice sur un widget Table
 * n'est pas encore un descripteur porteur de proprietes. */
static void test_completion_table_cell_couleur(void) {
    printf("\n[TEST] feat/1452 : TABLE_X[i][j].. propose Couleur/CouleurFond\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_Y est une Colonne\n"
        "\t\t\tType = colTypeNumerique\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tx = TABLE_X[1][1]..\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* pFR = make_completion_params(URI, 7, 20);
    JsonValue* iFR = handle_completion(pFR);
    ASSERT_HAS(iFR, "Couleur",      "1452 FR : Couleur proposee (TABLE_X[i][j]..)");
    ASSERT_HAS(iFR, "CouleurFond",  "1452 FR : CouleurFond proposee (TABLE_X[i][j]..)");
    json_free(iFR); json_free(pFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN = make_completion_params(URI, 7, 20);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_HAS    (iEN, "Color",           "1452 EN : Color proposee (TABLE_X[i][j]..)");
    ASSERT_HAS    (iEN, "BackgroundColor", "1452 EN : BackgroundColor proposee (TABLE_X[i][j]..)");
    ASSERT_NOT_HAS(iEN, "Couleur",         "1452 EN : Couleur (FR) absente");
    json_free(iEN); json_free(pEN);
    lspServer.language[0] = '\0';
}

/*  Lot 2c — forme colonne-first `TABLE_X.COL_Y[1]..` doit proposer
 * les memes proprietes de cellule que la forme `TABLE_X[i][j]..`. Le negatif
 * compte autant : `COL_Y[1]..` seul, sans racine Champ Table, ne doit
 * proposer aucun item — c'est le garde anti- (le niveau colonne n'est
 * pas livre, la completion ne doit pas promettre une forme que le
 * compilateur refuse). */
static void test_completion_table_colfirst_couleur(void) {
    printf("\n[TEST] feat/1452 Lot 2c : TABLE_X.COL_Y[1].. propose Couleur/CouleurFond\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_Y est une Colonne\n"
        "\t\t\tType = colTypeNumerique\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tx = TABLE_X.COL_Y[1]..\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* pFR = make_completion_params(URI, 7, 24);
    JsonValue* iFR = handle_completion(pFR);
    ASSERT_HAS(iFR, "Couleur",     "1452c FR : Couleur proposee (TABLE_X.COL_Y[1]..)");
    ASSERT_HAS(iFR, "CouleurFond", "1452c FR : CouleurFond proposee (TABLE_X.COL_Y[1]..)");
    json_free(iFR); json_free(pFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN = make_completion_params(URI, 7, 24);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_HAS    (iEN, "Color",           "1452c EN : Color proposee (TABLE_X.COL_Y[1]..)");
    ASSERT_HAS    (iEN, "BackgroundColor", "1452c EN : BackgroundColor proposee (TABLE_X.COL_Y[1]..)");
    ASSERT_NOT_HAS(iEN, "Couleur",         "1452c EN : Couleur (FR) absente");
    json_free(iEN); json_free(pEN);
    lspServer.language[0] = '\0';

    /* Negatif : `COL_Y[1]..` seul (sans `TABLE_X.` devant) — pas de racine
     * Champ Table, donc 0 item. C'est aussi le garde anti- : le
     * niveau colonne seul n'est pas livre, la completion ne doit pas
     * promettre ce que le compilateur refuse. */
    const char* srcSeul =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_Y est une Colonne\n"
        "\t\t\tType = colTypeNumerique\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tx = COL_Y[1]..\n"
        "FIN\n";
    setup_doc(URI, srcSeul);
    JsonValue* pSeul = make_completion_params(URI, 7, 16);
    JsonValue* iSeul = handle_completion(pSeul);
    if (count_items(iSeul) == 0) {
        g_pass++;
        printf("  [OK]   1452c negatif : COL_Y[1].. seul -> 0 item\n");
    } else {
        g_fail++;
        printf("  [FAIL] 1452c negatif : COL_Y[1].. seul -> attendu 0 item, obtenu %d\n",
               count_items(iSeul));
    }
    json_free(iSeul); json_free(pSeul);
}

/*  — receveur colonne NU `COL_A..` (sans crochet, sans racine Table
 * devant). La colonne unique d'une Table du fichier se resout comme receveur
 * de mise en forme : la completion doit proposer Couleur/CouleurFond (formes
 * EN Color/BackgroundColor), les DEUX seules props que l'ecriture nue accepte
 *. Elle ne doit PAS proposer les props declaratives de la colonne
 * (Libellé/Type/Visible/...), que le compilateur refuse a l'ecriture nue. */
static void test_completion_bare_column_couleur(void) {
    printf("\n[TEST] fix/1475 : COL_A.. (receveur nu) propose Couleur/CouleurFond\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_A est une Colonne\n"
        "\t\t\tType = colTypeTexte\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tCOL_A..\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 7 = "\tCOL_A.." : tab(0) C(1)O(2)L(3)_(4)A(5) .(6) .(7) ; curseur
     * juste apres le second point = colonne 8. */
    JsonValue* pFR = make_completion_params(URI, 7, 8);
    JsonValue* iFR = handle_completion(pFR);
    ASSERT_HAS    (iFR, "Couleur",     "1475 FR : Couleur proposee (COL_A..)");
    ASSERT_HAS    (iFR, "CouleurFond", "1475 FR : CouleurFond proposee (COL_A..)");
    ASSERT_NOT_HAS(iFR, "Libell\xc3\xa9", "1475 FR : Libellé ABSENT (refuse a l'ecriture nue)");
    ASSERT_NOT_HAS(iFR, "Visible",     "1475 FR : Visible ABSENT (refuse a l'ecriture nue)");
    json_free(iFR); json_free(pFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN = make_completion_params(URI, 7, 8);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_HAS    (iEN, "Color",           "1475 EN : Color proposee (COL_A..)");
    ASSERT_HAS    (iEN, "BackgroundColor", "1475 EN : BackgroundColor proposee (COL_A..)");
    ASSERT_NOT_HAS(iEN, "Couleur",         "1475 EN : Couleur (FR) absente");
    json_free(iEN); json_free(pEN);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *  — la completion apres `..` doit etre filtree par le type du
 * receveur. Avant ce ticket, les NEUF kinds builtin (tableau, file, pile,
 * liste, JSON, date, heure, dateheure, duree) recevaient la MEME liste de
 * 17 items : `t..` sur un tableau d'entiers proposait `PartieDate` et
 * `EnJours`, `d..` sur une Date proposait `Occurrence`.
 *
 * Le livrable est le NEGATIF. Verifier qu'`Occurrence` est proposee sur un
 * tableau n'attrape rien : elle l'etait deja. Seule l'assertion « EnJours
 * est ABSENTE de la liste d'un tableau » verrouille le filtre. On y ajoute
 * le cardinal exact : une liste filtree se mesure a son compte, sinon une
 * fuite d'un seul item passe entre les ASSERT_NOT_HAS choisis a la main.
 * ================================================================ */

/* Pose `<decl>` en ligne 0 et `x = <var>..` en ligne 1, puis rend les items.
 * Le curseur est en fin de ligne 1 (apres le second point). */
static JsonValue* complete_dotdot_on(const char* decl, const char* varName) {
    char src[512];
    snprintf(src, sizeof(src), "%s\nx = %s..\n", decl, varName);
    setup_doc(URI, src);
    int character = 4 + (int)strlen(varName) + 2;  /* "x = " + var + ".." */
    JsonValue* params = make_completion_params(URI, 1, character);
    JsonValue* items = handle_completion(params);
    json_free(params);
    return items;
}

static void test_completion_dotdot_builtin_kind_filter(void) {
    printf("\n[TEST] fix/1515 : `..` filtre par kind builtin (les 9 kinds)\n");

    /* --- tableau : Occurrence + Vide, rien d'autre. --- */
    JsonValue* iTab = complete_dotdot_on("t est un tableau de 3 entiers", "t");
    ASSERT_COUNT  (iTab, 2,               "1515 tableau : cardinal");
    ASSERT_HAS    (iTab, "Occurrence",    "1515 tableau : Occurrence (temoin)");
    ASSERT_HAS    (iTab, "Vide",          "1515 tableau : Vide (temoin)");
    ASSERT_NOT_HAS(iTab, "PartieDate",    "1515 tableau : PartieDate ABSENTE");
    ASSERT_NOT_HAS(iTab, "EnJours",       "1515 tableau : EnJours ABSENTE");
    ASSERT_NOT_HAS(iTab, "EnHeures",      "1515 tableau : EnHeures ABSENTE");
    ASSERT_NOT_HAS(iTab, "Ann\xc3\xa9""e","1515 tableau : Annee ABSENTE");
    ASSERT_NOT_HAS(iTab, "Type",          "1515 tableau : Type (JSON) ABSENTE");
    json_free(iTab);

    /* --- file / pile / liste : meme table PROPS_COLLECTION. --- */
    JsonValue* iFile = complete_dotdot_on("f est une file de entier", "f");
    ASSERT_COUNT  (iFile, 2,            "1515 file : cardinal");
    ASSERT_HAS    (iFile, "Occurrence", "1515 file : Occurrence (temoin)");
    ASSERT_NOT_HAS(iFile, "EnJours",    "1515 file : EnJours ABSENTE");
    ASSERT_NOT_HAS(iFile, "Type",       "1515 file : Type (JSON) ABSENTE");
    json_free(iFile);

    JsonValue* iPile = complete_dotdot_on("p est une pile de entier", "p");
    ASSERT_COUNT  (iPile, 2,            "1515 pile : cardinal");
    ASSERT_HAS    (iPile, "Vide",       "1515 pile : Vide (temoin)");
    ASSERT_NOT_HAS(iPile, "PartieHeure","1515 pile : PartieHeure ABSENTE");
    json_free(iPile);

    JsonValue* iListe = complete_dotdot_on("l est une liste de entier", "l");
    ASSERT_COUNT  (iListe, 2,             "1515 liste : cardinal");
    ASSERT_HAS    (iListe, "Occurrence",  "1515 liste : Occurrence (temoin)");
    ASSERT_NOT_HAS(iListe, "Milliseconde","1515 liste : Milliseconde ABSENTE");
    json_free(iListe);

    /* --- JSON : Occurrence + Vide + Type. --- */
    JsonValue* iJson = complete_dotdot_on("j est un json", "j");
    ASSERT_COUNT  (iJson, 3,            "1515 JSON : cardinal");
    ASSERT_HAS    (iJson, "Type",       "1515 JSON : Type (temoin, propre au JSON)");
    ASSERT_HAS    (iJson, "Occurrence", "1515 JSON : Occurrence (temoin)");
    ASSERT_NOT_HAS(iJson, "Jour",       "1515 JSON : Jour ABSENTE");
    ASSERT_NOT_HAS(iJson, "EnSecondes", "1515 JSON : EnSecondes ABSENTE");
    json_free(iJson);

    /* --- date : Annee / Mois / Jour seulement. --- */
    JsonValue* iDate = complete_dotdot_on("d est une date", "d");
    ASSERT_COUNT  (iDate, 3,                "1515 date : cardinal");
    ASSERT_HAS    (iDate, "Ann\xc3\xa9""e", "1515 date : Annee (temoin)");
    ASSERT_HAS    (iDate, "Mois",           "1515 date : Mois (temoin)");
    ASSERT_HAS    (iDate, "Jour",           "1515 date : Jour (temoin)");
    ASSERT_NOT_HAS(iDate, "Occurrence",     "1515 date : Occurrence ABSENTE");
    ASSERT_NOT_HAS(iDate, "Heure",          "1515 date : Heure ABSENTE");
    ASSERT_NOT_HAS(iDate, "PartieDate",     "1515 date : PartieDate ABSENTE");
    ASSERT_NOT_HAS(iDate, "EnJours",        "1515 date : EnJours ABSENTE");
    json_free(iDate);

    /* --- heure : Heure / Minute / Seconde / Milliseconde. --- */
    JsonValue* iHeure = complete_dotdot_on("h est une heure", "h");
    ASSERT_COUNT  (iHeure, 4,                 "1515 heure : cardinal");
    ASSERT_HAS    (iHeure, "Heure",           "1515 heure : Heure (temoin)");
    ASSERT_HAS    (iHeure, "Milliseconde",    "1515 heure : Milliseconde (temoin)");
    ASSERT_NOT_HAS(iHeure, "Ann\xc3\xa9""e",  "1515 heure : Annee ABSENTE");
    ASSERT_NOT_HAS(iHeure, "Occurrence",      "1515 heure : Occurrence ABSENTE");
    ASSERT_NOT_HAS(iHeure, "EnMinutes",       "1515 heure : EnMinutes ABSENTE");
    json_free(iHeure);

    /* --- dateheure : les 7 composantes + les 2 extractions de partie. --- */
    JsonValue* iDh = complete_dotdot_on("dh est une dateheure", "dh");
    ASSERT_COUNT  (iDh, 9,                 "1515 dateheure : cardinal");
    ASSERT_HAS    (iDh, "PartieDate",      "1515 dateheure : PartieDate (temoin)");
    ASSERT_HAS    (iDh, "PartieHeure",     "1515 dateheure : PartieHeure (temoin)");
    ASSERT_HAS    (iDh, "Ann\xc3\xa9""e",  "1515 dateheure : Annee (temoin)");
    ASSERT_HAS    (iDh, "Seconde",         "1515 dateheure : Seconde (temoin)");
    ASSERT_NOT_HAS(iDh, "Occurrence",      "1515 dateheure : Occurrence ABSENTE");
    ASSERT_NOT_HAS(iDh, "EnJours",         "1515 dateheure : EnJours ABSENTE");
    ASSERT_NOT_HAS(iDh, "Type",            "1515 dateheure : Type (JSON) ABSENTE");
    json_free(iDh);

    /* --- duree : les 5 totaux `En...` + les 5 composantes fines, que le
     * compilateur accepte aussi sur une duree (cf. PROPS_DURATION).
     * NB : la variable s'appelle `dur`, pas `du` — un receveur nomme `du`
     * n'est resolu par AUCUNE branche de la completion (0 item quel que
     * soit son type). Defaut distinct, hors perimetre de ce ticket. --- */
    JsonValue* iDur = complete_dotdot_on("dur est une dur\xc3\xa9""e", "dur");
    ASSERT_COUNT  (iDur, 10,                 "1515 duree : cardinal");
    ASSERT_HAS    (iDur, "EnJours",          "1515 duree : EnJours (temoin)");
    ASSERT_HAS    (iDur, "EnMillisecondes",  "1515 duree : EnMillisecondes (temoin)");
    ASSERT_HAS    (iDur, "Jour",             "1515 duree : Jour (composante fine)");
    ASSERT_HAS    (iDur, "Milliseconde",     "1515 duree : Milliseconde (composante fine)");
    ASSERT_NOT_HAS(iDur, "Occurrence",       "1515 duree : Occurrence ABSENTE");
    ASSERT_NOT_HAS(iDur, "Ann\xc3\xa9""e",   "1515 duree : Annee ABSENTE");
    ASSERT_NOT_HAS(iDur, "Mois",             "1515 duree : Mois ABSENTE");
    ASSERT_NOT_HAS(iDur, "PartieDate",       "1515 duree : PartieDate ABSENTE");
    ASSERT_NOT_HAS(iDur, "Type",             "1515 duree : Type (JSON) ABSENTE");
    json_free(iDur);

    /* --- Le filtre suit la langue : en EN, la liste d'une date reste de
     * cardinal 3, avec les libelles EN et sans fuite d'un autre kind. --- */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* iDateEN = complete_dotdot_on("d est une date", "d");
    ASSERT_COUNT  (iDateEN, 3,          "1515 date EN : cardinal");
    ASSERT_HAS    (iDateEN, "Year",     "1515 date EN : Year proposee");
    ASSERT_NOT_HAS(iDateEN, "InDays",   "1515 date EN : InDays ABSENTE");
    ASSERT_NOT_HAS(iDateEN, "Count",    "1515 date EN : Count ABSENTE");
    json_free(iDateEN);
    lspServer.language[0] = '\0';
}

/*  — non-regression des surfaces qui filtraient DEJA correctement.
 * Ce sont elles le patron : si le filtre builtin les abime, c'est ici que
 * ca se voit. Cardinaux releves sur l'etat d'avant le correctif. */
static void test_completion_dotdot_widget_counts_unchanged(void) {
    printf("\n[TEST] fix/1515 : non-regression des cardinaux widget\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLIB_A est un Champ Libelle\n"
        "\tFIN\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_Y est une Colonne\n"
        "\t\t\tType = colTypeNumerique\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tx = LIB_A..\n"
        "\ty = TABLE_X..\n"
        "\tz = TABLE_X[1][1]..\n"
        "FIN\n";
    setup_doc(URI, src);

    JsonValue* p1 = make_completion_params(URI, 9, 12);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_COUNT(i1, 54, "1515 non-reg : LIB_A.. = 54 props Libelle");
    json_free(i1); json_free(p1);

    JsonValue* p2 = make_completion_params(URI, 10, 14);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_COUNT(i2, 42, "1515/1881 non-reg : TABLE_X.. = 42 props Table (fix/1881 : +Etat +Police(FONT_PROPS) +Libelle ; feat/2122 : +ModeLoupe ; feat/2143 : +MenuLoupe)");
    json_free(i2); json_free(p2);

    JsonValue* p3 = make_completion_params(URI, 11, 22);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_COUNT(i3, 3, "1515 non-reg : TABLE_X[1][1].. = 3 props cellule (Couleur, CouleurFond ; feat/1595 : +Contenu)");
    json_free(i3); json_free(p3);
}

/*  — non-regression du POINT SIMPLE sur les types builtin. Depuis
 * , `.` designe un membre ou une methode, jamais une propriete. Le
 * filtrage pose sur `..` ne doit pas deborder sur cette surface : les
 * cardinaux de methodes restent ceux mesures avant correctif. */
static void test_completion_single_dot_methods_unchanged(void) {
    printf("\n[TEST] fix/1515 : non-regression du point simple (methodes)\n");

    setup_doc(URI, "t est un tableau de 3 entiers\nx = t.\n");
    JsonValue* p1 = make_completion_params(URI, 1, 6);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_COUNT  (i1, 24,           "1515 non-reg : t. = 24 methodes tableau");
    ASSERT_NOT_HAS(i1, "Occurrence", "1515 non-reg : t. sans propriete (ADR_0151)");
    json_free(i1); json_free(p1);

    /* Date : 16 methodes. `Mois` en fait partie — c'est la forme methode
     * `d.Mois()`, doublon type-vs-methode assume (invariant 5), donc sa
     * presence est correcte. Le garde porte sur `Occurrence`, propriete
     * d'un AUTRE kind, qui n'a rien a faire ici ni avant ni apres. */
    setup_doc(URI, "d est une date\nx = d.\n");
    JsonValue* p2 = make_completion_params(URI, 1, 6);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_COUNT  (i2, 16,           "1515 non-reg : d. = 16 methodes date");
    ASSERT_HAS    (i2, "EstValide",  "1515 non-reg : d. propose EstValide (methode)");
    ASSERT_NOT_HAS(i2, "Occurrence", "1515 non-reg : d. sans propriete d'un autre kind");
    json_free(i2); json_free(p2);

    setup_doc(URI, "cnx est une sqlConnexion\nx = cnx.\n");
    JsonValue* p3 = make_completion_params(URI, 1, 8);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_COUNT(i3, 2, "1515 non-reg : cnx. = 2 methodes sqlConnexion");
    json_free(i3); json_free(p3);
}

/*  second defaut — apres un point SIMPLE sur un Champ Table, le nom
 * de colonne est exactement la bonne suggestion (`TABLE_X[1].COL_A` en acces
 * valeur, `TABLE_X.COL_A[1]..Couleur` en designateur colonne-first). La
 * completion y rendait 0 item.
 *
 * Le negatif borne la promesse : deux niveaux de crochets designent une
 * CELLULE, pas une ligne — aucun nom de colonne n'y a de sens, et une
 * colonne d'une AUTRE table ne doit jamais fuiter. */
static void test_completion_table_columns_single_dot(void) {
    printf("\n[TEST] fix/1515 : noms de colonnes apres point simple sur Table\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_A est une Colonne\n"
        "\t\t\tType = colTypeNumerique\n"
        "\t\tFIN\n"
        "\t\tCOL_B est une Colonne\n"
        "\t\t\tType = colTypeTexte\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tTABLE_Z est un Champ Table\n"
        "\t\tCOL_AUTRE est une Colonne\n"
        "\t\t\tType = colTypeTexte\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tx = TABLE_X.\n"
        "\ty = TABLE_X[1].\n"
        "\tz = TABLE_X[1][1].\n"
        "FIN\n";
    setup_doc(URI, src);

    /* Designateur colonne-first : `TABLE_X.` propose les colonnes. */
    JsonValue* p1 = make_completion_params(URI, 15, 13);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_COUNT  (i1, 2,           "1515 colonnes : TABLE_X. = 2 colonnes");
    ASSERT_HAS    (i1, "COL_A",     "1515 colonnes : COL_A proposee");
    ASSERT_HAS    (i1, "COL_B",     "1515 colonnes : COL_B proposee");
    ASSERT_NOT_HAS(i1, "COL_AUTRE", "1515 colonnes : COL_AUTRE (autre Table) ABSENTE");
    json_free(i1); json_free(p1);

    /* Acces valeur : `TABLE_X[1].` propose les memes colonnes. */
    JsonValue* p2 = make_completion_params(URI, 16, 16);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_COUNT  (i2, 2,           "1515 colonnes : TABLE_X[1]. = 2 colonnes");
    ASSERT_HAS    (i2, "COL_A",     "1515 colonnes : COL_A proposee (ligne)");
    ASSERT_NOT_HAS(i2, "COL_AUTRE", "1515 colonnes : COL_AUTRE ABSENTE (ligne)");
    json_free(i2); json_free(p2);

    /* Negatif : deux crochets = une CELLULE. Pas de colonne a proposer. */
    JsonValue* p3 = make_completion_params(URI, 17, 19);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_NOT_HAS(i3, "COL_A", "1515 colonnes : TABLE_X[1][1]. sans COL_A (cellule)");
    ASSERT_NOT_HAS(i3, "COL_B", "1515 colonnes : TABLE_X[1][1]. sans COL_B (cellule)");
    json_free(i3); json_free(p3);
}

/*  L3 — methodes point FR-only DANS LES DEUX LANGUES : l'emission
 * saute les entrees isAlias (les formes EN sont justement des alias), donc
 * jamais proposees, meme en mode EN. */
static void test_completion_dot_methods_lang_l3(void) {
    printf("\n[TEST] fix/1341 L3 : methodes point bilingues (chaine./tableau.)\n");
    /* chaine : FR propose Majuscule (temoin), EN doit proposer Upper. */
    const char* srcStr =
        "s est une cha\xc3\xaene\n"
        "x = s.\n";
    setup_doc(URI, srcStr);
    JsonValue* pFR = make_completion_params(URI, 1, 6);
    JsonValue* iFR = handle_completion(pFR);
    ASSERT_HAS(iFR, "Majuscule", "L3 FR : Majuscule proposee (chaine., temoin)");
    json_free(iFR); json_free(pFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN = make_completion_params(URI, 1, 6);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_HAS    (iEN, "Upper",     "L3 EN : Upper proposee (chaine.)");
    /* Anti-doublon : une seule entree par methode, la forme FR ne doit pas
     * cohabiter avec l'EN dans la meme liste. */
    ASSERT_NOT_HAS(iEN, "Majuscule", "L3 EN : Majuscule (FR) absente (pas de doublon)");
    json_free(iEN); json_free(pEN);
    lspServer.language[0] = '\0';

    /* tableau : EN doit proposer Add (Ajoute). */
    const char* srcTab =
        "t est un tableau de entier\n"
        "t.\n";
    setup_doc(URI, srcTab);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN2 = make_completion_params(URI, 1, 2);
    JsonValue* iEN2 = handle_completion(pEN2);
    ASSERT_HAS    (iEN2, "Add",    "L3 EN : Add proposee (tableau.)");
    ASSERT_NOT_HAS(iEN2, "Ajoute", "L3 EN : Ajoute (FR) absente (pas de doublon)");
    json_free(iEN2); json_free(pEN2);
    lspServer.language[0] = '\0';
}

/*  L4 — sous-blocs d'Etat jamais offerts en completion (ni FR ni EN)
 * alors que la donnee bilingue existe (ui_decl.c CONTROL_KINDS). RED dans les
 * deux langues. */
static void test_completion_etat_subblocks_lang_l4(void) {
    printf("\n[TEST] fix/1341 L4 : sous-blocs Etat proposes (FR+EN, jamais offerts avant)\n");
    const char* src =
        "ETAT_X est un Etat\n"
        "\tINTRO est un \n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* pFR = make_completion_params(URI, 1, 14);
    JsonValue* iFR = handle_completion(pFR);
    ASSERT_HAS(iFR, "Corps",          "L4 FR : Corps proposee");
    ASSERT_HAS(iFR, "EnT\xc3\xaateDocument", "L4 FR : EnT\xc3\xaateDocument proposee");
    ASSERT_HAS(iFR, "HautDePage",     "L4 FR : HautDePage proposee");
    ASSERT_HAS(iFR, "BasDePage",      "L4 FR : BasDePage proposee");
    ASSERT_HAS(iFR, "FinDeDocument",  "L4 FR : FinDeDocument proposee");
    /*  — Iteration etait deja propose avant l'accentuation du canon,
     * mais jamais affirme : c'est ce trou qui a laisse la regression passer.
     * Verrou pose sur les deux langues (meme mot en EN). */
    ASSERT_HAS(iFR, "It\xc3\xa9ration",      "L4 FR : It\xc3\xa9ration proposee");
    json_free(iFR); json_free(pFR);

    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* pEN = make_completion_params(URI, 1, 14);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_HAS(iEN, "Body",           "L4 EN : Body proposee");
    ASSERT_HAS(iEN, "DocumentHeader", "L4 EN : DocumentHeader proposee");
    ASSERT_HAS(iEN, "PageHeader",     "L4 EN : PageHeader proposee");
    ASSERT_HAS(iEN, "PageFooter",     "L4 EN : PageFooter proposee");
    ASSERT_HAS(iEN, "DocumentFooter", "L4 EN : DocumentFooter proposee");
    ASSERT_HAS(iEN, "Iteration",      "L4 EN : Iteration proposee");
    /* Anti-doublon : une seule forme par bloc, le FR ne fuite pas en EN. */
    ASSERT_NOT_HAS(iEN, "Corps",          "L4 EN : Corps (FR) absente");
    ASSERT_NOT_HAS(iEN, "EnTeteDocument", "L4 EN : EnTeteDocument (FR heritee) absente");
    /*  — le canon FR porte desormais l'accent : sans ce verrou, la
     * ligne ci-dessus ne surveille plus aucune etiquette reellement emise. */
    ASSERT_NOT_HAS(iEN, "EnT\xc3\xaateDocument", "L4 EN : EnT\xc3\xaateDocument (FR canon) absente");
    ASSERT_NOT_HAS(iEN, "It\xc3\xa9ration",      "L4 EN : It\xc3\xa9ration (FR canon) absente");
    ASSERT_NOT_HAS(iEN, "HautDePage",     "L4 EN : HautDePage (FR) absente");
    ASSERT_NOT_HAS(iEN, "BasDePage",      "L4 EN : BasDePage (FR) absente");
    ASSERT_NOT_HAS(iEN, "FinDeDocument",  "L4 EN : FinDeDocument (FR) absente");
    json_free(iEN); json_free(pEN);
    lspServer.language[0] = '\0';
}

static void test_body_saisie_completion(void) {
    printf("\n[TEST] Body Saisie : Ctrl+Espace propose props Saisie\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une Saisie\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Valeur",         "Body Saisie");
    ASSERT_HAS(items, "TypeSaisie",     "Body Saisie");
    ASSERT_HAS(items, "Indication",     "Body Saisie");
    ASSERT_HAS(items, "MaxLongueur",    "Body Saisie");
    json_free(items);
    json_free(params);
}

static void test_body_libelle_completion(void) {
    printf("\n[TEST] Body Libelle : Ctrl+Espace propose props Libelle\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_X est un Libelle\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Libell\xc3\xa9",         "Body Libelle");
    ASSERT_HAS(items, "Couleur",                "Body Libelle");
    ASSERT_HAS(items, "TypeLibell\xc3\xa9",     "Body Libelle");
    json_free(items);
    json_free(params);
}

/*  (support #167) — `IDENT est un Champ <cursor>` (FR) / `is a Control|
 * Field <cursor>` (EN) doit proposer les types de contrôle concrets (Bouton,
 * Saisie, Libellé…), pas les primitifs ni les props du widget englobant. */
static void test_champ_modifier_types_completion(void) {
    printf("\n[TEST] fix/1151 : `est un Champ ` / `is a Control ` propose les types de controle\n");
    /* Ligne 1 : `\tSAI_X est un Champ ` — curseur apres le dernier espace (col 20). */
    const char* srcFR =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est un Champ \n"
        "FIN\n";
    setup_doc(URI, srcFR);
    JsonValue* params = make_completion_params(URI, 1, 20);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Bouton",         "fix/1151 Champ");
    ASSERT_HAS(items, "Saisie",         "fix/1151 Champ");
    ASSERT_HAS(items, "Libell\xc3\xa9", "fix/1151 Champ");
    json_free(items);
    json_free(params);

    /* Modificateur EN `Control` (= nameEN de Champ) reconnu pareillement. */
    const char* srcEN =
        "FEN_X est une Fenetre\n"
        "\tSAI_X is a Control \n"
        "FIN\n";
    setup_doc(URI, srcEN);
    params = make_completion_params(URI, 1, 20);
    items = handle_completion(params);
    ASSERT_HAS(items, "Bouton", "fix/1151 Control");
    ASSERT_HAS(items, "Saisie", "fix/1151 Control");
    json_free(items);
    json_free(params);

    /*  (support #167) — curseur COLLÉ après le modificateur, sans
     * espace : `\tSAI_X est un Champ` avec le curseur juste après le 'p'
     * (col 19). Config symétrique du cas espace jamais couverte par  :
     * le lookback tombait sur le cas type-partiel et proposait les primitifs
     * (entier…) au lieu des types de contrôle. */
    const char* srcGlued =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est un Champ\n"
        "FIN\n";
    setup_doc(URI, srcGlued);
    params = make_completion_params(URI, 1, 19);
    items = handle_completion(params);
    ASSERT_HAS(items, "Bouton",         "fix/1288 Champ collé (sans espace)");
    ASSERT_HAS(items, "Saisie",         "fix/1288 Champ collé (sans espace)");
    ASSERT_HAS(items, "Libell\xc3\xa9", "fix/1288 Champ collé (sans espace)");
    ASSERT_NOT_HAS(items, "entier",     "fix/1288 pas de primitif en cas collé");
    json_free(items);
    json_free(params);

    /* Idem modificateur EN `Control` collé (reconnu quelle que soit la langue,
     * cf. bloc EN de  ci-dessus) : `\tSAI_X is a Control` curseur après
     * le 'l' (col 19 : \t + "SAI_X is a Control" = 1+18). */
    const char* srcGluedEN =
        "FEN_X est une Fenetre\n"
        "\tSAI_X is a Control\n"
        "FIN\n";
    setup_doc(URI, srcGluedEN);
    params = make_completion_params(URI, 1, 19);
    items = handle_completion(params);
    ASSERT_HAS(items, "Bouton", "fix/1288 Control collé (EN)");
    ASSERT_HAS(items, "Saisie", "fix/1288 Control collé (EN)");
    json_free(items);
    json_free(params);
}

/*  (support #431) — forme EN SANS article : `SAI_X is Control <cursor>`
 * (et collé `is Control|`). Le fix #167 ne couvrait que la forme AVEC article
 * (`is a Control`) ; sans l'article, aucune branche typeDecl ne matchait le
 * modificateur -> completion generique au lieu des types de controle. On exige
 * la parite : apres `is Control` (sans article), proposer Bouton/Saisie/… et
 * PAS les primitifs. */
static void test_champ_modifier_no_article_en(void) {
    printf("\n[TEST] fix/2281 : `is Control ` / `is Control|` (sans article, EN) propose les types de controle\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* --- Cas espace : `\tSAI_X is Control ` curseur apres l'espace final (col 18). --- */
    const char* srcSpace =
        "FEN_X est une Fenetre\n"
        "\tSAI_X is Control \n"
        "FIN\n";
    setup_doc(URI, srcSpace);
    JsonValue* params = make_completion_params(URI, 1, 18);
    JsonValue* items = handle_completion(params);
    /* Locale EN -> labels EN canon des types de contrôle (Button/Input). */
    ASSERT_HAS    (items, "Button",  "fix/2281 is Control (espace) -> Button");
    ASSERT_HAS    (items, "Input",   "fix/2281 is Control (espace) -> Input");
    ASSERT_NOT_HAS(items, "entier",  "fix/2281 is Control (espace) pas de primitif");
    ASSERT_NOT_HAS(items, "integer", "fix/2281 is Control (espace) pas de primitif EN");
    json_free(items);
    json_free(params);

    /* --- Cas collé : `\tSAI_X is Control` curseur apres le 'l' (col 17). --- */
    const char* srcGlued =
        "FEN_X est une Fenetre\n"
        "\tSAI_X is Control\n"
        "FIN\n";
    setup_doc(URI, srcGlued);
    params = make_completion_params(URI, 1, 17);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Button",  "fix/2281 is Control (collé) -> Button");
    ASSERT_HAS    (items, "Input",   "fix/2281 is Control (collé) -> Input");
    ASSERT_NOT_HAS(items, "entier",  "fix/2281 is Control (collé) pas de primitif");
    ASSERT_NOT_HAS(items, "integer", "fix/2281 is Control (collé) pas de primitif EN");
    json_free(items);
    json_free(params);

    lspServer.language[0] = '\0';
}

/*  item 3 — `IDENT est un <cursor>` (FR) / `is a <cursor>` (EN) doit
 * aussi proposer les mots-cles de declaration menu (MenuPrincipal / SousMenu /
 * OptionMenu / MenuSeparateur / MenuContextuel), en plus des types primitifs /
 * composes / Fenetre-Etat. Ce sont des ouvreurs de bloc menu (is_widget=false,
 * hors table pour MenuContextuel) qui n'apparaissaient nulle part dans la
 * completion typeDecl. */
static void test_menu_kind_types_completion(void) {
    printf("\n[TEST] feat/1214 : `est un ` propose les mots-cles menu (FR/EN)\n");
    /* --- FR (defaut). Curseur apres `MNU_X est un ` (col 13). --- */
    const char* srcFR = "MNU_X est un \n";
    setup_doc(URI, srcFR);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "MenuPrincipal",           "feat/1214 menu FR MenuPrincipal");
    ASSERT_HAS(items, "SousMenu",                "feat/1214 menu FR SousMenu");
    ASSERT_HAS(items, "OptionMenu",              "feat/1214 menu FR OptionMenu");
    ASSERT_HAS(items, "MenuS\xc3\xa9parateur",   "feat/1214 menu FR MenuSeparateur");
    ASSERT_HAS(items, "MenuContextuel",          "feat/1214 menu FR MenuContextuel");
    json_free(items);
    json_free(params);

    /* --- EN. Curseur apres `MNU_X is a ` (col 11). --- */
    const char* srcEN = "MNU_X is a \n";
    setup_doc(URI, srcEN);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 0, 11);
    items = handle_completion(params);
    ASSERT_HAS(items, "MainMenu",      "feat/1214 menu EN MainMenu");
    ASSERT_HAS(items, "Submenu",       "feat/1214 menu EN Submenu");
    ASSERT_HAS(items, "MenuOption",    "feat/1214 menu EN MenuOption");
    ASSERT_HAS(items, "MenuSeparator", "feat/1214 menu EN MenuSeparator");
    ASSERT_HAS(items, "ContextMenu",   "feat/1214 menu EN ContextMenu");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  (support #309) — `COL_X est une <cursor>` dans le corps d'un Champ
 * Table doit proposer le type `Colonne`/`Column`. Colonne est is_widget=false
 * (sous-element de Table, ui_decl.c) donc absente de la boucle CONTROL_KINDS
 * de la completion typeDecl -> jamais offerte. On l'offre en CONTEXTE Table
 * (la ou une colonne se declare legitimement). Le negatif compte : hors d'un
 * Champ Table, `X est une <cursor>` ne doit PAS proposer Colonne (ce serait
 * refuse par le compilateur). */
static void test_colonne_type_completion_in_table(void) {
    printf("\n[TEST] fix/1883 : `COL_X est une ` dans un Champ Table propose Colonne/Column\n");
    /* --- FR (defaut). Curseur apres `\t\tCOL_X est une ` (col 16). --- */
    const char* srcFR =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_X est une \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcFR);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 2, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Colonne", "fix/1883 FR : Colonne proposee dans corps Table");
    json_free(items);
    json_free(params);

    /* --- EN. Curseur apres `\t\tCOL_X is a ` (col 13). --- */
    const char* srcEN =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_X is a \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcEN);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 2, 13);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Column",  "fix/1883 EN : Column proposee dans corps Table");
    ASSERT_NOT_HAS(items, "Colonne", "fix/1883 EN : Colonne (FR) absente");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';

    /* --- Negatif : hors d'un Champ Table, Colonne ne doit PAS etre proposee. --- */
    const char* srcOut =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une \n"
        "FIN\n";
    setup_doc(URI, srcOut);
    lspServer.language[0] = '\0';
    params = make_completion_params(URI, 1, 15);
    items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Colonne", "fix/1883 : Colonne absente hors Champ Table");
    json_free(items);
    json_free(params);
}

/*  — `VOL_X est un <cursor>` dans le corps d'un Champ Onglet doit
 * proposer le type `Volet`/`Pane`. Volet est is_widget=false (sous-bloc d'un
 * Onglet, comme Colonne l'est pour Table) donc absent de la boucle
 * CONTROL_KINDS -> jamais offert. On l'offre en CONTEXTE Onglet (gate 'N').
 * Miroir de  (Colonne en contexte Table). Volet est MASCULIN ->
 * article de confort "un" en FR. Le negatif compte : hors d'un Onglet
 * (contexte Table, fenetre directe), Volet ne doit PAS etre propose. */
static void test_volet_type_completion_in_onglet(void) {
    printf("\n[TEST] fix/1887 : `VOL_X est un ` dans un Champ Onglet propose Volet/Pane\n");
    /* --- FR (defaut). Curseur apres `\t\tVOL_X est un ` (col 14). --- */
    const char* srcFR =
        "FEN_X est une Fenetre\n"
        "\tONG_X est un Champ Onglet\n"
        "\t\tVOL_X est un \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcFR);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 2, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Volet", "fix/1887 FR : Volet proposee dans corps Onglet");
    json_free(items);
    json_free(params);

    /* --- EN. Curseur apres `\t\tVOL_X is a ` (col 13). --- */
    const char* srcEN =
        "FEN_X est une Fenetre\n"
        "\tONG_X est un Champ Onglet\n"
        "\t\tVOL_X is a \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcEN);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 2, 13);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Pane",  "fix/1887 EN : Pane proposee dans corps Onglet");
    ASSERT_NOT_HAS(items, "Volet", "fix/1887 EN : Volet (FR) absente");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';

    /* --- Negatif : hors d'un Champ Onglet, Volet ne doit PAS etre propose. --- */
    const char* srcOut =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une \n"
        "FIN\n";
    setup_doc(URI, srcOut);
    lspServer.language[0] = '\0';
    params = make_completion_params(URI, 1, 15);
    items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Volet", "fix/1887 : Volet absente hors Champ Onglet");
    json_free(items);
    json_free(params);

    /* --- Non-regression  : en contexte Table, Colonne toujours
     * offerte ET Volet absent (le compilo refuserait Volet dans une Table). --- */
    const char* srcTable =
        "FEN_X est une Fenetre\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tCOL_X est une \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcTable);
    lspServer.language[0] = '\0';
    params = make_completion_params(URI, 2, 16);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Colonne", "fix/1887 : Colonne toujours offerte en contexte Table");
    ASSERT_NOT_HAS(items, "Volet",   "fix/1887 : Volet absente en contexte Table");
    json_free(items);
    json_free(params);
}

/*  (support #169) — en mode EN (réglages VSCode anglais), taper le
 * fermeur `end` : le match EXACT END doit être préselectionné (VSCode le focus
 * par défaut) ET trié avant ses homonymes préfixés (EndsWith/EndProgram), pour
 * qu'une validation directe insère END et pas EndsWith. Le popup auto de VSCode
 * applique son propre score fuzzy et ignorait le sortText serveur -> preselect. */
static void test_end_closer_preselect_en(void) {
    printf("\n[TEST] fix/1164 : `end` (mode EN) -> END preselect + trie avant EndsWith\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"x\"\n"
        "\t\tend\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* items = handle_completion(make_completion_params(URI, 3, 5));
    lspServer.language[0] = '\0';
    ASSERT_HAS(items, "END", "fix/1164");
    ASSERT_SORT_BEFORE(items, "END", "EndsWith", "fix/1164");
    if (item_preselect(items, "END")) {
        g_pass++; printf("  [OK]   fix/1164 : 'END' preselect=true\n");
    } else {
        g_fail++; printf("  [FAIL] fix/1164 : 'END' preselect absent/false\n");
    }
    json_free(items);
}

/*  (support #278) — en mode EN, taper `loop` proposait 6 natives
 * `Looper*` (alias EN de ZoneRepetee*, prefixe "Loop") AVANT le mot-cle LOOP.
 * Le tri serveur (sortText + preselect) est correct, mais VSCode en popup auto
 * applique un score fuzzy avec BONUS de casse exacte : `loop` (minuscules) vs
 * le filterText desaccentue canonique "LOOP" ne matche aucun char en casse
 * exacte, alors que "LooperAddLine" matche "oop" en casse exacte -> les Looper*
 * gardent le bonus et restent devant.
 * Fix : sur un match EXACT (rang 0), le filterText = fragment REELLEMENT TAPE
 * (casse preservee) -> VSCode voit un full-match casse-exacte (`loop` vs "loop").
 * NB proxy serveur : ce test verrouille que le filterText = fragment tape ; la
 * POSITION visuelle en tete cote VSCode est validee humainement (hors harnais). */
static void test_loop_keyword_exact_first_en(void) {
    printf("\n[TEST] fix/1738 : `loop` (mode EN) -> LOOP filterText = fragment tape + preselect\n");
    const char* src = "loop\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* items = handle_completion(make_completion_params(URI, 0, 4));
    lspServer.language[0] = '\0';
    ASSERT_HAS(items, "LOOP", "fix/1738");
    ASSERT_SORT_BEFORE(items, "LOOP", "LooperAddLine", "fix/1738");
    if (item_preselect(items, "LOOP")) {
        g_pass++; printf("  [OK]   fix/1738 : 'LOOP' preselect=true\n");
    } else {
        g_fail++; printf("  [FAIL] fix/1738 : 'LOOP' preselect absent/false\n");
    }
    {
        const char* _ft = item_filter_text(items, "LOOP");
        if (_ft && strcmp(_ft, "loop") == 0) {
            g_pass++; printf("  [OK]   fix/1738 : filterText('LOOP')=\"loop\" (fragment tape)\n");
        } else {
            g_fail++; printf("  [FAIL] fix/1738 : filterText('LOOP')=\"%s\" attendu \"loop\"\n",
                             _ft ? _ft : "(null)");
        }
    }
    json_free(items);
}

static void test_filter_already_used_props(void) {
    printf("\n[TEST] Filtre props deja utilisees\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tTitre = \"x\"\n"
        "\tLargeur = 100\n"
        "\t\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Titre",      "Filtre");
    ASSERT_NOT_HAS(items, "Largeur",    "Filtre");
    ASSERT_HAS    (items, "Hauteur",    "Filtre");
    ASSERT_HAS    (items, "Visible",    "Filtre");
    json_free(items);
    json_free(params);
}

static void test_dot_dot_fenetre(void) {
    printf("\n[TEST] FEN_X.. propose props Fenetre (pas Date)\n");
    const char* src =
        "FEN_Login est une Fenetre\n"
        "\tTitre = \"x\"\n"
        "FIN\n"
        "\n"
        "x = FEN_Login..\n";
    setup_doc(URI, src);
    /* Curseur juste apres le '..' ligne 4 (0-indexed). 'x = FEN_Login..' = 15 chars. */
    JsonValue* params = make_completion_params(URI, 4, 15);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Titre",      "FEN..");
    ASSERT_HAS    (items, "Largeur",    "FEN..");
    ASSERT_NOT_HAS(items, "Annee",      "FEN.. (regression Date)");
    ASSERT_NOT_HAS(items, "EnHeures",   "FEN.. (regression Duree)");
    json_free(items);
    json_free(params);
}

static void test_dot_dot_saisie(void) {
    printf("\n[TEST] SAI_X.. propose props Saisie (pas Date)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_User est une Saisie\n"
        "\t\tIndication = \"nom\"\n"
        "\tFIN\n"
        "\n"
        "\tx = SAI_User..\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 15);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Valeur",     "SAI..");
    ASSERT_HAS    (items, "Indication", "SAI..");
    ASSERT_NOT_HAS(items, "Annee",      "SAI.. (regression Date)");
    json_free(items);
    json_free(params);
}

static void test_dot_dot_bouton(void) {
    printf("\n[TEST] BTN_X.. propose props Bouton\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_OK est un Bouton\n"
        "\t\tLibelle = \"OK\"\n"
        "\tFIN\n"
        "\n"
        "\tx = BTN_OK..\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Libell\xc3\xa9",     "BTN..");
    ASSERT_HAS    (items, "TypeBouton",         "BTN..");
    ASSERT_NOT_HAS(items, "Annee",              "BTN.. (regression Date)");
    json_free(items);
    json_free(params);
}

static void test_dot_dot_libelle(void) {
    printf("\n[TEST] LBL_X.. propose props Libelle\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_T est un Libelle\n"
        "\t\tLibelle = \"X\"\n"
        "\tFIN\n"
        "\n"
        "\tx = LBL_T..\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Libell\xc3\xa9",         "LBL..");
    ASSERT_HAS    (items, "Couleur",                "LBL..");
    ASSERT_HAS    (items, "TypeLibell\xc3\xa9",     "LBL..");
    ASSERT_NOT_HAS(items, "Annee",                  "LBL.. (regression Date)");
    json_free(items);
    json_free(params);
}

/*  — `MoiMême..` / `MySelf..` propose le socle commun de proprietes
 * de champ (intersection stricte du catalogue DSL sur tous les widgets champ).
 * MoiMême est generique : dans une procedure agnostique le type reel du champ
 * n'est connu qu'au runtime, donc on ne resout pas le type contextuel et on
 * emet le socle. Les props du socle (X, Y, Largeur, Hauteur, Visible, Style...)
 * sont presentes ; une prop specifique a un seul kind (TypeBouton) ne doit PAS
 * l'etre (criterion "pas de faux positif"). */
static void test_dot_dot_moimeme(void) {
    printf("\n[TEST] feat/1221 : MoiMeme.. / MySelf.. propose le socle commun de champ\n");
    /* --- FR : `x = MoiMême..` (curseur apres les deux points, col UTF-16 13). --- */
    const char* srcFR = "x = MoiM\xc3\xaame..\n";
    setup_doc(URI, srcFR);
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);
    /* Geometrie / layout — presents sur tous les kinds. */
    ASSERT_HAS    (items, "Visible",   "MoiMeme.. socle Visible");
    ASSERT_HAS    (items, "X",         "MoiMeme.. socle X");
    ASSERT_HAS    (items, "Largeur",   "MoiMeme.. socle Largeur");
    ASSERT_HAS    (items, "Hauteur",   "MoiMeme.. socle Hauteur");
    ASSERT_HAS    (items, "Style",     "MoiMeme.. socle Style");
    /*  (arbitrage dev, seuil >= 2 kinds) : les props non universelles
     * mais usuelles entrent desormais dans le socle. */
    ASSERT_HAS    (items, "Libell\xc3\xa9", "MoiMeme.. socle Libellé (>= 2 kinds)");
    ASSERT_HAS    (items, "Valeur",    "MoiMeme.. socle Valeur (>= 2 kinds)");
    ASSERT_HAS    (items, "Couleur",   "MoiMeme.. socle Couleur (>= 2 kinds)");
    ASSERT_HAS    (items, "Etat",      "MoiMeme.. socle Etat (>= 2 kinds)");
    ASSERT_HAS    (items, "Gris\xc3\xa9", "MoiMeme.. socle Grisé (>= 2 kinds)");
    /*  — MoiMême = champ ∪ Fenêtre ∪ État : la complétion doit refléter
     * la validation. Titre est une prop de Fenetre : présente pour
     * MoiMême.. (union), absente pour MonChamp.. (champ seul, cf test suivant). */
    ASSERT_HAS    (items, "Titre",     "MoiMeme.. prop Fenetre Titre (union)");
    /* Exclusions : kind-specific pur + accesseur de type primitif. */
    ASSERT_NOT_HAS(items, "TypeBouton","MoiMeme.. pas de prop kind-specifique");
    ASSERT_NOT_HAS(items, "Annee",     "MoiMeme.. pas de prop Date");
    json_free(items);
    json_free(params);

    /* --- EN : `x = MySelf..` (alias bilingue, col 12). --- */
    const char* srcEN = "x = MySelf..\n";
    setup_doc(URI, srcEN);
    params = make_completion_params(URI, 0, 12);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Visible",   "MySelf.. socle Visible");
    ASSERT_HAS    (items, "Largeur",   "MySelf.. socle Largeur");
    ASSERT_HAS    (items, "Titre",     "MySelf.. prop Fenetre Titre (union)");
    ASSERT_NOT_HAS(items, "TypeBouton","MySelf.. pas de prop kind-specifique");
    json_free(items);
    json_free(params);
}

/*  — receveurs reflexifs figés :
 *  - `MonChamp..` / `MyControl..` = variante stricte de MoiMême → MEME socle
 *    commun de champ (Libellé/Valeur/Couleur/Visible présents, TypeBouton
 *    kind-specifique absent).
 *  - `MaFenêtre..` / `MyWindow..` = la fenetre courante → props de Fenetre
 *    (Titre présent, prop Date absente). */
static void test_dot_dot_monchamp_mafenetre(void) {
    printf("\n[TEST] feat/1220 : MonChamp../MaFenetre.. (+ EN) propose socle champ / props Fenetre\n");

    /* --- MonChamp.. (FR) : socle champ identique à MoiMême. --- */
    const char* srcChampFR = "x = MonChamp..\n";
    setup_doc(URI, srcChampFR);
    JsonValue* params = make_completion_params(URI, 0, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Visible",       "MonChamp.. socle Visible");
    ASSERT_HAS    (items, "Libell\xc3\xa9", "MonChamp.. socle Libellé");
    ASSERT_HAS    (items, "Valeur",        "MonChamp.. socle Valeur");
    ASSERT_HAS    (items, "Couleur",       "MonChamp.. socle Couleur");
    /*  — garde anti-régression : MonChamp reste champ SEUL, donc pas de
     * prop Fenetre-spécifique (Titre) contrairement à MoiMême.. (union). */
    ASSERT_NOT_HAS(items, "Titre",         "MonChamp.. pas de prop Fenetre (champ seul)");
    ASSERT_NOT_HAS(items, "TypeBouton",    "MonChamp.. pas de prop kind-specifique");
    json_free(items);
    json_free(params);

    /* --- MyControl.. (EN) : meme socle champ. --- */
    const char* srcChampEN = "x = MyControl..\n";
    setup_doc(URI, srcChampEN);
    params = make_completion_params(URI, 0, 15);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Visible",       "MyControl.. socle Visible");
    ASSERT_HAS    (items, "Libell\xc3\xa9", "MyControl.. socle Libellé");
    ASSERT_NOT_HAS(items, "Titre",         "MyControl.. pas de prop Fenetre (champ seul)");
    ASSERT_NOT_HAS(items, "TypeBouton",    "MyControl.. pas de prop kind-specifique");
    json_free(items);
    json_free(params);

    /* --- MaFenêtre.. (FR) : props de Fenetre (Titre) pas de champ pur. --- */
    const char* srcFenFR = "x = MaFen\xc3\xaatre..\n";
    setup_doc(URI, srcFenFR);
    params = make_completion_params(URI, 0, 15);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Titre",         "MaFenetre.. prop Fenetre Titre");
    ASSERT_HAS    (items, "Largeur",       "MaFenetre.. prop Fenetre Largeur");
    ASSERT_NOT_HAS(items, "Annee",         "MaFenetre.. pas de prop Date");
    json_free(items);
    json_free(params);

    /* --- MyWindow.. (EN) : props de Fenetre. --- */
    const char* srcFenEN = "x = MyWindow..\n";
    setup_doc(URI, srcFenEN);
    params = make_completion_params(URI, 0, 14);
    items = handle_completion(params);
    ASSERT_HAS    (items, "Titre",         "MyWindow.. prop Fenetre Titre");
    ASSERT_NOT_HAS(items, "Annee",         "MyWindow.. pas de prop Date");
    json_free(items);
    json_free(params);
}

/* Regression : `req est une httpRequête` puis `req..` doit proposer les
 * fields de httpRequête (URL, Methode, ContenuRequête, ...) PAS des trucs
 * sans rapport (Annee, EnHeures, valeurs de constantes/enum). */
static void test_dot_dot_httprequete(void) {
    printf("\n[TEST] req..httpRequête propose les fields HTTP (regression)\n");
    const char* src =
        "req est une httpR\xc3\xa9qu\xc3\xaate\n"   /* "httpRequête" UTF-8 */
        "x = req..\n";
    setup_doc(URI, src);
    /* Curseur juste apres '..' ligne 1, col 9 ('x = req..' = 9 chars). */
    JsonValue* params = make_completion_params(URI, 1, 9);
    JsonValue* items = handle_completion(params);
    /* Au moins un field HTTP attendu — n'importe lequel suffit. URL est le
     * field le plus universel d'une httpRequête. */
    ASSERT_HAS    (items, "URL",        "req..httpReq");
    ASSERT_NOT_HAS(items, "Annee",      "req..httpReq (regression : Date)");
    ASSERT_NOT_HAS(items, "EnHeures",   "req..httpReq (regression : Duree)");
    ASSERT_NOT_HAS(items, "owlEntier",  "req..httpReq (regression : enum value)");
    ASSERT_NOT_HAS(items, "champDebut", "req..httpReq (regression : sentinel)");
    json_free(items);
    json_free(params);
}

/* Gate fallback OWL_BUILTIN_PROP_CATALOG : un type registry non-primitif
 * sans fields (chaineMultilingue) ne doit PAS heriter des props des types
 * primitifs (Annee/EnJours/EnHeures de Date/Duree). Resultat correct =
 * liste vide. */
static void test_dot_dot_chaine_multilingue_no_primitive_props(void) {
    printf("\n[TEST] var..chaineMultilingue ne crache pas Annee/EnJours\n");
    const char* src =
        "var est une chaineMultilingue\n"
        "x = var..\n";
    setup_doc(URI, src);
    /* 'x = var..' = 9 chars, curseur juste apres '..'. */
    JsonValue* params = make_completion_params(URI, 1, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Annee",    "var..chaineML (Date)");
    ASSERT_NOT_HAS(items, "EnJours",  "var..chaineML (Duree)");
    ASSERT_NOT_HAS(items, "EnHeures", "var..chaineML (Duree)");
    json_free(items);
    json_free(params);
}

/* Non-regression : un type primitif (Date) conserve ses props via le
 * fallback OWL_BUILTIN_PROP_CATALOG. Annee/Mois/Jour doivent rester. */
static void test_dot_dot_date_keeps_props(void) {
    printf("\n[TEST] var..Date conserve Annee/Mois/Jour (regression)\n");
    const char* src =
        "var est une Date\n"
        "x = var..\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Ann\xc3\xa9" "e", "var..Date");  /* "Année" canon */
    ASSERT_HAS(items, "Mois",  "var..Date");
    ASSERT_HAS(items, "Jour",  "var..Date");
    json_free(items);
    json_free(params);
}

/* Non-regression : type primitif Duree garde EnJours/EnHeures. */
static void test_dot_dot_duree_keeps_props(void) {
    printf("\n[TEST] var..Duree conserve EnJours/EnHeures (regression)\n");
    const char* src =
        "var est une Dur\xc3\xa9" "e\n"   /* "Durée" UTF-8, accent */
        "x = var..\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "EnJours",  "var..Duree");
    ASSERT_HAS(items, "EnHeures", "var..Duree");
    json_free(items);
    json_free(params);
}

/*  — au 1er arg d'une native ouvre-fenetre (Ouvre/Open,
 * OuvreFille/OpenChild, OuvreSoeur/OpenSibling), la completion ne doit
 * proposer que les vraies Fenetres, pas les Etats (un Etat s'ouvre via
 * ImprimeEtat). Hors ce contexte, Fenetres ET Etats restent proposes. */
static void test_ouvre_filtre_etats(void) {
    printf("\n[TEST] Ouvre(/Open( 1er arg : Fenetres oui, Etats non\n");

    /* FR : Ouvre( au 1er arg. */
    const char* srcFR =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "r est une chaine = Ouvre(\n";
    setup_doc(URI, srcFR);
    JsonValue* params = make_completion_params(URI, 4, 25); /* apres '(' */
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible",    "Ouvre( FR");
    ASSERT_NOT_HAS(items, "ETAT_Rapport", "Ouvre( FR (Etat filtre)");
    /*  — la liste est fenetres-only : ni native ni mot-cle noyes. */
    ASSERT_NOT_HAS(items, "Trace",        "Ouvre( FR (native absente)");
    ASSERT_NOT_HAS(items, "Info",         "Ouvre( FR (native absente)");
    ASSERT_NOT_HAS(items, "SI",           "Ouvre( FR (mot-cle absent)");
    json_free(items);
    json_free(params);

    /* EN : Open( au 1er arg (resolu au canon Ouvre via catalog_find_native). */
    const char* srcEN =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "res est une chaine = Open(\n";
    setup_doc(URI, srcEN);
    params = make_completion_params(URI, 4, 26); /* apres '(' */
    items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible",    "Open( EN");
    ASSERT_NOT_HAS(items, "ETAT_Rapport", "Open( EN (Etat filtre)");
    json_free(items);
    json_free(params);

    /* OuvreSoeur( au 1er arg : meme filtrage. */
    const char* srcSoeur =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "OuvreSoeur(\n";
    setup_doc(URI, srcSoeur);
    params = make_completion_params(URI, 4, 11); /* apres '(' */
    items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible",    "OuvreSoeur(");
    ASSERT_NOT_HAS(items, "ETAT_Rapport", "OuvreSoeur( (Etat filtre)");
    json_free(items);
    json_free(params);

    /* OuvreFille( au 1er arg : meme filtrage. */
    const char* srcFille =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "OuvreFille(\n";
    setup_doc(URI, srcFille);
    params = make_completion_params(URI, 4, 11); /* apres '(' */
    items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible",    "OuvreFille(");
    ASSERT_NOT_HAS(items, "ETAT_Rapport", "OuvreFille( (Etat filtre)");
    json_free(items);
    json_free(params);

    /* OpenChild( (canon EN de OuvreFille) : meme filtrage. */
    const char* srcChild =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "OpenChild(\n";
    setup_doc(URI, srcChild);
    params = make_completion_params(URI, 4, 10); /* apres '(' */
    items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible",    "OpenChild(");
    ASSERT_NOT_HAS(items, "ETAT_Rapport", "OpenChild( (Etat filtre)");
    json_free(items);
    json_free(params);

    /* OpenSibling( (canon EN de OuvreSoeur) : meme filtrage. */
    const char* srcSibling =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "OpenSibling(\n";
    setup_doc(URI, srcSibling);
    params = make_completion_params(URI, 4, 12); /* apres '(' */
    items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible",    "OpenSibling(");
    ASSERT_NOT_HAS(items, "ETAT_Rapport", "OpenSibling( (Etat filtre)");
    json_free(items);
    json_free(params);

    /* Non-regression : hors contexte ouvre-fenetre, l'Etat reste propose. */
    const char* srcHors =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "ETAT_Rapport est un Etat\n"
        "FIN\n"
        "x = \n";
    setup_doc(URI, srcHors);
    params = make_completion_params(URI, 4, 4);
    items = handle_completion(params);
    ASSERT_HAS(items, "FEN_Cible",    "hors-Ouvre (Fenetre)");
    ASSERT_HAS(items, "ETAT_Rapport", "hors-Ouvre (Etat propose)");
    json_free(items);
    json_free(params);
}

/*  — auto-popup sur le caractere '(' (context.triggerKind=2,
 * triggerCharacter="("). Contexte special ouvre-fenetre => fenetres-only.
 * Contexte generique (native quelconque) => liste vide (pas de popup
 * bruyante ; le signatureHelp s'affiche seul). Ctrl-Space (Invoked) garde
 * la liste complete — verifie par les autres tests qui n'envoient pas de
 * contexte. */
static void test_paren_trigger_completion(void) {
    printf("\n[TEST] fix/1279 auto-popup '(' : Ouvre=fenetres, native=vide\n");

    /* (a) Ouvre( declenche par '(' : fenetres uniquement, natives absentes. */
    const char* srcOuvre =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "r est une chaine = Ouvre(\n";
    setup_doc(URI, srcOuvre);
    JsonValue* params = make_completion_params_ctx(URI, 2, 25, 2, "(");
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Cible", "'(' trigger Ouvre (Fenetre)");
    ASSERT_NOT_HAS(items, "Trace",     "'(' trigger Ouvre (native absente)");
    ASSERT_NOT_HAS(items, "Info",      "'(' trigger Ouvre (native absente)");
    json_free(items);
    json_free(params);

    /* (b) Info( declenche par '(' : native generique -> liste vide. */
    const char* srcInfo =
        "FEN_Cible est une Fenetre\n"
        "FIN\n"
        "Info(\n";
    setup_doc(URI, srcInfo);
    params = make_completion_params_ctx(URI, 2, 5, 2, "(");
    items = handle_completion(params);
    if (count_items(items) == 0) {
        g_pass++;
        printf("  [OK]   '(' trigger Info : liste vide (pas de popup bruyante)\n");
    } else {
        g_fail++;
        printf("  [FAIL] '(' trigger Info : attendu liste vide, obtenu %d items\n",
               count_items(items));
    }
    json_free(items);
    json_free(params);

    /* (c) Non-regression : Ctrl-Space (Invoked, triggerKind=1) sur Info(
     * garde la liste generale complete (natives presentes). */
    setup_doc(URI, srcInfo);
    params = make_completion_params_ctx(URI, 2, 5, 1, NULL);
    items = handle_completion(params);
    ASSERT_HAS(items, "Trace", "Ctrl-Space Info( (liste generale complete)");
    json_free(items);
    json_free(params);
}

/*  — '(' doit figurer dans completionProvider.triggerCharacters
 * (a cote de '.' et ':') pour que l'auto-popup se declenche. */
static void test_init_completion_trigger_paren(void) {
    printf("\n[TEST] fix/1279 initialize : '(' dans completion triggerCharacters\n");
    JsonValue* params = json_new_object();
    JsonValue* result = handle_initialize(params);
    JsonValue* caps = result ? json_get(result, "capabilities") : NULL;
    JsonValue* cp   = caps ? json_get(caps, "completionProvider") : NULL;
    JsonValue* tc   = cp ? json_get(cp, "triggerCharacters") : NULL;
    bool hasParen = false;
    if (tc && tc->type == JSON_ARRAY) {
        for (int i = 0; i < tc->as.arrayVal.count; i++) {
            JsonValue* v = tc->as.arrayVal.items[i];
            if (v && v->type == JSON_STRING && v->as.stringVal.str
                && strcmp(v->as.stringVal.str, "(") == 0)
                hasParen = true;
        }
    }
    if (hasParen) {
        g_pass++;
        printf("  [OK]   fix/1279 : '(' present dans completion triggerCharacters\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1279 : '(' ABSENT des completion triggerCharacters\n");
    }
    if (result) json_free(result);
    json_free(params);
}

static void test_names_in_top_level(void) {
    printf("\n[TEST] Noms de Fenetre/Champs proposes en top-level\n");
    const char* src =
        "FEN_Login est une Fenetre\n"
        "\tSAI_User est une Saisie\n"
        "\tFIN\n"
        "\tBTN_OK est un Bouton\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "x = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 7, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "FEN_Login",  "Top-level");
    ASSERT_HAS(items, "SAI_User",   "Top-level");
    ASSERT_HAS(items, "BTN_OK",     "Top-level");
    json_free(items);
    json_free(params);
}

static void test_constante_block_completion(void) {
    printf("\n[TEST] Constantes du bloc CONSTANTE proposees en autocomplete\n");
    const char* src =
        "CONSTANTE\n"
        "\tTauxTVA = 20\n"
        "\tNomApp = \"Mon Appli\"\n"
        "\tPi_App = 3.14\n"
        "FIN\n"
        "\n"
        "x = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "TauxTVA", "Constante");
    ASSERT_HAS(items, "NomApp",  "Constante");
    ASSERT_HAS(items, "Pi_App",  "Constante");
    json_free(items);
    json_free(params);
}

static void test_couleur_enum_exists(void) {
    printf("\n[TEST] Enum Couleur existe et liste les couleurs canon\n");
    const OwlEnumInfo* e = owl_enum_info_find("Couleur");
    if (!e) {
        g_fail++;
        printf("  [FAIL] Couleur : enum 'Couleur' introuvable dans OWL_ENUM_CATALOG\n");
        return;
    }
    g_pass++;
    printf("  [OK]   Couleur : enum 'Couleur' present (%d valeurs)\n", e->valueCount);

    /* Check quelques constantes canon WL en membres. */
    const char* expected[] = {
        "RougeClair", "BleuFonc\xc3\xa9", "Blanc", "Noir",
        "Transparent", "JauneOr", NULL
    };
    for (int i = 0; expected[i]; i++) {
        if (owl_enum_info_contains(e, expected[i])) {
            g_pass++;
            printf("  [OK]   Couleur contient '%s'\n", expected[i]);
        } else {
            g_fail++;
            printf("  [FAIL] Couleur : '%s' MANQUANT dans les valeurs d'enum\n", expected[i]);
        }
    }
}

static void test_type_alias_en_resolves(void) {
    printf("\n[TEST] owl_type_info_find resout l'alias EN d'un type compose (Font -> Police)\n");

    /* Repro  : l'alias EN d'un type builtin doit resoudre l'entree
     * canon, sinon la recoloration semantique en position prop retombe sur
     * la couleur par defaut. */
    const OwlTypeInfo* byEN = owl_type_info_find("Font");
    if (byEN && strcmp(byEN->name, "Police") == 0) {
        g_pass++;
        printf("  [OK]   Font resout l'entree 'Police'\n");
    } else {
        g_fail++;
        printf("  [FAIL] Font : owl_type_info_find(\"Font\") = %s (attendu 'Police')\n",
               byEN ? byEN->name : "NULL");
    }

    /* Non-regression : le nom FR canon doit toujours resoudre. */
    const OwlTypeInfo* byFR = owl_type_info_find("Police");
    if (byFR && strcmp(byFR->name, "Police") == 0) {
        g_pass++;
        printf("  [OK]   Police resout toujours l'entree 'Police'\n");
    } else {
        g_fail++;
        printf("  [FAIL] Police : owl_type_info_find(\"Police\") = %s (attendu 'Police')\n",
               byFR ? byFR->name : "NULL");
    }
}

static void test_enum_alias_en_resolves(void) {
    printf("\n[TEST] owl_enum_info_find resout l'alias EN d'un enum (stringCompare -> chaineCompare)\n");

    /* Miroir de  sur owl_type_info_find : l'alias EN d'un enum
     * doit resoudre l'entree canon, sinon un enum ecrit sous sa forme EN
     * perd son traitement LSP (hover / completion filtree / coloration). */
    const OwlEnumInfo* byEN = owl_enum_info_find("stringCompare");
    if (byEN && strcmp(byEN->name, "cha\xc3\xaeneCompare") == 0) {
        g_pass++;
        printf("  [OK]   stringCompare resout l'entree 'chaineCompare'\n");
    } else {
        g_fail++;
        printf("  [FAIL] stringCompare : owl_enum_info_find(\"stringCompare\") = %s (attendu 'chaineCompare')\n",
               byEN ? byEN->name : "NULL");
    }

    /* Non-regression : le nom FR ascii doit toujours resoudre. */
    const OwlEnumInfo* byFR = owl_enum_info_find("chaineCompare");
    if (byFR && strcmp(byFR->name, "cha\xc3\xaeneCompare") == 0) {
        g_pass++;
        printf("  [OK]   chaineCompare resout toujours l'entree 'chaineCompare'\n");
    } else {
        g_fail++;
        printf("  [FAIL] chaineCompare : owl_enum_info_find(\"chaineCompare\") = %s (attendu 'chaineCompare')\n",
               byFR ? byFR->name : "NULL");
    }
}

static void test_couleur_completion_in_assignment(void) {
    printf("\n[TEST] Assignation 'c est une Couleur = ' propose les couleurs\n");
    const char* src = "c est une Couleur = \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 20);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "RougeClair", "Assign Couleur");
    ASSERT_HAS(items, "BleuFonc\xc3\xa9", "Assign Couleur");
    ASSERT_HAS(items, "Blanc",      "Assign Couleur");
    ASSERT_HAS(items, "Transparent","Assign Couleur");
    json_free(items);
    json_free(params);
}

static void test_proprietes_block_strict_completion(void) {
    printf("\n[TEST] DSL v2 : bloc PROPRIETES propose UNIQUEMENT les props\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    /* Doit proposer les props de Fenetre. */
    ASSERT_HAS    (items, "Titre",     "PROPRIETES Fenetre");
    ASSERT_HAS    (items, "Largeur",   "PROPRIETES Fenetre");
    /* NE doit PAS proposer les fonctions natives ni autre. */
    ASSERT_NOT_HAS(items, "Trace",     "PROPRIETES Fenetre (pas de native)");
    ASSERT_NOT_HAS(items, "Ferme",     "PROPRIETES Fenetre (pas de native)");
    ASSERT_NOT_HAS(items, "Ouvre",     "PROPRIETES Fenetre (pas de native)");
    ASSERT_NOT_HAS(items, "RougeClair","PROPRIETES Fenetre (pas de constante)");
    json_free(items);
    json_free(params);
}

static void test_proprietes_block_strict_completion_en(void) {
    /*  — meme contrat que la version FR mais fenetre 100% EN
     * (`FEN_X is a Window` + `PROPERTIES`). En EN l'article `a` s'intercale
     * entre `is` et `Window` ; sans transparence de l'article, le
     * classifieur de contexte n'empile pas le widget englobant et la
     * completion des proprietes meurt (plainte dev : completion KO dans
     * une fenetre EN). */
    printf("\n[TEST] fix/0688 : bloc PROPERTIES (fenetre EN) propose les props\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROPERTIES\n"
        "\t\t\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    /* Doit proposer les props de Fenetre (contexte widget detecte malgre
     * l'article EN). */
    ASSERT_HAS    (items, "Titre",     "PROPERTIES Window EN");
    ASSERT_HAS    (items, "Largeur",   "PROPERTIES Window EN");
    /* NE doit PAS proposer les fonctions natives (contexte strict). */
    ASSERT_NOT_HAS(items, "Trace",     "PROPERTIES Window EN (pas de native)");
    ASSERT_NOT_HAS(items, "Ouvre",     "PROPERTIES Window EN (pas de native)");
    json_free(items);
    json_free(params);
}

/*  — modifier EN `Control` (nameEN canon de Champ) : `IDENT is a
 * Control Button` doit empiler le widget englobant comme `Champ`/`Field`.
 * Sans le swallow de `Control` dans detect_dsl_context_ex, le vrai kind
 * (Button) n'etait jamais empile -> completion des props muette (ou pire,
 * desync de profondeur qui vide la pile). Meme famille que /0775. */
static void test_proprietes_block_completion_control_button_en(void) {
    printf("\n[TEST] fix/1074 : bloc PROPERTIES sous 'Control Button' (EN) propose les props bouton\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"X\"\n"
        "\tEND\n"
        "\tBTN_X is a Control Button\n"
        "\t\tPROPERTIES\n"
        "\t\t\t\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Ligne 6 (0-indexed), char 3 = apres les 3 tabs de la ligne vide. */
    JsonValue* params = make_completion_params(URI, 6, 3);
    JsonValue* items = handle_completion(params);
    /* ButtonType est button-specific : present seulement si le contexte
     * resout bien vers Button (pas Window, pas muet). */
    ASSERT_HAS    (items, "ButtonType", "Control Button EN (prop button-specific)");
    ASSERT_HAS    (items, "Caption",    "Control Button EN");
    ASSERT_NOT_HAS(items, "Placeholder","Control Button EN (pas une prop Input)");
    ASSERT_NOT_HAS(items, "Trace",      "Control Button EN (pas de native)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  — variante Saisie : `IDENT is a Control Input` (le cas exact du
 * ticket support #142). Meme contrat, props Input-specific. */
static void test_proprietes_block_completion_control_input_en(void) {
    printf("\n[TEST] fix/1074 : bloc PROPERTIES sous 'Control Input' (EN) propose les props saisie\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tSAI_X is a Control Input\n"
        "\t\tPROPERTIES\n"
        "\t\t\t\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Ligne 3 (0-indexed), char 3. */
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Placeholder", "Control Input EN (prop input-specific)");
    ASSERT_HAS    (items, "InputType",   "Control Input EN");
    ASSERT_NOT_HAS(items, "ButtonType",  "Control Input EN (pas une prop Button)");
    ASSERT_NOT_HAS(items, "Trace",       "Control Input EN (pas de native)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

static void test_couleur_completion_in_label_prop(void) {
    printf("\n[TEST] 'Couleur = ' dans body Libelle propose les couleurs\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_T est un Libelle\n"
        "\t\tLibelle = \"X\"\n"
        "\t\tCouleur = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "RougeClair", "LBL Couleur=");
    ASSERT_HAS(items, "BleuFonc\xc3\xa9", "LBL Couleur=");
    ASSERT_HAS(items, "Transparent","LBL Couleur=");
    json_free(items);
    json_free(params);
}

/*  — `CouleurFond = ` (prop COLOR bare, sans collision de type)
 * dans le body d'un widget DSL. Avant le fix : la detection de contexte
 * ne reconnaissait que `var..Field =` ou `var =` (var connu) ; une prop
 * bare COLOR sans nom de type homonyme retombait sur la completion des
 * NOMS de props (AlignH/Marge/...). On verifie qu'on propose desormais
 * les constantes couleur, et PAS la liste de props. */
static void test_couleurfond_completion_in_label_prop(void) {
    printf("\n[TEST] fix/0726 : 'CouleurFond = ' dans body Libelle propose les couleurs\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_T est un Libelle\n"
        "\t\tLibelle = \"X\"\n"
        "\t\tCouleurFond = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "RougeClair",      "LBL CouleurFond=");
    ASSERT_HAS(items, "BleuFonc\xc3\xa9", "LBL CouleurFond=");
    ASSERT_HAS(items, "Transparent",     "LBL CouleurFond=");
    /* NE doit PAS proposer la liste de noms de props. */
    ASSERT_NOT_HAS(items, "Marge",       "LBL CouleurFond= (pas de nom de prop)");
    ASSERT_NOT_HAS(items, "ColSpan",     "LBL CouleurFond= (pas de nom de prop)");
    ASSERT_NOT_HAS(items, "Alignement",  "LBL CouleurFond= (pas de nom de prop)");
    json_free(items);
    json_free(params);
}

/*  — variante EN : `BackgroundColor = ` (forme EN de CouleurFond).
 * Meme contrat que la version FR. */
static void test_backgroundcolor_completion_in_label_prop_en(void) {
    printf("\n[TEST] fix/0726 : 'BackgroundColor = ' (EN) dans body Label propose les couleurs\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tLBL_T is a Label\n"
        "\t\tCaption = \"X\"\n"
        "\t\tBackgroundColor = \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 3, 20);
    JsonValue* items = handle_completion(params);
    /* En mode EN les labels couleurs basculent sur leur nameEN. */
    ASSERT_HAS(items, "DarkBlue",        "Label BackgroundColor= EN");
    ASSERT_HAS(items, "Transparent",     "Label BackgroundColor= EN");
    ASSERT_NOT_HAS(items, "Margin",      "Label BackgroundColor= EN (pas de nom de prop)");
    ASSERT_NOT_HAS(items, "ColumnSpan",  "Label BackgroundColor= EN (pas de nom de prop)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';  /* reset defaut fr */
}

/*  — non-regression : une prop ENUM bare (`Alignement = `) dans
 * le body d'un widget propose toujours les valeurs de son enum. */
static void test_enum_prop_bare_completion_in_label(void) {
    printf("\n[TEST] fix/0726 : 'Alignement = ' (enum bare) propose les valeurs d'enum\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_T est un Libelle\n"
        "\t\tAlignement = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 15);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "alignCentre", "LBL Alignement= (valeur enum)");
    ASSERT_HAS(items, "alignFin",    "LBL Alignement= (valeur enum)");
    ASSERT_NOT_HAS(items, "Marge",   "LBL Alignement= (pas de nom de prop)");
    json_free(items);
    json_free(params);
}

/*  — non-regression : la completion des NOMS de props (curseur
 * PAS apres `=`) doit toujours fonctionner dans le body du widget. */
static void test_prop_name_completion_unaffected(void) {
    printf("\n[TEST] fix/0726 : completion des noms de props intacte (pas apres '=')\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_T est un Libelle\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "CouleurFond", "LBL noms de props");
    ASSERT_HAS(items, "Alignement",  "LBL noms de props");
    json_free(items);
    json_free(params);
}

/*  — non-regression PROPRIETES strict : la completion des NOMS de
 * props dans un bloc PROPRIETES reste bornee aux props (pas de constante
 * couleur qui fuiterait via mon ajout). */
static void test_prop_name_completion_strict_in_proprietes(void) {
    printf("\n[TEST] fix/0726 : noms de props stricts dans PROPRIETES (pas de couleur)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tLBL_T est un Libelle\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "CouleurFond",     "LBL PROPRIETES noms");
    ASSERT_NOT_HAS(items, "RougeClair",  "LBL PROPRIETES noms (pas de couleur)");
    json_free(items);
    json_free(params);
}

/*  (support #376) — `MargeBas = <cursor>` (prop widget a VALEUR
 * LIBRE, entier, sans jeu de valeurs enum), props posees DIRECTEMENT
 * dans le body du widget (sans bloc PROPRIETES, forme utilisee par
 * ). Avant le fix : le LHS "MargeBas" n'est ni variable, ni
 * type, ni enum connu -> aucun contexte de valeur ne s'arme
 * (propValueContext / simpleAssignContext / inDeclAssignValue tous
 * faux) -> la completion retombe sur le chemin GENERAL complet (mots-
 * cles, natives, noms de types, constantes...) qui n'a rien a voir avec
 * une valeur de MargeBas. C'est le coeur du bug remonte par le support :
 * "des qu'il n'y a PAS de valeur a fournir dans la liste, on obtient
 * n'importe quoi" — verifie empiriquement (2277 items retournes, dont
 * SI/Trace/entier) avant impl. Le fix peut renvoyer une liste vide ou
 * au plus un hint de type, mais jamais ce dump general. */
static void test_margebas_completion_bare_prop_no_garbage(void) {
    printf("\n[TEST] fix/2162 : 'MargeBas = ' (valeur libre, Bouton, sans PROPRIETES) ne dumpe pas le catalogue general\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_X est un Bouton\n"
        "\t\tLibelle = \"Valider\"\n"
        "\t\tMargeBas = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 3 (0-indexed), 2 tabs + "MargeBas = " (11 chars) = 13. */
    JsonValue* params = make_completion_params(URI, 3, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "SI",     "2162 MargeBas= : mot-cle SI absent (pas de dump general)");
    ASSERT_NOT_HAS(items, "ALORS",  "2162 MargeBas= : mot-cle ALORS absent (pas de dump general)");
    ASSERT_NOT_HAS(items, "Trace",  "2162 MargeBas= : native Trace absente (pas de dump general)");
    ASSERT_NOT_HAS(items, "entier", "2162 MargeBas= : nom de type entier absent (pas de dump general)");
    json_free(items);
    json_free(params);
}

/*  (support #376) — meme prop (MargeBas), mais cette fois DANS
 * un bloc PROPRIETES explicite. Verifie empiriquement : ce chemin ne
 * dumpe PAS le catalogue general (les mots-cles/natives sont deja
 * absents), mais dumpe a la place les 38 AUTRES noms de proprietes du
 * widget (TypeBouton, CouleurFond, Largeur...) comme si c'etaient des
 * valeurs possibles pour MargeBas — ce qui "n'a rien a voir" avec la
 * valeur attendue (un entier), exactement la formulation du declarant.
 * Flaveur distincte du bug bare (garde de la PROPRIETES-strict path qui
 * ignore le contexte valeur), mais meme famille : cursor apres '=' doit
 * cesser de renvoyer une liste sans rapport. */
static void test_margebas_completion_in_proprietes_block_no_sibling_names(void) {
    printf("\n[TEST] fix/2162 : 'MargeBas = ' (valeur libre, bloc PROPRIETES) ne propose pas les AUTRES noms de props\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_X est un Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Valider\"\n"
        "\t\t\tMargeBas = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 4 (0-indexed), 3 tabs + "MargeBas = " (11 chars) = 14. */
    JsonValue* params = make_completion_params(URI, 4, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "TypeBouton",  "2162 PROPRIETES MargeBas= : pas d'autre nom de prop (TypeBouton)");
    ASSERT_NOT_HAS(items, "CouleurFond", "2162 PROPRIETES MargeBas= : pas d'autre nom de prop (CouleurFond)");
    ASSERT_NOT_HAS(items, "Largeur",     "2162 PROPRIETES MargeBas= : pas d'autre nom de prop (Largeur)");
    json_free(items);
    json_free(params);
}

/*  (support #376) — variante EN, exactement les 3 proprietes
 * citees par le declarant : BottomMargin, Caption, ColumnSpan. Props
 * posees directement dans le body du widget (forme bare, meme famille
 * que le cas FR ci-dessus). Caption est une valeur libre TEXTE (chaine
 * multilangue), ColumnSpan et BottomMargin des valeurs libres ENTIER :
 * aucune n'a de jeu de valeurs enum, donc les trois doivent echapper au
 * dump general. */
static void test_bottommargin_caption_columnspan_no_garbage_en(void) {
    printf("\n[TEST] fix/2162 : BottomMargin/Caption/ColumnSpan (EN) sur Button ne dumpent pas le catalogue general\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* BottomMargin = <cursor> */
    const char* src1 =
        "FEN_X is a Window\n"
        "\tBTN_X is a Control Button\n"
        "\t\tCaption = \"OK\"\n"
        "\t\tBottomMargin = \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src1);
    /* Ligne 3, 2 tabs + "BottomMargin = " (15 chars) = 17. */
    JsonValue* p1 = make_completion_params(URI, 3, 17);
    JsonValue* i1 = handle_completion(p1);
    ASSERT_NOT_HAS(i1, "IF",      "2162 BottomMargin= EN : mot-cle IF absent");
    ASSERT_NOT_HAS(i1, "Trace",   "2162 BottomMargin= EN : native Trace absente");
    ASSERT_NOT_HAS(i1, "integer", "2162 BottomMargin= EN : nom de type integer absent");
    json_free(i1); json_free(p1);

    /* Caption = <cursor> */
    const char* src2 =
        "FEN_X is a Window\n"
        "\tBTN_X is a Control Button\n"
        "\t\tCaption = \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src2);
    /* Ligne 2, 2 tabs + "Caption = " (10 chars) = 12. */
    JsonValue* p2 = make_completion_params(URI, 2, 12);
    JsonValue* i2 = handle_completion(p2);
    ASSERT_NOT_HAS(i2, "IF",    "2162 Caption= EN : mot-cle IF absent");
    ASSERT_NOT_HAS(i2, "Trace", "2162 Caption= EN : native Trace absente");
    ASSERT_NOT_HAS(i2, "string","2162 Caption= EN : nom de type string absent");
    json_free(i2); json_free(p2);

    /* ColumnSpan = <cursor> */
    const char* src3 =
        "FEN_X is a Window\n"
        "\tBTN_X is a Control Button\n"
        "\t\tCaption = \"OK\"\n"
        "\t\tColumnSpan = \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src3);
    /* Ligne 3, 2 tabs + "ColumnSpan = " (13 chars) = 15. */
    JsonValue* p3 = make_completion_params(URI, 3, 15);
    JsonValue* i3 = handle_completion(p3);
    ASSERT_NOT_HAS(i3, "IF",      "2162 ColumnSpan= EN : mot-cle IF absent");
    ASSERT_NOT_HAS(i3, "Trace",   "2162 ColumnSpan= EN : native Trace absente");
    ASSERT_NOT_HAS(i3, "integer", "2162 ColumnSpan= EN : nom de type integer absent");
    json_free(i3); json_free(p3);

    lspServer.language[0] = '\0';  /* reset defaut fr */
}

/*  — non-regression : `TypeSaisie = <cursor>` (prop widget a
 * ENUM) dans un bloc PROPRIETES de Saisie doit CONTINUER de proposer les
 * valeurs de l'enum. Un fix qui sur-supprime la detection de contexte
 * casserait ce cas — c'est le garde-fou du critere 2 du ticket.
 * VERT des le depart (verifie empiriquement) : le walk-back arme
 * simpleAssignContext des qu'il reconnait un enum connu, independamment
 * du bloc PROPRIETES ; ce test verrouille ce comportement existant. */
static void test_typesaisie_enum_bare_prop_completion_non_regression(void) {
    printf("\n[TEST] fix/2162 : 'TypeSaisie = ' (prop enum, Saisie) propose toujours les valeurs (VERT — mecanisme existant)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tTypeSaisie = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 3, 3 tabs + "TypeSaisie = " (13 chars) = 16. */
    JsonValue* params = make_completion_params(URI, 3, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "typSaisieTexte", "2162 TypeSaisie= : valeur typSaisieTexte proposee");
    ASSERT_HAS    (items, "typSaisieDate",  "2162 TypeSaisie= : valeur typSaisieDate proposee");
    ASSERT_NOT_HAS(items, "Trace",          "2162 TypeSaisie= : pas de dump general (Trace absente)");
    ASSERT_NOT_HAS(items, "SI",             "2162 TypeSaisie= : pas de dump general (SI absent)");
    json_free(items);
    json_free(params);
}

/*  — non-regression : completion du NOM de propriete (curseur
 * AVANT le `=`, ligne vide du bloc PROPRIETES) inchangee. MargeBas et
 * TypeBouton (valeur libre et enum) doivent toutes les deux rester
 * proposees comme NOMS, seul le comportement APRES `=` est en cause.
 * Note : le canon FR du libelle est "Libellé" (avec accent) — "Libelle"
 * sans accent n'est que l'alias ASCII de saisie, jamais le label emis. */
static void test_prop_name_completion_before_equals_unaffected_2162(void) {
    printf("\n[TEST] fix/2162 : noms de props (avant '=') inchanges dans PROPRIETES Bouton\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_X est un Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "MargeBas",           "2162 noms props Bouton : MargeBas (valeur libre)");
    ASSERT_HAS(items, "TypeBouton",         "2162 noms props Bouton : TypeBouton (enum)");
    ASSERT_HAS(items, "Libell\xc3\xa9",     "2162 noms props Bouton : Libell\xc3\xa9 (canon accentue)");
    json_free(items);
    json_free(params);
}

/*  (support #313) — FormatAffichage (Texte/RTF) sur un Champ Saisie
 * n'a d'effet qu'en saisie multi-ligne. Le designer masque deja l'option hors
 * typSaisieMultiligne ; l'editeur de code (completion de VALEUR) doit miroiter
 * cette condition : la valeur RTF (formatTexteRiche / forme EN formatRichText)
 * ne doit etre proposee QUE si la prop soeur TypeSaisie vaut typSaisieMultiligne.
 * formatTexte (texte simple) reste propose partout. Condition sibling-aware
 * mirroir exact de ui_presentation.c (visible_if TypeSaisie=typSaisieMultiligne). */
static void test_formataffichage_richtext_hidden_non_multiligne_2282(void) {
    printf("\n[TEST] fix/2282 : FormatAffichage = ... masque formatTexteRiche hors saisie multi-ligne\n");

    /* (1) Champ mot de passe : RTF ABSENT, texte simple present. */
    const char* srcPass =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tTypeSaisie = typSaisieMotDePasse\n"
        "\t\t\tFormatAffichage = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcPass);
    /* Ligne 4, 3 tabs + "FormatAffichage = " (18 chars) = 21. */
    JsonValue* pPass = make_completion_params(URI, 4, 21);
    JsonValue* iPass = handle_completion(pPass);
    ASSERT_NOT_HAS(iPass, "formatTexteRiche", "2282 password : RTF (formatTexteRiche) masque");
    ASSERT_HAS    (iPass, "formatTexte",      "2282 password : texte simple (formatTexte) propose");
    json_free(iPass);
    json_free(pPass);

    /* (2) Champ multi-ligne : RTF PRESENT, texte simple present. */
    const char* srcMulti =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tTypeSaisie = typSaisieMultiligne\n"
        "\t\t\tFormatAffichage = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcMulti);
    JsonValue* pMulti = make_completion_params(URI, 4, 21);
    JsonValue* iMulti = handle_completion(pMulti);
    ASSERT_HAS(iMulti, "formatTexteRiche", "2282 multi-ligne : RTF (formatTexteRiche) propose");
    ASSERT_HAS(iMulti, "formatTexte",      "2282 multi-ligne : texte simple (formatTexte) propose");
    json_free(iMulti);
    json_free(pMulti);

    /* (3) Defaut implicite (pas de TypeSaisie = typSaisieTexte) : RTF ABSENT. */
    const char* srcDefaut =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tFormatAffichage = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, srcDefaut);
    /* Ligne 3, 3 tabs + "FormatAffichage = " = 21. */
    JsonValue* pDef = make_completion_params(URI, 3, 21);
    JsonValue* iDef = handle_completion(pDef);
    ASSERT_NOT_HAS(iDef, "formatTexteRiche", "2282 defaut (texte) : RTF masque sans TypeSaisie explicite");
    ASSERT_HAS    (iDef, "formatTexte",      "2282 defaut (texte) : texte simple propose");
    json_free(iDef);
    json_free(pDef);

    /* (4) Mode EN : le label RTF (formatRichText) masque sur mot de passe. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    setup_doc(URI, srcPass);
    JsonValue* pEN = make_completion_params(URI, 4, 21);
    JsonValue* iEN = handle_completion(pEN);
    ASSERT_NOT_HAS(iEN, "formatRichText", "2282 EN password : RTF (formatRichText) masque");
    ASSERT_HAS    (iEN, "formatText",     "2282 EN password : texte simple (formatText) propose");
    json_free(iEN);
    json_free(pEN);
    lspServer.language[0] = '\0';
}

/*  (support #378) — ColSpan/RowSpan n'ont de sens que si le parent
 * (fenetre ou conteneur) est en Disposition = dispGrille. Sans Disposition
 * explicite (defaut dispVerticale), la completion des NOMS de props dans
 * le bloc PROPRIETES du champ ne doit PAS proposer ColSpan/RowSpan — ils
 * seraient ignores en silence au rendu. Les autres noms de props (valeur
 * libre ou enum) restent proposes (non-regression). */
static void test_columnspan_rowspan_hidden_default_layout_2163(void) {
    printf("\n[TEST] fix/2163 : ColSpan/RowSpan absents de la completion hors dispGrille (defaut)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_X est un Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "ColSpan",  "2163 defaut (dispVerticale) : ColSpan absent (sans effet hors grille)");
    ASSERT_NOT_HAS(items, "RowSpan",  "2163 defaut (dispVerticale) : RowSpan absent (sans effet hors grille)");
    ASSERT_HAS(items, "MargeBas",     "2163 defaut : MargeBas toujours propose (non-regression)");
    ASSERT_HAS(items, "TypeBouton",   "2163 defaut : TypeBouton toujours propose (non-regression)");
    json_free(items);
    json_free(params);
}

/*  — meme regle sous une fenetre explicitement en dispLibre : le
 * positionnement libre n'est pas davantage compatible avec ColSpan/RowSpan
 * que le defaut dispVerticale. */
static void test_columnspan_rowspan_hidden_disp_libre_2163(void) {
    printf("\n[TEST] fix/2163 : ColSpan/RowSpan absents de la completion en dispLibre\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispLibre\n"
        "\tFIN\n"
        "\tBTN_X est un Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "ColSpan",  "2163 dispLibre : ColSpan absent (sans effet hors grille)");
    ASSERT_NOT_HAS(items, "RowSpan",  "2163 dispLibre : RowSpan absent (sans effet hors grille)");
    ASSERT_HAS(items, "MargeBas",     "2163 dispLibre : MargeBas toujours propose (non-regression)");
    json_free(items);
    json_free(params);
}

/*  — sous un parent en Disposition = dispGrille, ColSpan/RowSpan
 * redeviennent un usage legitime : la completion doit continuer a les
 * proposer (non-regression du cas d'usage correct). */
static void test_columnspan_rowspan_offered_disp_grille_2163(void) {
    printf("\n[TEST] fix/2163 : ColSpan/RowSpan proposes en completion sous dispGrille\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispGrille\n"
        "\tFIN\n"
        "\tBTN_X est un Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "ColSpan",  "2163 dispGrille : ColSpan propose (usage legitime)");
    ASSERT_HAS(items, "RowSpan",  "2163 dispGrille : RowSpan propose (usage legitime)");
    json_free(items);
    json_free(params);
}

/*  — parite bilingue : la forme EN canon (ColumnSpan) suit la
 * meme regle, cachee hors dispGrille et proposee sous dispGrille. */
static void test_columnspan_hidden_offered_en_2163(void) {
    printf("\n[TEST] fix/2163 : ColumnSpan (EN) suit la meme regle hors/en dispGrille\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    const char* srcDefault =
        "FEN_X is a Window\n"
        "\tBTN_X is a Control Button\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, srcDefault);
    JsonValue* pDefault = make_completion_params(URI, 3, 3);
    JsonValue* iDefault = handle_completion(pDefault);
    ASSERT_NOT_HAS(iDefault, "ColumnSpan", "2163 EN defaut : ColumnSpan absent hors dispGrille");
    json_free(iDefault); json_free(pDefault);

    const char* srcGrid =
        "FEN_X is a Window\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispGrid\n"
        "\tEND\n"
        "\tBTN_X is a Control Button\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, srcGrid);
    JsonValue* pGrid = make_completion_params(URI, 6, 3);
    JsonValue* iGrid = handle_completion(pGrid);
    ASSERT_HAS(iGrid, "ColumnSpan", "2163 EN dispGrid : ColumnSpan propose (usage legitime)");
    json_free(iGrid); json_free(pGrid);

    lspServer.language[0] = '\0';  /* reset defaut fr */
}

static void test_procedure_event_completion(void) {
    printf("\n[TEST] DSL v2 : 'PROCEDURE ' dans body widget propose events\n");
    /* Body Saisie : doit proposer surEntr\xc3\xa9""e, surSortie, surAChaqueModification,
     * surValidation. Pas Trace ni RougeClair.
     *  : `Initialisation` retire — le compilo le rejette deja
     * (E_SEM_DSL_INITIALISATION_OBSOLETE, ), l'init implicite vit
     * au top-level du widget. Test miroir du critere L109-110 du ticket. */
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tSAI_U est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tIndication = \"u\"\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Cursor ligne 8 (0-indexed) char 12 = juste apres 'PROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 8, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "surEntr\xc3\xa9""e",          "PROC body Saisie");
    ASSERT_HAS    (items, "surSortie",                    "PROC body Saisie");
    ASSERT_HAS    (items, "surAChaqueModification",       "PROC body Saisie");
    ASSERT_HAS    (items, "surValidation",                "PROC body Saisie");
    /*  : Initialisation NE doit PLUS etre propose (compilo le rejette). */
    ASSERT_NOT_HAS(items, "Initialisation",               "PROC body Saisie (obsolete fix/0497)");
    /* Bouton ne doit pas y etre. */
    ASSERT_NOT_HAS(items, "surClic",                      "PROC body Saisie (pas Bouton)");
    ASSERT_NOT_HAS(items, "Trace",                        "PROC body Saisie (pas native)");
    json_free(items);
    json_free(params);
}

static void test_procedure_event_filter_already_used(void) {
    printf("\n[TEST] DSL v2 : 'PROCEDURE ' filtre les events deja declares\n");
    /* surEntr\xc3\xa9""e deja declare -> ne doit pas etre propose. */
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tSAI_U est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tIndication = \"u\"\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surEntr\xc3\xa9""e()\n"
        "\t\t\tTrace(\"in\")\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Cursor ligne 11 (0-indexed) char 12 = juste apres 'PROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 11, 12);
    JsonValue* items = handle_completion(params);
    /* surEntrée DEJA declaré → absent. Les autres présents. */
    ASSERT_NOT_HAS(items, "surEntr\xc3\xa9""e",          "filter : surEntrée deja declare");
    ASSERT_HAS    (items, "surSortie",                    "filter : surSortie dispo");
    ASSERT_HAS    (items, "surAChaqueModification",       "filter : surAChaqueModification dispo");
    ASSERT_HAS    (items, "surValidation",                "filter : surValidation dispo");
    json_free(items);
    json_free(params);
}

/*  — `PROCEDURE ` hors body widget : doit respecter le contexte
 * fichier (body classe / fichier test / module standalone). Le fallback
 * actuel injectait les fixtures test runner partout, polluant les
 * fichiers classe et les scripts standalone. */
static void test_procedure_in_class_body_proposes_ctor_dtor(void) {
    printf("\n[TEST] fix/0324 : 'PROCEDURE ' body classe propose Constructeur/Destructeur (pas fixtures test)\n");
    const char* src =
        "cProduit est une Classe\n"
        "PUBLIC\n"
        "\tnom est une cha\xc3\xaene\n"
        "PROCEDURE \n"
        "FIN\n";
    /* URI nom *non* test_ et *non* cXxx — on s'appuie sur le sniff source. */
    setup_doc("file:///c:/test/cProduit.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/cProduit.owl", 3, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Constructeur",      "PROC body classe");
    ASSERT_HAS    (items, "Destructeur",       "PROC body classe");
    ASSERT_NOT_HAS(items, "apresChaqueTest",   "PROC body classe (pas fixture test)");
    ASSERT_NOT_HAS(items, "avantToutTest",     "PROC body classe (pas fixture test)");
    ASSERT_NOT_HAS(items, "scenario_nom",      "PROC body classe (pas prefixe scenario)");
    ASSERT_NOT_HAS(items, "should_nom",        "PROC body classe (pas prefixe should)");
    /*  : prefixe `detail` aligne sur lifecycle widget. */
    ASSERT_DETAIL_PREFIX(items, "Constructeur", "Lifecycle :", "PROC body classe (detail harmonise)");
    ASSERT_DETAIL_PREFIX(items, "Destructeur",  "Lifecycle :", "PROC body classe (detail harmonise)");
    json_free(items);
    json_free(params);
}

/*  — sniff fichier classe : la copule MIXTE `is une` (copule EN +
 * article FR) doit reconnaitre le body classe comme `est une` / `is a`. RED
 * avant fix (le sniff ne matchait que `est une Classe` et `is a Class`, donc
 * `is une Classe` ne classifiait pas → pas de Constructeur propose). */
static void test_procedure_in_class_body_copule_mixte(void) {
    printf("\n[TEST] fix/1039 : 'PROCEDURE ' body classe copule mixte `is une Classe` propose Constructeur/Destructeur\n");
    const char* src =
        "cProduit is une Classe\n"
        "PUBLIC\n"
        "\tnom est une cha\xc3\xaene\n"
        "PROCEDURE \n"
        "FIN\n";
    setup_doc("file:///c:/test/cProduit.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/cProduit.owl", 3, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Constructeur", "PROC body classe (copule mixte is une)");
    ASSERT_HAS(items, "Destructeur",  "PROC body classe (copule mixte is une)");
    json_free(items);
    json_free(params);
}

/*  — sniff fichier classe : copule EN complete `is a Class` (nom EN
 * du type). Non-regression du support EN existant apres branchement helper. */
static void test_procedure_in_class_body_copule_en(void) {
    printf("\n[TEST] fix/1039 : 'PROCEDURE ' body classe copule EN `is a Class` propose Constructeur/Destructeur\n");
    const char* src =
        "cProduit is a Class\n"
        "PUBLIC\n"
        "\tnom est une cha\xc3\xaene\n"
        "PROCEDURE \n"
        "FIN\n";
    setup_doc("file:///c:/test/cProduit.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/cProduit.owl", 3, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Constructeur", "PROC body classe (copule EN is a Class)");
    ASSERT_HAS(items, "Destructeur",  "PROC body classe (copule EN is a Class)");
    json_free(items);
    json_free(params);
}

/*  : FONCTION en body classe NE doit PAS proposer Constructeur /
 * Destructeur (sementiquement un constructeur n'a pas de type de retour).
 * L'utilisateur tape librement le nom de sa fonction membre. */
static void test_function_in_class_body_no_ctor_dtor(void) {
    printf("\n[TEST] fix/0333 : 'FONCTION ' body classe ne propose PAS Constructeur/Destructeur\n");
    const char* src =
        "cProduit est une Classe\n"
        "PUBLIC\n"
        "\tnom est une cha\xc3\xaene\n"
        "FONCTION \n"
        "FIN\n";
    setup_doc("file:///c:/test/cProduit.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/cProduit.owl", 3, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Constructeur",      "FONC body classe (constructeur sans retour bizarre)");
    ASSERT_NOT_HAS(items, "Destructeur",       "FONC body classe (destructeur sans retour bizarre)");
    /* Pas de fixtures non plus (basename non-test). */
    ASSERT_NOT_HAS(items, "avantToutTest",     "FONC body classe (pas fixture test)");
    ASSERT_NOT_HAS(items, "scenario_nom",      "FONC body classe (pas prefixe scenario)");
    json_free(items);
    json_free(params);
}

static void test_procedure_in_class_body_filters_existing_ctor(void) {
    printf("\n[TEST] fix/0324 : 'PROCEDURE ' body classe filtre Constructeur deja declare\n");
    const char* src =
        "cProduit est une Classe\n"
        "PUBLIC\n"
        "\tnom est une cha\xc3\xaene\n"
        "PROCEDURE Constructeur()\n"
        "FIN\n"
        "PROCEDURE \n"
        "FIN\n";
    setup_doc("file:///c:/test/cProduit.owl", src);
    /* Cursor ligne 5 char 10 = apres 'PROCEDURE '. */
    JsonValue* params = make_completion_params("file:///c:/test/cProduit.owl", 5, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Constructeur",      "filter : Constructeur deja declare");
    ASSERT_HAS    (items, "Destructeur",       "filter : Destructeur encore dispo");
    json_free(items);
    json_free(params);
}

static void test_procedure_in_test_file_proposes_fixtures(void) {
    printf("\n[TEST] fix/0324 : 'PROCEDURE ' fichier test_*.owl propose fixtures runner\n");
    const char* src =
        "// @sujet : Distinct\n"
        "PROCEDURE \n";
    setup_doc("file:///c:/test/test_Foo.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/test_Foo.owl", 1, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "avantToutTest",     "PROC test_*.owl");
    ASSERT_HAS    (items, "apresChaqueTest",   "PROC test_*.owl");
    ASSERT_HAS    (items, "scenario_nom",      "PROC test_*.owl");
    ASSERT_NOT_HAS(items, "Constructeur",      "PROC test_*.owl (pas classe)");
    ASSERT_NOT_HAS(items, "Destructeur",       "PROC test_*.owl (pas classe)");
    json_free(items);
    json_free(params);
}

static void test_procedure_in_standalone_module_no_test_fixtures(void) {
    printf("\n[TEST] fix/0324 : 'PROCEDURE ' module standalone n'injecte pas les fixtures test\n");
    const char* src =
        "// Script utilisateur ordinaire\n"
        "PROCEDURE \n";
    setup_doc("file:///c:/test/main.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/main.owl", 1, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "avantToutTest",     "PROC standalone (pas fixture test)");
    ASSERT_NOT_HAS(items, "apresChaqueTest",   "PROC standalone (pas fixture test)");
    ASSERT_NOT_HAS(items, "scenario_nom",      "PROC standalone (pas prefixe scenario)");
    ASSERT_NOT_HAS(items, "should_nom",        "PROC standalone (pas prefixe should)");
    ASSERT_NOT_HAS(items, "Constructeur",      "PROC standalone (pas classe)");
    ASSERT_NOT_HAS(items, "Destructeur",       "PROC standalone (pas classe)");
    json_free(items);
    json_free(params);
}

/*  — Regle d'or OWL : un fichier est un test SSI son basename
 * matche `test_*.owl`. Le tag `@sujet` ne doit JAMAIS suffire a
 * classifier un fichier comme test cote LSP / outils OWL utilisateur.
 * Verrouille la non-regression du helper `source_has_sujet_tag()` qui
 * avait ete introduit a tort par  puis retire. */
static void test_procedure_in_sujet_tagged_non_test_file_is_standalone(void) {
    printf("\n[TEST] fix/0332 : 'PROCEDURE ' fichier Foo.owl avec '@sujet' reste standalone (regle d'or test_*)\n");
    const char* src =
        "// @sujet : Distinct\n"
        "PROCEDURE \n";
    setup_doc("file:///c:/test/Foo.owl", src);
    JsonValue* params = make_completion_params("file:///c:/test/Foo.owl", 1, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "avantToutTest",     "PROC Foo.owl+@sujet (pas fixture : basename non-test)");
    ASSERT_NOT_HAS(items, "apresChaqueTest",   "PROC Foo.owl+@sujet (pas fixture : basename non-test)");
    ASSERT_NOT_HAS(items, "scenario_nom",      "PROC Foo.owl+@sujet (pas prefixe scenario)");
    ASSERT_NOT_HAS(items, "should_nom",        "PROC Foo.owl+@sujet (pas prefixe should)");
    ASSERT_NOT_HAS(items, "Constructeur",      "PROC Foo.owl+@sujet (pas classe)");
    ASSERT_NOT_HAS(items, "Destructeur",       "PROC Foo.owl+@sujet (pas classe)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *  Main
 * ================================================================ */

/* dsl_kind_string est expose non-static dans lsp_handlers.c (Lot F).
 * Forward decl ici car pas dans lsp_server.h. OWL_DSL_PROP_CATALOG vient
 * de owl_catalog.h (deja inclus). */
extern const char* dsl_kind_string(char k);

/* Lot F : autocomplete des valeurs d'enum Disposition dans le PROPRIETES
 * de la Fenetre. Regression vue manuellement : `dispHorizontale` n'etait
 * pas propose apres `Disposition = `. */
static void test_disposition_completion_in_window(void) {
    printf("\n[TEST] Lot F : 'Disposition = ' propose les 4 modes (Fenetre)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "dispVerticale",   "Disposition Fenetre");
    ASSERT_HAS(items, "dispHorizontale", "Disposition Fenetre");
    ASSERT_HAS(items, "dispGrille",      "Disposition Fenetre");
    ASSERT_HAS(items, "dispLibre",       "Disposition Fenetre");
    json_free(items);
    json_free(params);
}

/* Lot F : meme test mais dans le body d'un Conteneur — verifie que le LSP
 * detecte le kind Conteneur (regression precedente : dsl_kind_string ne
 * mappait pas 'O' -> Conteneur, donc l'autocomplete fail dans son body). */
static void test_disposition_completion_in_container(void) {
    printf("\n[TEST] Lot F : 'Disposition = ' propose les 4 modes (Conteneur)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES Disposition = dispLibre FIN\n"
        "\tCNT_X est un Conteneur\n"
        "\t\tPROPRIETES\n"
        "\t\t\tDisposition = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 4, 17);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "dispVerticale",   "Disposition Conteneur");
    ASSERT_HAS(items, "dispHorizontale", "Disposition Conteneur");
    ASSERT_HAS(items, "dispGrille",      "Disposition Conteneur");
    json_free(items);
    json_free(params);
}

/* Lot F — verrou de coherence : pour chaque kind unique apparaissant dans
 * OWL_DSL_PROP_CATALOG, on s'assure qu'il existe au moins UN char dans
 * dsl_kind_string qui le retourne. Sans ca, le LSP ne pourrait pas
 * resoudre le contexte de ce kind et perdrait l'autocomplete des props. */
static void test_all_dsl_kinds_routable(void) {
    printf("\n[TEST] Lot F : chaque kind du catalog est routable par dsl_kind_string\n");
    /* Collecte les kinds uniques. */
    const char* uniqueKinds[32];
    int nKinds = 0;
    for (int i = 0; i < OWL_DSL_PROP_CATALOG_COUNT; i++) {
        const char* k = OWL_DSL_PROP_CATALOG[i].kind;
        if (!k) continue;
        bool dup = false;
        for (int j = 0; j < nKinds; j++) {
            if (strcmp(uniqueKinds[j], k) == 0) { dup = true; break; }
        }
        if (!dup && nKinds < 32) uniqueKinds[nKinds++] = k;
    }
    /* Pour chaque kind, scan les chars 0x20..0x7F et verifie qu'au moins un
     * retourne ce kind via dsl_kind_string. */
    for (int i = 0; i < nKinds; i++) {
        bool found = false;
        for (int c = 0x20; c < 0x7F; c++) {
            const char* mapped = dsl_kind_string((char)c);
            if (mapped && strcmp(mapped, uniqueKinds[i]) == 0) {
                found = true; break;
            }
        }
        if (found) {
            g_pass++;
            printf("  [OK]   kind '%s' est routable\n", uniqueKinds[i]);
        } else {
            g_fail++;
            printf("  [FAIL] kind '%s' n'a PAS de mapping char dans "
                   "dsl_kind_string. L'autocomplete des props de ce kind "
                   "ne fonctionnera pas. Ajouter le case dans "
                   "lsp_handlers.c::dsl_kind_string + detect_dsl_context_ex.\n",
                   uniqueKinds[i]);
        }
    }
}

/* V1.5-C : LSP autocomplete dans un brace literal `var = { <cursor> }`. */
static void test_brace_literal_fields(void) {
    printf("\n[TEST] Lot V1.5-C : `var est un Cadre = { <cursor>` propose les fields\n");
    const char* src =
        "monCadre est un Cadre = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Couleur",        "Brace literal Cadre");
    ASSERT_HAS(items, "Epaisseur",      "Brace literal Cadre");
    ASSERT_HAS(items, "CoinHautGauche", "Brace literal Cadre");
    ASSERT_HAS(items, "TraitBas",       "Brace literal Cadre");
    json_free(items);
    json_free(params);
}

static void test_brace_literal_filter_used(void) {
    printf("\n[TEST] Lot V1.5-C : fields deja set sont filtres\n");
    const char* src =
        "monCadre est un Cadre = {\n"
        "\tCouleur = RougeFonce\n"
        "\tEpaisseur = 2\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 1);
    JsonValue* items = handle_completion(params);
    /* Couleur et Epaisseur deja set -> non proposes. */
    ASSERT_NOT_HAS(items, "Couleur",   "filtered Couleur");
    ASSERT_NOT_HAS(items, "Epaisseur", "filtered Epaisseur");
    /* Trait/Arrondi/Bords/etc. encore proposes. */
    ASSERT_HAS(items, "Trait",   "remaining Trait");
    ASSERT_HAS(items, "Arrondi", "remaining Arrondi");
    json_free(items);
    json_free(params);
}

static void test_brace_literal_enum_value(void) {
    printf("\n[TEST] Lot V1.5-C : RHS de field enum propose les valeurs\n");
    const char* src =
        "monCadre est un Cadre = {\n"
        "\tTrait = \n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "TraitContinu",   "enum value TraitContinu");
    ASSERT_HAS(items, "TraitTiret",     "enum value TraitTiret");
    ASSERT_HAS(items, "TraitDouble",    "enum value TraitDouble");
    json_free(items);
    json_free(params);
}

/* `Police = { Nom = <cursor> }` propose les iPolice* (iFamille). */
static void test_police_nom_proposes_iFamille(void) {
    printf("\n[TEST] Regression : `Police = { Nom = ` propose iFamille\n");
    const char* src =
        "p est une Police = {\n"
        "\tNom = \n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 7);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "iPoliceLiberationSans", "iFamille proposed");
    json_free(items);
    json_free(params);
}

/* Bug E.2 : RHS sur field non-enum dans brace literal inline avec virgules
 * doit retourner null (pas de propositions de fields aberrants). */
static void test_brace_literal_rhs_inline_string(void) {
    printf("\n[TEST] Regression E.2 : `Police = { Taille=14, Nom = <cursor> }` ne propose pas Barre/Couleur\n");
    const char* src =
        "s est un Style = {\n"
        "\tPolice = { Taille = 14, Gras = Vrai, Nom =  }\n"
        "}\n";
    setup_doc(URI, src);
    /* cursor juste apres `Nom =  ` (apres le double space, avant `}`) */
    JsonValue* params = make_completion_params(URI, 1, 44);
    JsonValue* items = handle_completion(params);
    /* On accepte null OU array vide. La regression est si on a des fields. */
    if (items && items->type == JSON_ARRAY) {
        ASSERT_NOT_HAS(items, "Barr\xc3\xa9", "no Barre");
        ASSERT_NOT_HAS(items, "Couleur",     "no Couleur");
        ASSERT_NOT_HAS(items, "Italique",    "no Italique");
        ASSERT_NOT_HAS(items, "Gras",        "no Gras");
    } else {
        printf("  [OK]   null retourne (pas de proposition aberrante)\n");
        g_pass++;
    }
    json_free(items);
    json_free(params);
}

/* Reproduit le cas user : `Cadre = {\n    <cursor>` (4 espaces, ligne propre). */
static void test_brace_literal_indent_line(void) {
    printf("\n[TEST] Regression : Ctrl+Espace dans brace literal sur ligne indentee\n");
    const char* src =
        "c est un Cadre = {\n"
        "    \n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Couleur",   "indented line proposes Couleur");
    ASSERT_HAS(items, "Epaisseur", "indented line proposes Epaisseur");
    json_free(items);
    json_free(params);
}

/* === Regression 2026-05-05 : bugs hover/autocomplete chain composed === */

/* Bug #5 : `widget..Cadre..BordBas..` (chain depuis une *constante*) ne
 * doit RIEN proposer — pas la liste fallback Date/Duree. */
static void test_chain_from_constant_no_completion(void) {
    printf("\n[TEST] Regression #5 : chain `..BordBas..` ne propose rien\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tLIB_X est un Libell\xc3\xa9\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tLIB_X..Cadre..BordBas..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Cursor right after `..` on line 3 (0-indexed). */
    JsonValue* params = make_completion_params(URI, 3, 27);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Annee",    "no Date.Annee proposed");
    ASSERT_NOT_HAS(items, "EnHeures", "no Duree.EnHeures proposed");
    json_free(items);
    json_free(params);
}

/* Bug #4 : `..Cadre..BordBas = <cursor>` doit proposer les valeurs de
 * l'enum bordsCadre (BordHaut, BordBas, BordGauche, BordDroite, BordTous). */
static void test_composed_field_rhs_enum(void) {
    printf("\n[TEST] Regression #4 : `..Cadre..Bords = ` propose enum bordsCadre\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tLIB_X est un Libell\xc3\xa9\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tLIB_X..Cadre..Bords = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 26);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "BordHaut",    "enum bordsCadre");
    ASSERT_HAS(items, "BordBas",     "enum bordsCadre");
    ASSERT_HAS(items, "BordGauche",  "enum bordsCadre");
    ASSERT_HAS(items, "BordDroite",  "enum bordsCadre");
    json_free(items);
    json_free(params);
}

/* Bug #7 : `Police = { Taille = 14, Gras = Vrai }` — apres la virgule,
 * le LSP doit reconnaitre Gras comme field de Police, pas tomber dans
 * le fallback. (Test indirect : autocomplete au curseur juste apres la
 * virgule propose Gras et Taille comme fields Police.) */
static void test_brace_literal_after_comma(void) {
    printf("\n[TEST] Regression #7 : fields apres virgule dans brace literal\n");
    const char* src =
        "monStyle est un Style = {\n"
        "\tPolice = { Taille = 14, \n"
        "}\n";
    setup_doc(URI, src);
    /* Cursor after `Taille = 14, ` on line 1. */
    JsonValue* params = make_completion_params(URI, 1, 24);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Gras",   "Police.Gras after comma");
    ASSERT_HAS(items, "Nom",    "Police.Nom after comma");
    /* Taille deja set -> filtre. */
    ASSERT_NOT_HAS(items, "Taille", "Taille filtered (already set)");
    json_free(items);
    json_free(params);
}

/*  — style inline sur un widget : `Style = { <cursor> }` dans le
 * bloc PROPRIETES d'une Saisie propose les champs de styleSaisie (résolution
 * réflexive prop→type via l'objectType de la prop Style). Les slots internes
 * d'état (`_Survol`/`_Focus`/`_Grise`/`_LectureSeule`) restent masqués. */
static void test_brace_literal_style_prop_on_saisie(void) {
    printf("\n[TEST] feat/0838 : `Style = { ` dans une Saisie propose les champs styleSaisie\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_U est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle = { \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 3 (0-based) : 3 tabs + "Style = { " ; curseur juste apres `{ `. */
    JsonValue* params = make_completion_params(URI, 3, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "CouleurFond",   "Style{} propose CouleurFond");
    ASSERT_HAS(items, "Couleur",       "Style{} propose Couleur");
    ASSERT_HAS(items, "RetraitGauche", "Style{} propose RetraitGauche");
    ASSERT_HAS(items, "Police",        "Style{} propose Police");
    ASSERT_HAS(items, "Cadre",         "Style{} propose Cadre");
    /* Slots internes d'etat : jamais proposes (prefixe `_`). */
    ASSERT_NOT_HAS(items, "_Survol",       "Style{} sans slot interne _Survol");
    ASSERT_NOT_HAS(items, "_Focus",        "Style{} sans slot interne _Focus");
    ASSERT_NOT_HAS(items, "_Grise",        "Style{} sans slot interne _Grise");
    ASSERT_NOT_HAS(items, "_LectureSeule", "Style{} sans slot interne _LectureSeule");
    json_free(items);
    json_free(params);
}

/*  (recadre ) — chaine style inline `Style..Image..<cursor>`
 * sur un Bouton doit resoudre le SOUS-ELEMENT Image du styleBouton
 * (styleImageBouton : Position) et PAS le TYPE bitmap global homonyme `Image`
 * (AvecAlpha, BitParPixel, Hauteur, Largeur...). Collision de noms : le contexte
 * composite (Bouton -> Style -> styleBouton -> Image) prime sur le type global. */
static void test_style_inline_subelement_image_on_bouton(void) {
    printf("\n[TEST] fix/0895 : `Style..Image..` (Bouton) propose Position, pas le type bitmap\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_U est un Bouton\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tBTN_U..Style..Image..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 3 (0-based) : 3 tabs + "BTN_U..Style..Image.." — curseur en fin. */
    int col = (int)strlen("\t\t\tBTN_U..Style..Image..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    /* Feuille du sous-element styleImageBouton. */
    ASSERT_HAS(items, "Position", "Style..Image.. propose Position");
    /* Props du TYPE bitmap global : jamais proposees ici. */
    ASSERT_NOT_HAS(items, "AvecAlpha",     "pas de prop du type bitmap AvecAlpha");
    ASSERT_NOT_HAS(items, "BitParPixel",   "pas de prop du type bitmap BitParPixel");
    ASSERT_NOT_HAS(items, "Hauteur",       "pas de prop du type bitmap Hauteur");
    ASSERT_NOT_HAS(items, "Largeur",       "pas de prop du type bitmap Largeur");
    json_free(items);
    json_free(params);
}

/*  — generalisation : `Style..Libellé..` sur un Bouton doit resoudre
 * le sous-element Libelle (styleZoneBouton) et proposer ses feuilles (Couleur,
 * CouleurFond, Police, Cadre...), pas retomber dans un fallback. */
static void test_style_inline_subelement_libelle_on_bouton(void) {
    printf("\n[TEST] fix/0895 : `Style..Libelle..` (Bouton) propose les feuilles de styleZoneBouton\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_U est un Bouton\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tBTN_U..Style..Libelle..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tBTN_U..Style..Libelle..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Couleur",     "Style..Libelle.. propose Couleur");
    ASSERT_HAS(items, "CouleurFond", "Style..Libelle.. propose CouleurFond");
    ASSERT_HAS(items, "Cadre",       "Style..Libelle.. propose Cadre");
    /* Le sous-element ne collisionne pas ici, mais on verrouille qu'aucune
     * prop du type bitmap ne fuite (garde-fou generalisation). */
    ASSERT_NOT_HAS(items, "AvecAlpha", "pas de prop du type bitmap AvecAlpha");
    json_free(items);
    json_free(params);
}

/*  — `Style..` racine (1er hop) dans le bloc PROPRIETES d'une Saisie
 * doit amorcer la descente sur le type RÉFLEXIF du widget (styleSaisie :
 * ZoneSaisie/Libellé/CadreExtérieur), PAS sur le type composé homonyme
 * GÉNÉRIQUE `Style` (Couleur/CurseurSouris/Survol/Retrait*...). */
static void test_style_root_reflexive_on_saisie(void) {
    printf("\n[TEST] fix/1290 : `Style..` racine (Saisie) -> sous-elements styleSaisie\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_U est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tStyle..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    /* Sous-elements reflexifs de styleSaisie. */
    ASSERT_HAS(items, "ZoneSaisie",          "Style.. propose ZoneSaisie");
    ASSERT_HAS(items, "Libell\xc3\xa9",       "Style.. propose Libellé");
    ASSERT_HAS(items, "CadreExt\xc3\xa9rieur","Style.. propose CadreExtérieur");
    /* Champs du type composé GÉNÉRIQUE `Style` : jamais proposes ici. */
    ASSERT_NOT_HAS(items, "CurseurSouris", "pas de champ generique CurseurSouris");
    ASSERT_NOT_HAS(items, "Survol",        "pas de champ generique Survol");
    ASSERT_NOT_HAS(items, "RetraitBas",    "pas de champ generique RetraitBas");
    json_free(items);
    json_free(params);
}

/*  — meme correction pour un autre widget a style reflexif : Bouton
 * (styleBouton : Libellé/Image/CadreExtérieur). */
static void test_style_root_reflexive_on_bouton(void) {
    printf("\n[TEST] fix/1290 : `Style..` racine (Bouton) -> sous-elements styleBouton\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_U est un Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tStyle..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Libell\xc3\xa9",        "Style.. (Bouton) propose Libellé");
    ASSERT_HAS(items, "Image",                 "Style.. (Bouton) propose Image");
    ASSERT_HAS(items, "CadreExt\xc3\xa9rieur", "Style.. (Bouton) propose CadreExtérieur");
    ASSERT_NOT_HAS(items, "CurseurSouris", "pas de champ generique CurseurSouris");
    ASSERT_NOT_HAS(items, "Survol",        "pas de champ generique Survol");
    json_free(items);
    json_free(params);
}

/*  — trou de couverture : la forme `est un Champ de Saisie` (filler
 * `de`/`of` entre le modificateur Champ et le genre) doit resoudre le meme
 * type reflexif styleSaisie que `est une Saisie` (type nu) et `is a Control
 * Input` (EN). Sans le fix, detect_dsl_context_ex perdait wasEst sur le
 * TOKEN_DE -> widget jamais empile -> repli generique (1 seul label). */
static void test_style_root_reflexive_on_saisie_champ_de(void) {
    printf("\n[TEST] fix/1290 : `Style..` racine (Champ de Saisie) -> styleSaisie\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_U est un Champ de Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tStyle..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "ZoneSaisie",          "Style.. (Champ de Saisie) propose ZoneSaisie");
    ASSERT_HAS(items, "Libell\xc3\xa9",       "Style.. (Champ de Saisie) propose Libellé");
    ASSERT_HAS(items, "CadreExt\xc3\xa9rieur","Style.. (Champ de Saisie) propose CadreExtérieur");
    ASSERT_NOT_HAS(items, "CurseurSouris", "pas de champ generique CurseurSouris");
    ASSERT_NOT_HAS(items, "Survol",        "pas de champ generique Survol");
    ASSERT_NOT_HAS(items, "RetraitBas",    "pas de champ generique RetraitBas");
    json_free(items);
    json_free(params);
}

/*  — variante sans filler : `est un Champ Bouton` (modificateur Champ
 * + genre nu, sans `de`). Doit resoudre styleBouton (Libellé/Image/CadreExt). */
static void test_style_root_reflexive_on_bouton_champ(void) {
    printf("\n[TEST] fix/1290 : `Style..` racine (Champ Bouton) -> styleBouton\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_U est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tStyle..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Libell\xc3\xa9",        "Style.. (Champ Bouton) propose Libellé");
    ASSERT_HAS(items, "Image",                 "Style.. (Champ Bouton) propose Image");
    ASSERT_HAS(items, "CadreExt\xc3\xa9rieur", "Style.. (Champ Bouton) propose CadreExtérieur");
    ASSERT_NOT_HAS(items, "CurseurSouris", "pas de champ generique CurseurSouris");
    ASSERT_NOT_HAS(items, "Survol",        "pas de champ generique Survol");
    json_free(items);
    json_free(params);
}

/*  — non-regression 2e hop : `Style..Libellé..` dans une Saisie doit
 * toujours resoudre le sous-type styleLibelléSaisie (Couleur/Police/Cadre...). */
static void test_style_second_hop_libelle_on_saisie(void) {
    printf("\n[TEST] fix/1290 : `Style..Libelle..` (Saisie) -> feuilles styleLibelleSaisie\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_U est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tStyle..Libelle..\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tStyle..Libelle..");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Couleur", "Style..Libelle.. propose Couleur");
    ASSERT_HAS(items, "Police",  "Style..Libelle.. propose Police");
    ASSERT_HAS(items, "Cadre",   "Style..Libelle.. propose Cadre");
    json_free(items);
    json_free(params);
}

/*  — curseur dans `Style..OptionMenu..Etats[` (surface MENU) : ne
 * propose QUE `survol` (regle survol-only des menus, compiler.c ~22094).
 * Aucun etat WIDGET (focus/appuye/grise/lectureSeule) ne doit fuiter ici —
 * parite stricte avec E_SEM_MENU_STYLE_STATE_UNKNOWN qui ne tolere que
 * 'survol'. Smoke EN sur la meme chaine (hover). */
static void test_completion_etats_bracket_menu_optionmenu(void) {
    printf("\n[TEST] feat/1228 : `Style..OptionMenu..Etats[` (menu) -> survol seul\n");
    const char* src =
        "FEN_X est une Fenetre\n"                     /* 0 */
        "\tMNU_M est un MenuPrincipal\n"              /* 1 */
        "\t\tPROPRIETES\n"                            /* 2 */
        "\t\t\tStyle..OptionMenu..Etats[\n"            /* 3 */
        "\t\tFIN\n"                                    /* 4 */
        "\tFIN\n"                                      /* 5 */
        "FIN\n";                                       /* 6 */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    int col = (int)strlen("\t\t\tStyle..OptionMenu..Etats[");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "survol", "Etats[ menu FR propose survol");
    ASSERT_NOT_HAS(items, "focus",  "Etats[ menu ne propose pas focus (widget)");
    ASSERT_NOT_HAS(items, "appuyé", "Etats[ menu ne propose pas appuyé (widget)");
    ASSERT_NOT_HAS(items, "grisé",  "Etats[ menu ne propose pas grisé (widget)");
    json_free(items);
    json_free(params);

    /* Smoke EN : meme chaine, langue anglaise -> hover. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 3, col);
    items = handle_completion(params);
    ASSERT_HAS(items, "hover", "Etats[ menu EN propose hover");
    ASSERT_NOT_HAS(items, "focus", "Etats[ menu EN ne propose pas focus (widget)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  — curseur dans `Style..ZoneSaisie..Etats[` (sous-type WIDGET
 * ZoneSaisie) : propose survol/focus/grisé/lectureSeule (STATES_styleZoneSaisie,
 * compiler.c ~10231). `appuyé` (propre au Bouton) doit etre absent -> prouve
 * que la complétion suit le sous-type resolu, pas une liste globale. */
static void test_completion_etats_bracket_widget_zonesaisie(void) {
    printf("\n[TEST] feat/1228 : `Style..ZoneSaisie..Etats[` (widget) -> jeu ZoneSaisie\n");
    const char* src =
        "FEN_X est une Fenetre\n"                       /* 0 */
        "\tSAI_X est un Champ Saisie\n"                /* 1 */
        "\t\tPROPRIETES\n"                              /* 2 */
        "\t\t\tStyle..ZoneSaisie..Etats[\n"              /* 3 */
        "\t\tFIN\n"                                      /* 4 */
        "\tFIN\n"                                        /* 5 */
        "FIN\n";                                         /* 6 */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    int col = (int)strlen("\t\t\tStyle..ZoneSaisie..Etats[");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "survol",       "Etats[ ZoneSaisie propose survol");
    ASSERT_HAS(items, "focus",        "Etats[ ZoneSaisie propose focus");
    ASSERT_HAS(items, "grisé",        "Etats[ ZoneSaisie propose grisé");
    ASSERT_HAS(items, "lectureSeule", "Etats[ ZoneSaisie propose lectureSeule");
    ASSERT_NOT_HAS(items, "appuyé", "Etats[ ZoneSaisie ne propose pas appuyé (propre au Bouton)");
    json_free(items);
    json_free(params);
}

/*  — curseur dans `Style..Image..Etats[` (sous-element ImageBouton,
 * BTN_U..Style..Image..) : propose UNIQUEMENT survol/grisé (STATES_styleImageBouton,
 * compiler.c ~10253), sans focus ni appuyé ni lectureSeule -> verrouille que ce
 * n'est pas une liste globale partagee entre sous-types. */
static void test_completion_etats_bracket_widget_imagebouton(void) {
    printf("\n[TEST] feat/1228 : `BTN_U..Style..Image..Etats[` (widget) -> survol/grisé seuls\n");
    const char* src =
        "FEN_X est une Fenetre\n"                            /* 0 */
        "\tBTN_U est un Bouton\n"                            /* 1 */
        "\t\tPROCEDURE surClic()\n"                          /* 2 */
        "\t\t\tBTN_U..Style..Image..Etats[\n"                 /* 3 */
        "\t\tFIN\n"                                           /* 4 */
        "\tFIN\n"                                             /* 5 */
        "FIN\n";                                              /* 6 */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    int col = (int)strlen("\t\t\tBTN_U..Style..Image..Etats[");
    JsonValue* params = make_completion_params(URI, 3, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "survol", "Etats[ Image bouton propose survol");
    ASSERT_HAS(items, "grisé",  "Etats[ Image bouton propose grisé");
    ASSERT_NOT_HAS(items, "focus",        "Etats[ Image bouton ne propose pas focus");
    ASSERT_NOT_HAS(items, "appuyé",       "Etats[ Image bouton ne propose pas appuyé");
    ASSERT_NOT_HAS(items, "lectureSeule", "Etats[ Image bouton ne propose pas lectureSeule");
    json_free(items);
    json_free(params);
}

/* Regression Phase 1bis : completion native item retourne un insertText
 * snippet riche `Name(${1:p1}, ${2:p2})` avec les noms de params, pas
 * le generique `Name($0)`. Pour les multi-overload, prend la sig la
 * plus courte (params obligatoires uniquement). */
static void test_completion_native_snippet_rich(void) {
    printf("\n[TEST] completion native : insertText snippet riche avec params\n");
    const char* src = "Agr\n";  /* On tape "Agr" pour declencher completion */
    setup_doc(URI, src);

    JsonValue* params = make_completion_params(URI, 0, 3);
    JsonValue* result = handle_completion(params);
    JsonValue* items = unwrap_items(result);

    bool found = false;
    const char* insertText = NULL;
    if (items && items->type == JSON_ARRAY) {
        for (int i = 0; i < items->as.arrayVal.count; i++) {
            JsonValue* item = items->as.arrayVal.items[i];
            const char* lbl = json_get_string(item, "label");
            if (lbl && strcmp(lbl, "Agr\xc3\xa8ge") == 0) {  /* Agrège */
                insertText = json_get_string(item, "insertText");
                found = true;
                break;
            }
        }
    }
    if (!found) {
        g_fail++;
        printf("  [FAIL] item 'Agrège' absent du completion\n");
    } else if (insertText && strstr(insertText, "${1:tableau}") != NULL
               && strstr(insertText, "${2:op\xc3\xa9ration}") != NULL
               && strstr(insertText, "${3:valeur initiale}") != NULL) {
        /* item 2 (forme riche) : snippet builder choisit la sig la plus RICHE
         * (tous les params, incluant optionnel "valeur initiale"). */
        g_pass++;
        printf("  [OK]   insertText snippet riche avec params nommes (sig riche 3 params)\n");
    } else {
        g_fail++;
        printf("  [FAIL] insertText incorrect — '%s'\n",
               insertText ? insertText : "(null)");
    }
    json_free(result);
    json_free(params);
}

/* Regression : completion snippet utilise un dropdown choices
 * `${N|v1,v2|}` pour les params avec allowedBaseEnum non-combinable.
 * Plus pratique qu'un placeholder editable car le user choisit
 * directement la valeur enum au lieu d'effacer un nom fictif. */
static void test_completion_native_snippet_enum_dropdown(void) {
    printf("\n[TEST] completion native : dropdown choices pour params enum\n");
    /* ToastAffiche : 5 params variadic, dont duree (E_toastDuree),
     * cadrageV (E_CadrageV), cadrageH (E_CadrageH) — tous non-combinables. */
    const char* src = "ToastA\n";
    setup_doc(URI, src);

    JsonValue* params = make_completion_params(URI, 0, 6);
    JsonValue* result = handle_completion(params);
    JsonValue* items = unwrap_items(result);

    const char* insertText = NULL;
    if (items && items->type == JSON_ARRAY) {
        for (int i = 0; i < items->as.arrayVal.count; i++) {
            JsonValue* item = items->as.arrayVal.items[i];
            const char* lbl = json_get_string(item, "label");
            if (lbl && strcmp(lbl, "ToastAffiche") == 0) {
                insertText = json_get_string(item, "insertText");
                break;
            }
        }
    }
    if (!insertText) {
        g_fail++;
        printf("  [FAIL] item 'ToastAffiche' absent du completion\n");
        json_free(result); json_free(params);
        return;
    }
    /* Verifie le format dropdown pour duree (2e param, E_toastDuree
     * non-combinable, 2 valeurs). */
    if (strstr(insertText, "${2|toastCourt,toastLong|}") != NULL) {
        g_pass++;
        printf("  [OK]   duree -> dropdown `${2|toastCourt,toastLong|}`\n");
    } else {
        g_fail++;
        printf("  [FAIL] duree pas en dropdown — insertText='%s'\n", insertText);
    }
    /* Verifie aussi cadrageV (E_CadrageV : cvHaut, cvCentre, cvMilieu, cvBas
     * — 4 valeurs, non-combinable). */
    if (strstr(insertText, "${3|cvHaut,cvCentre,cvMilieu,cvBas|}") != NULL) {
        g_pass++;
        printf("  [OK]   cadrageV -> dropdown 4 choix\n");
    } else {
        g_fail++;
        printf("  [FAIL] cadrageV pas en dropdown 4 choix\n");
    }
    /* message (pas d'enum) reste en placeholder editable. */
    if (strstr(insertText, "${1:message}") != NULL) {
        g_pass++;
        printf("  [OK]   message (sans enum) reste placeholder editable\n");
    } else {
        g_fail++;
        printf("  [FAIL] message pas en placeholder\n");
    }
    json_free(result);
    json_free(params);
}

/* Regression Phase 2 : signatureHelp expose toutes les variantes d'une
 * native multi-sig (Agrege 2-args vs 3-args) avec activeSignature qui
 * suit le commaCount courant. Avant : 1 seule sig retournee (sig[0]
 * arbitraire). */
static void test_signature_help_multi_overload(void) {
    printf("\n[TEST] signatureHelp : Agrège multi-overload\n");
    const char* src =
        "tab est un tableau d'entier\n"
        "result est un entier = Agrège(tab, fnReduce, 0)\n";
    setup_doc(URI, src);

    /* Position 1 : curseur apres 1ere virgule de Agrege(...). Char position
     * dans la ligne 1 : "result est un entier = Agrège(tab, " -> on cible
     * juste apres la 1ere virgule. La ligne contient "Agrège(tab, " ; le
     * "è" est 2 bytes UTF-8 donc la position char (= bytes) est ~35. */
    int comma1 = 0;
    {
        const char* line1 = strchr(src, '\n') + 1;  /* start of line 1 */
        const char* p = line1;
        const char* lineEnd = strchr(p, '\n');
        while (p < lineEnd && *p != ',') p++;
        comma1 = (int)((p + 2) - line1);  /* +2 : virgule + espace */
    }

    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(URI));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(1));
    json_object_set(pos, "character", json_new_int(comma1));
    json_object_set(params, "position", pos);

    JsonValue* result = handle_signature_help(params);
    JsonValue* sigs = result ? json_get(result, "signatures") : NULL;
    int sigCount = (sigs && sigs->type == JSON_ARRAY) ? sigs->as.arrayVal.count : 0;
    if (sigCount >= 2) {
        g_pass++;
        printf("  [OK]   signatures[] expose %d variantes (>= 2 pour Agrege)\n", sigCount);
    } else {
        g_fail++;
        printf("  [FAIL] signatures[] = %d (attendu >= 2)\n", sigCount);
    }
    /* activeSignature : avec commaCount=1, on est en train de saisir le
     * 2e param ; la sig courte (2 params) est la plus specifique. */
    int active = result ? (int)json_get_int(result, "activeSignature", -1) : -1;
    if (active >= 0) {
        g_pass++;
        printf("  [OK]   activeSignature = %d (calcule, pas hardcoded 0)\n", active);
    } else {
        g_fail++;
        printf("  [FAIL] activeSignature manquant\n");
    }
    json_free(result);
    json_free(params);
}

/* Regression : hover sur une variable user qui shadow une constante
 * built-in doit montrer la VARIABLE, pas la constante. WL canon : `tab est
 * un tableau` est accepte avec warning, la variable user gagne au runtime
 * — le LSP hover doit refleter cette resolution.
 *
 * Avant fix Bundle 2 : handle_hover consultait OWL_CONST_CATALOG avant
 * userSymbols, donc le hover sur la variable `tab` affichait "constante
 * TAB — Caract(9) — tabulation". Mensonge.
 *
 * Apres : userSymbols gagne. Le hover affiche `variable tab : tableau`. */
static void test_hover_user_var_shadows_const(void) {
    printf("\n[TEST] Hover : variable user shadow constante WL\n");
    const char* src =
        "tab est un tableau de cha\xc3\xaene\n"   /* tab : tableau */
        "Ajoute(tab, \"hello\")\n";
    setup_doc(URI, src);

    /* Position curseur sur le 'tab' ligne 0 col 1 (au milieu du mot). */
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(URI));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(0));
    json_object_set(pos, "character", json_new_int(1));
    json_object_set(params, "position", pos);

    JsonValue* result = handle_hover(params);
    JsonValue* contents = result ? json_get(result, "contents") : NULL;
    const char* value = contents ? json_get_string(contents, "value") : NULL;

    if (value && strstr(value, "variable") != NULL &&
        strstr(value, "tableau") != NULL &&
        strstr(value, "constante") == NULL) {
        g_pass++;
        printf("  [OK]   hover affiche variable user, pas constante TAB\n");
    } else {
        g_fail++;
        printf("  [FAIL] hover incorrect — value='%s'\n",
               value ? value : "(null)");
    }

    /* Position sur la 2e occurrence de tab (ligne 1) : doit aussi
     * resoudre vers la variable, peu importe que ce soit hors LHS decl. */
    json_free(params);
    params = json_new_object();
    td = json_new_object();
    json_object_set(td, "uri", json_new_string(URI));
    json_object_set(params, "textDocument", td);
    pos = json_new_object();
    json_object_set(pos, "line", json_new_int(1));
    json_object_set(pos, "character", json_new_int(8));  /* dans 'tab' de Ajoute(tab, ...) */
    json_object_set(params, "position", pos);

    JsonValue* result2 = handle_hover(params);
    JsonValue* contents2 = result2 ? json_get(result2, "contents") : NULL;
    const char* value2 = contents2 ? json_get_string(contents2, "value") : NULL;
    if (value2 && strstr(value2, "variable") != NULL &&
        strstr(value2, "constante") == NULL) {
        g_pass++;
        printf("  [OK]   hover sur usage (pas LHS) toujours variable user\n");
    } else {
        g_fail++;
        printf("  [FAIL] hover usage incorrect — value='%s'\n",
               value2 ? value2 : "(null)");
    }

    json_free(result);
    json_free(result2);
    json_free(params);
}

/* ================================================================
 *   — Hover EN quand lspServer.language == "en"
 *
 *  Le hover doit servir l'anglais sur tous les sous-cas quand le client a
 *  initialise language=en : natives (descriptionEN + titre nameEN), methodes
 *  (label "Method on <type>"), constantes (nameEN), types (nameEN). Fallback
 *  FR si pas de champ EN. Le mode FR (defaut) ne regresse pas.
 *
 *  RED-FIRST : avant impl, handle_hover ignore lspServer.language -> rend FR
 *  -> ces tests rougissent. Apres impl -> verts.
 * ================================================================ */

static JsonValue* make_hover_params(const char* uri, int line, int character) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    return params;
}


/* Recupere contents.value (markdown) d'un resultat hover, NULL si absent. */
static const char* hover_value(JsonValue* result) {
    JsonValue* contents = result ? json_get(result, "contents") : NULL;
    return contents ? json_get_string(contents, "value") : NULL;
}

#define ASSERT_HOVER_CONTAINS(val, needle, test_name) do { \
    if ((val) && strstr((val), (needle)) != NULL) { \
        g_pass++; \
        printf("  [OK]   %s : contient \"%s\"\n", test_name, needle); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : \"%s\" ABSENT — value=%.120s\n", \
               test_name, needle, (val) ? (val) : "(null)"); \
    } \
} while (0)

#define ASSERT_HOVER_NOT_CONTAINS(val, needle, test_name) do { \
    if (!(val) || strstr((val), (needle)) == NULL) { \
        g_pass++; \
        printf("  [OK]   %s : \"%s\" absent\n", test_name, needle); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : \"%s\" PRESENT alors qu'attendu absent\n", \
               test_name, needle); \
    } \
} while (0)

/*  — Hover sur un nom de procedure/fonction du projet : le detail
 * doit porter la signature (liste de parametres) et, pour une FONCTION, le
 * type de retour — pas seulement "Procedure"/"Fonction".
 * RED-FIRST : avant impl, detail = "Procedure"/"Fonction" (sans params) ->
 * ces assertions rougissent. Apres impl -> vertes. */
static void test_hover_proc_signature(void) {
    printf("\n[TEST] feat/2277 : hover procedure/fonction -> signature params\n");
    const char* src =
        "PROCEDURE Calcul(x est un entier, y est une cha\xc3\xaene)\n" /* 0 */
        "\tRENVOYER 0\n"                                                /* 1 */
        "FIN\n"                                                         /* 2 */
        "FONCTION Somme(a est un entier, b est un entier) : entier\n"   /* 3 */
        "\tRENVOYER a + b\n"                                            /* 4 */
        "FIN\n"                                                         /* 5 */
        "PROCEDURE Ping()\n"                                            /* 6 */
        "FIN\n"                                                         /* 7 */
        "Calcul(1, \"a\")\n";                                          /* 8 */
    setup_doc(URI, src);

    /* 1) Procedure avec parametres : signature complete dans le hover. */
    JsonValue* p1 = make_hover_params(URI, 0, 12); /* sur 'Calcul' decl */
    JsonValue* r1 = handle_hover(p1);
    const char* v1 = hover_value(r1);
    ASSERT_HOVER_CONTAINS(v1,
        "Procedure(x est un entier, y est une cha\xc3\xaene)",
        "hover proc params");

    /* Meme signature depuis un site d'appel (ligne 8). */
    JsonValue* p1b = make_hover_params(URI, 8, 2); /* sur 'Calcul(...)' appel */
    JsonValue* r1b = handle_hover(p1b);
    const char* v1b = hover_value(r1b);
    ASSERT_HOVER_CONTAINS(v1b,
        "Procedure(x est un entier, y est une cha\xc3\xaene)",
        "hover proc params sur appel");

    /* 2) Fonction avec parametres + type de retour. */
    JsonValue* p2 = make_hover_params(URI, 3, 11); /* sur 'Somme' */
    JsonValue* r2 = handle_hover(p2);
    const char* v2 = hover_value(r2);
    ASSERT_HOVER_CONTAINS(v2,
        "Fonction(a est un entier, b est un entier) : entier",
        "hover fonction params + retour");

    /* 3) Procedure sans parametre : parentheses vides. */
    JsonValue* p3 = make_hover_params(URI, 6, 12); /* sur 'Ping' */
    JsonValue* r3 = handle_hover(p3);
    const char* v3 = hover_value(r3);
    ASSERT_HOVER_CONTAINS(v3, "Procedure()", "hover proc sans param");

    json_free(r1);  json_free(p1);
    json_free(r1b); json_free(p1b);
    json_free(r2);  json_free(p2);
    json_free(r3);  json_free(p3);
}

/* Hover EN sur une native (Arrondi/Round) : desc EN + alias EN, pas la
 * desc FR. */
static void test_hover_native_en(void) {
    printf("\n[TEST] feat/0663 : hover EN sur native (Arrondi) -> desc EN\n");
    const char* src = "x = Arrondi(3.14, 1)\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 0, 6);  /* sur 'Arrondi' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Round to the specified", "hover native EN desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Arrondit un nombre",     "hover native EN no FR desc");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme native en mode FR (defaut) garde la desc FR. */
static void test_hover_native_fr_default(void) {
    printf("\n[TEST] feat/0663 : hover FR (defaut) sur native (Arrondi) -> desc FR\n");
    const char* src = "x = Arrondi(3.14, 1)\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_hover_params(URI, 0, 6);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Arrondit un nombre",     "hover native FR desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Round to the specified", "hover native FR no EN desc");
    json_free(result);
    json_free(params);
}

/*  — hover EN sur une native a nameEN distinct (DonneFocus/SetFocus) :
 * les lignes de signature doivent porter le nom EN (SetFocus(...)), pas le nom
 * FR canon (DonneFocus(...)). Les noms de params restent FR (content gap acte
 * cf ). */
static void test_hover_native_signature_name_en(void) {
    printf("\n[TEST] fix/0707 : hover EN sur native (SetFocus) -> signature nom EN\n");
    const char* src = "DonneFocus()\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 0, 2);  /* sur 'DonneFocus' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "SetFocus(",   "hover native EN signature nom EN");
    ASSERT_HOVER_NOT_CONTAINS(v, "DonneFocus(", "hover native EN signature no FR nom");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression  : meme native en mode FR (defaut) garde le nom FR
 * canon dans la signature. */
static void test_hover_native_signature_name_fr_default(void) {
    printf("\n[TEST] fix/0707 : hover FR (defaut) sur native (DonneFocus) -> signature nom FR\n");
    const char* src = "DonneFocus()\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_hover_params(URI, 0, 2);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "DonneFocus(", "hover native FR signature nom FR");
    ASSERT_HOVER_NOT_CONTAINS(v, "SetFocus(",   "hover native FR signature no EN nom");
    json_free(result);
    json_free(params);
}

/*  items 1&2 — hover menu-aware. Dans un bloc `IDENT est un
 * MenuPrincipal` / `MenuContextuel`, la chaine `Style..<surface>..<feuille>`
 * doit resoudre sur le type de style typé du menu (styleMenuPrincipal /
 * styleMenuContextuel) et ses sous-surfaces (Barre/SousMenu/OptionMenu), PAS
 * sur le styleConteneur de la fenetre englobante ni sur une native homonyme
 * (`Arrondi()`). RED avant impl : hover `Style` -> styleConteneur, hover
 * `Arrondi` -> native Arrondi(). */
static void test_hover_menu_style_chain_principal(void) {
    printf("\n[TEST] feat/1214 : hover `Style..` menu-aware (MenuPrincipal)\n");
    const char* src =
        "FEN_X est une Fenetre\n"                        /* 0 */
        "\tMNU_M est un MenuPrincipal\n"                 /* 1 */
        "\t\tPROPRIETES\n"                               /* 2 */
        "\t\t\tStyle..Barre..CouleurFond = 0\n"          /* 3 */
        "\t\t\tStyle..SousMenu..Arrondi = 8\n"           /* 4 */
        "\t\t\tStyle..OptionMenu..CouleurFond = 0\n"     /* 5 */
        "\t\tFIN\n"                                      /* 6 */
        "\tFIN\n"                                        /* 7 */
        "FIN\n";                                         /* 8 */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    /* Racine `Style` (ligne 3, `Style` col 3..7) -> styleMenuPrincipal. */
    JsonValue* r = handle_hover(make_hover_params(URI, 3, 5));
    const char* v = hover_value(r);
    ASSERT_HOVER_CONTAINS    (v, "styleMenuPrincipal", "Style racine -> styleMenuPrincipal");
    ASSERT_HOVER_NOT_CONTAINS(v, "styleConteneur",     "Style racine != styleConteneur");
    json_free(r);

    /* Surface `Barre` (ligne 3, apres `Style..` col 10..15). */
    r = handle_hover(make_hover_params(URI, 3, 12));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "styleMenuPrincipalBarre", "Barre -> styleMenuPrincipalBarre");
    json_free(r);

    /* Feuille `CouleurFond` (ligne 3, apres `Style..Barre..` col 17..). */
    r = handle_hover(make_hover_params(URI, 3, 20));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "styleMenuPrincipalBarre", "CouleurFond -> champ de styleMenuPrincipalBarre");
    json_free(r);

    /* Surface `SousMenu` (ligne 4, apres `Style..` col 10..18). */
    r = handle_hover(make_hover_params(URI, 4, 13));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "styleMenuPrincipalSousMenu", "SousMenu -> styleMenuPrincipalSousMenu");
    json_free(r);

    /* Feuille `Arrondi` (ligne 4, apres `Style..SousMenu..` col 20..) : NE DOIT
     * PAS resoudre sur la native Arrondi(). */
    r = handle_hover(make_hover_params(URI, 4, 22));
    v = hover_value(r);
    ASSERT_HOVER_NOT_CONTAINS(v, "Arrondit",                   "Arrondi menu != native Arrondi()");
    ASSERT_HOVER_CONTAINS    (v, "styleMenuPrincipalSousMenu", "Arrondi -> champ de styleMenuPrincipalSousMenu");
    json_free(r);

    /* Surface `OptionMenu` (ligne 5, apres `Style..` col 10..20). */
    r = handle_hover(make_hover_params(URI, 5, 14));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "styleMenuPrincipalOption", "OptionMenu -> styleMenuPrincipalOption");
    json_free(r);
}

/*  — variante MenuContextuel : `Style` racine -> styleMenuContextuel
 * (pas de surface Barre). + un smoke EN sur la racine. */
static void test_hover_menu_style_chain_contextuel(void) {
    printf("\n[TEST] feat/1214 : hover `Style..` menu-aware (MenuContextuel)\n");
    const char* src =
        "FEN_X est une Fenetre\n"                          /* 0 */
        "\tMNU_C est un MenuContextuel\n"                  /* 1 */
        "\t\tPROPRIETES\n"                                 /* 2 */
        "\t\t\tStyle..SousMenu..CouleurFond = 0\n"         /* 3 */
        "\t\t\tStyle..OptionMenu..Couleur = 0\n"           /* 4 */
        "\t\tFIN\n"                                        /* 5 */
        "\tFIN\n"                                          /* 6 */
        "FIN\n";                                           /* 7 */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    /* Racine `Style` (ligne 3, col 3..7) -> styleMenuContextuel. */
    JsonValue* r = handle_hover(make_hover_params(URI, 3, 5));
    const char* v = hover_value(r);
    ASSERT_HOVER_CONTAINS    (v, "styleMenuContextuel", "Style racine -> styleMenuContextuel");
    ASSERT_HOVER_NOT_CONTAINS(v, "styleConteneur",      "Style racine != styleConteneur");
    json_free(r);

    /* Surface `SousMenu` (ligne 3, col 10..18). */
    r = handle_hover(make_hover_params(URI, 3, 13));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "styleMenuPrincipalSousMenu", "SousMenu (contextuel) -> styleMenuPrincipalSousMenu");
    json_free(r);

    /* Smoke EN : racine `Style` sert le nom EN styleContextMenu. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    r = handle_hover(make_hover_params(URI, 3, 5));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "styleContextMenu", "Style racine EN -> styleContextMenu");
    json_free(r);
    lspServer.language[0] = '\0';
}

/*  — hover sur une propriete du bloc `est un Projet` (support #253).
 * F1 sur StartupWindow/FenetreDemarrage (et les autres proprietes de projet)
 * ne rendait aucune aide. Le hover doit resoudre le nom contre le catalogue de
 * champs du type Projet, en respectant l'invariant bilingue (descFR client FR,
 * descEN client EN) et l'accent/casse-insensibilite. Couvre les deux formes du
 * nom (FR FenetreDemarrage/FenêtreDémarrage, EN StartupWindow) + une 2e
 * propriete (Langues, Theme) pour prouver que c'est tout le bloc. */
static void test_hover_projet_property_en(void) {
    printf("\n[TEST] fix/1589 : hover propriete Projet (StartupWindow) EN -> desc EN\n");
    const char* src =
        "MonApp est un Projet\n"                        /* 0 */
        "\tPROPRIETES\n"                                /* 1 */
        "\t\tStartupWindow = FEN_Principal\n"           /* 2 */
        "\tFIN\n"                                       /* 3 */
        "FIN\n";                                        /* 4 */
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* `StartupWindow` ligne 2, col 2..15 -> curseur col 6. */
    JsonValue* r = handle_hover(make_hover_params(URI, 2, 6));
    const char* v = hover_value(r);
    ASSERT_HOVER_CONTAINS    (v, "Window opened when the project starts",
                             "StartupWindow EN -> desc EN");
    ASSERT_HOVER_CONTAINS    (v, "StartupWindow", "StartupWindow EN -> titre EN");
    ASSERT_HOVER_NOT_CONTAINS(v, "Fenêtre ouverte au lancement",
                             "StartupWindow EN -> pas la desc FR");
    json_free(r);
    lspServer.language[0] = '\0';
}

static void test_hover_projet_property_fr_default(void) {
    printf("\n[TEST] fix/1589 : hover propriete Projet (FenetreDemarrage) FR -> desc FR\n");
    /* Forme FR sans accent (toleree, canon = FenêtreDémarrage). */
    const char* srcAscii =
        "MonApp est un Projet\n"                        /* 0 */
        "\tPROPRIETES\n"                                /* 1 */
        "\t\tFenetreDemarrage = FEN_Principal\n"        /* 2 */
        "\tFIN\n"                                       /* 3 */
        "FIN\n";                                        /* 4 */
    setup_doc(URI, srcAscii);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* r = handle_hover(make_hover_params(URI, 2, 6));
    const char* v = hover_value(r);
    ASSERT_HOVER_CONTAINS    (v, "Fenêtre ouverte au lancement du projet",
                             "FenetreDemarrage FR -> desc FR");
    ASSERT_HOVER_NOT_CONTAINS(v, "Window opened when",
                             "FenetreDemarrage FR -> pas la desc EN");
    json_free(r);

    /* Forme FR accentuee canon FenêtreDémarrage : meme resultat. */
    const char* srcAccent =
        "MonApp est un Projet\n"                        /* 0 */
        "\tPROPRIETES\n"                                /* 1 */
        "\t\tFenêtreDémarrage = FEN_Principal\n"        /* 2 */
        "\tFIN\n"                                       /* 3 */
        "FIN\n";                                        /* 4 */
    setup_doc(URI, srcAccent);
    /* `Fenêtre...` : les 2 tabs = 2 chars, curseur en col 5 (dans le mot). */
    r = handle_hover(make_hover_params(URI, 2, 5));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "Fenêtre ouverte au lancement du projet",
                          "FenêtreDémarrage (accent) FR -> desc FR");
    json_free(r);
}

/* 2e propriete pour prouver que c'est tout le bloc, pas un cas isole. */
static void test_hover_projet_property_second(void) {
    printf("\n[TEST] fix/1589 : hover 2e propriete Projet (Langues/Theme)\n");
    const char* src =
        "MonApp est un Projet\n"                        /* 0 */
        "\tPROPRIETES\n"                                /* 1 */
        "\t\tLangues = [langueFrançais]\n"              /* 2 */
        "\t\tTheme = MonTheme\n"                        /* 3 */
        "\tFIN\n"                                       /* 4 */
        "FIN\n";                                        /* 5 */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    /* `Langues` ligne 2, col 2..9 -> curseur col 5. */
    JsonValue* r = handle_hover(make_hover_params(URI, 2, 5));
    const char* v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "Langues du projet", "Langues -> desc FR");
    json_free(r);

    /* `Theme` ligne 3, col 2..7 -> curseur col 4. */
    r = handle_hover(make_hover_params(URI, 3, 4));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS(v, "appliqué au projet", "Theme -> desc FR");
    json_free(r);
}

/*  — recidive support #253 : bloc projet ecrit ENTIEREMENT en anglais
 * (`is a Project` + `PROPERTIES`), client EN. Les gardes  utilisaient un
 * bloc FR (`est un Projet` + `PROPRIETES`) avec seulement un NOM de propriete EN,
 * ne couvrant jamais le scenario d'un utilisateur 100% anglophone. Ce test
 * verrouille la parite : le hover doit resoudre la propriete et rendre sa desc EN
 * dans un bloc entierement EN. */
static void test_hover_projet_property_full_en(void) {
    printf("\n[TEST] fix/1775 : hover propriete Projet bloc 100%% EN -> desc EN\n");
    const char* src =
        "MyApp is a Project\n"                          /* 0 */
        "\tPROPERTIES\n"                                /* 1 */
        "\t\tStartupWindow = FEN_Main\n"                /* 2 */
        "\t\tLanguages = [languageEnglish]\n"           /* 3 */
        "\tEND\n"                                       /* 4 */
        "END\n";                                        /* 5 */
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* `StartupWindow` ligne 2, col 2..15 -> curseur col 6. */
    JsonValue* r = handle_hover(make_hover_params(URI, 2, 6));
    const char* v = hover_value(r);
    ASSERT_HOVER_CONTAINS    (v, "Window opened when the project starts",
                             "bloc EN StartupWindow -> desc EN");
    ASSERT_HOVER_CONTAINS    (v, "StartupWindow", "bloc EN StartupWindow -> titre EN");
    ASSERT_HOVER_NOT_CONTAINS(v, "Fenêtre ouverte au lancement",
                             "bloc EN StartupWindow -> pas la desc FR");
    json_free(r);

    /* `Languages` ligne 3, col 2..11 -> curseur col 5. */
    r = handle_hover(make_hover_params(URI, 3, 5));
    v = hover_value(r);
    ASSERT_HOVER_CONTAINS    (v, "Project languages",
                             "bloc EN Languages -> desc EN");
    ASSERT_HOVER_NOT_CONTAINS(v, "Langues du projet",
                             "bloc EN Languages -> pas la desc FR");
    json_free(r);
    lspServer.language[0] = '\0';
}

/* Hover EN sur une methode d'extension : label "Method on", pas "Methode sur". */
static void test_hover_method_en(void) {
    printf("\n[TEST] feat/0663 : hover EN sur methode -> 'Method on'\n");
    const char* src =
        "s est une cha\xc3\xaene\n"
        "x = s.Majuscule()\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 1, 7);  /* sur 'Majuscule' apres '.' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Method on",   "hover method EN label");
    ASSERT_HOVER_NOT_CONTAINS(v, "thode sur",   "hover method EN no FR label");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme methode en mode FR garde 'Methode sur'. */
static void test_hover_method_fr_default(void) {
    printf("\n[TEST] feat/0663 : hover FR (defaut) sur methode -> 'Methode sur'\n");
    const char* src =
        "s est une cha\xc3\xaene\n"
        "x = s.Majuscule()\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 7);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "thode sur", "hover method FR label");
    json_free(result);
    json_free(params);
}

/*  — hover EN sur une valeur d'enum : la liste « other values »
 * doit lister les noms EN des valeurs soeurs (dispHorizontal/dispGrid/
 * dispFree), pas les noms FR (dispHorizontale/dispGrille/dispLibre).
 * RED-FIRST : avant impl, la boucle « other values » emet *v (nom FR). */
static void test_hover_enum_sibling_values_en(void) {
    printf("\n[TEST] fix/0675 : hover EN sur valeur enum -> soeurs en EN\n");
    const char* src = "x = dispVerticale\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 0, 8);  /* sur 'dispVerticale' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "other values",  "hover enum EN label");
    ASSERT_HOVER_CONTAINS    (v, "dispHorizontal","hover enum sibling EN dispHorizontal");
    ASSERT_HOVER_CONTAINS    (v, "dispGrid",      "hover enum sibling EN dispGrid");
    ASSERT_HOVER_CONTAINS    (v, "dispFree",      "hover enum sibling EN dispFree");
    /* Les noms FR ne doivent plus apparaitre comme valeurs soeurs. Comme
     * "dispHorizontal" est un prefixe de "dispHorizontale", on cible la
     * forme FR exacte entre backticks (`dispGrille`/`dispLibre` sont
     * disjoints des EN, suffisants pour prouver la bascule). */
    ASSERT_HOVER_NOT_CONTAINS(v, "dispGrille",    "hover enum EN no FR dispGrille");
    ASSERT_HOVER_NOT_CONTAINS(v, "dispLibre",     "hover enum EN no FR dispLibre");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme valeur d'enum en mode FR (defaut) garde les noms
 * FR des valeurs soeurs. */
static void test_hover_enum_sibling_values_fr_default(void) {
    printf("\n[TEST] fix/0675 : hover FR (defaut) sur valeur enum -> soeurs en FR\n");
    const char* src = "x = dispVerticale\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 0, 8);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "autres valeurs", "hover enum FR label");
    ASSERT_HOVER_CONTAINS    (v, "dispGrille",     "hover enum sibling FR dispGrille");
    ASSERT_HOVER_CONTAINS    (v, "dispLibre",      "hover enum sibling FR dispLibre");
    ASSERT_HOVER_NOT_CONTAINS(v, "dispGrid`",      "hover enum FR no EN dispGrid");
    ASSERT_HOVER_NOT_CONTAINS(v, "dispFree",       "hover enum FR no EN dispFree");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — Completion EN sur constantes + types
 *
 *  En mode language=en, la completion doit servir l'anglais pour les
 *  constantes (nameEN) et les types (nameEN), comme elle le fait deja
 *  pour les natives. Fallback FR si pas de forme EN. Le mode FR (defaut)
 *  ne regresse pas.
 *
 *  RED-FIRST : avant impl, les boucles const/type emettent c->name/t->name
 *  sans brancher sur wantEN -> en mode EN le label reste FR -> ces tests
 *  rougissent. Apres impl -> verts.
 * ================================================================ */

/* Completion EN : une constante avec nameEN sort en EN (sizeGB), pas en FR. */
static void test_completion_const_en(void) {
    printf("\n[TEST] fix/0665 : completion EN sur constante -> nameEN (sizeGB)\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "sizeGB",   "completion const EN label");
    ASSERT_NOT_HAS(items, "tailleGo", "completion const EN no FR label");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme constante en mode FR (defaut) garde la forme FR. */
static void test_completion_const_fr_default(void) {
    printf("\n[TEST] fix/0665 : completion FR (defaut) sur constante -> tailleGo\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "tailleGo", "completion const FR label");
    ASSERT_NOT_HAS(items, "sizeGB",   "completion const FR no EN label");
    json_free(items);
    json_free(params);
}

/* Completion EN : un type avec nameEN sort en EN (httpRequest), pas en FR. */
static void test_completion_type_en(void) {
    printf("\n[TEST] fix/0665 : completion EN sur type -> nameEN (httpRequest)\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "httpRequest",          "completion type EN label");
    ASSERT_NOT_HAS(items, "httpRequ\xc3\xaate",   "completion type EN no FR label");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme type en mode FR (defaut) garde la forme FR canon. */
static void test_completion_type_fr_default(void) {
    printf("\n[TEST] fix/0665 : completion FR (defaut) sur type -> httpRequete canon\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "httpRequ\xc3\xaate", "completion type FR label");
    ASSERT_NOT_HAS(items, "httpRequest",        "completion type FR no EN label");
    json_free(items);
    json_free(params);
}

/* Fallback : une constante sans forme EN distincte (nameEN == name) garde
 * son label FR meme en mode EN (pas de vide). */
static void test_completion_const_en_fallback(void) {
    printf("\n[TEST] fix/0665 : completion EN sur const sans nameEN -> fallback FR\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    /* ccNormal a nameEN == name -> hasEN faux -> fallback FR, present en EN. */
    ASSERT_HAS(items, "ccNormal", "completion const EN fallback");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *   — La completion de type ne propose plus les types NON
 *  declarables. `variant` a ete retiré du langage, `octet` n'est pas
 *  un type de declaration valide : tous deux etaient encore listes
 *  dans OWL_PRIM_TYPES (vestige). On verrouille leur absence dans les
 *  deux contextes d'emission (dump general + type-declaration) et dans
 *  les deux langues, avec non-regression sur les primitifs restants.
 * ================================================================ */

/* Contexte general (`x = <cursor>`) : ni variant ni octet, FR et EN,
 * mais les primitifs valides restent (entier). */
static void test_completion_no_variant_octet_general(void) {
    printf("\n[TEST] fix/0827 : dump general ne propose plus variant/octet\n");
    const char* src = "x = \n";
    setup_doc(URI, src);

    /* FR (defaut). */
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "variant", "general FR : pas de variant");
    ASSERT_NOT_HAS(items, "octet",   "general FR : pas de octet");
    ASSERT_HAS    (items, "entier",  "general FR : entier reste (non-regression)");
    json_free(items);
    json_free(params);

    /* EN : variant/octet n'ont pas de forme EN distincte, le label
     * retomberait sur "variant"/"octet" — doit rester absent. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 0, 4);
    items = handle_completion(params);
    ASSERT_NOT_HAS(items, "variant", "general EN : pas de variant");
    ASSERT_NOT_HAS(items, "octet",   "general EN : pas de octet");
    ASSERT_HAS    (items, "integer", "general EN : integer reste (non-regression)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Contexte type-declaration (`v est un <cursor>`) : ni variant ni
 * octet, FR et EN, primitifs valides toujours proposes. */
static void test_completion_no_variant_octet_typedecl(void) {
    printf("\n[TEST] fix/0827 : contexte type-declaration ne propose plus variant/octet\n");
    const char* src = "v est un \n";  /* curseur apres l'article */
    setup_doc(URI, src);

    /* FR (defaut). Curseur col 9 = apres "v est un ". */
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "variant",   "typedecl FR : pas de variant");
    ASSERT_NOT_HAS(items, "octet",     "typedecl FR : pas de octet");
    ASSERT_HAS    (items, "cha\xc3\xaene", "typedecl FR : chaine reste (non-regression)");
    json_free(items);
    json_free(params);

    /* EN. */
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 0, 9);
    items = handle_completion(params);
    ASSERT_NOT_HAS(items, "variant", "typedecl EN : pas de variant");
    ASSERT_NOT_HAS(items, "octet",   "typedecl EN : pas de octet");
    ASSERT_HAS    (items, "string",  "typedecl EN : string reste (non-regression)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  — types DSL top-level (Fenetre/Etat) emis en completion de
 * type avec snippet auto-fermant (bloc + FIN/END). Support #120 : apres
 * `X est une <curseur>`, proposer Fenetre/Etat avec la fermeture auto
 * comme PROCEDURE/CONSTANTE. RED avant impl : les deux labels sont absents
 * du contexte typeDecl (seuls primitifs + types builtin registres + structs
 * user y sont emis). Forme EN canon derivee de ui_decl.c CONTROL_KINDS
 * (Fenetre->Window, Etat->Report). */
static void test_completion_typedecl_fenetre_etat_snippet(void) {
    printf("\n[TEST] feat/0994 : typedecl propose Fenetre/Etat avec snippet auto-FIN\n");

    /* --- FR (defaut). Curseur apres `X est une ` (article present). --- */
    const char* srcFR = "X est une \n";  /* 10 cars avant le curseur col 10 */
    setup_doc(URI, srcFR);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Fen\xc3\xaatre", "typedecl FR : Fenetre propose");
    ASSERT_HAS(items, "\xc3\x89tat",   "typedecl FR : Etat propose (canon accentue)");
    ASSERT_INSERT_CONTAINS(items, "Fen\xc3\xaatre", "PROPRIETES", "snippet Fenetre PROPRIETES");
    ASSERT_INSERT_CONTAINS(items, "Fen\xc3\xaatre", "FIN",        "snippet Fenetre FIN");
    ASSERT_INSERT_CONTAINS(items, "\xc3\x89tat", "est un Corps",  "snippet Etat bloc Corps");
    ASSERT_INSERT_CONTAINS(items, "\xc3\x89tat", "FIN",           "snippet Etat FIN");
    /*  — le snippet insere doit etre deja canon : sinon le formateur
     * recanonise a l'enregistrement ce que la completion vient d'inserer. */
    ASSERT_INSERT_CONTAINS(items, "\xc3\x89tat", "\xc3\x89tat\n", "snippet Etat ouvre sur le canon accentue");
    ASSERT_INSERT_NOT_CONTAINS(items, "Fen\xc3\xaatre", "PROPERTIES", "snippet Fenetre FR sans PROPERTIES EN");
    ASSERT_INSERT_FORMAT_SNIPPET(items, "Fen\xc3\xaatre", "Fenetre insertTextFormat snippet");
    ASSERT_INSERT_FORMAT_SNIPPET(items, "\xc3\x89tat",     "Etat insertTextFormat snippet");
    json_free(items);
    json_free(params);

    /* --- EN. Curseur apres `X is a ` (article present). --- */
    const char* srcEN = "X is a \n";  /* 7 cars avant le curseur col 7 */
    setup_doc(URI, srcEN);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    params = make_completion_params(URI, 0, 7);
    items = handle_completion(params);
    ASSERT_HAS(items, "Window", "typedecl EN : Window propose");
    ASSERT_HAS(items, "Report", "typedecl EN : Report propose");
    ASSERT_INSERT_CONTAINS(items, "Window", "PROPERTIES",   "snippet Window PROPERTIES EN");
    ASSERT_INSERT_CONTAINS(items, "Window", "END",          "snippet Window END EN");
    ASSERT_INSERT_CONTAINS(items, "Report", "is a Body",     "snippet Report bloc Body EN");
    ASSERT_INSERT_CONTAINS(items, "Report", "END",          "snippet Report END EN");
    ASSERT_INSERT_NOT_CONTAINS(items, "Window", "PROPRIETES", "snippet Window EN sans PROPRIETES FR");
    ASSERT_INSERT_NOT_CONTAINS(items, "Window", "FIN",        "snippet Window EN sans FIN");
    ASSERT_INSERT_FORMAT_SNIPPET(items, "Window", "Window insertTextFormat snippet");
    ASSERT_INSERT_FORMAT_SNIPPET(items, "Report", "Report insertTextFormat snippet");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  (support #251) — widgets proposes apres `est un Champ <cursor>`
 * (controlModifierContext, ) n'ont AUCUN snippet auto-fermant : label
 * seul, sans insertTextFormat=2 ni corps PROPRIETES/FIN. RED avant impl —
 * verrouille le squelette leger (PROPRIETES + FIN, comme Fenetre/Etat) pour
 * les widgets les plus courants. */
static void test_champ_modifier_types_snippet_auto_fin(void) {
    printf("\n[TEST] feat/1588 : `est un Champ <cursor>` propose un snippet auto-FIN par widget\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tSAI_X est un Champ \n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 1, 20);
    JsonValue* items = handle_completion(params);

    static const char* widgets[] = {
        "Bouton", "Saisie", "Table", "Conteneur", "Image", "Liste", "Combo", NULL
    };
    for (int i = 0; widgets[i]; i++) {
        ASSERT_HAS(items, widgets[i], "feat/1588 widget propose (non-regression fix/1151)");
        ASSERT_INSERT_CONTAINS(items, widgets[i], "PROPRIETES",
            "feat/1588 snippet widget contient PROPRIETES");
        ASSERT_INSERT_CONTAINS(items, widgets[i], "FIN",
            "feat/1588 snippet widget contient FIN");
        ASSERT_INSERT_FORMAT_SNIPPET(items, widgets[i],
            "feat/1588 widget insertTextFormat=2 (snippet)");
    }
    json_free(items);
    json_free(params);
}

/*  — les ouvreurs de bloc MENU proposes en position typeDecl
 * (`MNU_X est un <cursor>`, ) n'ont pas de snippet auto-FIN. Un menu
 * accepte un bloc PROPRIETES (cf. tests Style..OptionMenu.. plus haut) : le
 * squelette leger attendu est identique a Fenetre (PROPRIETES + FIN). RED
 * avant impl. */
static void test_menu_kind_types_snippet_auto_fin(void) {
    printf("\n[TEST] feat/1588 : `est un <cursor>` (blocs menu) propose un snippet auto-FIN\n");
    const char* src = "MNU_X est un \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);

    static const char* menus[] = {
        "MenuPrincipal", "SousMenu", "OptionMenu", "MenuS\xc3\xa9parateur", NULL
    };
    for (int i = 0; menus[i]; i++) {
        ASSERT_HAS(items, menus[i], "feat/1588 menu propose (non-regression feat/1214)");
        ASSERT_INSERT_CONTAINS(items, menus[i], "PROPRIETES",
            "feat/1588 snippet menu contient PROPRIETES");
        ASSERT_INSERT_CONTAINS(items, menus[i], "FIN",
            "feat/1588 snippet menu contient FIN");
        ASSERT_INSERT_FORMAT_SNIPPET(items, menus[i],
            "feat/1588 menu insertTextFormat=2 (snippet)");
    }
    json_free(items);
    json_free(params);
}

/*  — sous-blocs d'un Etat proposes en position typeDecl
 * (`INTRO est un <cursor>`,  L4) n'ont pas de snippet auto-FIN. Ces
 * sous-blocs acceptent un PROPRIETES (cf.  : `PROPRIETES Hauteur=30
 * FIN`) : squelette leger attendu identique aux autres ouvreurs de bloc. RED
 * avant impl. */
static void test_completion_etat_subblocks_snippet_auto_fin(void) {
    printf("\n[TEST] feat/1588 : sous-blocs Etat proposent un snippet auto-FIN\n");
    const char* src =
        "ETAT_X est un Etat\n"
        "\tINTRO est un \n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 1, 14);
    JsonValue* items = handle_completion(params);

    static const char* subblocks[] = {
        "Corps", "EnT\xc3\xaateDocument", "BasDePage", NULL
    };
    for (int i = 0; subblocks[i]; i++) {
        ASSERT_HAS(items, subblocks[i], "feat/1588 sous-bloc Etat propose (non-regression fix/1341)");
        ASSERT_INSERT_CONTAINS(items, subblocks[i], "PROPRIETES",
            "feat/1588 snippet sous-bloc Etat contient PROPRIETES");
        ASSERT_INSERT_CONTAINS(items, subblocks[i], "FIN",
            "feat/1588 snippet sous-bloc Etat contient FIN");
        ASSERT_INSERT_FORMAT_SNIPPET(items, subblocks[i],
            "feat/1588 sous-bloc Etat insertTextFormat=2 (snippet)");
    }
    json_free(items);
    json_free(params);
}

/*  — le mot-cle QUAND (ouvreur de la forme generale du handler
 * d'exception `QUAND EXCEPTION ... FIN`, cf. docs/public_documentation/
 * 08_exceptions.md) est present dans la table de mots-cles (kw[]) mais absent
 * de KW_SNIPPETS : aucun corps ni FIN insere. RED avant impl. Note : le
 * mot-cle EXCEPTION seul (re-levee dans un handler) ne rouvre pas de bloc et
 * n'a donc pas de FIN a verrouiller ici (cf. doc ligne 353). */
static void test_completion_quand_snippet_auto_fin(void) {
    printf("\n[TEST] feat/1588 : mot-cle QUAND propose un snippet auto-FIN (QUAND EXCEPTION ... FIN)\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "QUAND", "feat/1588 QUAND propose (non-regression)");
    ASSERT_INSERT_CONTAINS(items, "QUAND", "EXCEPTION",
        "feat/1588 snippet QUAND contient EXCEPTION");
    ASSERT_INSERT_CONTAINS(items, "QUAND", "FIN",
        "feat/1588 snippet QUAND contient FIN");
    ASSERT_INSERT_FORMAT_SNIPPET(items, "QUAND", "feat/1588 QUAND insertTextFormat=2 (snippet)");
    json_free(items);
    json_free(params);
}

/*  — le type conteneur Thème (compagnon namespace de styles nommes,
 * ) n'a pas d'entree dans DSL_TYPE_SNIPPETS : contrairement a
 * Fenetre/Etat/Structure/Classe, il n'est meme pas propose comme candidat
 * typeDecl aujourd'hui (aucune emission de "Thème" trouvee dans
 * lsp_handlers.c). RED avant impl, pour la meme raison structurelle que les
 * autres cas : le squelette leger attendu est un bloc auto-fermant (FIN),
 * sans PROPRIETES (un Thème ne porte pas de PROPRIETES, son corps declare
 * directement des styles nommes `nom est un styleXxx = {...}`). */
static void test_completion_typedecl_theme_snippet_auto_fin(void) {
    printf("\n[TEST] feat/1588 : typedecl `X est un <cursor>` propose Th\xc3\xa8me avec snippet auto-FIN\n");
    const char* src = "X est un \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Th\xc3\xa8me", "feat/1588 typedecl Theme propose");
    ASSERT_INSERT_CONTAINS(items, "Th\xc3\xa8me", "FIN", "feat/1588 snippet Theme contient FIN");
    ASSERT_INSERT_FORMAT_SNIPPET(items, "Th\xc3\xa8me", "feat/1588 Theme insertTextFormat=2 (snippet)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — Completion typeDecl EN des types composes du registre
 *
 *  En mode language=en, contexte typeDecl (`X is a `), les types
 *  composes du registre (chaineMultilingue, httpRequete…) doivent etre
 *  proposes avec leur label EN (multilingualString) et non le label FR.
 *  RED avant fix : la boucle "Builtin registered types" emet t->name
 *  (FR) sans bascule wantEN. Symetrie FR : chaineMultilingue en locale fr.
 * ================================================================ */
static void test_completion_typedecl_composed_type_en(void) {
    printf("\n[TEST] fix/1032 : typedecl EN propose multilingualString (nameEN)\n");

    /* --- EN. Curseur apres `X is a `. --- */
    const char* srcEN = "X is a \n";  /* col 7 en fin de ligne */
    setup_doc(URI, srcEN);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 0, 7);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "multilingualString", "typedecl EN : label nameEN propose");
    ASSERT_NOT_HAS(items, "cha\xc3\xaeneMultilingue", "typedecl EN : pas de label FR");
    json_free(items);
    json_free(params);

    /* --- FR (defaut). Curseur apres `X est une `. --- */
    const char* srcFR = "X est une \n";  /* col 10 */
    setup_doc(URI, srcFR);
    lspServer.language[0] = '\0';
    params = make_completion_params(URI, 0, 10);
    items = handle_completion(params);
    ASSERT_HAS    (items, "cha\xc3\xaeneMultilingue", "typedecl FR : label FR propose");
    ASSERT_NOT_HAS(items, "multilingualString", "typedecl FR : pas de label EN");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *   — Completion EN des mots-cles (labels + snippets)
 *
 *  En mode language=en, la completion des mots-cles doit servir la
 *  forme EN canon (IF/PROPERTIES/FOR...) pour le label ET pour le corps
 *  du snippet block-opener (IF...THEN...END, FOR...TO...END). Le mode FR
 *  (defaut) ne regresse pas (SI/PROPRIETES, SI...ALORS...FIN).
 *
 *  Deux trous corriges :
 *   - Trou 1 : PROPRIETES (hors keyword catalog, DSL Fenetre) n'avait pas
 *     de forme EN -> label restait FR en locale EN. keyword_en_form est
 *     decouple du catalog (resolution token via le scanner).
 *   - Trou 2 : les snippets KW_SNIPPETS etaient toujours FR.
 *
 *  RED-FIRST : avant impl, PROPERTIES absent en EN, et insertText reste
 *  FR (SI...ALORS...FIN) meme avec label IF -> ces tests rougissent.
 * ================================================================ */

/* Completion EN : labels mots-cles en forme EN (IF, PROPERTIES). */
static void test_completion_keyword_label_en(void) {
    printf("\n[TEST] fix/0768 : completion EN mots-cles -> IF / PROPERTIES\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    /* Trou 1 : PROPRIETES (hors catalog) traduit en PROPERTIES. */
    ASSERT_HAS    (items, "PROPERTIES",  "kw EN label (PROPRIETES->PROPERTIES)");
    ASSERT_NOT_HAS(items, "PROPRIETES",  "kw EN no FR (PROPRIETES)");
    /* Mot-cle catalog deja gere par , on verrouille la non-regression. */
    ASSERT_HAS    (items, "IF",          "kw EN label (SI->IF)");
    ASSERT_NOT_HAS(items, "SI",          "kw EN no FR (SI)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : labels mots-cles FR (defaut) preserves. */
static void test_completion_keyword_label_fr_default(void) {
    printf("\n[TEST] fix/0768 : completion FR (defaut) mots-cles -> SI / PROPRIETES\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "PROPRIETES",  "kw FR label (PROPRIETES)");
    ASSERT_NOT_HAS(items, "PROPERTIES",  "kw FR no EN (PROPERTIES)");
    ASSERT_HAS    (items, "SI",          "kw FR label (SI)");
    ASSERT_NOT_HAS(items, "IF",          "kw FR no EN (IF)");
    json_free(items);
    json_free(params);
}

/*  (support #431) — `PROPRIETE` (singulier) n'est PAS un mot-cle canon :
 * le scanner ne connait que `PROPRIETES`/`PROPERTIES` (TOKEN_PROPRIETES). Le
 * singulier trainait dans la liste kw[] de la completion (artefact), sans forme
 * EN -> en locale EN il tombait en fallback FR et polluait la liste (propose
 * avant PROPERTIES en tapant `prop`). On exige son retrait de la completion,
 * dans les deux locales ; le pluriel canon reste (PROPERTIES en EN, PROPRIETES
 * en FR). */
static void test_completion_propriete_singular_removed(void) {
    printf("\n[TEST] fix/2281 : PROPRIETE singulier (artefact) retire de la completion\n");
    const char* src = "x = \n";

    /* --- EN : PROPERTIES canon present, PROPRIETE singulier absent. --- */
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "PROPERTIES", "fix/2281 EN PROPERTIES canon present");
    ASSERT_NOT_HAS(items, "PROPRIETE",  "fix/2281 EN PROPRIETE singulier (artefact) absent");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';

    /* --- FR : PROPRIETES canon present, PROPRIETE singulier absent. --- */
    setup_doc(URI, src);
    params = make_completion_params(URI, 0, 4);
    items = handle_completion(params);
    ASSERT_HAS    (items, "PROPRIETES", "fix/2281 FR PROPRIETES canon present");
    ASSERT_NOT_HAS(items, "PROPRIETE",  "fix/2281 FR PROPRIETE singulier (artefact) absent");
    json_free(items);
    json_free(params);
}

/* Completion EN : snippets block-opener en EN canon (corps traduit). */
static void test_completion_keyword_snippet_en(void) {
    printf("\n[TEST] fix/0768 : completion EN snippets -> IF...THEN...END\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    /* SI -> IF : corps IF ... THEN ... END, plus aucun FR. */
    ASSERT_INSERT_CONTAINS    (items, "IF", "IF ${1:condition} THEN", "snippet IF en-tete");
    ASSERT_INSERT_CONTAINS    (items, "IF", "END",   "snippet IF END");
    ASSERT_INSERT_NOT_CONTAINS(items, "IF", "ALORS", "snippet IF sans ALORS");
    ASSERT_INSERT_NOT_CONTAINS(items, "IF", "FIN",   "snippet IF sans FIN");
    /* FOR -> corps FOR ... TO ... END (le `A` FR devient `TO`). */
    ASSERT_INSERT_CONTAINS    (items, "FOR", "TO ${3:N}", "snippet FOR borne TO");
    ASSERT_INSERT_NOT_CONTAINS(items, "FOR", " A ",       "snippet FOR sans ` A `");
    /* PROPERTIES (trou 1+2) : bloc PROPERTIES ... END. */
    ASSERT_INSERT_CONTAINS    (items, "PROPERTIES", "PROPERTIES\n\t$0\nEND",
                              "snippet PROPERTIES EN");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : snippets FR (defaut) preserves. */
static void test_completion_keyword_snippet_fr_default(void) {
    printf("\n[TEST] fix/0768 : completion FR (defaut) snippets -> SI...ALORS...FIN\n");
    const char* src = "x = \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "SI", "SI ${1:condition} ALORS", "snippet SI en-tete");
    ASSERT_INSERT_CONTAINS    (items, "SI", "FIN",  "snippet SI FIN");
    ASSERT_INSERT_NOT_CONTAINS(items, "SI", "THEN", "snippet SI sans THEN");
    ASSERT_INSERT_CONTAINS    (items, "POUR", "A ${3:N}",  "snippet POUR borne A");
    ASSERT_INSERT_CONTAINS    (items, "PROPRIETES", "PROPRIETES\n\t$0\nFIN",
                              "snippet PROPRIETES FR");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — Completion EN des props DSL + valeurs d'enum
 *
 *  En mode language=en, dans une fenetre EN (`is a Window`, contexte
 *  detecte grace a 0688), la completion doit servir les NOMS de props
 *  en EN (Layout, Width...) et, apres `Layout = `, les VALEURS d'enum
 *  en EN (dispFree...). Fallback FR si pas de forme EN. Le mode FR
 *  (defaut) ne regresse pas.
 *
 *  RED-FIRST : avant impl, la boucle props emet h->name (FR) sans
 *  brancher sur wantEN, et la boucle valeurs emet ei->values (FR) ->
 *  en mode EN aucun label EN -> ces tests rougissent. Apres impl ->
 *  verts. La forme EN d'une valeur d'enum est resolue via le
 *  const-catalog (nameEN du membre), comme le hover  — PAS
 *  de champ valuesEN sur OwlEnum.
 * ================================================================ */

/* Completion EN : noms de props d'une fenetre EN -> EN (Layout, Width). */
static void test_completion_props_window_en(void) {
    printf("\n[TEST] fix/0690 : completion EN props Window -> Layout/Width\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROPERTIES\n"
        "\t\t\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    /* Labels EN proposes (nameEN), pas les FR canon. */
    ASSERT_HAS    (items, "Layout",      "completion props Window EN (Disposition->Layout)");
    ASSERT_HAS    (items, "Width",       "completion props Window EN (Largeur->Width)");
    ASSERT_NOT_HAS(items, "Disposition", "completion props Window EN no FR (Disposition)");
    ASSERT_NOT_HAS(items, "Largeur",     "completion props Window EN no FR (Largeur)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme fenetre en FR (defaut) garde les labels FR canon. */
static void test_completion_props_window_fr_default(void) {
    printf("\n[TEST] fix/0690 : completion FR (defaut) props Fenetre -> Disposition/Largeur\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Disposition", "completion props Fenetre FR (Disposition)");
    ASSERT_HAS    (items, "Largeur",     "completion props Fenetre FR (Largeur)");
    ASSERT_NOT_HAS(items, "Layout",      "completion props Fenetre FR no EN (Layout)");
    ASSERT_NOT_HAS(items, "Width",       "completion props Fenetre FR no EN (Width)");
    json_free(items);
    json_free(params);
}

/* Completion EN : valeurs d'enum apres `Layout = ` -> EN (dispFree). */
static void test_completion_enum_values_window_en(void) {
    printf("\n[TEST] fix/0690 : completion EN valeurs enum 'Layout = ' -> dispFree\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROPERTIES\n"
        "\t\tLayout = \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* Cursor ligne 2 (0-indexed) apres 'Layout = ' (2 tabs + 9 chars). */
    JsonValue* params = make_completion_params(URI, 2, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "dispFree",  "completion enum val EN (dispLibre->dispFree)");
    ASSERT_NOT_HAS(items, "dispLibre", "completion enum val EN no FR (dispLibre)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : valeurs d'enum en FR (defaut) gardent la forme FR. */
static void test_completion_enum_values_window_fr_default(void) {
    printf("\n[TEST] fix/0690 : completion FR (defaut) valeurs enum 'Disposition = ' -> dispLibre\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    /* Cursor ligne 2 apres 'Disposition = ' (2 tabs + 14 chars). */
    JsonValue* params = make_completion_params(URI, 2, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "dispLibre", "completion enum val FR (dispLibre)");
    ASSERT_NOT_HAS(items, "dispFree",  "completion enum val FR no EN (dispFree)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — Completion EN des VALEURS d'enum hors DSL-PROPRIETES
 *
 *   n'a bilingualise que le contexte DSL-PROPRIETES. Cinq
 *  autres contextes de completion enum-value restaient FR-only :
 *  composed-type field, propValue fieldBase/Opts, simple-assign
 *  (var typee enum = ), POUR TOUTE...AVEC, et call-arg. En mode
 *  language=en ils doivent servir le nameEN du membre (resolu au
 *  const-catalog, comme ) + detail EN ("Value of"/"Member of"/
 *  "Combinable option of"). Fallback FR si pas de nameEN.
 *
 *  RED-FIRST : avant impl, ces boucles emettent *v (FR) + detail FR
 *  sans brancher sur wantEN -> en mode EN aucun label EN -> rouge.
 *  Apres impl -> vert.
 * ================================================================ */

/* call-arg : TestSimuleSouris(champ, ) — l'arg 2 (action) a un
 * allowedBaseEnum 'actionSouris' sur la signature primaire.
 * En EN -> LeftClick + detail EN "Member of". */
static void test_completion_enum_callarg_en(void) {
    printf("\n[TEST] fix/0696 : completion EN call-arg enum -> LeftClick\n");
    const char* src = "TestSimuleSouris(BTN_X, )\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* Curseur juste apres la virgule (arg index 1). */
    int comma1 = 0;
    {
        const char* lineEnd = strchr(src, '\n');
        const char* p = src;
        while (p < lineEnd && *p != ',') p++;
        comma1 = (int)((p + 2) - src);  /* +2 : virgule + espace */
    }
    JsonValue* params = make_completion_params(URI, 0, comma1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "LeftClick",  "completion call-arg EN (ClicGauche->LeftClick)");
    ASSERT_NOT_HAS(items, "ClicGauche", "completion call-arg EN no FR (ClicGauche)");
    ASSERT_DETAIL_PREFIX(items, "LeftClick", "Member of",
                         "completion call-arg EN detail");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme call-arg en FR (defaut) garde la forme FR. */
static void test_completion_enum_callarg_fr_default(void) {
    printf("\n[TEST] fix/0696 : completion FR (defaut) call-arg enum -> ClicGauche\n");
    const char* src = "TestSimuleSouris(BTN_X, )\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    int comma1 = 0;
    {
        const char* lineEnd = strchr(src, '\n');
        const char* p = src;
        while (p < lineEnd && *p != ',') p++;
        comma1 = (int)((p + 2) - src);
    }
    JsonValue* params = make_completion_params(URI, 0, comma1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "ClicGauche", "completion call-arg FR (ClicGauche)");
    ASSERT_NOT_HAS(items, "LeftClick",  "completion call-arg FR no EN (LeftClick)");
    ASSERT_DETAIL_PREFIX(items, "ClicGauche", "Membre de",
                         "completion call-arg FR detail");
    json_free(items);
    json_free(params);
}

/* simple-assign : var typee 'disposition' puis 'var = ' -> valeurs
 * de l'enum registry. En EN -> dispFree + detail EN "Value of". */
static void test_completion_enum_simpleassign_en(void) {
    printf("\n[TEST] fix/0696 : completion EN simple-assign enum -> dispFree\n");
    const char* src =
        "d est un disposition\n"
        "d = \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 1, 4);  /* apres 'd = ' */
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "dispFree",  "completion simple-assign EN (dispLibre->dispFree)");
    ASSERT_NOT_HAS(items, "dispLibre", "completion simple-assign EN no FR (dispLibre)");
    ASSERT_DETAIL_PREFIX(items, "dispFree", "Value of",
                         "completion simple-assign EN detail");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme simple-assign en FR (defaut). */
static void test_completion_enum_simpleassign_fr_default(void) {
    printf("\n[TEST] fix/0696 : completion FR (defaut) simple-assign enum -> dispLibre\n");
    const char* src =
        "d est un disposition\n"
        "d = \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_completion_params(URI, 1, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "dispLibre", "completion simple-assign FR (dispLibre)");
    ASSERT_NOT_HAS(items, "dispFree",  "completion simple-assign FR no EN (dispFree)");
    ASSERT_DETAIL_PREFIX(items, "dispLibre", "Valeur de",
                         "completion simple-assign FR detail");
    json_free(items);
    json_free(params);
}

/* POUR TOUTE POSITION ... AVEC : valeurs combinables de
 * optionsRechercheChaine. En EN -> FromEnd + detail EN. */
static void test_completion_enum_pourtoute_en(void) {
    printf("\n[TEST] fix/0696 : completion EN POUR TOUTE...AVEC -> FromEnd\n");
    const char* src =
        "POUR TOUTE POSITION p DE \"a\" DANS \"abcabc\" AVEC \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* Curseur en fin de ligne (apres 'AVEC '). */
    int col = (int)strlen(src) - 1;  /* avant le \n */
    JsonValue* params = make_completion_params(URI, 0, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "FromEnd",   "completion POUR TOUTE EN (DepuisFin->FromEnd)");
    ASSERT_NOT_HAS(items, "DepuisFin", "completion POUR TOUTE EN no FR (DepuisFin)");
    /* Detail force le passage par la vraie branche POUR TOUTE (pas le
     * catalogue general de constantes). */
    ASSERT_DETAIL_PREFIX(items, "FromEnd", "Combinable option of",
                         "completion POUR TOUTE EN detail");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme POUR TOUTE en FR (defaut). */
static void test_completion_enum_pourtoute_fr_default(void) {
    printf("\n[TEST] fix/0696 : completion FR (defaut) POUR TOUTE...AVEC -> DepuisFin\n");
    const char* src =
        "POUR TOUTE POSITION p DE \"a\" DANS \"abcabc\" AVEC \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    int col = (int)strlen(src) - 1;
    JsonValue* params = make_completion_params(URI, 0, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "DepuisFin", "completion POUR TOUTE FR (DepuisFin)");
    ASSERT_NOT_HAS(items, "FromEnd",   "completion POUR TOUTE FR no EN (FromEnd)");
    ASSERT_DETAIL_PREFIX(items, "DepuisFin", "Option combinable de",
                         "completion POUR TOUTE FR detail");
    json_free(items);
    json_free(params);
}

/* Forward decl : defini plus bas (utilise ici par le test insertText EN). */
static const char* find_insertText_for_label(JsonValue* r, const char* label);

/* ================================================================
 *   — Completion + hover EN des events DSL
 *
 *  En mode language=en, la completion d'events (`PROCEDURE ` + ctrl+space)
 *  doit servir les noms EN (onClick, onChange…) et descEN, pour les 4
 *  sous-categories : widget type, lifecycle Fenetre, Etat top, sous-controle
 *  Etat. Fallback FR si pas de forme EN. Le hover sur un event sert aussi
 *  l'EN. Le mode FR (defaut) ne regresse pas.
 *
 *  RED-FIRST : avant impl, les 4 boucles event emettent d->canonical (FR)
 *  sans brancher sur wantEN, et handle_hover n'a aucune branche event ->
 *  ces tests rougissent. Apres impl -> verts.
 * ================================================================ */

/* (1) Widget type : body Champ Bouton -> surClic doit sortir 'onClick' en EN. */
static void test_completion_event_widget_en(void) {
    printf("\n[TEST] fix/0693 : completion EN event widget (Bouton) -> onClick\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Champ Bouton\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 2, 12);  /* apres 'PROCEDURE ' */
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "onClick", "completion event widget EN (surClic->onClick)");
    ASSERT_NOT_HAS(items, "surClic", "completion event widget EN no FR (surClic)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme body en FR (defaut) garde surClic canon. */
static void test_completion_event_widget_fr_default(void) {
    printf("\n[TEST] fix/0693 : completion FR (defaut) event widget -> surClic\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Champ Bouton\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "surClic", "completion event widget FR (surClic)");
    ASSERT_NOT_HAS(items, "onClick", "completion event widget FR no EN (onClick)");
    json_free(items);
    json_free(params);
}

/* Verifie que l'insertText de l'event EN porte bien le nom EN (handler
 * 'PROCEDURE onClick()' — accepte par le compilo, cf smoke ). */
static void test_completion_event_widget_en_insert(void) {
    printf("\n[TEST] fix/0693 : insertText event widget EN porte 'onClick'\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Champ Bouton\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    const char* it = find_insertText_for_label(items, "onClick");
    if (it && strstr(it, "onClick") != NULL) {
        g_pass++;
        printf("  [OK]   fix/0693 insertText EN contient 'onClick'\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0693 insertText EN='%s' (attendu 'onClick')\n",
               it ? it : "(null)");
    }
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* (2) Lifecycle Fenetre : body Fenetre EN -> FinInitialisation doit sortir
 * son nom EN (onEndInit / onInit selon catalogue). On verifie au moins que
 * la forme EN du premier event lifecycle est servie, pas la forme FR. */
static void test_completion_event_window_lifecycle_en(void) {
    printf("\n[TEST] fix/0693 : completion EN event lifecycle Fenetre\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 1, 11);  /* apres 'PROCEDURE ' (1 tab) */
    JsonValue* items = handle_completion(params);
    /* FinInitialisation a un nameEN : il ne doit PAS apparaitre en FR canon. */
    ASSERT_NOT_HAS(items, "FinInitialisation",
                   "completion event lifecycle EN no FR (FinInitialisation)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression FR lifecycle Fenetre : FinInitialisation present en FR. */
static void test_completion_event_window_lifecycle_fr_default(void) {
    printf("\n[TEST] fix/0693 : completion FR (defaut) event lifecycle Fenetre\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "FinInitialisation",
               "completion event lifecycle FR (FinInitialisation)");
    json_free(items);
    json_free(params);
}

/*  — filtre bilingue : une PROCEDURE declaree sous sa forme EN
 * (alias nameEN) doit etre reconnue par le filtre "deja declare", peu
 * importe la langue active. Bug : le filtre ne scannait que la forme
 * canon FR, donc une proc declaree en EN restait reproposee (double
 * declaration possible). */

/* (1) Lifecycle Fenetre, mode EN : 'PROCEDURE EndInitialization()' deja
 * declare -> ni EndInitialization ni FinInitialisation ne doivent etre
 * reproposes. */
static void test_procedure_event_filter_already_used_lifecycle_en(void) {
    printf("\n[TEST] feat/0727 : filtre EN lifecycle Fenetre (EndInitialization deja declare)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE EndInitialization()\n"
        "\t\tTrace(\"init\")\n"
        "\tFIN\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* Cursor ligne 4 (0-indexed) char 11 = apres '\tPROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 4, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "EndInitialization",
                   "filtre EN : EndInitialization deja declare (forme EN)");
    ASSERT_NOT_HAS(items, "FinInitialisation",
                   "filtre EN : FinInitialisation aussi filtre (meme event)");
    /* Un autre event lifecycle non declare reste dispo (forme EN). */
    ASSERT_HAS(items, "Closing",
               "filtre EN : Fermeture/Closing toujours dispo");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* (2) Event widget typé, mode EN : 'PROCEDURE onClick()' deja declare sur
 * un Bouton -> ni onClick ni surClic reproposes. */
static void test_procedure_event_filter_widget_already_used_en(void) {
    printf("\n[TEST] feat/0727 : filtre EN event widget typé (onClick deja declare)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Champ Bouton\n"
        "\t\tPROCEDURE onClick()\n"
        "\t\t\tTrace(\"clic\")\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    /* Cursor ligne 5 (0-indexed) char 12 = apres '\t\tPROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 5, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "onClick",
                   "filtre EN : onClick deja declare (forme EN)");
    ASSERT_NOT_HAS(items, "surClic",
                   "filtre EN : surClic aussi filtre (meme event)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* (3) Non-regression croisee : proc declaree en FR canon, completion en
 * mode EN -> l'event reste filtre (ni FR ni EN reproposes). Le filtre
 * doit etre robuste quelle que soit la combinaison langue-source. */
static void test_procedure_event_filter_fr_decl_en_mode(void) {
    printf("\n[TEST] feat/0727 : proc FR canon declaree, completion EN -> filtree\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tTrace(\"init\")\n"
        "\tFIN\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 4, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "EndInitialization",
                   "filtre : FinInitialisation declare FR -> EndInitialization filtre (mode EN)");
    ASSERT_NOT_HAS(items, "FinInitialisation",
                   "filtre : FinInitialisation declare FR -> filtre (mode EN)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *   — completion `PROCEDURE `+Ctrl+Espace dans un bloc
 *  OpérationHTTP/ServiceREST : proposer les events natifs du catalogue
 *  du bloc courant, filtres aux non-encore-declares. AVANT fix : aucun
 *  routage bloc service/operation -> fallback generique (deversement,
 *  centaines d'entrees), les events natifs n'apparaissent pas parmi les
 *  premieres propositions filtrees. ROUGE attendu (catalogues events
 *  service/operation absents + routage 's'/'h' non cable, cf.
 *  dsl_kind_string commentaire "Detection de contexte au curseur a
 *  cabler").
 * ================================================================ */

/* (1) Bloc OpérationHTTP : seul `surRequête` est propose (rien d'autre —
 * pas de deversement generique). */
static void test_completion_event_operation_http_fr(void) {
    printf("\n[TEST] fix/2012 : completion FR event OpérationHTTP -> surRequete seul\n");
    const char* src =
        "ServiceFacture est un ServiceREST\n"
        "\tHealthCheck est une OpérationHTTP\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';  /* defaut fr */

    JsonValue* params = make_completion_params(URI, 2, 12);  /* apres '\t\tPROCEDURE ' */
    JsonValue* items = handle_completion(params);
    ASSERT_HAS  (items, "surRequ\xc3\xaate", "completion event OpérationHTTP FR (surRequête)");
    ASSERT_COUNT(items, 1, "completion event OpérationHTTP FR : surRequête seul, pas de deversement");
    json_free(items);
    json_free(params);
}

/* Graphie EN : onRequest, jamais surRequete. */
static void test_completion_event_operation_http_en(void) {
    printf("\n[TEST] fix/2012 : completion EN event HTTPOperation -> onRequest seul\n");
    const char* src =
        "BillingService is a ServiceREST\n"
        "\tHealthCheck is an HTTPOperation\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "onRequest",   "completion event HTTPOperation EN (onRequest)");
    ASSERT_NOT_HAS(items, "surRequete",  "completion event HTTPOperation EN no FR (surRequete)");
    ASSERT_COUNT  (items, 1, "completion event HTTPOperation EN : onRequest seul");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* (2) Bloc ServiceREST (top-level, hors OpérationHTTP) : les 5 events/hooks
 * niveau service (surAuthentification, surAutorisation, surErreur,
 * FinInitialisation, Fermeture) sont proposes — rien de plus. */
static void test_completion_event_service_rest_fr(void) {
    printf("\n[TEST] fix/2012 : completion FR events ServiceREST -> 5 hooks niveau service\n");
    const char* src =
        "ServiceFacture est un ServiceREST\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_completion_params(URI, 1, 11);  /* apres '\tPROCEDURE ' */
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "surAuthentification", "completion event ServiceREST FR (surAuthentification)");
    ASSERT_HAS(items, "surAutorisation",     "completion event ServiceREST FR (surAutorisation)");
    ASSERT_HAS(items, "surErreur",           "completion event ServiceREST FR (surErreur)");
    ASSERT_HAS(items, "FinInitialisation",   "completion event ServiceREST FR (FinInitialisation)");
    ASSERT_HAS(items, "Fermeture",           "completion event ServiceREST FR (Fermeture)");
    ASSERT_COUNT(items, 5, "completion event ServiceREST FR : 5 hooks, pas de deversement");
    json_free(items);
    json_free(params);
}

/* (3) Filtre non-encore-declare : `surAuthentification` deja pose ->
 * ni lui ni sa forme EN ne sont reproposes, les 4 autres restent. */
static void test_completion_event_service_rest_filter_already_used_fr(void) {
    printf("\n[TEST] fix/2012 : filtre ServiceREST FR — surAuthentification deja declare\n");
    const char* src =
        "ServiceFacture est un ServiceREST\n"
        "\tPROCEDURE surAuthentification()\n"
        "\tFIN\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_completion_params(URI, 3, 11);  /* apres '\tPROCEDURE ' */
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surAuthentification",
                   "filtre ServiceREST FR : surAuthentification deja declare -> filtre");
    ASSERT_HAS(items, "surAutorisation",   "filtre ServiceREST FR : surAutorisation toujours dispo");
    ASSERT_HAS(items, "surErreur",         "filtre ServiceREST FR : surErreur toujours dispo");
    ASSERT_HAS(items, "FinInitialisation", "filtre ServiceREST FR : FinInitialisation toujours dispo");
    ASSERT_HAS(items, "Fermeture",         "filtre ServiceREST FR : Fermeture toujours dispo");
    ASSERT_COUNT(items, 4, "filtre ServiceREST FR : 4 hooks restants apres declaration de 1");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — support #158 : en mode langue EN, les snippets de
 *  completion inseraient encore du FRANCAIS (mot-cle `FIN` en fin de
 *  corps de snippet event, et article `un/une` devant un type). La
 *  bascule wantEN n'etait pas appliquee aux 5 sites d'emission de
 *  snippet (events lifecycle widget/projet/classe/fixtures + article
 *  type-declaration), alors qu'elle l'etait aux labels juste a cote.
 *  RED avant fix (END attendu, FIN produit ; `a` attendu, `un/une`
 *  produit). Temoins FR : formes inchangees.
 * ================================================================ */

/* Site 1 — lifecycle Fenetre (PUSH_LIFECYCLE_ITEM). EN -> corps se
 * termine par END, jamais FIN. */
static void test_completion_lifecycle_event_en_end_keyword(void) {
    printf("\n[TEST] fix/1125 : snippet event lifecycle EN finit par END (pas FIN)\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 1, 11);  /* apres '\tPROCEDURE ' */
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "EndInitialization", "END",
                               "event lifecycle EN : snippet ferme par END");
    ASSERT_INSERT_NOT_CONTAINS(items, "EndInitialization", "FIN",
                               "event lifecycle EN : pas de FIN dans le snippet");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Temoin FR (non-regression) : le meme snippet garde FIN en mode FR. */
static void test_completion_lifecycle_event_fr_fin_keyword(void) {
    printf("\n[TEST] fix/1125 : snippet event lifecycle FR garde FIN (temoin)\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "FinInitialisation", "FIN",
                               "event lifecycle FR : snippet ferme par FIN");
    ASSERT_INSERT_NOT_CONTAINS(items, "FinInitialisation", "END",
                               "event lifecycle FR : pas de END dans le snippet");
    json_free(items);
    json_free(params);
}

/* Site 2 — event widget type (onClick sur Bouton). EN -> END. */
static void test_completion_typed_widget_event_en_end_keyword(void) {
    printf("\n[TEST] fix/1125 : snippet event widget type EN finit par END\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tBTN_A is a Control Button\n"
        "\t\tPROCEDURE \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 2, 12);  /* apres '\t\tPROCEDURE ' */
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "onClick", "END",
                               "event widget EN : snippet ferme par END");
    ASSERT_INSERT_NOT_CONTAINS(items, "onClick", "FIN",
                               "event widget EN : pas de FIN dans le snippet");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Site 3 — event Projet (Closing) au top-level d'un projet.owl. EN -> END. */
static void test_completion_projet_event_en_end_keyword(void) {
    printf("\n[TEST] fix/1125 : snippet event Projet EN finit par END\n");
    const char* src =
        "MonApp is a Project\n"
        "END\n"
        "PROCEDURE \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 2, 10);  /* apres 'PROCEDURE ' top-level */
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "Closing", "END",
                               "event Projet EN : snippet ferme par END");
    ASSERT_INSERT_NOT_CONTAINS(items, "Closing", "FIN",
                               "event Projet EN : pas de FIN dans le snippet");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Site 4 — event Classe (Constructeur/Destructeur) en body classe. Le
 * nom reste FR (pas de forme EN), mais le mot-cle de fermeture doit
 * suivre la langue : EN -> END. */
static void test_completion_class_event_en_end_keyword(void) {
    printf("\n[TEST] fix/1125 : snippet event Classe EN finit par END\n");
    const char* src =
        "cProduit is a Class\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "Constructeur", "END",
                               "event Classe EN : snippet ferme par END");
    ASSERT_INSERT_NOT_CONTAINS(items, "Constructeur", "FIN",
                               "event Classe EN : pas de FIN dans le snippet");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Site 5 — fixtures test runner + prefixes scenario_. EN -> END. */
static void test_completion_test_fixture_en_end_keyword(void) {
    printf("\n[TEST] fix/1125 : snippet fixture/scenario test EN finit par END\n");
    const char* src = "PROCEDURE \n";
    setup_doc("file:///c:/test/test_Foo.owl", src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params("file:///c:/test/test_Foo.owl", 0, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS    (items, "beforeEachTest", "END",
                               "fixture test EN : snippet ferme par END");
    ASSERT_INSERT_NOT_CONTAINS(items, "beforeEachTest", "FIN",
                               "fixture test EN : pas de FIN dans le snippet");
    ASSERT_INSERT_CONTAINS    (items, "scenario_nom", "END",
                               "prefixe scenario EN : snippet ferme par END");
    ASSERT_INSERT_NOT_CONTAINS(items, "scenario_nom", "FIN",
                               "prefixe scenario EN : pas de FIN dans le snippet");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Site article — declaration typee sans article (`X is `). 
 * (support #271) : en mode EN, la completion n'insere PLUS d'article de
 * confort (l'article `a` etait toujours faux pour les types a son voyelle,
 * ex. `a integer`). L'insertText = le type seul. Le runtime reste tolerant
 * (l'article est accepte SI tape). Le FR est inchange (cf. temoin ci-dessous). */
static void test_completion_typedecl_article_en(void) {
    printf("\n[TEST] fix/1641 : type-declaration EN sans article de confort\n");
    const char* src = "X is \n";  /* col 5 apres 'X is ' (pas d'article) */
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 0, 5);
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_EQUALS      (items, "string", "string",
                               "type EN : insertText = type seul (pas d'article)");
    ASSERT_INSERT_NOT_CONTAINS(items, "string", "a ",
                               "type EN : pas d'article EN 'a'");
    ASSERT_INSERT_NOT_CONTAINS(items, "string", "une",
                               "type EN : pas d'article FR 'une'");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Temoin FR : sans article (`X est `), l'insertText prefixe l'article FR. */
static void test_completion_typedecl_article_fr(void) {
    printf("\n[TEST] fix/1125 : article type-declaration FR = 'une' (temoin)\n");
    const char* src = "X est \n";  /* col 6 apres 'X est ' */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 6);
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS(items, "cha\xc3\xaene", "une cha\xc3\xaene",
                           "article FR : insertText prefixe 'une'");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   (support #271) — l'article de confort n'est plus insere en
 *  mode EN, sur TOUTES les familles de types proposees en position
 *  type-declaration (`X is <cursor>`) : primitifs, types registres, types
 *  DSL. Le FR conserve strictement `un`/`une` (grammaticalement corrects,
 *  geres par genre). Le runtime reste tolerant : l'article est accepte SI
 *  l'utilisateur le tape (comportement typeDeclHasArticle inchange, non
 *  reteste ici — deja couvert ailleurs).
 * ================================================================ */
static void test_fix1641_typedecl_no_article_en(void) {
    printf("\n[TEST] fix/1641 : EN sans article (primitif + registre + DSL)\n");
    const char* src = "X is \n";  /* col 5 apres 'X is ' */
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 0, 5);
    JsonValue* items = handle_completion(params);
    /* Primitif — l'exemple du ticket : `iMyVar is integer`, pas `a integer`. */
    ASSERT_INSERT_EQUALS      (items, "integer", "integer",
                               "primitif EN : insertText = 'integer'");
    ASSERT_INSERT_NOT_CONTAINS(items, "integer", "a ",
                               "primitif EN : pas de 'a '");
    /* Type registre. */
    ASSERT_INSERT_EQUALS      (items, "multilingualString", "multilingualString",
                               "registre EN : insertText = type seul");
    ASSERT_INSERT_NOT_CONTAINS(items, "multilingualString", "a ",
                               "registre EN : pas de 'a '");
    /* Type DSL (snippet auto-fermant) : le corps commence par le type,
     * jamais par l'article. */
    ASSERT_INSERT_NOT_CONTAINS(items, "Window", "a Window",
                               "DSL EN : pas de 'a Window'");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Temoin non-regression FR pour  : les memes familles conservent
 * l'article FR (un/une). */
static void test_fix1641_typedecl_article_fr_baseline(void) {
    printf("\n[TEST] fix/1641 : FR conserve l'article (temoin non-regression)\n");
    const char* src = "X est \n";  /* col 6 apres 'X est ' */
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 0, 6);
    JsonValue* items = handle_completion(params);
    /* Primitif : `un entier`. */
    ASSERT_INSERT_EQUALS  (items, "entier", "un entier",
                           "primitif FR : insertText = 'un entier'");
    /* Type registre : `un chaîneMultilingue`. */
    ASSERT_INSERT_CONTAINS(items, "cha\xc3\xaeneMultilingue", "un cha\xc3\xaeneMultilingue",
                           "registre FR : insertText prefixe 'un'");
    /* Type DSL : `une Fenêtre(...)`. */
    ASSERT_INSERT_CONTAINS(items, "Fen\xc3\xaatre", "une Fen\xc3\xaatre",
                           "DSL FR : insertText prefixe 'une'");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — digue anti-fuite linguistique.  a corrige 5 sites
 *  d'emission connus au cas par cas (snippets events + article type-
 *  declaration). Correctif ponctuel, site par site : rien n'empeche qu'un
 *  6e site (ou une regression future) reintroduise du francais dans une
 *  sortie EN (ou l'inverse). Cette section construit une DIGUE generique :
 *  un scanner qui inspecte TOUS les champs texte user-facing
 *  (label/detail/insertText/documentation) de CHAQUE item retourne par la
 *  completion, et compte les occurrences de tokens de la langue OPPOSEE
 *  au mode courant.
 *
 *  Anti-faux-positif : matching sur FRONTIERE DE MOT stricte. Le token
 *  doit etre encadre par des caracteres non alphanumeriques/non-'_' des
 *  DEUX cotes (les octets UTF-8 multi-byte, bit haut positionne, comptent
 *  comme "lettres" pour ne pas couper un mot accentue au milieu). Sans
 *  cette frontiere, "un"/"une" matcherait a l'interieur d'identifiants
 *  legitimes (`Function`), et "est" a l'interieur de `beforeEachTest` ou
 *  `test_Foo.owl` (fixture EN deja correcte,  site 5). Verifie a
 *  la main sur ces deux cas avant de figer la liste noire.
 *
 *  Liste noire volontairement restreinte a des MOTS-CLES / ARTICLES /
 *  COPULES non-ambigus, jamais des mots qui peuvent legitimement
 *  apparaitre dans un identifiant ou une phrase EN :
 *   FR (verifiee en mode EN)  : FIN, SI, ALORS, SINON, POUR, TANTQUE,
 *                               RETOUR, un, une, "Type utilisateur".
 *   EN (verifiee en mode FR)  : END, IF, ELSE, WHILE, RETURN.
 *  L'article EN "a"/"an" n'est PAS dans la liste generique (bruit — "a"
 *  seul est une lettre anglaise tres frequente) ; il est verifie
 *  ponctuellement via ASSERT_INSERT_CONTAINS sur l'item cible, comme le
 *  faisait deja .
 * ================================================================ */

static bool is_word_byte(unsigned char c) {
    return isalnum(c) || c == '_' || (c & 0x80) != 0;
}

/* Compte + logue (stdout, capture par -vv du runner) les occurrences
 * frontiere-de-mot de `token` dans `text`. `field`/`itemLabel`/`modeDesc`
 * ne servent qu'au rapport humain. */
static int count_word_leaks(const char* text, const char* token,
                             const char* field, const char* itemLabel,
                             const char* modeDesc) {
    if (!text || !token || !*token) return 0;
    int leaks = 0;
    size_t tlen = strlen(token);
    const char* p = text;
    while ((p = strstr(p, token)) != NULL) {
        bool leftOk  = (p == text) || !is_word_byte((unsigned char)p[-1]);
        bool rightOk = !is_word_byte((unsigned char)p[tlen]);
        if (leftOk && rightOk) {
            leaks++;
            printf("  [LEAK] mode=%s item='%s' champ=%s token=\"%s\"\n",
                   modeDesc, itemLabel ? itemLabel : "?", field, token);
        }
        p += tlen;
    }
    return leaks;
}

/* Champ `documentation`, qu'il soit pose en string simple ou en objet
 * markdown {kind,value}. */
static const char* item_doc_string(JsonValue* item) {
    JsonValue* doc = json_get(item, "documentation");
    if (!doc) return NULL;
    if (doc->type == JSON_STRING) return doc->as.stringVal.str;
    if (doc->type == JSON_OBJECT) return json_get_string(doc, "value");
    return NULL;
}

/* Scanne TOUS les items d'un resultat de completion contre une liste
 * noire de tokens, sur les 4 champs texte user-facing. Renvoie le total
 * de fuites detectees (0 = digue propre pour ce resultat). */
static int scan_completion_for_leaks(JsonValue* result, const char** tokens,
                                      int tokenCount, const char* modeDesc) {
    JsonValue* items = unwrap_items(result);
    if (!items || items->type != JSON_ARRAY) return 0;
    int total = 0;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl    = json_get_string(item, "label");
        const char* detail = json_get_string(item, "detail");
        const char* insert = json_get_string(item, "insertText");
        const char* doc    = item_doc_string(item);
        for (int t = 0; t < tokenCount; t++) {
            total += count_word_leaks(lbl,    tokens[t], "label",         lbl, modeDesc);
            total += count_word_leaks(detail, tokens[t], "detail",        lbl, modeDesc);
            total += count_word_leaks(insert, tokens[t], "insertText",    lbl, modeDesc);
            total += count_word_leaks(doc,    tokens[t], "documentation", lbl, modeDesc);
        }
    }
    return total;
}

/* Liste noire FR (verifiee en mode EN). */
static const char* FR_LEAK_TOKENS[] = {
    "FIN", "SI", "ALORS", "SINON", "POUR", "TANTQUE", "RETOUR",
    "un", "une", "Type utilisateur",
};
#define FR_LEAK_TOKENS_COUNT ((int)(sizeof(FR_LEAK_TOKENS)/sizeof(FR_LEAK_TOKENS[0])))

/* Liste noire EN (verifiee en mode FR, temoin non-regression). */
static const char* EN_LEAK_TOKENS[] = {
    "END", "IF", "ELSE", "WHILE", "RETURN",
};
#define EN_LEAK_TOKENS_COUNT ((int)(sizeof(EN_LEAK_TOKENS)/sizeof(EN_LEAK_TOKENS[0])))

/* Liste dediee aux `detail` de types primitifs (OWL_PRIM_TYPES[].desc) :
 * ce champ n'a jamais eu de variante EN (pas de champ descEN dans la
 * struct), independamment du site nomme par le ticket . Isolee du
 * blacklist generique pour ne pas polluer les autres scans (ces mots ne
 * sont pas des mots-cles/copules mais des noms de types, propres a cette
 * verification ciblee). */
static const char* FR_TYPEDESC_LEAK_TOKENS[] = {
    "entier", "cha\xc3\xaene", "bool\xc3\xa9""en", "r\xc3\xa9""el",
    "mon\xc3\xa9""taire", "num\xc3\xa9""rique", "caract\xc3\xa8""re",
    "dur\xc3\xa9""e", "Divers",
};
#define FR_TYPEDESC_LEAK_TOKENS_COUNT \
    ((int)(sizeof(FR_TYPEDESC_LEAK_TOKENS)/sizeof(FR_TYPEDESC_LEAK_TOKENS[0])))

/*  — liste dediee a la completion des MEMBRES d'un type Police
 * (apres `..`). Cible les labels FR (Nom/Taille/Gras/...) et quelques mots
 * distinctifs des descriptions FR (Vrai/famille/police/typographiques), qui
 * DISPARAISSENT tous en mode EN au profit des formes anglaises du catalogue
 * (Name/Size/Bold/... + descriptions EN). Volontairement hors de cette liste :
 * les NOMS DE TYPE du detail (chaine/entier/booleen), dont la colonne typeEN
 * du catalogue est identique au FR (gap data hors-scope, cf. Notes builder). */
static const char* FR_FONT_LEAK_TOKENS[] = {
    "Nom", "Taille", "Gras", "Couleur",
    "Vrai", "famille", "police", "typographiques",
};
#define FR_FONT_LEAK_TOKENS_COUNT \
    ((int)(sizeof(FR_FONT_LEAK_TOKENS)/sizeof(FR_FONT_LEAK_TOKENS[0])))

#define ASSERT_NO_LEAKS(count, test_name) do { \
    if ((count) == 0) { \
        g_pass++; \
        printf("  [OK]   %s : 0 fuite detectee\n", test_name); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : %d fuite(s) detectee(s) (cf. [LEAK] ci-dessus)\n", \
               test_name, (count)); \
    } \
} while (0)

/* Cible ROUGE nommee par le ticket : lsp_handlers.c:5008-5019, completion
 * de type-declaration sur un type Structure UTILISATEUR (pas les types
 * enregistres du catalogue, deja corriges par ). L'article insere
 * reste fige "un" et le detail est "Type utilisateur (structure/classe)"
 * quelle que soit la langue active. */
static void test_leak_userstruct_declaration_en(void) {
    printf("\n[TEST] fix/1127 : digue EN -- declaration de type Structure utilisateur (site connu 5008-5019)\n");
    const char* src =
        "MaStruct is a Structure\n"
        "\tchamp is a text\n"
        "END\n"
        "x is \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 3, 5);  /* apres 'x is ' */
    JsonValue* items = handle_completion(params);

    /* Assertion ciblee :  retire l'article de confort en EN
     * (insertText = type utilisateur seul, plus de 'a MaStruct'). */
    ASSERT_INSERT_EQUALS      (items, "MaStruct", "MaStruct",
                               "type utilisateur EN : insertText = type seul (fix/1641)");
    ASSERT_INSERT_NOT_CONTAINS(items, "MaStruct", "un MaStruct",
                               "type utilisateur EN : pas d'article FR 'un' fige (fix/1127)");

    /* Digue generique : aucune fuite FR sur l'ensemble des items. */
    int leaks = scan_completion_for_leaks(items, FR_LEAK_TOKENS,
                                           FR_LEAK_TOKENS_COUNT, "EN");
    ASSERT_NO_LEAKS(leaks, "digue EN : declaration Structure utilisateur (fix/1127)");

    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Temoin FR (non-regression) : meme contexte en FR, doit rester propre.
 * NB : le site (lsp_handlers.c:5015) prefixe TOUJOURS "un" pour un type
 * Structure utilisateur, quelle que soit la langue et quel que soit le
 * genre grammatical reel du nom (convention deja en place avant ,
 * hors-scope de ce ticket qui porte sur la bascule FR/EN, pas le genre).
 * Le temoin FR verifie donc "un MaStruct" (comportement FR inchangee),
 * pas "une MaStruct" qui serait une attente inventee sans base. */
static void test_leak_userstruct_declaration_fr_baseline(void) {
    printf("\n[TEST] fix/1127 : temoin FR -- declaration Structure utilisateur inchangee\n");
    const char* src =
        "MaStruct est une Structure\n"
        "\tchamp est un texte\n"
        "FIN\n"
        "x est \n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 3, 6);  /* apres 'x est ' */
    JsonValue* items = handle_completion(params);
    ASSERT_INSERT_CONTAINS(items, "MaStruct", "un MaStruct",
                           "type utilisateur FR : article 'un' inchange (temoin fix/1127)");
    int leaks = scan_completion_for_leaks(items, EN_LEAK_TOKENS,
                                          EN_LEAK_TOKENS_COUNT, "FR");
    ASSERT_NO_LEAKS(leaks, "digue FR : declaration Structure utilisateur (temoin fix/1127)");
    json_free(items);
    json_free(params);
}

/* Fuite ADDITIONNELLE (hors liste du ticket) : le `detail` des types
 * PRIMITIFS (OWL_PRIM_TYPES[].desc, ex "Type entier 64-bit signe") n'a
 * jamais de variante EN -- contrairement au label et a l'insertText, deja
 * corriges par . Meme fichier/fonction que le site nomme par le
 * ticket (handle_completion, branche typeDeclContext), mais ~40 lignes
 * plus haut (OWL_PRIM_TYPES loop, avant la loop OWL_TYPE_CATALOG). */
static void test_leak_primitive_typedecl_detail_en(void) {
    printf("\n[TEST] fix/1127 : digue EN -- detail des types primitifs (fuite additionnelle, hors liste ticket)\n");
    const char* src = "X is \n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 0, 5);
    JsonValue* items = handle_completion(params);
    /* label + insertText en EN :  retire l'article de confort
     * (insertText = type seul, plus de 'a string'). */
    ASSERT_INSERT_EQUALS(items, "string", "string",
                         "type primitif EN : insertText = type seul (fix/1641)");
    /* Le detail, lui, n'a jamais ete localise. */
    int leaks = scan_completion_for_leaks(items, FR_TYPEDESC_LEAK_TOKENS,
                                           FR_TYPEDESC_LEAK_TOKENS_COUNT, "EN");
    ASSERT_NO_LEAKS(leaks, "digue EN : detail type primitif (fuite additionnelle fix/1127)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression / preuve anti-faux-positif : la digue generique (FR_LEAK_
 * TOKENS) appliquee a un site DEJA corrige par  (snippet event
 * lifecycle) doit rester VERTE — sinon la liste noire est trop large. */
static void test_leak_lifecycle_event_en_clean(void) {
    printf("\n[TEST] fix/1127 : digue EN -- snippet event lifecycle (site deja fixe fix/1125, non-regression)\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    int leaks = scan_completion_for_leaks(items, FR_LEAK_TOKENS,
                                           FR_LEAK_TOKENS_COUNT, "EN");
    ASSERT_NO_LEAKS(leaks, "digue EN : snippet event lifecycle (non-regression fix/1125 + fix/1127)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Idem sur le snippet fixtures test (site 5 de ). */
static void test_leak_test_fixture_en_clean(void) {
    printf("\n[TEST] fix/1127 : digue EN -- snippet fixture test (site deja fixe fix/1125, non-regression)\n");
    const char* src = "PROCEDURE \n";
    setup_doc("file:///c:/test/test_Foo.owl", src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params("file:///c:/test/test_Foo.owl", 0, 10);
    JsonValue* items = handle_completion(params);
    int leaks = scan_completion_for_leaks(items, FR_LEAK_TOKENS,
                                           FR_LEAK_TOKENS_COUNT, "EN");
    ASSERT_NO_LEAKS(leaks, "digue EN : snippet fixture test (non-regression fix/1125 + fix/1127, anti-FP beforeEachTest/test_Foo.owl)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  — completion des MEMBRES d'un type Police (apres `..`). Le chemin
 * registry (OwlTypeInfo->fields) emettait les labels FR (Nom/Taille/Gras/...)
 * et descriptions FR en dur, meme en mode serveur EN, contrairement au bloc
 * Projet qui applique deja wantEN/nameEN. Cible ROUGE : lsp_handlers.c:5173. */
static void test_leak_font_member_completion_en(void) {
    printf("\n[TEST] fix/1146 : digue EN -- completion membres Police (p.. en mode EN)\n");
    const char* src =
        "p est une Police\n"
        "p..\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 1, 3);  /* apres 'p..' */
    JsonValue* items = handle_completion(params);

    /* Labels EN attendus (nameEN du catalogue OWL_TYPE_FIELDS_12). */
    ASSERT_HAS(items, "Name",       "Police EN : 'Name' present");
    ASSERT_HAS(items, "Size",       "Police EN : 'Size' present");
    ASSERT_HAS(items, "Bold",       "Police EN : 'Bold' present");
    ASSERT_HAS(items, "Italic",     "Police EN : 'Italic' present");
    ASSERT_HAS(items, "Underlined", "Police EN : 'Underlined' present");
    ASSERT_HAS(items, "StrikeOut",  "Police EN : 'StrikeOut' present");
    ASSERT_HAS(items, "Color",      "Police EN : 'Color' present");

    /* Labels FR : bannis en mode EN. */
    ASSERT_NOT_HAS(items, "Nom",      "Police EN : label FR 'Nom' absent");
    ASSERT_NOT_HAS(items, "Taille",   "Police EN : label FR 'Taille' absent");
    ASSERT_NOT_HAS(items, "Gras",     "Police EN : label FR 'Gras' absent");
    ASSERT_NOT_HAS(items, "Italique", "Police EN : label FR 'Italique' absent");
    ASSERT_NOT_HAS(items, "Couleur",  "Police EN : label FR 'Couleur' absent");

    /* Digue generique : aucune fuite FR (labels + descriptions). */
    int leaks = scan_completion_for_leaks(items, FR_FONT_LEAK_TOKENS,
                                           FR_FONT_LEAK_TOKENS_COUNT, "EN");
    ASSERT_NO_LEAKS(leaks, "digue EN : completion membres Police (fix/1146)");

    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Temoin FR (non-regression) : meme contexte en FR, labels FR inchanges +
 * aucune fuite EN. Verrouille la byte-identite du mode FR. */
static void test_leak_font_member_completion_fr_baseline(void) {
    printf("\n[TEST] fix/1146 : temoin FR -- completion membres Police inchangee\n");
    const char* src =
        "p est une Police\n"
        "p..\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    JsonValue* params = make_completion_params(URI, 1, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Nom",      "Police FR : 'Nom' present (temoin fix/1146)");
    ASSERT_HAS(items, "Taille",   "Police FR : 'Taille' present (temoin fix/1146)");
    ASSERT_HAS(items, "Gras",     "Police FR : 'Gras' present (temoin fix/1146)");
    ASSERT_HAS(items, "Italique", "Police FR : 'Italique' present (temoin fix/1146)");
    ASSERT_HAS(items, "Couleur",  "Police FR : 'Couleur' present (temoin fix/1146)");
    ASSERT_NOT_HAS(items, "Bold",  "Police FR : label EN 'Bold' absent (temoin fix/1146)");
    int leaks = scan_completion_for_leaks(items, EN_LEAK_TOKENS,
                                          EN_LEAK_TOKENS_COUNT, "FR");
    ASSERT_NO_LEAKS(leaks, "digue FR : completion membres Police (temoin fix/1146)");
    json_free(items);
    json_free(params);
}

/*  — signatureHelp (l'aide qui popup pendant la saisie des args
 * d'un appel) n'appliquait jamais la bascule de langue : le champ
 * `documentation` de la signature emettait `fn->description` (FR) en dur,
 * meme en mode serveur EN, contrairement au hover et a la completion.
 * Digue : scanner les `documentation` de chaque signature retournee et
 * compter les fuites FR en mode EN. Le niveau param (params[].documentation)
 * reste FR (pas de descEN au catalogue) — HORS-SCOPE, non scanne ici. */
static int scan_sighelp_docs_for_leaks(JsonValue* result, const char** tokens,
                                        int tokenCount, const char* modeDesc) {
    if (!result) return 0;
    JsonValue* sigs = json_get(result, "signatures");
    if (!sigs || sigs->type != JSON_ARRAY) return 0;
    int total = 0;
    for (int i = 0; i < sigs->as.arrayVal.count; i++) {
        JsonValue* sig = sigs->as.arrayVal.items[i];
        const char* lbl = json_get_string(sig, "label");
        const char* doc = item_doc_string(sig);
        for (int t = 0; t < tokenCount; t++)
            total += count_word_leaks(doc, tokens[t], "documentation",
                                      lbl, modeDesc);
    }
    return total;
}

/* Construit les params LSP pour un appel signatureHelp (position curseur). */
static JsonValue* make_sighelp_params(const char* uri, int line, int character) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    json_object_set(params, "position", pos);
    return params;
}

/* Renvoie la `documentation` (string) de la 1ere signature du resultat. */
static const char* sighelp_first_doc(JsonValue* result) {
    if (!result) return NULL;
    JsonValue* sigs = json_get(result, "signatures");
    if (!sigs || sigs->type != JSON_ARRAY || sigs->as.arrayVal.count == 0)
        return NULL;
    return item_doc_string(sigs->as.arrayVal.items[0]);
}

/* Cible ROUGE nommee par le ticket : appel `fExtractPath(` (nom EN) en mode
 * serveur EN. La description FR "Extrait un composant d'un chemin..." fuit le
 * token FR "un" ; l'attendu est la description EN "Extract a component...". */
static void test_leak_signaturehelp_en(void) {
    printf("\n[TEST] fix/1140 : digue EN -- signatureHelp documentation (fExtractPath)\n");
    const char* src = "sFile is a text = fExtractPath(sPath, fFile)\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Curseur juste apres la '(' de fExtractPath( : "sFile is a text = "
     * = 18 chars, "fExtractPath" = 12, '(' a l'index 30, curseur = 31. */
    JsonValue* params = make_sighelp_params(URI, 0, 31);
    JsonValue* result = handle_signature_help(params);

    /* Valeur exacte : la doc doit etre la description EN, pas la FR. */
    const char* doc = sighelp_first_doc(result);
    if (doc && strcmp(doc, "Extract a component of a path (fDisque, fRepertoire, fFichier, fExtension).") == 0) {
        g_pass++;
        printf("  [OK]   documentation = description EN attendue\n");
    } else {
        g_fail++;
        printf("  [FAIL] documentation = \"%s\" (attendu description EN)\n",
               doc ? doc : "(null)");
    }

    /* Digue generique : aucune fuite FR sur les docs de signature. */
    int leaks = scan_sighelp_docs_for_leaks(result, FR_LEAK_TOKENS,
                                             FR_LEAK_TOKENS_COUNT, "EN");
    ASSERT_NO_LEAKS(leaks, "digue EN : signatureHelp documentation (fix/1140)");

    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Temoin FR (non-regression) : meme appel en FR (defaut), la doc doit rester
 * la description FR byte-identique (garantit que le mode FR n'a pas bouge). */
static void test_leak_signaturehelp_fr_baseline(void) {
    printf("\n[TEST] fix/1140 : temoin FR -- signatureHelp documentation inchangee\n");
    const char* src = "sFichier est un texte = fExtraitChemin(sChemin, fFichier)\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';
    /* Curseur apres la '(' : "sFichier est un texte = " = 24, "fExtraitChemin"
     * = 14, '(' a l'index 38, curseur = 39. */
    JsonValue* params = make_sighelp_params(URI, 0, 39);
    JsonValue* result = handle_signature_help(params);

    const char* doc = sighelp_first_doc(result);
    if (doc && strcmp(doc, "Extrait un composant d'un chemin (fDisque, fRepertoire, fFichier, fExtension).") == 0) {
        g_pass++;
        printf("  [OK]   documentation = description FR inchangee (byte-identique)\n");
    } else {
        g_fail++;
        printf("  [FAIL] documentation = \"%s\" (attendu description FR)\n",
               doc ? doc : "(null)");
    }

    int leaks = scan_sighelp_docs_for_leaks(result, EN_LEAK_TOKENS,
                                            EN_LEAK_TOKENS_COUNT, "FR");
    ASSERT_NO_LEAKS(leaks, "digue FR : signatureHelp documentation (temoin fix/1140)");

    json_free(result);
    json_free(params);
}

/*  — fichier fenetre 100% ANGLAIS (`w_Test is a Window`) : la
 * completion apres `PROCEDURE ` dans le body doit proposer les events du
 * cycle de vie (EndInitialization, Closing, ...) et PAS retomber sur la
 * liste generique de natives. Regression signalee apres 1.0.3 : les tests
 * EN preexistants utilisaient tous un en-tete FR (`FEN_X est une Fenetre`)
 * + langue=en, jamais l'article EN `is a` dans l'en-tete widget combine a
 * la completion PROCEDURE. */
static void test_procedure_event_completion_full_english(void) {
    printf("\n[TEST] feat/0984 : 'PROCEDURE ' body Fenetre full-English propose events\n");
    const char* src =
        "w_Test is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"Test\"\n"
        "\tEND\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Cursor ligne 4 (0-indexed) char 11 = juste apres '\tPROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 4, 11);
    JsonValue* items = handle_completion(params);
    /* Events lifecycle Fenetre (forme EN car langue=en). */
    ASSERT_HAS    (items, "EndInitialization", "PROC body Window EN");
    ASSERT_HAS    (items, "Closing",           "PROC body Window EN");
    /* Pas de pollution generique : natives absentes. */
    ASSERT_NOT_HAS(items, "Trace",             "PROC body Window EN (pas de native)");
    ASSERT_NOT_HAS(items, "Ouvre",             "PROC body Window EN (pas de native)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  — baseline FR : meme scenario mais en-tete FR. Doit deja
 * marcher (non-regression). */
static void test_procedure_event_completion_full_french(void) {
    printf("\n[TEST] feat/0984 : 'PROCEDURE ' body Fenetre FR propose events (baseline)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"Test\"\n"
        "\tFIN\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    /* Cursor ligne 4 (0-indexed) char 11 = juste apres '\tPROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 4, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "FinInitialisation", "PROC body Fenetre FR");
    ASSERT_HAS    (items, "Fermeture",         "PROC body Fenetre FR");
    ASSERT_NOT_HAS(items, "Trace",             "PROC body Fenetre FR (pas de native)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *  — audit EXHAUSTIF dedup (event_already_declared) sur les
 * angles morts full-English. Le besoin dev : ne PAS reproposer un event
 * DEJA UTILISE dans le bloc courant, exactement comme pour les proprietes.
 * ================================================================ */

/* Cas 1 — events TYPES de widget (Bouton full-English) : 3 events declares
 * (onClick / onEntry / onExit) doivent etre filtres au 2e `PROCEDURE `, et
 * ne PAS reapparaitre sous leur forme FR (surClic / surEntree / surSortie).
 * Un event non declare (onDoubleClick) reste propose : preuve que la
 * completion tourne encore. */
static void test_dedup_typed_widget_events_en(void) {
    printf("\n[TEST] feat/0984 : dedup events typES bouton full-English (onClick/onEntry/onExit)\n");
    const char* src =
        "w_Test is a Window\n"
        "\tBTN_A is a Field Button\n"
        "\t\tPROCEDURE onClick()\n"
        "\t\t\tInfo(\"hi\")\n"
        "\t\tEND\n"
        "\t\tPROCEDURE onEntry()\n"
        "\t\tEND\n"
        "\t\tPROCEDURE onExit()\n"
        "\t\tEND\n"
        "\t\tPROCEDURE \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Ligne 9 (0-indexed) = "\t\tPROCEDURE " -> 2 tabs + 10 chars = char 12. */
    JsonValue* params = make_completion_params(URI, 9, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "onClick",       "cas1 : onClick declare -> filtre (EN)");
    ASSERT_NOT_HAS(items, "onEntry",       "cas1 : onEntry declare -> filtre (EN)");
    ASSERT_NOT_HAS(items, "onExit",        "cas1 : onExit declare -> filtre (EN)");
    ASSERT_NOT_HAS(items, "surClic",       "cas1 : forme FR surClic jamais reproposee (EN)");
    ASSERT_NOT_HAS(items, "surEntr\xc3\xa9""e", "cas1 : forme FR surEntree jamais reproposee (EN)");
    ASSERT_HAS    (items, "onDoubleClick", "cas1 : onDoubleClick non declare -> propose (EN)");
    ASSERT_NOT_HAS(items, "Trace",         "cas1 : pas de native generique");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Cas 2 — 1ere procedure AVEC corps realiste (Info + SI/POUR imbriques avec
 * leurs END internes). Le calcul blockStart/blockEnd (depth matching) ne doit
 * pas etre casse par les END internes -> EndInitialization reste filtre. */
static void test_dedup_lifecycle_with_body_en(void) {
    printf("\n[TEST] feat/0984 : dedup lifecycle avec corps non trivial (SI/POUR internes)\n");
    const char* src =
        "w_Test is a Window\n"
        "\tPROCEDURE EndInitialization()\n"
        "\t\tInfo(\"start\")\n"
        "\t\tIF 1 = 1 THEN\n"
        "\t\t\tInfo(\"branch\")\n"
        "\t\tEND\n"
        "\t\tFOR i = 1 TO 3\n"
        "\t\t\tInfo(i)\n"
        "\t\tEND\n"
        "\tEND\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Ligne 10 = "\tPROCEDURE " -> 1 tab + 10 chars = char 11. */
    JsonValue* params = make_completion_params(URI, 10, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "EndInitialization", "cas2 : EndInitialization (corps riche) -> filtre");
    ASSERT_NOT_HAS(items, "FinInitialisation", "cas2 : forme FR jamais reproposee");
    ASSERT_HAS    (items, "Closing",           "cas2 : Closing non declare -> propose");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Cas 3 — event declare APRES le curseur ( scanne tout le bloc
 * jusqu'a FIN). Curseur sur un `PROCEDURE ` place AVANT une PROCEDURE
 * EndInitialization() declaree plus bas -> EndInitialization filtre. */
static void test_dedup_event_declared_after_cursor_en(void) {
    printf("\n[TEST] feat/0984 : dedup event declare APRES le curseur (scan bloc complet)\n");
    const char* src =
        "w_Test is a Window\n"
        "\tPROCEDURE \n"
        "\tPROCEDURE EndInitialization()\n"
        "\t\tInfo(\"later\")\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Ligne 1 = "\tPROCEDURE " -> char 11. */
    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "EndInitialization", "cas3 : declare APRES curseur -> filtre");
    ASSERT_HAS    (items, "Closing",           "cas3 : Closing non declare -> propose");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Cas 4 — isolation multi-widgets : bouton A declare onClick, bouton B vide.
 * Dans A -> onClick filtre. Dans B -> onClick PROPOSE (pas utilise DANS B).
 * La dedup doit etre scopee au bloc courant, pas au fichier. */
static void test_dedup_multi_widget_isolation_en(void) {
    printf("\n[TEST] feat/0984 : dedup isolation multi-widgets (scope bloc, pas fichier)\n");
    const char* src =
        "w_Test is a Window\n"
        "\tBTN_A is a Field Button\n"
        "\t\tPROCEDURE onClick()\n"
        "\t\t\tInfo(\"A\")\n"
        "\t\tEND\n"
        "\t\tPROCEDURE \n"
        "\tEND\n"
        "\tBTN_B is a Field Button\n"
        "\t\tPROCEDURE \n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Curseur dans A : ligne 5 = "\t\tPROCEDURE " -> char 12. */
    JsonValue* paramsA = make_completion_params(URI, 5, 12);
    JsonValue* itemsA = handle_completion(paramsA);
    ASSERT_NOT_HAS(itemsA, "onClick",       "cas4 : onClick utilise DANS A -> filtre dans A");
    ASSERT_HAS    (itemsA, "onDoubleClick", "cas4 : onDoubleClick libre -> propose dans A");
    json_free(itemsA);
    json_free(paramsA);
    /* Curseur dans B : ligne 8 = "\t\tPROCEDURE " -> char 12. */
    JsonValue* paramsB = make_completion_params(URI, 8, 12);
    JsonValue* itemsB = handle_completion(paramsB);
    ASSERT_HAS    (itemsB, "onClick",       "cas4 : onClick NON utilise dans B -> propose dans B");
    json_free(itemsB);
    json_free(paramsB);
    lspServer.language[0] = '\0';
}

/* Cas 5 — forme mixte : event declare en FR canon (FinInitialisation) dans un
 * fichier a en-tete full-English (`is a Window`), completion langue=en ->
 * l'item EN EndInitialization doit etre filtre (reconnaissance croisee
 * FR canon <=> EN, coeur ). */
static void test_dedup_mixed_fr_decl_english_header_en(void) {
    printf("\n[TEST] feat/0984 : dedup forme mixte (FR canon declare, en-tete EN, mode EN)\n");
    const char* src =
        "w_Test is a Window\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tInfo(\"init\")\n"
        "\tEND\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    /* Ligne 4 = "\tPROCEDURE " -> char 11. */
    JsonValue* params = make_completion_params(URI, 4, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "EndInitialization", "cas5 : FinInitialisation FR declare -> EndInitialization EN filtre");
    ASSERT_NOT_HAS(items, "FinInitialisation", "cas5 : forme FR non reproposee (mode EN)");
    ASSERT_HAS    (items, "Closing",           "cas5 : Closing non declare -> propose");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Hover EN sur un event widget (surClic) -> desc EN, pas FR. */
static void test_hover_event_en(void) {
    printf("\n[TEST] fix/0693 : hover EN event (surClic) -> desc EN\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Champ Bouton\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 2, 13);  /* sur 'surClic' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Fires on left click", "hover event EN desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Declenche au clic",   "hover event EN no FR desc");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme event en FR (defaut) -> desc FR. */
static void test_hover_event_fr_default(void) {
    printf("\n[TEST] fix/0693 : hover FR (defaut) event (surClic) -> desc FR\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tBTN_A est un Champ Bouton\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 2, 13);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Declenche au clic",   "hover event FR desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Fires on left click", "hover event FR no EN desc");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — hover event contextuel pour la collision Fermeture/Closing.
 *  Cet event est declare dans le catalogue Etat (desc rapport) ET dans le
 *  lifecycle Fenetre (desc fenetre). Le hover doit servir la desc selon le
 *  bloc DSL englobant, pas toujours la desc rapport (ordre de recherche).
 * ================================================================ */

/* Contexte Fenetre : Fermeture -> desc fenetre, pas la desc rapport. */
static void test_hover_fermeture_fenetre_fr(void) {
    printf("\n[TEST] fix/0864 : hover Fermeture en contexte Fenetre -> desc fenetre\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE Fermeture()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 13);  /* sur 'Fermeture' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "ne se ferme", "hover Fermeture Fenetre desc fenetre");
    ASSERT_HOVER_NOT_CONTAINS(v, "rapport",     "hover Fermeture Fenetre no desc rapport");
    json_free(result);
    json_free(params);
}

/* Contexte Fenetre EN : Closing -> desc EN fenetre, pas la desc rapport. */
static void test_hover_closing_fenetre_en(void) {
    printf("\n[TEST] fix/0864 : hover Closing (EN) en contexte Fenetre -> desc EN fenetre\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE Closing()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 1, 13);  /* sur 'Closing' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "window closes", "hover Closing Fenetre EN desc fenetre");
    ASSERT_HOVER_NOT_CONTAINS(v, "report",        "hover Closing Fenetre EN no desc rapport");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Contexte Etat : Fermeture -> desc rapport preservee. */
static void test_hover_fermeture_etat_fr(void) {
    printf("\n[TEST] fix/0864 : hover Fermeture en contexte Etat -> desc rapport\n");
    const char* src =
        "ETAT_X est un Etat\n"
        "\tPROCEDURE Fermeture()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 13);  /* sur 'Fermeture' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "rapport",     "hover Fermeture Etat desc rapport");
    ASSERT_HOVER_NOT_CONTAINS(v, "ne se ferme", "hover Fermeture Etat no desc fenetre");
    json_free(result);
    json_free(params);
}

/* Hors de tout bloc DSL (top-level main.owl) : defaut = desc fenetre
 * (cas support #65 : code colle au top-level). */
static void test_hover_fermeture_top_level_fr(void) {
    printf("\n[TEST] fix/0864 : hover Fermeture hors bloc (top-level) -> desc fenetre\n");
    const char* src =
        "PROCEDURE Fermeture()\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 0, 12);  /* sur 'Fermeture' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "ne se ferme", "hover Fermeture top-level desc fenetre");
    ASSERT_HOVER_NOT_CONTAINS(v, "rapport",     "hover Fermeture top-level no desc rapport");
    json_free(result);
    json_free(params);
}

/* Garde-fou : un event non-collision (FinInitialisation) reste inchange en
 * contexte Fenetre. */
static void test_hover_fininitialisation_fenetre_unchanged(void) {
    printf("\n[TEST] fix/0864 : hover FinInitialisation en contexte Fenetre -> inchange\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 13);  /* sur 'FinInitialisation' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "sous-champs", "hover FinInitialisation desc inchangee");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — bilinguisme LSP : derniers trous
 *    (a) mots-cles : completion + hover EN (label_en/descEN du catalogue
 *        keyword via owl_keyword_find_by_word)
 *    (c) hover des props DSL (OWL_DSL_PROP_CATALOG, nameEN/descEN)
 *
 *  RED-FIRST : avant impl, la completion mots-cles emet la forme FR et
 *  la doc FR meme en mode EN ; le hover mots-cles ignore wantEN ; le
 *  hover des props DSL n'existe pas (retour null). Ces tests rougissent.
 * ================================================================ */

/* (c) Hover sur une prop DSL (Titre/Title de Fenetre) en mode EN : doit
 *     servir le titre nameEN + la desc EN, pas la desc FR. */
static void test_hover_dsl_prop_en(void) {
    printf("\n[TEST] fix/0697 : hover EN prop DSL (Title) -> desc EN\n");
    const char* src =
        "FEN_X is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"x\"\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 2, 4);  /* sur 'Title' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "Title", "hover prop DSL EN titre");
    /* La desc EN du catalogue parle de la barre de titre (caption). */
    ASSERT_HOVER_CONTAINS(v, "Title bar text", "hover prop DSL EN desc");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme prop en FR (defaut) -> titre + desc FR. */
static void test_hover_dsl_prop_fr_default(void) {
    printf("\n[TEST] fix/0697 : hover FR (defaut) prop DSL (Titre) -> desc FR\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"x\"\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 2, 4);  /* sur 'Titre' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Titre",     "hover prop DSL FR titre");
    ASSERT_HOVER_CONTAINS    (v, "barre de titre", "hover prop DSL FR desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Title bar text", "hover prop DSL FR no EN desc");
    json_free(result);
    json_free(params);
}

/* (a) Completion mots-cles en mode EN : la forme EN du mot-cle (IF pour
 *     SI) est proposee, la doc EN l'accompagne, la forme FR n'apparait pas. */
static void test_completion_keyword_en(void) {
    printf("\n[TEST] fix/0697 : completion EN mots-cles -> IF/WHILE + doc EN\n");
    const char* src = "x = 1\n\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 1, 0);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "IF",      "completion keyword EN (SI->IF)");
    ASSERT_HAS    (items, "WHILE",   "completion keyword EN (TANTQUE->WHILE)");
    ASSERT_NOT_HAS(items, "SI",      "completion keyword EN no FR (SI)");
    ASSERT_NOT_HAS(items, "TANTQUE", "completion keyword EN no FR (TANTQUE)");
    /* doc EN attachee au mot-cle IF (descEN du sens SI). */
    ASSERT_DOC_CONTAINS(items, "IF", "condition", "completion keyword EN doc");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : completion FR (defaut) garde la forme + doc FR. */
static void test_completion_keyword_fr_default(void) {
    printf("\n[TEST] fix/0697 : completion FR (defaut) mots-cles -> SI/TANTQUE\n");
    const char* src = "x = 1\n\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_completion_params(URI, 1, 0);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "SI",      "completion keyword FR (SI)");
    ASSERT_HAS    (items, "TANTQUE", "completion keyword FR (TANTQUE)");
    ASSERT_NOT_HAS(items, "IF",      "completion keyword FR no EN (IF)");
    ASSERT_NOT_HAS(items, "WHILE",   "completion keyword FR no EN (WHILE)");
    json_free(items);
    json_free(params);
}

/* (a) Hover mot-cle en mode EN : titre du sens EN (label_en) + desc EN. */
static void test_hover_keyword_en(void) {
    printf("\n[TEST] fix/0697 : hover EN mot-cle (SI) -> sens + desc EN\n");
    const char* src = "SI x ALORS\nFIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 0, 0);  /* sur 'SI' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Conditional branching", "hover keyword EN label");
    ASSERT_HOVER_CONTAINS    (v, "Runs the ALORS block",  "hover keyword EN desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Branchement conditionnel", "hover keyword EN no FR label");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : hover mot-cle FR (defaut) -> sens + desc FR. */
static void test_hover_keyword_fr_default(void) {
    printf("\n[TEST] fix/0697 : hover FR (defaut) mot-cle (SI) -> sens + desc FR\n");
    const char* src = "SI x ALORS\nFIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 0, 0);  /* sur 'SI' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "bloc ALORS",          "hover keyword FR desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Runs the ALORS block", "hover keyword FR no EN desc");
    json_free(result);
    json_free(params);
}

/*  — le mot-cle GLOBAL (bascule de portee top-level, exporte la
 * declaration au-dela du fichier) doit apparaitre en completion. Avant le
 * fix, seule la native awsGlobalAccelerator sortait sur `global`. */
static void test_completion_keyword_global_fr(void) {
    printf("\n[TEST] fix/1925 : completion FR contient le mot-cle GLOBAL\n");
    const char* src = "x = 1\n\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_completion_params(URI, 1, 0);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "GLOBAL", "completion keyword GLOBAL present");
    json_free(items);
    json_free(params);
}

/*  — hover sur `global` : fiche non vide decrivant la portee projet. */
static void test_hover_keyword_global_fr(void) {
    printf("\n[TEST] fix/1925 : hover FR mot-cle (global) -> fiche portee projet\n");
    const char* src = "global gCompteur est un entier = 0\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 0, 0);  /* sur 'global' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "portée du projet", "hover keyword GLOBAL desc FR");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — hover bilingue : le CORPS suit la langue active
 *
 *  Le titre passait deja en EN mais le corps restait FR
 *  car la donnee EN du registry (descEN type/champ, nameEN type de
 *  champ/retour/param) n'etait pas plumbee dans le catalogue LSP.
 *
 *  RED-FIRST : avant impl le catalogue n'a pas descEN(type),
 *  nameEN/descEN/typeEN(champ), returnTypeEN(sig), typeEN(param) ->
 *  ces tests rougissent. Apres impl -> verts. Le mode FR ne regresse pas.
 * ================================================================ */

/* Hover EN sur un type composé (Timer) : description EN + champ EN
 * (nom `Interval`, type `duration`, desc EN). */
static void test_hover_type_body_en(void) {
    printf("\n[TEST] fix/0706 : hover EN sur type (Timer) -> corps EN\n");
    const char* src = "t est un Timer\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 0, 9);  /* sur 'Timer' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Periodic timer",          "hover type EN desc");
    ASSERT_HOVER_CONTAINS    (v, "Interval",                "hover type EN champ nom");
    ASSERT_HOVER_CONTAINS    (v, "duration",                "hover type EN champ type");
    ASSERT_HOVER_CONTAINS    (v, "period between two triggers", "hover type EN champ desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Minuteur",                "hover type EN no FR desc");
    ASSERT_HOVER_NOT_CONTAINS(v, "Intervalle",              "hover type EN no FR champ nom");
    ASSERT_HOVER_NOT_CONTAINS(v, "p\xc3\xa9riode entre",    "hover type EN no FR champ desc");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme type en FR (defaut) garde le corps FR. */
static void test_hover_type_body_fr_default(void) {
    printf("\n[TEST] fix/0706 : hover FR (defaut) sur type (Timer) -> corps FR\n");
    const char* src = "t est un Timer\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 0, 9);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Minuteur",       "hover type FR desc");
    ASSERT_HOVER_CONTAINS    (v, "Intervalle",     "hover type FR champ nom");
    ASSERT_HOVER_NOT_CONTAINS(v, "Periodic timer", "hover type FR no EN desc");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — hover sur une propriété callback : la bulle affiche la
 *  signature attendue de la procédure (en plus de la description).
 *  Se nourrit de OwlFieldInfo.callbackSig (posé par ).
 * ================================================================ */

/* Hover FR sur `opts..SurToken` (OptionsGénération) -> signature (chaîne, booléen). */
static void test_hover_callback_sig_fr(void) {
    printf("\n[TEST] feat/1711 : hover FR callback ..SurToken -> signature\n");
    const char* src = "opts est un OptionsG\xc3\xa9n\xc3\xa9ration\nopts..SurToken = p\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 9);  /* sur 'SurToken' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "Signature attendue",   "hover callback FR label");
    ASSERT_HOVER_CONTAINS(v, "(cha\xc3\xaene, bool\xc3\xa9""en)",
                          "hover callback FR signature exacte");
    ASSERT_HOVER_CONTAINS(v, "morceau",              "hover callback FR desc conservee");
    json_free(result);
    json_free(params);
}

/* Hover EN sur `opts..SurToken` -> Expected signature (string, boolean). */
static void test_hover_callback_sig_en(void) {
    printf("\n[TEST] feat/1711 : hover EN callback ..SurToken -> signature EN\n");
    const char* src = "opts est un OptionsG\xc3\xa9n\xc3\xa9ration\nopts..SurToken = p\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 1, 9);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "Expected signature", "hover callback EN label");
    ASSERT_HOVER_CONTAINS    (v, "(string, boolean)",  "hover callback EN signature exacte");
    ASSERT_HOVER_NOT_CONTAINS(v, "Signature attendue", "hover callback EN no FR label");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Signature multi-param `optsE..SurÉpoque` (OptionsEntraînement) -> (entier, entier, réel). */
static void test_hover_callback_sig_multi(void) {
    printf("\n[TEST] feat/1711 : hover FR callback ..Sur\xc3\x89poque -> 3 params\n");
    const char* src = "optsE est un OptionsEntra\xc3\xaenement\noptsE..Sur\xc3\x89poque = p\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 10);  /* sur 'SurÉpoque' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "(entier, entier, r\xc3\xa9""el)",
                          "hover callback multi signature exacte");
    json_free(result);
    json_free(params);
}

/* Callback zéro-paramètre `t..Procédure` (Timer, callbackSig "") -> (aucun paramètre). */
static void test_hover_callback_sig_zero_param(void) {
    printf("\n[TEST] feat/1711 : hover FR callback zero-param (Timer) -> aucun parametre\n");
    const char* src = "t est un Timer\nt..Proc\xc3\xa9""dure = p\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 6);  /* sur 'Procédure' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "Signature attendue",         "hover zero-param FR label");
    ASSERT_HOVER_CONTAINS(v, "(aucun param\xc3\xa8tre)",    "hover zero-param FR mention");
    json_free(result);
    json_free(params);
}

/* Non-régression : un champ NON callback (`opts..Température`, callbackSig NULL)
 * n'affiche AUCUNE ligne de signature (le hover retombe sur l'existant). */
static void test_hover_non_callback_field_unchanged(void) {
    printf("\n[TEST] feat/1711 : hover champ non-callback -> pas de signature\n");
    const char* src = "opts est un OptionsG\xc3\xa9n\xc3\xa9ration\nopts..Temp\xc3\xa9rature = 0.5\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 1, 10);  /* sur 'Température' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_NOT_CONTAINS(v, "Signature attendue",
                              "hover champ non-callback sans signature");
    json_free(result);
    json_free(params);
}

/* Hover EN sur une native sans retour (TimerDémarre/TimerStart) :
 * `Returns : nothing` (le mot du return_type traduit), pas `rien`. */
static void test_hover_native_return_en(void) {
    printf("\n[TEST] fix/0706 : hover EN native -> Returns nothing\n");
    const char* src = "TimerStart(t)\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 0, 2);  /* sur 'TimerStart' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "nothing", "hover native EN return nothing");
    ASSERT_HOVER_NOT_CONTAINS(v, "`rien`", "hover native EN no FR rien");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme native en FR (defaut) garde `rien`. */
static void test_hover_native_return_fr_default(void) {
    printf("\n[TEST] fix/0706 : hover FR (defaut) native -> Retourne rien\n");
    const char* src = "TimerStart(t)\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 0, 2);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "rien", "hover native FR return rien");
    json_free(result);
    json_free(params);
}

/* Hover EN sur un event avec param type (surMolette/onWheel sur Image) :
 * le type du param est rendu `integer`, pas `entier`. */
static void test_hover_event_param_type_en(void) {
    printf("\n[TEST] fix/0706 : hover EN event -> param type integer\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tIMG_A est un Champ Image\n"
        "\t\tPROCEDURE surMolette(mouvement, x, y)\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_hover_params(URI, 2, 14);  /* sur 'surMolette' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "integer", "hover event EN param type integer");
    ASSERT_HOVER_NOT_CONTAINS(v, "entier",  "hover event EN no FR entier");
    json_free(result);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : meme event en FR (defaut) garde `entier`. */
static void test_hover_event_param_type_fr_default(void) {
    printf("\n[TEST] fix/0706 : hover FR (defaut) event -> param type entier\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tIMG_A est un Champ Image\n"
        "\t\tPROCEDURE surMolette(mouvement, x, y)\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    JsonValue* params = make_hover_params(URI, 2, 14);
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "entier",  "hover event FR param type entier");
    ASSERT_HOVER_NOT_CONTAINS(v, "integer", "hover event FR no EN integer");
    json_free(result);
    json_free(params);
}

/* V1.7-2 : verifie la coherence interne du OWL_COMPOSED_TYPE_CATALOG.
 * OwlComposedTypeField + OWL_COMPOSED_TYPE_CATALOG sont declares dans
 * owl_catalog.h (deja inclus). */
static void test_composed_types_coherence(void) {
    printf("\n[TEST] Lot V1.7-2 : OWL_COMPOSED_TYPE_CATALOG coherence\n");
    int issues = 0;
    for (int i = 0; i < OWL_COMPOSED_TYPE_CATALOG_COUNT; i++) {
        const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[i];
        if (!f->subTypeName) continue;
        if (strcmp(f->fieldType, "instance") == 0) {
            int found = 0;
            for (int j = 0; j < OWL_COMPOSED_TYPE_CATALOG_COUNT; j++) {
                if (strcmp(OWL_COMPOSED_TYPE_CATALOG[j].typeName, f->subTypeName) == 0) {
                    found = 1; break;
                }
            }
            if (!found) {
                g_fail++; issues++;
                printf("  [FAIL] %s.%s reference sous-type 'instance' '%s' "
                       "absent du catalog\n",
                       f->typeName, f->fieldName, f->subTypeName);
            }
        } else if (strcmp(f->fieldType, "enum") == 0) {
            if (!owl_enum_info_find(f->subTypeName)) {
                g_fail++; issues++;
                printf("  [FAIL] %s.%s reference sous-type 'enum' '%s' "
                       "absent de OWL_ENUM_CATALOG\n",
                       f->typeName, f->fieldName, f->subTypeName);
            }
        }
    }
    if (issues == 0) {
        g_pass++;
        printf("  [OK]   coherence : %d entrees verifiees\n",
               OWL_COMPOSED_TYPE_CATALOG_COUNT);
    }
}

/* Init du runtime necessaire au LSP (catalogue events des widgets). Le vrai
 * serveur appelle ca dans lsp/lsp_main.c — sans cet appel le harness avait
 * un catalogue d'events vide, ce qui faisait silencieusement echouer les
 * tests d'autocomplete d'events typés (surEntrée, surSortie, etc.). */
extern void event_catalog_widgets_init(void);

/* POUR TOUTE POSITION ... AVEC <cursor> : propose les 4 valeurs de
 * l'enum optionsRechercheChaine. */
static void test_pour_toute_position_avec_completion(void) {
    printf("\n[TEST] POUR TOUTE POSITION ... AVEC <cursor> propose les options\n");
    const char* src = "POUR TOUTE POSITION p DE \"x\" DANS s AVEC \n";
    setup_doc(URI, src);
    /* Curseur fin de ligne (apres "AVEC "). Ligne 0 char 41. */
    JsonValue* params = make_completion_params(URI, 0, 41);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "MotComplet",          "AVEC propose MotComplet");
    ASSERT_HAS    (items, "SansCasse",           "AVEC propose SansCasse");
    ASSERT_HAS    (items, "DepuisFin",           "AVEC propose DepuisFin");
    ASSERT_HAS    (items, "DepuisD\xc3\xa9" "but", "AVEC propose DepuisDebut");
    /* Ne doit pas polluer avec autre chose dans ce contexte strict. */
    ASSERT_NOT_HAS(items, "Trace",                "AVEC : pas de native");
    ASSERT_NOT_HAS(items, "AVEC",                 "AVEC : pas le keyword AVEC en double");
    json_free(items);
    json_free(params);
}

/* POUR TOUTE POSITION ... <cursor> sans AVEC encore tape : propose
 * le keyword AVEC + les 4 valeurs (utilisables en suffix positionnel
 * pour DepuisFin / DepuisDebut). */
static void test_pour_toute_position_suffix_completion(void) {
    printf("\n[TEST] POUR TOUTE POSITION ... <cursor> propose AVEC + options\n");
    const char* src = "POUR TOUTE POSITION p DE \"x\" DANS s \n";
    setup_doc(URI, src);
    /* Curseur ligne 0 char 35 (apres "DANS s "). */
    JsonValue* params = make_completion_params(URI, 0, 35);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AVEC",                 "Suffix : propose AVEC");
    ASSERT_HAS(items, "DepuisFin",            "Suffix : propose DepuisFin");
    ASSERT_HAS(items, "DepuisD\xc3\xa9" "but","Suffix : propose DepuisDebut");
    ASSERT_HAS(items, "MotComplet",           "Suffix : propose MotComplet");
    ASSERT_HAS(items, "SansCasse",            "Suffix : propose SansCasse");
    json_free(items);
    json_free(params);
}

/* Phase 2a — ChaineFormate options propose les 10 ccXxx via
 * E_chaineFormateOptions (sur-ensemble de chaineCompare). */
static void test_chaine_formate_propose_cc_options(void) {
    printf("\n[TEST] ChaineFormate(s, <cursor>) propose les options ccXxx\n");
    const char* src = "s est une chaine = \"hello\"\nx est une chaine = Cha\xc3\xae" "neFormate(s, )\n";
    setup_doc(URI, src);
    /* Curseur ligne 1, position apres "ChaineFormate(s, " — character 38 */
    JsonValue* params = make_completion_params(URI, 1, 38);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "ccMajuscule",   "ChaineFormate propose ccMajuscule");
    ASSERT_HAS(items, "ccMinuscule",   "ChaineFormate propose ccMinuscule");
    ASSERT_HAS(items, "ccSansAccent",  "ChaineFormate propose ccSansAccent");
    ASSERT_HAS(items, "ccSansCasse",   "ChaineFormate propose ccSansCasse");
    json_free(items);
    json_free(params);
}

/* Phase 2b — acceptsLiterals=true : ListeDeplaceLigne.destination
 * propose les tdXxx ET garde le catalogue general (variables, Trace).
 * Si acceptsLiterals etait faux/absent, on ne verrait que les tdXxx. */
static void test_accepts_literals_destination(void) {
    printf("\n[TEST] ListeDeplaceLigne(l, n, <cursor>) propose tdXxx + catalogue general\n");
    const char* src = "PROCEDURE P()\n\tListeDeplaceLigne(LST_X, 1, )\nFIN\n";
    setup_doc(URI, src);
    /* Curseur ligne 1, character 30 (apres "1, "). */
    JsonValue* params = make_completion_params(URI, 1, 30);
    JsonValue* items = handle_completion(params);
    /* Les cibles relatives doivent etre la (enum tdCibleRelative). */
    ASSERT_HAS(items, "tdPremier", "destination propose tdPremier (enum cible relative)");
    ASSERT_HAS(items, "tdSuivant", "destination propose tdSuivant");
    /* Catalogue general doit AUSSI etre la (acceptsLiterals=true). */
    ASSERT_HAS(items, "Trace",     "destination garde catalogue general (Trace present)");
    json_free(items);
    json_free(params);
}

/* Hors d'un POUR TOUTE POSITION, on ne doit pas court-circuiter le
 * catalogue general (= "Trace" et autres natives doivent rester
 * presents). Garantit que ma detection ne hijacke pas les autres
 * contextes par erreur. */
static void test_avec_block_not_position_context(void) {
    printf("\n[TEST] Bloc AVEC sans POSITION : catalogue general intact\n");
    const char* src = "AVEC monObjet\n\t\n";
    setup_doc(URI, src);
    /* Curseur dans le corps AVEC = ligne 1 (après la tab). NB  :
     * l'ancienne position (0,14) pointait AU-DELÀ du `\n` de la ligne 0
     * (13 chars) ; elle ne « marchait » que parce que l'ancien
     * `cursor_offset += character` brut franchissait le newline pour
     * atterrir sur la ligne 1. Avec la conversion UTF-16→octet (clamp au
     * `\n`), on exprime la vraie intention : curseur sur la ligne vide. */
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    /* Trace est une native standard, doit etre presente dans le general. */
    ASSERT_HAS(items, "Trace", "AVEC bloc : catalogue general non hijacke");
    json_free(items);
    json_free(params);
}

/*  — Bug A : sur un contexte a gros catalogue (>800 items),
 * handle_completion doit retourner un tableau NU d'items, jamais un
 * CompletionList { isIncomplete, items }. Le wrap unique est la
 * responsabilite de lsp_server.c ; un double-wrap produit
 * result.items = { isIncomplete, items:[...] } (objet au lieu de
 * tableau) -> VSCode "No Suggestions". */
static void test_completion_single_wrap_large_context(void) {
    printf("\n[TEST] fix/0810 : gros contexte -> tableau nu (pas de double-wrap)\n");
    const char* src = "AVEC monObjet\n\t\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    /* Retour doit etre un tableau JSON direct, jamais un objet
     * CompletionList (sinon lsp_server.c re-wrappe -> double nesting). */
    if (items && items->type == JSON_ARRAY) {
        g_pass++;
        printf("  [OK]   single-wrap : retour tableau nu\n");
    } else {
        g_fail++;
        printf("  [FAIL] single-wrap : retour type=%d (attendu JSON_ARRAY=%d)\n",
               items ? (int)items->type : -1, (int)JSON_ARRAY);
    }
    /* Sanity : le contexte doit bien depasser 800 items (sinon le test
     * n'exerce pas le chemin fautif du cap). */
    if (items && items->type == JSON_ARRAY && items->as.arrayVal.count > 800) {
        g_pass++;
        printf("  [OK]   single-wrap : contexte >800 items (chemin cap exerce)\n");
    } else {
        g_fail++;
        printf("  [FAIL] single-wrap : contexte <=800 items, chemin cap non exerce\n");
    }
    json_free(items);
    json_free(params);
}

/*  — Bug B : fChargeTexte a 2 signatures ; l'enum owlEncodage
 * est porte par le 2e param de signatures[1] (signatures[0] n'a qu'un
 * param). La completion au 2e arg doit parcourir TOUTES les sigs et
 * proposer les membres owlEncodage filtres strict, pas le catalogue
 * general. encUTF8 est aussi une constante du catalogue general : on
 * discrimine via le detail "Membre de" + l'absence du catalogue
 * (fOuvre/Trace). */
static void test_completion_enum_callarg_overloaded_sig(void) {
    printf("\n[TEST] fix/0810 : fChargeTexte 2e arg -> membres owlEncodage (sig surchargee)\n");
    const char* src = "res est une chaine = fChargeTexte(\"f.txt\", )\n";
    setup_doc(URI, src);
    /* Replique l'etat serveur reel : langue "fr" (default prod), PAS vide.
     * Un test qui vidait la langue passait alors que le chemin serveur
     * reel echouait sur une repro mal echappee ; on ancre desormais la
     * langue exacte du dev. */
    snprintf(lspServer.language, sizeof(lspServer.language), "fr");
    /* Curseur juste apres la virgule + espace (arg index 1). */
    const char* comma = strchr(src, ',');
    int col = (int)((comma + 2) - src);
    JsonValue* params = make_completion_params(URI, 0, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "encUTF8",        "fChargeTexte arg2 propose encUTF8");
    ASSERT_HAS(items, "encWindows1252", "fChargeTexte arg2 propose encWindows1252");
    ASSERT_DETAIL_PREFIX(items, "encUTF8", "Membre de",
                         "fChargeTexte arg2 detail enum (Membre de)");
    /* Filtre strict (acceptsLiterals=false) : le catalogue general ne
     * doit PAS fuiter. */
    ASSERT_NOT_HAS(items, "fOuvre", "fChargeTexte arg2 sans catalogue general (fOuvre)");
    ASSERT_NOT_HAS(items, "Trace",  "fChargeTexte arg2 sans catalogue general (Trace)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';  /* reset defaut pour tests suivants */
}

/*  — Scenario 1 : ':' dans methode inline d'une classe propose
 * les champs (et methodes) de la classe courante. */
static void test_colon_proposes_class_fields(void) {
    printf("\n[TEST] fix/0104 ':' dans methode inline propose champs classe\n");
    const char* src =
        "cPersonne est une Classe\n"
        "PUBLIC\n"
        "\tsNom est une cha\xc3\xaene\n"           /* sNom est une chaîne */
        "\tnAge est un entier\n"
        "\n"
        "\tPROCEDURE Saluer()\n"
        "\t\tTrace(:)\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 6 (0-indexed) = "\t\tTrace(:)", curseur juste apres ':'.
     * Indent = 2 tabs (2 chars), puis "Trace(:" = 7 chars → col 9. */
    JsonValue* params = make_completion_params(URI, 6, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "sNom",   "colon-member : champ public");
    ASSERT_HAS(items, "nAge",   "colon-member : champ public");
    ASSERT_HAS(items, "Saluer", "colon-member : methode publique");
    /* Anti faux-positif : sans logique colon-member, le LSP ne devrait
     * proposer que des items "globaux" (natifs/keywords). On verifie
     * qu'au moins un item etranger a la classe N'est PAS la quand on
     * a vraiment scope-colon (filtrage restreint aux membres de classe). */
    ASSERT_NOT_HAS(items, "Trace", "colon-member : pas de native (scope restreint)");
    json_free(items);
    json_free(params);
}

/*  — Scenario 2 : 'obj.' sur variable typee classe propose
 * les membres PUBLICs de la classe. */
static void test_dot_on_class_var_proposes_public_members(void) {
    printf("\n[TEST] fix/0104 'p.' propose membres publics de la classe\n");
    const char* src =
        "cPersonne est une Classe\n"
        "PUBLIC\n"
        "\tsNom est une cha\xc3\xaene\n"
        "\tnAge est un entier\n"
        "\tPROCEDURE Saluer()\n"
        "\t\tTrace(\"hi\")\n"
        "\tFIN\n"
        "PRIVEE\n"
        "\tnSecret est un entier\n"
        "FIN\n"
        "\n"
        "p est un cPersonne\n"
        "p.\n";
    setup_doc(URI, src);
    /* Derniere ligne = "p.", curseur apres '.', col 2.
     * Lignes (0-indexed) : 0..9 classe, 10 vide, 11 "p est…", 12 "p." */
    JsonValue* params = make_completion_params(URI, 12, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "sNom",    "p. : champ public");
    ASSERT_HAS    (items, "Saluer",  "p. : methode publique");
    ASSERT_NOT_HAS(items, "nSecret", "p. : champ prive filtre");
    json_free(items);
    json_free(params);
}

/*  — Scenario 3 : Ctrl+Espace top-level dans body methode propose
 * la fusion (natives + champs classe englobante). Sans prefixe tape pour
 * eviter le filtre server-side prefix (>300 items). */
static void test_ident_in_method_body_mixes_locals_and_fields(void) {
    printf("\n[TEST] fix/0104 Ctrl+Espace body methode : natives + champs\n");
    const char* src =
        "cPersonne est une Classe\n"
        "PUBLIC\n"
        "\tsNom est une cha\xc3\xaene\n"
        "\tPROCEDURE Saluer()\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 4 = "\t\t", curseur apres les 2 tabs : col 2.
     * Aucun prefixe alpha → pas de filtre server-side. */
    JsonValue* params = make_completion_params(URI, 4, 2);
    JsonValue* items = handle_completion(params);
    /* Trace est natif standard, doit etre present (non-regression). */
    ASSERT_HAS(items, "Trace",  "body methode : native presente");
    /* sNom = champ de classe courante, doit etre propose en suggestion. */
    ASSERT_HAS(items, "sNom",   "body methode : champ classe propose");
    json_free(items);
    json_free(params);
}

/*  — autocomplete dans le `{}` d'une init typee chaineMultilingue.
 * Le type expose un literalKeysEnum (&E_langueXxx) ; le LSP doit proposer
 * les valeurs de cet enum (langueFrancais / langueAnglais / ...). */
static void test_literal_init_chaine_multilingue(void) {
    printf("\n[TEST] feat/0111 : `s est une chaineMultilingue = { <cursor>` propose les langues\n");
    const char* src =
        "sTest est une chaineMultilingue = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "literal init : langueFrancais");
    ASSERT_HAS(items, "langueAnglais",            "literal init : langueAnglais");
    json_free(items);
    json_free(params);
}

/* Robustesse : 2e cle dans le `{}` deja partiellement rempli. */
static void test_literal_init_chaine_multilingue_second_key(void) {
    printf("\n[TEST] feat/0111 : 2e cle dans `{ langueAnglais: \"x\", <cursor> }`\n");
    const char* src =
        "sTest est une chaineMultilingue = { langueAnglais: \"x\",\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "literal init 2e cle : langueFrancais");
    ASSERT_HAS(items, "langueAnglais",            "literal init 2e cle : langueAnglais");
    json_free(items);
    json_free(params);
}

/* Un type SANS literalKeysEnum (Date) dans le meme contexte `{}` ne doit
 * pas polluer avec des langues hallucinees. */
static void test_literal_init_type_without_enum_no_pollution(void) {
    printf("\n[TEST] feat/0111 : `d est une Date = { <cursor>` ne crache pas de langue\n");
    const char* src =
        "d est une Date = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "langueFran\xc3\xa7" "ais", "Date {} : pas de langueFrancais");
    ASSERT_NOT_HAS(items, "langueAnglais",            "Date {} : pas de langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — meme feature (literalKeysEnum → langues) mais quand la
 * variable est declaree SEPAREMENT de son affectation. lhsName = "m"
 * (nom de variable), pas un nom de type ; il faut scanner en arriere
 * `m est une chaineMultilingue` pour resoudre le type. RED avant fix
 * (aucune langue proposee car owl_type_info_find("m") == NULL). */
static void test_literal_init_chaine_multilingue_decl_separee(void) {
    printf("\n[TEST] fix/0860 : decl separee `m est une chaineMultilingue` puis `m = { <cursor>` propose les langues\n");
    const char* src =
        "m est une chaineMultilingue\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "decl separee : langueFrancais");
    ASSERT_HAS(items, "langueAnglais",            "decl separee : langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — non-pollution : une variable declaree separement mais de
 * type SANS literalKeysEnum (entier) ne doit PAS voir de langues. Le
 * scan-arriere resout le type declare, qui n'a pas de literalKeysEnum
 * → literalTypeKnownNoEnum preserve. */
static void test_literal_init_decl_separee_non_multilingue_no_pollution(void) {
    printf("\n[TEST] fix/0860 : decl separee `n est un entier` puis `n = { <cursor>` ne crache pas de langue\n");
    const char* src =
        "n est un entier\n"
        "n = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "langueFran\xc3\xa7" "ais", "decl separee entier : pas de langueFrancais");
    ASSERT_NOT_HAS(items, "langueAnglais",            "decl separee entier : pas de langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — le scan-arriere du type declare doit reconnaitre la copule
 * ANGLAISE `is a/an` en plus de `est un[e]` (parite FR/EN). RED avant fix
 * (le scan ne matchait que "est un", donc `m is a chaineMultilingue` ne
 * resolvait jamais le type → aucune langue proposee). */
static void test_literal_init_chaine_multilingue_decl_en_is_a(void) {
    printf("\n[TEST] fix/1033 : decl separee EN `m is a chaineMultilingue` puis `m = { <cursor>` propose les langues\n");
    const char* src =
        "m is a chaineMultilingue\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "decl EN is a : langueFrancais");
    ASSERT_HAS(items, "langueAnglais",            "decl EN is a : langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — forme MIXTE copule EN + article FR `is un` : c'est le cas
 * exact de l'image du support (#61). Doit resoudre le type comme les autres. */
static void test_literal_init_chaine_multilingue_decl_mixte_is_un(void) {
    printf("\n[TEST] fix/1033 : decl separee mixte `m is un chaineMultilingue` (image #61) propose les langues\n");
    const char* src =
        "m is un chaineMultilingue\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "decl mixte is un : langueFrancais");
    ASSERT_HAS(items, "langueAnglais",            "decl mixte is un : langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — forme entierement EN : copule `is an` + nom EN du type
 * (multilingualString). Verifie que le scan bilingue + le lookup alias EN
 * cooperent bout-en-bout. */
static void test_literal_init_chaine_multilingue_decl_en_full(void) {
    printf("\n[TEST] fix/1033 : decl separee EN complete `m is an multilingualString` propose les langues\n");
    const char* src =
        "m is an multilingualString\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "decl EN full : langueFrancais");
    ASSERT_HAS(items, "langueAnglais",            "decl EN full : langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — non-pollution en mode EN : `n is a entier` (type sans
 * literalKeysEnum) ne doit PAS proposer de langue. Le scan bilingue
 * resout bien le type mais celui-ci n'a pas d'enum de cles. */
static void test_literal_init_decl_en_non_multilingue_no_pollution(void) {
    printf("\n[TEST] fix/1033 : decl separee EN `n is a entier` ne crache pas de langue\n");
    const char* src =
        "n is a entier\n"
        "n = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "langueFran\xc3\xa7" "ais", "decl EN entier : pas de langueFrancais");
    ASSERT_NOT_HAS(items, "langueAnglais",            "decl EN entier : pas de langueAnglais");
    json_free(items);
    json_free(params);
}

/*  — scan-arriere durée : la copule ANGLAISE `is a`
 * doit resoudre le type durée declare separement, comme `est un[e]`. RED
 * avant fix (le scan matchait seulement "est un" → `m is a durée` ne
 * declenchait jamais isDureeLiteral → aucune composante proposee). */
static void test_literal_init_duree_decl_en_is_a(void) {
    printf("\n[TEST] fix/1039 : decl separee EN `m is a durée` puis `m = { <cursor>` propose les composantes\n");
    const char* src =
        "m is a dur\xc3\xa9""e\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Heure",  "duree decl EN is a : Heure");
    ASSERT_HAS(items, "Minute", "duree decl EN is a : Minute");
    json_free(items);
    json_free(params);
}

/*  — forme MIXTE copule EN + article FR `is un` sur durée. */
static void test_literal_init_duree_decl_mixte_is_un(void) {
    printf("\n[TEST] fix/1039 : decl separee mixte `m is un durée` puis `m = { <cursor>` propose les composantes\n");
    const char* src =
        "m is un dur\xc3\xa9""e\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Heure",  "duree decl mixte is un : Heure");
    ASSERT_HAS(items, "Minute", "duree decl mixte is un : Minute");
    json_free(items);
    json_free(params);
}

/*  — non-regression FR canon `est une durée` : composantes toujours
 * proposees apres branchement sur le helper bilingue. */
static void test_literal_init_duree_decl_fr(void) {
    printf("\n[TEST] fix/1039 : non-reg FR `m est une durée` propose les composantes\n");
    const char* src =
        "m est une dur\xc3\xa9""e\n"
        "m = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Heure",  "duree decl FR : Heure");
    ASSERT_HAS(items, "Minute", "duree decl FR : Minute");
    json_free(items);
    json_free(params);
}

/*  — nom de type EN `duration` (pas seulement `durée`/`duree`
 * matché en dur par le cas spécial) doit résoudre le littéral `{}` et
 * proposer les composantes de construction en forme EN (Hour/Minute/
 * Second/Day/Millisecond). RED avant fix : isDureeLiteral matche
 * uniquement "durée"/"duree" en dur (L6585-6587), et owl_type_info_find
 * ("duration") échoue car durée n'a pas d'entrée dans OWL_TYPE_CATALOG
 * (fields absents) -> literalTypeKnownNoEnum -> aucune composante. */
static void test_literal_init_duree_decl_en_type_duration(void) {
    printf("\n[TEST] fix/1043 : decl separee `d is a duration` puis `d = { <cursor>` propose les composantes EN\n");
    const char* src =
        "d is a duration\n"
        "d = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Hour",        "duree type EN 'duration' : Hour");
    ASSERT_HAS(items, "Minute",      "duree type EN 'duration' : Minute");
    ASSERT_HAS(items, "Second",      "duree type EN 'duration' : Second");
    ASSERT_HAS(items, "Day",         "duree type EN 'duration' : Day");
    ASSERT_HAS(items, "Millisecond", "duree type EN 'duration' : Millisecond");
    json_free(items);
    json_free(params);
}

/*  — mode langue LSP=en (wantEN) sur une déclaration FR `durée`
 * doit servir les composantes EN, pas uniquement FR. RED avant fix : le
 * cas spécial isDureeLiteral (L6752-6802) ignore totalement wantEN et
 * émet toujours la liste FR hardcodée (Jour/Heure/Minute/Seconde/
 * Milliseconde), même quand lspServer.language == "en". */
static void test_literal_init_duree_wanten_composantes_en(void) {
    printf("\n[TEST] fix/1043 : mode langue EN sur `d est une durée = { <cursor>` propose les composantes EN\n");
    const char* src =
        "d est une dur\xc3\xa9""e\n"
        "d = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Hour",        "duree wantEN : Hour");
    ASSERT_HAS(items, "Day",         "duree wantEN : Day");
    ASSERT_HAS(items, "Second",      "duree wantEN : Second");
    ASSERT_HAS(items, "Millisecond", "duree wantEN : Millisecond");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/*  — non-régression : déclaration FR `durée` en mode langue FR
 * (défaut) continue de proposer les composantes FR canon, après retrait
 * du cas spécial et bascule sur le chemin générique registry-driven. */
static void test_literal_init_duree_fr_preserved(void) {
    printf("\n[TEST] fix/1043 : non-reg `d est une durée = { <cursor>` (FR, defaut) propose Jour/Heure/Minute/Seconde/Milliseconde\n");
    const char* src =
        "d est une dur\xc3\xa9""e\n"
        "d = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Jour",        "duree FR preserve : Jour");
    ASSERT_HAS(items, "Heure",       "duree FR preserve : Heure");
    ASSERT_HAS(items, "Minute",      "duree FR preserve : Minute");
    ASSERT_HAS(items, "Seconde",     "duree FR preserve : Seconde");
    ASSERT_HAS(items, "Milliseconde","duree FR preserve : Milliseconde");
    json_free(items);
    json_free(params);
}

/*  — non-régression : les composantes proposées dans le littéral
 * `{}` d'une durée sont STRICTEMENT les composantes de construction, PAS
 * les totaux accesseurs (EnMillisecondes/InMilliseconds...). Vert
 * aujourd'hui (liste hardcodée à 5 items) ; doit rester vert après
 * bascule registry-driven (ti->fields ne doit exposer QUE les composantes
 * de construction, pas les totaux). */
static void test_literal_init_duree_excludes_totaux(void) {
    printf("\n[TEST] fix/1043 : litteral durée { } n'expose pas les totaux accesseurs (EnMillisecondes...)\n");
    const char* src =
        "d est une dur\xc3\xa9""e\n"
        "d = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "EnMillisecondes", "duree litteral : pas EnMillisecondes");
    ASSERT_NOT_HAS(items, "EnHeures",        "duree litteral : pas EnHeures");
    ASSERT_NOT_HAS(items, "InMilliseconds",  "duree litteral : pas InMilliseconds");
    json_free(items);
    json_free(params);
}

/* Declaration directe (pas d'include scanner.h ici : son enum TokenType
 * collisionne avec celui de windows.h inclus plus bas dans ce fichier).
 * scanner_strip_accents est linke via src/scanner.c dans owl-lsp-test. */
int scanner_strip_accents(const char* src, int srcLen, char* buf, int bufSize);

/* Skip local, miroir de lsp_handlers.c::is_primitive_prop_type (static,
 * non exportee). Depuis  (option B), les types PRIMITIFS peuvent
 * legitimement figurer dans OWL_TYPE_CATALOG (metadonnee de litteral) —
 * durée en est un, et DOIT exposer EnJours/EnHeures en `..` (cf.
 * test_dot_dot_duree_keeps_props). Ce guard ne s'applique donc qu'aux
 * types NON-primitifs du catalog. */
static bool test_type_is_primitive_prop_type(const char* typeName) {
    static const char* const PRIMITIVE_PROP_TYPES[] = {
        "tableau", "file", "pile", "liste", "JSON",
        "date", "heure", "dateheure", "duree",
    };
    char stripped[64];
    scanner_strip_accents(typeName, (int)strlen(typeName),
                          stripped, sizeof(stripped));
    int n = (int)(sizeof(PRIMITIVE_PROP_TYPES)/sizeof(PRIMITIVE_PROP_TYPES[0]));
    for (int i = 0; i < n; i++) {
        if (strcasecmp(stripped, PRIMITIVE_PROP_TYPES[i]) == 0) return true;
    }
    return false;
}

/*  — Filet anti-regression transverse du fallback aveugle
 * OWL_BUILTIN_PROP_CATALOG (complement comportemental de ).
 *
 * Pour CHAQUE type NON-PRIMITIF du catalog LSP (OWL_TYPE_CATALOG), simule
 * `v est une <Type>` puis declenche la completion `v..` et verifie
 * qu'AUCUN ne re-crache les proprietes primitives marqueurs (Année de
 * Date, EnJours/EnHeures de Duree, Mois). Ces props ne doivent
 * apparaitre QUE sur un type primitif (date/heure/duree/tableau...).
 * Tout type catalog non-primitif qui les expose = retour du fallback
 * aveugle (le bug ).
 *
 * Le nom de type est pris en forme canon accentuee (.name) : c'est ce
 * que l'utilisateur tape et ce sur quoi le receiverType est resolu.
 *
 * Depuis  (option B), les primitifs (durée en tete) PEUVENT
 * figurer dans OWL_TYPE_CATALOG (metadonnee de litteral) : on les skip
 * ici, ce n'est plus une invariant "hors catalog" mais un filtre
 * explicite par nom. Le complement non-regression (un type primitif
 * GARDE ses props) reste couvert par test_dot_dot_date_keeps_props /
 * _duree_keeps_props. */
static void test_no_type_regresses_to_primitive_fallback(void) {
    printf("\n[TEST] Aucun type catalog non-primitif ne regresse au fallback primitif (fix/0113)\n");
    /* Marqueurs : proprietes primitives en forme canon FR accentuee. */
    static const char* const PRIMITIVE_MARKERS[] = {
        ("Ann\xc3\xa9" "e"),   /* "Année" — Date */
        "Mois",              /* Date */
        "EnJours",           /* Duree */
        "EnHeures",          /* Duree */
    };
    int nMarkers = (int)(sizeof(PRIMITIVE_MARKERS)/sizeof(PRIMITIVE_MARKERS[0]));

    for (int t = 0; t < OWL_TYPE_CATALOG_COUNT; t++) {
        const char* typeName = OWL_TYPE_CATALOG[t].name; /* canon accentue */
        if (test_type_is_primitive_prop_type(typeName)) continue; /*  : durée etc. */

        /* `v est une <Type>\nx = v..\n` — curseur juste apres '..',
         * ligne 1, col = strlen("x = v..") = 7. */
        char src[256];
        snprintf(src, sizeof(src), "v est une %s\nx = v..\n", typeName);
        setup_doc(URI, src);
        JsonValue* params = make_completion_params(URI, 1, 7);
        JsonValue* items = handle_completion(params);

        for (int m = 0; m < nMarkers; m++) {
            char testName[160];
            snprintf(testName, sizeof(testName),
                     "type '%s' ne crache pas la prop primitive", typeName);
            ASSERT_NOT_HAS(items, PRIMITIVE_MARKERS[m], testName);
        }
        json_free(items);
        json_free(params);
    }
}

/* ================================================================
 *  Semantic Tokens helpers
 * ================================================================ */

/* Helper pour creer params textDocument/semanticTokens/full */
static JsonValue* make_semantic_tokens_params(const char* uri) {
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(uri));
    json_object_set(params, "textDocument", td);
    return params;
}

/* Decode le tableau delta-encode et cherche un token a (line,startChar)
 * avec le tokenType donne. Retourne true si trouve. */
static bool has_semantic_token(JsonValue* result, int line, int startChar, int tokenType) {
    if (!result || result->type != JSON_OBJECT) return false;
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) return false;

    int count = data->as.arrayVal.count;
    if (count % 5 != 0) return false;  /* format: 5 entiers par token */

    int curLine = 0, curStart = 0;
    for (int i = 0; i < count; i += 5) {
        int deltaLine = data->as.arrayVal.items[i]->as.intVal;
        int deltaStart = data->as.arrayVal.items[i+1]->as.intVal;
        /* int length = data->as.arrayVal.items[i+2]->as.intVal; -- unused */
        int tType = data->as.arrayVal.items[i+3]->as.intVal;
        /* int tMods = data->as.arrayVal.items[i+4]->as.intVal; -- unused */

        curLine += deltaLine;
        if (deltaLine != 0) curStart = 0;
        curStart += deltaStart;

        if (curLine == line && curStart == startChar && tType == tokenType) {
            return true;
        }
    }
    return false;
}

/*  — Verifie s'il existe un token (peu importe le type) a la position
 * donnee. Utilise pour asserter qu'un IDENT en position de member access
 * (`:ident`, `obj.ident`, `obj..ident`) ne produit AUCUN semantic token —
 * la coloration etant deleguee a la grammar TextMate. */
/* Helper conserve pour debug semantic tokens ( originel).
 * Suite ajustement test 0278-T1 par , plus utilise en assertion mais
 * reste disponible si besoin futur d'asserter "aucun token a (line,col)". */
__attribute__((unused))
static bool has_any_semantic_token_at(JsonValue* result, int line, int startChar) {
    if (!result || result->type != JSON_OBJECT) return false;
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) return false;

    int count = data->as.arrayVal.count;
    if (count % 5 != 0) return false;

    int curLine = 0, curStart = 0;
    for (int i = 0; i < count; i += 5) {
        int deltaLine = data->as.arrayVal.items[i]->as.intVal;
        int deltaStart = data->as.arrayVal.items[i+1]->as.intVal;
        curLine += deltaLine;
        if (deltaLine != 0) curStart = 0;
        curStart += deltaStart;
        if (curLine == line && curStart == startChar) return true;
    }
    return false;
}

/*  — Dump diagnostique : affiche tous les tokens emis. Utile pour
 * cerner empiriquement la classification courante quand on suspecte un
 * mismatch. Non-destructif (lecture seule). */
static void dump_semantic_tokens(JsonValue* result, const char* label) {
    if (!result || result->type != JSON_OBJECT) { printf("  [DUMP %s] result null\n", label); return; }
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) { printf("  [DUMP %s] data null\n", label); return; }
    int count = data->as.arrayVal.count;
    printf("  [DUMP %s] count=%d tokens\n", label, count / 5);
    int curLine = 0, curStart = 0;
    for (int i = 0; i < count; i += 5) {
        int dL = data->as.arrayVal.items[i]->as.intVal;
        int dS = data->as.arrayVal.items[i+1]->as.intVal;
        int len = data->as.arrayVal.items[i+2]->as.intVal;
        int tT = data->as.arrayVal.items[i+3]->as.intVal;
        int tM = data->as.arrayVal.items[i+4]->as.intVal;
        curLine += dL;
        if (dL != 0) curStart = 0;
        curStart += dS;
        printf("    token[%d] line=%d col=%d len=%d type=%d mods=%d\n",
               i/5, curLine, curStart, len, tT, tM);
    }
}

/*  — retourne la LONGUEUR (unites UTF-16) du token semantic present a
 * (line, startChar), ou -1 si absent. Sert a verifier que la borne du token
 * ne deborde pas sur le mot voisin (cas nom de bloc accentue -> keyword `est`). */
static int semantic_token_length_at(JsonValue* result, int line, int startChar) {
    if (!result || result->type != JSON_OBJECT) return -1;
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) return -1;
    int count = data->as.arrayVal.count;
    if (count % 5 != 0) return -1;
    int curLine = 0, curStart = 0;
    for (int i = 0; i < count; i += 5) {
        int deltaLine = data->as.arrayVal.items[i]->as.intVal;
        int deltaStart = data->as.arrayVal.items[i+1]->as.intVal;
        int len = data->as.arrayVal.items[i+2]->as.intVal;
        curLine += deltaLine;
        if (deltaLine != 0) curStart = 0;
        curStart += deltaStart;
        if (curLine == line && curStart == startChar) return len;
    }
    return -1;
}

/* Compte combien de tokens avec le tokenType donne existent. */
static int count_semantic_tokens_of_type(JsonValue* result, int tokenType) {
    if (!result || result->type != JSON_OBJECT) return 0;
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) return 0;

    int count = data->as.arrayVal.count;
    if (count % 5 != 0) return 0;

    int matches = 0;
    for (int i = 0; i < count; i += 5) {
        int tType = data->as.arrayVal.items[i+3]->as.intVal;
        if (tType == tokenType) matches++;
    }
    return matches;
}

/* Assert helpers pour semantic tokens */
#define ASSERT_HAS_TOKEN(result, line, startChar, tokenType, test_name) do { \
    if (has_semantic_token(result, line, startChar, tokenType)) { \
        g_pass++; \
        printf("  [OK]   %s : token type=%d at (%d,%d)\n", test_name, tokenType, line, startChar); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : token type=%d MANQUANT at (%d,%d)\n", \
               test_name, tokenType, line, startChar); \
    } \
} while (0)

#define ASSERT_NO_TOKEN_TYPE(result, tokenType, test_name) do { \
    int n = count_semantic_tokens_of_type(result, tokenType); \
    if (n == 0) { \
        g_pass++; \
        printf("  [OK]   %s : aucun token type=%d (correct)\n", test_name, tokenType); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : trouve %d token(s) type=%d (attendu 0)\n", \
               test_name, n, tokenType); \
    } \
} while (0)

/* Helper qui verifie tokenType ET tokenModifiers. Si expectedMods = -1,
 * on n'asserte pas sur tokenModifiers (utile pour transitions progressives).
 * Format tokenModifiers : bitfield, bit 0 = "global".
 * Exemples :
 *   - variable globale : tokenType=3, tokenModifiers=1 (bit global actif)
 *   - variable locale : tokenType=3, tokenModifiers=0 (pas de bit global)
 */
static bool has_semantic_token_opt(JsonValue* result, int line, int startChar,
                                   int tokenType, int expectedMods) {
    if (!result || result->type != JSON_OBJECT) return false;
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) return false;

    int count = data->as.arrayVal.count;
    if (count % 5 != 0) return false;

    int curLine = 0, curStart = 0;
    for (int i = 0; i < count; i += 5) {
        int deltaLine = data->as.arrayVal.items[i]->as.intVal;
        int deltaStart = data->as.arrayVal.items[i+1]->as.intVal;
        int tType = data->as.arrayVal.items[i+3]->as.intVal;
        int tMods = data->as.arrayVal.items[i+4]->as.intVal;

        curLine += deltaLine;
        if (deltaLine != 0) curStart = 0;
        curStart += deltaStart;

        if (curLine == line && curStart == startChar && tType == tokenType) {
            if (expectedMods == -1) return true;  /* don't care modifiers */
            if (tMods == expectedMods) return true;
        }
    }
    return false;
}

#define ASSERT_HAS_TOKEN_OPT(result, line, startChar, tokenType, tokenMods, test_name) do { \
    if (has_semantic_token_opt(result, line, startChar, tokenType, tokenMods)) { \
        g_pass++; \
        printf("  [OK]   %s : token type=%d mods=%d at (%d,%d)\n", \
               test_name, tokenType, tokenMods, line, startChar); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : token type=%d mods=%d MANQUANT at (%d,%d)\n", \
               test_name, tokenType, tokenMods, line, startChar); \
    } \
} while (0)

/* ================================================================
 *  Semantic Tokens tests
 * ================================================================ */

/* Test 1 : classe user — DÉCLARATION emit 'class' (1, vert foncé),
 * USAGE emit 'type' (4, violet). Révision 1 feedback dev : le modèle
 * déclaration=type était incorrect, corrigé pour déclaration=class. */
static void test_semantic_class_user_declaration_and_usage(void) {
    printf("\n[TEST] Semantic tokens : classe user — decl=class(1), usage=type(4)\n");
    const char* src =
        "cCompte est une Classe\n"
        "    solde est un entier\n"
        "FIN\n"
        "\n"
        "oBanque est un cCompte()\n"
        "autre de cCompte\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* cCompte ligne 0 col 0 (DECLARATION) : token type 1 'class' attendu */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "classe decl → class (vert fonce #009180)");
    /* cCompte ligne 4 col 15 (USAGE 'est un cCompte()') : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 4, 15, 4, "classe usage instantiation → type (violet #9575CD)");
    /* cCompte ligne 5 col 9 (USAGE 'de cCompte') : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 5, 9, 4, "classe usage ref type → type (violet #9575CD)");

    json_free(result);
    json_free(params);
}

/*  — nom de classe declaree en ANGLAIS avec article (`cPlateau is a
 * Class`) : le nom doit recevoir le MEME token semantique que la forme FR
 * (`cPlateau est une Classe` → class, index 1, vert fonce). En FR l'article
 * `un/une` est un filler jete par le scanner ; en EN `a` (TOKEN_A) / `an`
 * s'intercale entre la copule `is` et le mot-cle `Class`, decalant le peek de
 * detection de declaration (`allTokens[ti+2]` voit l'article, pas CLASSE).
 * Sans saut d'article, la decl EN retombe en `type` (4, violet). Parite
 * exigee avec la forme FR (meme famille d'article que  / ). */
static void test_semantic_class_decl_en_article(void) {
    printf("\n[TEST] fix/1213 : declaration classe EN `is a/an Class` -> class(1) (parite FR)\n");

    /* Reference FR (deja verte, garde-fou parite). */
    const char* srcFR =
        "cPlateau est une Classe\n"
        "    n est un entier\n"
        "FIN\n";
    setup_doc(URI, srcFR);
    JsonValue* pFR = make_semantic_tokens_params(URI);
    JsonValue* rFR = handle_semantic_tokens(pFR);
    ASSERT_HAS_TOKEN(rFR, 0, 0, 1, "cPlateau FR `est une Classe` -> class(1)");
    json_free(rFR); json_free(pFR);

    /* EN `is a Class` : le nom doit rester class(1), pas type(4). */
    const char* srcEN =
        "cPlateau is a Class\n"
        "    n est un entier\n"
        "FIN\n";
    setup_doc(URI, srcEN);
    JsonValue* pEN = make_semantic_tokens_params(URI);
    JsonValue* rEN = handle_semantic_tokens(pEN);
    if (!has_semantic_token(rEN, 0, 0, 1)) {
        dump_semantic_tokens(rEN, "cPlateau EN is a Class");
    }
    ASSERT_HAS_TOKEN(rEN, 0, 0, 1, "cPlateau EN `is a Class` -> class(1)");
    json_free(rEN); json_free(pEN);

    /* EN `is an Class` (article `an`). */
    const char* srcAN =
        "cPlateau is an Class\n"
        "    n est un entier\n"
        "FIN\n";
    setup_doc(URI, srcAN);
    JsonValue* pAN = make_semantic_tokens_params(URI);
    JsonValue* rAN = handle_semantic_tokens(pAN);
    ASSERT_HAS_TOKEN(rAN, 0, 0, 1, "cPlateau EN `is an Class` -> class(1)");
    json_free(rAN); json_free(pAN);
}

/*  — meme famille d'article cote nom de FENETRE declaree en EN
 * (`FEN_Test is a Window`). Les noms de fenetre recoivent class(1) via
 * userWindows[] (pas de distinction decl/usage), et l'enregistrement saute
 * deja l'article EN. Ce test verrouille la non-regression EN. */
static void test_semantic_window_decl_en_article(void) {
    printf("\n[TEST] fix/1213 : declaration fenetre EN `is a Window` -> class(1)\n");
    const char* src =
        "FEN_Test is a Window\n"
        "    Titre = \"x\"\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    if (!has_semantic_token(result, 0, 0, 1)) {
        dump_semantic_tokens(result, "FEN_Test EN is a Window");
    }
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "FEN_Test EN `is a Window` -> class(1)");
    json_free(result);
    json_free(params);
}

/* Test 2 : appel methode user emet token 'method' (index 5). */
static void test_semantic_method_user_call(void) {
    printf("\n[TEST] Semantic tokens : appel methode user (type=5)\n");
    const char* src =
        "cCompte est une Classe\n"
        "    solde est un entier\n"
        "    PROCEDURE Crediter(montant est un entier)\n"
        "        solde += montant\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "oBanque est un cCompte()\n"
        "oBanque.Crediter(1000)\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Crediter ligne 8 col 8 (appel methode) : token type 5 attendu */
    ASSERT_HAS_TOKEN(result, 8, 8, 5, "methode user call");

    json_free(result);
    json_free(params);
}

/* Test 3 : methode-forme native NE doit PAS emettre token method user. */
static void test_semantic_native_method_no_token(void) {
    printf("\n[TEST] Semantic tokens : methode-forme native sans token user\n");
    const char* src =
        "z est un zipArchive()\n"
        "z.Ajoute(\"test.txt\", zipAucun)\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Ajoute (methode native) : PAS de token type 5 */
    /* Note : on verifie qu'il n'y a AUCUN token type 5 dans ce fichier. */
    ASSERT_NO_TOKEN_TYPE(result, 5, "native method no user token");

    json_free(result);
    json_free(params);
}

/* Test 4 : Constructeur et Destructeur NE doivent PAS emettre token method (contrainte forte). */
static void test_semantic_constructor_destructor_no_method_token(void) {
    printf("\n[TEST] Semantic tokens : Constructeur/Destructeur sans token method (contrainte)\n");
    const char* src =
        "cCompte est une Classe\n"
        "    solde est un entier\n"
        "    PROCEDURE Constructeur(initial est un entier)\n"
        "        solde = initial\n"
        "    FIN\n"
        "    PROCEDURE Destructeur()\n"
        "        solde = 0\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "oBanque est un cCompte(100)\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Constructeur/Destructeur : PAS de token type 5 (colors orange grammar) */
    /* On verifie qu'il n'y a AUCUN token type 5 dans ce fichier. */
    ASSERT_NO_TOKEN_TYPE(result, 5, "Constructeur/Destructeur no method token");

    json_free(result);
    json_free(params);
}

/*  — collision nom de methode user vs native accentuee. Le nom de
 * methode `Réinitialise` (accentue, canon d'une methode-forme native du
 * mock-system) peut etre declare/appele avec une accentuation differente :
 * invariant 6.2 (accent-insensible) impose que decl `Réinitialise` + appel
 * `Reinitialise` (et reciproquement) resolvent la MEME methode → token method
 * (5) emis sur l'appel, sinon la grammar TextMate colore bleu (native).
 * Discriminant mesure : un nom NON-natif (`CalculeScore`) et un nom native
 * sans mismatch d'accent recevaient deja le token ; seul le mismatch d'accent
 * cassait (comparaison strcasecmp accent-sensible dans user_class_method_exists). */
static void test_semantic_method_user_accent_collision(void) {
    printf("\n[TEST] fix/1216 : methode user accent-mismatch vs native -> token method(5)\n");

    /* Cas 1 : declaration accentuee `Réinitialise`, appel sans accent. */
    const char* src1 =
        "cJeu est une Classe\n"
        "    score est un entier\n"
        "    PROCEDURE R\xC3\xA9initialise()\n"
        "        score = 0\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "oJeu est un cJeu()\n"
        "oJeu.Reinitialise()\n";
    setup_doc(URI, src1);
    JsonValue* p1 = make_semantic_tokens_params(URI);
    JsonValue* r1 = handle_semantic_tokens(p1);
    ASSERT_HAS_TOKEN(r1, 8, 5, 5, "decl accent / appel sans accent -> method(5)");
    json_free(r1); json_free(p1);

    /* Cas 2 : declaration sans accent, appel accentue `Réinitialise`. */
    const char* src2 =
        "cJeu est une Classe\n"
        "    score est un entier\n"
        "    PROCEDURE Reinitialise()\n"
        "        score = 0\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "oJeu est un cJeu()\n"
        "oJeu.R\xC3\xA9initialise()\n";
    setup_doc(URI, src2);
    JsonValue* p2 = make_semantic_tokens_params(URI);
    JsonValue* r2 = handle_semantic_tokens(p2);
    ASSERT_HAS_TOKEN(r2, 8, 5, 5, "decl sans accent / appel accent -> method(5)");
    json_free(r2); json_free(p2);

    /* Temoin non-regression : methode user au nom NON-natif -> method(5). */
    const char* src3 =
        "cJeu est une Classe\n"
        "    score est un entier\n"
        "    PROCEDURE CalculeScore()\n"
        "        score = 42\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "oJeu est un cJeu()\n"
        "oJeu.CalculeScore()\n";
    setup_doc(URI, src3);
    JsonValue* p3 = make_semantic_tokens_params(URI);
    JsonValue* r3 = handle_semantic_tokens(p3);
    ASSERT_HAS_TOKEN(r3, 8, 5, 5, "temoin CalculeScore (non-natif) -> method(5)");
    json_free(r3); json_free(p3);

    /* Non-regression : vraie native `.Ajoute()` sur tableau (aucune classe
     * user portant `Ajoute`) -> PAS de token method (reste bleu grammar). */
    const char* src4 =
        "t est un tableau de entier\n"
        "t.Ajoute(1)\n";
    setup_doc(URI, src4);
    JsonValue* p4 = make_semantic_tokens_params(URI);
    JsonValue* r4 = handle_semantic_tokens(p4);
    ASSERT_NO_TOKEN_TYPE(r4, 5, "native .Ajoute() sans classe user -> pas de method(5)");
    json_free(r4); json_free(p4);
}

/* Test 5 : appel methode user avec nom accent (couverture regression casse/accent). */
static void test_semantic_method_user_accented_name(void) {
    printf("\n[TEST] Semantic tokens : methode user avec nom accentue\n");
    const char* src =
        "cCompte est une Classe\n"
        "    solde est un entier\n"
        "    PROCEDURE Crediter(montant est un entier)\n"
        "        solde += montant\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "oBanque est un cCompte()\n"
        "oBanque.Crediter(500)\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Crediter ligne 8 col 8 : token type 5 attendu (meme sans accent dans code) */
    ASSERT_HAS_TOKEN(result, 8, 8, 5, "methode accented");

    json_free(result);
    json_free(params);
}

/*  — Test : appel direct d'une procedure user top-level emet token
 * 'method' (index 5, vert fonce), pas 'variable' (cyan). Avant ce fix, seuls
 * les appels qualifies `obj.Methode(...)` etaient classes method ; les appels
 * non-qualifies `Calcule(10, 20)` tombaient en variable. */
static void test_semantic_procedure_user_direct_call(void) {
    printf("\n[TEST] fix/0276 : appel direct procedure user → method (type=5)\n");
    const char* src =
        "PROCEDURE Calcule(a est un entier, b est un entier)\n"
        "    RENVOYER a + b\n"
        "FIN\n"
        "\n"
        "PROCEDURE Bienvenue(nom est une chaine)\n"
        "    Trace(\"Bonjour \" + nom)\n"
        "FIN\n"
        "\n"
        "nSomme est un entier = Calcule(10, 20)\n"
        "Bienvenue(\"OWL\")\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Calcule au site d'appel : ligne 8, col 23 (apres "nSomme est un entier = ") */
    ASSERT_HAS_TOKEN(result, 8, 23, 5, "Calcule appel direct → method");
    /* Bienvenue au site d'appel : ligne 9, col 0 */
    ASSERT_HAS_TOKEN(result, 9, 0, 5, "Bienvenue appel direct → method");

    json_free(result);
    json_free(params);
}

/*  — Test recursion : une procedure qui s'appelle elle-meme dans son
 * propre body doit aussi etre coloree method (l'appel recursif n'est pas
 * qualifie, c'est exactement le cas que ce fix adresse). */
static void test_semantic_procedure_user_recursion(void) {
    printf("\n[TEST] fix/0276 : recursion procedure user → method (type=5)\n");
    const char* src =
        "PROCEDURE Factorielle(n est un entier)\n"
        "    SI n <= 1 ALORS\n"
        "        RENVOYER 1\n"
        "    FIN\n"
        "    RENVOYER n * Factorielle(n - 1)\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Factorielle appel recursif : ligne 4, col 17 (apres "    RENVOYER n * ") */
    ASSERT_HAS_TOKEN(result, 4, 17, 5, "Factorielle recursion → method");

    json_free(result);
    json_free(params);
}

/*  — Le nom d'entree `Main` en contexte DECLARATION ne doit PAS
 * recevoir de token method (type=5, vert). La grammar TextMate le colore
 * bleu (support.function.event.owl, entry-point). Avant ce fix, Main
 * (present dans userSymbols kind==3 mais absent des catalogues d'events)
 * recevait un token method qui ecrasait le bleu → incoherence visuelle
 * vs les events (Fermeture). */
static void test_semantic_entry_point_main_decl_no_method(void) {
    printf("\n[TEST] fix/0866 : declaration PROCEDURE Main → aucun token method (bleu grammar)\n");
    const char* src =
        "PROCEDURE Main()\n"
        "    Trace(\"x\")\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Main ligne 0 col 10 ("PROCEDURE ") : pas de token method (type=5). */
    if (!has_semantic_token(result, 0, 10, 5)) {
        g_pass++;
        printf("  [OK]   Main decl : aucun token method à (0,10)\n");
    } else {
        g_fail++;
        printf("  [FAIL] Main decl : token method (type=5) présent à (0,10)\n");
        dump_semantic_tokens(result, "Main decl");
    }
    /* Trace est native (pas dans userSymbols) → aucun type=5 dans le doc. */
    ASSERT_NO_TOKEN_TYPE(result, 5, "Main decl : aucun token method dans le doc");

    json_free(result);
    json_free(params);
}

/*  — Casse-insensible (WL canon runtime) : `PROCEDURE main`
 * minuscule doit aussi echapper au token method. */
static void test_semantic_entry_point_main_decl_case_insensitive(void) {
    printf("\n[TEST] fix/0866 : PROCEDURE main (minuscule) → aucun token method\n");
    const char* src =
        "PROCEDURE main()\n"
        "    Trace(\"x\")\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* main ligne 0 col 10 : pas de token method (type=5). */
    if (!has_semantic_token(result, 0, 10, 5)) {
        g_pass++;
        printf("  [OK]   main decl (minuscule) : aucun token method à (0,10)\n");
    } else {
        g_fail++;
        printf("  [FAIL] main decl (minuscule) : token method (type=5) présent à (0,10)\n");
        dump_semantic_tokens(result, "main decl minuscule");
    }
    ASSERT_NO_TOKEN_TYPE(result, 5, "main decl minuscule : aucun token method dans le doc");

    json_free(result);
    json_free(params);
}

/*  — Non-regression : la suppression Main est scopee au nom `Main`
 * uniquement. Une procedure user normale (`Calcule`) garde son token method
 * (type=5) au SITE D'APPEL (contrat ). Le fix ne doit pas toucher
 * les procedures normales. */
static void test_semantic_entry_point_normal_proc_call_unchanged(void) {
    printf("\n[TEST] fix/0866 : proc normale Calcule → token method inchangé (fix/0276)\n");
    const char* src =
        "PROCEDURE Calcule(a est un entier)\n"
        "    RENVOYER a\n"
        "FIN\n"
        "\n"
        "x est un entier = Calcule(5)\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Site d'appel Calcule ligne 4 col 18 ("x est un entier = ") : method type=5. */
    ASSERT_HAS_TOKEN(result, 4, 18, 5, "Calcule appel → method (inchangé)");

    json_free(result);
    json_free(params);
}

/*  — Scenario support (ticket #66) : une fenetre avec un event
 * `Fermeture` (deja bleu via catalogue) et un point d'entree `Main`
 * (bleu via ). Aucun des deux noms ne doit avoir de token method
 * (type=5) — coherence visuelle bleue. La fenetre FEN_X garde son token
 * class (type=1). */
static void test_semantic_entry_point_window_scenario(void) {
    printf("\n[TEST] fix/0866 : scenario fenetre (Fermeture + Main) → aucun method, FEN_X class\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "    PROCEDURE Fermeture()\n"
        "        Trace(\"bye\")\n"
        "    FIN\n"
        "FIN\n"
        "PROCEDURE Main()\n"
        "    Trace(\"hi\")\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* FEN_X ligne 0 col 0 : token class (type=1). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "FEN_X → class (inchangé)");

    /* Fermeture ligne 1 col 14 ("    PROCEDURE ") : pas de method (event bleu). */
    if (!has_semantic_token(result, 1, 14, 5)) {
        g_pass++;
        printf("  [OK]   Fermeture event : aucun token method à (1,14)\n");
    } else {
        g_fail++;
        printf("  [FAIL] Fermeture event : token method (type=5) présent à (1,14)\n");
        dump_semantic_tokens(result, "scenario fenetre");
    }
    /* Main ligne 5 col 10 ("PROCEDURE ") : pas de method (entry-point bleu). */
    if (!has_semantic_token(result, 5, 10, 5)) {
        g_pass++;
        printf("  [OK]   Main entry-point : aucun token method à (5,10)\n");
    } else {
        g_fail++;
        printf("  [FAIL] Main entry-point : token method (type=5) présent à (5,10)\n");
        dump_semantic_tokens(result, "scenario fenetre");
    }

    json_free(result);
    json_free(params);
}

/*  — Non-regression : un parametre (ou local) dont le nom collisionne
 * avec une constante registry (casse-insensible, WL canon) doit etre colore
 * 'variable' (type=3), JAMAIS 'enumMember' (type=0, jaune italique). Le scope
 * local prime sur le registry. Avant  le param tombait sur la couleur
 * de la constante homonyme (cf. session de debug perdue sur `sTexte` vs
 * `STexte`). On utilise ici `DepuisFin`, constante registry de la famille
 * Chaines, comme nom de parametre. */
static void test_semantic_param_shadows_registry_constant(void) {
    printf("\n[TEST] fix/0517 : param homonyme constante registry → variable (pas enumMember)\n");
    const char* src =
        "PROCEDURE Analyse(DepuisFin est un entier)\n"
        "    nLocal est un entier = DepuisFin + 1\n"
        "    RENVOYER nLocal\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Declaration param ligne 0 col 18 ("PROCEDURE Analyse(") : variable, mods=0 (local). */
    ASSERT_HAS_TOKEN_OPT(result, 0, 18, 3, 0, "param DepuisFin decl → variable locale");
    /* Usage du param ligne 1 col 27 ("    nLocal est un entier = ") : variable locale. */
    ASSERT_HAS_TOKEN_OPT(result, 1, 27, 3, 0, "param DepuisFin usage → variable locale");
    /* Contrainte forte : aucun token enumMember (jaune italique constante) emis. */
    ASSERT_NO_TOKEN_TYPE(result, 0, "param homonyme constante : aucun token enumMember");

    json_free(result);
    json_free(params);
}

/*  — Non-regression : un appel native stdlib comme Trace("x") au
 * top-level ne doit PAS recevoir tokenType=5. Trace n'est pas dans
 * userSymbols (pas declare par l'user), il est colore via la grammar
 * TextMate. Sans ce garde-fou on transformerait toute fonction connue de
 * la grammar en method, ce qui ecraserait sa couleur native. */
static void test_semantic_native_direct_call_no_method_token(void) {
    printf("\n[TEST] fix/0276 : appel native direct (Trace) sans token method\n");
    const char* src =
        "Trace(\"hello\")\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Aucun token type 5 dans ce fichier (Trace est native, pas user). */
    ASSERT_NO_TOKEN_TYPE(result, 5, "Trace native call → pas de method token");

    json_free(result);
    json_free(params);
}

/* Test 6 : nom d'Enumeration emet token 'type' (index 4) en declaration et usage. */
static void test_semantic_enum_declaration_and_usage(void) {
    printf("\n[TEST] Semantic tokens : enum user — decl=type(4), usage=type(4)\n");
    const char* src =
        "Statut est une Enumeration\n"
        "    actif\n"
        "    inactif\n"
        "FIN\n"
        "\n"
        "eStatut est un Statut\n"
        "eStatut = Statut.actif\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Statut ligne 0 col 0 (DECLARATION) : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 0, 0, 4, "enum decl → type (violet #9575CD)");
    /* Statut ligne 5 col 15 (USAGE 'est un Statut') : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 5, 15, 4, "enum usage → type (violet #9575CD)");
    /* Statut ligne 6 col 10 (USAGE 'Statut.actif') : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 6, 10, 4, "enum usage access → type (violet #9575CD)");

    json_free(result);
    json_free(params);
}

/* Test 7 : nom de Combinaison emet token 'type' (index 4) en declaration et usage. */
static void test_semantic_combination_declaration_and_usage(void) {
    printf("\n[TEST] Semantic tokens : combinaison user — decl=type(4), usage=type(4)\n");
    const char* src =
        "Options est une Combinaison\n"
        "    lecture\n"
        "    ecriture\n"
        "FIN\n"
        "\n"
        "opts est une Options\n"
        "opts = Options.lecture + Options.ecriture\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Options ligne 0 col 0 (DECLARATION) : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 0, 0, 4, "combinaison decl → type (violet #9575CD)");
    /* Options ligne 5 col 13 (USAGE 'est une Options') : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 5, 13, 4, "combinaison usage → type (violet #9575CD)");
    /* Options ligne 6 col 7 (USAGE 'Options.lecture') : token type 4 'type' attendu */
    ASSERT_HAS_TOKEN(result, 6, 7, 4, "combinaison usage access → type (violet #9575CD)");

    json_free(result);
    json_free(params);
}

/* Test 8 : NON-REGRESSION classe — nom decl=class(1), usage=type(4) (inchange). */
static void test_semantic_class_non_regression(void) {
    printf("\n[TEST] Semantic tokens : classe non-regression (decl=class, usage=type)\n");
    const char* src =
        "cCompte est une Classe\n"
        "    solde est un entier\n"
        "FIN\n"
        "\n"
        "oBanque est un cCompte()\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* cCompte ligne 0 col 0 (DECLARATION) : token type 1 'class' attendu (VERT fonce) */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "classe decl → class (vert fonce #009180)");
    /* cCompte ligne 4 col 15 (USAGE 'est un cCompte()') : token type 4 'type' attendu (VIOLET) */
    ASSERT_HAS_TOKEN(result, 4, 15, 4, "classe usage → type (violet #9575CD)");

    json_free(result);
    json_free(params);
}

/* Test 9 : nom d'INTERFACE → token type (4, VIOLET). RED attendu (LSP ne suit pas
 * encore les noms d'interface via UserInterfaceInfo). Ticket  rev3. */
static void test_semantic_interface_declaration_and_usage(void) {
    printf("\n[TEST] Semantic tokens : interface nom→type (RED attendu, pas de suivi UserInterfaceInfo)\n");
    const char* src =
        "iTruc est une Interface\n"
        "    PROCEDURE Faire()\n"
        "    FIN\n"
        "FIN\n"
        "\n"
        "x est un iTruc\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* iTruc ligne 0 col 0 (DECLARATION) : token type 4 'type' attendu (VIOLET) */
    ASSERT_HAS_TOKEN(result, 0, 0, 4, "interface decl → type (violet #9575CD) [RED attendu]");
    /* iTruc ligne 5 col 9 (USAGE 'est un iTruc') : token type 4 'type' attendu (VIOLET) */
    ASSERT_HAS_TOKEN(result, 5, 9, 4, "interface usage → type (violet #9575CD) [RED attendu]");

    json_free(result);
    json_free(params);
}

/* Test 10 : acces qualifie Statut.actif → Statut=type(4, VIOLET), actif=enumMember(0, JAUNE).
 * RED attendu (LSP skippe emission apres le point). Ticket  rev4. */
static void test_semantic_enum_qualified_access(void) {
    printf("\n[TEST] Semantic tokens : Statut.actif → Statut=type(4) + actif=enumMember(0) [RED attendu]\n");
    const char* src =
        "Statut est une Enumeration\n"
        "    actif\n"
        "    inactif\n"
        "FIN\n"
        "\n"
        "eee est un Statut = Statut.actif\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Statut ligne 5 col 20 (avant le point) : token type 4 'type' attendu (VIOLET) */
    ASSERT_HAS_TOKEN(result, 5, 20, 4, "enum qualifie receiver Statut → type (violet #9575CD)");
    /* actif ligne 5 col 27 (apres le point) : token type 0 'enumMember' attendu (JAUNE) */
    ASSERT_HAS_TOKEN(result, 5, 27, 0, "enum qualifie membre actif → enumMember (jaune #B1C33A)");

    json_free(result);
    json_free(params);
}

/*  — nom d'etat dans Etats[<etat>] : token enumMember (0, jaune
 * italique #B1C33A), meme classe que les valeurs d'enum. Vaut PARTOUT (style
 * de widget ET de menu), pas de cas particulier menu. Sans la branche dediee,
 * l'ident d'etat (survol/focus/...) retombe vert par defaut. RED avant impl. */
static void test_semantic_etat_bracket_enum_member(void) {
    printf("\n[TEST] Semantic tokens : Etats[survol] → survol=enumMember(0) [widget + menu]\n");
    const char* src =
        "Style..ZoneSaisie..Etats[survol]..Couleur = Noir\n"
        "Style..OptionMenu..Etats[survol]..Couleur = Noir\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* survol col 25 ligne 0 (style de WIDGET ZoneSaisie) : enumMember (0) */
    ASSERT_HAS_TOKEN(result, 0, 25, 0, "Etats[survol] widget → enumMember (jaune #B1C33A)");
    /* survol col 25 ligne 1 (style de MENU OptionMenu) : enumMember (0) */
    ASSERT_HAS_TOKEN(result, 1, 25, 0, "Etats[survol] menu → enumMember (jaune #B1C33A)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — LSP scan widgets Liste/Image sans prefix Champ
 * ================================================================ */

/* Test 1 : LISTE_X est une Liste (sans Champ) → token property (2, vert fonce).
 * Heuristique : bloc PROPRIETES apres "est une Liste" indique widget, pas collection. */
static void test_semantic_widget_liste_sans_champ(void) {
    printf("\n[TEST] Semantic tokens : LISTE_X est une Liste (sans Champ) → property (vert fonce)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "    PROPRIETES\n"
        "        Titre = \"Test\"\n"
        "    FIN\n"
        "\n"
        "    LISTE_Items est une Liste\n"
        "        PROPRIETES\n"
        "            X = 10\n"
        "            Y = 10\n"
        "        FIN\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* LISTE_Items ligne 5 col 4 (widget Liste sans Champ) : token type 2 'property' attendu (VERT FONCE) */
    ASSERT_HAS_TOKEN(result, 5, 4, 2, "LISTE_Items sans Champ → property (vert fonce #009180)");

    json_free(result);
    json_free(params);
}

/* Test 2 : IMG_X est une Image (sans Champ) → token property (2, vert fonce). */
static void test_semantic_widget_image_sans_champ(void) {
    printf("\n[TEST] Semantic tokens : IMG_X est une Image (sans Champ) → property (vert fonce)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "    PROPRIETES\n"
        "        Titre = \"Test\"\n"
        "    FIN\n"
        "\n"
        "    IMG_Logo est une Image\n"
        "        PROPRIETES\n"
        "            X = 10\n"
        "            Y = 10\n"
        "        FIN\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* IMG_Logo ligne 5 col 4 (widget Image sans Champ) : token type 2 'property' attendu (VERT FONCE) */
    ASSERT_HAS_TOKEN(result, 5, 4, 2, "IMG_Logo sans Champ → property (vert fonce #009180)");

    json_free(result);
    json_free(params);
}

/* Test 3 : Desambiguation — tab est une Liste de chaines (collection, pas widget).
 * Le mot-cle 'de' apres 'Liste' indique collection → PAS de token property. */
static void test_semantic_liste_collection_non_widget(void) {
    printf("\n[TEST] Semantic tokens : tab est une Liste de chaines (collection, pas widget) — pas property\n");
    const char* src =
        "tab est une Liste de chaines\n"
        "tab.Ajoute(\"item1\")\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* tab ligne 0 col 0 (collection, pas widget) : NE DOIT PAS avoir token type 2 'property'.
     * Note : on verifie qu'il n'y a AUCUN token property dans ce fichier (pas de widget). */
    ASSERT_NO_TOKEN_TYPE(result, 2, "Liste collection (avec 'de') pas widget → pas property");

    json_free(result);
    json_free(params);
}

/* Test 4 : Non-regression — COMBO_X est une Combo (sans Champ) → property deja OK. */
static void test_semantic_widget_combo_sans_champ_non_regression(void) {
    printf("\n[TEST] Semantic tokens : COMBO_X est une Combo (sans Champ) → property (non-regression)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "    PROPRIETES\n"
        "        Titre = \"Test\"\n"
        "    FIN\n"
        "\n"
        "    COMBO_Choix est une Combo\n"
        "        PROPRIETES\n"
        "            X = 10\n"
        "            Y = 10\n"
        "        FIN\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* COMBO_Choix ligne 5 col 4 (widget Combo sans Champ, deja OK) : token type 2 'property' attendu */
    ASSERT_HAS_TOKEN(result, 5, 4, 2, "COMBO_Choix sans Champ → property (non-regression, vert fonce)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — LSP scan controle forme EN `<NOM> is a Control <Type>`
 * ================================================================ */

/* Le scan de controles reconnait la copule FR `est un Champ <Type>` mais
 * pas la forme EN canon `is a Control <Type>` (Control = nameEN du type
 * Champ, cf. check_ident_champ compiler.c + ). Sans Control comme
 * prefixe accepte, BTN_Ok restait classifie variable globale (cyan) au
 * lieu de controle (property, vert). FR posee dans le meme fichier en
 * non-regression. */
static void test_semantic_control_prefix_en_control(void) {
    printf("\n[TEST] Semantic tokens : <NOM> is a Control <Type> (forme EN) -> property (vert)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "    SAI_Fr est un Champ Saisie\n"
        "    BTN_Ok is a Control Button\n"
        "    SAI_En is a Control Input\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Forme FR (non-regression) : SAI_Fr ligne 1 col 4 -> property (2, vert). */
    ASSERT_HAS_TOKEN(result, 1, 4, 2, "SAI_Fr (FR Champ Saisie) -> property (vert)");
    /* Forme EN : BTN_Ok ligne 2 col 4 -> property (2, vert). */
    ASSERT_HAS_TOKEN(result, 2, 4, 2, "BTN_Ok (EN Control Button) -> property (vert)");
    /* Forme EN : SAI_En ligne 3 col 4 -> property (2, vert). */
    ASSERT_HAS_TOKEN(result, 3, 4, 2, "SAI_En (EN Control Input) -> property (vert)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — LSP coloration Champ Onglet / Volet / event surChangementVolet
 * ================================================================ */

/* Helper local : true s'il existe un token semantic (peu importe le type) a
 * (line, startChar). Sert a asserter qu'un event (surChangementVolet) ne
 * recoit AUCUN token LSP -> la grammar TextMate le colore bleu. */
static bool has_token_at_pos(JsonValue* result, int line, int startChar) {
    if (!result || result->type != JSON_OBJECT) return false;
    JsonValue* data = json_get(result, "data");
    if (!data || data->type != JSON_ARRAY) return false;
    int count = data->as.arrayVal.count;
    if (count % 5 != 0) return false;
    int curLine = 0, curStart = 0;
    for (int i = 0; i < count; i += 5) {
        int dL = data->as.arrayVal.items[i]->as.intVal;
        int dS = data->as.arrayVal.items[i+1]->as.intVal;
        curLine += dL;
        if (dL != 0) curStart = 0;
        curStart += dS;
        if (curLine == line && curStart == startChar) return true;
    }
    return false;
}

#define ASSERT_NO_TOKEN_AT(result, line, startChar, test_name) do { \
    if (!has_token_at_pos(result, line, startChar)) { \
        g_pass++; \
        printf("  [OK]   %s : aucun token LSP at (%d,%d)\n", test_name, line, startChar); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : token LSP present at (%d,%d) (devrait etre TextMate)\n", \
               test_name, line, startChar); \
    } \
} while (0)

/* Cas FR : Onglet -> Volet -> enfant + event surChangementVolet.
 *   - ONG_X (nom du Champ Onglet)   -> property (type 2, vert fonce)
 *   - VOL_A (nom du Volet)          -> property (type 2, vert fonce) []
 *   - le type `Volet` lui-meme      -> aucun token LSP (couleur type TextMate)
 *   - SAI_Y (enfant d'un Volet)     -> property (type 2, vert fonce)
 *   - surChangementVolet (event)    -> aucun token LSP (TextMate bleu) */
static void test_semantic_onglet_volet_enfant_event_fr(void) {
    printf("\n[TEST] fix/1313 : Onglet/Volet/enfant/event (FR) -> property + event non-override\n");
    const char* src =
        "FEN_Test est une Fenetre\n"                    /* 0 */
        "    ONG_X est un Champ Onglet\n"               /* 1 : ONG_X col 4 */
        "        VOL_A est un Volet\n"                  /* 2 : VOL_A col 8 */
        "            SAI_Y est un Champ Saisie\n"       /* 3 : SAI_Y col 12 */
        "            FIN\n"                             /* 4 */
        "        FIN\n"                                 /* 5 */
        "        PROCEDURE surChangementVolet()\n"      /* 6 : nom col 18 */
        "        FIN\n"                                 /* 7 */
        "    FIN\n"                                     /* 8 */
        "FIN\n";                                        /* 9 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "onglet FR");

    /* ONG_X : nom du Champ Onglet -> property (2), comme tout champ. */
    ASSERT_HAS_TOKEN(result, 1, 4, 2, "ONG_X (Champ Onglet) -> property (vert fonce)");
    /*  — VOL_A : le NOM du sous-bloc Volet est un nom de champ ->
     * property (2, vert fonce), comme ONG_X. Seul le TYPE `Volet` reste
     * en couleur type (vert clair). */
    ASSERT_HAS_TOKEN(result, 2, 8, 2, "VOL_A (nom du Volet) -> property (vert fonce)");
    /* Le TYPE `Volet` (col 21) reste sans token LSP : couleur type par la
     * grammar TextMate (arbitrage , non-regression). */
    ASSERT_NO_TOKEN_AT(result, 2, 21, "type Volet -> pas d'override LSP (couleur type)");
    /* SAI_Y : enfant d'un Volet (recursion Onglet->Volet->enfant) -> property (2). */
    ASSERT_HAS_TOKEN(result, 3, 12, 2, "SAI_Y (enfant de Volet) -> property (vert fonce)");
    /* surChangementVolet : event officiel -> le LSP n'emet PAS de token method
     * (type 5), la grammar TextMate colore bleu. */
    ASSERT_NO_TOKEN_AT(result, 6, 18, "surChangementVolet (event) -> pas d'override LSP");

    json_free(result);
    json_free(params);
}

/* Cas EN : Tab -> Pane -> enfant + event onPaneChange (miroir bilingue). */
static void test_semantic_onglet_volet_enfant_event_en(void) {
    printf("\n[TEST] fix/1313 : Tab/Pane/enfant/event (EN) -> property + event non-override\n");
    const char* src =
        "FEN_Test est une Fenetre\n"                    /* 0 */
        "    ONG_X is a Control Tab\n"                  /* 1 : ONG_X col 4 */
        "        VOL_A is a Pane\n"                     /* 2 : VOL_A col 8 */
        "            SAI_Y is a Control Input\n"        /* 3 : SAI_Y col 12 */
        "            FIN\n"                             /* 4 */
        "        FIN\n"                                 /* 5 */
        "        PROCEDURE onPaneChange()\n"            /* 6 : nom col 18 */
        "        FIN\n"                                 /* 7 */
        "    FIN\n"                                     /* 8 */
        "FIN\n";                                        /* 9 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "onglet EN");

    ASSERT_HAS_TOKEN(result, 1, 4, 2, "ONG_X (Control Tab) -> property (vert fonce)");
    ASSERT_HAS_TOKEN(result, 2, 8, 2, "VOL_A (nom du Pane) -> property (vert fonce)");
    ASSERT_NO_TOKEN_AT(result, 2, 19, "type Pane -> pas d'override LSP (couleur type)");
    ASSERT_HAS_TOKEN(result, 3, 12, 2, "SAI_Y (enfant de Pane) -> property (vert fonce)");
    ASSERT_NO_TOKEN_AT(result, 6, 18, "onPaneChange (event) -> pas d'override LSP");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — propriete d'en-tete de colonne ecrite `Label`/`Libellé`
 *  (forme EN/FR du widget Libellé, scannee TOKEN_LIBELLE) en position de
 *  PROP dans un bloc PROPRIETES -> property (2, vert), pas keyword (orange).
 *  Non-regression : `Libellé`/`Label` en position de DECLARATION de widget
 *  (`LBL_x est un Libellé`) reste sans token LSP (couleur keyword TextMate).
 * ================================================================ */

/* Cas propriete : `Label`/`Libellé` en tete de statement dans le PROPRIETES
 * d'une Colonne. Le token est TOKEN_LIBELLE (keyword scanner), pas un IDENT :
 * sans la branche  il retombe keyword (orange) via TextMate, alors que
 * les proprietes voisines (Width, X...) sont vertes. */
static void test_semantic_column_label_property_fr(void) {
    printf("\n[TEST] fix/1870 : `Label`/`Libellé` prop d'en-tete de colonne -> property (2, vert)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"                    /* 0 */
        "    TABLE_X est un Champ Table\n"              /* 1 */
        "        COL_X est une Colonne\n"               /* 2 */
        "            PROPRIETES\n"                       /* 3 */
        "                Label = \"Nom\"\n"             /* 4 : Label col 16 */
        "                Width = 100\n"                 /* 5 : Width col 16 */
        "            FIN\n"                             /* 6 */
        "        FIN\n"                                 /* 7 */
        "    FIN\n"                                     /* 8 */
        "FIN\n";                                        /* 9 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "column Label FR");

    /* Label (col 16, ligne 4) : forme EN du Libellé, en position de prop ->
     * property (2). C'est le coeur du bug support #301. */
    ASSERT_HAS_TOKEN(result, 4, 16, 2, "Label (prop en-tete colonne) -> property (2, vert)");
    /* Width (col 16, ligne 5) : prop voisine deja verte (IDENT, branche
     * ). Ancre de comparaison : les deux props sont vertes. */
    ASSERT_HAS_TOKEN(result, 5, 16, 2, "Width (prop voisine) -> property (2, vert)");

    json_free(result);
    json_free(params);
}

/* Cas FR canon `Libellé` (accentue) : meme code path (TOKEN_LIBELLE), on
 * verrouille la forme accentuee -> property (2). */
static void test_semantic_column_libelle_property_accent(void) {
    printf("\n[TEST] fix/1870 : `Libell\xc3\xa9` (accentue) prop de colonne -> property (2, vert)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"                    /* 0 */
        "    TABLE_X est un Champ Table\n"              /* 1 */
        "        COL_X est une Colonne\n"               /* 2 */
        "            PROPRIETES\n"                       /* 3 */
        "                Libell\xc3\xa9 = \"Nom\"\n"    /* 4 : Libellé col 16 */
        "            FIN\n"                             /* 5 */
        "        FIN\n"                                 /* 6 */
        "    FIN\n"                                     /* 7 */
        "FIN\n";                                        /* 8 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "column Libellé accent");

    ASSERT_HAS_TOKEN(result, 4, 16, 2, "Libell\xc3\xa9 (prop en-tete colonne) -> property (2, vert)");

    json_free(result);
    json_free(params);
}

/* Non-regression : `Libellé` comme TYPE de declaration de widget
 * (`LBL_x est un Libellé`) NE doit PAS devenir property. Aucun token LSP a
 * la position du type -> la grammar TextMate le colore keyword (orange). */
static void test_semantic_libelle_declaration_stays_keyword(void) {
    printf("\n[TEST] fix/1870 non-reg : `Libell\xc3\xa9` type de declaration reste keyword (pas de token LSP)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"                    /* 0 */
        "    LBL_x est un Libell\xc3\xa9\n"             /* 1 : LBL_x col 4, Libellé col 17 */
        "        PROPRIETES\n"                           /* 2 */
        "            Texte = \"Bonjour\"\n"             /* 3 */
        "        FIN\n"                                 /* 4 */
        "    FIN\n"                                     /* 5 */
        "FIN\n";                                        /* 6 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "Libellé declaration");

    /* LBL_x (col 4) : nom de controle -> property (2), inchange. */
    ASSERT_HAS_TOKEN(result, 1, 4, 2, "LBL_x (nom de Libelle) -> property (2)");
    /* Le TYPE `Libellé` (col 17) : aucun token LSP -> keyword TextMate. */
    ASSERT_NO_TOKEN_AT(result, 1, 17, "type Libell\xc3\xa9 en declaration -> pas d'override LSP (keyword)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — LSP scope classe / params : champs = property (vert fonce),
 *             params = locale (sans modifier "global")
 * ================================================================ */

/* Test 1 : Dans body de Classe, champ declare "sNom est une chaine = """
 * → token type 2 (property, vert fonce), PAS cyan (variable globale). */
static void test_semantic_class_field_not_global(void) {
    printf("\n[TEST] fix/0277 : champ classe (sNom, rPrix) → property (type=2, vert fonce), pas cyan\n");
    const char* src =
        "cProduit est une Classe\n"
        "PUBLIC\n"
        "    sNom  est une chaîne = \"\"\n"
        "    rPrix est un réel    = 0.0\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* sNom ligne 2 col 4 (champ classe) : token type 2 'property' attendu (vert fonce #009180) */
    ASSERT_HAS_TOKEN(result, 2, 4, 2, "sNom (champ classe) → property (vert fonce #009180)");
    /* rPrix ligne 3 col 4 (champ classe) : token type 2 'property' attendu */
    ASSERT_HAS_TOKEN(result, 3, 4, 2, "rPrix (champ classe) → property (vert fonce #009180)");

    json_free(result);
    json_free(params);
}

/* Test 2 : Parametre de procedure top-level "PROCEDURE Calcule(nA est un entier)"
 * → nA est tokenType=3 (variable) SANS modifier "global" (donc tokenMods=0).
 * Aujourd'hui nA/nB tombent en variable (type=3), ce qui est correct.
 * Le BUG est qu'ils ont tokenModifiers=1 (global) au lieu de 0 (local).
 * Ce test verifie que le modifier est bien 0 (local). */
static void test_semantic_procedure_param_local_not_global(void) {
    printf("\n[TEST] fix/0277 : params procedure (nA, nB) → variable locale (type=3, mods=0), pas global (mods=1)\n");
    const char* src =
        "PROCEDURE Calcule(nA est un entier, nB est un entier)\n"
        "    nResultat est un entier = nA + nB\n"
        "    RENVOYER nResultat\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* nA ligne 0 col 18 (param) : tokenType=3 (variable), tokenModifiers=0 (local, pas global).
     * BUG ACTUEL : tokenModifiers=1 (global) → echoue. Apres fix : tokenModifiers=0 → passe. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 18, 3, 0, "nA (param) → variable locale (mods=0)");
    /* nB ligne 0 col 36 (param) : tokenType=3 (variable), tokenModifiers=0 (local). */
    ASSERT_HAS_TOKEN_OPT(result, 0, 36, 3, 0, "nB (param) → variable locale (mods=0)");
    /* nResultat ligne 1 col 4 (variable locale declaree dans body) : aussi tokenModifiers=0. */
    ASSERT_HAS_TOKEN_OPT(result, 1, 4, 3, 0, "nResultat (var locale body) → mods=0");

    json_free(result);
    json_free(params);
}

/* Test 3 : Parametre dans Constructeur d'une Classe (cas combine).
 * Constructeur(sNom est une chaine) → sNom est param (type=3, mods=0).
 * sTitulaire (champ classe) → property (type=2). */
static void test_semantic_class_constructor_param_vs_field(void) {
    printf("\n[TEST] fix/0277 : Constructeur param (sNom) vs champ (sTitulaire)\n");
    const char* src =
        "cCompte est une Classe\n"
        "PUBLIC\n"
        "    sTitulaire est une chaîne = \"\"\n"
        "    PROCEDURE Constructeur(sNom est une chaîne)\n"
        "        :sTitulaire = sNom\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* sTitulaire ligne 2 col 4 (champ classe) : token type 2 'property' */
    ASSERT_HAS_TOKEN(result, 2, 4, 2, "sTitulaire (champ) → property (type=2)");
    /* sNom ligne 3 col 27 (param Constructeur) : tokenType=3, tokenModifiers=0 (local) */
    ASSERT_HAS_TOKEN_OPT(result, 3, 27, 3, 0, "sNom (param Constructeur) → variable locale (mods=0)");
    /* sNom ligne 4 col 22 (usage param dans body) : aussi tokenType=3, tokenModifiers=0 */
    ASSERT_HAS_TOKEN_OPT(result, 4, 22, 3, 0, "sNom (usage body) → variable locale (mods=0)");

    json_free(result);
    json_free(params);
}

/* Test 4 : Non-regression top-level. Variable globale au top-level reste cyan
 * (tokenType=3, tokenModifiers=1). */
static void test_semantic_global_variable_non_regression(void) {
    printf("\n[TEST] fix/0277 : non-regression — variable globale top-level reste cyan (mods=1)\n");
    const char* src =
        "nGlobal est un entier = 42\n"
        "sMessage est une chaîne = \"test\"\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* nGlobal ligne 0 col 0 (variable globale) : tokenType=3, tokenModifiers=1 (global) */
    ASSERT_HAS_TOKEN_OPT(result, 0, 0, 3, 1, "nGlobal (globale) → variable cyan (mods=1)");
    /* sMessage ligne 1 col 0 (variable globale) : tokenType=3, tokenModifiers=1 (global) */
    ASSERT_HAS_TOKEN_OPT(result, 1, 0, 3, 1, "sMessage (globale) → variable cyan (mods=1)");

    json_free(result);
    json_free(params);
}

/*  — bascules de portee top-level GLOBAL/LOCAL. Une variable declaree
 * sous LOCAL est confinee : token variable (type=3) SANS modifier `global`
 * (mods=0 → vert). Une variable declaree sous GLOBAL, ou avant toute bascule
 * (defaut = GLOBAL), est promue : token variable (type=3) AVEC modifier `global`
 * (mods=1 → cyan). Test caracterisant l'emission existante (bit 0 = global). */
static void test_semantic_toplevel_global_local_scope(void) {
    printf("\n[TEST] feat/1167 : bascules top-level GLOBAL/LOCAL → modifier global du token\n");
    const char* src =
        "gTotal est un entier = 0\n"
        "LOCAL\n"
        "compteurLocal est un entier = 0\n"
        "GLOBAL\n"
        "gAutre est un entier = 0\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* gTotal ligne 0 col 0 (defaut avant bascule → promue) : type=3, mods=1 */
    ASSERT_HAS_TOKEN_OPT(result, 0, 0, 3, 1, "gTotal (defaut → global) → variable cyan (mods=1)");
    /* compteurLocal ligne 2 col 0 (sous LOCAL → confinee) : type=3, mods=0 */
    ASSERT_HAS_TOKEN_OPT(result, 2, 0, 3, 0, "compteurLocal (sous LOCAL → confinee) → variable verte (mods=0)");
    /* gAutre ligne 4 col 0 (sous GLOBAL → promue) : type=3, mods=1 */
    ASSERT_HAS_TOKEN_OPT(result, 4, 0, 3, 1, "gAutre (sous GLOBAL → promue) → variable cyan (mods=1)");

    json_free(result);
    json_free(params);
}

/* Test 5 : Usage d'un champ depuis l'interieur de la classe (via `:nSolde`).
 * rMontant (param procedure) → variable locale (type=3, mods=0).
 * :nSolde (member access) → deja gere probablement, mais verifie que rMontant
 * n'est pas cyan (il ne doit PAS avoir tokenModifiers=1). */
static void test_semantic_class_method_param_usage(void) {
    printf("\n[TEST] fix/0277 : params methode classe (rMontant) → locale, pas global\n");
    const char* src =
        "cCompte est une Classe\n"
        "PUBLIC\n"
        "    nSolde est un entier = 0\n"
        "    PROCEDURE Crédite(rMontant est un réel)\n"
        "        :nSolde = :nSolde + rMontant\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* nSolde ligne 2 col 4 (champ classe) : token type 2 'property' */
    ASSERT_HAS_TOKEN(result, 2, 4, 2, "nSolde (champ) → property (type=2)");
    /* rMontant ligne 3 col 22 (param procedure) : tokenType=3, tokenModifiers=0 (local) */
    ASSERT_HAS_TOKEN_OPT(result, 3, 22, 3, 0, "rMontant (param) → variable locale (mods=0)");
    /* rMontant ligne 4 col 28 (usage param dans body) : aussi tokenType=3, tokenModifiers=0 */
    ASSERT_HAS_TOKEN_OPT(result, 4, 28, 3, 0, "rMontant (usage body) → variable locale (mods=0)");

    json_free(result);
    json_free(params);
}

/*  — variable declaree DIRECTEMENT dans un bloc widget = LOCALE au
 * widget, donc token variable (type=3) SANS le modifier global (mods=0).
 * Une variable au top-level de la Fenetre reste globale (mods=1).
 *
 * Avant fix : le scope-stack ne poussait rien pour les blocs widget /
 * PROPRIETES → une var de bloc widget etait evaluee stack vide →
 * IS_TOPLEVEL_GLOBAL()=true → mods=1 (cyan trompeur). */
static void test_semantic_widget_local_var_not_global(void) {
    printf("\n[TEST] fix/0954 : var de bloc widget → locale (mods=0), var top-level Fenetre → globale (mods=1)\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "    PROPRIETES\n"
        "        Titre = \"x\"\n"
        "    FIN\n"
        "    gGlobaleFenetre est une chaine = \"reste global\"\n"
        "    CMB_X est un Champ Combo\n"
        "        PROPRIETES\n"
        "            Largeur = 225\n"
        "        FIN\n"
        "        sLocaleWidget est une chaine = \"PAS global\"\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* gGlobaleFenetre ligne 4 col 4 : var top-level Fenetre → globale (mods=1). */
    ASSERT_HAS_TOKEN_OPT(result, 4, 4, 3, 1, "gGlobaleFenetre (top-level Fenetre) → globale (mods=1)");
    /* sLocaleWidget ligne 9 col 8 : var de bloc widget → PAS globale (mods=0). */
    ASSERT_HAS_TOKEN_OPT(result, 9, 8, 3, 0, "sLocaleWidget (bloc widget) → locale (mods=0)");

    json_free(result);
    json_free(params);
}

/*  — non-regression : un widget qui contient une PROCEDURE evenement.
 * La var top-level Fenetre reste globale, la var de bloc widget reste locale,
 * et la var declaree dans la PROCEDURE du widget reste locale (proc = isolant).
 * Verifie que l'equilibre du scope-stack (Fenetre/widget/PROPRIETES/proc)
 * n'est pas casse par les blocs imbriques. */
static void test_semantic_widget_procedure_local_non_regression(void) {
    printf("\n[TEST] fix/0954 : widget + PROCEDURE — global top-level + locales widget/proc\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "    gTop est un entier = 1\n"
        "    BTN_X est un Champ Bouton\n"
        "        PROPRIETES\n"
        "            Libelle = \"ok\"\n"
        "        FIN\n"
        "        sWidgetVar est une chaine = \"local\"\n"
        "        PROCEDURE surClic()\n"
        "            nLocalProc est un entier = 5\n"
        "        FIN\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* gTop ligne 1 col 4 : var top-level Fenetre → globale (mods=1). */
    ASSERT_HAS_TOKEN_OPT(result, 1, 4, 3, 1, "gTop (top-level Fenetre) → globale (mods=1)");
    /* sWidgetVar ligne 6 col 8 : var de bloc widget → locale (mods=0). */
    ASSERT_HAS_TOKEN_OPT(result, 6, 8, 3, 0, "sWidgetVar (bloc widget) → locale (mods=0)");
    /* nLocalProc ligne 8 col 12 : var declaree dans la PROCEDURE du widget → locale (mods=0). */
    ASSERT_HAS_TOKEN_OPT(result, 8, 12, 3, 0, "nLocalProc (proc widget) → locale (mods=0)");

    json_free(result);
    json_free(params);
}

/*  — param FONCTION top-level homonyme d'une constante registry.
 *
 * Repro : `FONCTION MontantDepuisTexte(sTexte est une chaîne)` — le param
 * `sTexte` collisionne (casse-insensible) avec une constante registry
 * (cas historique `STexte`, famille Saisie). La coloration jaune italique
 * "constante" vient de la grammar TextMate du plugin ; cote LSP, le fix
 * est d'emettre un semantic token `variable` (type=3, mods=0) sur le param
 * ET ses usages pour ecraser la grammar.
 *
 * La passe scope-aware couvre deja SCOPE_PROC/SCOPE_PARAMS, y
 * compris pour FONCTION top-level avec type de retour `: <type>` apres le
 * `)`, et au milieu d'autres declarations (Structure, globales, autres
 * procs). Ce test verrouille ce comportement en non-regression : le param
 * et son usage restent classes `variable`, jamais `enumMember`/constante,
 * quelle que soit la collision de nom avec le registry.
 *
 * Header : `FONCTION MontantDepuisTexte(sTexte est une chaîne) : monétaire`
 *   col 0  : FONCTION
 *   col 9  : MontantDepuisTexte (18 chars)
 *   col 27 : (
 *   col 28 : sTexte (param)
 * Body L1 : `\tsNorm est une chaîne = sTexte`
 *   col 0  : tab
 *   col 1  : sNorm (var locale)
 *   col 24 : sTexte (usage param) */
static void test_semantic_fonction_param_shadows_registry_const(void) {
    printf("\n[TEST] fix/0517 : param FONCTION (sTexte) homonyme constante registry → variable locale (type=3), pas constante\n");
    const char* src =
        "STDepense est une Structure\n"            /* L0 */
        "\tsLibelle est une chaîne\n"
        "\tmMontant est un monétaire\n"
        "FIN\n"
        "tabDepenses est un tableau de STDepense\n" /* L4 */
        "PROCEDURE ChargeDonnees()\n"               /* L5 */
        "\tSI 1 = 1 ALORS\n"
        "\t\tRENVOYER\n"
        "\tFIN\n"
        "FIN\n"
        "FONCTION MontantDepuisTexte(sTexte est une chaîne) : monétaire\n" /* L10 */
        "\tsNorm est une chaîne = sTexte\n"         /* L11 */
        "\tRENVOYER sNorm\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* sTexte (declaration param) ligne 10 col 28 : variable locale (type=3, mods=0). */
    ASSERT_HAS_TOKEN_OPT(result, 10, 28, 3, 0, "sTexte (param FONCTION) → variable locale (mods=0)");
    /* sTexte (usage dans body) ligne 11 col 24 : aussi variable locale (type=3, mods=0).
     * Sans token semantic emis ici, la grammar TextMate colorerait sTexte en
     * constante (jaune italique) — c'est le coeur du bug. */
    ASSERT_HAS_TOKEN_OPT(result, 11, 24, 3, 0, "sTexte (usage body) → variable locale (mods=0)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — LSP `:ident` (self member access OWL) = member access
 *
 *  Bug : `isMemberAccess = (prev == TOKEN_DOT || prev == TOKEN_DOUBLE_DOT)`
 *  omettait TOKEN_COLON. Conséquence : `:sTitulaire` (acces au champ
 *  classe via self-prefix `:`) ne passait pas dans les branches qui
 *  excluent member access. Pour un champ classe `sTitulaire` (kind=6,
 *  isGlobal=false) ca ne se manifestait pas immediatement. Mais des
 *  qu'un IDENT homonyme global existe par ailleurs dans le workspace
 *  scan, `:ident` se faisait classifier 'variable globale' (cyan, mods=1)
 *  au lieu d'etre laisse a la grammar TextMate comme un member access
 *  classique `obj.ident`.
 *
 *  Tests :
 *   - 0278-T1 : `:nGlobal` dans body proc d'une classe — alors qu'une
 *     variable globale `nGlobal` existe au top-level — NE doit PAS etre
 *     classifie cyan (tokenType=3 mods=1). Apres le fix : isMemberAccess
 *     vrai → skip globalVar block → aucun token emis (grammar TextMate
 *     prend le relais).
 *   - 0278-T2 : non-regression `obj.field` continue a etre traite comme
 *     member access (donc pas classifie variable globale meme si homonyme).
 *   - 0278-T3 : non-regression — un IDENT n'etant PAS member access
 *     (`nGlobal` au top-level apres declaration) reste bien cyan.
 * ================================================================ */

/* Test 0278-T1 : `:ident` dans body proc classe = member access classe.
 *
 * Note ajustement  (2026-06-04) : le test attendait initialement
 * "aucun token emis" (delegation TextMate).  a precise la decision
 * design : le `:ident` qui matche un champ de la classe englobante DOIT
 * etre emis explicitement comme property (tokenType=2, vert fonce).
 * Verbatim user  : "nSolde doit etre en vert fonce car on reference
 * un membre de la classe". L'assertion est donc inversee : on attend
 * tokenType=2 (property) sur `:nGlobal`, PAS l'absence de token.
 * Cf. ticket  (recadre LSP TOKEN_COLON dans isMemberAccess) et
 *  (classification scope-aware member access classe). */
static void test_semantic_colon_member_access_classe(void) {
    printf("\n[TEST] fix/0278+0284 : `:ident` (self member access) → property classe (vert fonce)\n");
    /* Setup deliberement piegeur : variable globale `nGlobal` ET classe
     * cCompte avec champ `nGlobal`. Quand on ecrit `:nGlobal` dans le body
     * du Constructeur, le `:` indique acces au champ classe — pas a la
     * globale. Pre-fix : `:` n'est pas member access → `nGlobal` matche
     * globalVar (cyan). Post- : `:` member access + lookup champ
     * classe englobante → tokenType=2 (property). */
    const char* src =
        "nGlobal est un entier = 0\n"
        "cCompte est une Classe\n"
        "PUBLIC\n"
        "    nGlobal est un entier = 0\n"
        "    PROCEDURE Constructeur()\n"
        "        :nGlobal = 42\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    dump_semantic_tokens(result, "0278-T1 (ajuste fix/0284)");

    /* `:nGlobal` ligne 5 col 9 : le ':' est col 8, le 'n' commence col 9.
     * ASSERT : tokenType=2 (property), pas tokenType=3 (variable cyan). */
    if (has_semantic_token_opt(result, 5, 9, 2, 0)) {
        g_pass++;
        printf("  [OK]   :nGlobal classe property (vert fonce) — member access classe\n");
    } else {
        g_fail++;
        printf("  [FAIL] :nGlobal pas property (vert fonce) — fix/0284 attendu\n");
    }

    /* Non-regression : NE doit PAS etre cyan globale (type=3 mods=1). */
    if (has_semantic_token_opt(result, 5, 9, 3, 1)) {
        g_fail++;
        printf("  [FAIL] :nGlobal classe cyan (mods=1) alors qu'il est member access classe\n");
    } else {
        g_pass++;
        printf("  [OK]   :nGlobal pas classe cyan (member access classe respecte)\n");
    }

    json_free(result);
    json_free(params);
}

/* Test 0278-T2 : non-regression `obj.ident` continue d'etre member access. */
static void test_semantic_dot_member_access_non_regression(void) {
    printf("\n[TEST] fix/0278 : non-regression obj.field reste member access\n");
    const char* src =
        "nGlobal est un entier = 0\n"
        "PROCEDURE Test()\n"
        "    oObj est un objet dynamique\n"
        "    nValeur est un entier = oObj.nGlobal\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* `oObj.nGlobal` ligne 3 — le `nGlobal` apres `.` est member access,
     * donc PAS cyan (mods=1) malgre l'homonyme global. */
    int dotPos = 0;
    /* Calcul position : "    nValeur est un entier = oObj." → 33 chars
     * avant le 'n' de nGlobal. Plus simple : on cherche le token a la fin
     * de la ligne et on verifie qu'il n'est pas globalVar. */
    (void)dotPos;
    /* Approximation robuste : aucun token cyan (type=3 mods=1) a la ligne 3
     * apres la colonne 30. */
    bool foundCyanAfter = false;
    if (result && result->type == JSON_OBJECT) {
        JsonValue* data = json_get(result, "data");
        if (data && data->type == JSON_ARRAY) {
            int count = data->as.arrayVal.count;
            int curLine = 0, curStart = 0;
            for (int i = 0; i < count; i += 5) {
                int dL = data->as.arrayVal.items[i]->as.intVal;
                int dS = data->as.arrayVal.items[i+1]->as.intVal;
                int tT = data->as.arrayVal.items[i+3]->as.intVal;
                int tM = data->as.arrayVal.items[i+4]->as.intVal;
                curLine += dL;
                if (dL != 0) curStart = 0;
                curStart += dS;
                if (curLine == 3 && curStart > 30 && tT == 3 && tM == 1) {
                    foundCyanAfter = true;
                }
            }
        }
    }
    if (foundCyanAfter) {
        g_fail++;
        printf("  [FAIL] obj.nGlobal classifie cyan (regression member access)\n");
    } else {
        g_pass++;
        printf("  [OK]   obj.nGlobal pas cyan (member access preserve)\n");
    }

    json_free(result);
    json_free(params);
}

/* Test 0278-T3 : non-regression variable globale isolee reste cyan. */
static void test_semantic_global_var_isolated_non_regression(void) {
    printf("\n[TEST] fix/0278 : non-regression — globale top-level isolee reste cyan\n");
    const char* src =
        "nGlobal est un entier = 42\n"
        "PROCEDURE Test()\n"
        "    nLocale est un entier = nGlobal\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* nGlobal ligne 0 col 0 (declaration top-level) : type=3 mods=1. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 0, 3, 1, "nGlobal (decl globale) → cyan (mods=1)");
    /* nGlobal ligne 2 col 28 (usage non-prefixe) : type=3 mods=1 aussi. */
    ASSERT_HAS_TOKEN_OPT(result, 2, 28, 3, 1, "nGlobal (usage non-prefixe) → cyan (mods=1)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — Etat sub-blocs vert fonce + member access classe
 * ================================================================ */

/* Test 0284-T1 : Etat sub-blocs (intro/corps/pied) → tokenType=1 (class, vert fonce).
 * Bug observe : scannés kind='E' mais classifiés cyan au lieu de vert fonce.
 * Cause suspectée : scope tracker  écrase la classification controlKind=='E'. */
static void test_semantic_etat_subblocks_control_names(void) {
    printf("\n[TEST] fix/0284 : Etat sub-blocs (intro/corps/pied) → tokenType=1 (class, vert fonce)\n");
    const char* src =
        "ETAT_Mensuel est un Etat\n"
        "    PROPRIETES Format=formatA4 FIN\n"
        "    intro est un EnTeteDocument\n"
        "        PROPRIETES Hauteur=30 FIN\n"
        "    FIN\n"
        "    corps est un Corps\n"
        "        PROPRIETES Hauteur=20 FIN\n"
        "    FIN\n"
        "    pied est un BasDePage\n"
        "        PROPRIETES Hauteur=15 FIN\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* dump_semantic_tokens(result, "0284-T1 Etat subblocks"); */

    /* ETAT_Mensuel ligne 0 col 0 (top-level Etat) : tokenType=1 (class). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "ETAT_Mensuel (top-level) → class (vert fonce)");

    /* intro ligne 2 col 4 (sub-bloc EnTeteDocument) : tokenType=1 (class, RED attendu). */
    ASSERT_HAS_TOKEN(result, 2, 4, 1, "intro (sub-bloc) → class (vert fonce) [RED attendu]");

    /* corps ligne 5 col 4 (sub-bloc Corps) : tokenType=1 (class, RED attendu). */
    ASSERT_HAS_TOKEN(result, 5, 4, 1, "corps (sub-bloc) → class (vert fonce) [RED attendu]");

    /* pied ligne 8 col 4 (sub-bloc BasDePage) : tokenType=1 (class, RED attendu). */
    ASSERT_HAS_TOKEN(result, 8, 4, 1, "pied (sub-bloc) → class (vert fonce) [RED attendu]");

    json_free(result);
    json_free(params);
}

/* Test 0284-T2 : Types des sub-blocs (EnTeteDocument/Corps/BasDePage) restent tokenType=4 (type, violet).
 * Bug potentiel : le scan Etat marque les NOMS en kind='E', mais les TYPES doivent
 * rester violet — vérifier non-régression. */
static void test_semantic_etat_subblocks_types_preserved(void) {
    printf("\n[TEST] fix/0284 : Types Etat (EnTeteDocument/Corps/BasDePage) → tokenType=4 (type, violet)\n");
    const char* src =
        "ETAT_Mensuel est un Etat\n"
        "    PROPRIETES Format=formatA4 FIN\n"
        "    intro est un EnTeteDocument\n"
        "        PROPRIETES Hauteur=30 FIN\n"
        "    FIN\n"
        "    corps est un Corps\n"
        "        PROPRIETES Hauteur=20 FIN\n"
        "    FIN\n"
        "    pied est un BasDePage\n"
        "        PROPRIETES Hauteur=15 FIN\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* EnTeteDocument ligne 2 col 17 (type après 'est un') : tokenType=4 (type, violet). */
    ASSERT_HAS_TOKEN(result, 2, 17, 4, "EnTeteDocument (type) → violet (non-regression)");

    /* Corps ligne 5 col 17 (type) : tokenType=4 (type, violet). */
    ASSERT_HAS_TOKEN(result, 5, 17, 4, "Corps (type) → violet (non-regression)");

    /* BasDePage ligne 8 col 16 (type) : tokenType=4 (type, violet). */
    ASSERT_HAS_TOKEN(result, 8, 16, 4, "BasDePage (type) → violet (non-regression)");

    json_free(result);
    json_free(params);
}

/*  — la forme EN d'un type compose (Font = EN de Police) en position
 * de declaration (`workfont est un Font`) doit etre coloree TYPE (tokenType=4,
 * violet), comme `Police`. Symptome support : Police violet, Font vert. La
 * classification `is_composed_type_name` iterait le catalogue en FR only. */
static void test_semantic_composed_type_en_font(void) {
    printf("\n[TEST] fix/1073 : type compose EN Font (est un / is) → tokenType=4 (type, violet)\n");
    const char* src =
        "workfont est un Font\n"   /* Font col 16 */
        "wf is Font\n"             /* Font col 6 */
        "wp est un Police\n";      /* Police col 10 (non-regression FR) */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    ASSERT_HAS_TOKEN(result, 0, 16, 4, "Font (est un) → violet");
    ASSERT_HAS_TOKEN(result, 1, 6, 4, "Font (is) → violet");
    ASSERT_HAS_TOKEN(result, 2, 10, 4, "Police (FR) → violet (non-regression)");

    json_free(result);
    json_free(params);
}

/*  — type compose EN en position de declaration avec la copule
 * anglaise `is a` (`X is a styleContainer`). L'article `a` (TOKEN_A)
 * s'intercale entre la copule et le type ; la branche `is_composed_type_name`
 * ne testait que `prev == TOKEN_EST` -> en EN le type retombait neutre
 * (class/vert). Doit etre tokenType=4 (type, violet), comme en FR. */
static void test_semantic_composed_type_en_container(void) {
    printf("\n[TEST] fix/1122 : type compose EN via `is a styleContainer` → tokenType=4 (type, violet)\n");
    const char* src =
        "X is a styleContainer\n"      /* styleContainer col 7 */
        "Y est un styleConteneur\n";   /* styleConteneur col 9 (non-regression FR) */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* styleContainer (EN, is a) col 7 : violet. ROUGE avant fix (copule EN). */
    ASSERT_HAS_TOKEN(result, 0, 7, 4, "styleContainer (is a) → violet");
    /* styleConteneur (FR, est un) col 9 : violet (non-regression). */
    ASSERT_HAS_TOKEN(result, 1, 9, 4, "styleConteneur (est un) → violet (non-regression)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — nom d'une declaration Projet colore comme une Fenetre
 * ================================================================ */

/* Le nom d'un `<Ident> est un Projet` doit etre classifie tokenType=1
 * (class, vert fonce), comme un nom de Fenetre/Etat — pas tokenType=3
 * (variable globale, cyan). */
static void test_semantic_projet_name_class_fr(void) {
    printf("\n[TEST] fix/0892 : nom Projet (FR) → tokenType=1 (class, vert fonce)\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFrançais\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* MonApp ligne 0 col 0 : tokenType=1 (class). ROUGE avant fix (type=3). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "MonApp (decl Projet FR) → class (vert fonce)");
    /* Le TYPE `Projet` (ligne 0 col 14) ne doit PAS etre colore class. */
    if (has_semantic_token(result, 0, 14, 1)) {
        printf("  [FAIL] le type 'Projet' ne doit pas etre classifie class (type=1)\n");
        g_fail++;
    } else {
        printf("  [OK]   type 'Projet' non classifie class\n");
        g_pass++;
    }

    json_free(result);
    json_free(params);
}

/* Graphie EN : `<Ident> is a Project` → nom tokenType=1. */
static void test_semantic_projet_name_class_en(void) {
    printf("\n[TEST] fix/0892 : nom Project (EN) → tokenType=1 (class, vert fonce)\n");
    const char* src =
        "MonApp is a Project\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFrançais\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* MonApp ligne 0 col 0 : tokenType=1 (class). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "MonApp (decl Project EN) → class (vert fonce)");

    json_free(result);
    json_free(params);
}

/* Non-regression  : une variable globale ordinaire nommee comme le
 * type ("projet") reste cyan (type=3), et un identifiant "Projet" hors
 * position `est un Projet` n'est PAS colore class. */
static void test_semantic_projet_non_regression_ordinary_var(void) {
    printf("\n[TEST] fix/0892 : globale ordinaire reste cyan (pas de faux positif Projet)\n");
    const char* src =
        "nProjet est un entier = 3\n"
        "PROCEDURE Test()\n"
        "    nLocale est un entier = nProjet\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* nProjet ligne 0 col 0 : globale ordinaire → type=3 mods=1, PAS class. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 0, 3, 1, "nProjet (globale ordinaire) → cyan");
    if (has_semantic_token(result, 0, 0, 1)) {
        printf("  [FAIL] nProjet ne doit pas etre classifie class (type=1)\n");
        g_fail++;
    } else {
        printf("  [OK]   nProjet non classifie class\n");
        g_pass++;
    }

    json_free(result);
    json_free(params);
}

/*  — nom declare d'un `<Ident> est un ServiceREST` et du sous-bloc
 * imbrique `<Ident> est une OpérationHTTP` : tokenType=1 (class, vert fonce),
 * comme Fenetre/Etat/Projet/Collection — pas tokenType=3 (globale, cyan).
 * L'operation est imbriquee dans le service : le scan lineaire doit la
 * classifier meme en contexte imbrique. Un `Etat` du meme fichier reste class
 * (non-regression). */
static void test_semantic_service_rest_operation_class_fr(void) {
    printf("\n[TEST] fix/2006 : noms ServiceREST + OpérationHTTP (FR) → class(1)\n");
    const char* src =
        "ServiceFacture est un ServiceREST\n"
        "\tHealthCheck est une OpérationHTTP\n"
        "\tFIN\n"
        "FIN\n"
        "ETAT_Facture est un État\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* ServiceFacture ligne 0 col 0 : class. ROUGE avant fix (type=3, cyan). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "ServiceFacture (decl ServiceREST) → class");
    /* HealthCheck ligne 1 col 1 (apres la tab) : class, meme imbrique. */
    ASSERT_HAS_TOKEN(result, 1, 1, 1, "HealthCheck (decl OpérationHTTP imbriquee) → class");
    /* Non-regression : ETAT_Facture (ligne 4 col 0) reste class. */
    ASSERT_HAS_TOKEN(result, 4, 0, 1, "ETAT_Facture (decl État) → class (non-regression)");
    /* Le TYPE `ServiceREST` (ligne 0 col 22) ne doit PAS etre colore class. */
    if (has_semantic_token(result, 0, 22, 1)) {
        printf("  [FAIL] le type 'ServiceREST' ne doit pas etre classifie class (type=1)\n");
        g_fail++;
    } else {
        printf("  [OK]   type 'ServiceREST' non classifie class\n");
        g_pass++;
    }

    json_free(result);
    json_free(params);
}

/* Graphie EN : `is a ServiceREST` / `is an HTTPOperation` → noms class(1). */
static void test_semantic_service_rest_operation_class_en(void) {
    printf("\n[TEST] fix/2006 : noms ServiceREST + HTTPOperation (EN) → class(1)\n");
    const char* src =
        "BillingService is a ServiceREST\n"
        "\tHealthCheck is an HTTPOperation\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* BillingService ligne 0 col 0 : class. */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "BillingService (decl ServiceREST EN) → class");
    /* HealthCheck ligne 1 col 1 : class, meme imbrique, article EN `an` saute. */
    ASSERT_HAS_TOKEN(result, 1, 1, 1, "HealthCheck (decl HTTPOperation EN) → class");

    json_free(result);
    json_free(params);
}

/* Non-regression  : une variable globale ordinaire n'est pas classifiee
 * class du seul fait qu'un ServiceREST existe ailleurs. */
static void test_semantic_service_non_regression_ordinary_var(void) {
    printf("\n[TEST] fix/2006 : globale ordinaire reste cyan (pas de faux positif service)\n");
    const char* src =
        "nService est un entier = 3\n"
        "PROCEDURE Test()\n"
        "    nLocale est un entier = nService\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* nService ligne 0 col 0 : globale ordinaire → type=3 mods=1, PAS class. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 0, 3, 1, "nService (globale ordinaire) → cyan");
    if (has_semantic_token(result, 0, 0, 1)) {
        printf("  [FAIL] nService ne doit pas etre classifie class (type=1)\n");
        g_fail++;
    } else {
        printf("  [OK]   nService non classifie class\n");
        g_pass++;
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — events natifs ServiceREST/OpérationHTTP colores event
 *  natif (pas method vert). `surRequête` (niveau OpérationHTTP) et
 *  `surErreur`/`surAuthentification` (niveau ServiceREST) doivent suivre
 *  le meme traitement que `surClic`/`surChangementVolet` : le LSP n'emet
 *  PAS de token method (type=5) sur le nom declare, la grammar TextMate
 *  prend le relais (couleur event, bleu). ROUGE avant fix (source unique
 *  catalogue absente pour le DSL service -> lsp_is_official_event rate
 *  ces noms -> token method (5) emis).
 * ================================================================ */

static void test_semantic_service_operation_event_not_method_fr(void) {
    printf("\n[TEST] fix/2012 : surRequete/surErreur/surAuthentification (FR) -> pas de token method\n");
    const char* src =
        "ServiceFacture est un ServiceREST\n"           /* 0 */
        "\tPROCEDURE surErreur()\n"                      /* 1 : nom col 12 */
        "\tFIN\n"                                        /* 2 */
        "\tPROCEDURE surAuthentification()\n"            /* 3 : nom col 12 */
        "\tFIN\n"                                        /* 4 */
        "\tHealthCheck est une OpérationHTTP\n"          /* 5 */
        "\t\tPROCEDURE surRequete()\n"                   /* 6 : nom col 13 */
        "\t\tFIN\n"                                      /* 7 */
        "\tFIN\n"                                        /* 8 */
        "FIN\n";                                         /* 9 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "service event FR");

    ASSERT_NO_TOKEN_AT(result, 1, 12, "surErreur (event service) -> pas de token method");
    ASSERT_NO_TOKEN_AT(result, 3, 12, "surAuthentification (event service) -> pas de token method");
    ASSERT_NO_TOKEN_AT(result, 6, 13, "surRequete (event operation) -> pas de token method");

    json_free(result);
    json_free(params);
}

/* Graphie EN : onError/onAuthentication/onRequest -> memes assertions. */
static void test_semantic_service_operation_event_not_method_en(void) {
    printf("\n[TEST] fix/2012 : onError/onAuthentication/onRequest (EN) -> pas de token method\n");
    const char* src =
        "BillingService is a ServiceREST\n"              /* 0 */
        "\tPROCEDURE onError()\n"                         /* 1 : nom col 12 */
        "\tFIN\n"                                         /* 2 */
        "\tPROCEDURE onAuthentication()\n"                /* 3 : nom col 12 */
        "\tFIN\n"                                         /* 4 */
        "\tHealthCheck is an HTTPOperation\n"             /* 5 */
        "\t\tPROCEDURE onRequest()\n"                     /* 6 : nom col 13 */
        "\t\tFIN\n"                                       /* 7 */
        "\tFIN\n"                                         /* 8 */
        "FIN\n";                                          /* 9 */
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);
    dump_semantic_tokens(result, "service event EN");

    ASSERT_NO_TOKEN_AT(result, 1, 12, "onError (event service EN) -> pas de token method");
    ASSERT_NO_TOKEN_AT(result, 3, 12, "onAuthentication (event service EN) -> pas de token method");
    ASSERT_NO_TOKEN_AT(result, 6, 13, "onRequest (event operation EN) -> pas de token method");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — TYPE d'une declaration Projet colore violet (type=4)
 * ================================================================ */

/* Le TYPE `Projet` d'un `<Ident> est un Projet` doit etre classifie
 * tokenType=4 (type, violet), comme un type de sous-bloc Etat — pas
 * neutre/cyan. Le NOM (1er token) reste tokenType=1 (class, ). */
static void test_semantic_projet_type_violet_fr(void) {
    printf("\n[TEST] fix/0894 : type Projet (FR) → tokenType=4 (type, violet)\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFrançais\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Projet ligne 0 col 14 : tokenType=4 (type). ROUGE avant fix. */
    ASSERT_HAS_TOKEN(result, 0, 14, 4, "Projet (type decl FR) → type (violet)");
    /* Non-regression  : le nom MonApp reste class (type=1). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "MonApp (nom decl FR) → class (vert fonce)");

    json_free(result);
    json_free(params);
}

/* Graphie EN : `<Ident> is a Project` → type Project tokenType=4. */
static void test_semantic_projet_type_violet_en(void) {
    printf("\n[TEST] fix/0894 : type Project (EN) → tokenType=4 (type, violet)\n");
    const char* src =
        "MonApp is a Project\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFrançais\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Project ligne 0 col 12 : tokenType=4 (type). */
    ASSERT_HAS_TOKEN(result, 0, 12, 4, "Project (type decl EN) → type (violet)");
    /* Le nom MonApp reste class (type=1). */
    ASSERT_HAS_TOKEN(result, 0, 0, 1, "MonApp (nom decl EN) → class (vert fonce)");

    json_free(result);
    json_free(params);
}

/* Non-regression  : un identifiant "Projet" hors position de type
 * de declaration (ici une globale ordinaire) n'est PAS colore type=4. */
static void test_semantic_projet_type_non_regression(void) {
    printf("\n[TEST] fix/0894 : 'Projet' hors declaration n'est pas type=4\n");
    const char* src =
        "Projet est un entier = 3\n"
        "PROCEDURE Test()\n"
        "    nLocale est un entier = Projet\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Le 'Projet' global (col 0) et son usage (ligne 2) ne doivent pas
     * etre type=4. La globale est declaree `Projet est un entier` : le
     * type reel est `entier`, pas Projet. */
    if (has_semantic_token(result, 0, 0, 4)) {
        printf("  [FAIL] 'Projet' (globale, decl) ne doit pas etre type=4\n");
        g_fail++;
    } else {
        printf("  [OK]   'Projet' (globale, decl) non type=4\n");
        g_pass++;
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — TYPE d'une declaration Theme colore violet (type=4)
 * ================================================================ */

/* Le TYPE `Thème` d'un `<Ident> est un Thème` doit etre classifie
 * tokenType=4 (type, violet), comme le type conteneur `Projet` —
 * pas neutre/cyan (defaut TextMate). ROUGE avant fix (aucune branche ne
 * reconnaissait `Thème` comme opener de bloc, il retombait au defaut). */
static void test_semantic_theme_type_violet_fr(void) {
    printf("\n[TEST] fix/1121 : type Th\xc3\xa8me (FR accentue) → tokenType=4 (type, violet)\n");
    const char* src =
        "MonApp est un Th\xc3\xa8me\n"   /* "Thème" UTF-8, accent */
        "\tstyleBtn est un styleBouton = {\n"
        "\t\tFond = RougeClair\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Thème ligne 0 col 14 : tokenType=4 (type). ROUGE avant fix. */
    ASSERT_HAS_TOKEN(result, 0, 14, 4, "Th\xc3\xa8me (type decl FR accentue) → type (violet)");

    json_free(result);
    json_free(params);
}

/* Graphie EN : `<Ident> is a Theme` → type Theme tokenType=4. */
static void test_semantic_theme_type_violet_en(void) {
    printf("\n[TEST] fix/1121 : type Theme (EN) → tokenType=4 (type, violet)\n");
    const char* src =
        "MonApp is a Theme\n"
        "\tstyleBtn est un styleBouton = {\n"
        "\t\tFond = RougeClair\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* Theme ligne 0 col 12 : tokenType=4 (type). */
    ASSERT_HAS_TOKEN(result, 0, 12, 4, "Theme (type decl EN) → type (violet)");

    json_free(result);
    json_free(params);
}

/* Non-regression  : un identifiant "Theme" hors position de type
 * de declaration (ici une globale ordinaire) n'est PAS colore type=4. */
static void test_semantic_theme_type_non_regression(void) {
    printf("\n[TEST] fix/1121 : 'Theme' hors declaration n'est pas type=4\n");
    const char* src =
        "Theme est un entier = 3\n"
        "PROCEDURE Test()\n"
        "    nLocale est un entier = Theme\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    if (has_semantic_token(result, 0, 0, 4)) {
        printf("  [FAIL] 'Theme' (globale, decl) ne doit pas etre type=4\n");
        g_fail++;
    } else {
        printf("  [OK]   'Theme' (globale, decl) non type=4\n");
        g_pass++;
    }

    json_free(result);
    json_free(params);
}

/* Test 0284-T3 : Member access `:nSolde` dans body méthode classe → tokenType=2 (property, vert foncé).
 * Bug observé : `:nSolde` dans body méthode Crédite classifié cyan (variable locale)
 * au lieu de vert foncé (member/property).
 * Cause suspectée :  skip isMemberAccess (TOKEN_COLON) mais n'émet pas
 * tokenType=2 pour le champ classe matché. */
static void test_semantic_colon_member_access_class_method(void) {
    printf("\n[TEST] fix/0284 : `:nSolde` dans body méthode classe → tokenType=2 (property, vert fonce)\n");
    const char* src =
        "cCompte est une Classe\n"
        "PUBLIC\n"
        "    nSolde est un réel = 0.0\n"
        "    sTitulaire est une chaîne = \"\"\n"
        "    PROCEDURE Crédite(rMontant est un réel)\n"
        "        :nSolde = :nSolde + rMontant\n"
        "    FIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* dump_semantic_tokens(result, "0284-T3 member access classe"); */

    /* Premier `:nSolde` ligne 5 col 9 (left-hand side) : tokenType=2 (property, RED attendu). */
    ASSERT_HAS_TOKEN(result, 5, 9, 2, ":nSolde (lhs) → property (vert fonce) [RED attendu]");

    /* Second `:nSolde` ligne 5 col 19 (rhs avant '+') : tokenType=2 (property, RED attendu). */
    ASSERT_HAS_TOKEN(result, 5, 19, 2, ":nSolde (rhs) → property (vert fonce) [RED attendu]");

    /* rMontant ligne 5 col 28 (param local) : tokenType=3 modifier=0 (local, cyan sans mods).
     * Non-régression . */
    ASSERT_HAS_TOKEN_OPT(result, 5, 28, 3, 0, "rMontant (param) → local (cyan mods=0)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — LSP scan workspace types cross-fichier
 * ================================================================ */

/* Resout dynamiquement le chemin absolu vers tests/lsp/fixtures/ depuis
 * le path source de ce fichier (__FILE__). Portable Windows/Linux. */
static void resolve_fixture_path(char* outBuf, size_t bufSize, const char* relPath) {
    char testFilePath[512];
    char testDir[512];

    /* __FILE__ = "tests/lsp/lsp_test_main.c" (relatif ou absolu selon compilateur).
     * On veut "tests/lsp/fixtures/<relPath>". */
    snprintf(testFilePath, sizeof(testFilePath), "%s", __FILE__);

    /* Extraire repertoire parent (tests/lsp/). */
    char* lastSlash = strrchr(testFilePath, '\\');
    if (!lastSlash) lastSlash = strrchr(testFilePath, '/');
    if (lastSlash) {
        *lastSlash = '\0';
        snprintf(testDir, sizeof(testDir), "%s", testFilePath);
    } else {
        snprintf(testDir, sizeof(testDir), ".");
    }

    /* Construire chemin complet vers fixtures/<relPath>. */
    char fixturePath[512];
    snprintf(fixturePath, sizeof(fixturePath), "%s/fixtures/%s", testDir, relPath);

    /* Normaliser via _fullpath (Windows) ou realpath (POSIX). */
#ifdef _WIN32
    _fullpath(outBuf, fixturePath, bufSize);
#else
    realpath(fixturePath, outBuf);
#endif
}

/* Helper : simule un initialize + didOpen avec rootPath workspace.
 * Le LSP doit scanner le disque pour trouver les types user declares
 * dans des fichiers non-ouverts. */
static void setup_workspace_with_rootpath(const char* rootPath, const char* openedUri, const char* openedContent) {
    /* Reset documents (style setup_doc). */
    for (int i = 0; i < lspServer.documentCount; i++) {
        free(lspServer.documents[i].content);
    }
    lspServer.documentCount = 1;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", openedUri);
    lspServer.documents[0].content = strdup(openedContent);
    lspServer.documents[0].contentLen = (int)strlen(openedContent);
    lspServer.documents[0].version = 1;

    /* Set rootPath (normalement vient de uri_to_filepath(rootUri), ici direct). */
    strncpy(lspServer.rootPath, rootPath, sizeof(lspServer.rootPath) - 1);
    lspServer.rootPath[sizeof(lspServer.rootPath) - 1] = '\0';
}

/* Test  : type cross-fichier non ouvert (LE cas central, rouge attendu).
 * Fixture mini-workspace avec 2 fichiers :
 *   - core/cBase.owl : declare type cBase
 *   - models/config.owl : utilise cBase (fichier ouvert dans VSCode)
 * Le LSP doit scanner le workspace disque et trouver cBase, donc proposer
 * ses champs (sNom) dans `config.`. ACTUELLEMENT : scan_user_symbols_force
 * itere SEULEMENT les docs ouverts → cBase inconnu → rouge attendu. */
static void test_cross_file_type_not_opened(void) {
    printf("\n[TEST] fix/0180 : type cross-fichier non ouvert resolu (RED attendu)\n");

    /* Resoudre chemins dynamiquement (portabilite multi-machine). */
    char workspaceRoot[512];
    char consumerPath[512];
    char consumerUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_type_crossfile");
    resolve_fixture_path(consumerPath, sizeof(consumerPath), "workspace_type_crossfile/models/config.owl");

    /* Convertir path absolu → URI file:// (normaliser separateurs pour URI). */
    snprintf(consumerUri, sizeof(consumerUri), "file:///%s", consumerPath);
    for (char* p = consumerUri + 8; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    const char* consumerSrc =
        "// Consommateur cross-file (fix/0180)\n"
        "config est un cBase\n"
        "config.\n";

    setup_workspace_with_rootpath(workspaceRoot, consumerUri, consumerSrc);

    /* Completion sur `config.` (ligne 2, col 7 = juste apres '.'). */
    JsonValue* params = make_completion_params(consumerUri, 2, 7);
    JsonValue* items = handle_completion(params);

    /* RED attendu : sNom (champ de cBase) doit etre PRESENT une fois le
     * scan disque implemente. ACTUELLEMENT ABSENT car cBase non scanne. */
    ASSERT_HAS(items, "sNom", "cross-file type : champ cBase.sNom propose [RED]");

    json_free(items);
    json_free(params);
}

/* Test  cas 2 : priorite doc ouvert > doc disque (GUARD vert).
 * Meme type declare en 2 versions (disque = casse A, buffer ouvert = casse B).
 * Le LSP doit utiliser la version buffer ouvert (B). Pour tester : on met
 * un champ DIFFERENT dans la version disque vs buffer.
 *
 * GUARD : ce test est VERT MAINTENANT (le LSP ne scanne que les docs ouverts,
 * donc voit sAutre) ET doit RESTER VERT APRES impl (scan disque actif mais
 * priorite buffer>disque correctement codee). Sa valeur : detecter une
 * regression si l'impl introduisait le scan disque SANS priorite buffer. */
static void test_cross_file_priority_opened_over_disk(void) {
    printf("\n[TEST] fix/0180 : priorite doc ouvert > doc disque (GUARD vert)\n");

    /* Fixture : le fichier disque core/cBase.owl existe avec champ sNom.
     * On simule qu'on a OUVERT ce meme fichier mais avec un buffer modifie
     * (champ sAutre au lieu de sNom). Le LSP doit proposer sAutre, pas sNom. */
    char workspaceRoot[512];
    char cBasePath[512];
    char cBaseUri[600];
    char consumerPath[512];
    char consumerUri[600];

    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_type_crossfile");
    resolve_fixture_path(cBasePath, sizeof(cBasePath), "workspace_type_crossfile/core/cBase.owl");
    resolve_fixture_path(consumerPath, sizeof(consumerPath), "workspace_type_crossfile/models/config.owl");

    /* Convertir paths absolus → URIs file://. */
    snprintf(cBaseUri, sizeof(cBaseUri), "file:///%s", cBasePath);
    snprintf(consumerUri, sizeof(consumerUri), "file:///%s", consumerPath);
    for (char* p = cBaseUri + 8; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = consumerUri + 8; *p; p++) if (*p == '\\') *p = '/';

    /* Buffer ouvert : version modifiee avec champ sAutre (pas sNom). */
    const char* cBaseSrc =
        "cBase est une Classe\n"
        "PUBLIC\n"
        "\tsAutre est une chaine\n"  /* different du disque (sNom) */
        "FIN\n";
    const char* consumerSrc =
        "config est un cBase\n"
        "config.\n";

    /* Reset documents + set rootPath. */
    for (int i = 0; i < lspServer.documentCount; i++) {
        free(lspServer.documents[i].content);
    }
    /* Doc 0 : cBase (version buffer ouvert, pas celle du disque). */
    lspServer.documentCount = 2;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", cBaseUri);
    lspServer.documents[0].content = strdup(cBaseSrc);
    lspServer.documents[0].contentLen = (int)strlen(cBaseSrc);
    lspServer.documents[0].version = 1;
    /* Doc 1 : consumer. */
    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", consumerUri);
    lspServer.documents[1].content = strdup(consumerSrc);
    lspServer.documents[1].contentLen = (int)strlen(consumerSrc);
    lspServer.documents[1].version = 1;
    /* rootPath. */
    strncpy(lspServer.rootPath, workspaceRoot, sizeof(lspServer.rootPath) - 1);
    lspServer.rootPath[sizeof(lspServer.rootPath) - 1] = '\0';

    /* Completion sur `config.` (ligne 1, col 7 = juste apres '.'). */
    JsonValue* params = make_completion_params(consumerUri, 1, 7);
    JsonValue* items = handle_completion(params);

    /* GUARD vert : le LSP DOIT proposer sAutre (buffer ouvert), PAS sNom (disque).
     * MAINTENANT : vert car le scan n'itere que les docs ouverts (donc voit sAutre).
     * APRES impl : doit rester vert via regle priorite (doc ouvert > disque). */
    ASSERT_HAS(items, "sAutre", "priorite buffer ouvert : champ sAutre present [GUARD]");
    /* sNom (version disque) NE doit PAS apparaitre. */
    ASSERT_NOT_HAS(items, "sNom", "priorite buffer ouvert : champ sNom (disque) absent [GUARD]");

    json_free(items);
    json_free(params);
}

/* Test  cas 3 : fallback sans rootPath (guard, vert maintenant ET apres).
 * Init LSP SANS rootPath. Le LSP doit garder le comportement actuel (docs ouverts
 * only, pas de scan disque tente). Pas de crash. Coherence diagnostics. */
static void test_fallback_without_rootpath(void) {
    printf("\n[TEST] fix/0180 : fallback sans rootPath (guard, vert attendu)\n");

    /* Reset documents + rootPath vide. */
    for (int i = 0; i < lspServer.documentCount; i++) {
        free(lspServer.documents[i].content);
    }
    const char* uri = "file:///c:/test/foo.owl";
    const char* src =
        "cLocal est une Classe\n"
        "PUBLIC\n"
        "\tsChamp est une chaine\n"
        "FIN\n"
        "\n"
        "obj est un cLocal\n"
        "obj.\n";
    lspServer.documentCount = 1;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uri);
    lspServer.documents[0].content = strdup(src);
    lspServer.documents[0].contentLen = (int)strlen(src);
    lspServer.documents[0].version = 1;
    /* rootPath vide. */
    lspServer.rootPath[0] = '\0';

    /* Completion sur `obj.` (ligne 6, col 4 = juste apres '.'). */
    JsonValue* params = make_completion_params(uri, 6, 4);
    JsonValue* items = handle_completion(params);

    /* VERT attendu : le type cLocal est dans le doc ouvert, donc le LSP le trouve.
     * Pas de crash meme sans rootPath. */
    ASSERT_HAS(items, "sChamp", "sans rootPath : type local resolu");

    json_free(items);
    json_free(params);
}

/*  — un fichier renomme en .owlNOT (pour l'exclure du projet) mais
 * reste ouvert dans un onglet ne doit PAS injecter ses symboles dans la
 * completion cross-documents. On monte 3 buffers ouverts : foo.owl (spar_Owl),
 * bar.owlNOT (spar_Not), baz.owl (spar_Baz). La completion dans foo.owl ne doit
 * proposer QUE les vars des documents .owl (spar_Owl, spar_Baz), pas spar_Not. */
static void test_completion_filters_non_owl_buffers(void) {
    printf("\n[TEST] fix/1835 : buffer .owlNOT ouvert exclu de la completion cross-docs\n");

    const char* fooUri = "file:///c:/test/foo.owl";
    const char* notUri = "file:///c:/test/bar.owlNOT";
    const char* bazUri = "file:///c:/test/baz.owl";

    const char* fooSrc =
        "spar_Owl est une chaine\n"
        "s est une chaine = \n";   /* curseur RHS = pool general d'identifiants */
    const char* notSrc = "spar_Not est une chaine\n";
    const char* bazSrc = "spar_Baz est une chaine\n";

    for (int i = 0; i < lspServer.documentCount; i++) {
        free(lspServer.documents[i].content);
    }
    lspServer.documentCount = 3;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", fooUri);
    lspServer.documents[0].content = strdup(fooSrc);
    lspServer.documents[0].contentLen = (int)strlen(fooSrc);
    lspServer.documents[0].version = 1;
    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", notUri);
    lspServer.documents[1].content = strdup(notSrc);
    lspServer.documents[1].contentLen = (int)strlen(notSrc);
    lspServer.documents[1].version = 1;
    snprintf(lspServer.documents[2].uri, sizeof(lspServer.documents[2].uri), "%s", bazUri);
    lspServer.documents[2].content = strdup(bazSrc);
    lspServer.documents[2].contentLen = (int)strlen(bazSrc);
    lspServer.documents[2].version = 1;
    lspServer.rootPath[0] = '\0';  /* purement buffer, pas de scan disque */

    /* Completion sur la RHS de `s est une chaine = ` (ligne 1, col 19). */
    JsonValue* params = make_completion_params(fooUri, 1, 19);
    JsonValue* items = handle_completion(params);

    ASSERT_HAS(items, "spar_Owl",     "1835 : var du .owl courant proposee");
    ASSERT_HAS(items, "spar_Baz",     "1835 : var d'un 2e .owl ouvert proposee (temoin cross-.owl)");
    ASSERT_NOT_HAS(items, "spar_Not", "1835 : var du .owlNOT ouvert ABSENTE");

    json_free(items);
    json_free(params);

    lspServer.documentCount = 1;  /* laisse l'etat propre pour le test suivant */
}

/*  — typeDeclContext doit poser filterText ASCII strippe pour
 * les labels accentues (chaîne, booléen, réel, httpRequête...) afin que
 * VSCode matche aussi sur la forme sans accent. Sinon taper `s est une
 * chai` masque la suggestion `chaîne`. */
static const char* find_filterText_for_label(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (lbl && strcmp(lbl, label) == 0) {
            return json_get_string(item, "filterText");
        }
    }
    return NULL;
}

#define ASSERT_FILTERTEXT(items, label, expected, test_name) do { \
    const char* got = find_filterText_for_label(items, label); \
    if (got && strcmp(got, expected) == 0) { \
        g_pass++; \
        printf("  [OK]   %s : filterText('%s') = '%s'\n", test_name, label, got); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : filterText('%s') = '%s' attendu '%s'\n", \
               test_name, label, got ? got : "(null)", expected); \
    } \
} while (0)

#define ASSERT_NO_FILTERTEXT(items, label, test_name) do { \
    const char* got = find_filterText_for_label(items, label); \
    if (!got) { \
        g_pass++; \
        printf("  [OK]   %s : pas de filterText sur '%s' (label deja ASCII)\n", \
               test_name, label); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : filterText('%s') = '%s' alors qu'attendu absent\n", \
               test_name, label, got); \
    } \
} while (0)

/*  — la forme EN d'un type compose (Font = EN de Police) doit etre
 * proposee/matchee a la completion quel que soit le mode. Symptome support :
 * taper `Font` ne suggere rien alors que `Pol` -> Police marche, meme en EN.
 * On verifie que le filterText de l'item type compose porte la forme EN pour
 * matcher le prefixe `Font` en mode FR (label=Police) — contexte declaration
 * de type (`workfont est un <cursor>`) ET pool general de types (Loop 2). */
static void test_completion_composed_type_en_font_filtertext(void) {
    printf("\n[TEST] fix/1073 : type compose Police porte 'Font' en filterText (FR)\n");

    /* --- Contexte declaration de type : `workfont est un <cursor>`. --- */
    const char* srcDecl = "workfont est un \n";  /* curseur col 16 */
    setup_doc(URI, srcDecl);
    lspServer.language[0] = '\0';  /* FR */
    JsonValue* params = make_completion_params(URI, 0, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Police", "typeDecl FR : Police propose");
    {
        const char* ft = find_filterText_for_label(items, "Police");
        if (ft && strstr(ft, "Font")) {
            g_pass++;
            printf("  [OK]   typeDecl FR : filterText('Police')='%s' contient 'Font'\n", ft);
        } else {
            g_fail++;
            printf("  [FAIL] typeDecl FR : filterText('Police')='%s' ne contient pas 'Font'\n",
                   ft ? ft : "(null)");
        }
    }
    json_free(items);
    json_free(params);

    /* --- Pool general (>300 items) : le cas reel du support. On tape la forme
     *     EN `Font` au top-level ; le type FR-labelise `Police` doit remonter
     *     via son filterText. Le filtre serveur (>300) ne regardait que le
     *     label : sans consultation du filterText, `Police` etait jete avant
     *     que VSCode ne matche `Font`. On verifie ici la surface bout-en-bout,
     *     pas seulement la presence du filterText. --- */
    const char* srcGen = "Font\n";  /* curseur col 4 apres `Font` */
    setup_doc(URI, srcGen);
    lspServer.language[0] = '\0';  /* FR : label = Police */
    params = make_completion_params(URI, 0, 4);
    items = handle_completion(params);
    ASSERT_HAS(items, "Police",
               "pool general : taper 'Font' surface 'Police' (filtre via filterText)");
    {
        const char* ft = find_filterText_for_label(items, "Police");
        if (ft && strstr(ft, "Font")) {
            g_pass++;
            printf("  [OK]   pool general : filterText('Police')='%s' contient 'Font'\n", ft);
        } else {
            g_fail++;
            printf("  [FAIL] pool general : filterText('Police')='%s' ne contient pas 'Font'\n",
                   ft ? ft : "(null)");
        }
    }
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

static void test_typedecl_primitive_chaine_filterText(void) {
    printf("\n[TEST] fix/0292 typeDecl primitive 'chaîne' a filterText='chaine'\n");
    /* `s est une cha` — curseur apres 'cha' = ligne 0, col 13. */
    const char* src = "s est une cha\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "cha\xc3\xaene", "fix/0292 primitive chaine");
    ASSERT_FILTERTEXT(items, "cha\xc3\xaene", "chaine",
                      "fix/0292 primitive chaine");
    json_free(items);
    json_free(params);
}

static void test_typedecl_primitive_booleen_reel_filterText(void) {
    printf("\n[TEST] fix/0292 typeDecl 'booléen' et 'réel' ont filterText ASCII\n");
    const char* src = "b est un \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_FILTERTEXT(items, "bool\xc3\xa9""en",  "booleen",
                      "fix/0292 primitive booleen");
    ASSERT_FILTERTEXT(items, "r\xc3\xa9""el",     "reel",
                      "fix/0292 primitive reel");
    ASSERT_FILTERTEXT(items, "mon\xc3\xa9taire",  "monetaire",
                      "fix/0292 primitive monetaire");
    ASSERT_FILTERTEXT(items, "num\xc3\xa9rique",  "numerique",
                      "fix/0292 primitive numerique");
    ASSERT_FILTERTEXT(items, "caract\xc3\xa8re",  "caractere",
                      "fix/0292 primitive caractere");
    ASSERT_FILTERTEXT(items, "dur\xc3\xa9""e",    "duree",
                      "fix/0292 primitive duree");
    json_free(items);
    json_free(params);
}

/*  — les sous-blocs d'Etat au canon FR accentue (EnTêteDocument,
 * Itération) doivent porter le meme filterText ASCII que les primitives
 * ci-dessus : une saisie sans accent doit continuer a surfacer l'item.
 * Chemin d'emission distinct (boucle CONTROL_KINDS, pas la table primitives),
 * d'ou le verrou dedie. */
static void test_etat_subblock_accent_filterText(void) {
    printf("\n[TEST] fix/1349 sous-blocs Etat accentues : filterText ASCII\n");
    const char* src =
        "ETAT_X est un Etat\n"
        "\tINTRO est un \n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_FILTERTEXT(items, "EnT\xc3\xaateDocument", "EnTeteDocument",
                      "fix/1349 sous-bloc EnT\xc3\xaateDocument");
    ASSERT_FILTERTEXT(items, "It\xc3\xa9ration", "Iteration",
                      "fix/1349 sous-bloc It\xc3\xa9ration");
    ASSERT_NO_FILTERTEXT(items, "Corps", "fix/1349 sous-bloc Corps (ASCII)");
    json_free(items);
    json_free(params);
}

static void test_typedecl_primitive_ascii_no_filterText(void) {
    printf("\n[TEST] fix/0292 typeDecl 'entier'/'date' (ASCII) — pas de filterText\n");
    const char* src = "e est un \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "entier", "fix/0292 primitive entier present");
    ASSERT_NO_FILTERTEXT(items, "entier", "fix/0292 primitive entier");
    ASSERT_NO_FILTERTEXT(items, "date",   "fix/0292 primitive date");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — semantic tokens `icones.<id>` → enumMember (jaune)
 *
 *  Le `icones.<id>` est canonique pour referencer un asset Iconify
 *  (catalog ). Le `<id>` doit etre annote enumMember (jaune
 *  italique #B1C33A) comme les autres constantes, pas neutre.
 *  Le receveur `icones` reste neutre (pas de token emis).
 * ================================================================ */

/* Cas 1 : `i est une Icone = icones.home` — `home` est enumMember(0). */
static void test_semantic_icones_dot_id_simple(void) {
    printf("\n[TEST] feat/0376 'icones.home' → home = enumMember(0) jaune\n");
    const char* src = "i est une Icone = icones.home\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'icones.home' commence col 18. '.' col 24. 'home' col 25. */
    ASSERT_HAS_TOKEN(result, 0, 25, 0,
        "feat/0376 home → enumMember (jaune)");

    json_free(result);
    json_free(params);
}

/* Cas 2 : `icones.microsoft_icon` — l'id long avec underscores doit aussi
 * etre enumMember. Verifie en plus que le receveur 'icones' n'a pas de
 * token enumMember (il reste neutre). */
static void test_semantic_icones_dot_id_underscore(void) {
    printf("\n[TEST] feat/0376 'icones.microsoft_icon' → id enumMember, 'icones' neutre\n");
    const char* src = "i est une Icone = icones.microsoft_icon\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'icones' commence col 18, '.' col 24, 'microsoft_icon' col 25. */
    ASSERT_HAS_TOKEN(result, 0, 25, 0,
        "feat/0376 microsoft_icon → enumMember (jaune)");
    /* Receveur 'icones' (col 18) NE doit PAS etre annote enumMember. */
    if (!has_semantic_token_opt(result, 0, 18, 0, 0)) {
        g_pass++;
        printf("  [OK]   feat/0376 'icones' (receveur) reste neutre (pas enumMember)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0376 'icones' (receveur) annote enumMember — devrait rester neutre\n");
    }

    json_free(result);
    json_free(params);
}

/* Cas 3 : variante accentuee `Icônes.home` — accent-/casse-insensible sur
 * le receveur, l'id reste enumMember. */
static void test_semantic_icones_dot_id_accent_case(void) {
    printf("\n[TEST] feat/0376 'Ic\xc3\xb4nes.Save' → Save = enumMember (accent+casse)\n");
    /* "Icônes" = 49 63 c3 b4 6e 65 73 — 7 bytes, 6 codepoints UTF-16.
     * Cursor UTF-16 col donc startChar du 'S' = 7 (apres 'Icones'(6) + '.'). */
    const char* src = "i est une Icone = Ic\xc3\xb4nes.Save\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'i est une Icone = ' = 18 chars + 'Icônes' = 6 codepoints + '.' = 1.
     * Total UTF-16 startChar du 'S' = 18 + 6 + 1 = 25. */
    ASSERT_HAS_TOKEN(result, 0, 25, 0,
        "feat/0376 Save apres Ic\xc3\xb4nes → enumMember (jaune)");

    json_free(result);
    json_free(params);
}

/* Non-regression : `obj.field` (receveur qui n'est PAS icones) ne doit
 * PAS recevoir un token enumMember surcharge. Le member access standard
 * reste a la grammar TextMate / fallback. */
static void test_semantic_non_icones_dot_no_enum_member(void) {
    printf("\n[TEST] feat/0376 non-regression : 'foo.bar' ne devient pas enumMember\n");
    const char* src =
        "foo est un entier = 0\n"
        "x = foo.bar\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'foo.bar' ligne 1, 'bar' col 8. NE doit PAS etre enumMember. */
    if (!has_semantic_token_opt(result, 1, 8, 0, 0)) {
        g_pass++;
        printf("  [OK]   feat/0376 'bar' apres 'foo' non-enumMember (non-regression)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0376 'bar' apres 'foo' annote enumMember — regression\n");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   +  — semantic token sur le RECEVEUR `icones`
 *
 *  Receveur `icones` dans `icones.home` doit emettre un token
 *  type=3 (variable) avec modifier 'namespace' (bit 1, mods=2), pour
 *  rendu blanc via la regle theme `variable.namespace` du package.json.
 *   emettait mods=0 en supposant qu'aucune regle theme donnerait
 *  un defaut neutre — faux : sans regle, le mot retombait sur le defaut
 *  TextMate `source.owl = #4CAF50` (vert).  ancre un modifier
 *  dedie pour cibler une regle theme specifique sans toucher au scope
 *  `variable` simple (qui ne doit pas regresser ailleurs).
 *  Le membre `home` reste enumMember(0).
 * ================================================================ */

/* Cas 1 : `icones.home` — receveur `icones` (col 18, len 6) emet
 * token type=3 (variable) modifier 'namespace' (bit 1 = mods=2). */
static void test_semantic_icones_receiver_variable_simple(void) {
    printf("\n[TEST] fix/0381 'icones.home' → 'icones' = variable(3) mods=2 (namespace)\n");
    const char* src = "i est une Icone = icones.home\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'icones' commence col 18 UTF-16, '.' col 24, 'home' col 25. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 18, 3, 2,
        "fix/0381 icones (receveur) → variable mods=namespace (2)");
    /* Le membre 'home' reste enumMember(0). */
    ASSERT_HAS_TOKEN(result, 0, 25, 0,
        "feat/0379 home (membre) → enumMember preserve");

    json_free(result);
    json_free(params);
}

/* Cas 2 : `Icônes.Save` — accent + casse mixte sur le receveur, doit
 * matcher pareil et emettre variable(3) mods=namespace (2) sur le receveur. */
static void test_semantic_icones_receiver_accent_case(void) {
    printf("\n[TEST] fix/0381 'Ic\xc3\xb4nes.Save' → receveur = variable(3) mods=2 (accent+casse)\n");
    const char* src = "i est une Icone = Ic\xc3\xb4nes.Save\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'Icônes' col 18 UTF-16 (6 codepoints), '.' col 24, 'Save' col 25. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 18, 3, 2,
        "fix/0381 Ic\xc3\xb4nes (receveur, accent) → variable mods=namespace (2)");

    json_free(result);
    json_free(params);
}

/* Cas 3 : non-regression — `foo.bar` (receveur PAS icones) NE doit PAS
 * recevoir un token variable(3) mods=2 (namespace) surcharge sur le
 * receveur du fait de la branche . Le receveur 'foo' (variable
 * user globale ligne 0) DOIT rester variable mods=1 (globale, cyan). */
static void test_semantic_non_icones_receiver_no_neutral_override(void) {
    printf("\n[TEST] fix/0381 non-regression : 'foo.bar' receveur foo reste classifie normalement\n");
    const char* src =
        "foo est un entier = 0\n"
        "x est un entier = foo.bar\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'foo' ligne 1 col 18 (apres "x est un entier = " = 18). Ce 'foo'
     * est une globale → variable mods=1 (cyan), pas mods=2. La branche
     *  ne doit PAS l'ecraser en mods=namespace. */
    if (!has_semantic_token_opt(result, 1, 18, 3, 2)) {
        g_pass++;
        printf("  [OK]   fix/0381 'foo' receveur non-icones ne devient pas variable mods=namespace\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0381 'foo' receveur non-icones annote variable mods=namespace — regression\n");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — coloration du namespace `theme.` (calque `icones.`)
 *
 *  `theme.<style>` reference un style nomme declare dans un bloc Theme
 *  (fichier designe par Projet..Theme). Coloration identique a `icones.` :
 *    - receveur `theme` (accent/casse-insensible) → variable(3) mods=2
 *      (namespace, rendu blanc via la regle theme variable.namespace)
 *    - membre `<style>` apres le point → enumMember(0) jaune, comme les id
 *      Iconify de `icones.<id>`.
 *  Le dev a tranche : coherence STRICTE avec `icones`.
 * ================================================================ */

/* Cas 1 : `s est un Style = theme.boutonPrimaire` — le membre `boutonPrimaire`
 * est enumMember(0), le receveur `theme` est variable(3) mods=2. */
static void test_semantic_theme_dot_style_simple(void) {
    printf("\n[TEST] feat/0982 'theme.boutonPrimaire' → membre enumMember(0), receveur variable(3) mods=2\n");
    const char* src = "s est un Style = theme.boutonPrimaire\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 's est un Style = ' = 17 chars. 'theme' col 17, '.' col 22,
     * 'boutonPrimaire' col 23. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 17, 3, 2,
        "feat/0982 theme (receveur) → variable mods=namespace (2)");
    ASSERT_HAS_TOKEN(result, 0, 23, 0,
        "feat/0982 boutonPrimaire (membre) → enumMember (jaune)");

    json_free(result);
    json_free(params);
}

/* Cas 2 : variante accentuee + casse `Thème.Save` — accent/casse-insensible
 * sur le receveur, le membre reste enumMember. */
static void test_semantic_theme_dot_style_accent_case(void) {
    printf("\n[TEST] feat/0982 'Th\xc3\xa8me.Save' → receveur variable(3) mods=2, membre enumMember(0) (accent+casse)\n");
    /* "Thème" = 54 68 c3 a8 6d 65 — 6 bytes, 5 codepoints UTF-16. */
    const char* src = "s est un Style = Th\xc3\xa8me.Save\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 's est un Style = ' = 17 codepoints, 'Thème' = 5 codepoints, '.' = 1.
     * Receveur col 17, membre 'Save' col 17 + 5 + 1 = 23. */
    ASSERT_HAS_TOKEN_OPT(result, 0, 17, 3, 2,
        "feat/0982 Th\xc3\xa8me (receveur, accent) → variable mods=namespace (2)");
    ASSERT_HAS_TOKEN(result, 0, 23, 0,
        "feat/0982 Save apres Th\xc3\xa8me → enumMember (jaune)");

    json_free(result);
    json_free(params);
}

/* Non-regression : `foo.bar` (receveur PAS theme) NE doit PAS recevoir le
 * token variable(3) mods=2 sur le receveur ni enumMember sur le membre. */
static void test_semantic_non_theme_dot_no_override(void) {
    printf("\n[TEST] feat/0982 non-regression : 'foo.bar' (non-theme) pas de surcharge namespace\n");
    const char* src =
        "foo est un entier = 0\n"
        "x est un entier = foo.bar\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* 'foo' ligne 1 col 18 : globale ordinaire → variable mods=1, PAS mods=2. */
    if (!has_semantic_token_opt(result, 1, 18, 3, 2)) {
        g_pass++;
        printf("  [OK]   feat/0982 'foo' receveur non-theme ne devient pas variable mods=namespace\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0982 'foo' receveur non-theme annote variable mods=namespace — regression\n");
    }
    /* 'bar' ligne 1 col 22 : NE doit PAS etre enumMember. */
    if (!has_semantic_token_opt(result, 1, 22, 0, 0)) {
        g_pass++;
        printf("  [OK]   feat/0982 'bar' apres 'foo' non-enumMember (non-regression)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0982 'bar' apres 'foo' annote enumMember — regression\n");
    }

    json_free(result);
    json_free(params);
}

/*  bug 1 : le nom de bloc `ThèmeDémo` (2 chars accentues) ne doit PAS
 * deborder sur le keyword `est` qui suit. La longueur du token doit etre
 * comptee en unites UTF-16 (9), pas en octets UTF-8 (11) — sinon les 2 octets
 * supplementaires colorient ` e` (espace + le `e` de `est`) en cyan. */
static void test_semantic_theme_block_name_length_utf16(void) {
    printf("\n[TEST] fix/1008 nom de bloc accentue 'Th\xc3\xa8meD\xc3\xa9mo' : longueur token = 9 (UTF-16), pas 11 (octets)\n");
    const char* src =
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "\tboutonPrimaire est un styleBouton = {\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    int len = semantic_token_length_at(result, 0, 0);
    if (len == 9) {
        g_pass++;
        printf("  [OK]   fix/1008 'Th\xc3\xa8meD\xc3\xa9mo' longueur=9 (UTF-16, ne deborde pas sur `est`)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1008 'Th\xc3\xa8meD\xc3\xa9mo' longueur=%d (attendu 9 UTF-16)\n", len);
    }

    json_free(result);
    json_free(params);
}

/*  bug 2 : dans le corps d'un bloc `est un Thème`, un type de style
 * `styleBouton` en position de declaration (`X est un styleBouton = {`) doit
 * etre colore TYPE (4, violet), pas property (2, vert). Le `= {` qui suit
 * ferait matcher la branche prop compose sinon. */
static void test_semantic_theme_style_type_violet(void) {
    printf("\n[TEST] fix/1008 'styleBouton' apres `est un` dans un bloc Th\xc3\xa8me -> type(4) violet\n");
    const char* src =
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "\tboutonPrimaire est un styleBouton = {\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* '\tboutonPrimaire est un ' = tab(0) + boutonPrimaire(1..14) + ' '(15)
     * + est(16..18) + ' '(19) + un(20..21) + ' '(22) -> styleBouton col 23. */
    ASSERT_HAS_TOKEN(result, 1, 23, 4,
        "fix/1008 styleBouton -> type(4) violet (pas property vert)");
    /* Non-regression : ne doit PLUS etre property(2). */
    if (!has_semantic_token(result, 1, 23, 2)) {
        g_pass++;
        printf("  [OK]   fix/1008 styleBouton n'est plus property(2)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1008 styleBouton encore property(2) — regression\n");
    }

    json_free(result);
    json_free(params);
}

/*  bug 3 : les noms de propriete d'un bloc PROPRIETES d'un `est un
 * Projet` (Langues, Thème) doivent etre colores property (2, vert fonce), pas
 * laisses a l'identifiant generique (vert clair de la grammar). La couche
 * semantic emet desormais un token property pour chaque nom de prop en tete de
 * ligne suivi de `=` a l'interieur d'un bloc PROPRIETES. */
static void test_semantic_projet_prop_names_property(void) {
    printf("\n[TEST] fix/1008 noms de props PROPRIETES du Projet (Langues, Th\xc3\xa8me) -> property(2)\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFran\xc3\xa7""ais\n"
        "\t\tTh\xc3\xa8me = \"theme.owl\"\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* '\t\tLangues' -> 2 tabs (col 0,1) + Langues col 2. */
    ASSERT_HAS_TOKEN(result, 2, 2, 2,
        "fix/1008 'Langues' (prop Projet) -> property(2) vert fonce");
    /* '\t\tThème' -> 2 tabs + Thème col 2 (ligne 3). */
    ASSERT_HAS_TOKEN(result, 3, 2, 2,
        "fix/1008 'Th\xc3\xa8me' (prop Projet) -> property(2) vert fonce");

    json_free(result);
    json_free(params);
}

/* Verification (Lot 2a) : le NOM de propriete `Thème` dans un bloc PROPRIETES
 * d'un `est un Projet` ne doit PAS etre colore type (4, violet). Le type
 * conteneur `Thème` a ete supprime : sur un build frais il n'y a plus de
 * residu type. Les noms de props d'un bloc PROPRIETES sont colores vert par
 * la grammar TextMate (pas de token semantic emis ici) — la seule chose que
 * la couche semantic doit garantir, c'est de NE PAS surcharger en violet. On
 * verifie donc la parite exacte avec `Langues`, un nom de prop connu-bon :
 * ni l'un ni l'autre n'est type=4 (le violet vu par le plugin 1.0.2 fige
 * venait d'un ancien build, corrige ici sur le build frais). */
static void test_semantic_projet_theme_prop_is_property(void) {
    printf("\n[TEST] feat/0982 'Th\xc3\xa8me' prop dans PROPRIETES du Projet → pas de violet (parite Langues)\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tTh\xc3\xa8me = \"theme_app\"\n"
        "\t\tLangues = langueFran\xc3\xa7""ais\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_semantic_tokens_params(URI);
    JsonValue* result = handle_semantic_tokens(params);

    /* '\t\tThème' = 2 tabs (col 0,1) + 'Thème' col 2. NE doit PAS etre
     * type=4 (violet, ancien type conteneur supprime). */
    if (!has_semantic_token(result, 2, 2, 4)) {
        g_pass++;
        printf("  [OK]   feat/0982 'Th\xc3\xa8me' n'est pas type=4 (pas de residu type conteneur)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0982 'Th\xc3\xa8me' colore type=4 — residu du type conteneur supprime\n");
    }
    /* Parite avec Langues (prop connue-bonne) : ni Thème ni Langues n'est
     * type=4. Si Langues l'etait, ce serait un bug general PROPRIETES, pas
     * specifique a Thème. */
    if (!has_semantic_token(result, 3, 2, 4)) {
        g_pass++;
        printf("  [OK]   feat/0982 'Langues' n'est pas type=4 (parite prop PROPRIETES)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0982 'Langues' colore type=4 — regression PROPRIETES\n");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — reference d'instance de theme dans `Thème = <ident>`
 * ================================================================ */

/* Monte deux documents ouverts (projet + compagnon theme) pour exercer la
 * resolution cross-fichier de la reference `Thème = ThèmeDémo`. */
static void fix1013_setup_project_and_theme(const char* projetUri,
                                            const char* projetSrc,
                                            const char* themeUri,
                                            const char* themeSrc) {
    for (int i = 0; i < lspServer.documentCount; i++) {
        free(lspServer.documents[i].content);
    }
    lspServer.documentCount = 2;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri),
             "%s", projetUri);
    lspServer.documents[0].content = strdup(projetSrc);
    lspServer.documents[0].contentLen = (int)strlen(projetSrc);
    lspServer.documents[0].version = 1;
    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri),
             "%s", themeUri);
    lspServer.documents[1].content = strdup(themeSrc);
    lspServer.documents[1].contentLen = (int)strlen(themeSrc);
    lspServer.documents[1].version = 1;
    /* Pas de rootPath : la resolution passe par les docs ouverts (le compagnon
     * theme est ouvert), pas par un walk disque d'un projet inexistant. */
    lspServer.rootPath[0] = '\0';
}

/* (a) Coloration : `ThèmeDémo` en position valeur du champ `Thème` du bloc
 * PROPRIETES d'un Projet doit etre colore comme une variable globale
 * (variable=3, mods=1 global, cyan) — cohérent avec toute autre ref de globale
 * d'instance. Le style est declare `ThèmeDémo est un Thème` dans le compagnon. */
static void test_semantic_theme_ref_variable_global(void) {
    printf("\n[TEST] fix/1013 : 'Th\xc3\xa8meD\xc3\xa9mo' dans `Th\xc3\xa8me = Th\xc3\xa8meD\xc3\xa9mo` -> variable(3) mods=1 (cyan globale)\n");
    const char* projetUri = "file:///c:/test/projet.owl";
    const char* themeUri = "file:///c:/test/theme.owl";
    const char* projetSrc =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tTh\xc3\xa8me = Th\xc3\xa8meD\xc3\xa9mo\n"
        "\tFIN\n"
        "FIN\n";
    const char* themeSrc =
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "\tboutonPrimaire est un styleBouton = {\n"
        "\t}\n"
        "FIN\n";
    fix1013_setup_project_and_theme(projetUri, projetSrc, themeUri, themeSrc);

    JsonValue* params = make_semantic_tokens_params(projetUri);
    JsonValue* result = handle_semantic_tokens(params);

    /* Ligne 2 : "\t\tThème = ThèmeDémo". 2 tabs (col 0,1), "Thème" col 2-6,
     * espace col 7, '=' col 8, espace col 9, "ThèmeDémo" col 10 (9 codepoints
     * UTF-16). */
    ASSERT_HAS_TOKEN_OPT(result, 2, 10, 3, 1,
        "fix/1013 'Th\xc3\xa8meD\xc3\xa9mo' (valeur du champ Th\xc3\xa8me) -> variable globale cyan");

    json_free(result);
    json_free(params);
}

/* (b) F2 / go-to-definition : sur `ThèmeDémo` dans `Thème = ThèmeDémo`
 * (projet.owl) -> Location vers la declaration `ThèmeDémo est un Thème` du
 * compagnon (ligne 0). */
static void test_definition_theme_ref_to_companion(void) {
    printf("\n[TEST] fix/1013 : F2 sur 'Th\xc3\xa8meD\xc3\xa9mo' -> declaration `... est un Th\xc3\xa8me` du compagnon (ligne 0)\n");
    const char* projetUri = "file:///c:/test/projet.owl";
    const char* themeUri = "file:///c:/test/theme.owl";
    const char* projetSrc =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tTh\xc3\xa8me = Th\xc3\xa8meD\xc3\xa9mo\n"
        "\tFIN\n"
        "FIN\n";
    const char* themeSrc =
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "\tboutonPrimaire est un styleBouton = {\n"
        "\t}\n"
        "FIN\n";
    fix1013_setup_project_and_theme(projetUri, projetSrc, themeUri, themeSrc);

    /* Curseur sur `ThèmeDémo`, ligne 2, col 10 (position valeur). */
    JsonValue* params = make_definition_params(projetUri, 2, 10);
    JsonValue* result = handle_definition(params);

    ASSERT_DEF_LINE(result, 0,
        "fix/1013 F2 'Th\xc3\xa8meD\xc3\xa9mo' -> ligne 0 compagnon");
    /* Verifie que l'URI pointe le compagnon, pas projet.owl. */
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, themeUri) == 0) {
            g_pass++;
            printf("  [OK]   fix/1013 Location.uri = compagnon theme.owl\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/1013 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", themeUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] fix/1013 pas de Location (handler renvoie null)\n");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — globales cross-fichier : nav (F2) + coloration d'une
 *  reference d'instance globale (`X est un <Type>`) dont la declaration
 *  vit dans un compagnon FERME (present sur disque, pas didOpen).
 *   couvrait le cas compagnon OUVERT ; ici l'index disque doit
 *  porter name + fileUri + position pour que ca marche fenetre fermee.
 *  Fixture disque workspace_instances_compagnon :
 *   - projet.owl : reference `Thème = ThèmeDémo`
 *   - theme.owl (FERME) : `ThèmeDémo est un Thème` (ligne 1)
 *   - core/config.owl (FERME) : classe cConfig + `configGlobale est un
 *     cConfig` (ligne 6)
 *   - views/consumer.owl : reference `configGlobale.sNom` (ligne 2)
 * ================================================================ */

/* Helper : chemin fixture -> URI file:///... (separateurs normalises). */
static void fix1017_path_to_uri(const char* path, char* outUri, size_t sz) {
    snprintf(outUri, sz, "file:///%s", path);
    for (char* p = outUri + 8; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* (a) F2 sur `ThèmeDémo` (ref dans projet.owl OUVERT) -> declaration
 * `ThèmeDémo est un Thème` du compagnon theme.owl FERME (ligne 1). */
static void test_definition_global_ref_closed_theme(void) {
    printf("\n[TEST] fix/1017 : F2 'Th\xc3\xa8meD\xc3\xa9mo' -> theme.owl FERME (ligne 1)\n");
    char workspaceRoot[512], projetPath[512], themePath[512];
    char projetUri[600], themeUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_instances_compagnon");
    resolve_fixture_path(projetPath, sizeof(projetPath), "workspace_instances_compagnon/projet.owl");
    resolve_fixture_path(themePath, sizeof(themePath), "workspace_instances_compagnon/theme.owl");
    fix1017_path_to_uri(projetPath, projetUri, sizeof(projetUri));
    fix1017_path_to_uri(themePath, themeUri, sizeof(themeUri));

    const char* projetSrc =
        "// Fixture LSP fix/1017 — projet ouvert, reference d'instance de theme.\n"
        "// La declaration `ThemeDemo est un Theme` vit dans theme.owl (compagnon FERME).\n"
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = [langueFran\xc3\xa7" "ais]\n"
        "\t\tTh\xc3\xa8me = Th\xc3\xa8meD\xc3\xa9mo\n"
        "\tFIN\n"
        "FIN\n";
    setup_workspace_with_rootpath(workspaceRoot, projetUri, projetSrc);

    /* Ref `ThèmeDémo` : ligne 5, col 10 (apres 2 tabs + "Thème = "). */
    JsonValue* params = make_definition_params(projetUri, 5, 10);
    JsonValue* result = handle_definition(params);

    ASSERT_DEF_LINE(result, 1,
        "fix/1017 F2 globale theme -> ligne 1 compagnon ferme");
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, themeUri) == 0) {
            g_pass++;
            printf("  [OK]   fix/1017 Location.uri = theme.owl ferme\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/1017 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", themeUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] fix/1017 pas de Location (compagnon ferme non indexe)\n");
    }
    json_free(result);
    json_free(params);
}

/* (b) Coloration : `ThèmeDémo` (ref dans projet.owl OUVERT) -> token
 * variable(3) mods=1 (global cyan) meme compagnon theme.owl FERME. */
static void test_semantic_global_ref_closed_theme(void) {
    printf("\n[TEST] fix/1017 : 'Th\xc3\xa8meD\xc3\xa9mo' -> variable(3) mods=1 (cyan) compagnon FERME\n");
    char workspaceRoot[512], projetPath[512];
    char projetUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_instances_compagnon");
    resolve_fixture_path(projetPath, sizeof(projetPath), "workspace_instances_compagnon/projet.owl");
    fix1017_path_to_uri(projetPath, projetUri, sizeof(projetUri));

    const char* projetSrc =
        "// Fixture LSP fix/1017 — projet ouvert, reference d'instance de theme.\n"
        "// La declaration `ThemeDemo est un Theme` vit dans theme.owl (compagnon FERME).\n"
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = [langueFran\xc3\xa7" "ais]\n"
        "\t\tTh\xc3\xa8me = Th\xc3\xa8meD\xc3\xa9mo\n"
        "\tFIN\n"
        "FIN\n";
    setup_workspace_with_rootpath(workspaceRoot, projetUri, projetSrc);

    JsonValue* params = make_semantic_tokens_params(projetUri);
    JsonValue* result = handle_semantic_tokens(params);

    ASSERT_HAS_TOKEN_OPT(result, 5, 10, 3, 1,
        "fix/1017 globale theme cyan (compagnon ferme)");
    json_free(result);
    json_free(params);
}

/* (c) Generalite : F2 sur `configGlobale` (instance de CLASSE user, ref
 * dans views/consumer.owl OUVERT) -> declaration `configGlobale est un
 * cConfig` de core/config.owl FERME (ligne 6). */
static void test_definition_global_ref_closed_class_instance(void) {
    printf("\n[TEST] fix/1017 : F2 'configGlobale' -> config.owl FERME (ligne 6)\n");
    char workspaceRoot[512], consumerPath[512], configPath[512];
    char consumerUri[600], configUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_instances_compagnon");
    resolve_fixture_path(consumerPath, sizeof(consumerPath),
                         "workspace_instances_compagnon/views/consumer.owl");
    resolve_fixture_path(configPath, sizeof(configPath),
                         "workspace_instances_compagnon/core/config.owl");
    fix1017_path_to_uri(consumerPath, consumerUri, sizeof(consumerUri));
    fix1017_path_to_uri(configPath, configUri, sizeof(configUri));

    const char* consumerSrc =
        "// Fixture LSP fix/1017 — consommateur OUVERT referencant une instance\n"
        "// globale de classe declaree dans core/config.owl (compagnon FERME).\n"
        "configGlobale.sNom = \"x\"\n";
    setup_workspace_with_rootpath(workspaceRoot, consumerUri, consumerSrc);

    /* Ref `configGlobale` : ligne 2, col 0. */
    JsonValue* params = make_definition_params(consumerUri, 2, 0);
    JsonValue* result = handle_definition(params);

    ASSERT_DEF_LINE(result, 6,
        "fix/1017 F2 globale classe -> ligne 6 compagnon ferme");
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, configUri) == 0) {
            g_pass++;
            printf("  [OK]   fix/1017 Location.uri = config.owl ferme\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/1017 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", configUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] fix/1017 pas de Location (compagnon ferme non indexe)\n");
    }
    json_free(result);
    json_free(params);
}

/* (d) Generalite coloration : `configGlobale` -> token variable(3) mods=1
 * (global cyan) meme compagnon core/config.owl FERME. */
static void test_semantic_global_ref_closed_class_instance(void) {
    printf("\n[TEST] fix/1017 : 'configGlobale' -> variable(3) mods=1 (cyan) compagnon FERME\n");
    char workspaceRoot[512], consumerPath[512];
    char consumerUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_instances_compagnon");
    resolve_fixture_path(consumerPath, sizeof(consumerPath),
                         "workspace_instances_compagnon/views/consumer.owl");
    fix1017_path_to_uri(consumerPath, consumerUri, sizeof(consumerUri));

    const char* consumerSrc =
        "// Fixture LSP fix/1017 — consommateur OUVERT referencant une instance\n"
        "// globale de classe declaree dans core/config.owl (compagnon FERME).\n"
        "configGlobale.sNom = \"x\"\n";
    setup_workspace_with_rootpath(workspaceRoot, consumerUri, consumerSrc);

    JsonValue* params = make_semantic_tokens_params(consumerUri);
    JsonValue* result = handle_semantic_tokens(params);

    ASSERT_HAS_TOKEN_OPT(result, 2, 0, 3, 1,
        "fix/1017 globale classe cyan (compagnon ferme)");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — F2 sur une reference de Fenetre/Etat -> declaration
 *  `X est une Fenetre` / `X est un Etat`, compagnon OUVERT ET FERME.
 *  Suite de /1017 : userWindows[] ne portait que le nom (pour
 *  la coloration) ; l'index windowDefs[] porte desormais name + position
 *  + source (docIndex ouvert / fileUri disque) pour la navigation.
 *  Fixture disque workspace_fenetre_etat_compagnon :
 *   - projet.owl : `FenetreDemarrage = FEN_Demo`
 *   - FEN_Demo.owl (FERME) : `FEN_Demo est une Fenetre` (ligne 0)
 *   - ETAT_Rapport.owl (FERME) : `ETAT_Rapport est un Etat` (ligne 0)
 *   - views/consumer.owl : `Ouvre(FEN_Demo)` / `ImprimeEtat(ETAT_Rapport)`
 * ================================================================ */

/* (a) F2 sur `FEN_Demo` dans `Ouvre(FEN_Demo)` (consumer OUVERT) ->
 * declaration `FEN_Demo est une Fenetre` du compagnon FEN_Demo.owl
 * OUVERT (ligne 0). Verifie aussi que la coloration du nom de fenetre
 * (token class=1) reste inchangee sur la reference. */
static void test_definition_window_ref_open(void) {
    printf("\n[TEST] fix/1018 : F2 'FEN_Demo' (Ouvre) -> compagnon OUVERT (ligne 0)\n");
    const char* consumerUri = "file:///c:/test/consumer.owl";
    const char* fenUri = "file:///c:/test/FEN_Demo.owl";
    const char* consumerSrc =
        "Ouvre(FEN_Demo)\n";
    const char* fenSrc =
        "FEN_Demo est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "FIN\n";
    fix1013_setup_project_and_theme(consumerUri, consumerSrc, fenUri, fenSrc);

    /* Ref `FEN_Demo` : ligne 0, col 6 (apres "Ouvre("). */
    JsonValue* params = make_definition_params(consumerUri, 0, 6);
    JsonValue* result = handle_definition(params);

    ASSERT_DEF_LINE(result, 0,
        "fix/1018 F2 fenetre (Ouvre) -> ligne 0 compagnon ouvert");
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, fenUri) == 0) {
            g_pass++;
            printf("  [OK]   fix/1018 Location.uri = FEN_Demo.owl ouvert\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/1018 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", fenUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] fix/1018 pas de Location (fenetre non indexee)\n");
    }
    json_free(result);
    json_free(params);

    /* Non-regression coloration : `FEN_Demo` (ref) reste token class=1. */
    JsonValue* stParams = make_semantic_tokens_params(consumerUri);
    JsonValue* stResult = handle_semantic_tokens(stParams);
    ASSERT_HAS_TOKEN_OPT(stResult, 0, 6, 1, 0,
        "fix/1018 nom de fenetre reste class (coloration inchangee)");
    json_free(stResult);
    json_free(stParams);
}

/* (b) F2 sur `FEN_Demo` dans `FenetreDemarrage = FEN_Demo` (projet.owl
 * OUVERT) -> declaration du compagnon FEN_Demo.owl FERME (ligne 0). */
static void test_definition_window_ref_closed(void) {
    printf("\n[TEST] fix/1018 : F2 'FEN_Demo' (FenetreDemarrage) -> compagnon FERME (ligne 0)\n");
    char workspaceRoot[512], projetPath[512], fenPath[512];
    char projetUri[600], fenUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_fenetre_etat_compagnon");
    resolve_fixture_path(projetPath, sizeof(projetPath), "workspace_fenetre_etat_compagnon/projet.owl");
    resolve_fixture_path(fenPath, sizeof(fenPath), "workspace_fenetre_etat_compagnon/FEN_Demo.owl");
    fix1017_path_to_uri(projetPath, projetUri, sizeof(projetUri));
    fix1017_path_to_uri(fenPath, fenUri, sizeof(fenUri));

    const char* projetSrc =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tFenetreDemarrage = FEN_Demo\n"
        "\tFIN\n"
        "FIN\n";
    setup_workspace_with_rootpath(workspaceRoot, projetUri, projetSrc);

    /* Ref `FEN_Demo` : ligne 2, col 21 (2 tabs + "FenetreDemarrage = "). */
    JsonValue* params = make_definition_params(projetUri, 2, 21);
    JsonValue* result = handle_definition(params);

    ASSERT_DEF_LINE(result, 0,
        "fix/1018 F2 fenetre (FenetreDemarrage) -> ligne 0 compagnon ferme");
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, fenUri) == 0) {
            g_pass++;
            printf("  [OK]   fix/1018 Location.uri = FEN_Demo.owl ferme\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/1018 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", fenUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] fix/1018 pas de Location (compagnon ferme non indexe)\n");
    }
    json_free(result);
    json_free(params);
}

/* (c) F2 sur `ETAT_Rapport` dans `ImprimeEtat(ETAT_Rapport)` (consumer
 * OUVERT) -> declaration `ETAT_Rapport est un Etat` du compagnon
 * ETAT_Rapport.owl FERME (ligne 0). Meme chemin, type Etat. */
static void test_definition_etat_ref_closed(void) {
    printf("\n[TEST] fix/1018 : F2 'ETAT_Rapport' -> compagnon Etat FERME (ligne 0)\n");
    char workspaceRoot[512], consumerPath[512], etatPath[512];
    char consumerUri[600], etatUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_fenetre_etat_compagnon");
    resolve_fixture_path(consumerPath, sizeof(consumerPath),
                         "workspace_fenetre_etat_compagnon/views/consumer.owl");
    resolve_fixture_path(etatPath, sizeof(etatPath),
                         "workspace_fenetre_etat_compagnon/ETAT_Rapport.owl");
    fix1017_path_to_uri(consumerPath, consumerUri, sizeof(consumerUri));
    fix1017_path_to_uri(etatPath, etatUri, sizeof(etatUri));

    const char* consumerSrc =
        "Ouvre(FEN_Demo)\n"
        "ImprimeEtat(ETAT_Rapport)\n";
    setup_workspace_with_rootpath(workspaceRoot, consumerUri, consumerSrc);

    /* Ref `ETAT_Rapport` : ligne 1, col 12 (apres "ImprimeEtat("). */
    JsonValue* params = make_definition_params(consumerUri, 1, 12);
    JsonValue* result = handle_definition(params);

    ASSERT_DEF_LINE(result, 0,
        "fix/1018 F2 etat -> ligne 0 compagnon ferme");
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, etatUri) == 0) {
            g_pass++;
            printf("  [OK]   fix/1018 Location.uri = ETAT_Rapport.owl ferme\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/1018 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", etatUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] fix/1018 pas de Location (compagnon Etat ferme non indexe)\n");
    }
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — catalogue lifecycle Fenetre (consolidation hardcode)
 * ================================================================ */

/* Autocomplete 'PROCEDURE ' dans body Fenetre propose les 4 lifecycle
 * events FR canon (FinInitialisation, Fermeture, AutorisationFermeture,
 * DemandeMiseAJour). Les alias EN ne sont pas proposes (proposeInCompletion=
 * false), mais reconnus au semantic tokens. */
static void test_lifecycle_fenetre_autocomplete(void) {
    printf("\n[TEST] fix/0501 : 'PROCEDURE ' body Fenetre propose FinInitialisation/Fermeture/AutorisationFermeture/DemandeMiseAJour\n");
    const char* src =
        "FEN_X est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\t\tDisposition = dispLibre\n"
        "\tFIN\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    /* Cursor ligne 5 char 11 = apres 'PROCEDURE '. */
    JsonValue* params = make_completion_params(URI, 5, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "FinInitialisation",       "lifecycle Fenetre FR");
    ASSERT_HAS(items, "Fermeture",               "lifecycle Fenetre FR");
    ASSERT_HAS(items, "AutorisationFermeture",   "lifecycle Fenetre FR");
    ASSERT_HAS(items, "DemandeMiseAJour",        "lifecycle Fenetre FR");
    /* Alias EN non proposes (proposeInCompletion=false), mais reconnus. */
    json_free(items);
    json_free(params);
}

/* AprèsImpression etait dans LSP_LIFECYCLE_EVENTS (widgetKindChar='e'),
 * mais ABSENT du catalogue runtime etat_subcontrol (qui ne contient que
 * AvantImpression). Le refacto le fait disparaitre cote LSP.
 * Status attendu (pre-impl) : ROUGE (encore propose via table statique).
 * Post-impl : VERT (elimination du fantome).
 *
 * Contexte CRITIQUE : sous-controle Etat (Libelle dans Corps), pas Fenetre.
 * Le filtre widgetKindChar='e' (sous-controle Etat) masquait le fantome
 * en contexte Fenetre ('F'), donc le test en contexte Fenetre etait
 * trivialement vert. Le VRAI bug est qu'AprèsImpression est propose en
 * contexte sous-controle Etat ('e') alors qu'il ne devrait pas l'etre. */
static void test_lifecycle_apres_impression_not_proposed(void) {
    printf("\n[TEST] fix/0501 : 'PROCEDURE ' sous-controle Etat propose AvantImpression (legitime) SANS AprèsImpression (fantome)\n");
    /* Source inspiree de tests/runtime/etat_lifecycle_top_level/init_une_fois/
     * etat_init_une_fois.owl. Curseur apres 'PROCEDURE ' dans le body du
     * Libelle (lignes 17-18, apres les PROPRIETES du sous-controle). */
    const char* src =
        "ETAT_Test est un Etat\n"
        "\tPROPRIETES\n"
        "\t\tFormat      = formatA4\n"
        "\t\tOrientation = orientPortrait\n"
        "\tFIN\n"
        "\n"
        "\tcorps est un Corps\n"
        "\t\tPROPRIETES\n"
        "\t\t\tHauteur = 50\n"
        "\t\tFIN\n"
        "\n"
        "\t\tLIB_X est un Libelle\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibelle      = \"test\"\n"
        "\t\t\t\tX            = 10\n"
        "\t\t\t\tY            = 10\n"
        "\t\t\t\tLargeur      = 50\n"
        "\t\t\t\tHauteur      = 15\n"
        "\t\t\t\tPoliceNom    = iPoliceLiberationSans\n"
        "\t\t\t\tPoliceTaille = 11\n"
        "\t\t\tFIN\n"
        "\n"
        "\t\t\tPROCEDURE \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Curseur ligne 22 (0-indexed) col 13 = juste apres 'PROCEDURE '
     * dans le body du Libelle sous-controle Etat (3 tabs + "PROCEDURE ").
     *  Option A : le contexte 'l' (LibelleEtat) est desormais
     * reconnu par isWidgetCtx → la branche autocomplete events route vers
     * owl_etat_subcontrol_event_catalog (AvantImpression seul). */
    JsonValue* params = make_completion_params(URI, 22, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression", "event legitime sous-controle Etat");
    ASSERT_NOT_HAS(items, "Apr\xc3\xa8sImpression", "fantome jamais dans le catalogue");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — affordance editeur de l'event Terminaison d'un bloc
 *  CollectionDeProcedures (completion + hover + coloration).
 * ================================================================ */

/* Autocomplete 'PROCEDURE ' dans un bloc CollectionDeProcedures propose
 * l'event Terminaison (FR canon). L'init implicite (top-level du bloc)
 * n'est pas un event nomme, donc pas propose. */
static void test_collection_terminaison_completion_fr(void) {
    printf("\n[TEST] feat/1171 : 'PROCEDURE ' dans CollectionDeProcedures propose Terminaison\n");
    const char* src =
        "MonCol est une CollectionDeProc\xc3\xa9""dures\n"
        "\tPROCEDURE \n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 1 = "\tPROCEDURE " -> tab(1)+PROCEDURE(9)+espace(1) = col 11. */
    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Terminaison", "feat/1171 collection FR");
    json_free(items);
    json_free(params);
}

/* Meme contexte en mode EN (reglages VSCode anglais) : l'alias EN
 * Termination est propose, avec le fermeur END dans le snippet. */
static void test_collection_terminaison_completion_en(void) {
    printf("\n[TEST] feat/1171 : mode EN -> Termination dans ProcedureCollection\n");
    const char* src =
        "MonCol is a ProcedureCollection\n"
        "\tPROCEDURE \n"
        "END\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    JsonValue* items = handle_completion(make_completion_params(URI, 1, 11));
    lspServer.language[0] = '\0';
    ASSERT_HAS(items, "Termination", "feat/1171 collection EN");
    json_free(items);
}

/* Hover sur Terminaison dans un bloc collection retourne sa fiche event
 * (signature + desc). */
static void test_collection_terminaison_hover(void) {
    printf("\n[TEST] feat/1171 : hover sur Terminaison -> desc event collection\n");
    const char* src =
        "MonCol est une CollectionDeProc\xc3\xa9""dures\n"
        "\tPROCEDURE Terminaison()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Terminaison sur la ligne 1, col 11 -> pointe au milieu du mot (col 13). */
    JsonValue* params = make_hover_params(URI, 1, 13);
    JsonValue* result = handle_hover(params);
    const char* val = hover_value(result);
    ASSERT_HOVER_CONTAINS(val, "Terminaison", "feat/1171 hover titre");
    ASSERT_HOVER_CONTAINS(val, "collection", "feat/1171 hover desc");
    json_free(result);
    json_free(params);
}

/* Garde : hors d'un bloc collection (fichier plat), Terminaison n'est
 * PAS propose comme event et reste une procedure user ordinaire
 * (token method vert), pas un event colore. */
static void test_collection_terminaison_flat_file_not_event(void) {
    printf("\n[TEST] feat/1171 : garde fichier plat -> Terminaison n'est pas un event\n");
    /* (a) Completion apres 'PROCEDURE ' au top-level plat : pas de Terminaison. */
    const char* srcC =
        "PROCEDURE \n"
        "FIN\n";
    setup_doc(URI, srcC);
    JsonValue* items = handle_completion(make_completion_params(URI, 0, 10));
    ASSERT_NOT_HAS(items, "Terminaison", "feat/1171 garde completion plate");
    json_free(items);

    /* (b) Coloration : au top-level plat, 'PROCEDURE Terminaison()' garde
     *     son token method (vert, type 5) — c'est une proc user normale. */
    const char* srcS =
        "PROCEDURE Terminaison()\n"
        "FIN\n";
    setup_doc(URI, srcS);
    JsonValue* semp = make_semantic_tokens_params(URI);
    JsonValue* sem = handle_semantic_tokens(semp);
    /* "PROCEDURE "(10) -> Terminaison a la col 10, ligne 0. */
    if (has_semantic_token(sem, 0, 10, 5)) {
        g_pass++;
        printf("  [OK]   feat/1171 : Terminaison plate = method (vert)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/1171 : Terminaison plate devrait etre method token\n");
    }
    json_free(sem);
    json_free(semp);
}

/*  — source complet d'un bloc CollectionDeProcedures (utilise en
 * same-file). Les usages sont ajoutes apres le bloc par chaque test. */
#define COLL_1178_BLOCK \
    "COL_Utils est une CollectionDeProc\xc3\xa9""dures\n" /* 0 */ \
    "\tgVersion est une cha\xc3\xaene = \"1.0\"\n"        /* 1 */ \
    "\tgMode est un entier = 1\n"                          /* 2 */ \
    "\tStatut est une \xc3\x89num\xc3\xa9ration\n"        /* 3 */ \
    "\t\tactif   = 10\n"                                   /* 4 */ \
    "\t\tinactif = 20\n"                                   /* 5 */ \
    "\tFIN\n"                                              /* 6 */ \
    "\tPROCEDURE Doubler(n est un entier)\n"              /* 7 */ \
    "\t\tRENVOYER n * 2\n"                                 /* 8 */ \
    "\tFIN\n"                                              /* 9 */ \
    "FIN\n"                                                /* 10 */

/* Facet 1 : le NOM du bloc a la declaration se colore class (type 1, vert
 * fonce) comme un nom de Fenetre/Etat, pas variable globale (cyan). */
static void test_collection_1178_decl_class(void) {
    printf("\n[TEST] fix/1178 : declaration COL_Utils -> class (type 1)\n");
    setup_doc(URI, COLL_1178_BLOCK);
    JsonValue* p = make_semantic_tokens_params(URI);
    JsonValue* r = handle_semantic_tokens(p);
    /* COL_Utils ligne 0 col 0 -> class (1). */
    ASSERT_HAS_TOKEN(r, 0, 0, 1, "COL decl -> class (vert fonce)");
    json_free(r); json_free(p);
}

/* Facet 2 : acces qualifie COL_Utils.gMode -> receveur class (1),
 * membre global cyan (variable type 3, mods 1). */
static void test_collection_1178_qualified_member(void) {
    printf("\n[TEST] fix/1178 : COL_Utils.gMode -> receveur class + membre cyan\n");
    const char* src = COLL_1178_BLOCK
        "y est un entier = COL_Utils.gMode\n";  /* 11 : COL_Utils col18, gMode col28 */
    setup_doc(URI, src);
    JsonValue* p = make_semantic_tokens_params(URI);
    JsonValue* r = handle_semantic_tokens(p);
    ASSERT_HAS_TOKEN(r, 11, 18, 1, "receveur COL_Utils -> class");
    ASSERT_HAS_TOKEN_OPT(r, 11, 28, 3, 1, "membre gMode -> variable globale (cyan)");
    json_free(r); json_free(p);
}

/* Facet 3 (same-file, guard) : appel nu Doubler(...) -> method (type 5). */
static void test_collection_1178_proc_call_nu_same_file(void) {
    printf("\n[TEST] fix/1178 : Doubler(21) nom nu -> method (type 5) same-file\n");
    const char* src = COLL_1178_BLOCK
        "x est un entier = Doubler(21)\n";  /* 11 : Doubler col18 */
    setup_doc(URI, src);
    JsonValue* p = make_semantic_tokens_params(URI);
    JsonValue* r = handle_semantic_tokens(p);
    ASSERT_HAS_TOKEN(r, 11, 18, 5, "Doubler nu -> method");
    json_free(r); json_free(p);
}

/* Facet 4 (same-file, guard) : COL_Utils.Statut.actif -> actif enumMember
 * (type 0 = jaune italique). */
static void test_collection_1178_enum_member_italic_same_file(void) {
    printf("\n[TEST] fix/1178 : COL_Utils.Statut.actif -> actif enumMember (italique)\n");
    const char* src = COLL_1178_BLOCK
        "z est un entier = COL_Utils.Statut.actif\n"; /* 11 : Statut col28, actif col35 */
    setup_doc(URI, src);
    JsonValue* p = make_semantic_tokens_params(URI);
    JsonValue* r = handle_semantic_tokens(p);
    /* Statut = type enum (violet, 4) ; actif = enumMember (0). */
    ASSERT_HAS_TOKEN(r, 11, 28, 4, "Statut (qualifie par COL) -> type enum");
    ASSERT_HAS_TOKEN(r, 11, 35, 0, "actif -> enumMember (italique)");
    json_free(r); json_free(p);
}

/* Ouvre projet.owl (fixture) avec utils.owl FERME sur disque (le vrai cas
 * VSCode : le compagnon collection n'est pas ouvert). */
static void setup_collection_fixture(const char** outProjUri) {
    static char projUri[600];
    char workspaceRoot[512];
    char projPath[512];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "collection");
    resolve_fixture_path(projPath, sizeof(projPath), "collection/projet.owl");
    snprintf(projUri, sizeof(projUri), "file:///%s", projPath);
    for (char* q = projUri + 8; *q; q++) if (*q == '\\') *q = '/';
    /* Buffer ouvert = contenu disque de projet.owl (lignes ci-dessous). */
    const char* projSrc =
        "DemoCollection est un Projet\n"                    /* 0 */
        "\tx est un entier = Doubler(21)\n"                 /* 1 : Doubler col19 */
        "\ty est un entier = COL_Utils.gMode\n"             /* 2 : COL col19, gMode col29 */
        "\tz est un entier = COL_Utils.Statut.actif\n"      /* 3 : actif col36 */
        "FIN\n";                                             /* 4 */
    setup_workspace_with_rootpath(workspaceRoot, projUri, projSrc);
    *outProjUri = projUri;
}

/* Facets 2,3,4 en CROSS-FILE (compagnon ferme) — le vrai bug VSCode. */
static void test_collection_1178_crossfile_tokens(void) {
    printf("\n[TEST] fix/1178 : tokens cross-file (utils.owl ferme)\n");
    const char* projUri;
    setup_collection_fixture(&projUri);
    JsonValue* p = make_semantic_tokens_params(projUri);
    JsonValue* r = handle_semantic_tokens(p);
    /* Facet 3 : Doubler nu -> method (5). */
    ASSERT_HAS_TOKEN(r, 1, 19, 5, "cross-file Doubler nu -> method");
    /* Facet 2 : COL_Utils receveur -> class (1). */
    ASSERT_HAS_TOKEN(r, 2, 19, 1, "cross-file COL_Utils receveur -> class");
    /* Facet 2 : gMode -> variable globale cyan (3, mods 1). */
    ASSERT_HAS_TOKEN_OPT(r, 2, 29, 3, 1, "cross-file gMode -> variable globale");
    /* Facet 4 : actif -> enumMember (0, italique). */
    ASSERT_HAS_TOKEN(r, 3, 36, 0, "cross-file actif -> enumMember");
    json_free(r); json_free(p);
}

/* Facets 6,7 : go-to-definition cross-file vers utils.owl. */
static void test_collection_1178_crossfile_definition(void) {
    printf("\n[TEST] fix/1178 : go-to-definition cross-file (utils.owl ferme)\n");
    const char* projUri;
    setup_collection_fixture(&projUri);
    /* Facet 6 : COL_Utils -> declaration ligne 0 de utils.owl. */
    JsonValue* d1 = handle_definition(make_definition_params(projUri, 2, 19));
    ASSERT_DEF_LINE(d1, 0, "def COL_Utils -> utils.owl:0");
    json_free(d1);
    /* Facet 7 : gMode -> ligne 2 de utils.owl. */
    JsonValue* d2 = handle_definition(make_definition_params(projUri, 2, 29));
    ASSERT_DEF_LINE(d2, 2, "def gMode -> utils.owl:2");
    json_free(d2);
    /* Facet 6 : Doubler -> ligne 7 de utils.owl. */
    JsonValue* d3 = handle_definition(make_definition_params(projUri, 1, 19));
    ASSERT_DEF_LINE(d3, 7, "def Doubler -> utils.owl:7");
    json_free(d3);
    /* Facet 7 : actif (membre d'enum) -> ligne 4 de utils.owl. */
    JsonValue* d4 = handle_definition(make_definition_params(projUri, 3, 36));
    ASSERT_DEF_LINE(d4, 4, "def actif -> utils.owl:4");
    json_free(d4);
}

/*  — une methode declaree APRES une methode contenant un `SINON SI`
 * disparaissait du scan de classe (coloration bleue + go-to-definition
 * echouant). Cause mesuree : le scan de corps de classe (scan_class_body)
 * comptait le `SI` d'un `SINON SI` comme une nouvelle ouverture de bloc, alors
 * qu'il partage le FIN du SI ouvrant. La boucle de saut de methode ne
 * retombait jamais a profondeur 0 et sur-consommait jusqu'a EOF, avalant le
 * FIN de la classe et les methodes suivantes.
 *
 * Le vrai cas VSCode (echiquier) : classe `cPlateau` dans un compagnon FERME,
 * methode `RecalculeEtat` avec `SINON SI`, puis `Reinitialise` en derniere
 * methode -> perdue. La collision de `Reinitialise` avec la native
 * `MockRéinitialise` etait une coincidence (c'etait la methode la plus tardive),
 * pas la cause. Le harness 1-doc de  ne pouvait PAS exercer ce cas :
 * la classe doit etre scannee depuis le disque (g_projectClasses). */
static void setup_1219_fixture(const char** outProjUri) {
    static char projUri[600];
    char workspaceRoot[512];
    char projPath[512];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "collision");
    resolve_fixture_path(projPath, sizeof(projPath), "collision/projet.owl");
    snprintf(projUri, sizeof(projUri), "file:///%s", projPath);
    for (char* q = projUri + 8; *q; q++) if (*q == '\\') *q = '/';
    /* Buffer ouvert = contenu disque de projet.owl. */
    const char* projSrc =
        "DemoEchecs est un Projet\n"    /* 0 */
        "\toJeu est un cPlateau()\n"    /* 1 */
        "\toJeu.Reinitialise()\n"       /* 2 : Reinitialise col 6 */
        "\toJeu.Joue()\n"               /* 3 : Joue col 6 */
        "FIN\n";                        /* 4 */
    setup_workspace_with_rootpath(workspaceRoot, projUri, projSrc);
    *outProjUri = projUri;
}

/* Coloration : l'appel cross-fichier `oJeu.Reinitialise()` (methode user
 * collisionnant une native) doit recevoir le token method(5), a parite avec
 * `oJeu.Joue()` (methode user non-collisionnante). */
static void test_1219_crossfile_native_collision_tokens(void) {
    printf("\n[TEST] fix/1219 : collision native cross-fichier -> token method(5)\n");
    const char* projUri;
    setup_1219_fixture(&projUri);
    JsonValue* p = make_semantic_tokens_params(projUri);
    JsonValue* r = handle_semantic_tokens(p);
    /* Temoin non-collisionnant : Joue -> method(5). */
    ASSERT_HAS_TOKEN(r, 3, 6, 5, "cross-file Joue (non-natif) -> method(5)");
    /* Cas bug : Reinitialise collisionne une native -> DOIT rester method(5). */
    ASSERT_HAS_TOKEN(r, 2, 6, 5, "cross-file Reinitialise (natif homonyme) -> method(5)");
    json_free(r); json_free(p);
}

/* go-to-definition : F2 sur `oJeu.Reinitialise()` doit sauter a la declaration
 * user dans cPlateau.owl (ligne 2), pas echouer sur la native. Temoin Joue
 * -> ligne 5. */
static void test_1219_crossfile_native_collision_definition(void) {
    printf("\n[TEST] fix/1219 : collision native cross-fichier -> go-to-def user\n");
    const char* projUri;
    setup_1219_fixture(&projUri);
    /* Temoin : Joue (declare AVANT la methode a SINON SI) -> cPlateau.owl:2. */
    JsonValue* dJoue = handle_definition(make_definition_params(projUri, 3, 6));
    ASSERT_DEF_LINE(dJoue, 2, "def Joue -> cPlateau.owl:2");
    json_free(dJoue);
    /* Cas bug : Reinitialise (declare APRES la methode RecalculeEtat qui
     * contient un `SINON SI`) -> cPlateau.owl:14 (pas null). */
    JsonValue* dReinit = handle_definition(make_definition_params(projUri, 2, 6));
    ASSERT_DEF_LINE(dReinit, 14, "def Reinitialise -> cPlateau.owl:14");
    json_free(dReinit);
}

/*  (2e passe) — Bug 1 : le mot-cle de type `Énumération` d'un enum
 * DÉCLARÉ dans un bloc CollectionDeProcedures doit garder la couleur type
 * (violet, tokenType 4), IDENTIQUE a hors collection. Sans le token semantique
 * emis cote LSP, la grammar TextMate recolorait le keyword en orange dans le
 * contexte du bloc. On verifie l'egalite de tokenType entre les deux contextes. */
static void test_collection_1178_enum_keyword_violet_both_contexts(void) {
    printf("\n[TEST] fix/1178 (p2) : mot-cle Énumération violet (type 4) dans/hors collection\n");
    /* Hors collection : `Statut est une Énumération` -> Énumération col 15. */
    setup_doc(URI,
        "Statut est une \xc3\x89num\xc3\xa9ration\n"  /* 0 : Énumération col 15 */
        "\tactif = 10\n"
        "FIN\n");
    JsonValue* p1 = make_semantic_tokens_params(URI);
    JsonValue* r1 = handle_semantic_tokens(p1);
    ASSERT_HAS_TOKEN(r1, 0, 15, 4, "hors collection : Énumération -> type (violet)");
    json_free(r1); json_free(p1);
    /* Dans collection : meme declaration indentee -> Énumération col 16. */
    setup_doc(URI,
        "COL_Utils est une CollectionDeProc\xc3\xa9""dures\n" /* 0 */
        "\tStatut est une \xc3\x89num\xc3\xa9ration\n"        /* 1 : Énumération col 16 */
        "\t\tactif = 10\n"                                     /* 2 */
        "\tFIN\n"                                              /* 3 */
        "FIN\n");                                              /* 4 */
    JsonValue* p2 = make_semantic_tokens_params(URI);
    JsonValue* r2 = handle_semantic_tokens(p2);
    ASSERT_HAS_TOKEN(r2, 1, 16, 4, "dans collection : Énumération -> type (violet), IDENTIQUE");
    json_free(r2); json_free(p2);
}

/*  (2e passe) — Bug 2 : go-to-def d'un membre d'enum QUALIFIÉ par une
 * collection (`COL_Utils.Statut.actif`) doit resoudre vers le membre de CETTE
 * collection, pas vers l'enum homonyme d'une AUTRE collection. Deux collections
 * declarent un enum `Statut` avec un membre `actif` a des lignes distinctes ;
 * le prefixe collection tranche. */
static void test_collection_1178_qualified_enum_member_scoped(void) {
    printf("\n[TEST] fix/1178 (p2) : go-to-def membre enum qualifie scope a la collection\n");
    const char* src =
        "COL_Config est une CollectionDeProc\xc3\xa9""dures\n" /* 0 */
        "\tStatut est une \xc3\x89num\xc3\xa9ration\n"         /* 1 */
        "\t\tactif   = 1\n"                                     /* 2 : COL_Config.actif */
        "\t\tinactif = 2\n"                                     /* 3 */
        "\tFIN\n"                                               /* 4 */
        "FIN\n"                                                 /* 5 */
        "COL_Utils est une CollectionDeProc\xc3\xa9""dures\n"  /* 6 */
        "\tStatut est une \xc3\x89num\xc3\xa9ration\n"         /* 7 */
        "\t\tactif   = 10\n"                                    /* 8 : COL_Utils.actif */
        "\t\tinactif = 20\n"                                    /* 9 */
        "\tFIN\n"                                               /* 10 */
        "FIN\n"                                                 /* 11 */
        "uStatut est un entier = COL_Utils.Statut.actif\n"     /* 12 : actif col 41 */
        "cStatut est un entier = COL_Config.Statut.actif\n";   /* 13 : actif col 42 */
    setup_doc(URI, src);
    /* COL_Utils.Statut.actif -> membre de COL_Utils (ligne 8), PAS COL_Config (2). */
    JsonValue* d1 = handle_definition(make_definition_params(URI, 12, 41));
    ASSERT_DEF_LINE(d1, 8, "COL_Utils.Statut.actif -> ligne 8 (COL_Utils)");
    json_free(d1);
    /* COL_Config.Statut.actif -> membre de COL_Config (ligne 2), PAS COL_Utils (8). */
    JsonValue* d2 = handle_definition(make_definition_params(URI, 13, 42));
    ASSERT_DEF_LINE(d2, 2, "COL_Config.Statut.actif -> ligne 2 (COL_Config)");
    json_free(d2);
}

/* Facet 5 : non-regression coloration hors collection (enum plat + fenetre). */
static void test_collection_1178_no_regression_plain_enum_window(void) {
    printf("\n[TEST] fix/1178 : non-regression enum/fenetre hors collection\n");
    const char* src =
        "FEN_Test est une Fenetre\n"          /* 0 : FEN_Test class */
        "FIN\n"                                /* 1 */
        "Couleur est une Enumeration\n"        /* 2 : Couleur type enum */
        "\trouge = 1\n"                        /* 3 */
        "\tvert = 2\n"                         /* 4 */
        "FIN\n"                                /* 5 */
        "c est un entier = Couleur.rouge\n";   /* 6 : Couleur col18, rouge col26 */
    setup_doc(URI, src);
    JsonValue* p = make_semantic_tokens_params(URI);
    JsonValue* r = handle_semantic_tokens(p);
    ASSERT_HAS_TOKEN(r, 0, 0, 1, "FEN_Test -> class (non-regression)");
    ASSERT_HAS_TOKEN(r, 6, 18, 4, "Couleur usage -> type enum (non-regression)");
    ASSERT_HAS_TOKEN(r, 6, 26, 0, "rouge -> enumMember (non-regression)");
    json_free(r); json_free(p);
}

/* Coloration dans le bloc : 'PROCEDURE Terminaison()' d'une collection ne
 * doit PAS emettre de token method (vert) — la grammar le colore event. */
static void test_collection_terminaison_colored_as_event(void) {
    printf("\n[TEST] feat/1171 : Terminaison dans le bloc n'est pas method (event)\n");
    const char* src =
        "MonCol est une CollectionDeProc\xc3\xa9""dures\n"
        "\tPROCEDURE Terminaison()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* semp = make_semantic_tokens_params(URI);
    JsonValue* sem = handle_semantic_tokens(semp);
    /* Ligne 1 = "\tPROCEDURE Terminaison()" -> Terminaison a la col 11. */
    if (!has_semantic_token(sem, 1, 11, 5)) {
        g_pass++;
        printf("  [OK]   feat/1171 : Terminaison bloc collection sans token method\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/1171 : Terminaison bloc collection colore method (event attendu)\n");
    }
    json_free(sem);
    json_free(semp);
}

/* ================================================================
 *   — completion `icones.<prefix>` (catalog Iconify)
 * ================================================================ */

/* Recupere le champ `kind` d'un item par label. -1 si introuvable. */
static int find_kind_for_label(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return -1;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (!lbl || strcmp(lbl, label) != 0) continue;
        return (int)json_get_int(item, "kind", -1);
    }
    return -1;
}

/* Recupere insertText d'un item par label. NULL si introuvable. */
static const char* find_insertText_for_label(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (!lbl || strcmp(lbl, label) != 0) continue;
        return json_get_string(item, "insertText");
    }
    return NULL;
}

/* Recupere detail d'un item par label. NULL si introuvable. */
static const char* find_detail_for_label(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return NULL;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        JsonValue* item = items->as.arrayVal.items[i];
        const char* lbl = json_get_string(item, "label");
        if (!lbl || strcmp(lbl, label) != 0) continue;
        return json_get_string(item, "detail");
    }
    return NULL;
}

/* Cas 1 : `icones.` (vide) → renvoie un lot d'items (limite a 50),
 * tous CompletionItemKind=EnumMember (20). */
static void test_icones_dot_empty(void) {
    printf("\n[TEST] feat/0375 'icones.' propose items (limite 50, kind=EnumMember)\n");
    const char* src = "i est une Icone = icones.\n";
    setup_doc(URI, src);
    /* Curseur juste apres le '.' = ligne 0, col 25. */
    JsonValue* params = make_completion_params(URI, 0, 25);
    JsonValue* items = handle_completion(params);
    int n = count_items(items);
    if (n > 0 && n <= 50) {
        g_pass++;
        printf("  [OK]   feat/0375 dot-empty : %d items (>0 et <=50)\n", n);
    } else {
        g_fail++;
        printf("  [FAIL] feat/0375 dot-empty : %d items (attendu 1..50)\n", n);
    }
    /* Verifie que le 1er item a kind=20 (EnumMember). */
    JsonValue* unwrapped = unwrap_items(items);
    if (unwrapped && unwrapped->type == JSON_ARRAY && unwrapped->as.arrayVal.count > 0) {
        JsonValue* first = unwrapped->as.arrayVal.items[0];
        int k = (int)json_get_int(first, "kind", -1);
        if (k == 20) {
            g_pass++;
            printf("  [OK]   feat/0375 dot-empty : kind=20 (EnumMember)\n");
        } else {
            g_fail++;
            printf("  [FAIL] feat/0375 dot-empty : kind=%d (attendu 20)\n", k);
        }
    }
    json_free(items);
    json_free(params);
}

/* Cas 2 : `icones.you` → propose youtube (et ses voisins). */
static void test_icones_dot_prefix_youtube(void) {
    printf("\n[TEST] feat/0375 'icones.you' propose 'youtube'\n");
    const char* src = "i est une Icone = icones.you\n";
    setup_doc(URI, src);
    /* Curseur apres 'you' = ligne 0, col 28. */
    JsonValue* params = make_completion_params(URI, 0, 28);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "youtube", "feat/0375 dot-prefix youtube");
    /* Kind doit etre EnumMember=20. */
    int k = find_kind_for_label(items, "youtube");
    if (k == 20) {
        g_pass++;
        printf("  [OK]   feat/0375 'youtube' kind=20\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0375 'youtube' kind=%d (attendu 20)\n", k);
    }
    json_free(items);
    json_free(params);
}

/* Cas 3 : `icones.red_h` (underscore) → l'utilisateur tape la forme
 * OWL-legale avec '_' ; le LSP normalise vers '-' pour lookup
 * catalog. Label = forme canon Iconify ('-'). insertText = forme OWL
 * avec '_'. */
static void test_icones_dot_underscore_normalize(void) {
    printf("\n[TEST] feat/0375 'icones.red_h' propose 'red-heart' avec insertText 'red_heart'\n");
    /* `red-heart` est dans fluent-emoji-flat. */
    const char* src = "i est une Icone = icones.red_h\n";
    setup_doc(URI, src);
    /* Curseur apres 'red_h' = ligne 0, col 30. */
    JsonValue* params = make_completion_params(URI, 0, 30);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "red-heart", "feat/0375 underscore normalize");
    const char* it = find_insertText_for_label(items, "red-heart");
    if (it && strcmp(it, "red_heart") == 0) {
        g_pass++;
        printf("  [OK]   feat/0375 'red-heart' insertText='red_heart'\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0375 'red-heart' insertText='%s' (attendu 'red_heart')\n",
               it ? it : "(null)");
    }
    json_free(items);
    json_free(params);
}

/*  : le champ `detail` doit exposer la categorie OWL
 * (`ui`, `marques`, `emoji`, `jeu`, `fichiers`, `classiques`) et
 * jamais le nom du set Iconify natif (`mdi`, `logos`,
 * `fluent-emoji-flat`, `material-symbols`...). Regle 7. */
static void test_icones_dot_detail_categorie_owl(void) {
    printf("\n[TEST] fix/0378 'icones.<x>' detail = categorie OWL (pas set Iconify)\n");

    /* Cas A : adobe-photoshop (exclusivement dans set logos = marques).
     * NB : `youtube` existe aussi dans `mdi` (categorie `ui`), donc on
     * choisit une icone exclusive au set logos pour un assert non
     * ambigu. Tape `icones.adobe_phot`, label resolu = `adobe-photoshop`. */
    {
        const char* src = "i est une Icone = icones.adobe_phot\n";
        setup_doc(URI, src);
        JsonValue* params = make_completion_params(URI, 0, 35);
        JsonValue* items = handle_completion(params);
        const char* det = find_detail_for_label(items, "adobe-photoshop");
        if (det && strcmp(det, "marques") == 0) {
            g_pass++;
            printf("  [OK]   fix/0378 'adobe-photoshop' detail='marques'\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/0378 'adobe-photoshop' detail='%s' (attendu 'marques')\n",
                   det ? det : "(null)");
        }
        json_free(items);
        json_free(params);
    }

    /* Cas B : red-heart (set fluent-emoji-flat = emoji). Tape `red_h`,
     * label resolu = `red-heart`. */
    {
        const char* src = "i est une Icone = icones.red_h\n";
        setup_doc(URI, src);
        JsonValue* params = make_completion_params(URI, 0, 30);
        JsonValue* items = handle_completion(params);
        const char* det = find_detail_for_label(items, "red-heart");
        if (det && strcmp(det, "emoji") == 0) {
            g_pass++;
            printf("  [OK]   fix/0378 'red-heart' detail='emoji'\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/0378 'red-heart' detail='%s' (attendu 'emoji')\n",
                   det ? det : "(null)");
        }
        json_free(items);
        json_free(params);
    }

    /* Cas C : home (set material-symbols = ui). */
    {
        const char* src = "i est une Icone = icones.home\n";
        setup_doc(URI, src);
        JsonValue* params = make_completion_params(URI, 0, 29);
        JsonValue* items = handle_completion(params);
        const char* det = find_detail_for_label(items, "home");
        if (det && strcmp(det, "ui") == 0) {
            g_pass++;
            printf("  [OK]   fix/0378 'home' detail='ui'\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/0378 'home' detail='%s' (attendu 'ui')\n",
                   det ? det : "(null)");
        }
        json_free(items);
        json_free(params);
    }

    /* Cas D : aucun item ne doit exposer un nom de set Iconify. Iterer
     * tous les items d'un lot complet et verifier que detail n'est
     * jamais un des noms interdits. */
    {
        const char* forbidden[] = {
            "mdi", "logos", "fluent-emoji-flat", "material-symbols",
            "fluent", "vscode-icons", "devicon", "carbon", "game-icons",
            "simple-icons", "line-md", "flat-color-icons"
        };
        const int forbiddenCount = (int)(sizeof(forbidden) / sizeof(forbidden[0]));
        const char* src = "i est une Icone = icones.\n";
        setup_doc(URI, src);
        JsonValue* params = make_completion_params(URI, 0, 25);
        JsonValue* items = handle_completion(params);
        JsonValue* arr = unwrap_items(items);
        int leak = 0;
        if (arr && arr->type == JSON_ARRAY) {
            for (int i = 0; i < arr->as.arrayVal.count; i++) {
                const char* det = json_get_string(arr->as.arrayVal.items[i], "detail");
                if (!det) continue;
                for (int j = 0; j < forbiddenCount; j++) {
                    if (strcmp(det, forbidden[j]) == 0) { leak++; break; }
                }
            }
        }
        if (leak == 0) {
            g_pass++;
            printf("  [OK]   fix/0378 aucun detail ne fuite un nom de set Iconify\n");
        } else {
            g_fail++;
            printf("  [FAIL] fix/0378 %d item(s) exposent un nom de set Iconify\n", leak);
        }
        json_free(items);
        json_free(params);
    }
}

static void test_typedecl_builtin_httprequete_filterText(void) {
    printf("\n[TEST] fix/0292 typeDecl builtin 'httpRequête' a filterText ASCII\n");
    /* `r est une httpr` — curseur apres 'httpr' = ligne 0, col 15. */
    const char* src = "r est une httpr\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 15);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "httpRequ\xc3\xaate", "fix/0292 builtin httpRequete");
    /* filterText doit contenir la forme ASCII strippee (httpRequete). On
     * accepte soit "httpRequete" pur, soit "httpRequête httpRequete" (pattern
     * du site 3240+ qui concatène les deux pour matcher dans les deux sens). */
    const char* ft = find_filterText_for_label(items, "httpRequ\xc3\xaate");
    if (ft && strstr(ft, "httpRequete") != NULL) {
        g_pass++;
        printf("  [OK]   fix/0292 builtin httpRequete : filterText='%s' contient 'httpRequete'\n", ft);
    } else {
        g_fail++;
        printf("  [FAIL] fix/0292 builtin httpRequete : filterText='%s' ne contient pas 'httpRequete'\n",
               ft ? ft : "(null)");
    }
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — completion bilingue en contexte de declaration
 *
 *  Bug : `typeDeclContext` se declenche uniquement sur `est`/`un`/`une`
 *  (FR). Le mot-cle EN `is` et les articles EN `a`/`an` ne sont pas
 *  detectes -> taper `sFile is string` propose des natives generiques
 *  (`StringBuild` kind=3 Function) au lieu de se limiter aux types.
 *
 *  Bug secondaire : la liste de types ne contient que les labels FR
 *  (chaîne, entier...) — jamais les formes EN (string, integer...).
 *  Taper `Str` apres `is` ne matche donc pas `string`.
 *
 *  Tests 1-4 : RED attendu avant impl.
 *  Test 5 (FR non-regression) : VERT attendu (le FR marchait deja).
 * ================================================================ */

/* Cas 1 — declencheur EN `is` : la liste doit proposer des TYPES
 * (kind 25 TypeParameter ou kind 22 Struct), pas des natives.
 * `sFile is ` = 9 chars (curseur col 9 apres l'espace). */
static void test_typedecl_en_is_proposes_types(void) {
    printf("\n[TEST] feat/0732 cas 1 : `sFile is ` propose types (entier/chaine)\n");
    const char* src = "sFile is \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 9);
    JsonValue* items = handle_completion(params);
    /* Au moins `entier` (forme FR) doit etre propose — si le contexte EN est
     * detecte, le emit-types loop tourne et entier apparait (kind 25). */
    ASSERT_HAS(items, "entier",        "feat/0732 is : type entier present");
    ASSERT_HAS(items, "cha\xc3\xaene", "feat/0732 is : type chaine present");
    /* Verifier que le kind de `entier` est 25 (TypeParameter) ou 22 (Struct),
     * pas 3 (Function). */
    int k = find_kind_for_label(items, "entier");
    if (k == 25 || k == 22) {
        g_pass++;
        printf("  [OK]   feat/0732 is : kind entier=%d (type, pas function)\n", k);
    } else {
        g_fail++;
        printf("  [FAIL] feat/0732 is : kind entier=%d (attendu 25 ou 22)\n", k);
    }
    json_free(items);
    json_free(params);
}

/* Cas 2 — non-fuite de native en declaration EN : `StringBuild` (native
 * kind=3 Function) NE doit PAS apparaitre apres `sFile is `.
 * C'est LE coeur du bug repporte — ce test est rouge avant impl. */
static void test_typedecl_en_is_no_native_leak(void) {
    printf("\n[TEST] feat/0732 cas 2 : `sFile is ` n'inclut pas StringBuild (native)\n");
    const char* src = "sFile is \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 9);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "StringBuild", "feat/0732 is : StringBuild absent (native filtree)");
    ASSERT_NOT_HAS(items, "Trace",       "feat/0732 is : Trace absent (native filtree)");
    json_free(items);
    json_free(params);
}

/* Cas 3a — article EN `a` : `sFile is a ` declenche typeDeclContext,
 * propose des types, et l'insertText ne re-prefixe pas l'article.
 * `sFile is a ` = 11 chars (curseur col 11). */
static void test_typedecl_en_is_a_proposes_types(void) {
    printf("\n[TEST] feat/0732 cas 3a : `sFile is a ` propose types\n");
    const char* src = "sFile is a \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",        "feat/0732 is a : type entier present");
    ASSERT_NOT_HAS(items, "StringBuild",   "feat/0732 is a : StringBuild absent");
    json_free(items);
    json_free(params);
}

/* Cas 3b — article EN `an` : `sFile is an ` = 13 chars (curseur col 13). */
static void test_typedecl_en_is_an_proposes_types(void) {
    printf("\n[TEST] feat/0732 cas 3b : `sFile is an ` propose types\n");
    const char* src = "sFile is an \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",        "feat/0732 is an : type entier present");
    ASSERT_NOT_HAS(items, "StringBuild",   "feat/0732 is an : StringBuild absent");
    json_free(items);
    json_free(params);
}

/* Cas 4 —  : `string` proposé UNIQUEMENT en mode EN (segmentation
 * langue). Avant  le contexte type-declaration double-emettait les deux
 * formes quelle que soit la langue ; desormais le label suit lspServer.language.
 * En EN, le label `string` est present (et `chaîne` absent) ; taper `Str`
 * matche donc `string`. `sFile is Str` = 12 chars (curseur col 12). */
static void test_typedecl_en_is_str_prefix_proposes_string(void) {
    printf("\n[TEST] fix/0819 cas 4 : `sFile is Str` propose 'string' en mode EN\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "sFile is Str\n";
    setup_doc(URI, src);
    /* curseur col 12 = apres 'Str' */
    JsonValue* params = make_completion_params(URI, 0, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "string",        "fix/0819 EN Str : label 'string' present");
    ASSERT_NOT_HAS(items, "cha\xc3\xaene", "fix/0819 EN Str : forme FR 'chaîne' absente");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';  /* reset defaut fr */
}

/* Cas 5 — non-regression FR : `sFile est ` et `sFile est un ` doivent
 * toujours proposer des types (comportement existant avant ).
 * Ces tests DOIVENT etre verts meme avant impl. */
static void test_typedecl_fr_est_non_regression(void) {
    printf("\n[TEST] feat/0732 cas 5a : `sFile est ` propose types (non-regression FR)\n");
    const char* src = "sFile est \n";
    setup_doc(URI, src);
    /* `sFile est ` = 10 chars, curseur col 10 */
    JsonValue* params = make_completion_params(URI, 0, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",        "feat/0732 FR non-reg : entier present");
    ASSERT_HAS    (items, "cha\xc3\xaene", "feat/0732 FR non-reg : chaine presente");
    ASSERT_NOT_HAS(items, "StringBuild",   "feat/0732 FR non-reg : StringBuild absent");
    ASSERT_NOT_HAS(items, "Trace",         "feat/0732 FR non-reg : Trace absent");
    json_free(items);
    json_free(params);
}

static void test_typedecl_fr_est_un_non_regression(void) {
    printf("\n[TEST] feat/0732 cas 5b : `sFile est un ` propose types (non-regression FR article)\n");
    const char* src = "sFile est un \n";
    setup_doc(URI, src);
    /* `sFile est un ` = 13 chars, curseur col 13 */
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",        "feat/0732 FR un non-reg : entier present");
    ASSERT_HAS    (items, "cha\xc3\xaene", "feat/0732 FR un non-reg : chaine presente");
    ASSERT_NOT_HAS(items, "StringBuild",   "feat/0732 FR un non-reg : StringBuild absent");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   (support #213) — completion de type en SIGNATURE et pour
 *  une variable LOCALE. `IDENT est/is <curseur>` doit proposer types +
 *  structures, pas le dump de mots-cles. La signature d'une
 *  PROCEDURE/FONCTION etait a tort classee "appel de fonction"
 *  (callArgContext), ce qui coupait le contexte type-declaration.
 * ================================================================ */

/* Cas A (FR) — parametre dans une signature : `PROCEDURE Calc(param est <cursor>)`. */
static void test_typedecl_signature_param_fr(void) {
    printf("\n[TEST] fix/1299 cas A FR : signature `PROCEDURE Calc(param est ` propose types\n");
    const char* src = "PROCEDURE Calc(param est )\n";
    setup_doc(URI, src);
    /* curseur col 25 = juste apres `est ` (avant la parenthese fermante) */
    JsonValue* params = make_completion_params(URI, 0, 25);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",        "fix/1299 sig FR : entier present");
    ASSERT_HAS    (items, "cha\xc3\xaene", "fix/1299 sig FR : chaine presente");
    ASSERT_NOT_HAS(items, "SI",            "fix/1299 sig FR : mot-cle SI absent");
    ASSERT_NOT_HAS(items, "IMPLEMENTE",    "fix/1299 sig FR : mot-cle IMPLEMENTE absent");
    int k = find_kind_for_label(items, "entier");
    if (k == 25 || k == 22) {
        g_pass++;
        printf("  [OK]   fix/1299 sig FR : kind entier=%d (type)\n", k);
    } else {
        g_fail++;
        printf("  [FAIL] fix/1299 sig FR : kind entier=%d (attendu 25 ou 22)\n", k);
    }
    json_free(items);
    json_free(params);
}

/* Cas A (EN) — reproduit le symptome exact du ticket : `is` en signature
 * proposait IF/inherits/implements au lieu des types. */
static void test_typedecl_signature_param_en(void) {
    printf("\n[TEST] fix/1299 cas A EN : signature `PROCEDURE Calc(param is ` -> types, pas IF/inherits\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "PROCEDURE Calc(param is )\n";
    setup_doc(URI, src);
    /* curseur col 24 = juste apres `is ` (avant la parenthese fermante) */
    JsonValue* params = make_completion_params(URI, 0, 24);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "integer",     "fix/1299 sig EN : integer present");
    ASSERT_NOT_HAS(items, "IF",          "fix/1299 sig EN : IF absent (symptome #213)");
    ASSERT_NOT_HAS(items, "inherits",    "fix/1299 sig EN : inherits absent (symptome #213)");
    ASSERT_NOT_HAS(items, "implements",  "fix/1299 sig EN : implements absent (symptome #213)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';  /* reset defaut fr */
}

/* Cas B (FR) — variable locale dans un corps : `resultat est <cursor>`.
 * Mecanisme distinct (pas de callArgContext ici) — garde-fou de non-regression. */
static void test_typedecl_local_var_body_fr(void) {
    printf("\n[TEST] fix/1299 cas B FR : variable locale `resultat est ` en corps propose types\n");
    const char* src = "PROCEDURE Calc()\n\tresultat est \n\tFIN\n";
    setup_doc(URI, src);
    /* ligne 1 = "\tresultat est " ; curseur col 14 = juste apres `est ` */
    JsonValue* params = make_completion_params(URI, 1, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",        "fix/1299 local FR : entier present");
    ASSERT_HAS    (items, "cha\xc3\xaene", "fix/1299 local FR : chaine presente");
    ASSERT_NOT_HAS(items, "SI",            "fix/1299 local FR : mot-cle SI absent");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   (support #215) — Structure/Classe candidats du contexte
 *  type-declaration. `X est une Structure`/`Classe` est une declaration
 *  valide mais ces deux mots-cles n'etaient jamais proposes.
 * ================================================================ */

static void test_typedecl_structure_classe_fr(void) {
    printf("\n[TEST] fix/1301 FR : `X est une ` propose Structure et Classe\n");
    const char* src = "X est une \n";
    setup_doc(URI, src);
    /* `X est une ` = 10 chars, curseur col 10 (apres l'article) */
    JsonValue* params = make_completion_params(URI, 0, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Structure", "fix/1301 FR : Structure proposee");
    ASSERT_HAS(items, "Classe",    "fix/1301 FR : Classe proposee");
    int k = find_kind_for_label(items, "Structure");
    if (k == 22) {
        g_pass++;
        printf("  [OK]   fix/1301 FR : kind Structure=22 (type)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1301 FR : kind Structure=%d (attendu 22)\n", k);
    }
    json_free(items);
    json_free(params);
}

static void test_typedecl_structure_classe_en(void) {
    printf("\n[TEST] fix/1301 EN : `X is a ` propose Structure et Class (pas Classe FR)\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "X is a \n";
    setup_doc(URI, src);
    /* `X is a ` = 7 chars, curseur col 7 (apres l'article) */
    JsonValue* params = make_completion_params(URI, 0, 7);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Structure", "fix/1301 EN : Structure proposee");
    ASSERT_HAS    (items, "Class",     "fix/1301 EN : Class proposee");
    ASSERT_NOT_HAS(items, "Classe",    "fix/1301 EN : forme FR 'Classe' absente en EN");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';  /* reset defaut fr */
}

/* ================================================================
 *   — segmentation langue COMPLETE des mots-cles de type
 * ================================================================
 *
 *  La completion des types primitifs est segmentee par lspServer.language,
 *  dans les DEUX contextes d'emission :
 *    - type-declaration (`x est un <cursor>` / `x is a <cursor>`) ;
 *    - dump general top-level (Ctrl+Espace sur prefixe nu).
 *  Mode EN -> formes EN seules (integer/string/...) ; mode FR -> formes FR
 *  seules (entier/chaîne/...). Les types sans forme EN distincte
 *  (date/buffer/variant/uuid/octet) sont proposes a l'identique dans les deux
 *  langues (fallback FR). Le runtime reste bilingue ; seule la completion filtre.
 * ================================================================ */

/* Type-declaration, mode EN : `sFile is a ` propose les formes EN, PAS les
 * formes FR. Les types sans forme EN distincte restent proposes (fallback). */
static void test_typedecl_en_segments_types(void) {
    printf("\n[TEST] fix/0819 typeDecl EN : `sFile is a ` propose integer/string, pas entier/chaîne\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "sFile is a \n";  /* 11 chars, curseur col 11 */
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 11);
    JsonValue* items = handle_completion(params);
    /* formes EN presentes */
    ASSERT_HAS    (items, "integer",        "fix/0819 EN typeDecl : integer present");
    ASSERT_HAS    (items, "string",         "fix/0819 EN typeDecl : string present");
    ASSERT_HAS    (items, "boolean",        "fix/0819 EN typeDecl : boolean present");
    ASSERT_HAS    (items, "array",          "fix/0819 EN typeDecl : array present");
    /* formes FR canon absentes en mode EN */
    ASSERT_NOT_HAS(items, "entier",         "fix/0819 EN typeDecl : entier absent");
    ASSERT_NOT_HAS(items, "cha\xc3\xaene",  "fix/0819 EN typeDecl : chaîne absente");
    ASSERT_NOT_HAS(items, "bool\xc3\xa9""en", "fix/0819 EN typeDecl : booléen absent");
    /* fallback FR (pas de forme EN distincte) : proposes dans les deux langues */
    ASSERT_HAS    (items, "date",           "fix/0819 EN typeDecl : date (fallback) present");
    ASSERT_HAS    (items, "buffer",         "fix/0819 EN typeDecl : buffer (fallback) present");
    ASSERT_HAS    (items, "uuid",           "fix/0819 EN typeDecl : uuid (fallback) present");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';  /* reset defaut fr */
}

/* Type-declaration, mode FR : `sFile est un ` propose les formes FR canon,
 * PAS les formes EN. Fin de la double-emission. */
static void test_typedecl_fr_segments_types(void) {
    printf("\n[TEST] fix/0819 typeDecl FR : `sFile est un ` propose entier/chaîne, pas integer/string\n");
    lspServer.language[0] = '\0';  /* defaut fr explicite */
    const char* src = "sFile est un \n";  /* 13 chars, curseur col 13 */
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",         "fix/0819 FR typeDecl : entier present");
    ASSERT_HAS    (items, "cha\xc3\xaene",  "fix/0819 FR typeDecl : chaîne presente");
    ASSERT_NOT_HAS(items, "integer",        "fix/0819 FR typeDecl : integer absent");
    ASSERT_NOT_HAS(items, "string",         "fix/0819 FR typeDecl : string absent");
    ASSERT_NOT_HAS(items, "boolean",        "fix/0819 FR typeDecl : boolean absent");
    /* fallback FR proposes en FR aussi (identiques) */
    ASSERT_HAS    (items, "date",           "fix/0819 FR typeDecl : date (fallback) present");
    ASSERT_HAS    (items, "buffer",         "fix/0819 FR typeDecl : buffer (fallback) present");
    json_free(items);
    json_free(params);
}

/* Dump general, mode EN : taper `integ` en ligne nue propose `integer`
 * (coeur du bug support #54 : avant fix, aucun mot-cle de type EN). */
static void test_general_en_integ_proposes_integer(void) {
    printf("\n[TEST] fix/0819 general EN : `integ` propose 'integer'\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "integ\n";  /* prefixe 'integ', curseur col 5 */
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 5);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "integer", "fix/0819 general EN : integer propose sur 'integ'");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Dump general, mode EN : taper `entie` ne propose PAS `entier` (forme FR
 * filtree hors mode FR). */
static void test_general_en_entie_no_entier(void) {
    printf("\n[TEST] fix/0819 general EN : `entie` ne propose PAS 'entier'\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "entie\n";  /* prefixe 'entie', curseur col 5 */
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 5);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "entier", "fix/0819 general EN : entier absent en mode EN");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Dump general, mode FR : taper `entie` propose `entier` (non-regression FR). */
static void test_general_fr_entie_proposes_entier(void) {
    printf("\n[TEST] fix/0819 general FR : `entie` propose 'entier'\n");
    lspServer.language[0] = '\0';  /* defaut fr */
    const char* src = "entie\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 5);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",  "fix/0819 general FR : entier propose sur 'entie'");
    ASSERT_NOT_HAS(items, "integer", "fix/0819 general FR : integer absent en mode FR");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — autocomplete event snippet : detection `()` apres curseur
 * ================================================================
 *
 * Scenario dev casse : `PROCE`+Tab -> snippet pose `PROCEDURE nom()\n\t\nFIN`.
 * L'user efface `nom`, Ctrl+Espace, choisit `surDoubleClic` -> le snippet
 * event re-pose `()...FIN` -> on a `surDoubleClic()\n\t$0\nFIN()\nFIN`
 * (`FIN()` parasite + double `FIN`).
 *
 * Fix : les items events (lifecycle + typés) inspectent le texte apres
 * curseur. Si `(` suit (en sautant espaces/tabs) -> structure deja en
 * place -> insertText = juste le nom canon. Sinon : snippet complet
 * `nom()\n\t$0\nFIN` (workflow "tapé main" preserve).
 */

/* Cas A — events typés widget (surClic sur Libelle), curseur AVANT `()`
 * existant : insertText = `surClic` (nom seul), pas le snippet complet. */
static void test_event_typed_paren_follows_inserts_name_only(void) {
    printf("\n[TEST] fix/0503 : event typé widget avec `()` qui suit -> insertText = nom seul\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tLIB_X est un Libell\xc3\xa9\n"
        "\t\tPROCEDURE ()\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Curseur ligne 2 (0-indexed) col 12 = juste apres `PROCEDURE `
     * (2 tabs + "PROCEDURE " = 2+10 = 12), AVANT le `()`. */
    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    const char* it = find_insertText_for_label(items, "surClic");
    if (it && strcmp(it, "surClic") == 0) {
        g_pass++;
        printf("  [OK]   fix/0503 surClic insertText='surClic' (nom seul)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0503 surClic insertText='%s' (attendu 'surClic' seul)\n",
               it ? it : "(null)");
    }
    json_free(items);
    json_free(params);
}

/* Cas B — events typés widget, curseur "nu" (rien apres) : insertText =
 * snippet complet `surClic()\n\t$0\nFIN` (workflow main preserve). */
static void test_event_typed_no_paren_inserts_full_snippet(void) {
    printf("\n[TEST] fix/0503 : event typé widget sans `()` qui suit -> snippet complet\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tLIB_X est un Libell\xc3\xa9\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Curseur ligne 2 col 12 = juste apres `PROCEDURE ` (rien apres). */
    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    const char* it = find_insertText_for_label(items, "surClic");
    if (it && strstr(it, "surClic(") != NULL && strstr(it, "\nFIN") != NULL) {
        g_pass++;
        printf("  [OK]   fix/0503 surClic insertText snippet complet (contient '(' + 'FIN')\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0503 surClic insertText='%s' (attendu snippet complet)\n",
               it ? it : "(null)");
    }
    json_free(items);
    json_free(params);
}

/* Cas B-bis — bug secondaire : event deja declare dans le widget ne doit
 * pas etre propose, MEME quand `()` suit le curseur (variation du scenario
 * dev : 1ere PROCEDURE surClic() complete, 2e PROCEDURE () en cours). */
static void test_event_already_declared_filtered_when_paren_follows(void) {
    printf("\n[TEST] fix/0503 : event deja declare filtre meme avec `()` qui suit\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tLIB_X est un Libell\xc3\xa9\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tTrace(\"in\")\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE ()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Curseur ligne 5 col 12 = apres `PROCEDURE ` de la 2e procedure. */
    JsonValue* params = make_completion_params(URI, 5, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surClic",       "filtre surClic deja declare");
    ASSERT_HAS    (items, "surDoubleClic", "surDoubleClic dispo (pas declare)");
    json_free(items);
    json_free(params);
}

/* Cas C — lifecycle Fenetre (FinInitialisation), curseur AVANT `()`
 * existant : insertText = nom seul. Vaut la macro PUSH_LIFECYCLE_ITEM. */
static void test_lifecycle_paren_follows_inserts_name_only(void) {
    printf("\n[TEST] fix/0503 : lifecycle Fenetre avec `()` qui suit -> insertText = nom seul\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tPROCEDURE ()\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Curseur ligne 1 col 11 = juste apres `PROCEDURE ` (1 tab + 10 chars). */
    JsonValue* params = make_completion_params(URI, 1, 11);
    JsonValue* items = handle_completion(params);
    const char* it = find_insertText_for_label(items, "FinInitialisation");
    if (it && strcmp(it, "FinInitialisation") == 0) {
        g_pass++;
        printf("  [OK]   fix/0503 FinInitialisation insertText='FinInitialisation' (nom seul)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0503 FinInitialisation insertText='%s' (attendu nom seul)\n",
               it ? it : "(null)");
    }
    json_free(items);
    json_free(params);
}

/*  — `event_already_declared` doit scanner TOUT le bloc widget,
 * pas seulement [block_start, cursor). Cas dev : on tape une nouvelle
 * `PROCEDURE ` AU-DESSUS d'une `PROCEDURE surClic()` deja declaree plus
 * bas dans le meme widget → surClic doit etre filtre. */
static void test_event_already_declared_after_cursor_same_widget(void) {
    printf("\n[TEST] fix/0505 : event declare APRES le curseur (meme widget) -> filtre\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tLIB_X est un Libell\xc3\xa9\n"
        "\t\tPROCEDURE \n"             /* curseur ici (ligne 2 col 12) */
        "\t\tPROCEDURE surClic()\n"    /* declare APRES le curseur */
        "\t\t\tTrace(\"hi\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surClic",       "filtre surClic declare apres le curseur");
    ASSERT_HAS    (items, "surDoubleClic", "surDoubleClic dispo (pas declare)");
    json_free(items);
    json_free(params);
}

/*  — scope strict : un event declare dans BTN_A voisin ne doit PAS
 * empecher la proposition du meme event dans BTN_B (widget courant du
 * curseur). Le filtre reste limite au widget enclosing. */
static void test_event_already_declared_scope_sibling_widget(void) {
    printf("\n[TEST] fix/0505 : event dans widget voisin ne bloque pas dans widget courant\n");
    const char* src =
        "FEN_X est une Fen\xc3\xaatre\n"
        "\tBTN_A est un Bouton\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tTrace(\"A\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tBTN_B est un Bouton\n"
        "\t\tPROCEDURE \n"             /* curseur ici (ligne 7 col 12) */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 7, 12);
    JsonValue* items = handle_completion(params);
    /* surClic dans BTN_A ne doit PAS bloquer surClic propose pour BTN_B. */
    ASSERT_HAS(items, "surClic", "surClic dispo dans BTN_B malgre surClic dans BTN_A");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — autocomplete events champ Etat : block_start coherent
 *
 *  Bug : `find_dsl_block_start` ne gere pas les ctxKind 'l'
 *  (LibelleEtat) ni 'm' (ImageEtat) → renvoie 0 (debut fichier) →
 *  toute la chaine event_already_declared / find_dsl_block_end opere
 *  sur un scope faux (le fichier entier au lieu du sous-controle).
 *  Le test  (test_lifecycle_apres_impression_not_proposed)
 *  passait par chance : aucune `PROCEDURE` voisine pour exposer le
 *  trou de scope.
 *
 *  Les tests ci-dessous EXPOSENT le trou en posant une `PROCEDURE
 *  AvantImpression()` dans un sous-controle voisin (LIB_A) du meme
 *  Etat, et verifient qu'a l'interieur de LIB_B le filtre ne bloque
 *  PAS la proposition (scope = sous-controle courant). Rouges avant
 *  fix (LIB_A "pollue" le scope file-entier).
 * ================================================================ */

/*  — LibelleEtat ('l') : un event declare dans un Libelle
 * voisin ne doit PAS bloquer la meme proposition dans le Libelle
 * courant. Verifie find_dsl_block_start('l') + bornage par
 * find_dsl_block_end. */
static void test_event_scope_sibling_libelle_etat(void) {
    printf("\n[TEST] fix/0512 : event dans LIB_A voisin ne bloque pas dans LIB_B (LibelleEtat)\n");
    const char* src =
        "ETAT_Test est un Etat\n"
        "\tcorps est un Corps\n"
        "\t\tLIB_A est un Libelle\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"A\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tLIB_B est un Libelle\n"
        "\t\t\tPROCEDURE \n"           /* curseur ligne 9 col 13 */
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 9, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression dispo dans LIB_B malgre PROCEDURE dans LIB_A");
    json_free(items);
    json_free(params);
}

/*  — LibelleEtat ('l') : un event deja declare DANS le
 * Libelle courant DOIT etre filtre (scope strict sur le sous-controle).
 * Verifie que find_dsl_block_end borne au FIN du Libelle, pas plus. */
static void test_event_already_declared_libelle_etat(void) {
    printf("\n[TEST] fix/0512 : event deja declare dans LIB courant -> filtre (LibelleEtat)\n");
    const char* src =
        "ETAT_Test est un Etat\n"
        "\tcorps est un Corps\n"
        "\t\tLIB_X est un Libelle\n"
        "\t\t\tPROCEDURE \n"               /* curseur ligne 3 col 13 */
        "\t\t\tPROCEDURE AvantImpression()\n" /* declare APRES le curseur, meme Libelle */
        "\t\t\t\tTrace(\"x\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "AvantImpression",
                   "AvantImpression deja declare dans LIB_X -> filtre");
    json_free(items);
    json_free(params);
}

/*  — ImageEtat ('m') : autocomplete events propose
 * AvantImpression dans une Image d'un Etat. Verifie detect 'm' +
 * branche autocomplete + scope. */
static void test_event_autocomplete_image_etat(void) {
    printf("\n[TEST] fix/0512 : 'PROCEDURE ' dans Image d'Etat propose AvantImpression\n");
    const char* src =
        "ETAT_Test est un Etat\n"
        "\tcorps est un Corps\n"
        "\t\tIMG_X est un Image\n"
        "\t\t\tPROCEDURE \n"           /* curseur ligne 3 col 13 */
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression propose dans IMG_X (Image d'Etat)");
    json_free(items);
    json_free(params);
}

/*  — ImageEtat ('m') : scope sibling. Une PROCEDURE
 * AvantImpression() dans IMG_A ne doit PAS bloquer dans IMG_B. */
static void test_event_scope_sibling_image_etat(void) {
    printf("\n[TEST] fix/0512 : event dans IMG_A voisin ne bloque pas dans IMG_B (ImageEtat)\n");
    const char* src =
        "ETAT_Test est un Etat\n"
        "\tcorps est un Corps\n"
        "\t\tIMG_A est un Image\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"A\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_B est un Image\n"
        "\t\t\tPROCEDURE \n"           /* curseur ligne 9 col 13 */
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 9, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression dispo dans IMG_B malgre PROCEDURE dans IMG_A");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — repro du VRAI fichier examples/etats/image_demo/ETAT_Demo.owl
 *
 *  Le bug expose : `est un Champ Image` (prefixe `Champ` + IDENTIFIER
 *  `Image`) n'est gere ni par `find_dsl_block_start` cas 'm' (volet A)
 *  ni par `find_dsl_block_end` (prev=EST perdu apres `Champ`, volet B).
 *  Resultat : block_start = 0, block_end = src_len → scope du fichier
 *  entier → un AvantImpression declare dans N'IMPORTE quel sous-controle
 *  voisin filtre la proposition dans tous les autres.
 *
 *  Les tests  utilisaient `est un Image` (sans `Champ`) →
 *  passaient par accident. Ici on reproduit la VRAIE syntaxe.
 * ================================================================ */

/* Volet A — `IDENT est un Champ Image` (prefixe Champ requis) :
 * dans un Champ Image SANS event declare, AvantImpression doit etre
 * propose. Repro IMG_LogoEtire d'ETAT_Demo.owl en isolation. */
static void test_event_champ_image_prefix_proposed(void) {
    printf("\n[TEST] fix/0513 : `est un Champ Image` (prefixe Champ) propose AvantImpression\n");
    const char* src =
        "ETAT_Test est un Etat\n"
        "\tcorps est un Corps\n"
        "\t\tIMG_X est un Champ Image\n"
        "\t\t\tPROCEDURE \n"               /* curseur ligne 3 col 13 */
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression propose dans IMG_X (Champ Image, prefixe Champ)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — `PROCEDURE ` dans un Champ Onglet propose les events
 *  du CATALOGUE du genre englobant, pas le catalogue du langage.
 *
 *  Deux defauts cumules avant fix :
 *   (1) `VOL_X est un Volet` (is_widget=false) n'etait pas empile par
 *       detect_dsl_context_ex alors que son FIN depilait -> le Champ
 *       Onglet englobant sautait, le contexte retombait sur la Fenetre
 *       (events Fenetre proposes) puis sur le pool generique.
 *   (2) find_dsl_block_start ne reconnaissait aucun genre sans token
 *       scanner dedie -> block_start=0 pour un Onglet : un event
 *       declare dans un Onglet VOISIN filtrait la proposition.
 * ================================================================ */

/* Cas nominal du ticket : curseur dans un Champ Onglet qui contient
 * deja un sous-bloc Volet. surChangementVolet doit etre propose, et la
 * liste doit rester STRICTEMENT le catalogue du genre (1 item) — pas le
 * catalogue du langage, pas les events de la Fenetre englobante. */
static void test_event_autocomplete_onglet(void) {
    printf("\n[TEST] fix/1378 : 'PROCEDURE ' dans un Champ Onglet propose surChangementVolet\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 5 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "surChangementVolet",
               "surChangementVolet propose dans ONG_M (apres un sous-bloc Volet)");
    ASSERT_NOT_HAS(items, "FinInitialisation",
                   "event Fenetre non propose dans un Champ Onglet");
    ASSERT_NOT_HAS(items, "Trace",
                   "pool generique du langage non propose apres 'PROCEDURE '");
    int n = count_items(items);
    if (n == 1) {
        g_pass++;
        printf("  [OK]   fix/1378 : liste = catalogue Onglet seul (1 item)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1378 : attendu 1 item (catalogue Onglet), obtenu %d\n", n);
    }
    json_free(items);
    json_free(params);
}

/* Le sous-bloc Volet ne doit pas desequilibrer la pile de contexte :
 * apres la FIN du Champ Onglet, le top-level de la Fenetre doit
 * retrouver le catalogue Fenetre (et pas celui de l'Onglet). */
static void test_event_autocomplete_apres_bloc_onglet(void) {
    printf("\n[TEST] fix/1378 : top-level Fenetre apres un bloc Onglet+Volet -> events Fenetre\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tBTN_IN est un Champ Bouton\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE \n"              /* curseur ligne 8 col 11 */
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 8, 11);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "FinInitialisation",
               "FinInitialisation propose au top-level de la Fenetre");
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "event Onglet non propose au top-level de la Fenetre");
    json_free(items);
    json_free(params);
}

/* Bornage du filtre "deja declare" : un surChangementVolet pose dans
 * ONG_A ne doit pas supprimer la proposition dans ONG_B (le scope doit
 * etre le bloc Onglet courant, pas la Fenetre entiere). */
static void test_event_scope_sibling_onglet(void) {
    printf("\n[TEST] fix/1378 : event dans ONG_A voisin ne bloque pas dans ONG_B\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_A est un Champ Onglet\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\t\tTrace(\"a\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tONG_B est un Champ Onglet\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 8 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 8, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "surChangementVolet",
               "surChangementVolet dispo dans ONG_B malgre l'event de ONG_A");
    json_free(items);
    json_free(params);
}

/* Le filtre "deja declare" reste actif DANS le meme Champ Onglet. */
static void test_event_already_declared_onglet(void) {
    printf("\n[TEST] fix/1378 : event deja declare dans l'Onglet courant -> filtre\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\t\tTrace(\"x\")\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 9 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 9, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "surChangementVolet deja declare dans ONG_M -> filtre");
    json_free(items);
    json_free(params);
}

/* Un Champ Bouton pose DANS un Volet garde son propre catalogue
 * (le filtre est pilote par le genre du bloc englobant le plus proche). */
static void test_event_autocomplete_bouton_dans_volet(void) {
    printf("\n[TEST] fix/1378 : 'PROCEDURE ' dans un Bouton imbrique dans un Volet -> events Bouton\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tBTN_OK est un Champ Bouton\n"
        "\t\t\t\tPROCEDURE \n"        /* curseur ligne 4 col 14 */
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 4, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "surClic", "surClic propose dans BTN_OK (imbrique dans un Volet)");
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "event Onglet non propose dans un Bouton enfant");
    json_free(items);
    json_free(params);
}

/* Bilingue : en mode EN, le catalogue Onglet sort sa forme EN
 * (onPaneChange) — y compris sur une source ecrite en EN
 * (`is a Control Onglet` / `is a Pane` / `END`). */
static void test_event_autocomplete_onglet_en(void) {
    printf("\n[TEST] fix/1378 : mode EN -> onPaneChange propose dans un Champ Onglet\n");
    const char* src =
        "FEN_T is a Window\n"
        "\tONG_M is a Control Onglet\n"
        "\t\tVOL_A is a Pane\n"
        "\t\tEND\n"
        "\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 5 col 12 */
        "\tEND\n"
        "END\n";
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "onPaneChange", "forme EN de l'event Onglet proposee en mode EN");
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "forme FR non proposee en mode EN");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *   — bornage du bloc widget quand un sous-controle utilise le
 *  filler `de`/`of` (`SAI_X est un Champ de Saisie`).
 *
 *  Avant fix : find_dsl_block_end traversait le modificateur `Champ` en
 *  preservant prev=EST, mais PAS le `de` qui suit. Le genre `Saisie`
 *  etait donc lu avec wasEst=false -> l'ouverture du sous-controle
 *  n'etait pas comptee alors que sa `FIN` decrementait la profondeur.
 *  Resultat : le bloc englobant etait borne a la FIN du sous-controle,
 *  et le filtre "event deja declare" ne voyait pas les PROCEDURE
 *  situees plus bas dans le meme bloc -> un event deja ecrit etait
 *  repropose. Les tests  ratait le cas : leurs geometries
 *  n'avaient que des `Champ Bouton` / `Volet` (jamais de filler `de`).
 * ================================================================ */

/* Geometrie minimale fautive : un `Champ de Saisie` dans le Volet, la
 * PROCEDURE event declaree APRES le Volet. */
static void test_event_already_declared_onglet_champ_de_saisie(void) {
    printf("\n[TEST] fix/1385 : 'Champ de Saisie' dans un Volet ne tronque plus le bloc Onglet\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_Demo est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tSAI_X est un Champ de Saisie\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 8 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 8, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "event deja declare plus bas dans le meme Onglet -> filtre");
    int n = count_items(items);
    if (n == 0) {
        g_pass++;
        printf("  [OK]   fix/1385 : catalogue Onglet epuise (0 item)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1385 : attendu 0 item, obtenu %d\n", n);
    }
    json_free(items);
    json_free(params);
}

/* Meme defaut sans Volet : le filler `de` seul suffit a casser le bornage. */
static void test_event_already_declared_onglet_saisie_directe(void) {
    printf("\n[TEST] fix/1385 : 'Champ de Saisie' directement dans l'Onglet\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_Demo est un Champ Onglet\n"
        "\t\tSAI_X est un Champ de Saisie\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 6 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "event deja declare -> filtre malgre le filler 'de'");
    json_free(items);
    json_free(params);
}

/* Geometrie complete du fichier de demo : 3 Volets, blocs PROPRIETES,
 * `Champ de Saisie`, Conteneur imbrique, et la PROCEDURE event posee
 * APRES tous les Volets. */
static void test_event_already_declared_onglet_geometrie_demo(void) {
    printf("\n[TEST] fix/1385 : geometrie demo (3 volets + saisie + conteneur, proc en fin de bloc)\n");
    const char* src =
        "FEN_D est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"x\"\n"
        "\tFIN\n"
        "\n"
        "\tONG_Demo est un Champ Onglet\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLargeur = 535\n"
        "\t\tFIN\n"
        "\n"
        "\t\tVOL_Profil est un Volet\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibelle = \"Profil\"\n"
        "\t\t\tFIN\n"
        "\n"
        "\t\t\tSAI_Nom est un Champ de Saisie\n"
        "\t\t\t\tPROPRIETES\n"
        "\t\t\t\t\tX = 20\n"
        "\t\t\t\tFIN\n"
        "\t\t\tFIN\n"
        "\n"
        "\t\t\tBTN_Enregistrer est un Champ Bouton\n"
        "\t\t\t\tPROPRIETES\n"
        "\t\t\t\t\tX = 20\n"
        "\t\t\t\tFIN\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tVOL_Options est un Volet\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibelle = \"Options\"\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tVOL_Avance est un Volet\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibelle = \"Avance\"\n"
        "\t\t\tFIN\n"
        "\n"
        "\t\t\tCNT_Bloc est un Champ Conteneur\n"
        "\t\t\t\tBTN_Reinit est un Champ Bouton\n"
        "\t\t\t\tFIN\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\t\tTrace(\"v\")\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 49 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 49, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "event deja declare en fin de bloc Onglet -> filtre");
    ASSERT_NOT_HAS(items, "Trace",
                   "pool generique du langage non propose apres 'PROCEDURE '");
    int n = count_items(items);
    if (n == 0) {
        g_pass++;
        printf("  [OK]   fix/1385 : geometrie demo -> 0 item (catalogue epuise)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1385 : geometrie demo -> attendu 0 item, obtenu %d\n", n);
    }
    json_free(items);
    json_free(params);
}

/* Non-regression du bornage repare : le scope reste le bloc Onglet
 * courant. Un event pose dans ONG_A ne filtre pas dans ONG_B, meme
 * quand un `Champ de Saisie` traine dans ONG_A. */
static void test_event_scope_sibling_onglet_champ_de_saisie(void) {
    printf("\n[TEST] fix/1385 : scope toujours borne a l'Onglet courant (voisin avec saisie)\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_A est un Champ Onglet\n"
        "\t\tSAI_X est un Champ de Saisie\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tONG_B est un Champ Onglet\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 9 col 12 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 9, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "surChangementVolet",
               "surChangementVolet toujours dispo dans ONG_B");
    json_free(items);
    json_free(params);
}

/* Le sous-controle `Champ de Saisie` garde son propre catalogue d'events
 * (le bornage repare ne doit pas lui voler son contexte). */
static void test_event_autocomplete_champ_de_saisie_dans_volet(void) {
    printf("\n[TEST] fix/1385 : 'PROCEDURE ' dans un 'Champ de Saisie' -> events Saisie\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tSAI_X est un Champ de Saisie\n"
        "\t\t\t\tPROCEDURE \n"        /* curseur ligne 4 col 14 */
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 4, 14);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "surAChaqueModification",
               "catalogue Saisie propose dans SAI_X");
    ASSERT_NOT_HAS(items, "surChangementVolet",
                   "event Onglet non propose dans le Champ de Saisie enfant");
    json_free(items);
    json_free(params);
}

/* Bilingue : le modificateur EN canon `Control` (nameEN de Champ,
 * ) doit borner le bloc comme `Champ`. Avant fix il n'etait
 * reconnu ni par find_dsl_block_start (block_start=0 -> scope = fichier
 * entier) ni par find_dsl_block_end : l'event pose dans ONG_A supprimait
 * la proposition dans ONG_B. */
static void test_event_scope_sibling_onglet_en_control(void) {
    printf("\n[TEST] fix/1385 : modificateur EN 'Control' borne le bloc (voisins ONG_A/ONG_B)\n");
    const char* src =
        "FEN_T is a Window\n"
        "\tONG_A is a Control Onglet\n"
        "\t\tPROCEDURE onPaneChange()\n"
        "\t\tEND\n"
        "\tEND\n"
        "\n"
        "\tONG_B is a Control Onglet\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 7 col 12 */
        "\tEND\n"
        "END\n";
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 7, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "onPaneChange",
               "onPaneChange dispo dans ONG_B malgre l'event de ONG_A");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Miroir EN du cas nominal : filler `of` apres `Control`. */
static void test_event_already_declared_onglet_en_control_of(void) {
    printf("\n[TEST] fix/1385 : filler EN 'of' apres 'Control' -> bornage correct\n");
    const char* src =
        "FEN_T is a Window\n"
        "\tONG_M is a Control Onglet\n"
        "\t\tSAI_X is a Control of Saisie\n"
        "\t\tEND\n"
        "\t\tPROCEDURE onPaneChange()\n"
        "\t\tEND\n"
        "\t\tPROCEDURE \n"            /* curseur ligne 6 col 12 */
        "\tEND\n"
        "END\n";
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "onPaneChange",
                   "event EN deja declare -> filtre malgre le filler 'of'");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *   — garde de coherence INTER-CHEMINS du prefixe de
 *  declaration d'un champ : le triplet de modificateurs
 *  `Champ`/`Field`/`Control` + le filler `de`/`of`.
 *
 *  La regle etait dupliquee dans quatre scans de lsp_handlers.c
 *  (contexte DSL, fenetre englobante, debut de bloc, fin de bloc).
 *  Chaque correctif de la serie 0513 / 0688 / 1290 / 1378 / 1385 n'a
 *  repare qu'une ou deux copies : contexte juste mais bornes fausses,
 *  ou l'inverse. Les tests ci-dessous exercent une MEME declaration
 *  par les trois observables qui traversent les quatre chemins, pour
 *  les quatre formes canon. Une regle reintroduite dans un seul
 *  chemin fait rougir la garde.
 *
 *    - contexte      : props proposees dans le bloc PROPRIETES du
 *                      champ declare (scan de contexte DSL) ;
 *    - bornes bloc   : event deja declare plus bas dans le bloc
 *                      englobant -> filtre (debut + fin de bloc) ;
 *    - fenetre       : globale de la fenetre englobante toujours
 *                      offerte en RHS de prop, globale d'une autre
 *                      fenetre toujours exclue (fenetre englobante).
 * ================================================================ */

typedef struct {
    const char* label;      /* forme testee, pour la sortie console */
    const char* decl;       /* declaration complete, sans indentation */
    const char* propOwn;    /* prop specifique au genre declare */
    const char* propOther;  /* prop specifique a l'AUTRE genre */
} ChampPrefixCase;

static const ChampPrefixCase CHAMP_PREFIX_CASES[] = {
    { "est un Champ de Saisie", "SAI_X est un Champ de Saisie",
      "Indication", "TypeBouton" },
    { "is a Control Saisie",    "SAI_X is a Control Saisie",
      "Indication", "TypeBouton" },
    { "est un Champ Bouton",    "BTN_X est un Champ Bouton",
      "TypeBouton", "Indication" },
    { "is a Field Button",      "BTN_X is a Field Button",
      "TypeBouton", "Indication" },
};
#define CHAMP_PREFIX_CASES_N \
    ((int)(sizeof(CHAMP_PREFIX_CASES) / sizeof(CHAMP_PREFIX_CASES[0])))

/* Chemin 1 — contexte DSL : le genre doit etre empile malgre le
 * modificateur (et le filler) traverse, sinon le bloc PROPRIETES
 * retombe sur la Fenetre englobante. */
static void test_champ_prefix_coherence_contexte(const ChampPrefixCase* c) {
    char src[512], msg[192];
    snprintf(src, sizeof(src),
        "FEN_T est une Fenetre\n"
        "\t%s\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n", c->decl);
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    snprintf(msg, sizeof(msg), "fix/1386 contexte [%s]", c->label);
    ASSERT_HAS    (items, c->propOwn,   msg);
    ASSERT_NOT_HAS(items, c->propOther, msg);
    json_free(items);
    json_free(params);
}

/* Chemin 2 — bornes du bloc englobant : l'ouverture du sous-controle
 * doit etre comptee comme sa FIN, sinon l'Onglet est borne trop tot
 * et l'event deja ecrit plus bas est repropose. */
static void test_champ_prefix_coherence_bornes(const ChampPrefixCase* c) {
    char src[512], msg[192];
    snprintf(src, sizeof(src),
        "FEN_T est une Fenetre\n"
        "\tONG_Demo est un Champ Onglet\n"
        "\t\t%s\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surChangementVolet()\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE \n"
        "\tFIN\n"
        "FIN\n", c->decl);
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 12);
    JsonValue* items = handle_completion(params);
    snprintf(msg, sizeof(msg), "fix/1386 bornes [%s]", c->label);
    ASSERT_NOT_HAS(items, "surChangementVolet", msg);
    json_free(items);
    json_free(params);
}

/* Chemin 3 — fenetre englobante : meme desequilibre ouverture/FIN, mais
 * observe par le scope des globales offertes en RHS de prop. Si la
 * declaration n'est pas empilee, sa FIN depile la Fenetre : le curseur
 * se retrouve hors de toute fenetre. */
static void test_champ_prefix_coherence_fenetre(const ChampPrefixCase* c) {
    char src[768], msg[192];
    snprintf(src, sizeof(src),
        "FEN_Test est une Fenetre\n"
        "\tgGlobaleFenetre est une chaine = \"a\"\n"
        "\t%s\n"
        "\tFIN\n"
        "\tCOMBO_A est un Champ Combo\n"
        "\t\tPROPRIETES\n"
        "\t\t\tContenu = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n"
        "FEN_Autre est une Fenetre\n"
        "\tgGlobaleAutre est une chaine = \"c\"\n"
        "FIN\n", c->decl);
    setup_doc(URI, src);
    int col = (int)strlen("\t\t\tContenu = ");
    JsonValue* params = make_completion_params(URI, 6, col);
    JsonValue* items = handle_completion(params);
    snprintf(msg, sizeof(msg), "fix/1386 fenetre [%s]", c->label);
    ASSERT_HAS    (items, "gGlobaleFenetre", msg);
    ASSERT_NOT_HAS(items, "gGlobaleAutre",   msg);
    json_free(items);
    json_free(params);
}

static void test_champ_prefix_coherence_inter_chemins(void) {
    printf("\n[TEST] fix/1386 : meme declaration traitee identiquement par les 4 chemins\n");
    for (int i = 0; i < CHAMP_PREFIX_CASES_N; i++) {
        const ChampPrefixCase* c = &CHAMP_PREFIX_CASES[i];
        test_champ_prefix_coherence_contexte(c);
        test_champ_prefix_coherence_bornes(c);
        test_champ_prefix_coherence_fenetre(c);
    }
}

/* Contre-epreuve : le filler `de` d'une COLLECTION ne doit jamais etre
 * traverse. Il arrive avec prev = `tableau` (pas la copule), et le type
 * d'element peut porter le nom d'un genre (`Image` est a la fois un type
 * raster et un Champ). Traverse a tort, il ouvrirait un bloc fantome
 * jamais ferme : le bloc PROPRIETES suivant tomberait sur Image au lieu
 * de la Fenetre. */
static void test_champ_prefix_collection_de_non_traverse(void) {
    printf("\n[TEST] fix/1386 : le 'de' d'un tableau de Image n'ouvre pas de bloc\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\ttabVignettes est un tableau de Image\n"
        "\tBTN_X est un Champ Bouton\n"
        "\tFIN\n"
        "\tPROPRIETES\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Titre",
                   "fix/1386 : props Fenetre apres 'tableau de Image'");
    ASSERT_NOT_HAS(items, "TypeBouton",
                   "fix/1386 : pas les props du Bouton referme");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — bloc PROPRIETES d'un Champ Onglet / d'un Volet.
 *
 *  Avant fix : sous un contexte 'p' (PROPRIETES), detect_dsl_context_ex
 *  resolvait le genre englobant en enumerant une liste de chars EN DUR
 *  qui omettait 'N' (Onglet) et 'V' (Volet). La boucle traversait donc
 *  ces deux niveaux et retombait sur 'F' -> la completion proposait les
 *  25 props de la Fenetre (Titre, Largeur, ...) au lieu de celles du
 *  genre reel. Le predicat est desormais derive du catalogue
 *  (dsl_kind_string != NULL), donc un genre ajoute au catalogue est
 *  couvert sans retoucher la resolution.
 * ================================================================ */

/* Cas nominal du ticket : PROPRIETES d'un Champ Onglet -> props Onglet. */
static void test_props_autocomplete_onglet(void) {
    printf("\n[TEST] fix/1379 : PROPRIETES d'un Champ Onglet propose les props Onglet\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"                    /* curseur ligne 3 col 3 */
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Position",       "prop Onglet Position");
    ASSERT_HAS(items, "Valeur",         "prop Onglet Valeur");
    ASSERT_HAS(items, "LargeurVolets",  "prop Onglet LargeurVolets");
    ASSERT_HAS(items, "AlignementLibell\xc3\xa9", "prop Onglet AlignementLibelle");
    ASSERT_NOT_HAS(items, "Titre",      "prop Fenetre Titre non proposee dans un Onglet");
    json_free(items);
    json_free(params);
}

/* Le sous-bloc Volet a son propre jeu de props (Libelle/Disposition/Visible)
 * — ni celles de l'Onglet parent, ni celles de la Fenetre. */
static void test_props_autocomplete_volet(void) {
    printf("\n[TEST] fix/1379 : PROPRIETES d'un Volet propose les props Volet\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\t\n"                  /* curseur ligne 4 col 4 */
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 4, 4);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Libell\xc3\xa9",  "prop Volet Libelle");
    ASSERT_HAS(items, "Disposition",     "prop Volet Disposition");
    ASSERT_HAS(items, "Visible",         "prop Volet Visible");
    ASSERT_NOT_HAS(items, "Titre",       "prop Fenetre Titre non proposee dans un Volet");
    ASSERT_NOT_HAS(items, "LargeurVolets", "prop Onglet non proposee dans un Volet");
    int n = count_items(items);
    if (n == 3) {
        g_pass++;
        printf("  [OK]   fix/1379 : liste = catalogue Volet seul (3 items)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1379 : attendu 3 items (catalogue Volet), obtenu %d\n", n);
    }
    json_free(items);
    json_free(params);
}

/* Bilingue : en mode EN le bloc PROPERTIES d'un Champ Onglet sort les
 * formes EN du catalogue Onglet. */
static void test_props_autocomplete_onglet_en(void) {
    printf("\n[TEST] fix/1379 : mode EN -> PROPERTIES d'un Champ Onglet propose TabWidth\n");
    const char* src =
        "FEN_T is a Window\n"
        "\tONG_M is a Control Onglet\n"
        "\t\tPROPERTIES\n"
        "\t\t\t\n"                    /* curseur ligne 3 col 3 */
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "TabWidth",   "forme EN de LargeurVolets proposee en mode EN");
    ASSERT_HAS(items, "Position",   "prop Onglet Position (EN)");
    ASSERT_NOT_HAS(items, "Title",  "prop Fenetre Title non proposee dans un Onglet (EN)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Non-regression : un Champ Bouton imbrique dans un Volet garde SES props
 * (le genre resolu est le plus proche englobant, pas le Volet). */
static void test_props_autocomplete_bouton_dans_volet(void) {
    printf("\n[TEST] fix/1379 : PROPRIETES d'un Bouton imbrique dans un Volet -> props Bouton\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\t\tBTN_OK est un Champ Bouton\n"
        "\t\t\t\tPROPRIETES\n"
        "\t\t\t\t\t\n"                /* curseur ligne 5 col 5 */
        "\t\t\t\tFIN\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 5, 5);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Libell\xc3\xa9",   "prop Bouton Libelle");
    ASSERT_NOT_HAS(items, "Disposition",  "prop Volet non proposee dans le Bouton enfant");
    ASSERT_NOT_HAS(items, "LargeurVolets","prop Onglet non proposee dans le Bouton enfant");
    ASSERT_NOT_HAS(items, "Titre",        "prop Fenetre non proposee dans le Bouton enfant");
    json_free(items);
    json_free(params);
}

/* Non-regression : le PROPRIETES de la Fenetre elle-meme, pose APRES un
 * bloc Onglet+Volet, reste sur le catalogue Fenetre. */
static void test_props_autocomplete_fenetre_apres_bloc_onglet(void) {
    printf("\n[TEST] fix/1379 : PROPRIETES Fenetre apres un bloc Onglet+Volet -> props Fenetre\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tONG_M est un Champ Onglet\n"
        "\t\tVOL_A est un Volet\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tPROPRIETES\n"
        "\t\t\n"                      /* curseur ligne 6 col 2 */
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Titre",            "prop Fenetre Titre");
    ASSERT_HAS(items, "Disposition",      "prop Fenetre Disposition");
    ASSERT_NOT_HAS(items, "LargeurVolets","prop Onglet non proposee dans la Fenetre");
    json_free(items);
    json_free(params);
}

/* Non-regression : Conteneur (genre a token scanner dedie) inchange. */
static void test_props_autocomplete_conteneur(void) {
    printf("\n[TEST] fix/1379 : PROPRIETES d'un Champ Conteneur -> props Conteneur\n");
    const char* src =
        "FEN_T est une Fenetre\n"
        "\tCTN_A est un Champ Conteneur\n"
        "\t\tPROPRIETES\n"
        "\t\t\t\n"                    /* curseur ligne 3 col 3 */
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Disposition",      "prop Conteneur Disposition");
    ASSERT_NOT_HAS(items, "Titre",        "prop Fenetre non proposee dans un Conteneur");
    ASSERT_NOT_HAS(items, "LargeurVolets","prop Onglet non proposee dans un Conteneur");
    json_free(items);
    json_free(params);
}

/* Volet A+B — repro fidele d'ETAT_Demo.owl : 3 Champ Image + 2 Libelles,
 * 2 events pre-declares (IMG_LogoCentre + LIB_Note). Test au curseur
 * dans IMG_LogoEtire (sans event) : doit proposer AvantImpression. Si
 * volet B casse, l'event de IMG_LogoCentre ou LIB_Note (voisins) bloque. */
static void test_event_champ_image_multi_siblings_etatdemo(void) {
    printf("\n[TEST] fix/0513 : repro ETAT_Demo.owl, curseur IMG_LogoEtire propose AvantImpression\n");
    const char* src =
        "ETAT_Demo est un Etat\n"
        "\tintro est un EnTeteDocument\n"
        "\t\tLIB_Titre est un Libelle\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoCentre est un Champ Image\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"centre\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoEtire est un Champ Image\n"
        "\t\t\tPROCEDURE \n"               /* curseur ligne 12 col 13 */
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoHomo est un Champ Image\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tcorps est un Corps\n"
        "\t\tLIB_Note est un Libelle\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"note\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 12, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression dispo dans IMG_LogoEtire malgre events voisins IMG_LogoCentre+LIB_Note");
    json_free(items);
    json_free(params);
}

/* Volet A+B — meme repro ETAT_Demo, curseur dans IMG_LogoHomo
 * (3e Champ Image, sans event). 2 events voisins (IMG_LogoCentre +
 * LIB_Note) ne doivent pas bloquer. */
static void test_event_champ_image_multi_siblings_etatdemo_logohomo(void) {
    printf("\n[TEST] fix/0513 : repro ETAT_Demo, curseur IMG_LogoHomo propose AvantImpression\n");
    const char* src =
        "ETAT_Demo est un Etat\n"
        "\tintro est un EnTeteDocument\n"
        "\t\tLIB_Titre est un Libelle\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoCentre est un Champ Image\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"centre\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoEtire est un Champ Image\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoHomo est un Champ Image\n"
        "\t\t\tPROCEDURE \n"               /* curseur ligne 15 col 13 */
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tcorps est un Corps\n"
        "\t\tLIB_Note est un Libelle\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"note\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 15, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression dispo dans IMG_LogoHomo malgre events voisins");
    json_free(items);
    json_free(params);
}

/* Volet B — meme repro, curseur dans LIB_Titre (Libelle SANS event).
 * Voisins LIB_Note et IMG_LogoCentre ont chacun un AvantImpression :
 * ne doivent PAS bloquer. */
static void test_event_libelle_etat_siblings_with_pre_declared_events(void) {
    printf("\n[TEST] fix/0513 : repro ETAT_Demo, curseur LIB_Titre propose AvantImpression malgre 2 events voisins\n");
    const char* src =
        "ETAT_Demo est un Etat\n"
        "\tintro est un EnTeteDocument\n"
        "\t\tLIB_Titre est un Libelle\n"
        "\t\t\tPROCEDURE \n"               /* curseur ligne 3 col 13 */
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoCentre est un Champ Image\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"centre\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tcorps est un Corps\n"
        "\t\tLIB_Note est un Libelle\n"
        "\t\t\tPROCEDURE AvantImpression()\n"
        "\t\t\t\tTrace(\"note\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "AvantImpression",
               "AvantImpression dispo dans LIB_Titre malgre events voisins");
    json_free(items);
    json_free(params);
}

/* Volet B (scope strict) — meme repro, curseur dans IMG_LogoCentre
 * qui A DEJA `AvantImpression`. Doit etre FILTRE (scope du champ
 * courant). Verifie que find_dsl_block_end borne correctement. */
static void test_event_champ_image_already_declared_in_self_filtered(void) {
    printf("\n[TEST] fix/0513 : repro ETAT_Demo, IMG_LogoCentre filtre AvantImpression (deja declare CHEZ LUI)\n");
    const char* src =
        "ETAT_Demo est un Etat\n"
        "\tintro est un EnTeteDocument\n"
        "\t\tIMG_LogoCentre est un Champ Image\n"
        "\t\t\tPROCEDURE \n"               /* curseur ligne 3 col 13 */
        "\t\t\tPROCEDURE AvantImpression()\n" /* declare APRES curseur, MEME champ */
        "\t\t\t\tTrace(\"centre\")\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\n"
        "\t\tIMG_LogoEtire est un Champ Image\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "AvantImpression",
                   "AvantImpression filtre dans IMG_LogoCentre (deja declare chez lui)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — diagnostics flicker : cache compiler_diag per-URI
 *
 *  Probleme : publish_diagnostics (scanner-only, chemin didOpen/didChange)
 *  ecrase les diagnostics semantiques poses par publish_compiler_diagnostics
 *  (chemin didSave). L'utilisateur voit l'erreur apres save, navigue vers
 *  le fichier (didOpen) et l'erreur disparait.
 *
 *  Fix attendu : LspServer porte un cache per-URI des diagnostics --check.
 *  publish_compiler_diagnostics le peuple ; publish_diagnostics le fusionne
 *  (cache compiler + erreurs lexicales fraiches) au lieu de le remplacer.
 *
 *  Interface requise exposee par l'implementeur (lsp_handlers.c) :
 *
 *    int lsp_get_compiler_diag_count(const char* uri)
 *        Retourne le nombre de diagnostics semantiques actuellement en cache
 *        pour cet URI. Retourne 0 si le cache est vide (pas d'erreur).
 *        Retourne -1 si l'URI est inconnu du cache (pas encore peuple).
 *        UNIQUEMENT pour les tests -- ne pas exporter en production.
 *
 *    void lsp_populate_compiler_diag_cache(const char* uri, int count)
 *        Peuple le cache avec `count` diagnostics fictifs pour cet URI.
 *        Permet de simuler un didSave avec erreurs sans lancer owl --check.
 *        UNIQUEMENT pour les tests -- ne pas exporter en production.
 *
 *    void lsp_clear_compiler_diag_cache(const char* uri)
 *        Efface le cache pour cet URI (= ce que fait publish_compiler_diagnostics
 *        quand --check ne retourne pas d'erreur pour ce fichier).
 *        UNIQUEMENT pour les tests -- ne pas exporter en production.
 *
 *  NOTE TESTABILITE : publish_diagnostics et publish_compiler_diagnostics sont
 *  des void qui ecrivent directement sur stdout (lsp_send_notification).
 *  Il est IMPOSSIBLE d'intercepter les notifications emises depuis le harness
 *  in-process sans rediriger stdout. Ces tests verifient UNIQUEMENT l'etat
 *  du cache interne (couche donnees), pas l'emission des notifications
 *  (couche transport). La validation que le bon set est emis reste visuelle
 *  dev (cf. scenario manuel ci-dessous).
 *
 *  SCENARIO DE VALIDATION MANUELLE (dev, VSCode) :
 *    1. Ouvrir un fichier .owl avec une erreur semantique (ex: type inconnu).
 *    2. Faire Save → l'erreur doit apparaitre dans le panneau Problemes.
 *    3. Cliquer sur le message d'erreur → VSCode ouvre le fichier au bon endroit.
 *    4. Verifier que l'erreur est TOUJOURS presente dans le panneau Problemes.
 *       (Avant fix : elle disparait ici. Apres fix : elle persiste.)
 *    5. Modifier le fichier (typer un espace) → l'erreur doit rester visible.
 *    6. Corriger l'erreur et sauvegarder → l'erreur doit disparaitre definitivement.
 * ================================================================ */

/* Stub compile-time : si l'implementeur n'a pas encore expose les fonctions
 * de test du cache, elles retournent une valeur sentinelle qui fait rougir
 * les assertions de maniere explicite et documentee. */
#ifndef LSP_DIAG_CACHE_API_IMPLEMENTED
static int lsp_get_compiler_diag_count(const char* uri) {
    (void)uri;
    return -2; /* sentinelle "pas encore implemente" */
}
static void lsp_populate_compiler_diag_cache(const char* uri, int count) {
    (void)uri; (void)count; /* no-op stub */
}
static void lsp_clear_compiler_diag_cache(const char* uri) {
    (void)uri; /* no-op stub */
}
#endif /* LSP_DIAG_CACHE_API_IMPLEMENTED */

/*  v2 -- Spy test-only : detecter si publish_compiler_diagnostics
 * a court-circuite (early-return) pour un scheme != file://.
 *
 * Contrat implementeur :
 *   1. Ajouter en tete de publish_compiler_diagnostics :
 *        if (strncmp(uri, "file://", 7) != 0) {
 *            lsp__diag_scheme_guard_fired = 1;
 *            return;
 *        }
 *   2. Exposer lsp_diag_scheme_was_rejected() et lsp_diag_scheme_reset()
 *      (ou les implementer inline avec la variable statique interne).
 *   3. Definir LSP_DIAG_SCHEME_GUARD_IMPLEMENTED.
 *
 * Valeurs de retour de lsp_diag_scheme_was_rejected() :
 *   -1 : sentinel "guard non implemente" (stub ci-dessous)
 *    0 : guard implemente mais n'a pas fire (scheme == file:// ou pas appele)
 *    1 : guard a fire -> early-return OK pour scheme != file://
 */
#ifndef LSP_DIAG_SCHEME_GUARD_IMPLEMENTED
static int lsp_diag_scheme_was_rejected(void) {
    return -1; /* sentinel "pas encore implemente" */
}
static void lsp_diag_scheme_reset(void) {
    /* no-op stub */
}
#endif /* LSP_DIAG_SCHEME_GUARD_IMPLEMENTED */

/*  — Test 1 : apres un didSave (cache peuple), un didOpen/didChange
 * ne doit PAS remettre le cache a zero.
 * RED attendu : lsp_get_compiler_diag_count retourne -2 (stub) au lieu de 1. */
static void test_diag_compiler_cache_persists_after_didopen(void) {
    printf("\n[TEST] fix/0639 T1 : cache compiler_diag persiste apres didOpen (RED attendu)\n");

    const char* uri = "file:///c:/test/fix0639_a.owl";
    const char* src = "x est un TypeInexistant\n"; /* erreur semantique fictive */

    setup_doc(uri, src);

    /* Simule un didSave : peuple le cache avec 1 diagnostic semantique. */
    lsp_populate_compiler_diag_cache(uri, 1);

    /* Simule un didOpen/didChange : chemin scanner-only. */
    publish_diagnostics(uri);

    /* Le cache doit contenir encore 1 diagnostic, pas 0. */
    int count = lsp_get_compiler_diag_count(uri);
    if (count == 1) {
        g_pass++;
        printf("  [OK]   fix/0639 T1 : cache = 1 apres didOpen (semantique preserved)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0639 T1 : cache = %d apres didOpen (attendu 1 — "
               "stub=-2 = pas implemente, 0 = semantique ecrasee)\n", count);
    }
}

/*  — Test 2 : une erreur lexicale fraiche au didChange ne doit pas
 * ecraser les diagnostics semantiques du cache.
 * RED attendu : lsp_get_compiler_diag_count retourne -2 (stub) au lieu de 2. */
static void test_diag_scanner_errors_dont_erase_compiler_cache(void) {
    printf("\n[TEST] fix/0639 T2 : erreur scanner au didChange ne purge pas le cache (RED attendu)\n");

    const char* uri = "file:///c:/test/fix0639_b.owl";
    /* '@' est un token illegal → publish_diagnostics genere 1 erreur lexicale. */
    const char* src = "@ erreur lexicale\n";

    setup_doc(uri, src);

    /* Simule un didSave avec 2 diagnostics semantiques en cache. */
    lsp_populate_compiler_diag_cache(uri, 2);

    /* Simule un didChange avec erreur lexicale : le scanner trouve '@'. */
    publish_diagnostics(uri);

    /* Le cache semantique doit toujours contenir 2 entrees. */
    int count = lsp_get_compiler_diag_count(uri);
    if (count == 2) {
        g_pass++;
        printf("  [OK]   fix/0639 T2 : cache = 2 apres didChange avec erreur lexicale\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0639 T2 : cache = %d (attendu 2 — "
               "stub=-2 = pas implemente, 0 = semantique ecrasee par lexical)\n", count);
    }
}

/*  — Test 3 : quand --check revient propre pour un URI, le cache
 * doit etre efface (pas de fantome). Simule via lsp_clear_compiler_diag_cache.
 * RED attendu : lsp_get_compiler_diag_count retourne -2 (stub) au lieu de 0. */
static void test_diag_cache_cleared_when_check_clean(void) {
    printf("\n[TEST] fix/0639 T3 : cache efface quand --check propre (RED attendu)\n");

    const char* uri = "file:///c:/test/fix0639_c.owl";
    const char* src = "n est un entier = 0\n"; /* source valide */

    setup_doc(uri, src);

    /* Peuple le cache (etat "avant save avec erreur"). */
    lsp_populate_compiler_diag_cache(uri, 3);

    /* Simule un --check propre : aucune erreur pour cet URI → efface le cache. */
    lsp_clear_compiler_diag_cache(uri);

    /* Le cache doit maintenant etre a 0, pas a 3. */
    int count = lsp_get_compiler_diag_count(uri);
    if (count == 0) {
        g_pass++;
        printf("  [OK]   fix/0639 T3 : cache = 0 apres --check propre (pas de fantome)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0639 T3 : cache = %d (attendu 0 — "
               "stub=-2 = pas implemente, >0 = fantome persiste)\n", count);
    }
}

/* ================================================================
 *   — cluster qualite auto-completion
 * ================================================================ */

/* C3 — `tab est un tableau de ` propose des noms de TYPES, pas des
 * natives en Dé* (Défile/Dépile). Le mot-cle `de` ne doit PAS etre pris
 * comme prefixe de filtre. */
static void test_tableau_de_proposes_types_not_natives(void) {
    printf("\n[TEST] fix/0578 C3 : `tableau de ` propose des types, pas Defile/Depile\n");
    const char* src = "tab est un tableau de \n";
    setup_doc(URI, src);
    /* 'tab est un tableau de ' = 22 chars, curseur en fin de ligne. */
    JsonValue* params = make_completion_params(URI, 0, 22);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",   "tableau de : type entier");
    ASSERT_HAS    (items, "cha\xc3\xaene", "tableau de : type chaine");
    ASSERT_HAS    (items, "bool\xc3\xa9""en", "tableau de : type booleen");
    ASSERT_NOT_HAS(items, "D\xc3\xa9""file", "tableau de : pas la native Defile");
    ASSERT_NOT_HAS(items, "D\xc3\xa9""pile", "tableau de : pas la native Depile");
    ASSERT_NOT_HAS(items, "Trace",    "tableau de : pas de native generique");
    json_free(items);
    json_free(params);
}

/* C3 bis — `liste de ` doit aussi proposer les types. */
static void test_liste_de_proposes_types(void) {
    printf("\n[TEST] fix/0578 C3 : `liste de ` propose des types\n");
    const char* src = "l est une liste de \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 19);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "entier",   "liste de : type entier");
    ASSERT_NOT_HAS(items, "Trace",    "liste de : pas de native");
    json_free(items);
    json_free(params);
}

/* C4 — `tab.Cherche` en appel fluent : le snippet ne doit PAS reinserer
 * le receveur en 1er param. Le receveur est implicite. */
static void test_fluent_method_drops_receiver_param(void) {
    printf("\n[TEST] fix/0578 C4 : `tab.Cherche` snippet sans le receveur en 1er param\n");
    const char* src =
        "tab est un tableau de entier\n"
        "x = tab.Cherche\n";
    setup_doc(URI, src);
    /* 'x = tab.Cherche' = 15 chars, curseur juste apres le '.' suffit pour
     * la completion ; on place le curseur apres '.' (col 8). */
    JsonValue* params = make_completion_params(URI, 1, 8);
    JsonValue* items = handle_completion(params);
    const char* it = find_insertText_for_label(items, "Cherche");
    /*  — receveur 'tableau' absent ET param 'valeur' PRESENT
     * (l'ancien `Cherche()` vide masquait l'argument requis). */
    if (it && strstr(it, "tableau") == NULL && strstr(it, "valeur") != NULL) {
        g_pass++;
        printf("  [OK]   fix/0581 C4 'Cherche' insertText='%s' (receveur absent, valeur gardee)\n", it);
    } else {
        g_fail++;
        printf("  [FAIL] fix/0581 C4 'Cherche' insertText='%s' (attendu: sans 'tableau', avec 'valeur')\n",
               it ? it : "(null)");
    }
    json_free(items);
    json_free(params);
    /* b — cas reel dev : le mot 'Cherche' ENTIEREMENT tape (curseur en
     * fin, col 15). Sans le fix, le contexte dot etait perdu -> signature
     * native AVEC le receveur 'tableau'. */
    JsonValue* params2 = make_completion_params(URI, 1, 15);
    JsonValue* items2 = handle_completion(params2);
    const char* it2 = find_insertText_for_label(items2, "Cherche");
    if (it2 && strstr(it2, "tableau") == NULL && strstr(it2, "valeur") != NULL) {
        g_pass++;
        printf("  [OK]   fix/0581 C4 col15 (mot complet) insertText='%s' (receveur absent, valeur gardee)\n", it2);
    } else {
        g_fail++;
        printf("  [FAIL] fix/0581 C4 col15 insertText='%s' (attendu: sans 'tableau', avec 'valeur')\n",
               it2 ? it2 : "(null)");
    }
    json_free(items2);
    json_free(params2);
}

/*  — hover sur une méthode fluent montre la signature réelle MOINS
 * le receveur : `.Cherche(valeur)`, pas `.Cherche()` vide. */
static void test_fluent_method_hover_shows_params(void) {
    printf("\n[TEST] fix/0581 : hover `.Cherche` montre `(valeur)` pas `()`\n");
    const char* src =
        "tab est un tableau de entier\n"
        "x = tab.Cherche(0)\n";
    setup_doc(URI, src);
    /* curseur dans 'Cherche' ligne 1 (C en col 8, on vise col 10). */
    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string(URI));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(1));
    json_object_set(pos, "character", json_new_int(10));
    json_object_set(params, "position", pos);

    JsonValue* result = handle_hover(params);
    JsonValue* contents = result ? json_get(result, "contents") : NULL;
    const char* value = contents ? json_get_string(contents, "value") : NULL;
    if (value && strstr(value, "valeur") != NULL && strstr(value, ".Cherche()") == NULL) {
        g_pass++;
        printf("  [OK]   fix/0581 hover montre l'argument 'valeur'\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/0581 hover value='%s' (attendu: contient 'valeur', pas '.Cherche()')\n",
               value ? value : "(null)");
    }
    json_free(result);
    json_free(params);
}

/* C1 — `icn est une Icone = { <curseur> }` propose les sous-props du type
 * Icone (Couleur, Rotation, Forme...). */
static void test_brace_literal_registered_type_fields(void) {
    printf("\n[TEST] fix/0578 C1 : `Icone = { ` propose les props du type Icone\n");
    const char* src =
        "icn est une Ic\xc3\xb4ne = {\n"
        "\t\n"
        "}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 1, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "Couleur",  "brace Icone : Couleur");
    ASSERT_HAS(items, "Rotation", "brace Icone : Rotation");
    ASSERT_HAS(items, "Forme",    "brace Icone : Forme");
    json_free(items);
    json_free(params);
}

/*  — `icn est une Icône = {‸}` brace VIDE sur la MÊME ligne,
 * après un type à accent (`ô` = 2 octets). La détection de contexte
 * ligne lit `character` en unités UTF-16 ; sans conversion en octet, le
 * curseur est vu après le `=` (contexte simpleAssign) au lieu de dans
 * `{}`, et les sous-props ne sont pas proposées. Curseur entre `{` et
 * `}` : 21 unités UTF-16 ("icn est une Icône = {"). */
static void test_brace_literal_same_line_after_accented_type(void) {
    printf("\n[TEST] fix/0580 : `Ic\xc3\xb4ne = {‸}` même ligne propose les props\n");
    const char* src = "icn est une Ic\xc3\xb4ne = {}\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 21);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Couleur",  "fix/0580 : Couleur dans {} même ligne");
    ASSERT_HAS    (items, "Rotation", "fix/0580 : Rotation dans {} même ligne");
    ASSERT_NOT_HAS(items, "Trace",    "fix/0580 : pas de native générique");
    json_free(items);
    json_free(params);
}

/*  — completion des props soeurs restantes dans un littéral inline
 * `{ }` d'un sous-élément de style à LHS ACCENTUÉ, dans un fichier thème.
 * `CadreExtérieur = { CouleurFond = Bleu, <cursor> }` : le Cas 2 de la
 * détection brace-literal résout le sous-type via le fieldName. Le catalogue
 * stocke ce fieldName accent-strippé (`CadreExterieur`) alors que la source
 * canonique est accentuée (`CadreExtérieur`) → la comparaison doit être
 * accent-insensible (miroir de composed_field_subtype), sinon le type ne se
 * résout pas et la complétion est vide. */
static void test_brace_literal_theme_accented_composed_after_comma(void) {
    printf("\n[TEST] fix/1019 : `CadreExt\xc3\xa9rieur = { .., <cursor> }` (thème) propose les props soeurs\n");
    const char* src =
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "\tboutonPrimaire est un styleBouton = {\n"
        "\t\tCadreExt\xc3\xa9rieur = { CouleurFond = Bleu, \n"
        "\t\t}\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 2 (0-based). Curseur en fin de ligne, après `Bleu, ` :
     * \t\t(2) + CadreExtérieur(14) + " = { "(5) + CouleurFond(11)
     * + " = "(3) + Bleu(4) + ", "(2) = 41 unités UTF-16. */
    JsonValue* params = make_completion_params(URI, 2, 41);
    JsonValue* items = handle_completion(params);
    /* styleCadreExtérieur : props soeurs restantes. */
    ASSERT_HAS    (items, "Cadre",         "fix/1019 : Cadre proposé");
    ASSERT_HAS    (items, "RetraitGauche", "fix/1019 : RetraitGauche proposé");
    /* CouleurFond déjà saisie → filtrée. */
    ASSERT_NOT_HAS(items, "CouleurFond",   "fix/1019 : CouleurFond filtrée (déjà set)");
    json_free(items);
    json_free(params);
}

/*  — contrôle : le MÊME contexte avec un LHS de sous-élément SANS
 * accent (`ZoneSaisie`) résout déjà aujourd'hui. Prouve que l'accent est le
 * seul discriminant du bug, et garde la non-régression du chemin ASCII. */
static void test_brace_literal_theme_ascii_composed_after_comma(void) {
    printf("\n[TEST] fix/1019 contrôle : `ZoneSaisie = { .., <cursor> }` (LHS ASCII) résout\n");
    const char* src =
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "\tsaisieCompacte est un styleSaisie = {\n"
        "\t\tZoneSaisie = { RetraitGauche = 2, \n"
        "\t\t}\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 2 : \t\t(2) + ZoneSaisie(10) + " = { "(5) + RetraitGauche(13)
     * + " = "(3) + 2(1) + ", "(2) = 36 unités UTF-16. */
    JsonValue* params = make_completion_params(URI, 2, 36);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Couleur",       "fix/1019 contrôle : Couleur proposé");
    ASSERT_HAS    (items, "Police",        "fix/1019 contrôle : Police proposé");
    ASSERT_NOT_HAS(items, "RetraitGauche", "fix/1019 contrôle : RetraitGauche filtrée (déjà set)");
    json_free(items);
    json_free(params);
}

/* C2 — `icn..Icone = ` (champ source catalogue du type Icone) propose
 * le namespace `icones.` et NE noie PAS dans tout le langage. */
static void test_prop_value_icone_offers_icones_namespace(void) {
    printf("\n[TEST] fix/0578 C2 : `icn..Icone = ` propose le namespace icones.\n");
    const char* src =
        "icn est une Ic\xc3\xb4ne\n"
        "icn..Icone = \n";
    setup_doc(URI, src);
    /* 'icn..Icone = ' = 13 chars, curseur en fin de ligne 1. */
    JsonValue* params = make_completion_params(URI, 1, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "icones",   "C2 : namespace icones propose");
    ASSERT_NOT_HAS(items, "Trace",    "C2 : pas de native generique");
    json_free(items);
    json_free(params);
}

/* Defini dans src/internal_tests/fmt_stability_test.c (linke dans owl-lsp-test
 * pour le gate de parite). */
extern int fmt_stability_self_test_run(void);

/* ================================================================
 *   — go-to-definition POO : classes, enums, membres,
 *              membres herités.
 *  Tous ces tests sont ROUGES avant impl (handle_definition renvoie
 *  null sur tout ce qui n'est pas userSymbols).
 * ================================================================ */

/* Cas 1 — F2 sur un nom de classe dans une declaration de variable.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : cBoisson est une Classe
 *   ligne 1 : PUBLIC
 *   ligne 2 : \tnVolume est un entier
 *   ligne 3 : FIN
 *   ligne 4 : (vide)
 *   ligne 5 : b est un cBoisson     ← curseur sur 'cBoisson' char 10
 *
 * Attendu : Location { line=0, char=0 } (debut de "cBoisson est une Classe").
 * Rouge attendu : handle_definition cherche uniquement userSymbols aujourd'hui.
 */
static void test_definition_class_name(void) {
    printf("\n[TEST] feat/0643 C1 : F2 sur nom de classe -> declaration (RED attendu)\n");
    const char* src =
        "cBoisson est une Classe\n"  /* ligne 0 */
        "PUBLIC\n"                   /* ligne 1 */
        "\tnVolume est un entier\n"  /* ligne 2 */
        "FIN\n"                      /* ligne 3 */
        "\n"                         /* ligne 4 */
        "b est un cBoisson\n";       /* ligne 5 — 'cBoisson' a char 10 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 5, 10);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 0, "def classe cBoisson");
    json_free(result);
    json_free(params);
}

/* Cas 2 — F2 sur un nom d'énum dans une declaration de variable.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : ETypeBoisson est une Enumeration
 *   ligne 1 : \tbiere
 *   ligne 2 : \tvin
 *   ligne 3 : FIN
 *   ligne 4 : (vide)
 *   ligne 5 : t est un ETypeBoisson   ← curseur sur 'ETypeBoisson' char 10
 *
 * Attendu : Location { line=0, char=0 }.
 * Rouge attendu : UserEnumInfo n'a pas de line/docIndex, handler renvoie null.
 */
static void test_definition_enum_name(void) {
    printf("\n[TEST] feat/0643 C2 : F2 sur nom d'enum -> declaration (RED attendu)\n");
    const char* src =
        "ETypeBoisson est une Enumeration\n"  /* ligne 0 */
        "\tbiere\n"                           /* ligne 1 */
        "\tvin\n"                             /* ligne 2 */
        "FIN\n"                               /* ligne 3 */
        "\n"                                  /* ligne 4 */
        "t est un ETypeBoisson\n";            /* ligne 5 — 'ETypeBoisson' a char 10 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 5, 10);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 0, "def enum ETypeBoisson");
    json_free(result);
    json_free(params);
}

/* Cas 3 — F2 sur un membre (champ) declare dans la classe.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : cBoisson est une Classe
 *   ligne 1 : PUBLIC
 *   ligne 2 : \tnVolume est un entier   ← declaration du champ
 *   ligne 3 : \tPROCEDURE Sert()
 *   ligne 4 : \tFIN
 *   ligne 5 : FIN
 *   ligne 6 : (vide)
 *   ligne 7 : b est un cBoisson
 *   ligne 8 : x = b.nVolume            ← curseur sur 'nVolume' char 6
 *
 * Attendu : Location { line=2, char=1 } (le tab + 'nVolume' dans le body).
 * Rouge attendu : UserClassMember n'a pas de champ 'line', handler renvoie null.
 */
static void test_definition_class_member(void) {
    printf("\n[TEST] feat/0643 C3 : F2 sur membre declare dans classe -> ligne membre (RED attendu)\n");
    const char* src =
        "cBoisson est une Classe\n"   /* ligne 0 */
        "PUBLIC\n"                    /* ligne 1 */
        "\tnVolume est un entier\n"   /* ligne 2 — 'nVolume' a char 1 */
        "\tPROCEDURE Sert()\n"       /* ligne 3 */
        "\tFIN\n"                     /* ligne 4 */
        "FIN\n"                       /* ligne 5 */
        "\n"                          /* ligne 6 */
        "b est un cBoisson\n"         /* ligne 7 */
        "x = b.nVolume\n";            /* ligne 8 — 'nVolume' a char 6 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 8, 6);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 2, "def membre nVolume dans cBoisson");
    json_free(result);
    json_free(params);
}

/* Cas 4 — F2 sur un membre herite (declare dans la classe de base).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : cBoisson est une Classe
 *   ligne 1 : PUBLIC
 *   ligne 2 : \tnAlcool est un entier  ← declaration dans la BASE
 *   ligne 3 : FIN
 *   ligne 4 : (vide)
 *   ligne 5 : cBiere est une Classe Herite de cBoisson
 *   ligne 6 : PUBLIC
 *   ligne 7 : \tnMousse est un entier
 *   ligne 8 : FIN
 *   ligne 9 : (vide)
 *   ligne 10: b est un cBiere
 *   ligne 11: x = b.nAlcool            ← curseur sur 'nAlcool' char 6
 *
 * Attendu : Location { line=2, char=1 } (membre dans cBoisson, pas cBiere).
 * Rouge attendu : baseClass non tracke dans UserClassInfo,
 *                 recherche hierarchique pas implementee.
 */
static void test_definition_inherited_member(void) {
    printf("\n[TEST] feat/0643 C4 : F2 sur membre herite -> declaration dans base (RED attendu)\n");
    const char* src =
        "cBoisson est une Classe\n"               /* ligne 0  */
        "PUBLIC\n"                                 /* ligne 1  */
        "\tnAlcool est un entier\n"               /* ligne 2 — 'nAlcool' a char 1 */
        "FIN\n"                                    /* ligne 3  */
        "\n"                                       /* ligne 4  */
        "cBiere est une Classe Herite de cBoisson\n" /* ligne 5 */
        "PUBLIC\n"                                 /* ligne 6  */
        "\tnMousse est un entier\n"               /* ligne 7  */
        "FIN\n"                                    /* ligne 8  */
        "\n"                                       /* ligne 9  */
        "b est un cBiere\n"                        /* ligne 10 */
        "x = b.nAlcool\n";                         /* ligne 11 — 'nAlcool' a char 6 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 11, 6);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 2, "def membre herite nAlcool (base cBoisson)");
    json_free(result);
    json_free(params);
}

/* Cas 5 — F2 sur une native ou un mot inconnu : null propre, pas de crash.
 *
 * Attendu : null. Doit deja passer vert (le handler renvoie json_new_null()
 * en fin de parcours userSymbols si le symbole n'est pas trouve).
 * C'est le seul test vert attendu de ce groupe.
 */
static void test_definition_unknown_symbol_null(void) {
    printf("\n[TEST] feat/0643 C5 : F2 sur native/mot inconnu -> null propre (VERT attendu)\n");
    const char* src = "Trace(\"hello\")\n";  /* ligne 0 — 'Trace' a char 0 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 0, 0);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_NULL(result, "def null sur native Trace");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   Lot 0 — moteur de portee/liaison scope-aware.
 *  Tous ces tests sont ROUGES avant impl (handle_definition resout
 *  encore "par nom" -> premier homonyme, pas de conscience de portee).
 *  Aucun shadow cross-scope ( : erreur compile) -- les cas
 *  ci-dessous n'emploient que des portees SŒURS (locales de
 *  procedures distinctes, sans lien englobant/inner).
 * ================================================================ */

/* Cas 1 (verrou central) — deux procedures F et G declarent chacune
 * leur PROPRE locale `x` (portees sœurs, legal — pas de shadow).
 * F2 sur l'occurrence de `x` DANS G doit resoudre vers LA declaration
 * de G (ligne 6), pas vers celle de F (ligne 1) — le resolveur actuel
 * "premier homonyme du fichier" renvoie F -> ROUGE attendu.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE F()
 *   ligne 1 : \tx est un entier        <- declaration F
 *   ligne 2 : \tx = 1
 *   ligne 3 : FIN
 *   ligne 4 : (vide)
 *   ligne 5 : PROCEDURE G()
 *   ligne 6 : \tx est un entier        <- declaration G (attendue)
 *   ligne 7 : \tx = 2
 *   ligne 8 : \tTrace(x)               <- curseur ici, char 7
 *   ligne 9 : FIN
 */
static void test_definition_scope_sibling_locals(void) {
    printf("\n[TEST] feat/1740 L0-C1 : F2 sur locale dans G -> declaration de G, pas F (RED attendu)\n");
    const char* src =
        "PROCEDURE F()\n"      /* ligne 0 */
        "\tx est un entier\n"  /* ligne 1 — declaration F */
        "\tx = 1\n"            /* ligne 2 */
        "FIN\n"                /* ligne 3 */
        "\n"                   /* ligne 4 */
        "PROCEDURE G()\n"      /* ligne 5 */
        "\tx est un entier\n"  /* ligne 6 — declaration G (attendue) */
        "\tx = 2\n"            /* ligne 7 */
        "\tTrace(x)\n"         /* ligne 8 — 'x' a char 7 */
        "FIN\n";               /* ligne 9 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 8, 7);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 6, "def locale x dans G -> ligne 6 (pas F ligne 1)");
    json_free(result);
    json_free(params);
}

/* Cas 2 — parametre de procedure. Deux procedures F et H declarent
 * chacune un parametre `p` homonyme (portees sœurs, legal). F2 sur
 * l'usage de `p` DANS H doit resoudre vers l'en-tete de H (ligne 4),
 * pas celle de F (ligne 0).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE F(p est un entier)   <- declaration F
 *   ligne 1 : \tRENVOYER p * 2
 *   ligne 2 : FIN
 *   ligne 3 : (vide)
 *   ligne 4 : PROCEDURE H(p est un entier)   <- declaration H (attendue)
 *   ligne 5 : \tRENVOYER p + 1               <- curseur ici, char 10
 *   ligne 6 : FIN
 */
static void test_definition_scope_sibling_params(void) {
    printf("\n[TEST] feat/1740 L0-C2 : F2 sur parametre dans H -> en-tete H, pas F (RED attendu)\n");
    const char* src =
        "PROCEDURE F(p est un entier)\n"  /* ligne 0 — declaration F */
        "\tRENVOYER p * 2\n"              /* ligne 1 */
        "FIN\n"                            /* ligne 2 */
        "\n"                                /* ligne 3 */
        "PROCEDURE H(p est un entier)\n"  /* ligne 4 — declaration H (attendue) */
        "\tRENVOYER p + 1\n"              /* ligne 5 — 'p' a char 10 */
        "FIN\n";                           /* ligne 6 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 5, 10);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 4, "def parametre p dans H -> ligne 4 (pas F ligne 0)");
    json_free(result);
    json_free(params);
}

/* Cas 3 — globale top-level vs champ de classe homonyme (axe
 * namespace VALUE vs MEMBER,  §2 "trois axes"). Un shadow
 * local/parametre du nom `g` serait un shadow ILLEGAL —
 * on discrimine donc via un champ de classe (namespace disjoint,
 * exception ) plutot qu'une locale homonyme.
 *
 * Fixture (0-indexed) :
 *   ligne 0  : g est un entier = 100         <- declaration globale
 *   ligne 1  : (vide)
 *   ligne 2  : cBoite est une Classe
 *   ligne 3  : PUBLIC
 *   ligne 4  : \tg est un entier              <- declaration champ (namespace distinct)
 *   ligne 5  : FIN
 *   ligne 6  : (vide)
 *   ligne 7  : PROCEDURE UseG()
 *   ligne 8  : \tTrace(g)                     <- curseur ici, char 7 -> attend ligne 0
 *   ligne 9  : FIN
 *   ligne 10 : (vide)
 *   ligne 11 : PROCEDURE UseBoiteG(b est un cBoite)
 *   ligne 12 : \tTrace(b.g)                   <- curseur ici, char 9 -> attend ligne 4
 *   ligne 13 : FIN
 */
static void test_definition_scope_global_vs_class_member(void) {
    printf("\n[TEST] feat/1740 L0-C3 : F2 sur globale g -> declaration top-level, pas champ classe (RED attendu)\n");
    const char* src =
        "g est un entier = 100\n"              /* ligne 0 */
        "\n"                                    /* ligne 1 */
        "cBoite est une Classe\n"               /* ligne 2 */
        "PUBLIC\n"                               /* ligne 3 */
        "\tg est un entier\n"                   /* ligne 4 — champ */
        "FIN\n"                                  /* ligne 5 */
        "\n"                                     /* ligne 6 */
        "PROCEDURE UseG()\n"                    /* ligne 7 */
        "\tTrace(g)\n"                          /* ligne 8 — 'g' a char 7 */
        "FIN\n"                                  /* ligne 9 */
        "\n"                                     /* ligne 10 */
        "PROCEDURE UseBoiteG(b est un cBoite)\n" /* ligne 11 */
        "\tTrace(b.g)\n"                        /* ligne 12 — 'g' a char 9 */
        "FIN\n";                                 /* ligne 13 */
    setup_doc(URI, src);

    JsonValue* p1 = make_definition_params(URI, 8, 7);
    JsonValue* r1 = handle_definition(p1);
    ASSERT_DEF_LINE(r1, 0, "def globale g (usage nu) -> ligne 0 (pas champ ligne 4)");
    json_free(r1);
    json_free(p1);

    JsonValue* p2 = make_definition_params(URI, 12, 9);
    JsonValue* r2 = handle_definition(p2);
    ASSERT_DEF_LINE(r2, 4, "def champ g (b.g) -> ligne 4 (pas globale ligne 0)");
    json_free(r2);
    json_free(p2);
}

/* Cas 4 — cross-fichier : globale declaree dans un compagnon OUVERT,
 * utilisee dans le fichier consommateur. La declaration doit pointer
 * le bon fichier (URI compagnon) ET la bonne ligne.
 *
 * companionSrc (ligne 0) : compteur est un entier = 0
 * consumerSrc  (ligne 0) : Trace(compteur)   <- curseur char 6
 */
static void test_definition_scope_cross_file_global(void) {
    printf("\n[TEST] feat/1740 L0-C4 : F2 cross-fichier sur globale -> compagnon ligne 0 (RED attendu)\n");
    const char* consumerUri = "file:///c:/test/consumer_1740.owl";
    const char* companionUri = "file:///c:/test/companion_1740.owl";
    const char* consumerSrc = "Trace(compteur)\n";           /* ligne 0 — 'compteur' a char 6 */
    const char* companionSrc = "compteur est un entier = 0\n"; /* ligne 0 — declaration */
    fix1013_setup_project_and_theme(consumerUri, consumerSrc, companionUri, companionSrc);

    JsonValue* params = make_definition_params(consumerUri, 0, 6);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 0, "def globale compteur -> ligne 0 compagnon");
    if (result && result->type == JSON_OBJECT) {
        const char* gotUri = json_get_string(result, "uri");
        if (gotUri && strcmp(gotUri, companionUri) == 0) {
            g_pass++;
            printf("  [OK]   feat/1740 L0-C4 Location.uri = companion_1740.owl\n");
        } else {
            g_fail++;
            printf("  [FAIL] feat/1740 L0-C4 Location.uri = %s (attendu %s)\n",
                   gotUri ? gotUri : "(null)", companionUri);
        }
    } else {
        g_fail++;
        printf("  [FAIL] feat/1740 L0-C4 pas de Location (globale cross-fichier non resolue)\n");
    }
    json_free(result);
    json_free(params);
}

/* Cas 5 (bonus) — bilingue/casse/accent : declaration accentuee,
 * usage sans accent et casse differente. Invariant 6.1/6.2 (CLAUDE.md) :
 * canonicalisation identique compilateur/LSP (scanner_canonicalize_identifier,
 * cf.  §1c). La resolution doit rester scope-correcte meme apres
 * canonicalisation — meme declaration visee quelle que soit la forme tapee.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : compteurG\xc3\xa9n\xc3\xa9ral est un entier = 5   <- declaration
 *   ligne 1 : (vide)
 *   ligne 2 : PROCEDURE Affiche()
 *   ligne 3 : \tTrace(compteurGeneral)    <- usage sans accent/majuscule, char 7
 *   ligne 4 : FIN
 */
static void test_definition_scope_accent_case_insensitive(void) {
    printf("\n[TEST] feat/1740 L0-C5 : F2 sur variable accentuee referencee sans accent (RED/bonus)\n");
    const char* src =
        "compteurG\xc3\xa9n\xc3\xa9ral est un entier = 5\n" /* ligne 0 */
        "\n"                                                  /* ligne 1 */
        "PROCEDURE Affiche()\n"                              /* ligne 2 */
        "\tTrace(compteurGeneral)\n"                         /* ligne 3 — char 7 */
        "FIN\n";                                              /* ligne 4 */
    setup_doc(URI, src);
    JsonValue* params = make_definition_params(URI, 3, 7);
    JsonValue* result = handle_definition(params);
    ASSERT_DEF_LINE(result, 0, "def compteurGeneral (sans accent) -> ligne 0 (declaration accentuee)");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   Lot 1 — textDocument/references scope-aware ( §3, §4).
 *  Tous ces tests sont ROUGES avant impl (handle_references est encore
 *  un stub qui renvoie []). Meme discipline sœurs que le Lot 0 ( :
 *  pas de shadow cross-scope, uniquement des portees sœurs).
 * ================================================================ */

/* Cas 1 (verrou central) — deux procedures F et G declarent chacune leur
 * PROPRE locale `x`, avec plusieurs usages chacune (portees sœurs, legal).
 * references sur l'occurrence de `x` DANS G doit renvoyer EXACTEMENT les
 * occurrences de G (declaration + 2 usages = 3), et ne JAMAIS inclure les
 * lignes de F.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE F()
 *   ligne 1 : \tx est un entier      <- decl F
 *   ligne 2 : \tx = 1                <- usage F
 *   ligne 3 : \tx = 2                <- usage F
 *   ligne 4 : FIN
 *   ligne 5 : (vide)
 *   ligne 6 : PROCEDURE G()
 *   ligne 7 : \tx est un entier      <- decl G
 *   ligne 8 : \tx = 10               <- usage G
 *   ligne 9 : \tTrace(x)             <- usage G, curseur ici char 7
 *   ligne 10: FIN
 */
static void test_references_scope_sibling_locals_isolated(void) {
    printf("\n[TEST] feat/1740 L1-C1 : references sur locale x dans G -> 3 occ, sans fuite vers F (RED attendu)\n");
    const char* src =
        "PROCEDURE F()\n"      /* ligne 0 */
        "\tx est un entier\n"  /* ligne 1 — decl F */
        "\tx = 1\n"            /* ligne 2 — usage F */
        "\tx = 2\n"            /* ligne 3 — usage F */
        "FIN\n"                /* ligne 4 */
        "\n"                   /* ligne 5 */
        "PROCEDURE G()\n"      /* ligne 6 */
        "\tx est un entier\n"  /* ligne 7 — decl G */
        "\tx = 10\n"           /* ligne 8 — usage G */
        "\tTrace(x)\n"         /* ligne 9 — usage G, char 7 */
        "FIN\n";               /* ligne 10 */
    setup_doc(URI, src);
    JsonValue* params = make_references_params(URI, 9, 7, true);
    JsonValue* result = handle_references(params);
    ASSERT_REFS_COUNT(result, 3, "references x dans G -> 3 occ (decl + 2 usages)");
    ASSERT_REFS_EXCLUDES_LINE(result, 1, "references x dans G -> pas la decl de F (ligne 1)");
    ASSERT_REFS_EXCLUDES_LINE(result, 2, "references x dans G -> pas l'usage de F (ligne 2)");
    ASSERT_REFS_EXCLUDES_LINE(result, 3, "references x dans G -> pas l'usage de F (ligne 3)");
    json_free(result);
    json_free(params);
}

/* Cas 2 — meme fixture, context.includeDeclaration=false : seuls les
 * usages restent (cardinal - 1 = 2), la declaration (ligne 7) exclue. */
static void test_references_scope_exclude_declaration(void) {
    printf("\n[TEST] feat/1740 L1-C2 : references x dans G, includeDeclaration=false -> 2 occ, sans decl (RED attendu)\n");
    const char* src =
        "PROCEDURE F()\n"      /* ligne 0 */
        "\tx est un entier\n"  /* ligne 1 */
        "\tx = 1\n"            /* ligne 2 */
        "\tx = 2\n"            /* ligne 3 */
        "FIN\n"                /* ligne 4 */
        "\n"                   /* ligne 5 */
        "PROCEDURE G()\n"      /* ligne 6 */
        "\tx est un entier\n"  /* ligne 7 — decl G, DOIT etre exclue */
        "\tx = 10\n"           /* ligne 8 — usage G */
        "\tTrace(x)\n"         /* ligne 9 — usage G, char 7 */
        "FIN\n";               /* ligne 10 */
    setup_doc(URI, src);
    JsonValue* params = make_references_params(URI, 9, 7, false);
    JsonValue* result = handle_references(params);
    ASSERT_REFS_COUNT(result, 2, "references x dans G sans decl -> 2 occ (usages seuls)");
    ASSERT_REFS_EXCLUDES_LINE(result, 7, "references x dans G sans decl -> pas la declaration (ligne 7)");
    json_free(result);
    json_free(params);
}

/* Cas 3 — parametre de procedure. `p` declare en en-tete, utilise deux
 * fois dans le corps. references depuis la declaration (en-tete) doit
 * remonter les 3 occurrences (decl + 2 usages).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE Calc(p est un entier)   <- decl, 'p' char 15
 *   ligne 1 : \tRENVOYER p * p + 1               <- 2 usages, char 10 et 14
 *   ligne 2 : FIN
 */
static void test_references_scope_parameter(void) {
    printf("\n[TEST] feat/1740 L1-C3 : references sur parametre p -> 3 occ (decl + 2 usages) (RED attendu)\n");
    const char* src =
        "PROCEDURE Calc(p est un entier)\n"  /* ligne 0 — 'p' decl a char 15 */
        "\tRENVOYER p * p + 1\n"             /* ligne 1 — 'p' usages char 10 et 14 */
        "FIN\n";                              /* ligne 2 */
    setup_doc(URI, src);
    JsonValue* params = make_references_params(URI, 0, 15, true);
    JsonValue* result = handle_references(params);
    ASSERT_REFS_COUNT(result, 3, "references parametre p -> 3 occ (decl + 2 usages corps)");
    json_free(result);
    json_free(params);
}

/* Cas 4 — globale top-level utilisee dans plusieurs procedures du meme
 * document. references depuis la declaration doit remonter toutes les
 * occurrences (decl + usages dans les 2 procedures). Cross-fichier hors
 * perimetre de ce lot (a preciser au Lot 2, cf.  question 6).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : compteur est un entier = 0                  <- decl
 *   ligne 1 : (vide)
 *   ligne 2 : PROCEDURE IncrementeCompteur()
 *   ligne 3 : \tcompteur = compteur + 1                    <- 2 usages
 *   ligne 4 : FIN
 *   ligne 5 : (vide)
 *   ligne 6 : PROCEDURE AfficheCompteur()
 *   ligne 7 : \tTrace(compteur)                            <- 1 usage
 *   ligne 8 : FIN
 */
static void test_references_scope_global_multi_procedure(void) {
    printf("\n[TEST] feat/1740 L1-C4 : references sur globale compteur -> 4 occ (decl + 3 usages) (RED attendu)\n");
    const char* src =
        "compteur est un entier = 0\n"          /* ligne 0 — decl */
        "\n"                                     /* ligne 1 */
        "PROCEDURE IncrementeCompteur()\n"      /* ligne 2 */
        "\tcompteur = compteur + 1\n"           /* ligne 3 — 2 usages */
        "FIN\n"                                  /* ligne 4 */
        "\n"                                     /* ligne 5 */
        "PROCEDURE AfficheCompteur()\n"         /* ligne 6 */
        "\tTrace(compteur)\n"                   /* ligne 7 — 1 usage */
        "FIN\n";                                 /* ligne 8 */
    setup_doc(URI, src);
    JsonValue* params = make_references_params(URI, 0, 0, true);
    JsonValue* result = handle_references(params);
    ASSERT_REFS_COUNT(result, 4, "references globale compteur -> 4 occ (decl + 3 usages)");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   Lot 2a — textDocument/rename + textDocument/prepareRename
 *  scope-aware, limite aux locales et parametres (portee
 *  PROC, mono-fichier). Les stubs (handle_rename / handle_prepare_rename)
 *  renvoient encore null systematiquement -> ROUGE attendu sur les cas
 *  qui exigent un resultat non-null (C1-C4). Le cas de refus natif (C6)
 *  est VERT d'emblee (le stub null matche deja le refus attendu) -- verrou
 *  de non-regression, pas un verrou TDD. Le cas C5 (globale) a ete
 *  RECADRE au Lot 2b (cf. `test_prepare_rename_allows_global_xfile` plus
 *  bas) : la globale devient renommable, ce n'est plus un refus.
 * ================================================================ */

/* Cas 1 (verrou central) — deux procedures F et G declarent chacune leur
 * PROPRE locale `x` (portees sœurs, legal). rename sur l'occurrence de
 * `x` DANS G avec newName="y" doit renvoyer un WorkspaceEdit avec un
 * TextEdit par occurrence de G (decl + 2 usages = 3), newText="y", et
 * AUCUN edit sur les lignes de F.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE F()
 *   ligne 1 : \tx est un entier      <- decl F
 *   ligne 2 : \tx = 1                <- usage F
 *   ligne 3 : \tx = 2                <- usage F
 *   ligne 4 : FIN
 *   ligne 5 : (vide)
 *   ligne 6 : PROCEDURE G()
 *   ligne 7 : \tx est un entier      <- decl G
 *   ligne 8 : \tx = 10               <- usage G
 *   ligne 9 : \tTrace(x)             <- usage G, curseur ici char 7
 *   ligne 10: FIN
 */
static void test_rename_scope_sibling_locals_isolated(void) {
    printf("\n[TEST] feat/1740 L2a-C1 : rename locale x dans G -> 3 edits, sans fuite vers F (RED attendu)\n");
    const char* src =
        "PROCEDURE F()\n"      /* ligne 0 */
        "\tx est un entier\n"  /* ligne 1 — decl F */
        "\tx = 1\n"            /* ligne 2 — usage F */
        "\tx = 2\n"            /* ligne 3 — usage F */
        "FIN\n"                /* ligne 4 */
        "\n"                   /* ligne 5 */
        "PROCEDURE G()\n"      /* ligne 6 */
        "\tx est un entier\n"  /* ligne 7 — decl G */
        "\tx = 10\n"           /* ligne 8 — usage G */
        "\tTrace(x)\n"         /* ligne 9 — usage G, char 7 */
        "FIN\n";               /* ligne 10 */
    setup_doc(URI, src);
    JsonValue* params = make_rename_params(URI, 9, 7, "y");
    JsonValue* result = handle_rename(params);
    ASSERT_RENAME_EDIT_COUNT(result, URI, 3, "rename x dans G -> 3 edits (decl + 2 usages)");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 1, "rename x dans G -> pas la decl de F (ligne 1)");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 2, "rename x dans G -> pas l'usage de F (ligne 2)");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 3, "rename x dans G -> pas l'usage de F (ligne 3)");
    ASSERT_RENAME_NEWTEXT_ALL(result, URI, "y", "rename x dans G -> newText verbatim \"y\"");
    json_free(result);
    json_free(params);
}

/* Cas 2 — parametre de procedure. `p` declare en en-tete, utilise deux
 * fois dans le corps. rename depuis un usage doit editer les 3 occurrences
 * (decl + 2 usages).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE Calc(p est un entier)   <- decl, 'p' char 15
 *   ligne 1 : \tRENVOYER p * p + 1               <- 2 usages, char 10 et 14, curseur ici
 *   ligne 2 : FIN
 */
static void test_rename_scope_parameter(void) {
    printf("\n[TEST] feat/1740 L2a-C2 : rename parametre p -> 3 edits (decl + 2 usages) (RED attendu)\n");
    const char* src =
        "PROCEDURE Calc(p est un entier)\n"  /* ligne 0 — 'p' decl a char 15 */
        "\tRENVOYER p * p + 1\n"             /* ligne 1 — 'p' usages char 10 et 14 */
        "FIN\n";                              /* ligne 2 */
    setup_doc(URI, src);
    JsonValue* params = make_rename_params(URI, 1, 10, "q");
    JsonValue* result = handle_rename(params);
    ASSERT_RENAME_EDIT_COUNT(result, URI, 3, "rename parametre p -> 3 edits (decl + 2 usages corps)");
    ASSERT_RENAME_NEWTEXT_ALL(result, URI, "q", "rename parametre p -> newText verbatim \"q\"");
    json_free(result);
    json_free(params);
}

/* Cas 3 — verbatim : newName avec une casse "non canon" (mix majuscule/
 * minuscule qui ne suit pas la convention camelCase du registre). Le
 * newText de chaque edit doit etre EXACTEMENT la chaine demandee, sans
 * reformatage (le formatter s'en charge au save, pas le rename).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE Calc()
 *   ligne 1 : \tcompteur est un entier   <- decl, curseur ici char 1
 *   ligne 2 : \tcompteur = 1
 *   ligne 3 : FIN
 */
static void test_rename_verbatim_no_canon(void) {
    printf("\n[TEST] feat/1740 L2a-C3 : rename verbatim, casse non-canon preservee (RED attendu)\n");
    const char* src =
        "PROCEDURE Calc()\n"      /* ligne 0 */
        "\tcompteur est un entier\n"  /* ligne 1 — decl, char 1 */
        "\tcompteur = 1\n"        /* ligne 2 — usage */
        "FIN\n";                   /* ligne 3 */
    setup_doc(URI, src);
    JsonValue* params = make_rename_params(URI, 1, 1, "moncompteur");
    JsonValue* result = handle_rename(params);
    ASSERT_RENAME_EDIT_COUNT(result, URI, 2, "rename verbatim -> 2 edits (decl + usage)");
    ASSERT_RENAME_NEWTEXT_ALL(result, URI, "moncompteur",
                               "rename verbatim -> newText exact \"moncompteur\" (pas de canonicalisation)");
    json_free(result);
    json_free(params);
}

/* Cas 4 — prepareRename autorise une locale : sur une locale, renvoie
 * un Range (le mot), non null.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : PROCEDURE Calc()
 *   ligne 1 : \tcompteur est un entier   <- curseur ici, char 1
 *   ligne 2 : FIN
 */
static void test_prepare_rename_allows_local(void) {
    printf("\n[TEST] feat/1740 L2a-C4 : prepareRename sur locale -> Range non-null (RED attendu)\n");
    const char* src =
        "PROCEDURE Calc()\n"          /* ligne 0 */
        "\tcompteur est un entier\n"  /* ligne 1 — char 1 */
        "FIN\n";                       /* ligne 2 */
    setup_doc(URI, src);
    JsonValue* params = make_prepare_rename_params(URI, 1, 1);
    JsonValue* result = handle_prepare_rename(params);
    ASSERT_PREPARE_RENAME_NOT_NULL(result, "prepareRename locale compteur -> Range");
    json_free(result);
    json_free(params);
}

/* Cas 5 — prepareRename AUTORISE desormais une globale (Lot 2b, decision
 * dev "globales incluses"). Au Lot 2a la portee top-level etait refusee
 * (comportement temporaire documente dans ce meme fichier) ; au Lot 2b
 * une globale devient renommable au meme titre qu'une locale/parametre,
 * grace a  (masquage cross-scope interdit -> une globale VALUE
 * est unique projet-wide). Ce test remplace l'ancien
 * `test_prepare_rename_refuses_global` (Lot 2a) : c'est un changement de
 * comportement inter-lots planifie et dev-approuve, pas un adoucissement
 * d'assertion. RED attendu tant que le Lot 2b n'est pas implemente (le
 * handler renvoie encore null pour KIND_TOPLEVEL).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : compteur est un entier = 0   <- curseur ici, char 0
 */
static void test_prepare_rename_allows_global_xfile(void) {
    printf("\n[TEST] feat/1740 L2b-C2 : prepareRename sur globale -> Range non-null (RED attendu, refus Lot 2a leve)\n");
    const char* src = "compteur est un entier = 0\n"; /* ligne 0 */
    setup_doc(URI, src);
    JsonValue* params = make_prepare_rename_params(URI, 0, 0);
    JsonValue* result = handle_prepare_rename(params);
    ASSERT_PREPARE_RENAME_NOT_NULL(result, "prepareRename globale compteur -> Range (Lot 2b autorise)");
    json_free(result);
    json_free(params);
}

/* Cas 6 — prepareRename refuse une native/mot-clé reserve dynamique
 * : curseur sur un appel de native -> null. VERT D'EMBLEE
 * avec le stub : verrou de non-regression du refus.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : Trace("hello")   <- curseur sur 'Trace', char 0
 */
static void test_prepare_rename_refuses_native(void) {
    printf("\n[TEST] feat/1740 L2a-C6 : prepareRename sur native Trace -> null (VERT D'EMBLEE, refus)\n");
    const char* src = "Trace(\"hello\")\n"; /* ligne 0 */
    setup_doc(URI, src);
    JsonValue* params = make_prepare_rename_params(URI, 0, 0);
    JsonValue* result = handle_prepare_rename(params);
    ASSERT_PREPARE_RENAME_NULL(result, "prepareRename native Trace -> null (refus)");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   Lot 2b — textDocument/rename etendu aux globales, PROJET
 *  ENTIER (, , ). Une globale (declaration
 *  KIND_TOPLEVEL) est unique projet-wide parmi les valeurs (masquage
 *  cross-scope interdit) -> toutes ses occurrences VALUE dans TOUS les
 *  fichiers du projet, ouverts ET fermes, sont le meme symbole. Le
 *  WorkspaceEdit doit donc couvrir les compagnons feres scannes sur
 *  disque, pas seulement le document ouvert. Aujourd'hui les handlers
 *  refusent encore toute globale (condition Lot 2a declScope.kind ==
 *  KIND_PROC) -> ROUGE attendu sur C1-C3. Pas de stub a ajouter, les
 *  handlers existent deja (cf. handle_rename / scope_rename_query).
 * ================================================================ */

/* Cas 1 (verrou central) — rename d'une globale cross-fichier. Fixture
 * disque xfile/ : projet.owl (OUVERT) declare et utilise
 * `gCompteur` ; le compagnon utils.owl (FERME, present sur disque
 * uniquement) l'utilise aussi. Le WorkspaceEdit attendu couvre les DEUX
 * URIs, avec newText="gTotal" verbatim partout.
 *
 * Fixture projet.owl (0-indexed) :
 *   ligne 0-2 : commentaires
 *   ligne 3   : DemoRenameGlobale est un Projet
 *   ligne 4   : \tPROPRIETES
 *   ligne 5   : \t\tLangues = [langueFrançais]
 *   ligne 6   : \tFIN
 *   ligne 7   : \tgCompteur est un entier = 0      <- decl, char 1
 *   ligne 8   : \tgCompteur = gCompteur + 1        <- 2 usages, char 1 et 13
 *   ligne 9   : FIN
 *
 * Fixture utils.owl (FERME, 0-indexed) :
 *   ligne 0-1 : commentaires
 *   ligne 2   : PROCEDURE AfficheCompteur()
 *   ligne 3   : \tTrace(gCompteur)                  <- 1 usage, char 7
 *   ligne 4   : FIN
 */
static void test_rename_global_cross_file(void) {
    printf("\n[TEST] feat/1740 L2b-C1 : rename globale gCompteur cross-fichier -> edits projet.owl + utils.owl (RED attendu)\n");

    char workspaceRoot[512], projetPath[512], utilsPath[512];
    char projetUri[600], utilsUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "xfile");
    resolve_fixture_path(projetPath, sizeof(projetPath), "xfile/projet.owl");
    resolve_fixture_path(utilsPath, sizeof(utilsPath), "xfile/utils.owl");
    fix1017_path_to_uri(projetPath, projetUri, sizeof(projetUri));
    fix1017_path_to_uri(utilsPath, utilsUri, sizeof(utilsUri));

    const char* projetSrc =
        "// Fixture LSP feat/1740 Lot 2b -- frontiere projet pour le rename cross-\n"
        "// fichier d'une globale. gCompteur est declaree et utilisee ici ; le\n"
        "// compagnon utils.owl (FERME) l'utilise aussi.\n"
        "DemoRenameGlobale est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = [langueFran\xc3\xa7" "ais]\n"
        "\tFIN\n"
        "\tgCompteur est un entier = 0\n"
        "\tgCompteur = gCompteur + 1\n"
        "FIN\n";
    setup_workspace_with_rootpath(workspaceRoot, projetUri, projetSrc);

    /* Curseur sur la declaration gCompteur (ligne 7, char 1). */
    JsonValue* params = make_rename_params(projetUri, 7, 1, "gTotal");
    JsonValue* result = handle_rename(params);

    ASSERT_RENAME_EDIT_COUNT(result, projetUri, 3,
        "rename gCompteur cross-fichier -> 3 edits dans projet.owl (decl + 2 usages)");
    ASSERT_RENAME_NEWTEXT_ALL(result, projetUri, "gTotal",
        "rename gCompteur cross-fichier -> newText verbatim \"gTotal\" (projet.owl)");
    ASSERT_RENAME_EDIT_COUNT(result, utilsUri, 1,
        "rename gCompteur cross-fichier -> 1 edit dans utils.owl FERME");
    ASSERT_RENAME_NEWTEXT_ALL(result, utilsUri, "gTotal",
        "rename gCompteur cross-fichier -> newText verbatim \"gTotal\" (utils.owl ferme)");

    json_free(result);
    json_free(params);
}

/* Cas 3 — isolation entre deux globales distinctes `gA` et `gB` (canonKey
 * different). Rename de `gA` ne doit editer AUCUNE occurrence de `gB`.
 * Mono-fichier ouvert (l'isolation par canonKey ne depend pas du
 * cross-fichier, verrou local suffisant).
 *
 * Fixture (0-indexed) :
 *   ligne 0 : gA est un entier = 1
 *   ligne 1 : gB est un entier = 2
 *   ligne 2 : (vide)
 *   ligne 3 : PROCEDURE UseA()
 *   ligne 4 : \tgA = gA + 1        <- 2 usages gA, char 1 et 6
 *   ligne 5 : FIN
 *   ligne 6 : (vide)
 *   ligne 7 : PROCEDURE UseB()
 *   ligne 8 : \tgB = gB + 1        <- 2 usages gB, char 1 et 6, JAMAIS touche
 *   ligne 9 : FIN
 */
static void test_rename_global_isolation_distinct(void) {
    printf("\n[TEST] feat/1740 L2b-C3 : rename globale gA n'affecte pas gB (RED attendu)\n");
    const char* src =
        "gA est un entier = 1\n" /* ligne 0 — decl gA, char 0 */
        "gB est un entier = 2\n" /* ligne 1 — decl gB, char 0, NE DOIT PAS etre editee */
        "\n"                     /* ligne 2 */
        "PROCEDURE UseA()\n"     /* ligne 3 */
        "\tgA = gA + 1\n"        /* ligne 4 — 2 usages gA */
        "FIN\n"                  /* ligne 5 */
        "\n"                     /* ligne 6 */
        "PROCEDURE UseB()\n"     /* ligne 7 */
        "\tgB = gB + 1\n"        /* ligne 8 — 2 usages gB, JAMAIS touche */
        "FIN\n";                 /* ligne 9 */
    setup_doc(URI, src);
    JsonValue* params = make_rename_params(URI, 0, 0, "gAlpha");
    JsonValue* result = handle_rename(params);
    ASSERT_RENAME_EDIT_COUNT(result, URI, 3,
        "rename gA -> 3 edits (decl + 2 usages), gB non touchee");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 1, "rename gA -> pas la decl de gB (ligne 1)");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 8, "rename gA -> pas les usages de gB (ligne 8)");
    ASSERT_RENAME_NEWTEXT_ALL(result, URI, "gAlpha", "rename gA -> newText verbatim \"gAlpha\"");
    json_free(result);
    json_free(params);
}

/* Cas 4 (sanity, optionnel) — une locale `autre` (nom distinct de la
 * globale renommee) dans la meme procedure ne doit jamais etre touchee
 * par le rename de la globale.
 *
 * Fixture (0-indexed) :
 *   ligne 0 : gCompteur est un entier = 0        <- decl, curseur ici char 0
 *   ligne 1 : (vide)
 *   ligne 2 : PROCEDURE Calc()
 *   ligne 3 : \tautre est un entier               <- decl locale, JAMAIS touchee
 *   ligne 4 : \tautre = 5                          <- usage locale, JAMAIS touchee
 *   ligne 5 : \tgCompteur = gCompteur + autre     <- 2 usages gCompteur
 *   ligne 6 : FIN
 */
static void test_rename_global_does_not_affect_local(void) {
    printf("\n[TEST] feat/1740 L2b-C4 : rename globale gCompteur n'affecte pas la locale 'autre' (RED attendu)\n");
    const char* src =
        "gCompteur est un entier = 0\n"          /* ligne 0 — decl, char 0 */
        "\n"                                      /* ligne 1 */
        "PROCEDURE Calc()\n"                     /* ligne 2 */
        "\tautre est un entier\n"                 /* ligne 3 — locale, jamais touchee */
        "\tautre = 5\n"                           /* ligne 4 — locale, jamais touchee */
        "\tgCompteur = gCompteur + autre\n"      /* ligne 5 — 2 usages gCompteur */
        "FIN\n";                                  /* ligne 6 */
    setup_doc(URI, src);
    JsonValue* params = make_rename_params(URI, 0, 0, "gTotal");
    JsonValue* result = handle_rename(params);
    ASSERT_RENAME_EDIT_COUNT(result, URI, 3,
        "rename gCompteur -> 3 edits (decl + 2 usages), locale 'autre' intacte");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 3, "rename gCompteur -> pas la decl de 'autre' (ligne 3)");
    ASSERT_RENAME_EXCLUDES_LINE(result, URI, 4, "rename gCompteur -> pas l'usage de 'autre' (ligne 4)");
    ASSERT_RENAME_NEWTEXT_ALL(result, URI, "gTotal", "rename gCompteur -> newText verbatim \"gTotal\"");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   — controles prefixes `Champ` indexes cross-fichier
 * ================================================================ */

/* Le langage impose le prefixe `Champ` sur les declarations widget
 * (`LIB_1 est un Champ Libellé`). `Champ` est un IDENTIFIER (pas un
 * keyword scanner) qui s'intercale entre `est` et le type widget.
 *
 * GUARD : un controle prefixe Champ declare dans un fichier
 * VOISIN (scanne depuis le disque, non ouvert) DOIT etre indexe dans
 * userControls[] et propose en completion d'un autre fichier du meme
 * projet. C'est le chemin cross-fichier de
 * scan_one_source_for_user_symbols (docIndex=-1).
 *
 * Note : ce chemin reconnait deja le prefixe Champ via la branche
 * `IDENT est [un Champ] <Type>` (sawChampPrefix), independante de la
 * mini-state-machine prev2/prev1/tok. Le test verrouille cette
 * couverture cross-fichier — VERT attendu, garde-fou contre une
 * regression du support Champ dans le scan disque. */
static void test_cross_file_champ_prefixed_controls(void) {
    printf("\n[TEST] fix/0647 : controles prefixes Champ cross-fichier (GUARD vert)\n");

    char workspaceRoot[512];
    char consumerPath[512];
    char consumerUri[600];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot), "workspace_completion_controles");
    resolve_fixture_path(consumerPath, sizeof(consumerPath),
                         "workspace_completion_controles/views/autre.owl");

    snprintf(consumerUri, sizeof(consumerUri), "file:///%s", consumerPath);
    for (char* p = consumerUri + 8; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    /* Consommateur ouvert : une ligne avec un prefixe partiel pour
     * declencher la completion des noms de controles (toplevel, hors
     * string/dot). Le voisin views/fenetre.owl declare LIB_Titre,
     * BTN_Valider, SAI_Nom en `est un Champ <Type>`. */
    const char* consumerSrc =
        "// Consommateur cross-file (fix/0647)\n"
        "x = \n";

    setup_workspace_with_rootpath(workspaceRoot, consumerUri, consumerSrc);

    /* Completion sur la ligne 1 (apres 'x = '), col 4 : contexte expression
     * top-level ou les noms de controles sont proposes (cf.
     * test_names_in_top_level). Le voisin views/fenetre.owl declare les 3
     * controles en `est un Champ <Type>`. On verifie l'INDEXATION
     * cross-fichier, pas le filtrage prefixe. */
    JsonValue* params = make_completion_params(consumerUri, 1, 4);
    JsonValue* items = handle_completion(params);

    ASSERT_HAS(items, "LIB_Titre",
               "cross-file Champ : Libellé prefixe Champ propose");
    ASSERT_HAS(items, "BTN_Valider",
               "cross-file Champ : Bouton prefixe Champ propose");
    ASSERT_HAS(items, "SAI_Nom",
               "cross-file Champ : Saisie prefixe Champ propose");

    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — bloc Projet : completion champs + langues, hover champ
 *  (RED attendus avant impl : le type Projet n'est PAS un widget/kind
 *  connu de DSL_KIND_CHARS, donc PROPRIETES sous 'APP est un Projet'
 *  ne route vers aucune logique dediee et tombe dans le fallback
 *  generique — natives/globales, pas les champs du catalogue Projet).
 * ================================================================ */

/* Completion des champs dans le bloc PROPRIETES d'un Projet : doit
 * proposer Langues/Description/Auteur/Licence (catalogue du type
 * Projet), pas les natives ni les constantes couleur. */
static void test_completion_projet_fields_proposes_catalog(void) {
    printf("\n[TEST] feat/0887 : bloc PROPRIETES Projet propose Langues/Description/Auteur/Licence\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Langues",     "PROPRIETES Projet");
    ASSERT_HAS    (items, "Description", "PROPRIETES Projet");
    ASSERT_HAS    (items, "Auteur",      "PROPRIETES Projet");
    ASSERT_HAS    (items, "Licence",     "PROPRIETES Projet");
    ASSERT_NOT_HAS(items, "Trace",       "PROPRIETES Projet (pas de native)");
    ASSERT_NOT_HAS(items, "Titre",       "PROPRIETES Projet (pas une prop Fenetre)");
    ASSERT_NOT_HAS(items, "RougeClair",  "PROPRIETES Projet (pas de constante)");
    json_free(items);
    json_free(params);
}

/* Filtrage des champs deja presents (parite widgets, cf.
 * test_filter_already_used_props) : Langues deja pose ne doit pas etre
 * re-propose, les 3 autres champs restent proposes. */
static void test_completion_projet_fields_filters_used(void) {
    printf("\n[TEST] feat/0887 : bloc PROPRIETES Projet filtre Langues deja pose\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFran\xc3\xa7" "ais\n"
        "\t\t\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 3, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Langues",     "PROPRIETES Projet filtre Langues");
    ASSERT_HAS    (items, "Description", "PROPRIETES Projet filtre (reste)");
    ASSERT_HAS    (items, "Auteur",      "PROPRIETES Projet filtre (reste)");
    ASSERT_HAS    (items, "Licence",     "PROPRIETES Projet filtre (reste)");
    json_free(items);
    json_free(params);
}

/* 'Langues = ' (scalaire) dans le bloc Projet propose les constantes
 * langueXxx officielles, pas les noms de champs du catalogue Projet. */
static void test_completion_projet_langue_scalar(void) {
    printf("\n[TEST] feat/0887 : 'Langues = ' (scalaire) propose les constantes langueXxx\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 12);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "langueFran\xc3\xa7" "ais", "Langues= scalaire (langueXxx)");
    ASSERT_HAS    (items, "langueAnglais",            "Langues= scalaire (langueXxx)");
    ASSERT_NOT_HAS(items, "Description",              "Langues= scalaire (pas de nom de champ)");
    ASSERT_NOT_HAS(items, "Auteur",                   "Langues= scalaire (pas de nom de champ)");
    json_free(items);
    json_free(params);
}

/* 'Langues = [ ' (forme liste) propose aussi les constantes langueXxx.
 * Cas explicitement marque optionnel/impl-dependant par le ticket : si
 * la forme liste diverge trop, le cas scalaire ci-dessus suffit a
 * couvrir le contrat minimal. */
static void test_completion_projet_langue_list_bracket(void) {
    printf("\n[TEST] feat/0887 : 'Langues = [' (liste) propose les constantes langueXxx\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = [\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 2, 13);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "langueFran\xc3\xa7" "ais", "Langues= liste (langueXxx)");
    ASSERT_HAS(items, "langueAnglais",            "Langues= liste (langueXxx)");
    json_free(items);
    json_free(params);
}

/*  — `Thème = <curseur>` dans un bloc Projet doit basculer en
 * completion de VALEUR typee : proposer les instances `est un Thème` du
 * projet, PAS des noms de propriete (bascule valeur, cf. Langues). */
static void test_completion_projet_theme_proposes_instances(void) {
    printf("\n[TEST] fix/1021 : 'Th\xc3\xa8me = ' propose les instances `est un Th\xc3\xa8me`\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tTh\xc3\xa8me = \n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "FIN\n"
        "FEN_Accueil est une Fen\xc3\xaatre\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 2, apres `\t\tThème = ` : 2 tabs + 8 unites UTF-16 = 10. */
    JsonValue* params = make_completion_params(URI, 2, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Th\xc3\xa8meD\xc3\xa9mo", "Theme= (instance de theme)");
    ASSERT_NOT_HAS(items, "Description",              "Theme= (pas de nom de champ)");
    ASSERT_NOT_HAS(items, "Langues",                  "Theme= (pas de nom de champ)");
    ASSERT_NOT_HAS(items, "Auteur",                   "Theme= (pas de nom de champ)");
    json_free(items);
    json_free(params);
}

/*  — `FenêtreDémarrage = <curseur>` propose les instances
 * `est une Fenêtre` du projet, pas des noms de propriete. */
static void test_completion_projet_fenetre_demarrage_proposes_instances(void) {
    printf("\n[TEST] fix/1021 : 'Fen\xc3\xaatreD\xc3\xa9marrage = ' propose les instances `est une Fen\xc3\xaatre`\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tFen\xc3\xaatreD\xc3\xa9marrage = \n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "FEN_Accueil est une Fen\xc3\xaatre\n"
        "FIN\n"
        "Th\xc3\xa8meD\xc3\xa9mo est un Th\xc3\xa8me\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 2 : 2 tabs + "FenêtreDémarrage = " = 16 + 3 = 19 unites UTF-16 -> 21. */
    JsonValue* params = make_completion_params(URI, 2, 21);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "FEN_Accueil", "FenetreDemarrage= (instance de fenetre)");
    ASSERT_NOT_HAS(items, "Description", "FenetreDemarrage= (pas de nom de champ)");
    ASSERT_NOT_HAS(items, "Langues",     "FenetreDemarrage= (pas de nom de champ)");
    json_free(items);
    json_free(params);
}

/*  — champ scalaire chaine (`Description = `) : au minimum ne plus
 * proposer de noms de propriete a tort (bascule valeur coherente). */
static void test_completion_projet_description_no_field_names(void) {
    printf("\n[TEST] fix/1021 : 'Description = ' ne propose plus de noms de champ Projet\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tDescription = \n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 2 : 2 tabs + "Description = " = 14 -> 16. */
    JsonValue* params = make_completion_params(URI, 2, 16);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "Auteur",  "Description= (pas de nom de champ)");
    ASSERT_NOT_HAS(items, "Langues", "Description= (pas de nom de champ)");
    ASSERT_NOT_HAS(items, "Licence", "Description= (pas de nom de champ)");
    json_free(items);
    json_free(params);
}

/* Hover sur le champ 'Langues' dans le bloc Projet : parite avec le
 * hover des props widget (, cf. test_hover_dsl_prop_fr_default). */
static void test_hover_projet_field_langues(void) {
    printf("\n[TEST] feat/0887 : hover 'Langues' dans bloc Projet -> desc du champ\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFran\xc3\xa7" "ais\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_hover_params(URI, 2, 4);  /* sur 'Langues' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "Langues",              "hover champ Projet titre");
    ASSERT_HOVER_CONTAINS(v, "langue par d\xc3\xa9" "faut", "hover champ Projet desc");
    json_free(result);
    json_free(params);
}

/* Hover sur le champ 'Description' dans le bloc Projet. */
static void test_hover_projet_field_description(void) {
    printf("\n[TEST] feat/0887 : hover 'Description' dans bloc Projet -> desc du champ\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tDescription = \"Mon application\"\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_hover_params(URI, 2, 4);  /* sur 'Description' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS(v, "Description",          "hover champ Projet Description titre");
    ASSERT_HOVER_CONTAINS(v, "Description courte du projet", "hover champ Projet Description desc");
    json_free(result);
    json_free(params);
}

/* ================================================================
 *   volet B -- Fermeture comme event de cycle de vie du
 *  PROJET (teardown), pas seulement Fenetre/Etat. Fermeture() est
 *  declaree en sibling top-level du bloc `IDENT est un Projet`, pas
 *  a l'interieur du bloc (contrairement aux events Fenetre/Etat qui
 *  vivent DANS leur bloc). Le contexte DSL 'J' ne couvre
 *  que l'interieur du bloc Projet (PROPRIETES) : au top-level APRES
 *  la FIN du bloc, isWidgetCtx est faux et la completion/hover ne
 *  distingue pas "ce document est un projet.owl" -- RED attendus
 *  avant impl.
 * ================================================================ */

/* Completion : 'PROCEDURE ' au top-level d'un document contenant un
 * bloc Projet doit proposer 'Fermeture' comme event de teardown du
 * projet (parite avec la proposition de FinInitialisation/Fermeture
 * dans un body Fenetre). Reste minimal : pas de catalogue Fenetre qui
 * fuite (FinInitialisation/AutorisationFermeture/DemandeMiseAJour ne
 * sont PAS des events projet, ne doivent pas apparaitre ici). */
static void test_completion_projet_toplevel_proposes_fermeture(void) {
    printf("\n[TEST] feat/0898 : 'PROCEDURE ' au top-level d'un projet.owl propose Fermeture\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFran\xc3\xa7" "ais\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Fermeture",             "completion top-level projet (Fermeture)");
    ASSERT_NOT_HAS(items, "FinInitialisation",     "completion top-level projet (pas le catalogue Fenetre)");
    ASSERT_NOT_HAS(items, "AutorisationFermeture", "completion top-level projet (pas le catalogue Fenetre)");
    ASSERT_NOT_HAS(items, "DemandeMiseAJour",      "completion top-level projet (pas le catalogue Fenetre)");
    json_free(items);
    json_free(params);
}

/*  — reconnaissance du bloc Projet avec copule MIXTE `is un Projet`
 * (copule EN + article FR + nom FR du type). RED avant fix : le sniff ne
 * matchait que `est un Projet` et `is a Project`, donc `is un Projet` ne
 * declenchait pas la proposition des events de cycle de vie du projet. */
static void test_completion_projet_toplevel_copule_mixte(void) {
    printf("\n[TEST] fix/1039 : 'PROCEDURE ' top-level projet copule mixte `is un Projet` propose Fermeture\n");
    const char* src =
        "APP is un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFran\xc3\xa7" "ais\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE \n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 6, 10);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "Fermeture",         "completion top-level projet mixte (Fermeture)");
    ASSERT_NOT_HAS(items, "FinInitialisation", "completion top-level projet mixte (pas catalogue Fenetre)");
    json_free(items);
    json_free(params);
}

/* Hover : 'Fermeture' declaree au top-level d'un document contenant un
 * bloc Projet doit expliquer le teardown du PROJET (mot "projet" dans
 * la desc), pas reutiliser par defaut la desc Fenetre ("ne se ferme")
 * ni la desc Etat ("rapport") -- ces deux dernieres n'ont pas de sens
 * pour un fichier projet.owl. */
static void test_hover_fermeture_projet_toplevel(void) {
    printf("\n[TEST] feat/0898 : hover Fermeture top-level d'un projet.owl -> desc teardown projet\n");
    const char* src =
        "APP est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = langueFran\xc3\xa7" "ais\n"
        "\tFIN\n"
        "FIN\n"
        "\n"
        "PROCEDURE Fermeture()\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_hover_params(URI, 6, 12);  /* sur 'Fermeture' */
    JsonValue* result = handle_hover(params);
    const char* v = hover_value(result);
    ASSERT_HOVER_CONTAINS    (v, "projet",      "hover Fermeture top-level projet (desc teardown)");
    ASSERT_HOVER_NOT_CONTAINS(v, "ne se ferme", "hover Fermeture top-level projet (pas desc fenetre)");
    ASSERT_HOVER_NOT_CONTAINS(v, "rapport",     "hover Fermeture top-level projet (pas desc etat)");
    json_free(result);
    json_free(params);
}

/*  -- declarations anticipees (fonctions definies apres main). */
static void test_init_document_symbol_provider(void);
static void test_init_folding_range_provider(void);
static void test_doc_symbol_fenetre_vide(void);
static void test_doc_symbol_un_champ_sans_event(void);
static void test_doc_symbol_champ_avec_event(void);
static void test_doc_symbol_event_fenetre(void);
static void test_doc_symbol_complet(void);
/*  -- commentaires blocs neutralises avant split_lines. */
static void test_fix1281_block_comment_fake_end_tronque(void);
static void test_fix1281_block_comment_mono_ligne(void);
static void test_fix1281_code_apres_fermeture_meme_ligne(void);
static void test_fix1281_non_regression_sans_bloc(void);
static void test_folding_fenetre_vide(void);
static void test_folding_un_champ(void);
static void test_folding_champ_avec_event(void);
static void test_folding_complet(void);
/*  Q1+Q2 -- SymbolKind + Declarations globales (RED attendus). */
static void test_doc_symbol_kinds(void);
static void test_doc_symbol_declarations_globales(void);
static void test_doc_symbol_no_decl_globales_vide(void);
static void test_doc_symbol_no_decl_globales_1champ(void);
static void test_doc_symbol_declarations_globales_window_en(void);
static void test_doc_symbol_declaration_sans_article(void);
/*  -- documentSymbol + foldingRange Etat et module/script (RED attendus). */
static void test_doc_symbol_etat_vide(void);
static void test_doc_symbol_etat_complet(void);
static void test_doc_symbol_etat_accent(void);
static void test_doc_symbol_etat_en(void);
static void test_doc_symbol_etat_kind(void);
static void test_folding_etat_vide(void);
static void test_folding_etat_complet(void);
static void test_doc_symbol_module_3procs(void);
static void test_doc_symbol_module_proc_prive(void);
static void test_doc_symbol_module_fonction(void);
static void test_doc_symbol_module_classe(void);
static void test_doc_symbol_module_classe_en(void);
static void test_doc_symbol_module_vide(void);
static void test_doc_symbol_module_kinds(void);
/*  -- fix FIN end.character (3 RED attendus). */
static void test_doc_symbol_fin_char_fenetre(void);
static void test_doc_symbol_fin_char_widget(void);
static void test_doc_symbol_fin_char_procedure(void);
/*  v2 -- Tests approche legere (diagnostics surs + robustesse scheme + lock cross-file). */
static void test_diag_no_spawn_on_nonfile_scheme(void); /* RED attendu */
static void test_completion_scheme_robustness(void);    /* VERT attendu -- lock comportemental */
static void test_cross_file_context_lock(void);         /* VERT attendu -- lock comportemental */
/*  -- sections d'etat (Corps/EnTeteDocument/...) comme conteneurs (RED avant impl). */
static void test_doc_symbol_etat_sections_hierarchy(void);
static void test_doc_symbol_etat_sections_ranges(void);
static void test_doc_symbol_etat_sections_not_in_decl_glob(void);
/*  -- documentSymbol enrichi (detail widget + script + projet, RED avant impl). */
static void test_doc_symbol_field_detail_widget_type(void);
static void test_doc_symbol_field_detail_en(void);
static void test_doc_symbol_script_vars(void);
static void test_doc_symbol_script_enum(void);
static void test_doc_symbol_script_constants(void);
static void test_doc_symbol_script_mixed_no_regress(void);
static void test_doc_symbol_projet(void);
static void test_doc_symbol_projet_en(void);

/*  -- documentSymbol : fichier Theme (racine + styles, RED avant impl). */
static void test_doc_symbol_theme(void);
static void test_doc_symbol_theme_en(void);

/*  -- documentSymbol : code d'initialisation top-level (RED avant impl). */
static void test_doc_symbol_module_init_fr(void);
static void test_doc_symbol_module_init_none(void);
static void test_doc_symbol_module_init_en(void);
static void test_doc_symbol_collection_init_fr(void);
static void test_doc_symbol_collection_init_none(void);
static void test_doc_symbol_collection_nested_struct(void);
static void test_doc_symbol_collection_large_no_drop(void);
static void test_doc_symbol_module_large_no_drop(void);

/*  — une globale top-level de fenetre est proposee en RHS d'une
 * prop a valeur libre (chaine) du bloc PROPRIETES d'un widget de la meme
 * fenetre. Le runtime la resout (, mangling FEN_X$var) ; la
 * completion doit la refleter. Avant  : `Contenu = <cursor>`
 * retombait sur la liste des NOMS de props (X/Y/Largeur...), absurde en RHS. */
static void test_window_global_offered_in_widget_prop_value(void) {
    printf("\n[TEST] fix/0970 : globale fenetre proposee en RHS de prop widget\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"Repro\"\n"
        "\tFIN\n"
        "\tgGlobaleFenetre est une chaine = \"coucou\"\n"
        "\tCOMBO_A est un Champ Combo\n"
        "\t\tPROPRIETES\n"
        "\t\t\tContenu = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 7 (0-based) : "\t\t\tContenu = " — curseur en fin de ligne. */
    int col = (int)strlen("\t\t\tContenu = ");
    JsonValue* params = make_completion_params(URI, 7, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "gGlobaleFenetre",
               "globale fenetre en RHS Contenu");
    /* Pas de pollution par les noms de props en RHS. */
    ASSERT_NOT_HAS(items, "Largeur", "pas de nom de prop en RHS Contenu");
    ASSERT_NOT_HAS(items, "X",       "pas de nom de prop en RHS Contenu");
    json_free(items);
    json_free(params);
}

/*  — scope strict : la globale de la fenetre englobante est offerte,
 * mais PAS une var locale d'un widget frere (isGlobal=Faux) ni la globale
 * d'une AUTRE fenetre (faux candidat : le runtime la mangle sous une autre
 * fenetre et echouerait a la resoudre). */
static void test_window_global_prop_value_scope_strict(void) {
    printf("\n[TEST] fix/0970 : scope strict globales fenetre en RHS de prop\n");
    const char* src =
        "FEN_Test est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"Repro\"\n"
        "\tFIN\n"
        "\tgGlobaleFenetre est une chaine = \"a\"\n"
        "\tCOMBO_A est un Champ Combo\n"
        "\t\tgLocaleWidget est une chaine = \"b\"\n"
        "\t\tPROPRIETES\n"
        "\t\t\tContenu = \n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n"
        "FEN_Autre est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"Autre\"\n"
        "\tFIN\n"
        "\tgGlobaleAutre est une chaine = \"c\"\n"
        "FIN\n";
    setup_doc(URI, src);
    /* Ligne 8 (0-based) : le `Contenu = ` du COMBO_A de FEN_Test. */
    int col = (int)strlen("\t\t\tContenu = ");
    JsonValue* params = make_completion_params(URI, 8, col);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS    (items, "gGlobaleFenetre", "globale de la fenetre englobante");
    ASSERT_NOT_HAS(items, "gLocaleWidget",   "pas la var locale d'un widget frere");
    ASSERT_NOT_HAS(items, "gGlobaleAutre",   "pas la globale d'une autre fenetre");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   -- completion : "commence par" d'abord PUIS "contient"
 *  dans le regime de pre-filtrage serveur (> 300 candidats).
 *
 *  Contexte : lsp_handlers.c applique un filtre server-side quand le
 *  pool d'items depasse 300 (natives + mots-cles + types + constantes
 *  + symboles utilisateur). Ce filtre ne garde aujourd'hui QUE les
 *  correspondances par PREFIXE sur le label -> une sous-chaine interne
 *  (ex: taper "Quote" sur une variable "gprasNeOwlQuote") est jetee.
 *  Cible post-impl :
 *    1. Les correspondances "contient" sont conservees.
 *    2. Les "commence par" sont classees AVANT via sortText.
 *    3. Le filtre lit aussi filterText (alias EN/ASCII), pas seulement
 *       label.
 *    4. Garde-fou : a 1 caractere tape, le regime reste prefixe pur.
 * ================================================================ */

/* Test 1 -- sous-chaine conservee en regime > 300.
 * "gprasNeOwlQuote" CONTIENT "Quote" mais ne COMMENCE PAS par "Quote".
 * On prouve d'abord que le pool depasse bien 300 (probe sans prefixe,
 * qui echappe au filtre car prefixLen==0 -> pas de reduction), puis on
 * verifie le comportement filtre reel sur le fragment "Quote".
 * RED attendu : le filtre actuel (prefixe strict sur label) jette
 * "gprasNeOwlQuote" puisqu'il ne COMMENCE PAS par "Quote". */
static void test_completion_over300_keeps_substring_match(void) {
    printf("\n[TEST] feat/0971 : regime >300, 'Quote' garde 'gprasNeOwlQuote' (contient, pas prefixe)\n");
    const char* src =
        "gprasNeOwlQuote est un entier = 5\n"
        "\n"
        "Quote\n";
    setup_doc(URI, src);

    /* Probe : ligne 1 (vide), col 0 -> aucun prefixe tape, le filtre
     * server-side (prefixLen>0 requis) ne s'applique PAS -> on recupere
     * le pool BRUT (natives+keywords+types+consts+symboles). Sert de
     * preuve que le regime >300 est reellement atteint dans ce document. */
    JsonValue* probeParams = make_completion_params(URI, 1, 0);
    JsonValue* probeItems = handle_completion(probeParams);
    int poolSize = count_items(probeItems);
    if (poolSize > 300) {
        g_pass++;
        printf("  [OK]   feat/0971 : pool brut = %d items (> 300, regime filtre garanti actif)\n", poolSize);
    } else {
        g_fail++;
        printf("  [FAIL] feat/0971 : pool brut = %d items (attendu > 300 -- le test ne prouve rien si <= 300)\n", poolSize);
    }
    json_free(probeItems);
    json_free(probeParams);

    /* Cas reel : ligne 2, curseur apres "Quote" (col 5) -> fragment tape
     * de 5 caracteres, le filtre server-side s'applique (pool > 300). */
    JsonValue* params = make_completion_params(URI, 2, 5);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "gprasNeOwlQuote",
               "feat/0971 : variable contenant 'Quote' (pas prefixe) presente");
    json_free(items);
    json_free(params);
}

/* Test 2 -- ordre prefix-first : parmi les natives reelles du catalogue,
 * "DateVersChaîne" COMMENCE PAR le fragment "Date" tape ; "ChaîneVersDate"
 * CONTIENT "Date" en milieu de nom sans commencer par. Les deux doivent
 * etre presentes (contains conserve, cf test 1) ET la premiere doit etre
 * classee avant la seconde via sortText.
 * RED attendu : aucun sortText n'est pose aujourd'hui sur ces items (NULL
 * des deux cotes) -> l'assertion d'ordre echoue. */
static void test_completion_over300_prefix_sorted_before_contains(void) {
    printf("\n[TEST] feat/0971 : regime >300, ordre 'commence par' avant 'contient'\n");
    const char* src = "Date\n";
    setup_doc(URI, src);
    /* Curseur juste apres "Date", col 4 -> fragment de 4 caracteres. */
    JsonValue* params = make_completion_params(URI, 0, 4);
    JsonValue* items = handle_completion(params);

    int poolNote = count_items(items);
    printf("  (info) items retournes apres filtre fragment 'Date' : %d\n", poolNote);

    ASSERT_HAS(items, "DateVersChaîne", "feat/0971 ordre : 'DateVersChaîne' (prefixe) present");
    ASSERT_HAS(items, "ChaîneVersDate", "feat/0971 ordre : 'ChaîneVersDate' (contient) present");
    ASSERT_SORT_BEFORE(items, "DateVersChaîne", "ChaîneVersDate",
                        "feat/0971 ordre : prefixe classe avant contient");
    json_free(items);
    json_free(params);
}

/* Test 3 -- garde-fou volume : a 1 SEUL caractere tape, le regime reste
 * PREFIXE PUR (le "contient" ne s'active qu'a partir de 2 caracteres).
 * "gprasNeOwlQuote" contient "Q" mais ne commence pas par "Q" -> absent.
 * C'est un invariant protege : VERT attendu AVANT et APRES impl (pas un
 * rouge attendu -- cf brief : "marque-le clairement comme test de
 * non-regression du garde-fou"). */
static void test_completion_over300_single_char_stays_prefix_only(void) {
    printf("\n[TEST] feat/0971 : garde-fou 1 caractere -- regime reste prefixe pur (non-regression)\n");
    const char* src =
        "gprasNeOwlQuote est un entier = 5\n"
        "\n"
        "Q\n";
    setup_doc(URI, src);
    JsonValue* probeParams = make_completion_params(URI, 1, 0);
    JsonValue* probeItems = handle_completion(probeParams);
    int poolSize = count_items(probeItems);
    if (poolSize > 300) {
        g_pass++;
        printf("  [OK]   feat/0971 garde-fou : pool brut = %d items (> 300, regime filtre actif)\n", poolSize);
    } else {
        g_fail++;
        printf("  [FAIL] feat/0971 garde-fou : pool brut = %d items (attendu > 300)\n", poolSize);
    }
    json_free(probeItems);
    json_free(probeParams);

    JsonValue* params = make_completion_params(URI, 2, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_NOT_HAS(items, "gprasNeOwlQuote",
                   "feat/0971 garde-fou : 1 char tape ('Q') -- pas de match contient, prefixe pur");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   -- match exact remonte en tete de la completion generique.
 *  Support #23 : dans un fichier full-English, `TestEnd is a window` +
 *  newline + `end`. Le mot-cle END (match exact case-insensible de `end`)
 *  arrivait derriere les fonctions homonymes (EndsWith, EndProgram, ...)
 *  car toutes partageaient le meme tier "commence par", departage par
 *  l'index d'insertion. Le fix ajoute un tier "match exact" prioritaire.
 *  Generique : ne cible aucun symbole particulier.
 * ================================================================ */

/* Cas 1 -- repro exact du support #23 (mot-cle EN). */
static void test_completion_exact_match_keyword_end_en(void) {
    printf("\n[TEST] fix/0992 : `end` (full-English) -> END en tete (avant EndsWith/EndProgram)\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "TestEnd is a window\n\nend\n";
    setup_doc(URI, src);
    /* Ligne 2 (0-indexed), col 3 = juste apres `end`. */
    JsonValue* params = make_completion_params(URI, 2, 3);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "END", "fix/0992 : mot-cle END present sur 'end'");
    ASSERT_SORT_BEFORE(items, "END", "EndsWith",
                       "fix/0992 : END (exact) avant EndsWith (commence par)");
    ASSERT_SORT_BEFORE(items, "END", "EndProgram",
                       "fix/0992 : END (exact) avant EndProgram (commence par)");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* Cas 2 -- generalite : mot-cle FR SI (exact) devant SinonSi/homonymes.
 * Prouve que le boost n'est pas cable sur END : n'importe quel match exact
 * remonte. */
static void test_completion_exact_match_keyword_si_fr(void) {
    printf("\n[TEST] fix/0992 : `si` (FR) -> SI en tete (match exact generique)\n");
    lspServer.language[0] = '\0'; /* defaut fr */
    const char* src = "si\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 2);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "SI", "fix/0992 : mot-cle SI present sur 'si'");
    /* Sinus (native) et SINON (mot-cle) commencent par 'si' : le match
     * exact SI doit tous les devancer. */
    ASSERT_SORT_BEFORE(items, "SI", "Sinus",
                       "fix/0992 : SI (exact) avant Sinus (native, commence par)");
    ASSERT_SORT_BEFORE(items, "SI", "SINON",
                       "fix/0992 : SI (exact) avant SINON (mot-cle, commence par)");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — mots-cles contextuels emetteur / champ courant / fenetre
 *  courante (MoiMême / MonChamp / MaFenêtre) proposes en completion
 *  generique, avec le LABEL CANON MIXTE en FR ET en EN (MySelf /
 *  MyControl / MyWindow), jamais la forme all-caps.
 * ================================================================ */
static void test_completion_self_context_keywords_fr(void) {
    printf("\n[TEST] feat/1224 : MoiMême/MonChamp/MaFenêtre en completion (FR)\n");
    lspServer.language[0] = '\0'; /* defaut fr */
    const char* src = "m\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "MoiM\xc3\xaa""me",   "feat/1224 FR : MoiMême propose");
    ASSERT_HAS(items, "MonChamp",           "feat/1224 FR : MonChamp propose");
    ASSERT_HAS(items, "MaFen\xc3\xaa""tre", "feat/1224 FR : MaFenêtre propose");
    /* Label canon mixte, PAS la forme majuscule (insererait une casse fausse). */
    ASSERT_NOT_HAS(items, "MOIM\xc3\x8a""ME", "feat/1224 FR : pas de MOIMÊME all-caps");
    json_free(items);
    json_free(params);
}

static void test_completion_self_context_keywords_en(void) {
    printf("\n[TEST] feat/1224 : MySelf/MyControl/MyWindow en completion (EN)\n");
    snprintf(lspServer.language, sizeof(lspServer.language), "en");
    const char* src = "m\n";
    setup_doc(URI, src);
    JsonValue* params = make_completion_params(URI, 0, 1);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "MySelf",    "feat/1224 EN : MySelf propose");
    ASSERT_HAS(items, "MyControl", "feat/1224 EN : MyControl propose");
    ASSERT_HAS(items, "MyWindow",  "feat/1224 EN : MyWindow propose");
    /* Canon EN mixte, PAS le uppercase generique MYSELF/MYCONTROL/MYWINDOW. */
    ASSERT_NOT_HAS(items, "MYSELF",    "feat/1224 EN : pas de MYSELF all-caps");
    ASSERT_NOT_HAS(items, "MYCONTROL", "feat/1224 EN : pas de MYCONTROL all-caps");
    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *   B2 — completion `theme.<prefix>` (styles du theme actif)
 *
 *  Le LSP resout le projet du fichier courant, lit `Projet..Thème` dans
 *  projet.owl, puis scanne le fichier de theme designe pour en extraire les
 *  styles nommes du bloc `est un Thème`. La completion `theme.<prefix>`
 *  propose ces styles (filtrable par type styleXxx).
 * ================================================================ */

/* Prepare un workspace projet-theme sur disque (fixture theme_ws)
 * avec le fichier consommateur ouvert, et retourne son URI dans outUri. */
static void setup_theme_workspace(char* outUri, size_t outUriSize,
                                  const char* consumerSrc) {
    char workspaceRoot[512];
    char consumerPath[512];
    resolve_fixture_path(workspaceRoot, sizeof(workspaceRoot),
                         "theme_ws");
    resolve_fixture_path(consumerPath, sizeof(consumerPath),
                         "theme_ws/FEN_Consommateur.owl");
    snprintf(outUri, outUriSize, "file:///%s", consumerPath);
    for (char* p = outUri + 8; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    setup_workspace_with_rootpath(workspaceRoot, outUri, consumerSrc);
}

/* Cas 1 : `theme.` (curseur juste apres le point) propose les styles nommes
 * du theme actif. Non filtre (contexte hors affectation `..Style`). */
static void test_theme_completion_lists_styles(void) {
    printf("\n[TEST] feat/0982 B2 : 'theme.' propose les styles du theme actif\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	BTN_Valider est un Bouton\n"
        "FIN\n"
        "s est un chaine = theme.\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    /* Ligne 3 (0-based), curseur apres `theme.` : `s est un chaine = theme.`
     * = 18 chars avant `theme`, + `theme.` (6) → col 24. */
    JsonValue* params = make_completion_params(uri, 3, 24);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "boutonPrimaire",
               "feat/0982 B2 : style boutonPrimaire propose");
    ASSERT_HAS(items, "boutonSecondaire",
               "feat/0982 B2 : style boutonSecondaire propose");
    ASSERT_HAS(items, "saisieCompacte",
               "feat/0982 B2 : style saisieCompacte propose");
    /* Un item non theme ne doit pas polluer : seuls les styles sortent. */
    ASSERT_NOT_HAS(items, "BTN_Valider",
                   "feat/0982 B2 : `theme.` ne propose pas les champs");
    json_free(items);
    json_free(params);
}

/* Cas 2 : `theme.bouton` (prefix partiel) filtre par prefix — les deux
 * boutons matchent, la saisie non. */
static void test_theme_completion_prefix_filter(void) {
    printf("\n[TEST] feat/0982 B2 : 'theme.bouton' filtre par prefix\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	BTN_Valider est un Bouton\n"
        "FIN\n"
        "s est un chaine = theme.bouton\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    /* `s est un chaine = theme.bouton` : 18 + `theme.` (6) + `bouton` (6) = 30. */
    JsonValue* params = make_completion_params(uri, 3, 30);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "boutonPrimaire",
               "feat/0982 B2 : prefix 'bouton' → boutonPrimaire");
    ASSERT_HAS(items, "boutonSecondaire",
               "feat/0982 B2 : prefix 'bouton' → boutonSecondaire");
    ASSERT_NOT_HAS(items, "saisieCompacte",
                   "feat/0982 B2 : prefix 'bouton' exclut saisieCompacte");
    json_free(items);
    json_free(params);
}

/* Cas 3 : l'item porte le type styleXxx dans `detail` (surface utilisateur :
 * distingue un styleBouton d'un styleSaisie a la lecture). */
static void test_theme_completion_detail_style_type(void) {
    printf("\n[TEST] feat/0982 B2 : detail = type styleXxx du style\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	BTN_Valider est un Bouton\n"
        "FIN\n"
        "s est un chaine = theme.\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    JsonValue* params = make_completion_params(uri, 3, 24);
    JsonValue* items = handle_completion(params);
    ASSERT_DETAIL_PREFIX(items, "boutonPrimaire", "styleBouton",
                         "feat/0982 B2 : boutonPrimaire detail=styleBouton");
    ASSERT_DETAIL_PREFIX(items, "saisieCompacte", "styleSaisie",
                         "feat/0982 B2 : saisieCompacte detail=styleSaisie");
    json_free(items);
    json_free(params);
}

/*  — `BTN..Style = theme.` filtre les styles au type styleBouton :
 * seuls les styles du bon type widget sont proposes (pas les styleSaisie). */
static void test_theme_completion_filtered_by_bouton(void) {
    printf("\n[TEST] feat/1009 : 'BTN..Style = theme.' ne propose que les styleBouton\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	BTN_Valider est un Bouton\n"
        "FIN\n"
        "BTN_Valider..Style = theme.\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    /* `BTN_Valider..Style = theme.` : 11 + `..`(2) + `Style`(5) + ` = `(3)
     * + `theme.`(6) = 27. */
    JsonValue* params = make_completion_params(uri, 3, 27);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "boutonPrimaire",
               "feat/1009 : styleBouton boutonPrimaire propose");
    ASSERT_HAS(items, "boutonSecondaire",
               "feat/1009 : styleBouton boutonSecondaire propose");
    ASSERT_NOT_HAS(items, "saisieCompacte",
                   "feat/1009 : styleSaisie exclu du contexte Bouton");
    json_free(items);
    json_free(params);
}

/*  — `SAI..Style = theme.` filtre les styles au type styleSaisie :
 * seuls les styleSaisie sont proposes (pas les styleBouton). */
static void test_theme_completion_filtered_by_saisie(void) {
    printf("\n[TEST] feat/1009 : 'SAI..Style = theme.' ne propose que les styleSaisie\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	SAI_Nom est une Saisie\n"
        "FIN\n"
        "SAI_Nom..Style = theme.\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    /* `SAI_Nom..Style = theme.` : 7 + `..`(2) + `Style`(5) + ` = `(3)
     * + `theme.`(6) = 23. */
    JsonValue* params = make_completion_params(uri, 3, 23);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "saisieCompacte",
               "feat/1009 : styleSaisie saisieCompacte propose");
    ASSERT_NOT_HAS(items, "boutonPrimaire",
                   "feat/1009 : styleBouton exclu du contexte Saisie");
    ASSERT_NOT_HAS(items, "boutonSecondaire",
                   "feat/1009 : styleBouton exclu du contexte Saisie");
    json_free(items);
    json_free(params);
}

/*  — forme BLOC `PROPRIETES ... Style = theme.` sur un Libellé :
 * le champ recepteur n'est pas nomme sur la ligne (pas de `CHAMP..Style`),
 * il vient du bloc widget englobant. Doit ne proposer QUE les styleLibellé
 * (usage reel de la demo). Sans le fix, fallback -> styleBouton propose. */
static void test_theme_completion_block_form_libelle(void) {
    printf("\n[TEST] fix/1015 : bloc 'PROPRIETES Style = theme.' sur Libelle -> styleLibelle seulement\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	LIB_Intro est un Champ Libellé\n"
        "		PROPRIETES\n"
        "			Style = theme.\n"
        "		FIN\n"
        "	FIN\n"
        "FIN\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    /* Ligne 3 (0-based) : `\t\t\tStyle = theme.` = 3 tabs + 14 chars = col 17. */
    JsonValue* params = make_completion_params(uri, 3, 17);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "libelléIntro",
               "fix/1015 : styleLibellé libelléIntro propose");
    ASSERT_NOT_HAS(items, "boutonPrimaire",
                   "fix/1015 : styleBouton exclu du contexte Libellé (bloc)");
    ASSERT_NOT_HAS(items, "boutonSecondaire",
                   "fix/1015 : styleBouton exclu du contexte Libellé (bloc)");
    ASSERT_NOT_HAS(items, "saisieCompacte",
                   "fix/1015 : styleSaisie exclu du contexte Libellé (bloc)");
    json_free(items);
    json_free(params);
}

/*  — forme BLOC sur un widget a nom accentue (ZoneRépétée) : le kind
 * derive (styleZoneRépétée) doit matcher malgre les accents (accent-strip
 * coherent entre objectType du catalogue et styleType du theme). */
static void test_theme_completion_block_form_accented_kind(void) {
    printf("\n[TEST] fix/1015 : bloc Style = theme. sur ZoneRépétée -> styleZoneRépétée seulement\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	ZR_Liste est un Champ ZoneRépétée\n"
        "		PROPRIETES\n"
        "			Style = theme.\n"
        "		FIN\n"
        "	FIN\n"
        "FIN\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    JsonValue* params = make_completion_params(uri, 3, 17);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "listeAérée",
               "fix/1015 : styleZoneRépétée listeAérée propose (kind accentue)");
    ASSERT_NOT_HAS(items, "boutonPrimaire",
                   "fix/1015 : styleBouton exclu du contexte ZoneRépétée (bloc)");
    ASSERT_NOT_HAS(items, "saisieCompacte",
                   "fix/1015 : styleSaisie exclu du contexte ZoneRépétée (bloc)");
    json_free(items);
    json_free(params);
}

/*  — forme BLOC sur un Bouton : non-regression, la forme bloc doit
 * filtrer comme la forme plate — seuls les styleBouton. */
static void test_theme_completion_block_form_bouton(void) {
    printf("\n[TEST] fix/1015 : bloc Style = theme. sur Bouton -> styleBouton seulement\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	BTN_Valider est un Champ Bouton\n"
        "		PROPRIETES\n"
        "			Style = theme.\n"
        "		FIN\n"
        "	FIN\n"
        "FIN\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    JsonValue* params = make_completion_params(uri, 3, 17);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "boutonPrimaire",
               "fix/1015 : styleBouton boutonPrimaire propose (bloc)");
    ASSERT_HAS(items, "boutonSecondaire",
               "fix/1015 : styleBouton boutonSecondaire propose (bloc)");
    ASSERT_NOT_HAS(items, "saisieCompacte",
                   "fix/1015 : styleSaisie exclu du contexte Bouton (bloc)");
    ASSERT_NOT_HAS(items, "libelléIntro",
                   "fix/1015 : styleLibellé exclu du contexte Bouton (bloc)");
    json_free(items);
    json_free(params);
}

/*  — fallback : `theme.` hors contexte `..Style` (type recepteur non
 * determinable) → tous les styles proposes, tous types confondus. */
static void test_theme_completion_fallback_all_when_no_receiver(void) {
    printf("\n[TEST] feat/1009 : 'theme.' hors contexte ..Style → tous les styles (fallback)\n");
    char uri[600];
    const char* src =
        "FEN_Consommateur est une Fenêtre\n"
        "	BTN_Valider est un Bouton\n"
        "FIN\n"
        "s est un chaine = theme.\n";
    setup_theme_workspace(uri, sizeof(uri), src);
    JsonValue* params = make_completion_params(uri, 3, 24);
    JsonValue* items = handle_completion(params);
    ASSERT_HAS(items, "boutonPrimaire",
               "feat/1009 fallback : styleBouton propose");
    ASSERT_HAS(items, "saisieCompacte",
               "feat/1009 fallback : styleSaisie propose");
    json_free(items);
    json_free(params);
}

/* ================================================================
 *   — lsp_run_check_bounded ne doit ni deadlocker ni tronquer
 *  une sortie `--check` volumineuse mais RAPIDE (compilo pas en boucle).
 * ================================================================ */

/* Resout dynamiquement le chemin absolu vers build/owl.exe depuis le
 * path source de ce fichier (__FILE__ = tests/lsp/lsp_test_main.c).
 * Portable Windows/Linux, meme pattern que resolve_fixture_path. */
static void resolve_owl_exe_path(char* outBuf, size_t bufSize) {
    char testFilePath[512];
    char testDir[512];

    snprintf(testFilePath, sizeof(testFilePath), "%s", __FILE__);
    char* lastSlash = strrchr(testFilePath, '\\');
    if (!lastSlash) lastSlash = strrchr(testFilePath, '/');
    if (lastSlash) {
        *lastSlash = '\0';
        snprintf(testDir, sizeof(testDir), "%s", testFilePath);
    } else {
        snprintf(testDir, sizeof(testDir), ".");
    }

#ifdef _WIN32
    char exePath[512];
    snprintf(exePath, sizeof(exePath), "%s/../../build/owl.exe", testDir);
    _fullpath(outBuf, exePath, bufSize);
#else
    char exePath[512];
    snprintf(exePath, sizeof(exePath), "%s/../../build/owl", testDir);
    realpath(exePath, outBuf);
#endif
}

/*  — repro du deadlock/troncature : `owl --check` sur un projet a
 * ~40 fichiers legacy produit une sortie JSON > 13000 octets, en largement
 * moins d'1s (le compilo ne boucle pas). lsp_run_check_bounded doit lire
 * cette sortie EN ENTIER et renvoyer LSP_RUN_OK -- pas LSP_RUN_TIMEOUT
 * (deadlock de pipe sur le buffer fixe cote appelant, cf. lsp_handlers.c
 * `char output[8192]`), et pas une sortie tronquee.
 *
 * timeoutMs volontairement modeste (3000ms, pas les 15000ms de prod) pour
 * que le cas rouge echoue vite : au lieu d'attendre 15s pour constater le
 * deadlock, on le constate en ~3s.
 *
 *  : sur cette machine, CreateProcessA bloque ~5015 ms sur ~1 lancement
 * sur 3 (scan a l'execution, reproduit sur un binaire de 30 Ko : environnemental,
 * pas propre a owl.exe). Le budget est desormais compte a partir du spawn cote
 * runner, donc ces 5 s ne le consomment plus ; le check lui-meme mesure 170-265 ms
 * pour cette fixture (41 fichiers), soit >10x de marge sous 3000 ms. */
static void test_fix1245_check_bounded_large_output_no_deadlock(void) {
    printf("\n[TEST] fix/1245 : sortie --check volumineuse (>13000o) mais rapide -> pas de faux timeout\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "bigoutput");

    /* Buffer identique en taille a celui de lsp_handlers.c (le bug reel). */
    char output[8192];
    int outLen = 0;
    const int timeoutMs = 3000;
    long long t0 = lsp_test_now_ms();
    LspRunStatus rs = lsp_run_check_bounded(exePath, fixtureDir, NULL,
                                            output, (int)sizeof(output),
                                            timeoutMs, &outLen);
    long long elapsedMs = lsp_test_now_ms() - t0;

    if (rs == LSP_RUN_OK) {
        g_pass++;
        printf("  [OK]   fix/1245 : status = LSP_RUN_OK en %lld ms (budget %d ms)\n",
               elapsedMs, timeoutMs);
    } else {
        g_fail++;
        printf("  [FAIL] fix/1245 : status = %s apres %lld ms (budget enfant %d ms),"
               " %d octets recus sur >13000 attendus\n",
               rs == LSP_RUN_TIMEOUT ? "LSP_RUN_TIMEOUT" : "LSP_RUN_SPAWN_FAIL",
               elapsedMs, timeoutMs, outLen);
        printf("         requete : \"%s\" --check \"%s\"\n", exePath, fixtureDir);
        if (rs == LSP_RUN_SPAWN_FAIL) {
            printf("         lecture : le process n'a pas demarre -- owl.exe absent /"
                   " non executable a ce chemin (build fait ?).\n");
        } else if (outLen == 0) {
            printf("         lecture : 0 octet = l'enfant a ete tue AVANT d'avoir ecrit."
                   " Ce n'est pas un deadlock de pipe\n"
                   "                   (qui laisse ~%d octets bloques). Cas connu :"
                   " creation de process lente\n"
                   "                   (scan a l'execution ~5 s) ou owl.exe qui crashe"
                   " au demarrage. elapsed=%lld ms\n"
                   "                   >> budget => le temps a ete brule hors du check"
                   " (cf. fix/1330).\n",
                   (int)sizeof(output), elapsedMs);
        } else {
            printf("         lecture : %d octets puis blocage = signature du deadlock de pipe"
                   " (plafond buffer %d)\n"
                   "                   -- le drain de lsp_proc.c a cesse de lire avant EOF.\n",
                   outLen, (int)sizeof(output));
        }
    }

    /* Meme si le status est OK, la sortie doit etre COMPLETE (pas tronquee
     * a cap-1 = 8191). Le contrat vise : sortie > 13000 octets capturee
     * en entier. Avec le buffer fixe actuel, outLen plafonne a 8191. */
    if (outLen > 13000) {
        g_pass++;
        printf("  [OK]   fix/1245 : sortie complete capturee (%d octets > 13000)\n", outLen);
    } else {
        g_fail++;
        printf("  [FAIL] fix/1245 : sortie incomplete -- %d octets capturees, >13000 attendues"
               " (status %s, %lld ms)\n",
               outLen,
               rs == LSP_RUN_OK ? "LSP_RUN_OK" :
               (rs == LSP_RUN_TIMEOUT ? "LSP_RUN_TIMEOUT" : "LSP_RUN_SPAWN_FAIL"),
               elapsedMs);
    }
}

/* ================================================================
 *   Lot 2 — le check projet doit refleter les BUFFERS ouverts
 *  non-sauves, pas le disque, pour les fichiers AUTRES que celui sauve.
 *
 *  Fixture tests/lsp/fixtures/overlay/ :
 *    - projet.owl : marqueur frontiere PROJECT.
 *    - fichierA.owl : valide sur disque, c'est LUI qu'on "sauve" (declenche
 *      publish_compiler_diagnostics).
 *    - fichierB.owl : ERREUR sur disque (FonctionDisqueInconnueLsp), mais
 *      son BUFFER ouvert dans le harness contient la version CORRIGEE.
 *
 *  Contrat vise (Lot 2) : publish_compiler_diagnostics construit l'overlay
 *  depuis lspServer.documents[] (tous les buffers ouverts, y compris B) et
 *  le passe a `owl --check <racine> --overlay-stdin`. Le check doit alors
 *  voir la version CORRIGEE de B (buffer), pas l'erreur du disque.
 *
 *  ROUGE ATTENDU aujourd'hui : publish_compiler_diagnostics ne construit ni
 *  ne passe encore d'overlay -> le check spawn lit le disque -> l'erreur de
 *  B est toujours presente dans le cache diagnostics (lsp_get_compiler_diag_count
 *  retourne >= 1 au lieu de 0). Spawn REEL d'owl.exe (pas de mock) : seam
 *  choisi = fonction publiee `publish_compiler_diagnostics` + cache per-URI
 *   (seule couche testable in-process, cf. note testabilite plus haut :
 *  les notifications stdout ne sont pas interceptables depuis ce harness).
 * ================================================================ */
static void test_feat1246_lot2_overlay_reflects_open_buffer_not_disk(void) {
    printf("\n[TEST] feat/1246 Lot 2 : didSave A reflete le buffer CORRIGE de B, pas le disque (RED attendu)\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "overlay");

    char uriA[600], uriB[600];
    snprintf(uriA, sizeof(uriA), "file:///%s/fichierA.owl", fixtureDir);
    snprintf(uriB, sizeof(uriB), "file:///%s/fichierB.owl", fixtureDir);
    /* Normalise les backslashes Windows en '/' pour un file:// propre. */
    for (char* p = uriA; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = uriB; *p; p++) if (*p == '\\') *p = '/';

    /* Deux buffers ouverts simultanement : A (identique au disque) et B
     * (CORRIGE dans le buffer, alors que le disque contient encore
     * l'erreur FonctionDisqueInconnueLsp). setup_doc() reset a 1 seul doc,
     * donc on peuple lspServer.documents[] directement pour les 2. */
    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 2;

    const char* srcA = "n est un entier = 1\nTrace(n)\n";
    const char* srcBFixed = "n est un entier = 1\nTrace(n)\n"; /* corrige : plus d'appel inconnu */

    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uriA);
    lspServer.documents[0].content = strdup(srcA);
    lspServer.documents[0].contentLen = (int)strlen(srcA);
    lspServer.documents[0].version = 1;

    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", uriB);
    lspServer.documents[1].content = strdup(srcBFixed);
    lspServer.documents[1].contentLen = (int)strlen(srcBFixed);
    lspServer.documents[1].version = 1;

    /* Reset tracking diagnostics + rootPath pour ne pas heriter d'un etat
     * d'un test precedent (lspServer est un global partage entre tests
     * dans ce harness in-process). Un rootPath laisse par un test workspace
     * anterieur ferait echouer le bornage `fmt_path_within` (
     * Lot 2 doit rester en mode PROJECT sur ce fixture). */
    lspServer.diagnosedUriCount = 0;
    lspServer.rootPath[0] = '\0';

    /* Simule le didSave de A : c'est le seul chemin qui declenche le check
     * projet. */
    publish_compiler_diagnostics(uriA);

    int countB = lsp_get_compiler_diag_count(uriB);
    if (countB == 0) {
        g_pass++;
        printf("  [OK]   feat/1246 Lot 2 : fichierB propre (overlay depuis buffer applique, disque ignore)\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/1246 Lot 2 : fichierB compte %d diagnostic(s) (attendu 0 -- "
               "le check a lu le DISQUE de B au lieu du buffer corrige ; overlay pas encore construit/passe)\n",
               countB);
    }
}

/* ================================================================
 *   — en mode SCRIPT (pas de projet.owl ancetre), le check
 *  doit porter sur le DOSSIER du fichier (voisins non-recursifs),
 *  pas sur le fichier seul.
 *
 *  Fixture tests/lsp/fixtures/script_scope/ :
 *    - PAS de projet.owl (mode SCRIPT).
 *    - fichierA.owl : valide, c'est LUI qu'on "sauve" (declenche
 *      publish_compiler_diagnostics).
 *    - fichierB.owl : VOISIN non ouvert, erreur sur DISQUE
 *      (FonctionVoisineInconnue).
 *
 *  Contrat vise : en mode script, le LSP passe le DOSSIER (pas le
 *  fichier) a `--check`, comme deja fait en mode PROJET (racine
 *  projet). L'erreur du voisin B doit remonter dans le cache
 *  diagnostics apres le check de A.
 *
 *  ROUGE ATTENDU aujourd'hui : le LSP passe encore le fichier SEUL
 *  en mode script -> le check ne voit jamais fichierB ->
 *  lsp_get_compiler_diag_count(uriB) reste a -1 (URI jamais peuple),
 *  pas >= 1.
 * ================================================================ */
static void test_feat1263_script_mode_checks_folder_neighbor_seen(void) {
    printf("\n[TEST] feat/1263 : mode script check dossier -- erreur voisin B remontee (RED attendu)\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "script_scope");

    char uriA[600], uriB[600];
    snprintf(uriA, sizeof(uriA), "file:///%s/fichierA.owl", fixtureDir);
    snprintf(uriB, sizeof(uriB), "file:///%s/fichierB.owl", fixtureDir);
    /* Normalise les backslashes Windows en '/' pour un file:// propre. */
    for (char* p = uriA; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = uriB; *p; p++) if (*p == '\\') *p = '/';

    /* Seul A est ouvert dans le harness (buffer identique au disque) ;
     * B n'est PAS ouvert -- son erreur ne peut venir que du DISQUE via
     * un scan dossier. */
    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 1;

    const char* srcA = "n est un entier = 1\nTrace(n)\n";
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uriA);
    lspServer.documents[0].content = strdup(srcA);
    lspServer.documents[0].contentLen = (int)strlen(srcA);
    lspServer.documents[0].version = 1;

    /* Reset tracking diagnostics + rootPath (global partage entre tests).
     * rootPath pointe sur le dossier fixture SANS projet.owl ancetre ->
     * mode SCRIPT. */
    lspServer.diagnosedUriCount = 0;
    snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", fixtureDir);

    /* Simule le didSave de A : declenche le check en mode script. */
    publish_compiler_diagnostics(uriA);

    int countB = lsp_get_compiler_diag_count(uriB);
    if (countB >= 1) {
        g_pass++;
        printf("  [OK]   feat/1263 : erreur du voisin fichierB remontee (count=%d, scope dossier applique)\n",
               countB);
    } else {
        g_fail++;
        printf("  [FAIL] feat/1263 : fichierB compte %d diagnostic(s) (attendu >= 1 -- "
               "le check est encore fichier-seul en mode script, le voisin B n'est jamais vu)\n",
               countB);
    }
}

/* ================================================================
 *   — un check qui timeout (LSP_RUN_TIMEOUT) ne doit PAS clobberer
 *  les diagnostics deja publies/mis en cache pour le fichier actif : avant
 *  , apply_check_result republiait un unique diagnostic "boucle
 *  possible" qui REMPLACAIT (semantique LSP publishDiagnostics) le dernier
 *  bon set -- fail silencieux, symptome support #370.
 *
 *  Seam choisi (meme contrainte de testabilite que les autres tests de ce
 *  fichier -- notifications stdout non interceptables in-process) : le
 *  cache par-URI  (lsp_get_compiler_diag_count) + deux hooks
 *  test-only exposes par lsp_handlers.c pour ce ticket :
 *
 *    void lsp_test_set_timeout_override_ms(int ms)
 *        Force le timeout adaptatif du PROCHAIN check spawn (0 = defaut
 *        production 15000 ms inchange). Permet de forcer deterministiquement
 *        un LSP_RUN_TIMEOUT sans attendre le vrai plancher (le spawn reel
 *        d'owl.exe prend largement plus qu'1 ms avant meme d'ecrire une
 *        sortie --  : le chrono est arme APRES la creation du
 *        process, donc pas de faux LSP_RUN_SPAWN_FAIL).
 *
 *    int lsp_check_timeout_branch_count(void) / _reset(void)
 *        Spy : compte les entrees dans le chemin LSP_RUN_TIMEOUT
 *        d'apply_check_result, AVANT tout throttle de notification.
 *        Preuve que le test a bien exerce CE chemin (pas un hasard --
 *        cf. test-writing-pitfalls section 3 Cas B).
 *
 *  Fixture reutilisee : tests/lsp/fixtures/bigoutput/ (projet a
 *  ~40 fichiers, deja utilisee par le test  ci-dessus qui mesure
 *  170-265 ms de duree reelle de check). Un budget de 50 ms est trop court
 *  pour ce volume mais largement superieur au delai de spawn du process
 *  ( : le chrono est arme APRES la creation du process) -- le
 *  timeout est donc un VRAI depassement d'execution, pas un faux
 *  LSP_RUN_SPAWN_FAIL. Un budget de 1 ms sur un fichier trivial avait ete
 *  essaye en premier lieu : le check terminait AVANT le premier sondage
 *  (poll) du harness, donc LSP_RUN_OK, jamais LSP_RUN_TIMEOUT -- d'ou le
 *  choix de cette fixture plus lourde.
 * ================================================================ */
static void test_fix2126_timeout_no_clobber_active_diagnostics(void) {
    printf("\n[TEST] fix/2126 : timeout du check ne clobbere pas les diagnostics du fichier actif\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "bigoutput");

    char uriA[600];
    snprintf(uriA, sizeof(uriA), "file:///%s/classe_01.owl", fixtureDir);
    for (char* p = uriA; *p; p++) if (*p == '\\') *p = '/';

    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 0;

    lspServer.diagnosedUriCount = 0;
    snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", fixtureDir);

    /* Etat "dernier bon set" simule : un check anterieur avait remonte 2
     * diagnostics semantiques sur A (peu importe leur nature reelle -- ce
     * qui compte est qu'un timeout ulterieur ne les efface pas). */
    lsp_populate_compiler_diag_cache(uriA, 2);
    lsp_check_timeout_branch_reset();

    /* Force un LSP_RUN_TIMEOUT deterministe sur le prochain check spawn :
     * 50 ms << 170-265 ms de duree reelle mesuree pour cette fixture. */
    lsp_test_set_timeout_override_ms(50);
    publish_compiler_diagnostics(uriA);
    lsp_test_set_timeout_override_ms(0);  /* restaure le defaut production */

    int branchCount = lsp_check_timeout_branch_count();
    int countA = lsp_get_compiler_diag_count(uriA);

    if (branchCount >= 1) {
        g_pass++;
        printf("  [OK]   fix/2126 : chemin LSP_RUN_TIMEOUT exerce (branchCount=%d)\n", branchCount);
    } else {
        g_fail++;
        printf("  [FAIL] fix/2126 : chemin LSP_RUN_TIMEOUT jamais exerce (branchCount=%d) -- "
               "le timeout override n'a pas produit LSP_RUN_TIMEOUT, l'assertion suivante "
               "ne prouverait rien\n", branchCount);
    }

    if (countA == 2) {
        g_pass++;
        printf("  [OK]   fix/2126 : cache diagnostics de A intact apres timeout (count=%d, pas clobber)\n",
               countA);
    } else {
        g_fail++;
        printf("  [FAIL] fix/2126 : cache diagnostics de A altere par le timeout (count=%d, attendu 2 -- "
               "le message timeout a clobber le dernier bon set)\n", countA);
    }
}

/* ================================================================
 *   — timeout adaptatif du check projet : fonction PURE testable
 *  (zero I/O). timeout_ms = borne(15000, 60000, 15000 + round(25 * Ko))
 *  ou 1 Ko = 1024 octets. Le plancher n'est atteint qu'a ~0 octet (le
 *  terme +25/Ko est toujours >= 0) ; il ne represente donc PAS n'importe
 *  quel "petit" projet mais le projet vide/quasi-vide. Le plafond 60000
 *  ecrete tout scope au-dela de ~1,84 Mo de source.
 *
 *  Interface exposee par lsp_handlers.c (decl dans lsp_server.h) :
 *    int lsp_adaptive_timeout_for_bytes(size_t bytes)
 * ================================================================ */
static void test_fix2133_adaptive_timeout_for_bytes(void) {
    printf("\n[TEST] fix/2133 : formule pure du timeout adaptatif (octets -> ms)\n");

    struct { size_t bytes; int expected; const char* label; } cases[] = {
        { 0,        15000, "plancher : projet vide -> 15000 ms" },
        { 10240,    15250, "10 Ko -> 15250 ms (au-dessus du plancher, arrondi)" },
        { 1048576,  40600, "1 Mo -> 15000 + round(25*1024) = 40600 ms" },
        { 5242880,  60000, "5 Mo -> plafond 60000 ms (calcul brut 143000 ecrete)" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = lsp_adaptive_timeout_for_bytes(cases[i].bytes);
        if (got == cases[i].expected) {
            g_pass++;
            printf("  [OK]   fix/2133 : %zu octets -> %d ms (%s)\n",
                   cases[i].bytes, got, cases[i].label);
        } else {
            g_fail++;
            printf("  [FAIL] fix/2133 : %zu octets -> %d ms, attendu %d (%s)\n",
                   cases[i].bytes, got, cases[i].expected, cases[i].label);
        }
    }

    /* Independance vis-a-vis du plafond : juste sous le seuil d'ecretage
     * (~1,84 Mo) la valeur doit etre STRICTEMENT sous 60000, prouvant que le
     * 60000 des gros cas vient bien du clamp et pas d'un calcul qui plafonne
     * tout seul. 1 800 000 octets -> 15000 + round(25*1757.8125) = 58945. */
    int sub = lsp_adaptive_timeout_for_bytes(1800000);
    if (sub > 15000 && sub < 60000) {
        g_pass++;
        printf("  [OK]   fix/2133 : 1 800 000 octets -> %d ms (dans la plage, ni plancher ni plafond)\n", sub);
    } else {
        g_fail++;
        printf("  [FAIL] fix/2133 : 1 800 000 octets -> %d ms, attendu strictement entre 15000 et 60000\n", sub);
    }
}

/* ================================================================
 *   Lot A — verrou observable : une erreur portee par un fichier
 *  VOISIN (mode SCRIPT, pas de projet.owl) doit se publier sur l'URI DU
 *  VOISIN, a sa vraie ligne/colonne — pas un faux positif ligne 1 sur le
 *  script sauve. Verrouille aussi le cycle de vie : une fois le voisin
 *  repare, son diagnostic doit disparaitre (pas de fantome).
 *
 *  Objectif du ticket (Lot B/C a venir) : retirer le format de rapport
 *  --check "mono" et tout unifier sur le format riche files[] deja gere
 *  par apply_check_result. Ce test verrouille le contrat AVANT cette
 *  bascule runtime.
 *
 *  Interface requise exposee par l'implementeur (lsp_handlers.c), meme
 *  contrat testabilite que lsp_get_compiler_diag_count :
 *
 *    int lsp_get_compiler_diag_line(const char* uri, int index)
 *        Ligne (1-based) du diagnostic `index` en cache pour cet URI.
 *        -1 si l'URI est inconnu du cache ou l'index hors bornes.
 *        UNIQUEMENT pour les tests -- ne pas exporter en production.
 *
 *    int lsp_get_compiler_diag_column(const char* uri, int index)
 *        Colonne (1-based), memes regles que ci-dessus.
 *        UNIQUEMENT pour les tests -- ne pas exporter en production.
 *
 *  Fixture tests/lsp/fixtures/script_neighbor/ (mode SCRIPT,
 *  aucun projet.owl) :
 *    - script.owl : sain, c'est LUI qu'on "sauve" (declenche
 *      publish_compiler_diagnostics).
 *    - FEN_Voisin.owl : erreur stable ligne 21 colonne 14
 *      ('ColonneInexistante' n'est pas une colonne de TABLE_X).
 * ================================================================ */
#define LSP_DIAG_LINE_API_IMPLEMENTED 1
#ifndef LSP_DIAG_LINE_API_IMPLEMENTED
static int lsp_get_compiler_diag_line(const char* uri, int index) {
    (void)uri; (void)index;
    return -2; /* sentinelle "pas encore implemente" */
}
static int lsp_get_compiler_diag_column(const char* uri, int index) {
    (void)uri; (void)index;
    return -2; /* sentinelle "pas encore implemente" */
}
#else
extern int lsp_get_compiler_diag_line(const char* uri, int index);   /* impl reelle (lsp_handlers.c) */
extern int lsp_get_compiler_diag_column(const char* uri, int index); /* impl reelle (lsp_handlers.c) */
#endif /* LSP_DIAG_LINE_API_IMPLEMENTED */

/* Test 1 : navigabilite (voisin a sa vraie position) + pas de faux
 * positif sur le script. RED attendu aujourd'hui : l'accesseur
 * ligne/colonne n'existe pas encore (-2). */
static void test_fix1471_script_neighbor_navigable_at_real_position(void) {
    printf("\n[TEST] fix/1471 : erreur voisin navigable a sa vraie position (RED attendu)\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "script_neighbor");

    char uriScript[600], uriVoisin[600];
    snprintf(uriScript, sizeof(uriScript), "file:///%s/script.owl", fixtureDir);
    snprintf(uriVoisin, sizeof(uriVoisin), "file:///%s/FEN_Voisin.owl", fixtureDir);
    for (char* p = uriScript; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = uriVoisin; *p; p++) if (*p == '\\') *p = '/';

    /* Seul le script est ouvert dans le harness -- le voisin fautif n'est
     * vu que par le scan disque du dossier (mode SCRIPT, ). */
    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 1;

    const char* srcScript =
        "// Script principal du dossier (mode SCRIPT, pas de projet.owl).\n"
        "Trace(\"ok\")\n";
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uriScript);
    lspServer.documents[0].content = strdup(srcScript);
    lspServer.documents[0].contentLen = (int)strlen(srcScript);
    lspServer.documents[0].version = 1;

    lspServer.diagnosedUriCount = 0;
    snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", fixtureDir);

    /* Simule le didSave du script : declenche le check en mode script. */
    publish_compiler_diagnostics(uriScript);

    int countScript = lsp_get_compiler_diag_count(uriScript);
    int countVoisin = lsp_get_compiler_diag_count(uriVoisin);
    int line = lsp_get_compiler_diag_line(uriVoisin, 0);
    int col  = lsp_get_compiler_diag_column(uriVoisin, 0);

    int ok = 1;
    if (countScript != 0) {
        ok = 0;
        printf("  [FAIL] fix/1471 : script.owl compte %d diagnostic(s) (attendu 0 -- "
               "faux positif sur le script, l'erreur appartient au voisin)\n", countScript);
    }
    if (countVoisin < 1) {
        ok = 0;
        printf("  [FAIL] fix/1471 : FEN_Voisin.owl compte %d diagnostic(s) (attendu >= 1 -- "
               "diagnostic voisin jamais publie sur sa propre URI)\n", countVoisin);
    }
    if (line != 21) {
        ok = 0;
        printf("  [FAIL] fix/1471 : ligne du diagnostic voisin = %d (attendu 21 -- "
               "pas la vraie position, ou API ligne/colonne non implementee [-2])\n", line);
    }
    if (col != 14) {
        ok = 0;
        printf("  [FAIL] fix/1471 : colonne du diagnostic voisin = %d (attendu 14 -- "
               "pas la vraie position, ou API ligne/colonne non implementee [-2])\n", col);
    }

    if (ok) {
        g_pass++;
        printf("  [OK]   fix/1471 : voisin navigable a 21:14, script propre (0 faux positif)\n");
    } else {
        g_fail++;
    }
}

/* Test 2 : cycle de vie -- une fois le voisin repare (buffer corrige, overlay
 *  prioritaire sur le disque), son diagnostic doit etre EFFACE, pas
 * laisse fantome sur une URI redevenue saine. RED attendu aujourd'hui via le
 * meme mecanisme (accesseur pas encore expose ne bloque pas ce test-ci, qui
 * ne s'appuie que sur lsp_get_compiler_diag_count deja reel -- mais reste
 * verrouille ici pour couvrir le cycle de vie complet du ticket). */
static void test_fix1471_script_neighbor_diagnostic_cleared_after_fix(void) {
    printf("\n[TEST] fix/1471 : diagnostic voisin efface une fois repare\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "script_neighbor");

    char uriScript[600], uriVoisin[600];
    snprintf(uriScript, sizeof(uriScript), "file:///%s/script.owl", fixtureDir);
    snprintf(uriVoisin, sizeof(uriVoisin), "file:///%s/FEN_Voisin.owl", fixtureDir);
    for (char* p = uriScript; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = uriVoisin; *p; p++) if (*p == '\\') *p = '/';

    /* Deux buffers ouverts : script (sain) + voisin (buffer initialement
     * CASSE, identique au disque). Setup independant du test 1 -- on
     * repeuple lspServer.documents[] directement pour les 2. */
    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 2;

    const char* srcScript =
        "// Script principal du dossier (mode SCRIPT, pas de projet.owl).\n"
        "Trace(\"ok\")\n";
    const char* srcVoisinBroken =
        "FEN_Voisin est une Fenêtre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispVerticale\n"
        "\t\tLargeur     = 400\n"
        "\t\tHauteur     = 300\n"
        "\tFIN\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tPROPRIETES\n"
        "\t\t\tÉtire = Vrai\n"
        "\t\tFIN\n"
        "\t\tCOL_A est une Colonne\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibellé = \"A\"\n"
        "\t\t\t\tType    = colTypeTexte\n"
        "\t\t\t\tLargeur = 100\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tPROCEDURE Peindre()\n"
        "\t\ti est un entier = 1\n"
        "\t\tTABLE_X[i].ColonneInexistante = \"valeur bidon\"\n"
        "\tFIN\n"
        "FIN\n";
    const char* srcVoisinFixed =
        "FEN_Voisin est une Fenêtre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispVerticale\n"
        "\t\tLargeur     = 400\n"
        "\t\tHauteur     = 300\n"
        "\tFIN\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tPROPRIETES\n"
        "\t\t\tÉtire = Vrai\n"
        "\t\tFIN\n"
        "\t\tCOL_A est une Colonne\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibellé = \"A\"\n"
        "\t\t\t\tType    = colTypeTexte\n"
        "\t\t\t\tLargeur = 100\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tPROCEDURE Peindre()\n"
        "\t\ti est un entier = 1\n"
        "\t\tTABLE_X[i].COL_A = \"valeur ok\"\n"
        "\tFIN\n"
        "FIN\n";

    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uriScript);
    lspServer.documents[0].content = strdup(srcScript);
    lspServer.documents[0].contentLen = (int)strlen(srcScript);
    lspServer.documents[0].version = 1;

    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", uriVoisin);
    lspServer.documents[1].content = strdup(srcVoisinBroken);
    lspServer.documents[1].contentLen = (int)strlen(srcVoisinBroken);
    lspServer.documents[1].version = 1;

    lspServer.diagnosedUriCount = 0;
    snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", fixtureDir);

    /* 1er save du script : le voisin casse (buffer) doit etre signale. */
    publish_compiler_diagnostics(uriScript);
    int countBefore = lsp_get_compiler_diag_count(uriVoisin);

    /* "Reparation" du voisin -- simule un didChange corrige, sans toucher
     * au disque de la fixture (le buffer prime sur le disque, ). */
    free(lspServer.documents[1].content);
    lspServer.documents[1].content = strdup(srcVoisinFixed);
    lspServer.documents[1].contentLen = (int)strlen(srcVoisinFixed);
    lspServer.documents[1].version = 2;

    /* 2e save du script : le check doit revoir le voisin CORRIGE. */
    publish_compiler_diagnostics(uriScript);
    int countAfter = lsp_get_compiler_diag_count(uriVoisin);

    if (countBefore >= 1 && countAfter == 0) {
        g_pass++;
        printf("  [OK]   fix/1471 : voisin signale (%d) puis efface (%d) une fois repare\n",
               countBefore, countAfter);
    } else {
        g_fail++;
        printf("  [FAIL] fix/1471 : voisin avant=%d apres=%d (attendu avant>=1, apres==0 -- "
               "diagnostic fantome si apres != 0, ou routing casse si avant < 1)\n",
               countBefore, countAfter);
    }
}

/* ================================================================
 *   Lot 2a — le check RESIDENT doit rendre EXACTEMENT le meme
 *  verdict que le spawn par check.
 *
 *  Ce harnais est le seul endroit qui possede les DEUX chemins : l'oracle
 *  (`lsp_run_check_bounded`, spawn dedie = ce que fait `owl --check`) et le
 *  client resident (`lsp_check_start/poll`). Il exerce en plus le vrai
 *  framing sur de vrais pipes, ce qu'un self-test in-process cote owl.exe
 *  ne verrait pas. D'ou le choix de le poser ici plutot que dans
 *  src/internal_tests/.
 *
 *  Les requetes sont enchainees sur UN SEUL resident : c'est la seule facon
 *  de prouver l'absence de pollution d'etat entre deux checks (registre / VM
 *  / tables compile-time), le point de risque du mode resident.
 * ================================================================ */

/* Enleve les CR : le spawn dedie passe par un pipe en mode TEXTE (le CRT
 * Windows traduit '\n' en "\r\n" a l'ecriture), la reponse resident est
 * framee donc forcement binaire. Seule difference legitime entre les deux. */
static void lsp1335_strip_cr(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) if (*r != '\r') *w++ = *r;
    *w = '\0';
}

/* Lance un check via lsp_check_start (donc via le resident) et attend son
 * verdict. Renvoie le status ; `out` recoit la sortie NUL-terminee. */
static LspRunStatus lsp1335_resident_check(const char* exePath, const char* target,
                                           const char* lang, const char* overlayJson,
                                           char* out, int cap) {
    out[0] = '\0';
    LspCheckJob* job = lsp_check_start(exePath, target, NULL, lang, overlayJson, 15000);
    if (!job) return LSP_RUN_SPAWN_FAIL;
    LspRunStatus st = LSP_RUN_OK;
    char* buf = NULL;
    int len = 0;
    while (!lsp_check_poll(job, &st, &buf, &len)) lsp_proc_sleep_ms(2);
    int copy = len < cap - 1 ? len : cap - 1;
    if (buf && copy > 0) memcpy(out, buf, (size_t)copy);
    out[copy] = '\0';
    free(buf);
    return st;
}

/* Compare le verdict resident au verdict du spawn dedie sur la meme cible. */
static void lsp1335_expect_same(const char* exePath, const char* target,
                                const char* label) {
    char oneshot[65536];
    char resident[65536];
    int oneshotLen = 0;
    LspRunStatus rsOne = lsp_run_check_bounded(exePath, target, NULL,
                                               oneshot, (int)sizeof(oneshot),
                                               15000, &oneshotLen);
    LspRunStatus rsRes = lsp1335_resident_check(exePath, target, NULL, NULL,
                                                resident, (int)sizeof(resident));
    lsp1335_strip_cr(oneshot);
    lsp1335_strip_cr(resident);

    if (rsOne == LSP_RUN_OK && rsRes == LSP_RUN_OK && strcmp(oneshot, resident) == 0) {
        g_pass++;
        printf("  [OK]   fix/1335 : %s -- verdict resident identique au spawn (%d octets)\n",
               label, (int)strlen(resident));
    } else {
        g_fail++;
        printf("  [FAIL] fix/1335 : %s -- divergence resident vs spawn\n", label);
        printf("         spawn    (%s) : %.400s\n",
               rsOne == LSP_RUN_OK ? "OK" : "KO", oneshot);
        printf("         resident (%s) : %.400s\n",
               rsRes == LSP_RUN_OK ? "OK" : "KO", resident);
    }
}

static void test_fix1335_lot2a_resident_equivalence(void) {
    printf("\n[TEST] fix/1335 Lot 2a : le check resident rend le meme verdict que le spawn\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));

    if (!lsp_check_server_warmup(exePath)) {
        g_fail++;
        printf("  [FAIL] fix/1335 : le resident n'a pas demarre (%s --check-server)\n", exePath);
        return;
    }
    g_pass++;
    printf("  [OK]   fix/1335 : resident demarre (1 seul spawn pour toute la serie)\n");

    char scriptDir[512], projDir[512], fileOk[600], fileErr[600];
    resolve_fixture_path(scriptDir, sizeof(scriptDir), "script_scope");
    resolve_fixture_path(projDir, sizeof(projDir), "overlay");
    snprintf(fileOk,  sizeof(fileOk),  "%s/fichierA.owl", scriptDir);
    snprintf(fileErr, sizeof(fileErr), "%s/fichierB.owl", scriptDir);

    lsp1335_expect_same(exePath, fileOk,    "fichier sain (mono)");
    lsp1335_expect_same(exePath, fileErr,   "fichier avec erreur (mono)");
    lsp1335_expect_same(exePath, projDir,   "projet multi-fichiers (dossier)");
    /* Rejoue la PREMIERE cible en fin de serie : meme resident, apres 3 checks
     * differents. Une divergence ici signalerait une fuite d'etat entre
     * requetes (registre, tables compile-time), le risque propre au mode
     * resident. */
    lsp1335_expect_same(exePath, fileOk,    "fichier sain rejoue apres 3 checks");

    /* Overlay non sauve : le meme resident doit voir le BUFFER, pas le disque.
     * fichierB.owl du fixture porte sur DISQUE un appel inconnu ; l'overlay le
     * remplace par une version saine. Le spawn dedie ne sait pas porter
     * d'overlay (lsp_run_check_bounded), donc l'oracle est ici le contenu :
     * l'erreur disque doit disparaitre du verdict, et reapparaitre sans
     * overlay. */
    char pathB[600];
    snprintf(pathB, sizeof(pathB), "%s/fichierB.owl", projDir);
    for (char* p = pathB; *p; p++) if (*p == '\\') *p = '/';
    char overlay[2048];
    snprintf(overlay, sizeof(overlay),
             "{\"overlays\":[{\"path\":\"%s\",\"content\":\"n est un entier = 1\\nTrace(n)\\n\"}]}",
             pathB);

    char sansOverlay[65536], avecOverlay[65536];
    lsp1335_resident_check(exePath, projDir, NULL, NULL, sansOverlay, (int)sizeof(sansOverlay));
    lsp1335_resident_check(exePath, projDir, NULL, overlay, avecOverlay, (int)sizeof(avecOverlay));

    if (strstr(sansOverlay, "FonctionDisqueInconnueLsp") != NULL) {
        g_pass++;
        printf("  [OK]   fix/1335 : sans overlay, l'erreur du DISQUE est bien rapportee\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1335 : sans overlay, l'erreur disque attendue est absente : %.300s\n",
               sansOverlay);
    }
    if (strstr(avecOverlay, "FonctionDisqueInconnueLsp") == NULL &&
        strstr(avecOverlay, "\"files\"") != NULL) {
        g_pass++;
        printf("  [OK]   fix/1335 : avec overlay, le resident voit le BUFFER (erreur disque effacee)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1335 : avec overlay, le resident lit encore le disque : %.300s\n",
               avecOverlay);
    }

    /* Le resident ne doit jamais survivre au LSP : l'arret est explicite. */
    lsp_check_server_stop();
    if (!lsp_check_server_is_alive()) {
        g_pass++;
        printf("  [OK]   fix/1335 : resident arrete (aucun process owl orphelin)\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1335 : le resident tourne encore apres lsp_check_server_stop()\n");
    }

    /* REPLI : resident indisponible (ici : arrete definitivement) ne doit PAS
     * priver l'editeur de diagnostics. lsp_check_start doit retomber sur un
     * spawn dedie et rendre le meme verdict. Regression inacceptable sinon. */
    char repli[65536];
    LspRunStatus rsRepli = lsp1335_resident_check(exePath, fileErr, NULL, NULL,
                                                  repli, (int)sizeof(repli));
    lsp1335_strip_cr(repli);
    if (rsRepli == LSP_RUN_OK && !lsp_check_server_is_alive() &&
        strstr(repli, "FonctionVoisineInconnue") != NULL) {
        g_pass++;
        printf("  [OK]   fix/1335 : resident indisponible -> repli spawn, diagnostics preserves\n");
    } else {
        g_fail++;
        printf("  [FAIL] fix/1335 : resident indisponible -> plus de diagnostics (status %d) : %.300s\n",
               (int)rsRepli, repli);
    }
}

/* ================================================================
 *   -- plage d'un diagnostic couvre le MOT en erreur
 *
 *  make_lsp_diag codait en dur end.character = start+1 : double-cliquer une
 *  erreur d'identifiant dans le panneau Problems ne selectionnait que la 1re
 *  lettre. lsp_diag_word_end_char calcule l'etendue du mot a (line, col) sur
 *  un buffer et rend le end.character en unites UTF-16 (protocole LSP), -1 si
 *  la position n'est pas sur un mot (repli 1-char inchange).
 *
 *  ATTENTION unites : la colonne d'un diagnostic --check est un offset OCTET
 *  1-based (le scanner incremente sa colonne par octet). Un identifiant
 *  accentue (`Thème`, `é` = 2 octets) doit donc rendre une longueur UTF-16
 *  (5) et non octet (6). RED attendu : le stub renvoie -2.
 * ================================================================ */
#ifndef LSP_DIAG_WORD_END_IMPLEMENTED
static int lsp_diag_word_end_char(const char* content, int line1based, int col1based) {
    (void)content; (void)line1based; (void)col1based;
    return -2; /* sentinelle "pas encore implemente" */
}
#else
extern int lsp_diag_word_end_char(const char* content, int line1based, int col1based); /* impl reelle (lsp_handlers.c) */
#endif /* LSP_DIAG_WORD_END_IMPLEMENTED */

static void fix1597_check(const char* name, int got, int expected) {
    if (got == expected) {
        g_pass++;
        printf("  [OK]   fix/1597 %s : end.character=%d\n", name, got);
    } else {
        g_fail++;
        printf("  [FAIL] fix/1597 %s : attendu end.character=%d, obtenu %d\n",
               name, expected, got);
    }
}

static void test_fix1597_diag_word_range(void) {
    printf("\n[TEST] fix/1597 : plage diagnostic couvre le mot entier\n");

    /* 1) Identifiant ASCII : `bufPacket` en debut de ligne 2, col 1 (octet).
     *    end.character UTF-16 = 9 (longueur du mot), pas 1. */
    fix1597_check("ascii bufPacket",
        lsp_diag_word_end_char("x est un entier = 3\nbufPacket = 5\n", 2, 1), 9);

    /* 2) Identifiant accentue `Thème` (T h è m e) : 6 octets mais 5 unites
     *    UTF-16. La conversion octet->UTF-16 doit rendre 5, pas 6. */
    fix1597_check("accent Theme",
        lsp_diag_word_end_char("x est un entier = 3\nThème = 5\n", 2, 1), 5);

    /* 3) Accent AVANT le mot fautif sur la meme ligne : la colonne --check
     *    (octet) vaut 15 pour `malVar` dans `Trace(café + malVar)`. Le mot
     *    s'etend jusqu'a l'octet 20 -> end.character UTF-16 = 19 (et non 20 :
     *    le `é` de café compte 1 unite UTF-16 pour 2 octets). */
    fix1597_check("accent avant mot",
        lsp_diag_word_end_char(
            "café est un entier = 3\nTrace(café + malVar)\n", 2, 15), 19);

    /* 4) Repli : position sur un operateur (`+`), pas un mot -> -1 (make_lsp_diag
     *    garde le 1-char historique). */
    fix1597_check("repli operateur",
        lsp_diag_word_end_char("a + b\n", 1, 3), -1);

    /* 5) Repli : identifiant d'une seule lettre -> -1 (end = start+1 = defaut,
     *    pas d'override necessaire). */
    fix1597_check("repli mono-lettre",
        lsp_diag_word_end_char("x = 3\n", 1, 1), -1);

    /* 6) Repli : buffer absent (fichier non ouvert, cas check projet voisin). */
    fix1597_check("repli buffer null",
        lsp_diag_word_end_char(NULL, 1, 1), -1);

    /* 7) Repli : ligne au-dela du buffer. */
    fix1597_check("repli ligne hors buffer",
        lsp_diag_word_end_char("a\n", 5, 1), -1);
}

/* ================================================================
 *   -- verrou PIPELINE (pas seulement la fonction pure)
 *
 *  test_fix1597_diag_word_range verrouille lsp_diag_word_end_char (logique
 *  pure sur un buffer synthetique) mais PAS le cablage reel de publication :
 *    publish_compiler_diagnostics -> apply_check_result
 *      -> diag_word_range_for_uri (find_doc + compute_word_range_utf16)
 *      -> diag_cache_add(endChar) -> lsp_get_compiler_diag_end_char
 *  Debrancher diag_word_range_for_uri d'un des 4 sites de publication ne
 *  ferait rougir aucun test sans ce verrou (Note reviewer ).
 *
 *  On rejoue la fixture script_neighbor : le voisin FEN_Voisin.owl
 *  porte l'erreur stable `'ColonneInexistante'` a la ligne 21 colonne 14
 *  (offset OCTET). Le mot fait 18 caracteres ASCII, donc la plage publiee
 *  doit avoir end.character = 13 (start 0-based) + 18 = 31 -- a condition que
 *  le buffer du voisin soit OUVERT (diag_word_range_for_uri exige find_doc).
 *  Sans buffer ouvert le repli -1 s'applique (cf. test 1 de ) : c'est
 *  pour ca qu'on ouvre ici le voisin comme document, contrairement au test 1.
 * ================================================================ */
static void test_fix1597_pipeline_end_char_from_open_buffer(void) {
    printf("\n[TEST] fix/1597 : plage publiee par le pipeline reel (end.character=31)\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "script_neighbor");

    char uriScript[600], uriVoisin[600];
    snprintf(uriScript, sizeof(uriScript), "file:///%s/script.owl", fixtureDir);
    snprintf(uriVoisin, sizeof(uriVoisin), "file:///%s/FEN_Voisin.owl", fixtureDir);
    for (char* p = uriScript; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = uriVoisin; *p; p++) if (*p == '\\') *p = '/';

    /* Deux buffers ouverts : script (sain) + voisin CASSE identique au disque.
     * Le voisin DOIT etre ouvert pour que diag_word_range_for_uri (find_doc)
     * calcule la plage sur son buffer -- sinon repli -1 (cas voisin non charge). */
    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 2;

    const char* srcScript =
        "// Script principal du dossier (mode SCRIPT, pas de projet.owl).\n"
        "Trace(\"ok\")\n";
    const char* srcVoisinBroken =
        "FEN_Voisin est une Fenêtre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispVerticale\n"
        "\t\tLargeur     = 400\n"
        "\t\tHauteur     = 300\n"
        "\tFIN\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tPROPRIETES\n"
        "\t\t\tÉtire = Vrai\n"
        "\t\tFIN\n"
        "\t\tCOL_A est une Colonne\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibellé = \"A\"\n"
        "\t\t\t\tType    = colTypeTexte\n"
        "\t\t\t\tLargeur = 100\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tPROCEDURE Peindre()\n"
        "\t\ti est un entier = 1\n"
        "\t\tTABLE_X[i].ColonneInexistante = \"valeur bidon\"\n"
        "\tFIN\n"
        "FIN\n";

    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uriScript);
    lspServer.documents[0].content = strdup(srcScript);
    lspServer.documents[0].contentLen = (int)strlen(srcScript);
    lspServer.documents[0].version = 1;

    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", uriVoisin);
    lspServer.documents[1].content = strdup(srcVoisinBroken);
    lspServer.documents[1].contentLen = (int)strlen(srcVoisinBroken);
    lspServer.documents[1].version = 1;

    lspServer.diagnosedUriCount = 0;
    snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", fixtureDir);

    /* didSave du script -> check projet -> le voisin casse est publie. */
    publish_compiler_diagnostics(uriScript);

    int countVoisin = lsp_get_compiler_diag_count(uriVoisin);
    int endChar     = lsp_get_compiler_diag_end_char(uriVoisin, 0);

    int ok = 1;
    if (countVoisin < 1) {
        ok = 0;
        printf("  [FAIL] fix/1597 : FEN_Voisin.owl compte %d diagnostic(s) (attendu >= 1 -- "
               "diagnostic voisin jamais publie, pipeline casse en amont)\n", countVoisin);
    }
    /* Le verrou : 31 prouve que diag_word_range_for_uri est bien cable au site
     * de publication ET que la plage a traverse le cache jusqu'a l'accesseur.
     * Un debranchage rendrait -1 (repli 1-char). */
    if (endChar != 31) {
        ok = 0;
        printf("  [FAIL] fix/1597 : end.character publie = %d (attendu 31 -- "
               "col 14 octet, mot `ColonneInexistante` 18 car -> 13+18 ; -1 = "
               "diag_word_range_for_uri debranche d'un site de publication)\n", endChar);
    }

    if (ok) {
        g_pass++;
        printf("  [OK]   fix/1597 : plage pipeline end.character=31 (mot entier couvert)\n");
    } else {
        g_fail++;
    }
}

/* ================================================================
 *   (REOUVERTURE) -- mismatch d'encodage d'URI c%3A vs c:
 *
 *  VSCode envoie l'URI du document PERCENT-ENCODE (file:///c%3A/...),
 *  mais filepath_to_uri (chemin --check multi-fichiers) laisse le `:`
 *  du drive cru (file:///c:/...). find_doc fait un strcmp EXACT -> le
 *  buffer ouvert n'est pas retrouve -> compute_word_range_utf16 jamais
 *  appelee -> repli plage 1-char (end.character = 31 attendu, mais -1
 *  observe sur le code d'avant fix, publie start+1).
 *
 *  Ce test reproduit le VRAI monde : le buffer voisin est stocke avec
 *  un URI percent-encode (comme VSCode), le check derive la forme crue.
 *  RED sur l'ancien code (find_doc strcmp echoue -> end.character != 31) ;
 *  GREEN apres (resolution tolerante a l'encodage).
 * ================================================================ */
static void test_fix1597_pipeline_uri_percent_encoded_mismatch(void) {
    printf("\n[TEST] fix/1597 : buffer resolu malgre URI percent-encode (c%%3A vs c:)\n");

    char exePath[512];
    resolve_owl_exe_path(exePath, sizeof(exePath));
    lspServer.owlExePath[0] = '\0';
    snprintf(lspServer.owlExePath, sizeof(lspServer.owlExePath), "%s", exePath);

    char fixtureDir[512];
    resolve_fixture_path(fixtureDir, sizeof(fixtureDir), "script_neighbor");

    char uriScript[600], uriVoisin[600];
    snprintf(uriScript, sizeof(uriScript), "file:///%s/script.owl", fixtureDir);
    snprintf(uriVoisin, sizeof(uriVoisin), "file:///%s/FEN_Voisin.owl", fixtureDir);
    for (char* p = uriScript; *p; p++) if (*p == '\\') *p = '/';
    for (char* p = uriVoisin; *p; p++) if (*p == '\\') *p = '/';

    /* Forme PERCENT-ENCODEE facon VSCode : file:///c%3A/... (drive minuscule
     * + %3A a la place du `:`). C'est cette forme qui est stockee cote
     * document ouvert, alors que le pipeline --check derive uriVoisin (cru). */
    char uriVoisinEnc[640];
    {
        const char* rest = uriVoisin + 8; /* apres "file:///" -> "X:/..." */
        char drive = rest[0];
        if (drive >= 'A' && drive <= 'Z') drive = (char)(drive - 'A' + 'a');
        /* rest[1] == ':' garanti par la construction file:///<drive>:/... */
        snprintf(uriVoisinEnc, sizeof(uriVoisinEnc), "file:///%c%%3A%s", drive, rest + 2);
    }

    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 2;

    const char* srcScript =
        "// Script principal du dossier (mode SCRIPT, pas de projet.owl).\n"
        "Trace(\"ok\")\n";
    const char* srcVoisinBroken =
        "FEN_Voisin est une Fenêtre\n"
        "\tPROPRIETES\n"
        "\t\tDisposition = dispVerticale\n"
        "\t\tLargeur     = 400\n"
        "\t\tHauteur     = 300\n"
        "\tFIN\n"
        "\tTABLE_X est un Champ Table\n"
        "\t\tPROPRIETES\n"
        "\t\t\tÉtire = Vrai\n"
        "\t\tFIN\n"
        "\t\tCOL_A est une Colonne\n"
        "\t\t\tPROPRIETES\n"
        "\t\t\t\tLibellé = \"A\"\n"
        "\t\t\t\tType    = colTypeTexte\n"
        "\t\t\t\tLargeur = 100\n"
        "\t\t\tFIN\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tPROCEDURE Peindre()\n"
        "\t\ti est un entier = 1\n"
        "\t\tTABLE_X[i].ColonneInexistante = \"valeur bidon\"\n"
        "\tFIN\n"
        "FIN\n";

    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", uriScript);
    lspServer.documents[0].content = strdup(srcScript);
    lspServer.documents[0].contentLen = (int)strlen(srcScript);
    lspServer.documents[0].version = 1;

    /* URI stocke = forme PERCENT-ENCODEE (le mismatch reproduit). */
    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", uriVoisinEnc);
    lspServer.documents[1].content = strdup(srcVoisinBroken);
    lspServer.documents[1].contentLen = (int)strlen(srcVoisinBroken);
    lspServer.documents[1].version = 1;

    lspServer.diagnosedUriCount = 0;
    snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", fixtureDir);

    publish_compiler_diagnostics(uriScript);

    /* Le cache est keye par la forme derivee du check (uriVoisin cru), pas par
     * l'URI stocke encode -> on lit avec uriVoisin. */
    int countVoisin = lsp_get_compiler_diag_count(uriVoisin);
    int endChar     = lsp_get_compiler_diag_end_char(uriVoisin, 0);

    int ok = 1;
    if (countVoisin < 1) {
        ok = 0;
        printf("  [FAIL] fix/1597 : FEN_Voisin.owl compte %d diagnostic(s) (attendu >= 1)\n",
               countVoisin);
    }
    if (endChar != 31) {
        ok = 0;
        printf("  [FAIL] fix/1597 : end.character = %d (attendu 31 -- buffer stocke sous URI "
               "percent-encode `%s` non retrouve par find_doc exact ; -1/start+1 = repli "
               "1-char, mot `ColonneInexistante` non couvert)\n", endChar, uriVoisinEnc);
    }

    if (ok) {
        g_pass++;
        printf("  [OK]   fix/1597 : buffer resolu malgre encodage URI, end.character=31\n");
    } else {
        g_fail++;
    }
}

/* ================================================================
 *   -- doublon FR+EN dans la completion des valeurs d'enum
 *  registre (support #272, meme cause racine que le doublon owl explain
 *  couvert par tests/cli/doc_lang/enum_dup_scan.c). `.values` de errInfo
 *  porte 16 formes FR canon + 10 alias EN comme entrees SEPAREES ; la
 *  boucle call-arg (lsp_handlers.c ~L7338) emet un item PAR entree de
 *  `.values`, donc chaque concept apparait deux fois dans la liste de
 *  completion de `ErreurInfo(<cursor>)`.
 *
 *  RED-FIRST : avant impl, ces deux tests comptent 26 items au lieu de
 *  16 (FR comme EN), et en mode EN le label resolu ("errSummary") existe
 *  en double (une fois via le pairage FR->EN de emit_enum_value_item,
 *  une fois via l'alias EN nu lui-meme resolu vers lui-meme).
 * ================================================================ */

/* Compte le nombre d'items de completion dont le label vaut exactement
 * `label` (detecte les doublons qu'ASSERT_HAS ne voit pas -- ASSERT_HAS
 * s'arrete au premier match). */
static int count_label_occurrences(JsonValue* r, const char* label) {
    JsonValue* items = unwrap_items(r);
    if (!items || items->type != JSON_ARRAY) return 0;
    int n = 0;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        const char* lbl = json_get_string(items->as.arrayVal.items[i], "label");
        if (lbl && strcmp(lbl, label) == 0) n++;
    }
    return n;
}

#define ASSERT_LABEL_ONCE(items, label, test_name) do { \
    int _n = count_label_occurrences((items), (label)); \
    if (_n == 1) { \
        g_pass++; \
        printf("  [OK]   %s : '%s' present exactement 1 fois\n", test_name, label); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s : '%s' present %d fois (attendu 1)\n", \
               test_name, label, _n); \
    } \
} while (0)

/* Call-arg FR (defaut) : ErreurInfo(<cursor>) -- arg 0, allowedBase=errInfo.
 * 16 concepts distincts attendus, formes FR canon uniquement, aucun alias
 * EN nu (ex "errSummary") ne doit fuiter dans la liste FR. */
static void test_fix1777_completion_errinfo_callarg_fr_no_duplicate(void) {
    printf("\n[TEST] fix/1777 : completion ErreurInfo(<cursor>) FR sans doublon FR+EN\n");
    const char* src = "ErreurInfo()\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    /* Curseur juste apres la parenthese ouvrante : "ErreurInfo(" = 11 chars. */
    JsonValue* params = make_completion_params(URI, 0, 11);
    JsonValue* items = handle_completion(params);

    ASSERT_COUNT(items, 16,
        "completion ErreurInfo FR : 16 concepts (pas 26 FR+EN)");
    ASSERT_HAS(items, "errR\xc3\xa9sum\xc3\xa9",
        "completion ErreurInfo FR : forme FR presente (errR\xc3\xa9sum\xc3\xa9)");
    ASSERT_NOT_HAS(items, "errSummary",
        "completion ErreurInfo FR : alias EN nu absent (errSummary)");
    ASSERT_NOT_HAS(items, "errSystemCode",
        "completion ErreurInfo FR : alias EN nu absent (errSystemCode)");

    json_free(items);
    json_free(params);
}

/* Call-arg EN : ErreurInfo(<cursor>) en mode langue EN. 16 concepts
 * distincts attendus, chaque concept en forme EN, AUCUN doublon (l'alias
 * EN ne doit pas apparaitre a la fois via le pairage FR->EN et comme
 * entree nue independante). */
static void test_fix1777_completion_errinfo_callarg_en_no_duplicate(void) {
    printf("\n[TEST] fix/1777 : completion ErreurInfo(<cursor>) EN sans doublon nu\n");
    const char* src = "ErreurInfo()\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 11);
    JsonValue* items = handle_completion(params);

    ASSERT_COUNT(items, 16,
        "completion ErreurInfo EN : 16 concepts (pas 26, pas de doublon nu)");
    ASSERT_LABEL_ONCE(items, "errSummary",
        "completion ErreurInfo EN : errSummary exactement 1 fois");
    ASSERT_LABEL_ONCE(items, "errSystemCode",
        "completion ErreurInfo EN : errSystemCode exactement 1 fois");
    ASSERT_NOT_HAS(items, "errR\xc3\xa9sum\xc3\xa9",
        "completion ErreurInfo EN : forme FR absente en mode EN (errR\xc3\xa9sum\xc3\xa9)");

    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *  — garde de non-regression du VOLET COMPLETION du doublon
 * enumWindow sur la native de reflexion ElementsDe/ElementsOf.
 *
 * Le param 0 de cette native est contraint par l'enum `elementProjet`,
 * dont `.values` ne contient qu'une seule valeur bilingue (`enumFenêtre`
 * FR / `enumWindow` EN) depuis . La completion call-arg passe par
 * `emit_enum_value_item` (language-aware) : FR -> `enumFenêtre` une fois,
 * EN -> `enumWindow` une fois, JAMAIS les deux ensemble, JAMAIS de doublon.
 *
 * Support #299 rapportait la liste doublonnee (enumFenêtre ET enumWindow).
 * Mecaniquement resolu par  ; ces deux tests le verrouillent pour
 * le cas exact de l'utilisateur, jamais couvert ( ne testait
 * qu'errInfo). Buffer en forme ASCII `ElementsDe(` (le scanner desaccentue,
 * le catalogue resout la native ; evite les offsets UTF-16 des accents).
 * ================================================================ */

/* Call-arg FR (defaut) : ElementsDe(<cursor>) -- arg 0, allowedBase =
 * elementProjet. Une seule valeur attendue en forme FR canon (enumFenêtre),
 * l'alias EN nu (enumWindow) ne doit PAS fuiter. */
static void test_fix1786_completion_elementsde_callarg_fr_no_duplicate(void) {
    printf("\n[TEST] fix/1786 : completion ElementsDe(<cursor>) FR sans doublon FR+EN\n");
    const char* src = "ElementsDe()\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    /* Curseur juste apres la parenthese ouvrante : "ElementsDe(" = 11 chars. */
    JsonValue* params = make_completion_params(URI, 0, 11);
    JsonValue* items = handle_completion(params);

    ASSERT_LABEL_ONCE(items, "enumFen\xc3\xaatre",
        "completion ElementsDe FR : enumFen\xc3\xaatre exactement 1 fois");
    ASSERT_NOT_HAS(items, "enumWindow",
        "completion ElementsDe FR : alias EN nu absent (enumWindow)");

    json_free(items);
    json_free(params);
}

/* Call-arg EN : ElementsDe(<cursor>) en mode langue EN. Une seule valeur
 * attendue en forme EN (enumWindow), la forme FR (enumFenêtre) ne doit PAS
 * apparaitre, et enumWindow doit apparaitre exactement une fois (pas de
 * doublon via le pairage FR->EN + une entree nue). */
static void test_fix1786_completion_elementsde_callarg_en_no_duplicate(void) {
    printf("\n[TEST] fix/1786 : completion ElementsDe(<cursor>) EN sans doublon nu\n");
    const char* src = "ElementsDe()\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 11);
    JsonValue* items = handle_completion(params);

    ASSERT_LABEL_ONCE(items, "enumWindow",
        "completion ElementsDe EN : enumWindow exactement 1 fois");
    ASSERT_NOT_HAS(items, "enumFen\xc3\xaatre",
        "completion ElementsDe EN : forme FR absente en mode EN (enumFen\xc3\xaatre)");

    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

/* ================================================================
 *  — support #299 (v1.0.8), symptome residuel : la completion
 * de la FONCTION ÉlémentsDe/ElementsOf (pas de son argument, deja
 * verrouille par  ci-dessus) insere un snippet dont le defaut
 * d'argument enum est TOUJOURS pris depuis la forme FR canon
 * (enumFenêtre), quelle que soit la langue courante.
 * `build_native_snippet_skip()` (lsp/lsp_handlers.c) lit
 * `OwlEnumInfo.values` brut (`E_elementProjet_values` dans
 * src/stdlib_parametre.c ne contient QUE "enumFenêtre") sans jamais
 * consulter `lspServer.language`, contrairement a `emit_enum_value_item`
 * qui gere deja le call-arg de facon language-aware. En mode EN,
 * l'utilisateur voit donc `ElementsOf(enumfenetre)` insere au lieu de
 * `ElementsOf(enumWindow)` — exactement le rapport support.
 * ================================================================ */

/* FR (defaut) : le snippet de la fonction porte deja la forme FR
 * canon attendue -- comportement de reference, doit rester vert. */
static void test_fix1956_completion_elementsof_function_snippet_fr(void) {
    printf("\n[TEST] fix/1956 : snippet fonction \xc3\x89l\xc3\xa9mentsDe FR = enumFen\xc3\xaatre\n");
    const char* src = "Element\n";
    setup_doc(URI, src);
    lspServer.language[0] = '\0';

    /* Curseur en fin de prefixe "Element" (7 chars, desaccentue —
     * matche la forme canon accentuee \xc3\x89l\xc3\xa9mentsDe). */
    JsonValue* params = make_completion_params(URI, 0, 7);
    JsonValue* items = handle_completion(params);

    ASSERT_INSERT_CONTAINS(items, "\xc3\x89l\xc3\xa9mentsDe", "enumFen\xc3\xaatre",
        "snippet fonction \xc3\x89l\xc3\xa9mentsDe FR contient enumFen\xc3\xaatre");
    ASSERT_INSERT_NOT_CONTAINS(items, "\xc3\x89l\xc3\xa9mentsDe", "enumWindow",
        "snippet fonction \xc3\x89l\xc3\xa9mentsDe FR ne contient PAS enumWindow");

    json_free(items);
    json_free(params);
}

/* EN : reproduit le symptome support #299 residuel v1.0.8 -- le
 * snippet de la fonction ElementsOf doit pre-remplir enumWindow (EN),
 * pas enumFenêtre/enumfenetre (FR). ROUGE ATTENDU tant que
 * build_native_snippet_skip() ne consulte pas lspServer.language pour
 * choisir la forme d'un allowedBaseEnum a valeur unique. */
static void test_fix1956_completion_elementsof_function_snippet_en(void) {
    printf("\n[TEST] fix/1956 : snippet fonction ElementsOf EN = enumWindow (pas enumFen\xc3\xaatre)\n");
    const char* src = "Element\n";
    setup_doc(URI, src);
    snprintf(lspServer.language, sizeof(lspServer.language), "en");

    JsonValue* params = make_completion_params(URI, 0, 7);
    JsonValue* items = handle_completion(params);

    ASSERT_INSERT_CONTAINS(items, "ElementsOf", "enumWindow",
        "snippet fonction ElementsOf EN contient enumWindow");
    ASSERT_INSERT_NOT_CONTAINS(items, "ElementsOf", "enumFen\xc3\xaatre",
        "snippet fonction ElementsOf EN ne contient PAS enumFen\xc3\xaatre (FR)");

    json_free(items);
    json_free(params);
    lspServer.language[0] = '\0';
}

int main(int argc, char** argv) {
    /* Gate de parite (ticket 0326) : meme suite fmt-stability qu'owl.exe mais
     * via le backend CATALOGUE. Doit passer les memes .expected.owl, ce qui
     * prouve que owl-lsp formate byte-identique a owl.exe. */
    if (argc > 1 && strcmp(argv[1], "--self-test-fmt-stability") == 0) {
        owl_format_set_canon(owl_fmt_canon_catalog());
        return fmt_stability_self_test_run() > 0 ? 1 : 0;
    }

    /* Stub init pour les variables LSP (pas de socket). */
    memset(&lspServer, 0, sizeof(lspServer));
    event_catalog_widgets_init();

    printf("======================================\n");
    printf("  OWL LSP — Test harness in-process\n");
    printf("======================================\n");

    test_body_fenetre_completion();
    test_body_bouton_completion();
    test_completion_indirection_scope();
    test_completion_comma_trigger_indirection();
    test_completion_local_scope_filter();
    test_completion_bool_literals_lang();
    test_completion_bool_literals_partial_prefix();
    test_completion_bool_literals_partial_prefix_procedure_context();
    test_completion_bool_decl_assign_filters_catalog();
    test_completion_bool_decl_assign_block_widget_no_props();
    test_completion_decl_assign_value_no_keywords_no_types();
    /*  -- completion consciente du type en position valeur d'un
     * decl-assign (RED avant impl, sauf surete/non-regression). */
    test_completion_declassign_type_filter_boolean();
    test_completion_declassign_type_filter_entier();
    test_completion_declassign_type_filter_chaine();
    test_completion_declassign_type_filter_enum();
    test_completion_declassign_type_filter_objet_heritage();
    test_completion_declassign_type_filter_variables_scope();
    test_completion_declassign_type_filter_fallback_surete();
    test_completion_declassign_type_filter_non_regression_reassign();
    test_completion_declassign_scalaire_agressif_boolean();
    test_completion_declassign_scalaire_agressif_entier();
    test_completion_declassign_scalaire_agressif_surete();
    test_completion_declassign_scalaire_agressif_lhs_non_scalaire();
    /*  -- LHS scalaire : exclusion en bloc des champs + procedures
     * (RED avant impl, sauf bornes permissif/hors-position). */
    test_completion_2028_exclut_champs_procedures_booleen();
    test_completion_2028_exclut_champs_procedures_entier();
    test_completion_2028_lhs_non_scalaire_permissif();
    test_completion_2028_hors_position_valeur_permissif();
    test_completion_eol_constants();
    test_completion_2107_const_sorttext_alpha();
    test_completion_composed_field_lang_l1();
    test_completion_dotdot_props_lang_l2();
    /*  Lot 2 -- mise en forme conditionnelle par cellule de Table
     * (RED attendu avant impl). */
    test_completion_table_cell_couleur();
    test_completion_table_colfirst_couleur();
    test_completion_bare_column_couleur();
    /*  -- filtrage par type du receveur apres `..` + noms de
     * colonnes apres point simple sur une Table (RED avant impl). */
    test_completion_dotdot_builtin_kind_filter();
    test_completion_dotdot_widget_counts_unchanged();
    test_completion_single_dot_methods_unchanged();
    test_completion_table_columns_single_dot();
    test_completion_dot_methods_lang_l3();
    test_completion_etat_subblocks_lang_l4();
    test_body_saisie_completion();
    test_body_libelle_completion();
    test_champ_modifier_types_completion();
    test_champ_modifier_no_article_en();
    test_menu_kind_types_completion();
    test_colonne_type_completion_in_table();
    test_volet_type_completion_in_onglet();
    test_end_closer_preselect_en();
    test_loop_keyword_exact_first_en();
    test_filter_already_used_props();
    test_dot_dot_fenetre();
    test_dot_dot_saisie();
    test_dot_dot_bouton();
    test_dot_dot_libelle();
    test_dot_dot_moimeme();
    test_dot_dot_monchamp_mafenetre();
    test_dot_dot_httprequete();
    test_dot_dot_chaine_multilingue_no_primitive_props();
    test_dot_dot_date_keeps_props();
    test_dot_dot_duree_keeps_props();
    test_names_in_top_level();
    test_ouvre_filtre_etats();
    test_paren_trigger_completion();
    test_init_completion_trigger_paren();
    test_constante_block_completion();
    test_couleur_enum_exists();
    test_type_alias_en_resolves();
    test_enum_alias_en_resolves();
    test_couleur_completion_in_assignment();
    test_couleur_completion_in_label_prop();
    test_couleurfond_completion_in_label_prop();
    test_backgroundcolor_completion_in_label_prop_en();
    test_enum_prop_bare_completion_in_label();
    test_prop_name_completion_unaffected();
    test_prop_name_completion_strict_in_proprietes();
    test_margebas_completion_bare_prop_no_garbage();
    test_margebas_completion_in_proprietes_block_no_sibling_names();
    test_bottommargin_caption_columnspan_no_garbage_en();
    test_typesaisie_enum_bare_prop_completion_non_regression();
    test_prop_name_completion_before_equals_unaffected_2162();
    test_formataffichage_richtext_hidden_non_multiligne_2282();
    test_columnspan_rowspan_hidden_default_layout_2163();
    test_columnspan_rowspan_hidden_disp_libre_2163();
    test_columnspan_rowspan_offered_disp_grille_2163();
    test_columnspan_hidden_offered_en_2163();
    test_proprietes_block_strict_completion();
    test_proprietes_block_strict_completion_en();
    test_proprietes_block_completion_control_button_en();
    test_proprietes_block_completion_control_input_en();
    test_procedure_event_completion();
    test_procedure_event_filter_already_used();

    /*  — `PROCEDURE ` hors body widget : respect contexte fichier. */
    test_procedure_in_class_body_proposes_ctor_dtor();
    /*  — copule bilingue/mixte du sniff fichier classe. */
    test_procedure_in_class_body_copule_mixte();
    test_procedure_in_class_body_copule_en();
    /*  : FONCTION en body classe ne doit pas proposer ctor/dtor. */
    test_function_in_class_body_no_ctor_dtor();
    test_procedure_in_class_body_filters_existing_ctor();
    test_procedure_in_test_file_proposes_fixtures();
    test_procedure_in_standalone_module_no_test_fixtures();
    /*  — verrou regle d'or test_* (pas de detection via @sujet). */
    test_procedure_in_sujet_tagged_non_test_file_is_standalone();
    test_disposition_completion_in_window();
    test_disposition_completion_in_container();
    test_all_dsl_kinds_routable();
    test_brace_literal_fields();
    test_brace_literal_filter_used();
    test_brace_literal_enum_value();
    test_brace_literal_indent_line();
    test_brace_literal_rhs_inline_string();
    test_police_nom_proposes_iFamille();
    test_chain_from_constant_no_completion();
    test_composed_field_rhs_enum();
    test_brace_literal_after_comma();
    test_brace_literal_theme_accented_composed_after_comma();
    test_brace_literal_theme_ascii_composed_after_comma();
    test_brace_literal_style_prop_on_saisie();
    /*  — `Style..` racine amorce le type reflexif du widget. */
    test_style_root_reflexive_on_saisie();
    test_style_root_reflexive_on_bouton();
    test_style_root_reflexive_on_saisie_champ_de();
    test_style_root_reflexive_on_bouton_champ();
    test_style_second_hop_libelle_on_saisie();
    test_style_inline_subelement_image_on_bouton();
    /*  — globale fenetre proposee en RHS de prop widget (+scope). */
    test_window_global_offered_in_widget_prop_value();
    test_window_global_prop_value_scope_strict();
    test_style_inline_subelement_libelle_on_bouton();
    /*  — completion des etats valides dans `Etats[` (menu + widgets). */
    test_completion_etats_bracket_menu_optionmenu();
    test_completion_etats_bracket_widget_zonesaisie();
    test_completion_etats_bracket_widget_imagebouton();
    test_hover_user_var_shadows_const();
    test_completion_native_snippet_rich();
    test_completion_native_snippet_enum_dropdown();
    test_signature_help_multi_overload();
    test_composed_types_coherence();
    test_pour_toute_position_avec_completion();
    test_pour_toute_position_suffix_completion();
    test_avec_block_not_position_context();
    test_chaine_formate_propose_cc_options();
    test_accepts_literals_destination();

    /*  — autocomplete body class (3 scenarios). */
    test_colon_proposes_class_fields();
    test_dot_on_class_var_proposes_public_members();
    test_ident_in_method_body_mixes_locals_and_fields();

    /*  — autocomplete literal init typee (literalKeysEnum). */
    test_literal_init_chaine_multilingue();
    test_literal_init_chaine_multilingue_second_key();
    test_literal_init_type_without_enum_no_pollution();
    test_literal_init_chaine_multilingue_decl_separee();
    test_literal_init_decl_separee_non_multilingue_no_pollution();
    /*  — copule bilingue (is a/an, formes mixtes) du scan-arriere. */
    test_literal_init_chaine_multilingue_decl_en_is_a();
    test_literal_init_chaine_multilingue_decl_mixte_is_un();
    test_literal_init_chaine_multilingue_decl_en_full();
    test_literal_init_decl_en_non_multilingue_no_pollution();
    /*  — copule bilingue du scan-arriere durée. */
    test_literal_init_duree_decl_en_is_a();
    test_literal_init_duree_decl_mixte_is_un();
    test_literal_init_duree_decl_fr();
    /*  — durée literal completion registry-driven bilingue. */
    test_literal_init_duree_decl_en_type_duration();
    test_literal_init_duree_wanten_composantes_en();
    test_literal_init_duree_fr_preserved();
    test_literal_init_duree_excludes_totaux();

    /*  — filet transverse anti-fallback-aveugle. */
    test_no_type_regresses_to_primitive_fallback();

    /*  — semantic tokens classe user + methode user. */
    test_semantic_class_user_declaration_and_usage();
    /*  — parite article EN (classe / fenetre). */
    test_semantic_class_decl_en_article();
    test_semantic_window_decl_en_article();
    test_semantic_method_user_call();
    test_semantic_method_user_accent_collision();
    test_semantic_native_method_no_token();
    test_semantic_constructor_destructor_no_method_token();
    test_semantic_method_user_accented_name();

    /*  — appel direct procedure user top-level → method (vert fonce). */
    test_semantic_procedure_user_direct_call();
    test_semantic_procedure_user_recursion();
    test_semantic_native_direct_call_no_method_token();

    /*  — point d'entree Main en declaration = bleu grammar (pas method vert). */
    test_semantic_entry_point_main_decl_no_method();
    test_semantic_entry_point_main_decl_case_insensitive();
    test_semantic_entry_point_normal_proc_call_unchanged();
    test_semantic_entry_point_window_scenario();

    /*  — param homonyme d'une constante registry → variable, pas enumMember. */
    test_semantic_param_shadows_registry_constant();

    /*  rev3 — semantic tokens enum/combinaison/interface user. */
    test_semantic_enum_declaration_and_usage();
    test_semantic_combination_declaration_and_usage();
    test_semantic_class_non_regression();

    /*  rev3+4 — semantic tokens interface nom→type + acces qualifie. */
    test_semantic_interface_declaration_and_usage();
    test_semantic_enum_qualified_access();
    test_semantic_etat_bracket_enum_member();

    /*  — LSP scan widgets Liste/Image sans prefix Champ. */
    test_semantic_widget_liste_sans_champ();
    test_semantic_widget_image_sans_champ();
    test_semantic_liste_collection_non_widget();
    test_semantic_widget_combo_sans_champ_non_regression();

    /*  — LSP scan controle forme EN `is a Control <Type>`. */
    test_semantic_control_prefix_en_control();

    /*  — LSP coloration Champ Onglet / Volet / event surChangementVolet. */
    test_semantic_onglet_volet_enfant_event_fr();
    test_semantic_onglet_volet_enfant_event_en();

    /*  — `Label`/`Libellé` prop d'en-tete de colonne -> property. */
    test_semantic_column_label_property_fr();
    test_semantic_column_libelle_property_accent();
    test_semantic_libelle_declaration_stays_keyword();

    /*  — LSP scope classe / params (champs = property, params = locale). */
    test_semantic_class_field_not_global();
    test_semantic_procedure_param_local_not_global();
    test_semantic_class_constructor_param_vs_field();
    test_semantic_global_variable_non_regression();
    test_semantic_toplevel_global_local_scope();
    test_semantic_class_method_param_usage();
    test_semantic_fonction_param_shadows_registry_const();

    /*  — var de bloc widget = locale (pas cyan-globale). */
    test_semantic_widget_local_var_not_global();
    test_semantic_widget_procedure_local_non_regression();

    /*  — `:ident` (self member access OWL) = member access classe (ajuste ). */
    test_semantic_colon_member_access_classe();
    test_semantic_dot_member_access_non_regression();
    test_semantic_global_var_isolated_non_regression();

    /*  — Etat sub-blocs vert fonce + member access classe `:nSolde`. */
    test_semantic_etat_subblocks_control_names();
    test_semantic_etat_subblocks_types_preserved();
    test_semantic_composed_type_en_font();
    test_semantic_composed_type_en_container();
    test_semantic_colon_member_access_class_method();

    /*  — nom d'une declaration Projet colore comme une Fenetre. */
    test_semantic_projet_name_class_fr();
    test_semantic_projet_name_class_en();
    test_semantic_projet_non_regression_ordinary_var();

    /*  — noms ServiceREST / OpérationHTTP colores class (vert fonce). */
    test_semantic_service_rest_operation_class_fr();
    test_semantic_service_rest_operation_class_en();
    test_semantic_service_non_regression_ordinary_var();

    /*  — events natifs ServiceREST/OpérationHTTP colores event
     * natif (pas method vert). RED avant impl (catalogue events absent). */
    test_semantic_service_operation_event_not_method_fr();
    test_semantic_service_operation_event_not_method_en();

    /*  — TYPE d'une declaration Projet colore violet (type=4). */
    test_semantic_projet_type_violet_fr();
    test_semantic_projet_type_violet_en();
    test_semantic_projet_type_non_regression();

    /*  — TYPE d'une declaration Theme colore violet (type=4). */
    test_semantic_theme_type_violet_fr();
    test_semantic_theme_type_violet_en();
    test_semantic_theme_type_non_regression();

    /*  — LSP scan workspace types cross-fichier. */
    test_cross_file_type_not_opened();               /* RED attendu */
    test_cross_file_priority_opened_over_disk();     /* GUARD vert (priorite buffer>disque) */
    test_fallback_without_rootpath();                /* GUARD vert (sans rootPath) */
    test_completion_filters_non_owl_buffers();        /*  : .owlNOT ouvert exclu */

    /*  B2 — completion `theme.<prefix>` (styles du theme actif). */
    test_theme_completion_lists_styles();
    test_theme_completion_prefix_filter();
    test_theme_completion_detail_style_type();
    test_theme_completion_filtered_by_bouton();
    test_theme_completion_filtered_by_saisie();
    test_theme_completion_block_form_libelle();
    test_theme_completion_block_form_accented_kind();
    test_theme_completion_block_form_bouton();
    test_theme_completion_fallback_all_when_no_receiver();

    /*  — filterText ASCII sur types accentues dans typeDeclContext. */
    test_typedecl_primitive_chaine_filterText();
    test_typedecl_primitive_booleen_reel_filterText();
    test_typedecl_primitive_ascii_no_filterText();
    test_etat_subblock_accent_filterText();
    test_typedecl_builtin_httprequete_filterText();

    /*  — completion bilingue en contexte de declaration (RED 1-4, VERT 5). */
    test_typedecl_en_is_proposes_types();
    test_typedecl_en_is_no_native_leak();
    test_typedecl_en_is_a_proposes_types();
    test_typedecl_en_is_an_proposes_types();
    test_typedecl_en_is_str_prefix_proposes_string();
    test_typedecl_fr_est_non_regression();
    test_typedecl_fr_est_un_non_regression();

    /*  (support #213) — type-decl en signature PROCEDURE/FONCTION
     * (cas A) et pour une variable locale en corps (cas B). */
    test_typedecl_signature_param_fr();
    test_typedecl_signature_param_en();
    test_typedecl_local_var_body_fr();

    /*  (support #215) — Structure/Classe candidats type-decl. */
    test_typedecl_structure_classe_fr();
    test_typedecl_structure_classe_en();

    /*  — segmentation langue complete des mots-cles de type
     * (type-declaration + dump general), les 2 contextes. */
    test_typedecl_en_segments_types();
    test_typedecl_fr_segments_types();
    test_general_en_integ_proposes_integer();
    test_general_en_entie_no_entier();
    test_general_fr_entie_proposes_entier();

    /*  — match exact remonte en tete de la completion generique. */
    test_completion_exact_match_keyword_end_en();
    test_completion_exact_match_keyword_si_fr();

    /*  — mots-cles contextuels MoiMême/MonChamp/MaFenêtre. */
    test_completion_self_context_keywords_fr();
    test_completion_self_context_keywords_en();

    /*  — completion catalog icones. */
    test_icones_dot_empty();
    test_icones_dot_prefix_youtube();
    test_icones_dot_underscore_normalize();
    test_icones_dot_detail_categorie_owl();

    /*  — semantic tokens `icones.<id>` → enumMember (jaune). */
    test_semantic_icones_dot_id_simple();
    test_semantic_icones_dot_id_underscore();
    test_semantic_icones_dot_id_accent_case();
    test_semantic_non_icones_dot_no_enum_member();

    /*  — semantic token sur le RECEVEUR `icones` → variable
     * mods=0 (rendu blanc/neutre via theme OWL). */
    test_semantic_icones_receiver_variable_simple();
    test_semantic_icones_receiver_accent_case();
    test_semantic_non_icones_receiver_no_neutral_override();

    /*  — coloration du namespace `theme.` (calque `icones.`). */
    test_semantic_theme_dot_style_simple();
    test_semantic_theme_dot_style_accent_case();
    test_semantic_non_theme_dot_no_override();
    test_semantic_projet_theme_prop_is_property();

    /*  — finitions IDE theme : borne token nom de bloc accentue,
     * type styleXxx en violet dans le bloc Theme, props Projet en vert fonce. */
    test_semantic_theme_block_name_length_utf16();
    test_semantic_theme_style_type_violet();
    test_semantic_projet_prop_names_property();

    /*  — reference d'instance de theme (coloration + go-to-def). */
    test_semantic_theme_ref_variable_global();
    test_definition_theme_ref_to_companion();

    /*  — globale cross-fichier (nav + coloration) compagnon FERME. */
    test_definition_global_ref_closed_theme();
    test_semantic_global_ref_closed_theme();
    test_definition_global_ref_closed_class_instance();
    test_semantic_global_ref_closed_class_instance();
    test_definition_window_ref_open();
    test_definition_window_ref_closed();
    test_definition_etat_ref_closed();

    /*  — catalogue lifecycle Fenetre (consolidation hardcode). */
    test_lifecycle_fenetre_autocomplete();
    test_lifecycle_apres_impression_not_proposed();

    /*  — coloration + navigation des symboles de collection. */
    test_collection_1178_decl_class();
    test_collection_1178_qualified_member();
    test_collection_1178_proc_call_nu_same_file();
    test_collection_1178_enum_member_italic_same_file();
    test_collection_1178_crossfile_tokens();
    test_collection_1178_crossfile_definition();
    test_1219_crossfile_native_collision_tokens();
    test_1219_crossfile_native_collision_definition();
    test_collection_1178_no_regression_plain_enum_window();
    test_collection_1178_enum_keyword_violet_both_contexts();
    test_collection_1178_qualified_enum_member_scoped();

    /*  — event Terminaison du bloc CollectionDeProcedures. */
    test_collection_terminaison_completion_fr();
    test_collection_terminaison_completion_en();
    test_collection_terminaison_hover();
    test_collection_terminaison_flat_file_not_event();
    test_collection_terminaison_colored_as_event();

    /*  — autocomplete event : detection `()` apres curseur. */
    test_event_typed_paren_follows_inserts_name_only();
    test_event_typed_no_paren_inserts_full_snippet();
    test_event_already_declared_filtered_when_paren_follows();
    test_lifecycle_paren_follows_inserts_name_only();

    /*  — scope filtre etendu a tout le bloc widget courant. */
    test_event_already_declared_after_cursor_same_widget();
    test_event_already_declared_scope_sibling_widget();

    /*  — sous-controles Etat ('l' LibelleEtat, 'm' ImageEtat) :
     * block_start/block_end coherents → filtre event au bon scope. */
    test_event_scope_sibling_libelle_etat();
    test_event_already_declared_libelle_etat();
    test_event_autocomplete_image_etat();
    test_event_scope_sibling_image_etat();

    /*  — repro fichier reel ETAT_Demo.owl : `est un Champ Image`
     * (prefixe Champ) + multi-champs + events pre-declares chez voisins. */
    test_event_champ_image_prefix_proposed();
    test_event_champ_image_multi_siblings_etatdemo();
    test_event_champ_image_multi_siblings_etatdemo_logohomo();
    test_event_libelle_etat_siblings_with_pre_declared_events();
    test_event_champ_image_already_declared_in_self_filtered();

    /*  — events du Champ Onglet (genre sans token scanner dedie)
     * + equilibrage du sous-bloc Volet dans la pile de contexte. */
    test_event_autocomplete_onglet();
    test_event_autocomplete_apres_bloc_onglet();
    test_event_scope_sibling_onglet();
    test_event_already_declared_onglet();
    test_event_autocomplete_bouton_dans_volet();
    test_event_autocomplete_onglet_en();

    /*  — filler `de`/`of` apres le modificateur `Champ`/`Control` :
     * l'ouverture du sous-controle n'etait pas comptee alors que sa FIN
     * l'etait -> bloc widget borne trop tot -> event deja declare plus bas
     * dans le meme bloc repropose. */
    test_event_already_declared_onglet_champ_de_saisie();
    test_event_already_declared_onglet_saisie_directe();
    test_event_already_declared_onglet_geometrie_demo();
    test_event_scope_sibling_onglet_champ_de_saisie();
    test_event_autocomplete_champ_de_saisie_dans_volet();
    test_event_scope_sibling_onglet_en_control();
    test_event_already_declared_onglet_en_control_of();

    /*  — garde de coherence inter-chemins : le prefixe de
     * declaration d'un champ (modificateur + filler) est desormais un
     * helper unique consomme par les quatre scans. */
    test_champ_prefix_coherence_inter_chemins();
    test_champ_prefix_collection_de_non_traverse();

    /*  — props du Champ Onglet / du Volet dans un bloc PROPRIETES
     * (resolution du genre englobant derivee du catalogue). */
    test_props_autocomplete_onglet();
    test_props_autocomplete_volet();
    test_props_autocomplete_onglet_en();
    test_props_autocomplete_bouton_dans_volet();
    test_props_autocomplete_fenetre_apres_bloc_onglet();
    test_props_autocomplete_conteneur();

    /*  — cluster qualite auto-completion. */
    test_tableau_de_proposes_types_not_natives();
    test_liste_de_proposes_types();
    test_fluent_method_drops_receiver_param();
    test_fluent_method_hover_shows_params();
    test_brace_literal_registered_type_fields();
    test_brace_literal_same_line_after_accented_type();
    test_prop_value_icone_offers_icones_namespace();

    /*  — diagnostics flicker : cache compiler_diag per-URI (RED attendu). */
    test_diag_compiler_cache_persists_after_didopen();
    test_diag_scanner_errors_dont_erase_compiler_cache();
    test_diag_cache_cleared_when_check_clean();

    /*  — go-to-definition POO (C1-C4 rouges attendus, C5 vert). */
    test_definition_class_name();
    test_definition_enum_name();
    test_definition_class_member();
    test_definition_inherited_member();
    test_definition_unknown_symbol_null();

    /*  Lot 0 — moteur de portee/liaison scope-aware.
     * C1 (portees sœurs, verrou central) et suite : RED attendus avant
     * impl de lsp_scope.{c,h}. */
    test_definition_scope_sibling_locals();
    test_definition_scope_sibling_params();
    test_definition_scope_global_vs_class_member();
    test_definition_scope_cross_file_global();
    test_definition_scope_accent_case_insensitive();

    /*  Lot 1 — textDocument/references scope-aware.
     * Stub handle_references renvoie [] -> tous RED attendus (cardinal
     * 0 vs N). Logique de collecte occurrences[] laissee a l'implementer. */
    test_references_scope_sibling_locals_isolated();
    test_references_scope_exclude_declaration();
    test_references_scope_parameter();
    test_references_scope_global_multi_procedure();

    /*  Lot 2a — textDocument/rename + prepareRename scope-aware
     *, limite aux locales/parametres. Stubs renvoient null ->
     * RED attendu sur C1-C4 (exigent un resultat non-null). C6 est
     * VERT d'emblee (refus native deja matche par le stub null). */
    test_rename_scope_sibling_locals_isolated();
    test_rename_scope_parameter();
    test_rename_verbatim_no_canon();
    test_prepare_rename_allows_local();
    test_prepare_rename_allows_global_xfile();
    test_prepare_rename_refuses_native();

    /*  Lot 2b — rename etendu aux globales projet-wide (ouverts +
     * fermes). Handlers actuels refusent encore KIND_TOPLEVEL -> RED
     * attendu sur les 3 cas. */
    test_rename_global_cross_file();
    test_rename_global_isolation_distinct();
    test_rename_global_does_not_affect_local();

    /*  — controles prefixes Champ indexes cross-fichier (GUARD vert). */
    test_cross_file_champ_prefixed_controls();

    /*  — hover procedure/fonction -> signature (RED avant impl). */
    test_hover_proc_signature();

    /*  — hover EN quand language=en (RED attendu avant impl). */
    test_hover_native_en();
    test_hover_native_fr_default();
    test_hover_native_signature_name_en();
    test_hover_native_signature_name_fr_default();
    test_hover_method_en();
    test_hover_method_fr_default();

    /*  items 1&2 — hover `Style..` menu-aware (RED avant impl). */
    test_hover_menu_style_chain_principal();
    test_hover_menu_style_chain_contextuel();

    /*  — hover propriete de bloc Projet (support #253). */
    test_hover_projet_property_en();
    test_hover_projet_property_fr_default();
    test_hover_projet_property_second();
    test_hover_projet_property_full_en();

    /*  — hover EN valeur enum : soeurs en noms EN (RED avant impl). */
    test_hover_enum_sibling_values_en();
    test_hover_enum_sibling_values_fr_default();

    /*  — completion EN const/type quand language=en (RED avant impl). */
    test_completion_const_en();
    test_completion_const_fr_default();
    test_completion_type_en();
    test_completion_type_fr_default();
    test_completion_const_en_fallback();

    /*  — purge variant/octet de la completion de type. */
    test_completion_no_variant_octet_general();
    test_completion_no_variant_octet_typedecl();

    /*  — typedecl propose Fenetre/Etat avec snippet auto-FIN. */
    test_completion_typedecl_fenetre_etat_snippet();

    /*  (support #251) — snippets auto-FIN manquants sur
     * widgets/blocs menu/sous-blocs Etat/QUAND/Theme (RED avant impl). */
    test_champ_modifier_types_snippet_auto_fin();
    test_menu_kind_types_snippet_auto_fin();
    test_completion_etat_subblocks_snippet_auto_fin();
    test_completion_quand_snippet_auto_fin();
    test_completion_typedecl_theme_snippet_auto_fin();

    /*  — typedecl EN des types composes (nameEN). */
    test_completion_typedecl_composed_type_en();
    test_completion_composed_type_en_font_filtertext();

    /*  — completion EN mots-cles labels + snippets (RED avant impl). */
    test_completion_keyword_label_en();
    test_completion_keyword_label_fr_default();
    test_completion_propriete_singular_removed();
    test_completion_keyword_snippet_en();
    test_completion_keyword_snippet_fr_default();

    /*  — completion EN props DSL + valeurs enum (RED avant impl). */
    test_completion_props_window_en();
    test_completion_props_window_fr_default();
    test_completion_enum_values_window_en();
    test_completion_enum_values_window_fr_default();

    /*  — completion EN valeurs enum hors DSL-PROPRIETES (RED avant impl). */
    test_completion_enum_callarg_en();
    test_completion_enum_callarg_fr_default();
    test_completion_enum_simpleassign_en();
    test_completion_enum_simpleassign_fr_default();
    test_completion_enum_pourtoute_en();
    test_completion_enum_pourtoute_fr_default();

    /*  — single-wrap gros contexte + enum-param sur sig surchargee. */
    test_completion_single_wrap_large_context();
    test_completion_enum_callarg_overloaded_sig();

    /*  — completion + hover EN des events DSL (RED avant impl). */
    test_completion_event_widget_en();
    test_completion_event_widget_fr_default();
    test_completion_event_widget_en_insert();
    test_completion_event_window_lifecycle_en();
    test_completion_event_window_lifecycle_fr_default();
    /*  — filtre bilingue des events deja declares. */
    test_procedure_event_filter_already_used_lifecycle_en();
    test_procedure_event_filter_widget_already_used_en();
    test_procedure_event_filter_fr_decl_en_mode();
    /*  — completion events ServiceREST/OpérationHTTP filtree,
     * pas de deversement generique. RED avant impl (catalogues +
     * routage bloc 's'/'h' absents). */
    test_completion_event_operation_http_fr();
    test_completion_event_operation_http_en();
    test_completion_event_service_rest_fr();
    test_completion_event_service_rest_filter_already_used_fr();
    /*  — snippets de completion EN : mot-cle de fermeture END
     * (pas FIN) et article `a` (pas un/une). RED avant impl. */
    test_completion_lifecycle_event_en_end_keyword();
    test_completion_lifecycle_event_fr_fin_keyword();
    test_completion_typed_widget_event_en_end_keyword();
    test_completion_projet_event_en_end_keyword();
    test_completion_class_event_en_end_keyword();
    test_completion_test_fixture_en_end_keyword();
    test_completion_typedecl_article_en();
    test_completion_typedecl_article_fr();
    /*  (support #271) — plus d'article de confort en EN (toutes
     * familles), FR inchange. */
    test_fix1641_typedecl_no_article_en();
    test_fix1641_typedecl_article_fr_baseline();
    /*  — digue anti-fuite linguistique generalisee (cf. section
     * dediee plus haut). RED attendu sur le site nomme par le ticket
     * (Structure utilisateur) + fuite additionnelle (detail type primitif). */
    test_leak_userstruct_declaration_en();
    test_leak_userstruct_declaration_fr_baseline();
    test_leak_primitive_typedecl_detail_en();
    test_leak_lifecycle_event_en_clean();
    test_leak_test_fixture_en_clean();
    /*  — digue signatureHelp : la doc de signature doit basculer
     * FR/EN comme hover/completion. RED attendu tant que handle_signature_help
     * emet fn->description en dur. */
    test_leak_signaturehelp_en();
    test_leak_signaturehelp_fr_baseline();
    /*  — digue completion des membres d'un type Police (apres `..`).
     * RED attendu tant que le chemin registry emet les labels/descs FR en dur. */
    test_leak_font_member_completion_en();
    test_leak_font_member_completion_fr_baseline();
    /*  — completion events fenetre full-English (`is a Window`). */
    test_procedure_event_completion_full_english();
    test_procedure_event_completion_full_french();
    /*  — audit exhaustif dedup event_already_declared (angles morts). */
    test_dedup_typed_widget_events_en();
    test_dedup_lifecycle_with_body_en();
    test_dedup_event_declared_after_cursor_en();
    test_dedup_multi_widget_isolation_en();
    test_dedup_mixed_fr_decl_english_header_en();
    test_hover_event_en();
    test_hover_event_fr_default();

    /*  — hover event contextuel (collision Fermeture/Closing). */
    test_hover_fermeture_fenetre_fr();
    test_hover_closing_fenetre_en();
    test_hover_fermeture_etat_fr();
    test_hover_fermeture_top_level_fr();
    test_hover_fininitialisation_fenetre_unchanged();

    /*  — derniers trous bilingues : props DSL hover + mots-cles
     * completion/hover EN (RED avant impl). */
    test_hover_dsl_prop_en();
    test_hover_dsl_prop_fr_default();
    test_completion_keyword_en();
    test_completion_keyword_fr_default();
    test_hover_keyword_en();
    test_hover_keyword_fr_default();
    test_completion_keyword_global_fr();
    test_hover_keyword_global_fr();

    /*  — hover bilingue corps (type/native/event) : RED avant impl. */
    test_hover_type_body_en();
    test_hover_type_body_fr_default();

    /*  — hover signature de callback (..SurToken/..SurÉpoque). */
    test_hover_callback_sig_fr();
    test_hover_callback_sig_en();
    test_hover_callback_sig_multi();
    test_hover_callback_sig_zero_param();
    test_hover_non_callback_field_unchanged();
    test_hover_native_return_en();
    test_hover_native_return_fr_default();
    test_hover_event_param_type_en();
    test_hover_event_param_type_fr_default();

    /*  -- documentSymbol + foldingRange Fenetre (RED attendus avant impl). */
    test_init_document_symbol_provider();
    test_init_folding_range_provider();
    test_doc_symbol_fenetre_vide();
    test_doc_symbol_un_champ_sans_event();
    test_doc_symbol_champ_avec_event();
    test_doc_symbol_event_fenetre();
    test_doc_symbol_complet();
    /*  -- commentaires blocs neutralises avant split_lines (RED avant
     * impl sur les cas tronque + code-apres-fermeture). */
    test_fix1281_block_comment_fake_end_tronque();
    test_fix1281_block_comment_mono_ligne();
    test_fix1281_code_apres_fermeture_meme_ligne();
    test_fix1281_non_regression_sans_bloc();
    /*  Q1+Q2 -- SymbolKind + Declarations globales (RED attendus). */
    test_doc_symbol_kinds();
    test_doc_symbol_declarations_globales();
    test_doc_symbol_no_decl_globales_vide();
    test_doc_symbol_no_decl_globales_1champ();
    test_doc_symbol_declarations_globales_window_en();
    test_doc_symbol_declaration_sans_article();
    test_folding_fenetre_vide();
    test_folding_un_champ();
    test_folding_champ_avec_event();
    test_folding_complet();

    /*  -- documentSymbol + foldingRange Etat et module/script (RED attendus). */
    test_doc_symbol_etat_vide();
    test_doc_symbol_etat_complet();
    test_doc_symbol_etat_accent();
    test_doc_symbol_etat_en();
    test_doc_symbol_etat_kind();
    test_folding_etat_vide();
    test_folding_etat_complet();
    test_doc_symbol_module_3procs();
    test_doc_symbol_module_proc_prive();
    test_doc_symbol_module_fonction();
    test_doc_symbol_module_classe();
    test_doc_symbol_module_classe_en();
    test_doc_symbol_module_vide();
    test_doc_symbol_module_kinds();

    /*  -- fix FIN end.character (3 tests, 4 assertions RED). */
    test_doc_symbol_fin_char_fenetre();
    test_doc_symbol_fin_char_widget();
    test_doc_symbol_fin_char_procedure();
    /*  v2 -- Diagnostics surs + robustesse scheme + lock cross-file. */
    test_diag_no_spawn_on_nonfile_scheme();   /* RED attendu avant impl guard */
    test_completion_scheme_robustness();       /* VERT attendu -- lock */
    test_cross_file_context_lock();            /* VERT attendu -- lock */

    /*  -- sections d'etat comme conteneurs (RED attendus avant impl). */
    test_doc_symbol_etat_sections_hierarchy();
    test_doc_symbol_etat_sections_ranges();
    test_doc_symbol_etat_sections_not_in_decl_glob();

    /*  -- bloc Projet : completion champs/langues + hover (RED
     * attendus avant impl, cf. tests eux-memes). */
    test_completion_projet_fields_proposes_catalog();
    test_completion_projet_fields_filters_used();
    test_completion_projet_langue_scalar();
    test_completion_projet_langue_list_bracket();
    test_completion_projet_theme_proposes_instances();
    test_completion_projet_fenetre_demarrage_proposes_instances();
    test_completion_projet_description_no_field_names();
    test_hover_projet_field_langues();
    test_hover_projet_field_description();

    /*  volet B -- Fermeture comme event projet (RED attendus
     * avant impl, cf. tests eux-memes). */
    test_completion_projet_toplevel_proposes_fermeture();
    /*  — copule mixte du sniff bloc Projet (source_declares_projet). */
    test_completion_projet_toplevel_copule_mixte();
    test_hover_fermeture_projet_toplevel();

    /*  -- completion regime >300 : prefix-first PUIS contains
     * (RED attendus sauf garde-fou 1 caractere, non-regression VERT). */
    test_completion_over300_keeps_substring_match();
    test_completion_over300_prefix_sorted_before_contains();
    test_completion_over300_single_char_stays_prefix_only();

    /*  -- documentSymbol enrichi (detail widget + script + projet).
     * RED attendus avant impl (detail absent, script vars/enum/const non captes,
     * projet.owl vide). */
    test_doc_symbol_field_detail_widget_type();
    test_doc_symbol_field_detail_en();
    test_doc_symbol_script_vars();
    test_doc_symbol_script_enum();
    test_doc_symbol_script_constants();
    test_doc_symbol_script_mixed_no_regress();
    test_doc_symbol_projet();
    test_doc_symbol_projet_en();

    /*  -- documentSymbol : fichier Theme (racine Theme -> styles nommes,
     * PAS le contenu du litteral). */
    test_doc_symbol_theme();
    test_doc_symbol_theme_en();

    /*  -- documentSymbol : code d'initialisation top-level agrege
     * (symbole Event localise FR/EN, RED attendus avant impl). */
    test_doc_symbol_module_init_fr();
    test_doc_symbol_module_init_none();
    test_doc_symbol_module_init_en();
    test_doc_symbol_collection_init_fr();
    test_doc_symbol_collection_init_none();

    /*  -- structure imbriquee dans une collection : noeud conteneur
     * Struct(23) portant ses champs en enfants, pas a plat sous Decl.globales. */
    test_doc_symbol_collection_nested_struct();

    /*  -- gros fichiers : les tableaux a capacite fixe droppaient
     * silencieusement les symboles (procedures apres 128 membres, items
     * apres 256). RED attendu avant passage en croissance dynamique. */
    test_doc_symbol_collection_large_no_drop();
    test_doc_symbol_module_large_no_drop();

    /*  -- deadlock/troncature runner sur sortie --check volumineuse
     * (RED attendu avant fix : buffer fixe 8192o cote appelant). */
    test_fix1245_check_bounded_large_output_no_deadlock();

    /*  Lot 2 -- overlay LSP construit depuis les buffers ouverts
     * (RED attendu avant impl : le check spawn lit encore le disque). */
    test_feat1246_lot2_overlay_reflects_open_buffer_not_disk();

    /*  -- mode script doit checker le dossier (voisins non-recursifs),
     * pas le fichier seul (RED attendu avant impl). */
    test_feat1263_script_mode_checks_folder_neighbor_seen();

    /*  -- un timeout de check ne doit pas clobberer les diagnostics
     * du fichier actif (comportement deja implemente, verrouille ici). */
    test_fix2126_timeout_no_clobber_active_diagnostics();

    /*  -- formule pure du timeout adaptatif du check projet
     * (octets -> ms, plancher/plafond/arrondi). RED avant impl : la fonction
     * lsp_adaptive_timeout_for_bytes n'est pas encore exposee. */
    test_fix2133_adaptive_timeout_for_bytes();

    /*  Lot A -- verrou observable : erreur voisin navigable a sa
     * vraie position + cycle de vie (diagnostic efface une fois repare).
     * RED attendu sur la navigabilite (accesseur ligne/colonne pas encore
     * expose par l'implementeur). */
    test_fix1471_script_neighbor_navigable_at_real_position();
    test_fix1471_script_neighbor_diagnostic_cleared_after_fix();

    /*  -- la plage d'un diagnostic couvre le mot en erreur (etendue
     * calculee sur le buffer + conversion octet->UTF-16). RED attendu tant
     * que lsp_diag_word_end_char n'est pas implemente (stub -2). */
    test_fix1597_diag_word_range();

    /*  -- verrou pipeline : la plage etendue traverse reellement
     * publish_compiler_diagnostics -> cache -> accesseur (le test pur
     * ci-dessus ne couvrait que la fonction isolee). */
    test_fix1597_pipeline_end_char_from_open_buffer();
    test_fix1597_pipeline_uri_percent_encoded_mismatch();

    /*  -- doublon FR+EN dans la completion des valeurs d'enum
     * (ErreurInfo call-arg). RED attendu tant que .values conserve les
     * alias EN redondants et que la completion les emet bruts. */
    test_fix1777_completion_errinfo_callarg_fr_no_duplicate();
    test_fix1777_completion_errinfo_callarg_en_no_duplicate();
    test_fix1786_completion_elementsde_callarg_fr_no_duplicate();
    test_fix1786_completion_elementsde_callarg_en_no_duplicate();
    test_fix1956_completion_elementsof_function_snippet_fr();
    test_fix1956_completion_elementsof_function_snippet_en();

    /*  Lot 2a -- equivalence resident vs spawn. EN DERNIER : le test
     * finit par arreter definitivement le resident (verification anti-orphelin),
     * ce qui ferait retomber les tests suivants sur le spawn par check. */
    test_fix1335_lot2a_resident_equivalence();

    printf("\n======================================\n");
    printf("  RESULTAT : %d cas — %d OK / %d FAIL\n", g_pass + g_fail, g_pass, g_fail);
    printf("======================================\n");

    /* Garde ensemble vide : "0 OK / 0 FAIL" est un vert vide. */
    if (g_pass + g_fail == 0) {
        printf("  CORPUS VIDE : 0 cas execute. Un controle qui n'examine rien "
               "ne prouve rien.\n");
        return 1;
    }
    return g_fail > 0 ? 1 : 0;
}

/* ================================================================
 *   -- documentSymbol + foldingRange (tests TDD, RED attendus)
 *
 *  Tous ces tests sont ROUGES avant impl (handle_document_symbol et
 *  handle_folding_range retournent NULL dans le stub).
 *
 *  NEEDS_INPUT identifie dans ces tests (a trancher par coordinator) :
 *    1. SymbolKind pour Fenetre (Class=5 ? Module=2 ?), Champ (Field=8 ?
 *       Property=7 ?), procedure d'evenement (Method=6 ? Event=24 ?).
 *       Les tests ci-dessous ne testent PAS le SymbolKind pour ne pas
 *       lier l'impl a un choix non valide par le coordinator.
 *    2. Noeud "declarations globales" : si la fenetre contient des
 *       variables locales (ex : nCompteur sur la fixture complet), faut-il
 *       un noeud enfant dedie ? Quel nom ? Non teste ici par precaution.
 *    3. Range d'un champ : inclut-il les PROCEDURE imbriquees ?
 *       Hypothese retenue dans les tests : OUI (range couvre tout le bloc
 *       jusqu'au FIN du champ).
 *
 * ================================================================ */

/* ----- 1. Capacites announce dans initialize ----------------------- */

static void test_init_document_symbol_provider(void) {
    printf("\n[TEST] feat/0751 initialize annonce documentSymbolProvider\n");
    JsonValue* params = json_new_object();
    JsonValue* result = handle_initialize(params);
    JsonValue* caps = result ? json_get(result, "capabilities") : NULL;
    bool has = caps && json_get(caps, "documentSymbolProvider") != NULL;
    if (has) {
        g_pass++;
        printf("  [OK]   feat/0751 : documentSymbolProvider annonce dans caps\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0751 : documentSymbolProvider ABSENT des capabilities"
               " (non impl)\n");
    }
    if (result) json_free(result);
    json_free(params);
}

static void test_init_folding_range_provider(void) {
    printf("\n[TEST] feat/0751 initialize annonce foldingRangeProvider\n");
    JsonValue* params = json_new_object();
    JsonValue* result = handle_initialize(params);
    JsonValue* caps = result ? json_get(result, "capabilities") : NULL;
    bool has = caps && json_get(caps, "foldingRangeProvider") != NULL;
    if (has) {
        g_pass++;
        printf("  [OK]   feat/0751 : foldingRangeProvider annonce dans caps\n");
    } else {
        g_fail++;
        printf("  [FAIL] feat/0751 : foldingRangeProvider ABSENT des capabilities"
               " (non impl)\n");
    }
    if (result) json_free(result);
    json_free(params);
}

/* ----- 2. textDocument/documentSymbol ------------------------------ */

/* Fixture "fenetre vide" :
 *   line 0 : FEN_Vide est une Fenetre
 *   line 1 : FIN
 * Attendu : 1 symbole racine FEN_Vide, 0 enfants.
 */
static void test_doc_symbol_fenetre_vide(void) {
    printf("\n[TEST] feat/0751 documentSymbol : Fenetre vide -> racine seule\n");
    const char* src =
        "FEN_Vide est une Fenetre\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* RED : result doit etre un tableau de 1 element. */
    ASSERT_SYM_ARRAY_COUNT(result, 1, "0751/fen-vide : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "FEN_Vide", "0751/fen-vide : racine = FEN_Vide");

    /* Ces assertions s'executent apres impl quand FEN_Vide est trouve. */
    {
        JsonValue* fen = find_sym(result, "FEN_Vide");
        if (fen) {
            /* Fenetre vide : pas d'enfants champ/evenement. */
            ASSERT_SYM_CHILDREN_COUNT(fen, 0, "0751/fen-vide : 0 enfants");
            /* Range couvre les 2 lignes. */
            ASSERT_SYM_LINE(fen, 0, 1, "0751/fen-vide : FEN_Vide range [0..1]");
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "1 champ sans evenement" :
 *   line 0 : FEN_UnChamp est une Fenetre
 *   line 1 :   BTN_OK est un Champ Bouton
 *   line 2 :     PROPRIETES
 *   line 3 :       Libelle = "OK"
 *   line 4 :     FIN
 *   line 5 :   FIN
 *   line 6 : FIN
 * Attendu : FEN_UnChamp racine, BTN_OK enfant direct, 0 sous-enfants BTN_OK.
 */
static void test_doc_symbol_un_champ_sans_event(void) {
    printf("\n[TEST] feat/0751 documentSymbol : 1 champ sans evenement\n");
    const char* src =
        "FEN_UnChamp est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0751/1champ : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "FEN_UnChamp", "0751/1champ : racine = FEN_UnChamp");

    {
        JsonValue* fen = find_sym(result, "FEN_UnChamp");
        JsonValue* fen_ch = sym_children(fen);
        /* BTN_OK doit etre enfant direct de la fenetre. */
        ASSERT_SYM_EXISTS(fen_ch, "BTN_OK", "0751/1champ : BTN_OK enfant direct");

        JsonValue* btn = find_sym(fen_ch, "BTN_OK");
        if (btn) {
            /* BTN_OK sans procedure : 0 enfants. */
            ASSERT_SYM_CHILDREN_COUNT(btn, 0, "0751/1champ : BTN_OK 0 enfants");
            /* BTN_OK range : ligne 1 (declaration) a ligne 5 (FIN champ). */
            ASSERT_SYM_LINE(btn, 1, 5, "0751/1champ : BTN_OK range [1..5]");
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "champ avec surClic" :
 *   line  0 : FEN_AvecEvent est une Fenetre
 *   line  1 :   BTN_OK est un Champ Bouton
 *   line  2 :     PROPRIETES
 *   line  3 :       Libelle = "OK"
 *   line  4 :     FIN
 *   line  5 :   (vide)
 *   line  6 :     PROCEDURE surClic()
 *   line  7 :       Trace("clic")
 *   line  8 :     FIN
 *   line  9 :   FIN
 *   line 10 : FIN
 * Attendu : BTN_OK a surClic comme enfant, range surClic [6..8].
 */
static void test_doc_symbol_champ_avec_event(void) {
    printf("\n[TEST] feat/0751 documentSymbol : champ avec surClic\n");
    const char* src =
        "FEN_AvecEvent est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tTrace(\"clic\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "FEN_AvecEvent", "0751/event : racine FEN_AvecEvent");

    {
        JsonValue* fen    = find_sym(result, "FEN_AvecEvent");
        JsonValue* fen_ch = sym_children(fen);
        JsonValue* btn    = find_sym(fen_ch, "BTN_OK");
        JsonValue* btn_ch = sym_children(btn);

        ASSERT_SYM_EXISTS(btn_ch, "surClic", "0751/event : surClic enfant de BTN_OK");

        JsonValue* sc = find_sym(btn_ch, "surClic");
        if (sc) {
            ASSERT_SYM_LINE(sc, 6, 8, "0751/event : surClic range [6..8]");
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "evenement au niveau fenetre" :
 *   line 0 : FEN_Init est une Fenetre
 *   line 1 : (vide)
 *   line 2 :   PROCEDURE FinInitialisation()
 *   line 3 :     Trace("init")
 *   line 4 :   FIN
 *   line 5 : FIN
 * Attendu : FinInitialisation enfant direct de FEN_Init, range [2..4].
 */
static void test_doc_symbol_event_fenetre(void) {
    printf("\n[TEST] feat/0751 documentSymbol : FinInitialisation au niveau fenetre\n");
    const char* src =
        "FEN_Init est une Fenetre\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tTrace(\"init\")\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "FEN_Init", "0751/fen-event : racine FEN_Init");

    {
        JsonValue* fen = find_sym(result, "FEN_Init");
        JsonValue* ch  = sym_children(fen);

        ASSERT_SYM_EXISTS(ch, "FinInitialisation",
            "0751/fen-event : FinInitialisation enfant direct");

        JsonValue* fi = find_sym(ch, "FinInitialisation");
        if (fi) {
            ASSERT_SYM_LINE(fi, 2, 4, "0751/fen-event : FinInitialisation range [2..4]");
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "hierarchie complete" (N champs + events fenetre + events champ) :
 *   line  0 : FEN_Complet est une Fenetre
 *   line  1 :   nCompteur est un entier = 0        <- decl locale
 *   line  2 : (vide)
 *   line  3 :   BTN_Valider est un Champ Bouton
 *   line  4 :     PROPRIETES
 *   line  5 :       Libelle = "Valider"
 *   line  6 :     FIN
 *   line  7 : (vide)
 *   line  8 :     PROCEDURE surClic()
 *   line  9 :       nCompteur++
 *   line 10 :     FIN
 *   line 11 :   FIN
 *   line 12 : (vide)
 *   line 13 :   SAI_Nom est un Champ Saisie
 *   line 14 :     PROPRIETES
 *   line 15 :       Valeur = ""
 *   line 16 :     FIN
 *   line 17 : (vide)
 *   line 18 :     PROCEDURE surAChaqueModification()
 *   line 19 :       Trace("modif")
 *   line 20 :     FIN
 *   line 21 :   FIN
 *   line 22 : (vide)
 *   line 23 :   PROCEDURE FinInitialisation()
 *   line 24 :     nCompteur = 0
 *   line 25 :   FIN
 *   line 26 : FIN
 *
 * NEEDS_INPUT : le noeud "declarations globales" (nCompteur, line 1) est
 * non teste ici (nom et presence a confirmer par coordinator).
 * Les assertions verifient BTN_Valider, SAI_Nom, FinInitialisation.
 */
static void test_doc_symbol_complet(void) {
    printf("\n[TEST] feat/0751 documentSymbol : hierarchie complete\n");
    const char* src =
        "FEN_Complet est une Fenetre\n"
        "\tnCompteur est un entier = 0\n"
        "\n"
        "\tBTN_Valider est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Valider\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tnCompteur++\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tSAI_Nom est un Champ Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tValeur = \"\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surAChaqueModification()\n"
        "\t\t\tTrace(\"modif\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tnCompteur = 0\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0751/complet : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "FEN_Complet", "0751/complet : racine FEN_Complet");

    {
        JsonValue* fen    = find_sym(result, "FEN_Complet");
        JsonValue* fen_ch = sym_children(fen);

        /* Trois enfants attendus (au moins) : BTN_Valider, SAI_Nom, FinInitialisation. */
        ASSERT_SYM_EXISTS(fen_ch, "BTN_Valider",
            "0751/complet : BTN_Valider enfant direct");
        ASSERT_SYM_EXISTS(fen_ch, "SAI_Nom",
            "0751/complet : SAI_Nom enfant direct");
        ASSERT_SYM_EXISTS(fen_ch, "FinInitialisation",
            "0751/complet : FinInitialisation enfant direct");

        /* BTN_Valider : range [3..11], enfant surClic. */
        {
            JsonValue* btn    = find_sym(fen_ch, "BTN_Valider");
            JsonValue* btn_ch = sym_children(btn);
            if (btn) {
                ASSERT_SYM_LINE(btn, 3, 11, "0751/complet : BTN_Valider range [3..11]");
                ASSERT_SYM_EXISTS(btn_ch, "surClic",
                    "0751/complet : surClic enfant BTN_Valider");
                JsonValue* sc = find_sym(btn_ch, "surClic");
                if (sc) {
                    ASSERT_SYM_LINE(sc, 8, 10, "0751/complet : surClic range [8..10]");
                }
            }
        }

        /* SAI_Nom : range [13..21], enfant surAChaqueModification. */
        {
            JsonValue* sai    = find_sym(fen_ch, "SAI_Nom");
            JsonValue* sai_ch = sym_children(sai);
            if (sai) {
                ASSERT_SYM_LINE(sai, 13, 21, "0751/complet : SAI_Nom range [13..21]");
                ASSERT_SYM_EXISTS(sai_ch, "surAChaqueModification",
                    "0751/complet : surAChaqueModification enfant SAI_Nom");
            }
        }

        /* FinInitialisation au niveau fenetre : range [23..25]. */
        {
            JsonValue* fi = find_sym(fen_ch, "FinInitialisation");
            if (fi) {
                ASSERT_SYM_LINE(fi, 23, 25,
                    "0751/complet : FinInitialisation range [23..25]");
            }
        }
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   -- commentaires blocs (slash-etoile ... etoile-slash) neutralises
 *  avant le decoupage en lignes du parseur de structure (outline/owl_explorer).
 *  Sans neutralisation, une fausse fin de bloc DANS un commentaire
 *  (ex "end" colle a la fermeture etoile-slash) est lue comme un vrai FIN
 *  et tronque tout le code aval.
 * ================================================================ */

/* Fixture : fenetre avec un commentaire bloc a l'indent 0 (= indent de la
 * fenetre) dont la fermeture est "end" colle a etoile-slash (le mot 'end'
 * est le 1er token de la ligne). AVANT fix : la ligne 8 est vue comme le FIN
 * de la fenetre -> EDT_Second et BTN_Go (apres le commentaire) tombent hors
 * fenetre -> absents de l'outline. APRES fix : les 3 controles presents. */
static void test_fix1281_block_comment_fake_end_tronque(void) {
    printf("\n[TEST] fix/1281 documentSymbol : commentaire bloc 'end*/' ne tronque plus\n");
    const char* src =
        "FEN_Decoder est une Fenetre\n"          /* 0 */
        "\tEDT_First est un Champ Saisie\n"      /* 1 */
        "\t\tPROPRIETES\n"                        /* 2 */
        "\t\t\tLibelle = \"A\"\n"                /* 3 */
        "\t\tFIN\n"                               /* 4 */
        "\tFIN\n"                                 /* 5 */
        "\t/*NOT SUPPORTED PROCEDURE Foo()\n"    /* 6 */
        "\t EDT_Result = \"x\"\n"                /* 7 */
        "end*/\n"                                 /* 8 : fausse fin de bloc */
        "\tEDT_Second est un Champ Saisie\n"     /* 9 */
        "\t\tPROPRIETES\n"                        /* 10 */
        "\t\t\tLibelle = \"B\"\n"                /* 11 */
        "\t\tFIN\n"                               /* 12 */
        "\tFIN\n"                                 /* 13 */
        "\tBTN_Go est un Champ Bouton\n"         /* 14 */
        "\t\tPROPRIETES\n"                        /* 15 */
        "\t\t\tLibelle = \"Go\"\n"               /* 16 */
        "\t\tFIN\n"                               /* 17 */
        "\tFIN\n"                                 /* 18 */
        "FIN\n";                                  /* 19 */
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1281/tronque : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "FEN_Decoder", "1281/tronque : racine FEN_Decoder");

    JsonValue* fen    = find_sym(result, "FEN_Decoder");
    JsonValue* fen_ch = sym_children(fen);

    /* Le controle AVANT le commentaire : deja present meme avant fix. */
    ASSERT_SYM_EXISTS(fen_ch, "EDT_First",  "1281/tronque : EDT_First present");
    /* Les controles APRES le commentaire : le coeur du bug (RED avant fix). */
    ASSERT_SYM_EXISTS(fen_ch, "EDT_Second", "1281/tronque : EDT_Second present (post-comment)");
    ASSERT_SYM_EXISTS(fen_ch, "BTN_Go",     "1281/tronque : BTN_Go present (post-comment)");

    /* Valeurs exactes : ranges des controles apres le commentaire. */
    JsonValue* symFirst = find_sym(fen_ch, "EDT_First");
    if (symFirst) ASSERT_SYM_LINE(symFirst, 1, 5,  "1281/tronque : EDT_First range [1..5]");
    JsonValue* symSecond = find_sym(fen_ch, "EDT_Second");
    if (symSecond) ASSERT_SYM_LINE(symSecond, 9, 13, "1281/tronque : EDT_Second range [9..13]");
    JsonValue* btn = find_sym(fen_ch, "BTN_Go");
    if (btn) ASSERT_SYM_LINE(btn, 14, 18,  "1281/tronque : BTN_Go range [14..18]");
    /* La fenetre couvre bien jusqu'a son vrai FIN (ligne 19). */
    if (fen) ASSERT_SYM_LINE(fen, 0, 19,   "1281/tronque : FEN_Decoder range [0..19]");

    json_free(result);
    json_free(params);
}

/* Fixture : commentaire bloc mono-ligne (slash-etoile end etoile-slash) en fin
 * d'une declaration. Le mot 'end' est DANS le commentaire mais n'est pas 1er
 * token de ligne -> pas de fausse fin. Non-regression : la declaration et le
 * controle suivant restent captures a l'identique. */
static void test_fix1281_block_comment_mono_ligne(void) {
    printf("\n[TEST] fix/1281 documentSymbol : commentaire bloc mono-ligne inoffensif\n");
    const char* src =
        "FEN_M est une Fenetre\n"                    /* 0 */
        "\tnCpt est un entier = 0 /* end */\n"       /* 1 : decl + comm inline */
        "\tBTN_X est un Champ Bouton\n"              /* 2 */
        "\t\tPROPRIETES\n"                            /* 3 */
        "\t\t\tLibelle = \"X\"\n"                    /* 4 */
        "\t\tFIN\n"                                   /* 5 */
        "\tFIN\n"                                     /* 6 */
        "FIN\n";                                      /* 7 */
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "FEN_M", "1281/mono : racine FEN_M");
    JsonValue* fen    = find_sym(result, "FEN_M");
    JsonValue* fen_ch = sym_children(fen);
    /* BTN_X (apres la decl a commentaire inline) present, range exacte. */
    ASSERT_SYM_EXISTS(fen_ch, "BTN_X", "1281/mono : BTN_X present");
    JsonValue* btn = find_sym(fen_ch, "BTN_X");
    if (btn) ASSERT_SYM_LINE(btn, 2, 6, "1281/mono : BTN_X range [2..6]");

    json_free(result);
    json_free(params);
}

/* Fixture (module/script EN) : commentaire bloc ferme en DEBUT de ligne,
 * suivi de vrai code sur la MEME ligne. Le blanchiment doit s'arreter a
 * '*​/' : 'nGlobal is int = 42' reste analyse. AVANT fix : la ligne commence
 * par '/' -> non-declarative -> nGlobal absent (RED). */
static void test_fix1281_code_apres_fermeture_meme_ligne(void) {
    printf("\n[TEST] fix/1281 documentSymbol : code reel apres '*/' meme ligne\n");
    const char* src =
        "/* header comment */ nGlobal is int = 42\n" /* 0 */
        "PROCEDURE Faire()\n"                         /* 1 */
        "\tTrace(\"x\")\n"                           /* 2 */
        "END\n";                                      /* 3 */
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* Fichier EN -> regroupement sous "Global declarations". */
    ASSERT_SYM_EXISTS(result, "Global declarations",
        "1281/apres : noeud 'Global declarations' present");
    JsonValue* dg = find_sym(result, "Global declarations");
    JsonValue* dg_ch = sym_children(dg);
    ASSERT_SYM_EXISTS(dg_ch, "nGlobal", "1281/apres : nGlobal capture (code apres */)");
    JsonValue* v = find_sym(dg_ch, "nGlobal");
    if (v) ASSERT_SYM_LINE(v, 0, 0, "1281/apres : nGlobal range [0..0]");
    /* Le callable reste present aussi. */
    ASSERT_SYM_EXISTS(result, "Faire", "1281/apres : Faire present");

    json_free(result);
    json_free(params);
}

/* Non-regression : fenetre SANS commentaire bloc -> outline inchange. */
static void test_fix1281_non_regression_sans_bloc(void) {
    printf("\n[TEST] fix/1281 documentSymbol : non-regression sans commentaire bloc\n");
    const char* src =
        "FEN_Plain est une Fenetre\n"            /* 0 */
        "\tEDT_A est un Champ Saisie\n"          /* 1 */
        "\t\tPROPRIETES\n"                        /* 2 */
        "\t\t\tLibelle = \"A\"\n"                /* 3 */
        "\t\tFIN\n"                               /* 4 */
        "\tFIN\n"                                 /* 5 */
        "\tEDT_B est un Champ Saisie\n"          /* 6 */
        "\t\tPROPRIETES\n"                        /* 7 */
        "\t\t\tLibelle = \"B\"\n"                /* 8 */
        "\t\tFIN\n"                               /* 9 */
        "\tFIN\n"                                 /* 10 */
        "FIN\n";                                  /* 11 */
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1281/noreg : 1 symbole racine");
    JsonValue* fen    = find_sym(result, "FEN_Plain");
    JsonValue* fen_ch = sym_children(fen);
    ASSERT_SYM_EXISTS(fen_ch, "EDT_A", "1281/noreg : EDT_A present");
    ASSERT_SYM_EXISTS(fen_ch, "EDT_B", "1281/noreg : EDT_B present");
    JsonValue* a = find_sym(fen_ch, "EDT_A");
    if (a) ASSERT_SYM_LINE(a, 1, 5,  "1281/noreg : EDT_A range [1..5]");
    JsonValue* b = find_sym(fen_ch, "EDT_B");
    if (b) ASSERT_SYM_LINE(b, 6, 10, "1281/noreg : EDT_B range [6..10]");

    json_free(result);
    json_free(params);
}

/* ----- 3. textDocument/foldingRange --------------------------------- */

/* Fixture "fenetre vide" : aucun champ, aucune procedure.
 * Attendu : 0 plages repliables (pas de bloc champ ni proc).
 */
static void test_folding_fenetre_vide(void) {
    printf("\n[TEST] feat/0751 foldingRange : Fenetre vide -> 0 plages\n");
    const char* src =
        "FEN_Vide est une Fenetre\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_folding_range(params);

    /* Attendu : array valide avec 0 elements (pas de champ ni proc). */
    ASSERT_FOLD_ARRAY_VALID(result, "0751/fold-vide : array valide");
    ASSERT_FOLD_COUNT(result, 0, "0751/fold-vide : 0 foldingRanges");

    json_free(result);
    json_free(params);
}

/* Fixture "1 champ sans evenement" :
 *   BTN_OK : Champ Bouton, lines 1-5.
 * Attendu : 1 plage {startLine:1, endLine:5}.
 */
static void test_folding_un_champ(void) {
    printf("\n[TEST] feat/0751 foldingRange : 1 champ sans evenement\n");
    const char* src =
        "FEN_UnChamp est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_folding_range(params);

    ASSERT_FOLD_ARRAY_VALID(result, "0751/fold-1champ : array valide");
    ASSERT_HAS_FOLD(result, 1, 5, "0751/fold-1champ : BTN_OK [1..5]");

    json_free(result);
    json_free(params);
}

/* Fixture "champ avec surClic" :
 *   BTN_OK : lines 1-9, surClic : lines 6-8.
 * Attendu : 2 plages.
 */
static void test_folding_champ_avec_event(void) {
    printf("\n[TEST] feat/0751 foldingRange : champ avec surClic\n");
    const char* src =
        "FEN_AvecEvent est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tTrace(\"clic\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_folding_range(params);

    ASSERT_FOLD_ARRAY_VALID(result, "0751/fold-event : array valide");
    ASSERT_HAS_FOLD(result, 1, 9, "0751/fold-event : BTN_OK [1..9]");
    ASSERT_HAS_FOLD(result, 6, 8, "0751/fold-event : surClic [6..8]");

    json_free(result);
    json_free(params);
}

/* Fixture "hierarchie complete" (meme fixture que test_doc_symbol_complet) :
 *   BTN_Valider       : lines 3-11
 *   surClic           : lines 8-10
 *   SAI_Nom           : lines 13-21
 *   surAChaqueModif   : lines 18-20
 *   FinInitialisation : lines 23-25
 * Attendu : 5 plages.
 */
static void test_folding_complet(void) {
    printf("\n[TEST] feat/0751 foldingRange : hierarchie complete\n");
    const char* src =
        "FEN_Complet est une Fenetre\n"
        "\tnCompteur est un entier = 0\n"
        "\n"
        "\tBTN_Valider est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Valider\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tnCompteur++\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tSAI_Nom est un Champ Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tValeur = \"\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surAChaqueModification()\n"
        "\t\t\tTrace(\"modif\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tnCompteur = 0\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_folding_range(params);

    /* Pas de FOLD_COUNT exact ici : PROPRIETES...FIN peut legitimement
     * generer des plages supplementaires (decision implementer). On verifie
     * les 5 plages obligatoires selon la spec ticket. */
    ASSERT_FOLD_ARRAY_VALID(result, "0751/fold-complet : array valide");
    ASSERT_HAS_FOLD(result,  3, 11, "0751/fold-complet : BTN_Valider [3..11]");
    ASSERT_HAS_FOLD(result,  8, 10, "0751/fold-complet : surClic [8..10]");
    ASSERT_HAS_FOLD(result, 13, 21, "0751/fold-complet : SAI_Nom [13..21]");
    ASSERT_HAS_FOLD(result, 18, 20, "0751/fold-complet : surAChaqueModification [18..20]");
    ASSERT_HAS_FOLD(result, 23, 25, "0751/fold-complet : FinInitialisation [23..25]");

    json_free(result);
    json_free(params);
}

/* ----- 4. SymbolKind (Q1) ------------------------------------------ */

/* Verifie que les kinds LSP emis correspondent a la spec :
 *   Fenetre    -> SymbolKind.Class   = 5
 *   Champ      -> SymbolKind.Field   = 8
 *   Procedure  -> SymbolKind.Method  = 6
 * Fixture : "hierarchie complete" (reutilise la meme source que complet). */
static void test_doc_symbol_kinds(void) {
    printf("\n[TEST] feat/0751 Q1 : SymbolKind Fenetre=5 Champ=8 Method=6\n");
    const char* src =
        "FEN_Complet est une Fenetre\n"
        "\tnCompteur est un entier = 0\n"
        "\n"
        "\tBTN_Valider est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Valider\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tnCompteur++\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tSAI_Nom est un Champ Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tValeur = \"\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surAChaqueModification()\n"
        "\t\t\tTrace(\"modif\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tnCompteur = 0\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* Fenetre (racine) -> kind = Class = 5. */
    {
        JsonValue* fen = find_sym(result, "FEN_Complet");
        ASSERT_SYM_KIND(fen, LSP_KIND_CLASS,
            "0751/Q1 : FEN_Complet kind=Class(5)");
    }

    /* Champ -> kind = Field = 8. */
    {
        JsonValue* fen    = find_sym(result, "FEN_Complet");
        JsonValue* fen_ch = sym_children(fen);
        JsonValue* btn    = find_sym(fen_ch, "BTN_Valider");
        ASSERT_SYM_KIND(btn, LSP_KIND_FIELD,
            "0751/Q1 : BTN_Valider kind=Field(8)");
    }

    /* Procedure d'evenement -> kind = Method = 6. */
    {
        JsonValue* fen    = find_sym(result, "FEN_Complet");
        JsonValue* fen_ch = sym_children(fen);
        JsonValue* btn    = find_sym(fen_ch, "BTN_Valider");
        JsonValue* btn_ch = sym_children(btn);
        JsonValue* sc     = find_sym(btn_ch, "surClic");
        ASSERT_SYM_KIND(sc, LSP_KIND_METHOD,
            "0751/Q1 : surClic kind=Method(6)");
    }

    json_free(result);
    json_free(params);
}

/* ----- 5. Declarations globales (Q2) -------------------------------- */

/* Verifie que la fixture "complet" (qui contient `nCompteur est un entier`)
 * emet un noeud enfant "Declarations globales" (kind=Namespace=3) avec
 * "nCompteur" (kind=Variable=13) comme enfant. */
static void test_doc_symbol_declarations_globales(void) {
    /* "D\xc3\xa9" "clarations globales" = "Declarations globales" en UTF-8. */
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";

    printf("\n[TEST] feat/0751 Q2 : noeud Decl.globales present et correct\n");
    const char* src =
        "FEN_Complet est une Fenetre\n"
        "\tnCompteur est un entier = 0\n"
        "\n"
        "\tBTN_Valider est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Valider\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\tnCompteur++\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tSAI_Nom est un Champ Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tValeur = \"\"\n"
        "\t\tFIN\n"
        "\n"
        "\t\tPROCEDURE surAChaqueModification()\n"
        "\t\t\tTrace(\"modif\")\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tnCompteur = 0\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_Complet");
    JsonValue* fen_ch = sym_children(fen);

    /* Le noeud groupeur doit etre present comme enfant direct. */
    ASSERT_SYM_EXISTS(fen_ch, DECL_GLOB,
        "0751/Q2 : noeud Decl.globales present enfant FEN_Complet");

    {
        JsonValue* dg = find_sym(fen_ch, DECL_GLOB);

        /* Kind du noeud groupeur : Namespace = 3. */
        ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
            "0751/Q2 : Decl.globales kind=Namespace(3)");

        /* La variable nCompteur est enfant du noeud groupeur. */
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_EXISTS(dg_ch, "nCompteur",
            "0751/Q2 : nCompteur enfant de Decl.globales");

        /* Kind de la variable : Variable = 13. */
        JsonValue* nco = find_sym(dg_ch, "nCompteur");
        ASSERT_SYM_KIND(nco, LSP_KIND_VARIABLE,
            "0751/Q2 : nCompteur kind=Variable(13)");
    }

    json_free(result);
    json_free(params);
}

/* Verifie que la fixture "fenetre vide" (0 declarations corps) n'emet PAS
 * le noeud "Declarations globales". */
static void test_doc_symbol_no_decl_globales_vide(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";

    printf("\n[TEST] feat/0751 Q2 : fenetre vide -> pas de noeud Decl.globales\n");
    const char* src =
        "FEN_Vide est une Fenetre\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_Vide");
    JsonValue* fen_ch = sym_children(fen);

    ASSERT_SYM_NOT_EXISTS(fen_ch, DECL_GLOB,
        "0751/Q2/vide : pas de Decl.globales sur fenetre vide");

    json_free(result);
    json_free(params);
}

/* Verifie que la fixture "1 champ sans event" (0 declarations corps)
 * n'emet PAS le noeud "Declarations globales". */
static void test_doc_symbol_no_decl_globales_1champ(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";

    printf("\n[TEST] feat/0751 Q2 : 1 champ sans event -> pas de noeud Decl.globales\n");
    const char* src =
        "FEN_UnChamp est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_UnChamp");
    JsonValue* fen_ch = sym_children(fen);

    ASSERT_SYM_NOT_EXISTS(fen_ch, DECL_GLOB,
        "0751/Q2/1champ : pas de Decl.globales sur fenetre sans decls");

    json_free(result);
    json_free(params);
}

/*  : le noeud groupeur de la fenetre est localise selon la langue du
 * fichier. Une fenetre EN (mots-cles `is a` / `END`) emet "Global
 * declarations" (et PAS la forme FR). Structure inchangee (Namespace 3,
 * variable enfant). */
static void test_doc_symbol_declarations_globales_window_en(void) {
    printf("\n[TEST] fix/1230 : fenetre EN -> noeud 'Global declarations' localise\n");
    const char* src =
        "FEN_En is a Window\n"
        "\tnCounter is a integer = 0\n"
        "\tBTN_Ok is a Control Button\n"
        "\t\tPROPERTIES\n"
        "\t\t\tCaption = \"Ok\"\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_En");
    JsonValue* fen_ch = sym_children(fen);

    /* Libelle EN present, forme FR absente. */
    ASSERT_SYM_EXISTS(fen_ch, "Global declarations",
        "1230/win-en : noeud Global declarations (localise EN)");
    ASSERT_SYM_NOT_EXISTS(fen_ch, "D\xc3\xa9" "clarations globales",
        "1230/win-en : pas de forme FR dans une fenetre EN");
    {
        JsonValue* dg = find_sym(fen_ch, "Global declarations");
        ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
            "1230/win-en : Global declarations kind=Namespace(3)");
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_EXISTS(dg_ch, "nCounter",
            "1230/win-en : nCounter enfant de Global declarations");
        ASSERT_SYM_KIND(find_sym(dg_ch, "nCounter"), LSP_KIND_VARIABLE,
            "1230/win-en : nCounter kind=Variable(13)");
    }

    json_free(result);
    json_free(params);
}

/*  -- support #211 : une declaration ecrite SANS l'article de confort
 * ("Test is string" au lieu de "Test is a string") doit apparaitre dans
 * l'outline au meme titre qu'avec l'article. Le compilateur et l'extension
 * l'acceptent deja ; le LSP etait l'outlier. */
static void test_doc_symbol_declaration_sans_article(void) {
    printf("\n[TEST] fix/1287 : declaration sans article de confort -> presente dans l'outline\n");
    const char* src =
        "FEN_En is a Window\n"
        "\tTest is string = \"tt\"\n"      /* forme NUE (sans 'a') -- le bug */
        "\tiValue is a integer = 23\n"     /* forme avec article -- temoin */
        "END\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_En");
    JsonValue* fen_ch = sym_children(fen);
    JsonValue* dg     = find_sym(fen_ch, "Global declarations");
    JsonValue* dg_ch  = sym_children(dg);

    /* Les DEUX declarations apparaissent, article present ou non. */
    ASSERT_SYM_EXISTS(dg_ch, "Test",
        "1287 : 'Test is string' (sans article) present dans l'outline");
    ASSERT_SYM_KIND(find_sym(dg_ch, "Test"), LSP_KIND_VARIABLE,
        "1287 : Test kind=Variable(13)");
    ASSERT_SYM_DETAIL(find_sym(dg_ch, "Test"), "string",
        "1287 : Test detail=string (type extrait correctement en forme nue)");
    ASSERT_SYM_EXISTS(dg_ch, "iValue",
        "1287 : 'iValue is a integer' (avec article) toujours present");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   -- documentSymbol + foldingRange Etat et module/script
 *
 *  14 tests : 7 Etat (doc_symbol x5 + folding x2) + 7 module/script.
 *  Tous ROUGES avant impl : handle_document_symbol retourne NULL
 *  (ou un tableau Fenetre-only) pour les fichiers Etat et module.
 *
 *  Choix Function(12) vs Method(6) pour callables top-level module :
 *    PROCEDURE/FONCTION standalone (pas dans une Classe/Fenetre/Etat)
 *    -> SymbolKind.Function = 12.
 *    Justification : LSP 3.17 reserve Method(6) aux membres lies a un
 *    objet. Les callables top-level OWL (scenario_*, helpers module)
 *    sont des fonctions libres -> Function(12) semantiquement correct.
 *    Les methodes de Classe restent Method(6) (elles sont membres d'objet).
 * ================================================================ */

/* ----- (A) Etat -- documentSymbol ---------------------------------- */

/* Fixture "etat vide" :
 *   line 0 : ETAT_Simple est un Etat
 *   line 1 : FIN
 * Attendu : 1 symbole racine ETAT_Simple, 0 enfants, kind=Class.
 */
static void test_doc_symbol_etat_vide(void) {
    printf("\n[TEST] feat/0755 documentSymbol : Etat vide -> racine seule\n");
    const char* src =
        "ETAT_Simple est un Etat\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/etat-vide : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "ETAT_Simple", "0755/etat-vide : racine = ETAT_Simple");

    {
        JsonValue* etat = find_sym(result, "ETAT_Simple");
        if (etat) {
            ASSERT_SYM_CHILDREN_COUNT(etat, 0, "0755/etat-vide : 0 enfants");
            ASSERT_SYM_LINE(etat, 0, 1, "0755/etat-vide : ETAT_Simple range [0..1]");
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "etat avec champ et procedure" :
 *   line  0 : ETAT_Champs est un Etat
 *   line  1 :   LIB_Titre est un Champ Libelle
 *   line  2 :     PROPRIETES
 *   line  3 :       Libelle = "Titre"
 *   line  4 :     FIN
 *   line  5 :   FIN
 *   line  6 :
 *   line  7 :   PROCEDURE FinInitialisation()
 *   line  8 :     Trace("init")
 *   line  9 :   FIN
 *   line 10 : FIN
 * Attendu : racine ETAT_Champs, enfants LIB_Titre (Field) + FinInit (Method).
 */
static void test_doc_symbol_etat_complet(void) {
    printf("\n[TEST] feat/0755 documentSymbol : Etat avec champ + proc\n");
    const char* src =
        "ETAT_Champs est un Etat\n"
        "\tLIB_Titre est un Champ Libelle\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Titre\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tTrace(\"init\")\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/etat-complet : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "ETAT_Champs", "0755/etat-complet : racine = ETAT_Champs");

    {
        JsonValue* etat    = find_sym(result, "ETAT_Champs");
        JsonValue* etat_ch = sym_children(etat);

        ASSERT_SYM_EXISTS(etat_ch, "LIB_Titre",
            "0755/etat-complet : LIB_Titre enfant direct");
        ASSERT_SYM_EXISTS(etat_ch, "FinInitialisation",
            "0755/etat-complet : FinInitialisation enfant direct");

        {
            JsonValue* lib = find_sym(etat_ch, "LIB_Titre");
            if (lib) {
                ASSERT_SYM_LINE(lib, 1, 5, "0755/etat-complet : LIB_Titre range [1..5]");
            }
        }
        {
            JsonValue* fi = find_sym(etat_ch, "FinInitialisation");
            if (fi) {
                ASSERT_SYM_LINE(fi, 7, 9,
                    "0755/etat-complet : FinInitialisation range [7..9]");
            }
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "etat accentue" : verrouille l'insensibilite accent sur le
 * mot-cle "Etat" (ici "\xc3\x89tat" = "Etat" avec E majuscule U+00C9).
 *   line 0 : ETAT_Acc est un Etat  (E accentue)
 *   line 1 : FIN
 * Attendu : ETAT_Acc reconnu comme racine malgre l'accent.
 */
static void test_doc_symbol_etat_accent(void) {
    printf("\n[TEST] feat/0755 documentSymbol : Etat accent (est un "
           "\xc3\x89" "tat)\n");
    const char* src =
        "ETAT_Acc est un \xc3\x89" "tat\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/etat-accent : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "ETAT_Acc",
        "0755/etat-accent : ETAT_Acc reconnu (accent insensible sur Etat)");

    json_free(result);
    json_free(params);
}

/* Fixture "etat EN" : mot-cle anglais "is a Report".
 *   line 0 : ETAT_En is a Report
 *   line 1 : FIN
 * Attendu : ETAT_En reconnu comme racine.
 */
static void test_doc_symbol_etat_en(void) {
    printf("\n[TEST] feat/0755 documentSymbol : Etat EN (is a Report)\n");
    const char* src =
        "ETAT_En is a Report\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/etat-en : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "ETAT_En",
        "0755/etat-en : ETAT_En reconnu (is a Report = Etat EN)");

    json_free(result);
    json_free(params);
}

/* Verifie les kinds emis pour l'Etat :
 *   racine Etat  -> Class = 5 (identique Fenetre, spec ticket).
 *   champ Etat   -> Field = 8.
 */
static void test_doc_symbol_etat_kind(void) {
    printf("\n[TEST] feat/0755 Q1 : Etat kind=Class(5), champ kind=Field(8)\n");
    const char* src =
        "ETAT_Kind est un Etat\n"
        "\tLIB_X est un Champ Libelle\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"X\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    {
        JsonValue* etat = find_sym(result, "ETAT_Kind");
        ASSERT_SYM_KIND(etat, LSP_KIND_CLASS,
            "0755/Q1 : ETAT_Kind kind=Class(5)");
    }

    {
        JsonValue* etat    = find_sym(result, "ETAT_Kind");
        JsonValue* etat_ch = sym_children(etat);
        JsonValue* lib     = find_sym(etat_ch, "LIB_X");
        ASSERT_SYM_KIND(lib, LSP_KIND_FIELD,
            "0755/Q1 : LIB_X dans Etat kind=Field(8)");
    }

    json_free(result);
    json_free(params);
}

/* ----- (A) Etat -- foldingRange ------------------------------------- */

/* Fixture "etat vide" : 0 plages. */
static void test_folding_etat_vide(void) {
    printf("\n[TEST] feat/0755 foldingRange : Etat vide -> 0 plages\n");
    const char* src =
        "ETAT_Simple est un Etat\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_folding_range(params);

    ASSERT_FOLD_ARRAY_VALID(result, "0755/fold-etat-vide : array valide");
    ASSERT_FOLD_COUNT(result, 0, "0755/fold-etat-vide : 0 foldingRanges");

    json_free(result);
    json_free(params);
}

/* Fixture "etat avec champ + proc" (meme source que etat_complet) :
 *   LIB_Titre         : lines 1-5
 *   FinInitialisation : lines 7-9
 * Attendu : au moins 2 plages.
 */
static void test_folding_etat_complet(void) {
    printf("\n[TEST] feat/0755 foldingRange : Etat champ + proc\n");
    const char* src =
        "ETAT_Champs est un Etat\n"
        "\tLIB_Titre est un Champ Libelle\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Titre\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tTrace(\"init\")\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_folding_range(params);

    ASSERT_FOLD_ARRAY_VALID(result, "0755/fold-etat-complet : array valide");
    ASSERT_HAS_FOLD(result, 1, 5, "0755/fold-etat-complet : LIB_Titre [1..5]");
    ASSERT_HAS_FOLD(result, 7, 9,
        "0755/fold-etat-complet : FinInitialisation [7..9]");

    json_free(result);
    json_free(params);
}

/* ----- (B) Module/script -- documentSymbol ------------------------- */

/* Fixture "3 PROCEDURE PUBLIQUE scenario_*" (type test_*.owl) :
 *   line 0 : PROCEDURE PUBLIQUE scenario_foo()
 *   line 1 :   Trace("foo")
 *   line 2 : FIN
 *   line 3 :
 *   line 4 : PROCEDURE PUBLIQUE scenario_bar()
 *   line 5 :   Trace("bar")
 *   line 6 : FIN
 *   line 7 :
 *   line 8 : PROCEDURE PUBLIQUE scenario_baz()
 *   line 9 :   Trace("baz")
 *   line 10: FIN
 * Attendu : 3 symboles racine. Cas explicite du dev : les PROCEDURE PUBLIQUE
 * scenario_* de test_*.owl doivent apparaitre dans l'outline.
 */
static void test_doc_symbol_module_3procs(void) {
    printf("\n[TEST] feat/0755 documentSymbol : module 3 PROCEDURE PUBLIQUE\n");
    const char* src =
        "PROCEDURE PUBLIQUE scenario_foo()\n"
        "\tTrace(\"foo\")\n"
        "FIN\n"
        "\n"
        "PROCEDURE PUBLIQUE scenario_bar()\n"
        "\tTrace(\"bar\")\n"
        "FIN\n"
        "\n"
        "PROCEDURE PUBLIQUE scenario_baz()\n"
        "\tTrace(\"baz\")\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 3, "0755/module-3procs : 3 symboles racine");
    ASSERT_SYM_EXISTS(result, "scenario_foo",
        "0755/module-3procs : scenario_foo present");
    ASSERT_SYM_EXISTS(result, "scenario_bar",
        "0755/module-3procs : scenario_bar present");
    ASSERT_SYM_EXISTS(result, "scenario_baz",
        "0755/module-3procs : scenario_baz present");

    json_free(result);
    json_free(params);
}

/* Fixture "PROCEDURE privee" (sans PUBLIQUE) :
 *   line 0 : PROCEDURE MaProc()
 *   line 1 :   Trace("priv")
 *   line 2 : FIN
 * Attendu : 1 symbole racine MaProc.
 */
static void test_doc_symbol_module_proc_prive(void) {
    printf("\n[TEST] feat/0755 documentSymbol : module PROCEDURE privee\n");
    const char* src =
        "PROCEDURE MaProc()\n"
        "\tTrace(\"priv\")\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/module-proc-priv : 1 symbole");
    ASSERT_SYM_EXISTS(result, "MaProc",
        "0755/module-proc-priv : MaProc present");

    json_free(result);
    json_free(params);
}

/* Fixture "FONCTION top-level" :
 *   line 0 : FONCTION MaFonction() : entier
 *   line 1 :   RENVOYER 42
 *   line 2 : FIN
 * Attendu : 1 symbole racine MaFonction.
 */
static void test_doc_symbol_module_fonction(void) {
    printf("\n[TEST] feat/0755 documentSymbol : module FONCTION top-level\n");
    const char* src =
        "FONCTION MaFonction() : entier\n"
        "\tRENVOYER 42\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/module-fonction : 1 symbole");
    ASSERT_SYM_EXISTS(result, "MaFonction",
        "0755/module-fonction : MaFonction present");

    json_free(result);
    json_free(params);
}

/* Fixture "classe avec methodes et champ" :
 *   line  0 : cCompteur est une Classe
 *   line  1 :   nValeur est un entier
 *   line  2 :
 *   line  3 :   PROCEDURE CONSTRUCTEUR(n est un entier)
 *   line  4 :     nValeur = n
 *   line  5 :   FIN
 *   line  6 :
 *   line  7 :   PROCEDURE Incrementer()
 *   line  8 :     nValeur++
 *   line  9 :   FIN
 *   line 10 : FIN
 * Attendu : racine cCompteur (Class=5), enfants :
 *   nValeur      -> Field  = 8
 *   CONSTRUCTEUR -> Method = 6
 *   Incrementer  -> Method = 6
 */
static void test_doc_symbol_module_classe(void) {
    printf("\n[TEST] feat/0755 documentSymbol : module classe avec methodes\n");
    const char* src =
        "cCompteur est une Classe\n"
        "\tnValeur est un entier\n"
        "\n"
        "\tPROCEDURE CONSTRUCTEUR(n est un entier)\n"
        "\t\tnValeur = n\n"
        "\tFIN\n"
        "\n"
        "\tPROCEDURE Incrementer()\n"
        "\t\tnValeur++\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/module-classe : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "cCompteur",
        "0755/module-classe : cCompteur present");

    {
        JsonValue* cls    = find_sym(result, "cCompteur");
        JsonValue* cls_ch = sym_children(cls);

        ASSERT_SYM_EXISTS(cls_ch, "nValeur",
            "0755/module-classe : nValeur enfant (Field)");
        ASSERT_SYM_EXISTS(cls_ch, "CONSTRUCTEUR",
            "0755/module-classe : CONSTRUCTEUR enfant (Method)");
        ASSERT_SYM_EXISTS(cls_ch, "Incrementer",
            "0755/module-classe : Incrementer enfant (Method)");
    }

    json_free(result);
    json_free(params);
}

/* Fixture "classe EN" : mot-cle "is a class" (syntaxe EN).
 *   line 0 : cWidget is a class
 *   line 1 :   nId est un entier
 *   line 2 :   PROCEDURE GetId()
 *   line 3 :     RENVOYER nId
 *   line 4 :   FIN
 *   line 5 : FIN
 * Attendu : cWidget reconnu comme racine Class=5.
 */
static void test_doc_symbol_module_classe_en(void) {
    printf("\n[TEST] feat/0755 documentSymbol : module classe EN (is a class)\n");
    const char* src =
        "cWidget is a class\n"
        "\tnId est un entier\n"
        "\tPROCEDURE GetId()\n"
        "\t\tRENVOYER nId\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "0755/module-classe-en : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "cWidget",
        "0755/module-classe-en : cWidget reconnu (is a class)");

    json_free(result);
    json_free(params);
}

/* Fixture "module vide" : aucune declaration top-level.
 * Attendu : tableau vide = 0 symboles.
 */
static void test_doc_symbol_module_vide(void) {
    printf("\n[TEST] feat/0755 documentSymbol : module vide -> 0 symboles\n");
    const char* src =
        "// module sans declaration\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 0,
        "0755/module-vide : 0 symboles (module vide)");

    json_free(result);
    json_free(params);
}

/* Verifie les kinds emis pour les nouveaux cas module/script :
 *   PROCEDURE top-level  -> SymbolKind.Function = 12  (callable libre)
 *   Classe top-level     -> SymbolKind.Class    = 5
 *   Methode de classe    -> SymbolKind.Method   = 6   (membre lie a objet)
 *   Champ de classe      -> SymbolKind.Field    = 8
 *
 * Fixture : 1 PROCEDURE top-level + 1 Classe avec 1 methode + 1 champ.
 */
static void test_doc_symbol_module_kinds(void) {
    printf("\n[TEST] feat/0755 Q1 : kinds module Proc=Fn=12 Class=5 Meth=6 Field=8\n");
    const char* src =
        "PROCEDURE TopProc()\n"
        "\tTrace(\"top\")\n"
        "FIN\n"
        "\n"
        "cMa est une Classe\n"
        "\tnX est un entier\n"
        "\tPROCEDURE MonAction()\n"
        "\t\tnX++\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* PROCEDURE top-level -> Function = 12 (callable libre, non lie a un objet). */
    {
        JsonValue* proc = find_sym(result, "TopProc");
        ASSERT_SYM_KIND(proc, LSP_KIND_FUNCTION,
            "0755/Q1 : TopProc kind=Function(12)");
    }

    /* Classe top-level -> Class = 5. */
    {
        JsonValue* cls = find_sym(result, "cMa");
        ASSERT_SYM_KIND(cls, LSP_KIND_CLASS,
            "0755/Q1 : cMa kind=Class(5)");
    }

    /* Methode de classe -> Method = 6 (membre lie a l'objet). */
    {
        JsonValue* cls    = find_sym(result, "cMa");
        JsonValue* cls_ch = sym_children(cls);
        JsonValue* meth   = find_sym(cls_ch, "MonAction");
        ASSERT_SYM_KIND(meth, LSP_KIND_METHOD,
            "0755/Q1 : MonAction kind=Method(6)");
    }

    /* Champ de classe -> Field = 8. */
    {
        JsonValue* cls    = find_sym(result, "cMa");
        JsonValue* cls_ch = sym_children(cls);
        JsonValue* field  = find_sym(cls_ch, "nX");
        ASSERT_SYM_KIND(field, LSP_KIND_FIELD,
            "0755/Q1 : nX kind=Field(8)");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   -- A. Fix plage FIN du documentSymbol (3 tests RED)
 *
 *  Bug actuel : make_symbol dans lsp_document_structure.c passe eChar=0
 *  en dur dans make_range(sLine, 0, eLine, 0). La specification requiert
 *  que range.end.character = longueur de la ligne FIN (FIN inclus).
 *
 *  Valeurs attendues :
 *    - FIN sans indent (fenetre top-level) : "FIN\n" -> eChar = 3
 *    - \tFIN (widget 1 tab)                : "\tFIN\n" -> eChar = 4
 *    - \t\tFIN (procedure 2 tabs)          : "\t\tFIN\n" -> eChar = 5
 *
 *  Les assertions ASSERT_SYM_LINE existantes restent vertes (non touchees).
 *  Seules les assertions ASSERT_SYM_END_CHAR sont RED avant impl.
 * ================================================================ */

/* Fixture "fenetre vide" :
 *   line 0 : FEN_Vide est une Fenetre
 *   line 1 : FIN  <-- "FIN\n" = 3 chars
 * Rouge : range.end.character vaut 0, attendu 3.
 */
static void test_doc_symbol_fin_char_fenetre(void) {
    printf("\n[TEST] feat/0766 fix-FIN : FEN_Vide range.end.character = 3\n");
    const char* src =
        "FEN_Vide est une Fenetre\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen = find_sym(result, "FEN_Vide");

    /* Rouge : eChar = 0 actuellement, attendu 3. */
    ASSERT_SYM_END_CHAR(fen, 3,
        "0766/fin-char/fenetre : \"FIN\" = 3 chars");
    /* Regression guard : ligne reste [0..1]. */
    ASSERT_SYM_LINE(fen, 0, 1,
        "0766/fin-char/fenetre : range ligne [0..1] inchangee");

    json_free(result);
    json_free(params);
}

/* Fixture "1 champ sans evenement" (reutilise fixture ) :
 *   line 5 : \tFIN  (FIN du BTN_OK, 1 tab) -> eChar attendu = 4
 *   line 6 : FIN    (FIN de la fenetre)    -> eChar attendu = 3
 * Rouge : eChar = 0 pour les deux.
 */
static void test_doc_symbol_fin_char_widget(void) {
    printf("\n[TEST] feat/0766 fix-FIN : BTN_OK range.end.character = 4 (tab+FIN)\n");
    const char* src =
        "FEN_UnChamp est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_UnChamp");
    JsonValue* fen_ch = sym_children(fen);
    JsonValue* btn    = find_sym(fen_ch, "BTN_OK");

    /* BTN_OK FIN line 5 : "\tFIN" = 4 chars. */
    ASSERT_SYM_END_CHAR(btn, 4,
        "0766/fin-char/widget : \"\\tFIN\" = 4 chars");
    /* FEN_UnChamp FIN line 6 : "FIN" = 3 chars. */
    ASSERT_SYM_END_CHAR(fen, 3,
        "0766/fin-char/fenetre-chapeau : \"FIN\" = 3 chars");
    /* Regression guard. */
    ASSERT_SYM_LINE(btn, 1, 5,
        "0766/fin-char/widget : ligne BTN_OK [1..5] inchangee");

    json_free(result);
    json_free(params);
}

/* Fixture "champ avec surClic" (reutilise fixture ) :
 *   line 8 : \t\tFIN  (FIN de surClic, 2 tabs) -> eChar attendu = 5
 * Rouge : eChar = 0.
 */
static void test_doc_symbol_fin_char_procedure(void) {
    printf("\n[TEST] feat/0766 fix-FIN : surClic range.end.character = 5 (2tabs+FIN)\n");
    const char* src =
        "FEN_AvecEvent est une Fenetre\n"   /* line 0 */
        "\tBTN_OK est un Champ Bouton\n"    /* line 1 */
        "\t\tPROPRIETES\n"                  /* line 2 */
        "\t\t\tLibelle = \"OK\"\n"          /* line 3 */
        "\t\tFIN\n"                         /* line 4 */
        "\n"                                /* line 5 */
        "\t\tPROCEDURE surClic()\n"         /* line 6 */
        "\t\t\tTrace(\"clic\")\n"           /* line 7 */
        "\t\tFIN\n"                         /* line 8 : \t\tFIN = 5 chars */
        "\tFIN\n"                           /* line 9 */
        "FIN\n";                            /* line 10 */
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_AvecEvent");
    JsonValue* fen_ch = sym_children(fen);
    JsonValue* btn    = find_sym(fen_ch, "BTN_OK");
    JsonValue* btn_ch = sym_children(btn);
    JsonValue* sc     = find_sym(btn_ch, "surClic");

    /* surClic FIN line 8 : "\t\tFIN" = 5 chars. */
    ASSERT_SYM_END_CHAR(sc, 5,
        "0766/fin-char/proc : \"\\t\\tFIN\" = 5 chars");
    /* Regression guard : surClic reste [6..8]. */
    ASSERT_SYM_LINE(sc, 6, 8,
        "0766/fin-char/proc : ligne surClic [6..8] inchangee");

    json_free(result);
    json_free(params);
}


/* ================================================================
 *   v2 -- Tests approche legere
 *
 *  Trois tests conformes a la spec v2 (recadrage coordinator).
 *
 *  A. test_diag_no_spawn_on_nonfile_scheme -- RED attendu
 *     publish_compiler_diagnostics sur un URI owl-bloc:// ne doit pas
 *     spawner owl --check. Court-circuit via guard scheme != file://.
 *     Aujourd'hui le guard n'existe pas -> RED (via stub sentinel -1).
 *
 *  B. test_completion_scheme_robustness -- VERT attendu (lock)
 *     VERROUILLAGE COMPORTEMENTAL : handle_completion sur owl-bloc://
 *     ne crashe pas et retourne un resultat non-NULL. Ce comportement
 *     resulte de la structure du handler (find_doc + scan content).
 *     Vert avant ET apres impl. But : detecter une regression future.
 *
 *  C. test_cross_file_context_lock -- VERT attendu (lock)
 *     VERROUILLAGE COMPORTEMENTAL : la boucle scan (lsp_handlers.c:791)
 *     scanne TOUS les docs ouverts sans filtre de scheme. Avec le parent
 *     file:// ouvert, ses symboles (SAI_Voisin, FEN_X) sont visibles
 *     depuis une completion dans le doc owl-bloc://.
 *     C'est la raison pour laquelle le protocole slice n'est pas necessaire
 *     pour le contexte voisins (finding commit 595e3907).
 *     Vert avant ET apres impl. But : detecter si un refactor futur
 *     casse ce cross-file par inadvertance.
 * ================================================================ */

/* A. Diagnostics surs : publish_compiler_diagnostics sur owl-bloc:// ne doit
 * pas spawner owl --check. Utilise le spy API lsp_diag_scheme_was_rejected()
 * (stub retourne -1 = guard non implemente -> RED attendu). */
static void test_diag_no_spawn_on_nonfile_scheme(void) {
    printf("\n[TEST] feat/0766 v2 diag-safe : pas de spawn owl --check sur owl-bloc:// (RED attendu)\n");
    const char* owl_bloc_uri = "owl-bloc:///c:/test/FEN_X.owl/BTN_OK";

    lsp_diag_scheme_reset();
    setup_doc(owl_bloc_uri, "PROPRIETES\nLibelle = \"OK\"\nFIN\n");
    publish_compiler_diagnostics(owl_bloc_uri);

    int fired = lsp_diag_scheme_was_rejected();
    if (fired == 1) {
        g_pass++;
        printf("  [OK]   0766/diag-safe : guard scheme != file:// -> early-return (pas de spawn)\n");
    } else {
        g_fail++;
        printf("  [FAIL] 0766/diag-safe : fired=%d (attendu 1 -- "
               "-1=guard non impl, 0=pas de fire sur owl-bloc://)\n", fired);
    }
}

/* B. Robustesse scheme : handle_completion sur owl-bloc:// ne crashe pas.
 * VERROUILLAGE COMPORTEMENTAL -- vert avant ET apres impl.
 * Ne pas confondre avec un vert inattendu TDD : cette feature EXISTE deja
 * (le handler ne crashe pas sur scheme custom), le test verrouille ce
 * comportement existant contre une regression future. */
static void test_completion_scheme_robustness(void) {
    printf("\n[TEST] feat/0766 v2 rob-scheme : handle_completion owl-bloc:// non-crash (VERT -- lock)\n");
    const char* owl_bloc_uri = "owl-bloc:///c:/test/FEN_X.owl/BTN_OK";
    const char* src =
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n";
    setup_doc(owl_bloc_uri, src);
    JsonValue* params = make_completion_params(owl_bloc_uri, 5, 3);
    JsonValue* result = handle_completion(params);

    if (result != NULL) {
        g_pass++;
        printf("  [OK]   0766/rob-scheme : handle_completion owl-bloc:// retourne non-NULL\n");
    } else {
        g_fail++;
        printf("  [FAIL] 0766/rob-scheme : handle_completion owl-bloc:// retourne NULL\n");
    }

    json_free(result);
    json_free(params);
}

/* C. Lock contexte cross-file : SAI_Voisin et FEN_X visibles depuis owl-bloc://.
 * VERROUILLAGE COMPORTEMENTAL -- vert avant ET apres impl.
 * La boucle scan (lsp_handlers.c:791) balaie TOUS les docs ouverts sans
 * filtre de scheme. Avec le parent file:// ouvert, ses controles et fenetres
 * sont visibles dans la completion du doc owl-bloc://.
 * C'est precisement la raison pour laquelle le protocole slice n'est pas
 * necessaire pour obtenir le contexte voisins (finding commit 595e3907). */
static void test_cross_file_context_lock(void) {
    printf("\n[TEST] feat/0766 v2 lock cross-file : SAI_Voisin/FEN_X visibles depuis owl-bloc:// (VERT -- lock)\n");

    const char* parent_uri = "file:///c:/test/FEN_X.owl";
    const char* bloc_uri   = "owl-bloc:///c:/test/FEN_X.owl/BTN_OK";

    for (int i = 0; i < lspServer.documentCount; i++) free(lspServer.documents[i].content);
    lspServer.documentCount = 2;

    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "%s", parent_uri);
    const char* parent_src =
        "FEN_X est une Fenetre\n"
        "\tSAI_Voisin est une Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\tFIN\n"
        "FIN\n";
    lspServer.documents[0].content    = strdup(parent_src);
    lspServer.documents[0].contentLen = (int)strlen(parent_src);
    lspServer.documents[0].version    = 1;

    snprintf(lspServer.documents[1].uri, sizeof(lspServer.documents[1].uri), "%s", bloc_uri);
    const char* bloc_src =
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\t\tPROCEDURE surClic()\n"
        "\t\t\t\n"
        "\t\tFIN\n"
        "\tFIN\n";
    lspServer.documents[1].content    = strdup(bloc_src);
    lspServer.documents[1].contentLen = (int)strlen(bloc_src);
    lspServer.documents[1].version    = 1;

    JsonValue* params = make_completion_params(bloc_uri, 5, 3);
    JsonValue* items  = handle_completion(params);

    ASSERT_HAS(items, "SAI_Voisin",
        "0766/lock-cross-file : SAI_Voisin visible depuis owl-bloc:// (cross-file scan)");
    ASSERT_HAS(items, "FEN_X",
        "0766/lock-cross-file : FEN_X visible depuis owl-bloc:// (cross-file scan)");

    json_free(items);
    json_free(params);
}

/* ================================================================
 *   -- Sections d'etat comme conteneurs documentSymbol
 *
 *  Bug actuel : un `<nom> est un <Type>` qui OUVRE un bloc FIN a
 *  l'interieur d'un Etat est traite comme une declaration mono-ligne
 *  (L_DECL -> variable sous "Declarations globales", kind=Variable=13).
 *  Les enfants de la section sont aplatis au niveau etat.
 *
 *  Comportement ATTENDU :
 *    - `<nom> est un <Type>` qui ouvre un bloc FIN = conteneur (Field=8)
 *      enfant direct de l'Etat, avec ses champs/events imbriques.
 *    - `<nom> est un <Type>` mono-ligne SANS bloc = reste dans
 *      "Declarations globales" (Namespace=3, kind=Variable=13).
 *
 *  Fixture commune (3 tests) : ETAT_T avec nGlobal (globale mono-ligne),
 *  section intro (EnTeteDocument, bloc), section ligne (Corps, bloc).
 *
 *   line  0 : ETAT_T est un Etat
 *   line  1 :   nGlobal est un entier = 0            <- globale mono-ligne
 *   line  2 :   intro est un EnTeteDocument           <- section/conteneur
 *   line  3 :     PROPRIETES
 *   line  4 :     FIN
 *   line  5 :     LIB_X est un Champ Libelle
 *   line  6 :       PROPRIETES
 *   line  7 :         Libelle = "X"
 *   line  8 :       FIN
 *   line  9 :     FIN
 *   line 10 :     PROCEDURE AvantImpression()
 *   line 11 :       Trace("avant")
 *   line 12 :     FIN
 *   line 13 :   FIN                                   <- fin de intro
 *   line 14 :   ligne est un Corps                    <- section/conteneur
 *   line 15 :     LIB_Y est un Champ Libelle
 *   line 16 :       PROPRIETES
 *   line 17 :         Libelle = "Y"
 *   line 18 :       FIN
 *   line 19 :     FIN
 *   line 20 :   FIN                                   <- fin de ligne
 *   line 21 :   PROCEDURE Lecture()
 *   line 22 :     Trace("lecture")
 *   line 23 :   FIN
 *   line 24 : FIN
 *
 *  Tous ROUGES avant impl : le parseur de conteneur ne reconnait pas
 *  encore les sections (intro et ligne tombent dans Declarations globales
 *  en Variable=13, et leurs enfants sont aplatis ou perdus).
 * ================================================================ */

/* Source fixture partagee (macro pour eviter la duplication). */
#define FEAT_0770_SRC \
    "ETAT_T est un Etat\n"                    /* line  0 */ \
    "\tnGlobal est un entier = 0\n"           /* line  1 */ \
    "\tintro est un EnTeteDocument\n"         /* line  2 */ \
    "\t\tPROPRIETES\n"                        /* line  3 */ \
    "\t\tFIN\n"                               /* line  4 */ \
    "\t\tLIB_X est un Champ Libelle\n"        /* line  5 */ \
    "\t\t\tPROPRIETES\n"                      /* line  6 */ \
    "\t\t\t\tLibelle = \"X\"\n"               /* line  7 */ \
    "\t\t\tFIN\n"                             /* line  8 */ \
    "\t\tFIN\n"                               /* line  9 */ \
    "\t\tPROCEDURE AvantImpression()\n"       /* line 10 */ \
    "\t\t\tTrace(\"avant\")\n"                /* line 11 */ \
    "\t\tFIN\n"                               /* line 12 */ \
    "\tFIN\n"                                 /* line 13 */ \
    "\tligne est un Corps\n"                  /* line 14 */ \
    "\t\tLIB_Y est un Champ Libelle\n"        /* line 15 */ \
    "\t\t\tPROPRIETES\n"                      /* line 16 */ \
    "\t\t\t\tLibelle = \"Y\"\n"               /* line 17 */ \
    "\t\t\tFIN\n"                             /* line 18 */ \
    "\t\tFIN\n"                               /* line 19 */ \
    "\tFIN\n"                                 /* line 20 */ \
    "\tPROCEDURE Lecture()\n"                 /* line 21 */ \
    "\t\tTrace(\"lecture\")\n"                /* line 22 */ \
    "\tFIN\n"                                 /* line 23 */ \
    "FIN\n"                                   /* line 24 */

/* ---------------------------------------------------------------
 * Test 1 : hierarchie et kinds des sections.
 * RED attendus :
 *   - intro et ligne non trouves comme enfants directs de ETAT_T
 *     (ils tombent dans Declarations globales ou sont absents)
 *   - LIB_X et AvantImpression non trouves sous intro
 *   - LIB_Y non trouve sous ligne
 *   - kinds Variable(13) au lieu de Field(8) pour intro/ligne
 * --------------------------------------------------------------- */
static void test_doc_symbol_etat_sections_hierarchy(void) {
    printf("\n[TEST] feat/0770 documentSymbol : sections etat -> conteneurs (hierarchy)\n");
    const char* src = FEAT_0770_SRC;
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* Racine ETAT_T presente et de kind Class. */
    ASSERT_SYM_EXISTS(result, "ETAT_T",
        "0770/hierarchy : racine ETAT_T presente");
    {
        JsonValue* etat    = find_sym(result, "ETAT_T");
        JsonValue* etat_ch = sym_children(etat);

        ASSERT_SYM_KIND(etat, LSP_KIND_CLASS,
            "0770/hierarchy : ETAT_T kind=Class(5)");

        /* intro : enfant direct de ETAT_T, kind Field (pas Variable). */
        ASSERT_SYM_EXISTS(etat_ch, "intro",
            "0770/hierarchy : intro enfant direct de ETAT_T");
        {
            JsonValue* sec_intro = find_sym(etat_ch, "intro");
            ASSERT_SYM_KIND(sec_intro, LSP_KIND_FIELD,
                "0770/hierarchy : intro kind=Field(8) (pas Variable=13)");

            /* intro contient 2 enfants : LIB_X et AvantImpression. */
            ASSERT_SYM_CHILDREN_COUNT(sec_intro, 2,
                "0770/hierarchy : intro a 2 enfants (LIB_X + AvantImpression)");
            JsonValue* intro_ch = sym_children(sec_intro);
            ASSERT_SYM_EXISTS(intro_ch, "LIB_X",
                "0770/hierarchy : LIB_X enfant de intro");
            {
                JsonValue* lib_x = find_sym(intro_ch, "LIB_X");
                ASSERT_SYM_KIND(lib_x, LSP_KIND_FIELD,
                    "0770/hierarchy : LIB_X kind=Field(8)");
            }
            ASSERT_SYM_EXISTS(intro_ch, "AvantImpression",
                "0770/hierarchy : AvantImpression enfant de intro");
            {
                JsonValue* av = find_sym(intro_ch, "AvantImpression");
                ASSERT_SYM_KIND(av, LSP_KIND_METHOD,
                    "0770/hierarchy : AvantImpression kind=Method(6)");
            }
        }

        /* ligne : enfant direct de ETAT_T, kind Field. */
        ASSERT_SYM_EXISTS(etat_ch, "ligne",
            "0770/hierarchy : ligne enfant direct de ETAT_T");
        {
            JsonValue* sec_ligne = find_sym(etat_ch, "ligne");
            ASSERT_SYM_KIND(sec_ligne, LSP_KIND_FIELD,
                "0770/hierarchy : ligne kind=Field(8) (pas Variable=13)");

            /* ligne contient 1 enfant : LIB_Y. */
            ASSERT_SYM_CHILDREN_COUNT(sec_ligne, 1,
                "0770/hierarchy : ligne a 1 enfant (LIB_Y)");
            JsonValue* ligne_ch = sym_children(sec_ligne);
            ASSERT_SYM_EXISTS(ligne_ch, "LIB_Y",
                "0770/hierarchy : LIB_Y enfant de ligne");
            {
                JsonValue* lib_y = find_sym(ligne_ch, "LIB_Y");
                ASSERT_SYM_KIND(lib_y, LSP_KIND_FIELD,
                    "0770/hierarchy : LIB_Y kind=Field(8)");
            }
        }

        /* Lecture : evenement etat, enfant direct de ETAT_T, kind Method. */
        ASSERT_SYM_EXISTS(etat_ch, "Lecture",
            "0770/hierarchy : Lecture enfant direct de ETAT_T");
        {
            JsonValue* lec = find_sym(etat_ch, "Lecture");
            ASSERT_SYM_KIND(lec, LSP_KIND_METHOD,
                "0770/hierarchy : Lecture kind=Method(6)");
        }
    }

    json_free(result);
    json_free(params);
}

/* ---------------------------------------------------------------
 * Test 2 : plages (ranges) des sections et de leurs contenus.
 * RED attendus : les symboles ne sont pas trouves a la bonne
 * position dans la hierarchie => ASSERT_SYM_LINE echoue.
 * --------------------------------------------------------------- */
static void test_doc_symbol_etat_sections_ranges(void) {
    printf("\n[TEST] feat/0770 documentSymbol : sections etat -> ranges correctes\n");
    const char* src = FEAT_0770_SRC;
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* etat    = find_sym(result, "ETAT_T");
    JsonValue* etat_ch = sym_children(etat);

    /* intro : line 2 (declaration) -> line 13 (FIN). */
    {
        JsonValue* sec_intro = find_sym(etat_ch, "intro");
        ASSERT_SYM_LINE(sec_intro, 2, 13,
            "0770/ranges : intro range [2..13]");

        JsonValue* intro_ch = sym_children(sec_intro);

        /* LIB_X dans intro : line 5 -> line 9. */
        {
            JsonValue* lib_x = find_sym(intro_ch, "LIB_X");
            ASSERT_SYM_LINE(lib_x, 5, 9,
                "0770/ranges : LIB_X range [5..9]");
        }

        /* AvantImpression dans intro : line 10 -> line 12. */
        {
            JsonValue* av = find_sym(intro_ch, "AvantImpression");
            ASSERT_SYM_LINE(av, 10, 12,
                "0770/ranges : AvantImpression range [10..12]");
        }
    }

    /* ligne : line 14 -> line 20. */
    {
        JsonValue* sec_ligne = find_sym(etat_ch, "ligne");
        ASSERT_SYM_LINE(sec_ligne, 14, 20,
            "0770/ranges : ligne range [14..20]");

        JsonValue* ligne_ch = sym_children(sec_ligne);

        /* LIB_Y dans ligne : line 15 -> line 19. */
        {
            JsonValue* lib_y = find_sym(ligne_ch, "LIB_Y");
            ASSERT_SYM_LINE(lib_y, 15, 19,
                "0770/ranges : LIB_Y range [15..19]");
        }
    }

    /* Lecture au niveau etat : line 21 -> line 23. */
    {
        JsonValue* lec = find_sym(etat_ch, "Lecture");
        ASSERT_SYM_LINE(lec, 21, 23,
            "0770/ranges : Lecture range [21..23]");
    }

    json_free(result);
    json_free(params);
}

/* ---------------------------------------------------------------
 * Test 3 : nGlobal reste dans "Declarations globales",
 *           les sections n'y tombent PAS.
 * RED attendus :
 *   - intro et ligne se retrouvent dans Declarations globales
 *     (bug actuel : traites comme declarations simples Variable=13)
 *   - assertions NOT_EXISTS echouent car intro/ligne y sont presents
 * --------------------------------------------------------------- */
static void test_doc_symbol_etat_sections_not_in_decl_glob(void) {
    /* UTF-8 : "D\xc3\xa9clarations globales" = "Declarations globales" */
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";

    printf("\n[TEST] feat/0770 documentSymbol : sections etat hors Decl.globales\n");
    const char* src = FEAT_0770_SRC;
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* etat    = find_sym(result, "ETAT_T");
    JsonValue* etat_ch = sym_children(etat);

    /* "Declarations globales" doit exister (pour nGlobal). */
    ASSERT_SYM_EXISTS(etat_ch, DECL_GLOB,
        "0770/decl-glob : noeud Decl.globales present (pour nGlobal)");

    {
        JsonValue* dg    = find_sym(etat_ch, DECL_GLOB);
        JsonValue* dg_ch = sym_children(dg);

        /* nGlobal (mono-ligne) est sous Decl.globales. */
        ASSERT_SYM_EXISTS(dg_ch, "nGlobal",
            "0770/decl-glob : nGlobal sous Decl.globales");

        /* Les sections intro et ligne NE doivent PAS etre dans Decl.globales. */
        ASSERT_SYM_NOT_EXISTS(dg_ch, "intro",
            "0770/decl-glob : intro ABSENT de Decl.globales (c'est un conteneur)");
        ASSERT_SYM_NOT_EXISTS(dg_ch, "ligne",
            "0770/decl-glob : ligne ABSENT de Decl.globales (c'est un conteneur)");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   -- documentSymbol enrichi (RED attendus avant impl)
 *
 *  3 enrichissements ADDITIFS (ne cassent pas les symboles/ranges deja
 *  emis, seul un champ "detail" s'ajoute) :
 *   1. Type de widget dans "detail" : `est un Champ Bouton` -> detail="Bouton"
 *      (bilingue : `is a Control Button` -> detail="Button").
 *   2. Declarations top-level d'un SCRIPT : variables (Variable + detail=type),
 *      types/enums (conteneur Enum + membres), constantes (conteneur Constant).
 *   3. Structure de `projet.owl` : racine Module + detail=type, bloc PROPRIETES
 *      (Namespace) avec ses cles (Property + detail=valeur).
 * ================================================================ */

/* ----- (1) Type de widget dans detail ------------------------------ */

/* Fixture "fenetre a champs types varies" :
 *   line 0 : FEN_Types est une Fenetre
 *   line 1 :   BTN_OK est un Champ Bouton
 *   line 2..4 :   PROPRIETES ... FIN
 *   line 5 :   FIN
 *   line 6 :   SAI_Nom est un Champ Saisie
 *   line 7..9 :   PROPRIETES ... FIN
 *   line 10 :  FIN
 *   line 11 :  TBL_Data est un Champ Table
 *   line 12..14 : PROPRIETES ... FIN
 *   line 15 :  FIN
 *   line 16 : FIN
 * Attendu : chaque champ porte "detail" = son type de widget, sans
 * regresser existence/hierarchie/ranges.
 */
static void test_doc_symbol_field_detail_widget_type(void) {
    printf("\n[TEST] feat/1195 documentSymbol : detail = type de widget (FR)\n");
    const char* src =
        "FEN_Types est une Fenetre\n"
        "\tBTN_OK est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"OK\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tSAI_Nom est un Champ Saisie\n"
        "\t\tPROPRIETES\n"
        "\t\t\tValeur = \"\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "\tTBL_Data est un Champ Table\n"
        "\t\tPROPRIETES\n"
        "\t\t\tHauteur = 200\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_Types");
    JsonValue* fen_ch = sym_children(fen);

    /* Non-regression : les 3 champs restent presents comme enfants directs. */
    ASSERT_SYM_EXISTS(fen_ch, "BTN_OK",   "1195/detail : BTN_OK present");
    ASSERT_SYM_EXISTS(fen_ch, "SAI_Nom",  "1195/detail : SAI_Nom present");
    ASSERT_SYM_EXISTS(fen_ch, "TBL_Data", "1195/detail : TBL_Data present");

    /* Non-regression range : BTN_OK ligne 1 -> FIN champ ligne 5. */
    ASSERT_SYM_LINE(find_sym(fen_ch, "BTN_OK"), 1, 5,
        "1195/detail : BTN_OK range [1..5] inchange");

    /* Enrichissement : detail = type de widget exact. */
    ASSERT_SYM_DETAIL(find_sym(fen_ch, "BTN_OK"),   "Bouton",
        "1195/detail : BTN_OK detail=Bouton");
    ASSERT_SYM_DETAIL(find_sym(fen_ch, "SAI_Nom"),  "Saisie",
        "1195/detail : SAI_Nom detail=Saisie");
    ASSERT_SYM_DETAIL(find_sym(fen_ch, "TBL_Data"), "Table",
        "1195/detail : TBL_Data detail=Table");

    json_free(result);
    json_free(params);
}

/* Fixture EN : `is a Window` + `is a Control <Type>`.
 *   line 0 : FEN_En is a Window
 *   line 1 :   BTN_Go is a Control Button
 *   line 2..4 : PROPRIETES ... FIN
 *   line 5 :   FIN
 *   line 6 : FIN
 * Attendu : BTN_Go present (non-regression) + detail = "Button".
 */
static void test_doc_symbol_field_detail_en(void) {
    printf("\n[TEST] feat/1195 documentSymbol : detail = type de widget (EN)\n");
    const char* src =
        "FEN_En is a Window\n"
        "\tBTN_Go is a Control Button\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Go\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* fen    = find_sym(result, "FEN_En");
    JsonValue* fen_ch = sym_children(fen);

    ASSERT_SYM_EXISTS(fen_ch, "BTN_Go", "1195/detail-en : BTN_Go present");
    ASSERT_SYM_DETAIL(find_sym(fen_ch, "BTN_Go"), "Button",
        "1195/detail-en : BTN_Go detail=Button");

    json_free(result);
    json_free(params);
}

/* ----- (2) Declarations top-level d'un script ---------------------- */

/* Fixture "script : variables top-level" :
 *   line 0 : gCompteur est un entier = 0
 *   line 1 : gNom est une chaine = "hello"
 * Les variables globales top-level sont regroupees sous UN noeud
 * "Declarations globales" (Namespace 3, meme modele que la fenetre) pose en
 * tete, dont elles sont les enfants (Variable, detail = type declare).
 * Range = 1re -> derniere declaration. */
static void test_doc_symbol_script_vars(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1230 documentSymbol : variables top-level sous Declarations globales\n");
    const char* src =
        "gCompteur est un entier = 0\n"
        "gNom est une chaine = \"hello\"\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* Un seul symbole racine : Declarations globales, regroupant les 2 variables. */
    ASSERT_SYM_ARRAY_COUNT(result, 1, "1230/script-vars : 1 symbole racine (Decl.globales)");
    JsonValue* dg = find_sym(result, DECL_GLOB);
    ASSERT_SYM_EXISTS(result, DECL_GLOB,
        "1230/script-vars : Declarations globales present");
    ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
        "1230/script-vars : Declarations globales kind=Namespace(3)");
    ASSERT_SYM_LINE(dg, 0, 1,
        "1230/script-vars : Declarations globales range [0..1]");

    JsonValue* dg_ch = sym_children(dg);
    ASSERT_SYM_CHILDREN_COUNT(dg, 2,
        "1230/script-vars : 2 variables sous Declarations globales");
    ASSERT_SYM_EXISTS(dg_ch, "gCompteur", "1230/script-vars : gCompteur present");
    ASSERT_SYM_EXISTS(dg_ch, "gNom",      "1230/script-vars : gNom present");
    ASSERT_SYM_KIND(find_sym(dg_ch, "gCompteur"), LSP_KIND_VARIABLE,
        "1230/script-vars : gCompteur kind=Variable(13)");
    ASSERT_SYM_DETAIL(find_sym(dg_ch, "gCompteur"), "entier",
        "1230/script-vars : gCompteur detail=entier");
    ASSERT_SYM_DETAIL(find_sym(dg_ch, "gNom"), "chaine",
        "1230/script-vars : gNom detail=chaine");

    json_free(result);
    json_free(params);
}

/* Fixture "script : type enum top-level" :
 *   line 0 : EStatut est une Enumeration
 *   line 1 :   actif = 1
 *   line 2 :   inactif = 2
 *   line 3 : FIN
 * Attendu : racine EStatut kind=Enum(10) avec 2 membres EnumMember(22).
 */
static void test_doc_symbol_script_enum(void) {
    printf("\n[TEST] feat/1195 documentSymbol : script type Enumeration\n");
    const char* src =
        "EStatut est une Enumeration\n"
        "\tactif = 1\n"
        "\tinactif = 2\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1195/script-enum : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "EStatut", "1195/script-enum : EStatut present");
    ASSERT_SYM_KIND(find_sym(result, "EStatut"), LSP_KIND_ENUM,
        "1195/script-enum : EStatut kind=Enum(10)");
    {
        JsonValue* en    = find_sym(result, "EStatut");
        JsonValue* en_ch = sym_children(en);
        ASSERT_SYM_CHILDREN_COUNT(en, 2, "1195/script-enum : 2 membres");
        ASSERT_SYM_EXISTS(en_ch, "actif",   "1195/script-enum : actif membre");
        ASSERT_SYM_EXISTS(en_ch, "inactif", "1195/script-enum : inactif membre");
        ASSERT_SYM_KIND(find_sym(en_ch, "actif"), LSP_KIND_ENUMMEMBER,
            "1195/script-enum : actif kind=EnumMember(22)");
    }

    json_free(result);
    json_free(params);
}

/* Fixture "script : bloc de constantes top-level" :
 *   line 0 : CONSTANTE
 *   line 1 :   SEUIL = 100
 *   line 2 :   NOM = "app"
 *   line 3 : FIN
 * Attendu : racine "CONSTANTE" kind=Constant(14) avec 2 membres Constant(14).
 */
static void test_doc_symbol_script_constants(void) {
    printf("\n[TEST] feat/1195 documentSymbol : script bloc CONSTANTE\n");
    const char* src =
        "CONSTANTE\n"
        "\tSEUIL = 100\n"
        "\tNOM = \"app\"\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1195/script-const : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "CONSTANTE", "1195/script-const : bloc CONSTANTE present");
    ASSERT_SYM_KIND(find_sym(result, "CONSTANTE"), LSP_KIND_CONSTANT,
        "1195/script-const : CONSTANTE kind=Constant(14)");
    {
        JsonValue* cst    = find_sym(result, "CONSTANTE");
        JsonValue* cst_ch = sym_children(cst);
        ASSERT_SYM_EXISTS(cst_ch, "SEUIL", "1195/script-const : SEUIL membre");
        ASSERT_SYM_EXISTS(cst_ch, "NOM",   "1195/script-const : NOM membre");
        ASSERT_SYM_KIND(find_sym(cst_ch, "SEUIL"), LSP_KIND_CONSTANT,
            "1195/script-const : SEUIL kind=Constant(14)");
    }

    json_free(result);
    json_free(params);
}

/* Module mixte variable + procedure + code imperatif top-level :
 * la procedure reste un frere racine (Function) ; la variable globale passe
 * sous "Declarations globales" (Namespace 3) pose en tete ; le code imperatif
 * (Trace) n'est PAS capture (declarations seules, modele fenetre). */
static void test_doc_symbol_script_mixed_no_regress(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1230 documentSymbol : script mixte (variable sous Declarations globales)\n");
    const char* src =
        "gEtat est un entier = 0\n"
        "\n"
        "PROCEDURE PUBLIQUE scenario_foo()\n"
        "\tTrace(\"foo\")\n"
        "FIN\n"
        "\n"
        "Trace(\"demarrage\")\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* 2 symboles racine : Declarations globales (1er) + scenario_foo (frere).
     * Le Trace top-level ne produit AUCUN symbole. */
    ASSERT_SYM_ARRAY_COUNT(result, 2, "1230/script-mixte : 2 symboles racine");

    /* La procedure reste un frere racine Function. */
    ASSERT_SYM_EXISTS(result, "scenario_foo",
        "1230/script-mixte : scenario_foo present");
    ASSERT_SYM_KIND(find_sym(result, "scenario_foo"), LSP_KIND_FUNCTION,
        "1230/script-mixte : scenario_foo kind=Function(12)");

    /* La variable globale passe sous Declarations globales ; range = la decl seule. */
    JsonValue* dg = find_sym(result, DECL_GLOB);
    ASSERT_SYM_EXISTS(result, DECL_GLOB,
        "1230/script-mixte : Declarations globales present");
    ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
        "1230/script-mixte : Declarations globales kind=Namespace(3)");
    ASSERT_SYM_LINE(dg, 0, 0,
        "1230/script-mixte : range [0..0] (declaration seule, pas le Trace)");
    {
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_CHILDREN_COUNT(dg, 1,
            "1230/script-mixte : 1 enfant (gEtat), pas de code imperatif");
        ASSERT_SYM_EXISTS(dg_ch, "gEtat", "1230/script-mixte : gEtat sous Declarations globales");
        ASSERT_SYM_KIND(find_sym(dg_ch, "gEtat"), LSP_KIND_VARIABLE,
            "1230/script-mixte : gEtat kind=Variable(13)");
    }
    ASSERT_SYM_NOT_EXISTS(result, "gEtat",
        "1230/script-mixte : gEtat PLUS un frere racine (sous Declarations globales)");

    json_free(result);
    json_free(params);
}

/* ----- (3) Structure de projet.owl --------------------------------- */

/* Fixture "projet.owl" :
 *   line 0 : MonApp est un Projet
 *   line 1 :   PROPRIETES
 *   line 2 :     Langues = [langueFrancais]
 *   line 3 :     FenetreDemarrage = FEN_Accueil
 *   line 4 :     Description = "Mon application"
 *   line 5 :   FIN
 *   line 6 : (vide)
 *   line 7 :   Resultat est une chaine = Ouvre(FEN_Accueil)
 *   line 8 : FIN
 * Attendu : racine MonApp kind=Module(2) detail="Projet" ; enfant PROPRIETES
 * (Namespace) portant les cles (Property + detail=valeur) ; la variable
 * top-level du corps (Resultat) apparait aussi.
 */
static void test_doc_symbol_projet(void) {
    printf("\n[TEST] feat/1195 documentSymbol : projet.owl (FR)\n");
    const char* src =
        "MonApp est un Projet\n"
        "\tPROPRIETES\n"
        "\t\tLangues = [langueFrancais]\n"
        "\t\tFenetreDemarrage = FEN_Accueil\n"
        "\t\tDescription = \"Mon application\"\n"
        "\tFIN\n"
        "\n"
        "\tResultat est une chaine = Ouvre(FEN_Accueil)\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1195/projet : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "MonApp", "1195/projet : MonApp present");
    ASSERT_SYM_KIND(find_sym(result, "MonApp"), LSP_KIND_MODULE,
        "1195/projet : MonApp kind=Module(2)");
    ASSERT_SYM_DETAIL(find_sym(result, "MonApp"), "Projet",
        "1195/projet : MonApp detail=Projet");

    {
        JsonValue* proj    = find_sym(result, "MonApp");
        JsonValue* proj_ch = sym_children(proj);

        ASSERT_SYM_EXISTS(proj_ch, "PROPRIETES",
            "1195/projet : bloc PROPRIETES present");
        ASSERT_SYM_EXISTS(proj_ch, "Resultat",
            "1195/projet : variable corps Resultat presente");
        ASSERT_SYM_DETAIL(find_sym(proj_ch, "Resultat"), "chaine",
            "1195/projet : Resultat detail=chaine");

        JsonValue* props    = find_sym(proj_ch, "PROPRIETES");
        JsonValue* props_ch = sym_children(props);
        ASSERT_SYM_KIND(props, LSP_KIND_NAMESPACE,
            "1195/projet : PROPRIETES kind=Namespace(3)");
        ASSERT_SYM_EXISTS(props_ch, "Langues",
            "1195/projet : cle Langues");
        ASSERT_SYM_EXISTS(props_ch, "FenetreDemarrage",
            "1195/projet : cle FenetreDemarrage");
        ASSERT_SYM_EXISTS(props_ch, "Description",
            "1195/projet : cle Description");
        ASSERT_SYM_KIND(find_sym(props_ch, "Langues"), LSP_KIND_PROPERTY,
            "1195/projet : Langues kind=Property(7)");
        ASSERT_SYM_DETAIL(find_sym(props_ch, "Langues"), "[langueFrancais]",
            "1195/projet : Langues detail=[langueFrancais]");
        ASSERT_SYM_DETAIL(find_sym(props_ch, "FenetreDemarrage"), "FEN_Accueil",
            "1195/projet : FenetreDemarrage detail=FEN_Accueil");
        ASSERT_SYM_DETAIL(find_sym(props_ch, "Description"), "\"Mon application\"",
            "1195/projet : Description detail=\"Mon application\"");
    }

    json_free(result);
    json_free(params);
}

/* Fixture "projet.owl EN" : `is a Project` + `PROPERTIES`.
 *   line 0 : MyApp is a Project
 *   line 1 :   PROPERTIES
 *   line 2 :     Langues = [langueEnglish]
 *   line 3 :   FIN
 *   line 4 : FIN
 * Attendu : racine MyApp kind=Module detail="Project", PROPERTIES + Langues.
 */
static void test_doc_symbol_projet_en(void) {
    printf("\n[TEST] feat/1195 documentSymbol : projet.owl (EN)\n");
    const char* src =
        "MyApp is a Project\n"
        "\tPROPERTIES\n"
        "\t\tLangues = [langueEnglish]\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1195/projet-en : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "MyApp", "1195/projet-en : MyApp present");
    ASSERT_SYM_KIND(find_sym(result, "MyApp"), LSP_KIND_MODULE,
        "1195/projet-en : MyApp kind=Module(2)");
    ASSERT_SYM_DETAIL(find_sym(result, "MyApp"), "Project",
        "1195/projet-en : MyApp detail=Project");
    {
        JsonValue* proj    = find_sym(result, "MyApp");
        JsonValue* proj_ch = sym_children(proj);
        ASSERT_SYM_EXISTS(proj_ch, "PROPERTIES",
            "1195/projet-en : bloc PROPERTIES present");
        JsonValue* props_ch = sym_children(find_sym(proj_ch, "PROPERTIES"));
        ASSERT_SYM_EXISTS(props_ch, "Langues",
            "1195/projet-en : cle Langues");
    }

    json_free(result);
    json_free(params);
}

/* Fixture "theme.owl FR" : `<Nom> est un Thème` + styles nommes en enfants.
 * Le litteral `{ ... }` de chaque style (Cadre/Etats/Couleur...) NE DOIT PAS
 * apparaitre comme sous-symbole : on reste a 2 niveaux (Theme -> styles).
 *   line 0 : ThemeDemo est un Thème
 *   line 1 :   fondFenetre est un styleConteneur = {
 *   line 2 :     CadreExterieur = { CouleurFond = RVB(245, 245, 247) }
 *   line 3 :   }
 *   line 4 :   boutonPrimaire est un styleBouton = {
 *   line 5 :     Libellé = { Couleur = Blanc }
 *   line 6 :     Etats = {
 *   line 7 :       Survol = { CouleurFond = Bleu }
 *   line 8 :     }
 *   line 9 :   }
 *   line 10: FIN
 * Attendu : racine ThemeDemo kind=Module detail="Thème",
 *           enfants = fondFenetre (styleConteneur), boutonPrimaire (styleBouton),
 *           PAS de sous-symbole "CadreExterieur"/"Etats"/"Survol"/"Libellé". */
static void test_doc_symbol_theme(void) {
    printf("\n[TEST] fix/1197 documentSymbol : theme (FR)\n");
    const char* src =
        "ThemeDemo est un Th\xc3\xa8me\n"
        "\tfondFenetre est un styleConteneur = {\n"
        "\t\tCadreExterieur = { CouleurFond = RVB(245, 245, 247) }\n"
        "\t}\n"
        "\tboutonPrimaire est un styleBouton = {\n"
        "\t\tLibell\xc3\xa9 = { Couleur = Blanc }\n"
        "\t\tEtats = {\n"
        "\t\t\tSurvol = { CouleurFond = Bleu }\n"
        "\t\t}\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1197/theme : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "ThemeDemo", "1197/theme : ThemeDemo present");
    ASSERT_SYM_KIND(find_sym(result, "ThemeDemo"), LSP_KIND_MODULE,
        "1197/theme : ThemeDemo kind=Module(2)");
    ASSERT_SYM_DETAIL(find_sym(result, "ThemeDemo"), "Th\xc3\xa8me",
        "1197/theme : ThemeDemo detail=Th\xc3\xa8me");
    ASSERT_SYM_LINE(find_sym(result, "ThemeDemo"), 0, 10,
        "1197/theme : racine range [0..10]");

    {
        JsonValue* th    = find_sym(result, "ThemeDemo");
        JsonValue* th_ch = sym_children(th);

        ASSERT_SYM_CHILDREN_COUNT(th, 2, "1197/theme : 2 styles enfants");
        ASSERT_SYM_EXISTS(th_ch, "fondFenetre",
            "1197/theme : style fondFenetre present");
        ASSERT_SYM_EXISTS(th_ch, "boutonPrimaire",
            "1197/theme : style boutonPrimaire present");
        ASSERT_SYM_KIND(find_sym(th_ch, "fondFenetre"), LSP_KIND_VARIABLE,
            "1197/theme : fondFenetre kind=Variable(13)");
        ASSERT_SYM_DETAIL(find_sym(th_ch, "fondFenetre"), "styleConteneur",
            "1197/theme : fondFenetre detail=styleConteneur");
        ASSERT_SYM_DETAIL(find_sym(th_ch, "boutonPrimaire"), "styleBouton",
            "1197/theme : boutonPrimaire detail=styleBouton");
        ASSERT_SYM_LINE(find_sym(th_ch, "boutonPrimaire"), 4, 9,
            "1197/theme : boutonPrimaire range [4..9] (bloc complet)");

        /* Le litteral de style NE DOIT PAS produire de sous-symboles :
         * ni au niveau racine, ni comme enfant d'un style. */
        ASSERT_SYM_CHILDREN_COUNT(find_sym(th_ch, "boutonPrimaire"), 0,
            "1197/theme : boutonPrimaire sans sous-symbole (pas de vrac)");
        if (find_sym(th_ch, "Etats") || find_sym(th_ch, "CadreExterieur") ||
            find_sym(th_ch, "Survol") || find_sym(result, "Etats")) {
            g_fail++;
            printf("  [FAIL] 1197/theme : le litteral { } fuit en symbole (vrac)\n");
        } else {
            g_pass++;
            printf("  [OK]   1197/theme : aucun symbole issu du litteral { }\n");
        }
    }

    json_free(result);
    json_free(params);
}

/* Fixture "theme.owl EN" : `<Nom> is a Theme` + styles `is a style<Type>`.
 *   line 0 : ThemeDemoEN is a Theme
 *   line 1 :   labelTitle is a styleLabel = {
 *   line 2 :     Label = { Color = RGB(29, 29, 31) }
 *   line 3 :   }
 *   line 4 : FIN
 * Attendu : racine ThemeDemoEN detail="Theme", enfant labelTitle (styleLabel). */
static void test_doc_symbol_theme_en(void) {
    printf("\n[TEST] fix/1197 documentSymbol : theme (EN)\n");
    const char* src =
        "ThemeDemoEN is a Theme\n"
        "\tlabelTitle is a styleLabel = {\n"
        "\t\tLabel = { Color = RGB(29, 29, 31) }\n"
        "\t}\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1197/theme-en : 1 symbole racine");
    ASSERT_SYM_EXISTS(result, "ThemeDemoEN", "1197/theme-en : ThemeDemoEN present");
    ASSERT_SYM_KIND(find_sym(result, "ThemeDemoEN"), LSP_KIND_MODULE,
        "1197/theme-en : ThemeDemoEN kind=Module(2)");
    ASSERT_SYM_DETAIL(find_sym(result, "ThemeDemoEN"), "Theme",
        "1197/theme-en : ThemeDemoEN detail=Theme");
    {
        JsonValue* th    = find_sym(result, "ThemeDemoEN");
        JsonValue* th_ch = sym_children(th);
        ASSERT_SYM_CHILDREN_COUNT(th, 1, "1197/theme-en : 1 style enfant");
        ASSERT_SYM_EXISTS(th_ch, "labelTitle",
            "1197/theme-en : style labelTitle present");
        ASSERT_SYM_DETAIL(find_sym(th_ch, "labelTitle"), "styleLabel",
            "1197/theme-en : labelTitle detail=styleLabel");
    }

    json_free(result);
    json_free(params);
}

/* ================================================================
 *   -- documentSymbol : "Declarations globales" en tete
 *
 *  Les declarations de variables globales d'un element (fichier plat ou
 *  bloc CollectionDeProcedures) sont regroupees sous UN noeud
 *  "Declarations globales" (FR) / "Global declarations" (EN, localise selon
 *  la langue du fichier), kind Namespace(3), POSE EN PREMIER — meme modele
 *  que la fenetre. Il CONTIENT les declarations de variables globales
 *  (enfants imbriques) SEULES ; le code imperatif n'est PAS capture. Les
 *  enumerations, classes, procedures restent freres APRES. Range = 1re ->
 *  derniere declaration. Apparait des qu'il y a >=1 declaration de variable.
 * ================================================================ */

/* Fixture "fichier plat FR : variable globale + code imperatif" :
 *   line 0 : gCompteur est un entier = 0     (variable globale)
 *   line 1 :
 *   line 2 : PROCEDURE PUBLIQUE traiter()     (callable -> Function, frere)
 *   line 3 :   Trace("dans traiter")
 *   line 4 : FIN
 *   line 5 :
 *   line 6 : Trace("demarrage")               (code imperatif, NON capture)
 *   line 7 : traiter()                         (code imperatif, NON capture)
 * Attendu : "Declarations globales" (Namespace 3) EN PREMIER, range [0..0]
 * (la declaration seule ; le code imperatif ne contribue PAS au range).
 * gCompteur est un ENFANT (plus un frere plat). traiter reste un frere. */
static void test_doc_symbol_module_init_fr(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1230 documentSymbol : Declarations globales (fichier plat FR)\n");
    const char* src =
        "gCompteur est un entier = 0\n"
        "\n"
        "PROCEDURE PUBLIQUE traiter()\n"
        "\tTrace(\"dans traiter\")\n"
        "FIN\n"
        "\n"
        "Trace(\"demarrage\")\n"
        "traiter()\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    /* Exactement 2 symboles racine : Declarations globales (1er) + traiter. */
    ASSERT_SYM_ARRAY_COUNT(result, 2, "1230/init-fr : 2 symboles racine");

    /* Declarations globales pose EN PREMIER. */
    {
        JsonValue* first = (result && result->type == JSON_ARRAY && result->as.arrayVal.count > 0)
                           ? result->as.arrayVal.items[0] : NULL;
        const char* fn = first ? json_get_string(first, "name") : NULL;
        if (fn && strcmp(fn, DECL_GLOB) == 0) {
            g_pass++;
            printf("  [OK]   1230/init-fr : Declarations globales est le 1er enfant\n");
        } else {
            g_fail++;
            printf("  [FAIL] 1230/init-fr : 1er enfant attendu 'Declarations globales', obtenu '%s'\n",
                   fn ? fn : "(null)");
        }
    }

    JsonValue* dg = find_sym(result, DECL_GLOB);
    ASSERT_SYM_EXISTS(result, DECL_GLOB,
        "1230/init-fr : symbole Declarations globales present");
    ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
        "1230/init-fr : Declarations globales kind=Namespace(3)");
    ASSERT_SYM_LINE(dg, 0, 0,
        "1230/init-fr : range [0..0] (declaration seule, pas le code imperatif)");
    {
        int sel = sym_sel_start_line(dg);
        if (sel == 0) {
            g_pass++;
            printf("  [OK]   1230/init-fr : selectionRange 1re ligne (0)\n");
        } else {
            g_fail++;
            printf("  [FAIL] 1230/init-fr : selectionRange attendu 0, obtenu %d\n", sel);
        }
    }

    /* gCompteur : ENFANT de Declarations globales (plus un frere plat). */
    {
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_CHILDREN_COUNT(dg, 1,
            "1230/init-fr : Declarations globales a 1 enfant (gCompteur)");
        ASSERT_SYM_EXISTS(dg_ch, "gCompteur",
            "1230/init-fr : gCompteur sous Declarations globales");
        ASSERT_SYM_KIND(find_sym(dg_ch, "gCompteur"), LSP_KIND_VARIABLE,
            "1230/init-fr : gCompteur kind=Variable(13)");
        ASSERT_SYM_DETAIL(find_sym(dg_ch, "gCompteur"), "entier",
            "1230/init-fr : gCompteur detail=entier");
    }
    ASSERT_SYM_NOT_EXISTS(result, "gCompteur",
        "1230/init-fr : gCompteur PLUS un frere racine (sous Declarations globales)");

    /* traiter : frere racine apres Declarations globales. */
    ASSERT_SYM_EXISTS(result, "traiter", "1230/init-fr : traiter frere racine");
    ASSERT_SYM_KIND(find_sym(result, "traiter"), LSP_KIND_FUNCTION,
        "1230/init-fr : traiter kind=Function(12)");

    json_free(result);
    json_free(params);
}

/* Fixture "aucune declaration de variable" (callables + code imperatif seul) :
 *   line 0 : PROCEDURE PUBLIQUE scenario_foo()
 *   line 1 :   Trace("foo")
 *   line 2 : FIN
 *   line 3 :
 *   line 4 : Trace("demarrage")               (code imperatif, PAS une decl)
 * Attendu : AUCUN noeud Declarations globales — il n'apparait que s'il
 * existe >=1 declaration de variable globale (le code imperatif seul ne
 * suffit plus, modele fenetre). */
static void test_doc_symbol_module_init_none(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1230 documentSymbol : imperatif seul sans decl -> pas de noeud\n");
    const char* src =
        "PROCEDURE PUBLIQUE scenario_foo()\n"
        "\tTrace(\"foo\")\n"
        "FIN\n"
        "\n"
        "Trace(\"demarrage\")\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, 1, "1230/init-none : 1 seul symbole racine");
    ASSERT_SYM_EXISTS(result, "scenario_foo",
        "1230/init-none : scenario_foo present");
    ASSERT_SYM_NOT_EXISTS(result, DECL_GLOB,
        "1230/init-none : pas de noeud Declarations globales (FR)");
    ASSERT_SYM_NOT_EXISTS(result, "Global declarations",
        "1230/init-none : pas de noeud Global declarations (EN)");

    json_free(result);
    json_free(params);
}

/* Fixture "fichier plat EN : variable globale + code imperatif" (mots-cles EN) :
 *   line 0 : gReady is a boolean = False       (variable globale)
 *   line 1 :
 *   line 2 : PROCEDURE PUBLIQUE process()       (callable, frere)
 *   line 3 :   Trace("in process")
 *   line 4 : END
 *   line 5 :
 *   line 6 : process()                          (code imperatif, NON capture)
 * Attendu : libelle localise "Global declarations" (EN), Namespace(3), pose
 * en premier, contenant la variable gReady ; pas de forme FR ; range [0..0]. */
static void test_doc_symbol_module_init_en(void) {
    printf("\n[TEST] fix/1230 documentSymbol : Global declarations (fichier plat EN)\n");
    const char* src =
        "gReady is a boolean = False\n"
        "\n"
        "PROCEDURE PUBLIQUE process()\n"
        "\tTrace(\"in process\")\n"
        "END\n"
        "\n"
        "process()\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    JsonValue* dg = find_sym(result, "Global declarations");
    ASSERT_SYM_EXISTS(result, "Global declarations",
        "1230/init-en : noeud Global declarations present (localise EN)");
    ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
        "1230/init-en : Global declarations kind=Namespace(3)");
    ASSERT_SYM_NOT_EXISTS(result, "D\xc3\xa9" "clarations globales",
        "1230/init-en : pas de forme FR dans un fichier EN");
    ASSERT_SYM_LINE(dg, 0, 0,
        "1230/init-en : range [0..0] (declaration seule, pas le code imperatif)");

    /* La variable globale EN passe sous Global declarations. */
    {
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_EXISTS(dg_ch, "gReady",
            "1230/init-en : gReady sous Global declarations");
        ASSERT_SYM_KIND(find_sym(dg_ch, "gReady"), LSP_KIND_VARIABLE,
            "1230/init-en : gReady kind=Variable(13)");
    }
    ASSERT_SYM_EXISTS(result, "process", "1230/init-en : process frere racine");

    json_free(result);
    json_free(params);
}

/* Fixture "bloc CollectionDeProcedures : variable globale + code imperatif"
 * (cas reel support #190) :
 *   line 0 : COL_Utils est une CollectionDeProcedures   (conteneur)
 *   line 1 :   gMode est un entier = 0                   (variable globale)
 *   line 2 :
 *   line 3 :   PROCEDURE PUBLIQUE traiter()              (membre -> Method, frere)
 *   line 4 :     Trace("dans traiter")
 *   line 5 :   FIN
 *   line 6 :
 *   line 7 :   Trace("demarrage")                        (code imperatif, NON capture)
 *   line 8 :   traiter()                                 (code imperatif, NON capture)
 *   line 9 : FIN
 * Attendu : sous COL_Utils, "Declarations globales" (Namespace 3) EN PREMIER,
 * range [1..1] (la declaration seule ; le code imperatif ne contribue PAS).
 * gMode est un ENFANT (plus un enfant plat de COL_Utils). traiter reste un
 * enfant direct de COL_Utils (frere apres Declarations globales). */
static void test_doc_symbol_collection_init_fr(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1230 documentSymbol : Declarations globales (CollectionDeProcedures)\n");
    const char* src =
        "COL_Utils est une CollectionDeProc\xc3\xa9""dures\n"
        "\tgMode est un entier = 0\n"
        "\n"
        "\tPROCEDURE PUBLIQUE traiter()\n"
        "\t\tTrace(\"dans traiter\")\n"
        "\tFIN\n"
        "\n"
        "\tTrace(\"demarrage\")\n"
        "\ttraiter()\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "COL_Utils", "1230/col-init : COL_Utils present");
    JsonValue* col    = find_sym(result, "COL_Utils");
    JsonValue* col_ch = sym_children(col);

    /* 2 enfants directs : Declarations globales (1er) + traiter (frere). */
    ASSERT_SYM_CHILDREN_COUNT(col, 2,
        "1230/col-init : COL_Utils a 2 enfants (Decl.globales + traiter)");
    {
        JsonValue* first = (col_ch && col_ch->type == JSON_ARRAY && col_ch->as.arrayVal.count > 0)
                           ? col_ch->as.arrayVal.items[0] : NULL;
        const char* fn = first ? json_get_string(first, "name") : NULL;
        if (fn && strcmp(fn, DECL_GLOB) == 0) {
            g_pass++;
            printf("  [OK]   1230/col-init : Declarations globales est le 1er enfant\n");
        } else {
            g_fail++;
            printf("  [FAIL] 1230/col-init : 1er enfant attendu 'Declarations globales', obtenu '%s'\n",
                   fn ? fn : "(null)");
        }
    }

    /* Declarations globales enfant de COL_Utils, avec la variable gMode en enfant. */
    JsonValue* dg = find_sym(col_ch, DECL_GLOB);
    ASSERT_SYM_EXISTS(col_ch, DECL_GLOB,
        "1230/col-init : Declarations globales enfant de COL_Utils");
    ASSERT_SYM_KIND(dg, LSP_KIND_NAMESPACE,
        "1230/col-init : Declarations globales kind=Namespace(3)");
    ASSERT_SYM_LINE(dg, 1, 1,
        "1230/col-init : range [1..1] (declaration seule, pas le code imperatif)");
    {
        int sel = sym_sel_start_line(dg);
        if (sel == 1) {
            g_pass++;
            printf("  [OK]   1230/col-init : selectionRange 1re ligne (1)\n");
        } else {
            g_fail++;
            printf("  [FAIL] 1230/col-init : selectionRange attendu 1, obtenu %d\n", sel);
        }
    }
    {
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_CHILDREN_COUNT(dg, 1,
            "1230/col-init : Declarations globales a 1 enfant (gMode)");
        ASSERT_SYM_EXISTS(dg_ch, "gMode",
            "1230/col-init : gMode sous Declarations globales");
        ASSERT_SYM_KIND(find_sym(dg_ch, "gMode"), LSP_KIND_FIELD,
            "1230/col-init : gMode kind=Field(8)");
    }
    ASSERT_SYM_NOT_EXISTS(col_ch, "gMode",
        "1230/col-init : gMode PLUS un enfant direct de COL_Utils (sous Decl.globales)");

    /* traiter : enfant direct de COL_Utils (frere apres Declarations globales). */
    ASSERT_SYM_EXISTS(col_ch, "traiter",
        "1230/col-init : traiter enfant direct de COL_Utils");
    ASSERT_SYM_KIND(find_sym(col_ch, "traiter"), LSP_KIND_METHOD,
        "1230/col-init : traiter kind=Method(6)");

    json_free(result);
    json_free(params);
}

/* Fixture "collection : code imperatif seul, aucune declaration de variable" :
 *   line 0 : COL_Vide est une CollectionDeProcedures
 *   line 1 :   PROCEDURE PUBLIQUE scenario_foo()
 *   line 2 :     Trace("foo")
 *   line 3 :   FIN
 *   line 4 :   Trace("demarrage")             (code imperatif, PAS une decl)
 *   line 5 : FIN
 * Attendu : AUCUN noeud Declarations globales sous COL_Vide (le code
 * imperatif seul ne suffit plus a l'emettre, modele fenetre). */
static void test_doc_symbol_collection_init_none(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1230 documentSymbol : collection imperatif seul -> pas de noeud\n");
    const char* src =
        "COL_Vide est une CollectionDeProc\xc3\xa9""dures\n"
        "\tPROCEDURE PUBLIQUE scenario_foo()\n"
        "\t\tTrace(\"foo\")\n"
        "\tFIN\n"
        "\tTrace(\"demarrage\")\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "COL_Vide", "1230/col-none : COL_Vide present");
    JsonValue* col    = find_sym(result, "COL_Vide");
    JsonValue* col_ch = sym_children(col);

    ASSERT_SYM_EXISTS(col_ch, "scenario_foo",
        "1230/col-none : scenario_foo present");
    ASSERT_SYM_NOT_EXISTS(col_ch, DECL_GLOB,
        "1230/col-none : pas de noeud Declarations globales (FR)");
    ASSERT_SYM_NOT_EXISTS(col_ch, "Global declarations",
        "1230/col-none : pas de noeud Global declarations (EN)");

    json_free(result);
    json_free(params);
}

/* Fixture "collection contenant une structure imbriquee" :
 *   line 0 : COL_Modele est une CollectionDeProcedures
 *   line 1 :   stPoint est une Structure
 *   line 2 :     x est un entier
 *   line 3 :     y est un entier
 *   line 4 :   FIN
 *   line 5 :   gCompteur est un entier = 0        (VRAIE variable globale)
 *   line 6 :   PROCEDURE PUBLIQUE traiter()
 *   line 7 :     Trace("x")
 *   line 8 :   FIN
 *   line 9 : FIN
 * Attendu (, conteneur-avec-enfants) :
 *   - stPoint = UN noeud Struct(23), enfant direct de la collection, portant
 *     ses champs x/y (Field=8) en ENFANTS ;
 *   - x et y ne remontent PAS a plat sous "Declarations globales" ;
 *   - gCompteur (vraie globale) reste sous "Declarations globales" ;
 *   - traiter reste un frere Method(6).
 */
static void test_doc_symbol_collection_nested_struct(void) {
    const char* DECL_GLOB = "D\xc3\xa9" "clarations globales";
    printf("\n[TEST] fix/1762 documentSymbol : structure imbriquee dans une collection\n");
    const char* src =
        "COL_Modele est une CollectionDeProc\xc3\xa9""dures\n"
        "\tstPoint est une Structure\n"
        "\t\tx est un entier\n"
        "\t\ty est un entier\n"
        "\tFIN\n"
        "\tgCompteur est un entier = 0\n"
        "\tPROCEDURE PUBLIQUE traiter()\n"
        "\t\tTrace(\"x\")\n"
        "\tFIN\n"
        "FIN\n";
    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "COL_Modele", "1762/nested : COL_Modele present");
    JsonValue* col    = find_sym(result, "COL_Modele");
    JsonValue* col_ch = sym_children(col);

    /* stPoint : noeud conteneur Struct(23), enfant DIRECT de la collection. */
    ASSERT_SYM_EXISTS(col_ch, "stPoint",
        "1762/nested : stPoint enfant direct de la collection");
    JsonValue* st = find_sym(col_ch, "stPoint");
    ASSERT_SYM_KIND(st, LSP_KIND_STRUCT,
        "1762/nested : stPoint kind=Struct(23)");
    ASSERT_SYM_DETAIL(st, "Structure",
        "1762/nested : stPoint detail=Structure (type ecrit)");
    ASSERT_SYM_LINE(st, 1, 4,
        "1762/nested : stPoint range [1..4] (declaration + FIN)");

    /* Ses champs x/y sont des ENFANTS du noeud structure (Field=8). */
    {
        JsonValue* st_ch = sym_children(st);
        ASSERT_SYM_CHILDREN_COUNT(st, 2,
            "1762/nested : stPoint a 2 champs enfants (x, y)");
        ASSERT_SYM_EXISTS(st_ch, "x", "1762/nested : x enfant de stPoint");
        ASSERT_SYM_EXISTS(st_ch, "y", "1762/nested : y enfant de stPoint");
        ASSERT_SYM_KIND(find_sym(st_ch, "x"), LSP_KIND_FIELD,
            "1762/nested : x kind=Field(8)");
        ASSERT_SYM_KIND(find_sym(st_ch, "y"), LSP_KIND_FIELD,
            "1762/nested : y kind=Field(8)");
    }

    /* x/y ne sont PAS des enfants directs de la collection. */
    ASSERT_SYM_NOT_EXISTS(col_ch, "x",
        "1762/nested : x n'est PAS enfant direct de la collection");
    ASSERT_SYM_NOT_EXISTS(col_ch, "y",
        "1762/nested : y n'est PAS enfant direct de la collection");

    /* "Declarations globales" existe et ne contient QUE gCompteur (pas x/y). */
    ASSERT_SYM_EXISTS(col_ch, DECL_GLOB,
        "1762/nested : Declarations globales present");
    {
        JsonValue* dg = find_sym(col_ch, DECL_GLOB);
        JsonValue* dg_ch = sym_children(dg);
        ASSERT_SYM_CHILDREN_COUNT(dg, 1,
            "1762/nested : Declarations globales a 1 seul enfant (gCompteur)");
        ASSERT_SYM_EXISTS(dg_ch, "gCompteur",
            "1762/nested : gCompteur sous Declarations globales");
        ASSERT_SYM_NOT_EXISTS(dg_ch, "x",
            "1762/nested : x PAS sous Declarations globales");
        ASSERT_SYM_NOT_EXISTS(dg_ch, "y",
            "1762/nested : y PAS sous Declarations globales");
    }

    /* traiter reste un frere Method(6). */
    ASSERT_SYM_EXISTS(col_ch, "traiter",
        "1762/nested : traiter frere de la collection");
    ASSERT_SYM_KIND(find_sym(col_ch, "traiter"), LSP_KIND_METHOD,
        "1762/nested : traiter kind=Method(6)");

    json_free(result);
    json_free(params);
}

/* ================================================================
 *  -- Outline/Owl Explorer masquent les procedures sur un
 *  gros fichier CollectionDeProcedures / module top-level.
 *
 *  Cause : tableaux a capacite FIXE dans lsp_document_structure.c qui
 *  droppent silencieusement les symboles au-dela de la borne :
 *    - membres d'un conteneur (128) : les constantes remplissent les
 *      slots AVANT les procedures declarees apres -> procedures perdues.
 *    - items top-level (256) : la "limite a 256" du support.
 *
 *  ROUGES avant fix : les procedures declarees apres la borne sont
 *  absentes de documentSymbol. VERTS apres passage en croissance
 *  dynamique (aucun symbole droppe quelle que soit la taille).
 * ================================================================ */

/* Grande collection : ~150 constantes SUIVIES de 10 procedures nommees
 * de facon deterministe. Avant fix, les 128 premiers slots de membres
 * sont satures par les constantes, donc AUCUNE procedure n'est ajoutee. */
static void test_doc_symbol_collection_large_no_drop(void) {
    printf("\n[TEST] fix/1741 documentSymbol : grosse collection -> procedures non droppees\n");
    const int NCONST = 150;
    const int NPROC  = 10;
    size_t cap = 64 * 1024;
    char* src = malloc(cap);
    size_t off = 0;
    off += (size_t)snprintf(src + off, cap - off,
        "COL_Big est une CollectionDeProc\xc3\xa9""dures\n");
    for (int i = 0; i < NCONST; i++)
        off += (size_t)snprintf(src + off, cap - off,
            "\tKCONST_%03d = %d\n", i, i);
    for (int i = 0; i < NPROC; i++)
        off += (size_t)snprintf(src + off, cap - off,
            "\tPROCEDURE PUBLIQUE Proc_%03d()\n\t\tTrace(\"p\")\n\tFIN\n", i);
    off += (size_t)snprintf(src + off, cap - off, "FIN\n");

    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_EXISTS(result, "COL_Big", "1741/col-large : COL_Big present");
    JsonValue* col    = find_sym(result, "COL_Big");
    JsonValue* col_ch = sym_children(col);

    /* CHAQUE procedure doit etre presente comme enfant direct de COL_Big. */
    int missing = 0;
    for (int i = 0; i < NPROC; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "Proc_%03d", i);
        if (!find_sym(col_ch, nm)) missing++;
    }
    if (missing == 0) {
        g_pass++;
        printf("  [OK]   1741/col-large : les %d procedures sont toutes exposees\n", NPROC);
    } else {
        g_fail++;
        printf("  [FAIL] 1741/col-large : %d/%d procedures droppees (cap fixe membres)\n",
               missing, NPROC);
    }
    /* Ancrage sur la derniere procedure (la plus exposee au drop). */
    ASSERT_SYM_EXISTS(col_ch, "Proc_009",
        "1741/col-large : Proc_009 (derniere) presente");

    json_free(result);
    json_free(params);
    free(src);
}

/* Gros module : 300 procedures libres top-level (> 256). Avant fix, les
 * items au-dela de 256 sont droppes -> la 300e est absente. */
static void test_doc_symbol_module_large_no_drop(void) {
    printf("\n[TEST] fix/1741 documentSymbol : gros module top-level (>256 items)\n");
    const int NPROC = 300;
    size_t cap = 64 * 1024;
    char* src = malloc(cap);
    size_t off = 0;
    for (int i = 0; i < NPROC; i++)
        off += (size_t)snprintf(src + off, cap - off,
            "PROCEDURE PUBLIQUE Proc_%03d()\n\tTrace(\"x\")\nFIN\n\n", i);

    setup_doc(URI, src);
    JsonValue* params = make_text_document_params(URI);
    JsonValue* result = handle_document_symbol(params);

    ASSERT_SYM_ARRAY_COUNT(result, NPROC,
        "1741/module-large : les 300 procedures sont exposees");
    /* Ancrage sur la 300e (index 299) : la plus exposee au drop de cap 256. */
    ASSERT_SYM_EXISTS(result, "Proc_299",
        "1741/module-large : Proc_299 (300e) presente");
    ASSERT_SYM_EXISTS(result, "Proc_256",
        "1741/module-large : Proc_256 (au-dela de l'ancienne borne 256) presente");

    json_free(result);
    json_free(params);
    free(src);
}
