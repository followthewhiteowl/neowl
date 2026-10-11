// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* OWL Markdown stdlib — wraps md4c (CommonMark 0.31 + GFM extensions).
 *
 * Exposes :
 *   - MarkdownVersTexte(s)            -> texte brut sans balises
 *   - MarkdownVersHTML(s [, css])     -> HTML, avec CSS optionnel injecté
 *
 * Plus prefix methods sur chaîne :
 *   s.MarkdownVersTexte() / s.VersTexte()
 *   s.MarkdownVersHTML([css]) / s.VersHTML([css])
 *
 * Sécurité : md4c-html applique par défaut un encodage HTML safe sur
 * le contenu utilisateur (entités `&` `<` `>` `"`). Le raw HTML inline
 * est passé tel quel par md4c (CommonMark le permet) — pour la version
 * "secure" on désactive le flag MD_FLAG_PERMISSIVEAUTOLINKS partout où
 * c'est utile et on n'autorise PAS le raw HTML par défaut. doc ne
 * mentionne pas de paramètre sécurité ; on aligne sur le défaut sain.
 *
 * Extensions GFM activées par défaut :
 *   MD_FLAG_TABLES         : tables GFM
 *   MD_FLAG_STRIKETHROUGH  : ~~barré~~
 *   MD_FLAG_TASKLISTS      : - [ ] / - [x]
 *   MD_FLAG_PERMISSIVEAUTOLINKS : URLs nues autodétectées
 *
 * Doc référence : doc/VISION.md §5bis (palier B = superset CommonMark+GFM
 * sous noms canon). */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "memory.h"
#include "vm.h"
#include <string.h>
#include <stdlib.h>

#include "md4c.h"
#include "md4c-html.h"

/* ================================================================
 * Buffer dynamique pour collecter la sortie via callback md4c.
 * ================================================================ */
typedef struct {
    char* data;
    size_t size;
    size_t cap;
} MdBuf;

static void mdbuf_init(MdBuf* b) {
    b->data = NULL; b->size = 0; b->cap = 0;
}
static void mdbuf_free(MdBuf* b) {
    if (b->data) free(b->data);
    b->data = NULL; b->size = b->cap = 0;
}
static void mdbuf_append(MdBuf* b, const char* s, size_t n) {
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

/* ================================================================
 * MarkdownVersHTML — md4c-html callback
 * ================================================================ */
static void md_html_cb(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    mdbuf_append((MdBuf*)userdata, text, size);
}

/* Common GFM extensions enabled in both renderers. */
static const unsigned MD_PARSER_FLAGS_COMMON =
    MD_FLAG_TABLES |
    MD_FLAG_STRIKETHROUGH |
    MD_FLAG_TASKLISTS |
    MD_FLAG_PERMISSIVEAUTOLINKS;

/* HTML renderer adds NOHTML for safety: raw HTML is escaped to entities,
 * preventing <script> injection. Text renderer keeps HTML detection on so
 * we can skip MD_TEXT_HTML callbacks (extension OWL: clean text output). */
#define MD_PARSER_FLAGS_HTML_SECURE  (MD_PARSER_FLAGS_COMMON | MD_FLAG_NOHTML)
#define MD_PARSER_FLAGS_TEXT         (MD_PARSER_FLAGS_COMMON)

/* ================================================================
 * MarkdownVersTexte — plain text renderer (custom callbacks)
 * Stratégie : on parse via md4c et on capture uniquement les MD_TEXT_*
 * blocks (le contenu textuel pur). Les délimiteurs de bloc/inline sont
 * implicitement ignorés. Sauts de ligne réinjectés à la fin de chaque
 * bloc pour conserver la lisibilité.
 * ================================================================ */
typedef struct {
    MdBuf  buf;
    int    listDepth;
    bool   atBlockStart;
    bool   inHtmlBlock;
    bool   inHtmlInline;  /* <span>, <br/>, etc. — extension "<span color>" */
} TextRenderState;

static void text_emit_block_separator(TextRenderState* st) {
    if (st->buf.size > 0 &&
        st->buf.data[st->buf.size - 1] != '\n') {
        mdbuf_append(&st->buf, "\n", 1);
    }
}

static int text_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    TextRenderState* st = (TextRenderState*)userdata;
    (void)detail;
    if (type == MD_BLOCK_HTML) st->inHtmlBlock = true;
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) st->listDepth++;
    st->atBlockStart = true;
    return 0;
}

