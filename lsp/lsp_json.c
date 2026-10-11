// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "lsp_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ================================================================
 * JSON PARSER -- minimal recursive descent
 * ================================================================ */

typedef struct {
    const char* src;
    int pos;
    int len;
} JsonParser;

static void jp_skip_ws(JsonParser* p) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos]))
        p->pos++;
}

static char jp_peek(JsonParser* p) {
    jp_skip_ws(p);
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static char jp_advance(JsonParser* p) {
    return p->pos < p->len ? p->src[p->pos++] : '\0';
}

static JsonValue* jp_parse_value(JsonParser* p);

static char* jp_parse_string_raw(JsonParser* p, int* outLen) {
    jp_skip_ws(p);
    if (jp_advance(p) != '"') return NULL;

    int start = p->pos;
    /* Simple: scan to closing quote, handle \" escapes */
    int cap = 256;
    char* buf = malloc(cap);
    int len = 0;

    while (p->pos < p->len && p->src[p->pos] != '"') {
        char c = p->src[p->pos++];
        if (c == '\\' && p->pos < p->len) {
            char esc = p->src[p->pos++];
            switch (esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                default:   c = esc; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = c;
    }
    (void)start;
    if (p->pos < p->len) p->pos++; /* skip closing " */
    buf[len] = '\0';
    if (outLen) *outLen = len;
    return buf;
}

static JsonValue* jp_parse_string(JsonParser* p) {
    int len;
    char* str = jp_parse_string_raw(p, &len);
    if (!str) return NULL;
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_STRING;
    v->as.stringVal.str = str;
    v->as.stringVal.len = len;
    return v;
}

static JsonValue* jp_parse_number(JsonParser* p) {
    jp_skip_ws(p);
    int64_t val = 0;
    bool neg = false;
    if (p->src[p->pos] == '-') { neg = true; p->pos++; }
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
        val = val * 10 + (p->src[p->pos++] - '0');
    }
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_INT;
    v->as.intVal = neg ? -val : val;
    return v;
}

static JsonValue* jp_parse_object(JsonParser* p) {
    jp_skip_ws(p);
    p->pos++; /* skip { */
    JsonValue* obj = json_new_object();

    if (jp_peek(p) == '}') { p->pos++; return obj; }

    for (;;) {
        int keyLen;
        char* key = jp_parse_string_raw(p, &keyLen);
        if (!key) break;
        jp_skip_ws(p);
        if (jp_advance(p) != ':') { free(key); break; }
        JsonValue* val = jp_parse_value(p);
        if (!val) { free(key); break; }
        json_object_set(obj, key, val);
        free(key);
        jp_skip_ws(p);
        if (jp_peek(p) == ',') { p->pos++; continue; }
        break;
    }
    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') p->pos++;
    return obj;
}

static JsonValue* jp_parse_array(JsonParser* p) {
    jp_skip_ws(p);
    p->pos++; /* skip [ */
    JsonValue* arr = json_new_array();

    if (jp_peek(p) == ']') { p->pos++; return arr; }

    for (;;) {
        JsonValue* val = jp_parse_value(p);
        if (!val) break;
        json_array_push(arr, val);
        jp_skip_ws(p);
        if (jp_peek(p) == ',') { p->pos++; continue; }
        break;
    }
    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') p->pos++;
    return arr;
}

static JsonValue* jp_parse_value(JsonParser* p) {
    char c = jp_peek(p);
    if (c == '"') return jp_parse_string(p);
    if (c == '{') return jp_parse_object(p);
    if (c == '[') return jp_parse_array(p);
    if (c == '-' || isdigit((unsigned char)c)) return jp_parse_number(p);
    if (strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return json_new_bool(true);
    }
    if (strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return json_new_bool(false);
    }
    if (strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return json_new_null();
    }
    return NULL;
}

JsonValue* json_parse(const char* source, int length) {
    JsonParser p = { .src = source, .pos = 0, .len = length };
    return jp_parse_value(&p);
}

/* ================================================================
 * ACCESS HELPERS
 * ================================================================ */

JsonValue* json_get(JsonValue* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        if (strcmp(obj->as.objectVal.pairs[i].key, key) == 0)
            return obj->as.objectVal.pairs[i].value;
    }
    return NULL;
}

const char* json_get_string(JsonValue* obj, const char* key) {
    JsonValue* v = json_get(obj, key);
    if (v && v->type == JSON_STRING) return v->as.stringVal.str;
    return NULL;
}

int64_t json_get_int(JsonValue* obj, const char* key, int64_t defaultVal) {
    JsonValue* v = json_get(obj, key);
    if (v && v->type == JSON_INT) return v->as.intVal;
    return defaultVal;
}

bool json_get_bool(JsonValue* obj, const char* key, bool defaultVal) {
    JsonValue* v = json_get(obj, key);
    if (v && v->type == JSON_BOOL) return v->as.boolVal;
    return defaultVal;
}

/* ================================================================
 * BUILDERS
 * ================================================================ */

JsonValue* json_new_object(void) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_OBJECT;
    v->as.objectVal.capacity = 16;
    v->as.objectVal.pairs = calloc(16, sizeof(JsonPair));
    return v;
}

JsonValue* json_new_array(void) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_ARRAY;
    v->as.arrayVal.capacity = 16;
    v->as.arrayVal.items = calloc(16, sizeof(JsonValue*));
    return v;
}

