// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* OWL Texte Riche — Markdown étendu OWL → HTML pour widgets Qt6.
 *
 * Format unique exposé au user, transpilé en interne vers HTML (widgets
 * Qt6) ou ODF (éditique V2).
 *
 * Pipeline V1 :
 *   source OWL Markdown étendu
 *           ↓ pre-process (extensions OWL → HTML inline)
 *   Markdown standard avec balises HTML inline
 *           ↓ md4c (CommonMark + GFM)
 *   HTML final pour QTextDocument::setHtml
 *
 * Extensions OWL reconnues :
 *   [c:<couleur>]…[/c]    → couleur texte (constante canon, hex #RRGGBB, RVB(...))
 *   [cf:<couleur>]…[/cf]  → couleur de fond
 *   [s:<n>]…[/s]          → taille en pt
 *   [p:<nom>]…[/p]        → nom de police
 *   [esp:<n>]…[/esp]      → letter-spacing en px
 *   [j:<mode>]…[/j]       → alignement (gauche/droite/centre/justifie + alias EN)
 *
 * Markdown standard via md4c : **gras**, *italique*, __souligné__, ~~barré~~,
 * `code`, listes -/1., liens, tableaux, autolinks.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "memory.h"
#include "vm.h"
#include "object.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "md4c.h"
#include "md4c-html.h"

/* ================================================================
 * Buffer dynamique (même pattern que stdlib_markdown.c).
 * ================================================================ */
typedef struct {
    char* data;
    size_t size;
    size_t cap;
} TrBuf;

static void tr_init(TrBuf* b) { b->data = NULL; b->size = 0; b->cap = 0; }
static void tr_free(TrBuf* b) {
    if (b->data) free(b->data);
    b->data = NULL; b->size = b->cap = 0;
}
static void tr_append(TrBuf* b, const char* s, size_t n) {
    if (b->size + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 256;
        while (newcap < b->size + n + 1) newcap *= 2;
        char* nd = (char*)realloc(b->data, newcap);
        if (!nd) return;
        b->data = nd;
        b->cap = newcap;
    }
    memcpy(b->data + b->size, s, n);
    b->size += n;
    b->data[b->size] = 0;
}
static void tr_append_cstr(TrBuf* b, const char* s) { tr_append(b, s, strlen(s)); }

/* ================================================================
 * Résolution de couleur
 * 3 syntaxes acceptées :
 *   - hex direct : "#RGB" ou "#RRGGBB"
 *   - constante canon : recherche dans vm.globals (Rouge, BleuFoncé, etc.)
 *   - RVB(r,g,b) : not parsed in V1 — l'utilisateur passe par hex/constante
 *                  pour rester simple. Une expression dynamique sortira en V1.5.
 *
 * Output toujours en #RRGGBB (CSS-friendly).
 * Retourne 1 si résolu, 0 sinon (laisse l'output vide pour fallback).
 * ================================================================ */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int resolve_color(const char* val, int valLen, char outHex[8]) {
    /* Skip whitespace */
    while (valLen > 0 && (*val == ' ' || *val == '\t')) { val++; valLen--; }
    while (valLen > 0 && (val[valLen-1] == ' ' || val[valLen-1] == '\t')) valLen--;
    if (valLen == 0) return 0;

    /* Hex direct */
    if (val[0] == '#') {
        if (valLen == 4) {
            /* #RGB → #RRGGBB */
            int r = hex_digit(val[1]);
            int g = hex_digit(val[2]);
            int b = hex_digit(val[3]);
            if (r < 0 || g < 0 || b < 0) return 0;
            snprintf(outHex, 8, "#%X%X%X%X%X%X", r, r, g, g, b, b);
            return 1;
        }
        if (valLen == 7) {
            for (int i = 1; i < 7; i++) if (hex_digit(val[i]) < 0) return 0;
            memcpy(outHex, val, 7);
            outHex[7] = 0;
            return 1;
        }
        return 0;
    }

    /* Constante canon : lookup vm.globals via key string. Stockée en BGR pack
     * (canon : R + G*256 + B*65536). On convertit en #RRGGBB. */
    if (valLen < 64) {
        char key[64];
        memcpy(key, val, valLen);
        key[valLen] = 0;
        ObjString* k = obj_copy_string(key, valLen);
        Value v;
        if (table_get(&vm.globals, k, &v) && IS_INTEGER(v)) {
            int rgb = (int)AS_INTEGER(v);
            int r = rgb & 0xFF;
            int g = (rgb >> 8) & 0xFF;
            int b = (rgb >> 16) & 0xFF;
            snprintf(outHex, 8, "#%02X%02X%02X", r, g, b);
            return 1;
        }
    }

    return 0;
}

/* ================================================================
 * Pre-process : substitue les extensions OWL [tag:val]…[/tag] par
 * leur équivalent HTML inline.
 *
 * Algorithme simple (single-pass, O(n)) :
 *   - Scan le source à la recherche de '[' suivi d'un tag connu suivi de ':'
 *   - Match la fermeture [/tag] correspondante (matching naïf, pas
 *     d'imbrication identique nested — pour V1 on l'accepte)
 *   - Émet <span style="..."> ou <p style="..."> selon le tag
 *
 * Caractères échappés : \\[ et \\] passent en literal [/]. Reste du backslash
 * passe à md4c qui gère les escapes Markdown.
 *
 * Limitation V1 : si tag mal formé, on émet le source literal (md4c le verra
 * comme du texte normal). Pas d'erreur compile-time pour V1.
 * ================================================================ */

typedef struct {
    const char* tag;        /* "c", "cf", "s", "p", "esp", "j" */
    const char* htmlTag;    /* "span" ou "p" pour block-level */
    const char* cssAttr;    /* "color", "background-color", "font-size", ... */
    const char* unit;       /* "" ou "pt" ou "px" */
    int isAlignment;        /* 1 = remap value via map_align ; 0 = passthrough */
    int isColor;             /* 1 = passe par resolve_color */
} TrTag;

/* Mapping alignement canon → CSS */
static const char* map_align(const char* val, int valLen) {
    if (valLen == 6 && (memcmp(val, "gauche", 6) == 0 || memcmp(val, "Gauche", 6) == 0)) return "left";
    if (valLen == 4 && (memcmp(val, "left", 4) == 0   || memcmp(val, "Left", 4) == 0))   return "left";
    if (valLen == 6 && (memcmp(val, "droite", 6) == 0 || memcmp(val, "Droite", 6) == 0)) return "right";
    if (valLen == 5 && (memcmp(val, "right", 5) == 0  || memcmp(val, "Right", 5) == 0))  return "right";
    if (valLen == 6 && (memcmp(val, "centre", 6) == 0 || memcmp(val, "Centre", 6) == 0)) return "center";
    if (valLen == 6 && (memcmp(val, "center", 6) == 0 || memcmp(val, "Center", 6) == 0)) return "center";
    if (valLen >= 8 && memcmp(val, "justifi", 7) == 0) return "justify";
    if (valLen >= 9 && memcmp(val, "justified", 9) == 0) return "justify";
    return NULL;
}

static const TrTag g_tr_tags[] = {
    {"c",   "span", "color",            "",   0, 1},
    {"cf",  "span", "background-color", "",   0, 1},
    {"s",   "span", "font-size",        "pt", 0, 0},
    {"p",   "span", "font-family",      "",   0, 0},
    {"esp", "span", "letter-spacing",   "px", 0, 0},
    {"j",   "p",    "text-align",       "",   1, 0},
    {NULL, NULL, NULL, NULL, 0, 0}
};

static const TrTag* find_tag(const char* tagStart, int tagLen) {
    for (int i = 0; g_tr_tags[i].tag; i++) {
        if ((int)strlen(g_tr_tags[i].tag) == tagLen &&
            memcmp(g_tr_tags[i].tag, tagStart, tagLen) == 0) {
            return &g_tr_tags[i];
        }
    }
    return NULL;
}

/* Find matching [/tag] starting from `from`. Retourne offset après ']' ou -1. */
static int find_close(const char* src, int srcLen, int from, const char* tag, int tagLen) {
    /* Recherche linéaire de la séquence "[/<tag>]" */
    int needLen = 3 + tagLen;  /* [ / tag ] */
    char need[16];
    if (tagLen + 3 > 15) return -1;  /* tag trop long */
    need[0] = '[';
    need[1] = '/';
    memcpy(need + 2, tag, tagLen);
    need[2 + tagLen] = ']';
    need[3 + tagLen] = 0;
    int searchEnd = srcLen - needLen;
    for (int i = from; i <= searchEnd; i++) {
        if (memcmp(src + i, need, (size_t)needLen) == 0) {
            return i;  /* offset du '[' de la fermeture */
        }
    }
    return -1;
}

/* Pre-process full pass — substitue les extensions par leur HTML, copie le
 * reste tel quel. Recursion-free : on fait une passe et chaque tag fermé
 * peut contenir d'autres tags (qui seront traités lors de la passe suivante).
 * V1 : single-pass — pas d'imbrication même tag (rare en pratique). */
static void preprocess_extensions(const char* src, int srcLen, TrBuf* out) {
    int i = 0;
    while (i < srcLen) {
        /* Échappement \\[ et \\] */
        if (src[i] == '\\' && i + 1 < srcLen && (src[i+1] == '[' || src[i+1] == ']')) {
            tr_append(out, src + i + 1, 1);
            i += 2;
            continue;
        }
        if (src[i] == '[' && i + 1 < srcLen) {
            /* Cherche un ':' avant ']' pour tagName:value */
            int colon = -1, close = -1;
            for (int j = i + 1; j < srcLen; j++) {
                if (src[j] == ':' && colon < 0) { colon = j; }
                if (src[j] == ']') { close = j; break; }
                if (src[j] == '\n') break;  /* abandon — multi-line tag invalide V1 */
            }
            if (colon > i && close > colon) {
                int tagLen = colon - i - 1;
                const TrTag* td = find_tag(src + i + 1, tagLen);
                if (td) {
                    /* Match find close [/tag] */
                    int valStart = colon + 1;
                    int valLen = close - valStart;
                    const char* val = src + valStart;
                    /* Skip ws around val */
                    while (valLen > 0 && (*val == ' ' || *val == '\t')) { val++; valLen--; }
                    while (valLen > 0 && (val[valLen-1] == ' ' || val[valLen-1] == '\t')) valLen--;

                    int closeIdx = find_close(src, srcLen, close + 1, td->tag, tagLen);
                    if (closeIdx >= 0) {
                        /* Émet <htmlTag style="cssAttr: <val><unit>;">…</htmlTag> */
                        char styleVal[128] = {0};
                        int ok = 1;
                        if (td->isColor) {
                            char hex[8] = {0};
                            if (resolve_color(val, valLen, hex)) {
                                snprintf(styleVal, sizeof(styleVal), "%s", hex);
                            } else {
                                ok = 0;
                            }
                        } else if (td->isAlignment) {
                            const char* m = map_align(val, valLen);
                            if (m) snprintf(styleVal, sizeof(styleVal), "%s", m);
                            else   ok = 0;
                        } else {
                            /* Numeric ou string passthrough */
                            if (valLen < (int)sizeof(styleVal) - 8) {
                                memcpy(styleVal, val, valLen);
                                styleVal[valLen] = 0;
                                if (td->unit && *td->unit) {
                                    strcat(styleVal, td->unit);
                                }
                            } else {
                                ok = 0;
                            }
                        }
                        if (ok) {
                            tr_append_cstr(out, "<");
                            tr_append_cstr(out, td->htmlTag);
                            tr_append_cstr(out, " style=\"");
                            tr_append_cstr(out, td->cssAttr);
                            tr_append_cstr(out, ":");
                            tr_append_cstr(out, styleVal);
                            tr_append_cstr(out, "\">");
                            /* Recursion : pre-process aussi le contenu */
                            preprocess_extensions(src + close + 1,
                                                   closeIdx - (close + 1), out);
                            tr_append_cstr(out, "</");
                            tr_append_cstr(out, td->htmlTag);
                            tr_append_cstr(out, ">");
                            i = closeIdx + tagLen + 3;  /* skip [/tag] */
                            continue;
                        }
                    }
                }
            }
        }
        tr_append(out, src + i, 1);
        i++;
    }
}

/* ================================================================
 * md4c callback : append au buffer.
 * ================================================================ */
static void tr_md4c_cb(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    tr_append((TrBuf*)userdata, text, size);
}

/* Flags md4c : on accepte le HTML inline (généré par notre pre-process)
 * + tables + strikethrough + tasklists + autolinks. Pas de NOHTML car
 * les <span> qu'on émet doivent passer. */
#define TR_MD_FLAGS \
    (MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | \
     MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_UNDERLINE)

/* ================================================================
 * API publique : transpile une source Markdown OWL en HTML.
 * Appelé par stdlib_ui.c apply runtime quand FormatAffichage = texteRiche.
 * ================================================================ */
ObjString* owl_text_riche_to_html(const char* src, int srcLen) {
    if (srcLen <= 0) return obj_copy_string("", 0);

    /* Étape 1 : pre-process extensions OWL → HTML inline */
    TrBuf pre;
    tr_init(&pre);
    preprocess_extensions(src, srcLen, &pre);

    /* Étape 2 : md4c standard sur le résultat */
    TrBuf html;
    tr_init(&html);
    md_html(pre.data ? pre.data : "", (MD_SIZE)pre.size,
            tr_md4c_cb, &html, TR_MD_FLAGS, MD_HTML_FLAG_SKIP_UTF8_BOM);

    ObjString* out = obj_copy_string(html.data ? html.data : "", (int)html.size);
    tr_free(&pre);
    tr_free(&html);
    return out;
}

/* ================================================================
 * Native exposée au canon : TexteRicheVersHTML(s).
 * Utile pour debug et pour cas user-code avancé.
 * ================================================================ */
static Value fn_texte_riche_vers_html(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* s = AS_STRING(args[0]);
    return OBJ_VAL(owl_text_riche_to_html(s->chars, s->length));
}

static const OwlParam p_tr[] = {
    {"texte", "cha\xc3\xaene", "texte au format Texte Riche OWL"}, {0}
};
static const OwlSignature SIG_TR_HTML[] = {
    {1, p_tr, "cha\xc3\xaene", "rendu HTML pour le moteur d'affichage"}, {0}
};

static const OwlMethodBinding MB_TR[] = {
    {"chaine", "TexteRicheVersHTML", 1},
    {"chaine", "RichTextToHTML",     1},
    {NULL, NULL, 0}
};

static const OwlNativeDef TEXT_RICHE_DEFS[] = {
    { .canonical = "TexteRicheVersHTML",
      .nameEN    = "RichTextToHTML",
      .category  = "Chaines",
      .descFR    = "Transpile une chaîne au format Texte Riche OWL (Markdown étendu) en HTML pour le moteur d'affichage interne. Inline tags supportés : **gras**, *italique*, __souligné__, ~~barré~~, `code`, [c:couleur]…[/c], [cf:couleur]…[/cf], [s:n]…[/s], [p:police]…[/p], [esp:n]…[/esp], [j:mode]…[/j].",
      .descEN    = "Transpile an OWL Rich Text string (extended Markdown) to HTML for the internal display engine. Supported tags: **bold**, *italic*, __underline__, ~~strike~~, `code`, [c:color]…[/c], [cf:color]…[/cf], [s:n]…[/s], [p:font]…[/p], [esp:n]…[/esp], [j:mode]…[/j].",
      .signatures = SIG_TR_HTML,
      .impl       = fn_texte_riche_vers_html,
      .methodBindings = MB_TR },
};

void stdlib_text_riche_init(void) {
    owl_registry_register_module(TEXT_RICHE_DEFS,
        (int)(sizeof(TEXT_RICHE_DEFS) / sizeof(TEXT_RICHE_DEFS[0])));
}