static int text_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    TextRenderState* st = (TextRenderState*)userdata;
    (void)detail;
    if (type == MD_BLOCK_HTML) st->inHtmlBlock = false;
    if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) st->listDepth--;
    /* Séparation entre blocs : un saut de ligne propre. */
    if (type == MD_BLOCK_P || type == MD_BLOCK_H ||
        type == MD_BLOCK_LI || type == MD_BLOCK_QUOTE ||
        type == MD_BLOCK_CODE || type == MD_BLOCK_HR) {
        text_emit_block_separator(st);
    }
    return 0;
}

static int text_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
    (void)type; (void)detail; (void)userdata;
    /* Liens / images / strong / em / code : on laisse passer le texte
     * via les callbacks text() qui suivent — rien à émettre côté délimiteur. */
    return 0;
}

static int text_leave_span(MD_SPANTYPE type, void* detail, void* userdata) {
    (void)type; (void)detail; (void)userdata;
    return 0;
}

static int text_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    TextRenderState* st = (TextRenderState*)userdata;
    /* Skip raw HTML content entirely (extension OWL : MarkdownVersTexte
     * sortie propre, pas de balises résiduelles). Aussi : skip tout texte
     * dedans un BLOCK HTML (ex: <script>alert(1)</script>). */
    if (type == MD_TEXT_HTML) return 0;
    if (st->inHtmlBlock) return 0;
    /* Skip URLs from autolinks (sinon on aurait à la fois le texte + l'URL) */
    /* MD_TEXT_NORMAL / MD_TEXT_CODE / MD_TEXT_BR / MD_TEXT_SOFTBR / MD_TEXT_ENTITY / MD_TEXT_NULLCHAR / MD_TEXT_LATEXMATH */
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        mdbuf_append(&st->buf, "\n", 1);
        return 0;
    }
    if (type == MD_TEXT_NULLCHAR) {
        /* Replacement char Unicode (md4c convention) */
        mdbuf_append(&st->buf, "\xEF\xBF\xBD", 3);
        return 0;
    }
    mdbuf_append(&st->buf, text, size);
    return 0;
}

static ObjString* owl_md_to_text(const char* src, int srcLen) {
    TextRenderState st;
    mdbuf_init(&st.buf);
    st.listDepth = 0;
    st.atBlockStart = true;
    st.inHtmlBlock = false;
    st.inHtmlInline = false;

    MD_PARSER parser;
    memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags       = MD_PARSER_FLAGS_TEXT;
    parser.enter_block = text_enter_block;
    parser.leave_block = text_leave_block;
    parser.enter_span  = text_enter_span;
    parser.leave_span  = text_leave_span;
    parser.text        = text_text;

    md_parse(src, (MD_SIZE)srcLen, &parser, &st);

    /* Trim trailing newlines for a clean string */
    while (st.buf.size > 0 && st.buf.data[st.buf.size - 1] == '\n') {
        st.buf.size--;
        st.buf.data[st.buf.size] = 0;
    }

    ObjString* out = obj_copy_string(st.buf.data ? st.buf.data : "",
                                     (int)st.buf.size);
    mdbuf_free(&st.buf);
    return out;
}

static ObjString* owl_md_to_html(const char* src, int srcLen,
                                 const char* css, int cssLen) {
    MdBuf buf;
    mdbuf_init(&buf);

    /* Optional CSS prelude — wrap in <style> block at the top */
    if (css && cssLen > 0) {
        mdbuf_append(&buf, "<style>", 7);
        mdbuf_append(&buf, css, (size_t)cssLen);
        mdbuf_append(&buf, "</style>\n", 9);
    }

    /* MD_HTML_FLAG_SKIP_UTF8_BOM : ignore BOM if present
     * MD_HTML_FLAG_DEBUG : NO -- debug spam in stderr
     * No raw HTML escape needed: MD_FLAG_NOHTML strips it at parse time. */
    md_html(src, (MD_SIZE)srcLen, md_html_cb, &buf,
            MD_PARSER_FLAGS_HTML_SECURE, MD_HTML_FLAG_SKIP_UTF8_BOM);

    ObjString* out = obj_copy_string(buf.data ? buf.data : "",
                                     (int)buf.size);
    mdbuf_free(&buf);
    return out;
}