JsonValue* json_new_string(const char* str) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_STRING;
    v->as.stringVal.len = (int)strlen(str);
    v->as.stringVal.str = strdup(str);
    return v;
}

JsonValue* json_new_int(int64_t val) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_INT;
    v->as.intVal = val;
    return v;
}

JsonValue* json_new_bool(bool val) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_BOOL;
    v->as.boolVal = val;
    return v;
}

JsonValue* json_new_null(void) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    v->type = JSON_NULL;
    return v;
}

void json_object_set(JsonValue* obj, const char* key, JsonValue* value) {
    if (!obj || obj->type != JSON_OBJECT) return;
    /* Check for existing key */
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        if (strcmp(obj->as.objectVal.pairs[i].key, key) == 0) {
            json_free(obj->as.objectVal.pairs[i].value);
            obj->as.objectVal.pairs[i].value = value;
            return;
        }
    }
    /* New key */
    if (obj->as.objectVal.count >= obj->as.objectVal.capacity) {
        obj->as.objectVal.capacity *= 2;
        obj->as.objectVal.pairs = realloc(obj->as.objectVal.pairs,
            obj->as.objectVal.capacity * sizeof(JsonPair));
    }
    int idx = obj->as.objectVal.count++;
    obj->as.objectVal.pairs[idx].key = strdup(key);
    obj->as.objectVal.pairs[idx].value = value;
}

void json_array_push(JsonValue* arr, JsonValue* value) {
    if (!arr || arr->type != JSON_ARRAY) return;
    if (arr->as.arrayVal.count >= arr->as.arrayVal.capacity) {
        arr->as.arrayVal.capacity *= 2;
        arr->as.arrayVal.items = realloc(arr->as.arrayVal.items,
            arr->as.arrayVal.capacity * sizeof(JsonValue*));
    }
    arr->as.arrayVal.items[arr->as.arrayVal.count++] = value;
}

/* ================================================================
 * SERIALIZATION
 * ================================================================ */

typedef struct {
    char* buf;
    int len;
    int cap;
} JsonWriter;

static void jw_init(JsonWriter* w) {
    w->cap = 1024;
    w->buf = malloc(w->cap);
    w->len = 0;
}

static void jw_write(JsonWriter* w, const char* str, int slen) {
    while (w->len + slen + 1 >= w->cap) { w->cap *= 2; w->buf = realloc(w->buf, w->cap); }
    memcpy(w->buf + w->len, str, slen);
    w->len += slen;
}

static void jw_puts(JsonWriter* w, const char* str) {
    jw_write(w, str, (int)strlen(str));
}

static void jw_write_string(JsonWriter* w, const char* str) {
    jw_puts(w, "\"");
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '"':  jw_puts(w, "\\\""); break;
            case '\\': jw_puts(w, "\\\\"); break;
            case '\n': jw_puts(w, "\\n"); break;
            case '\r': jw_puts(w, "\\r"); break;
            case '\t': jw_puts(w, "\\t"); break;
            default:   jw_write(w, p, 1); break;
        }
    }
    jw_puts(w, "\"");
}

static void jw_serialize(JsonWriter* w, JsonValue* v) {
    if (!v) { jw_puts(w, "null"); return; }
    switch (v->type) {
        case JSON_NULL: jw_puts(w, "null"); break;
        case JSON_BOOL: jw_puts(w, v->as.boolVal ? "true" : "false"); break;
        case JSON_INT: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)v->as.intVal);
            jw_puts(w, buf);
            break;
        }
        case JSON_STRING:
            jw_write_string(w, v->as.stringVal.str);
            break;
        case JSON_ARRAY:
            jw_puts(w, "[");
            for (int i = 0; i < v->as.arrayVal.count; i++) {
                if (i > 0) jw_puts(w, ",");
                jw_serialize(w, v->as.arrayVal.items[i]);
            }
            jw_puts(w, "]");
            break;
        case JSON_OBJECT:
            jw_puts(w, "{");
            for (int i = 0; i < v->as.objectVal.count; i++) {
                if (i > 0) jw_puts(w, ",");
                jw_write_string(w, v->as.objectVal.pairs[i].key);
                jw_puts(w, ":");
                jw_serialize(w, v->as.objectVal.pairs[i].value);
            }
            jw_puts(w, "}");
            break;
    }
}

char* json_serialize(JsonValue* value, int* outLen) {
    JsonWriter w;
    jw_init(&w);
    jw_serialize(&w, value);
    w.buf[w.len] = '\0';
    if (outLen) *outLen = w.len;
    return w.buf;
}

/* ================================================================
 * CLEANUP
 * ================================================================ */

void json_free(JsonValue* value) {
    if (!value) return;
    switch (value->type) {
        case JSON_STRING:
            free(value->as.stringVal.str);
            break;
        case JSON_ARRAY:
            for (int i = 0; i < value->as.arrayVal.count; i++)
                json_free(value->as.arrayVal.items[i]);
            free(value->as.arrayVal.items);
            break;
        case JSON_OBJECT:
            for (int i = 0; i < value->as.objectVal.count; i++) {
                free(value->as.objectVal.pairs[i].key);
                json_free(value->as.objectVal.pairs[i].value);
            }
            free(value->as.objectVal.pairs);
            break;
        default: break;
    }
    free(value);
}