/* ================================================================
 * Native bindings
 * ================================================================ */
static Value fn_markdown_vers_texte(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* s = AS_STRING(args[0]);
    return OBJ_VAL(owl_md_to_text(s->chars, s->length));
}

static Value fn_markdown_vers_html(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* s = AS_STRING(args[0]);
    const char* css = NULL;
    int cssLen = 0;
    if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* c = AS_STRING(args[1]);
        css = c->chars;
        cssLen = c->length;
    }
    return OBJ_VAL(owl_md_to_html(s->chars, s->length, css, cssLen));
}

/* ================================================================
 * Registry
 * ================================================================ */
static const OwlParam p_md_s[] = {
    {"texte", "chaîne", "texte au format Markdown"}, {0}
};
static const OwlParam p_md_html1[] = {
    {"texte", "chaîne", "texte au format Markdown"},
    {0}
};
static const OwlParam p_md_html2[] = {
    {"texte", "chaîne", "texte au format Markdown"},
    {"styleCSS", "chaîne", "feuille CSS optionnelle injectée en <style>"},
    {0}
};
static const OwlSignature SIG_MD_TEXTE[] = {
    {1, p_md_s, "chaîne", "texte brut sans balises Markdown"}, {0}
};
static const OwlSignature SIG_MD_HTML[] = {
    {1, p_md_html1, "chaîne", "rendu HTML"},
    {2, p_md_html2, "chaîne", "rendu HTML avec feuille CSS embarquée"},
    {0}
};

static const OwlMethodBinding MB_MD_TEXTE[] = {
    {"chaine", "MarkdownVersTexte", 1},
    {"chaine", "MarkdownToText",    1},
    {"chaine", "VersTexte",         1},  /* short OWL form */
    {"chaine", "ToText",            1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_MD_HTML[] = {
    {"chaine", "MarkdownVersHTML", -1},
    {"chaine", "MarkdownToHTML",   -1},
    {"chaine", "VersHTML",         -1},
    {"chaine", "ToHTML",           -1},
    {NULL, NULL, 0}
};

static const OwlNativeDef MARKDOWN_DEFS[] = {
    { .canonical = "MarkdownVersTexte",
      .nameEN    = "MarkdownToText",
      .category  = "Chaines",
      .descFR    = "Convertit un texte Markdown en texte brut (balises supprimées). Implémentation md4c CommonMark + GFM (tables, ~~barré~~, task lists, autolinks). Le raw HTML est strippé.",
      .descEN    = "Strip Markdown formatting from text (md4c CommonMark + GFM). Raw HTML stripped.",
      .signatures = SIG_MD_TEXTE,
      .impl       = fn_markdown_vers_texte,
      .methodBindings = MB_MD_TEXTE },
    { .canonical = "MarkdownVersHTML",
      .nameEN    = "MarkdownToHTML",
      .category  = "Chaines",
      .descFR    = "Convertit un texte Markdown en HTML (md4c CommonMark + GFM). Le raw HTML est filtré pour la sécurité (pas de <script> injecté). Le second paramètre optionnel injecte une feuille CSS en <style>.",
      .descEN    = "Convert Markdown to HTML (md4c CommonMark + GFM). Raw HTML stripped (no script injection). Optional CSS injected as <style>.",
      .signatures = SIG_MD_HTML,
      .impl       = fn_markdown_vers_html,
      .methodBindings = MB_MD_HTML },
};

void stdlib_markdown_init(void) {
    owl_registry_register_module(MARKDOWN_DEFS,
        (int)(sizeof(MARKDOWN_DEFS) / sizeof(MARKDOWN_DEFS[0])));
}
