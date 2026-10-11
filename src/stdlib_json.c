// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "vm.h"
#include "compiler.h"
#include "scanner.h"
#include <ctype.h>
#include <errno.h>   /* strtoll ERANGE -> montée numérique sur entier > int64 */

/* ================================================================
 * JSON PARSER -- recursive descent, allocates ObjJson via GC
 * ================================================================ */

/* Options bitmask pour JSONValide (combinable avec '+'). 0 = strict RFC. */
#define OWL_JSON_STRICT              0
#define OWL_JSON_AVEC_COMMENTAIRES   1   /* tolere les commentaires C-style */

typedef struct {
    const char* src;
    int pos;
    int len;
    bool hasError;             /* mis a true sur la 1ere erreur rencontree */
    int  errPos;               /* position de l'erreur (1-based) */
    char errMsg[160];          /* message d'erreur (plain text, sans accents) */
    bool allowBlockComments;   /* mode JSONC : tolere les commentaires bloc */
} JParser;

static void jp_set_error(JParser* p, const char* msg) {
    if (p->hasError) return;  /* garder la 1ere erreur */
    p->hasError = true;
    p->errPos = p->pos + 1;  /* 1-based pour message utilisateur */
    int n = (int)strlen(msg);
    if (n >= (int)sizeof(p->errMsg)) n = (int)sizeof(p->errMsg) - 1;
    memcpy(p->errMsg, msg, (size_t)n);
    p->errMsg[n] = '\0';
}

static void jp_ws(JParser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (isspace((unsigned char)c)) {
            p->pos++;
            continue;
        }
        /* Mode commentaires : skip les blocs C-style */
        if (p->allowBlockComments && c == '/' && p->pos + 1 < p->len &&
            p->src[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len &&
                   !(p->src[p->pos] == '*' && p->src[p->pos + 1] == '/')) {
                p->pos++;
            }
            if (p->pos + 1 < p->len) {
                p->pos += 2;  /* consume '*' '/' */
            } else {
                jp_set_error(p, "comment: '*/' attendu pour fermer le commentaire");
                return;
            }
            continue;
        }
        break;
    }
}

static ObjJson* jp_value(JParser* p);

static ObjString* jp_string(JParser* p) {
    p->pos++; /* skip " */
    int start = p->pos;
    /* Buffer DYNAMIQUE : une valeur chaine JSON peut depasser toute borne
     * fixe (overlay de sources completes VSCode > 8 Ko, ). On croit
     * par realloc pour ne JAMAIS tronquer silencieusement (meme classe de bug
     * que l'ancien buffer fixe 8192 o). buf est un tampon C brut, pas un objet
     * GC : aucun rooting a prevoir malgre le obj_copy_string final. */
    int cap = 256;
    int j = 0;
    char* buf = (char*)malloc((size_t)cap);
    if (buf == NULL) {
        jp_set_error(p, "string: memoire insuffisante");
        return obj_copy_string("", 0);
    }
    while (p->pos < p->len && p->src[p->pos] != '"') {
        /* Garantit au moins 4 octets libres avant chaque ajout : le plus long
         * ajout ponctuel = 3 octets (un \uXXXX transcode en UTF-8). */
        if (j + 4 >= cap) {
            cap *= 2;
            char* grown = (char*)realloc(buf, (size_t)cap);
            if (grown == NULL) {
                free(buf);
                jp_set_error(p, "string: memoire insuffisante");
                return obj_copy_string("", 0);
            }
            buf = grown;
        }
        if (p->src[p->pos] == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            switch (p->src[p->pos]) {
                case '"':  buf[j++] = '"'; break;
                case '\\': buf[j++] = '\\'; break;
                case '/':  buf[j++] = '/'; break;
                case 'n':  buf[j++] = '\n'; break;
                case 'r':  buf[j++] = '\r'; break;
                case 't':  buf[j++] = '\t'; break;
                case 'b':  buf[j++] = '\b'; break;
                case 'f':  buf[j++] = '\f'; break;
                case 'u': {
                    /* \uXXXX -> UTF-8 */
                    if (p->pos + 4 < p->len) {
                        char hex[5] = { p->src[p->pos+1], p->src[p->pos+2],
                                        p->src[p->pos+3], p->src[p->pos+4], '\0' };
                        unsigned int cp = (unsigned int)strtol(hex, NULL, 16);
                        p->pos += 4;
                        if (cp < 0x80) {
                            buf[j++] = (char)cp;
                        } else if (cp < 0x800) {
                            buf[j++] = (char)(0xC0 | (cp >> 6));
                            buf[j++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            buf[j++] = (char)(0xE0 | (cp >> 12));
                            buf[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            buf[j++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: buf[j++] = p->src[p->pos]; break;
            }
        } else {
            buf[j++] = p->src[p->pos];
        }
        p->pos++;
    }
    (void)start;
    ObjString* result;
    /* Strict : la string DOIT se terminer par un '"' */
    if (p->pos >= p->len) {
        jp_set_error(p, "string: '\"' attendu pour fermer la chaine");
        result = obj_copy_string(buf, j);
    } else {
        p->pos++; /* skip closing " */
        result = obj_copy_string(buf, j);
    }
    free(buf);
    return result;
}

static ObjJson* jp_object(JParser* p) {
    p->pos++; /* skip { */
    ObjJson* obj = obj_new_json(JSON_T_OBJECT);
    /* Root `obj` on the VM stack for the whole parsing. Each recursive
     * jp_value / jp_string allocates and may trigger GC, which would
     * collect `obj` otherwise (it's only referenced from this C frame). */
    vm_push(OBJ_VAL(obj));
    jp_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') { p->pos++; vm_pop(); return obj; }

    for (;;) {
        jp_ws(p);
        /* Strict : doit commencer par une key string. Sinon erreur. */
        if (p->pos >= p->len || p->src[p->pos] != '"') {
            jp_set_error(p, "object: clef attendue (chaine entre guillemets)");
            vm_pop();
            return obj;
        }
        /*  : push key avant jp_value(p) recursif qui peut allouer
         * → GC → key (fresh ObjString) collected → UAF dans
         * obj_json_object_set. */
        ObjString* key = jp_string(p);
        vm_push(OBJ_VAL(key));
        jp_ws(p);
        /* Strict : ':' obligatoire apres la key. */
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            jp_set_error(p, "object: ':' attendu apres la clef");
            vm_pop(); /* key */
            vm_pop(); /* obj */
            return obj;
        }
        p->pos++; /* consume ':' */
        ObjJson* val = jp_value(p);
        if (val) obj_json_object_set(obj, key, val);
        vm_pop(); /* key */
        if (p->hasError) { vm_pop(); return obj; }
        jp_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            jp_ws(p);
            /* Strict RFC : trailing comma `{"a":1,}` interdite. Si on
             * voit '}' juste apres la virgule, c'est une erreur. */
            if (p->pos < p->len && p->src[p->pos] == '}') {
                jp_set_error(p, "object: trailing comma interdite avant '}'");
                vm_pop();
                return obj;
            }
            continue;
        }
        break;
    }
    jp_ws(p);
    /* Strict : '}' obligatoire pour fermer l'object. */
    if (p->pos >= p->len || p->src[p->pos] != '}') {
        jp_set_error(p, "object: '}' attendu pour fermer l'objet");
        vm_pop();
        return obj;
    }
    p->pos++; /* consume '}' */
    vm_pop();
    return obj;
}

static ObjJson* jp_array(JParser* p) {
    p->pos++; /* skip [ */
    ObjJson* arr = obj_new_json(JSON_T_ARRAY);
    /* Same rationale as jp_object: root arr for the whole parsing. */
    vm_push(OBJ_VAL(arr));
    jp_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') { p->pos++; vm_pop(); return arr; }

    for (;;) {
        ObjJson* val = jp_value(p);
        if (val) obj_json_array_push(arr, val);
        if (p->hasError) { vm_pop(); return arr; }
        jp_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            jp_ws(p);
            /* Strict RFC : trailing comma `[1,2,]` interdite. */
            if (p->pos < p->len && p->src[p->pos] == ']') {
                jp_set_error(p, "array: trailing comma interdite avant ']'");
                vm_pop();
                return arr;
            }
            continue;
        }
        break;
    }
    jp_ws(p);
    /* Strict : ']' obligatoire pour fermer l'array. */
    if (p->pos >= p->len || p->src[p->pos] != ']') {
        jp_set_error(p, "array: ']' attendu pour fermer le tableau");
        vm_pop();
        return arr;
    }
    p->pos++; /* consume ']' */
    vm_pop();
    return arr;
}

/* Parse un nombre JSON (RFC 8259 strict).
 *   number = [ minus ] int [ frac ] [ exp ]
 *   int    = "0" | digit1-9 *digit       -- pas de leading zero (sauf 0 seul)
 *   frac   = "." 1*digit                 -- au moins 1 digit apres le dot
 *   exp    = ("e"|"E") [sign] 1*digit    -- au moins 1 digit dans l'exposant
 */
static ObjJson* jp_number(JParser* p) {
    int start = p->pos;
    bool isReal = false;
    if (p->src[p->pos] == '-') p->pos++;
    /* int part : '0' seul OU digit1-9 puis 0+ digits (refus leading zero) */
    if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
        jp_set_error(p, "number: chiffre attendu apres le signe '-'");
        return obj_new_json_int(0);
    }
    if (p->src[p->pos] == '0') {
        p->pos++;
        /* Apres '0', si on a un autre digit => leading zero invalide */
        if (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
            jp_set_error(p, "number: leading zero interdit (ex: '01' invalide, '0' ou '0.5' OK)");
            return obj_new_json_int(0);
        }
    } else {
        /* digit1-9 puis 0+ digits */
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    /* frac : "." obligatoirement suivi d'au moins 1 digit */
    if (p->pos < p->len && p->src[p->pos] == '.') {
        isReal = true; p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            jp_set_error(p, "number: au moins un chiffre attendu apres le point decimal");
            return obj_new_json_int(0);
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    /* exp : "e"|"E" [sign] obligatoirement suivi d'au moins 1 digit */
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        isReal = true; p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            jp_set_error(p, "number: au moins un chiffre attendu dans l'exposant");
            return obj_new_json_int(0);
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    char buf[64];
    int len = p->pos - start;
    if (len >= 63) len = 62;
    memcpy(buf, p->src + start, len);
    buf[len] = '\0';
    if (isReal) {
        /* Decimal sans exposant : conserver l'exact (numérique bignum) si le
         * double perd la precision. Critere : parser le texte en numérique
         * exact, le reformatter, comparer au shortest-roundtrip du double. Si
         * identiques, le double suffit (interop standard preserve) ; sinon on
         * garde l'exact. Les nombres a exposant restent reels (forme interop
         * typique). */
        bool hasExp = (strchr(buf, 'e') != NULL || strchr(buf, 'E') != NULL);
        if (!hasExp) {
            Value nv = numeric_from_string(buf);
            char exactStr[128];
            char dblStr[64];
            numeric_to_str(nv, exactStr, sizeof(exactStr));
            owl_real_to_str(strtod(buf, NULL), dblStr, sizeof(dblStr));
            if (strcmp(exactStr, dblStr) != 0)
                return obj_new_json_numeric(nv);
        }
        return obj_new_json_real(strtod(buf, NULL));
    }
    /* Entier : montée auto vers numérique si la magnitude dépasse int64 (plus
     * de saturation/troncature silencieuse via strtoll — cohérent ). */
    errno = 0;
    long long iv = strtoll(buf, NULL, 10);
    if (errno == ERANGE)
        return obj_new_json_numeric(numeric_from_string(buf));
    return obj_new_json_int(iv);
}

/* RFC 8259 : true/false/null sont des "literal names". Strict : on
 * verifie aussi que le caractere suivant n'est pas un digit/letter
 * (sinon "trueXYZ" passerait pour "true" + reste en garbage). */
static bool jp_match_literal(JParser* p, const char* lit, int litLen) {
    if (p->pos + litLen > p->len) return false;
    if (memcmp(p->src + p->pos, lit, (size_t)litLen) != 0) return false;
    /* Caractere suivant : doit etre fin ou non-alphanumeric */
    int after = p->pos + litLen;
    if (after < p->len) {
        char c = p->src[after];
        if (isalnum((unsigned char)c) || c == '_') return false;
    }
    p->pos += litLen;
    return true;
}

static ObjJson* jp_value(JParser* p) {
    jp_ws(p);
    /* Strict : si on est en bout de buffer, rien a parser -> erreur. */
    if (p->pos >= p->len) {
        jp_set_error(p, "value: valeur JSON attendue (objet, tableau, chaine, nombre, true, false ou null)");
        return obj_new_json(JSON_T_NULL);
    }
    char c = p->src[p->pos];
    if (c == '"') {
        /*  : push s avant obj_new_json_string qui alloue → peut
         * grow → GC → s (fresh ObjString) collected → ObjJson recoit
         * dangling pointer. Bug exhibe par test_JSONVersChaine :
         * "Alice" collected pendant parse panier imbrique, slot reutilise
         * par "qte" → racine.values[0] = JSON_T_STRING("qte"). */
        ObjString* s = jp_string(p);
        vm_push(OBJ_VAL(s));
        ObjJson* result = obj_new_json_string(s);
        vm_pop();
        return result;
    }
    if (c == '{') return jp_object(p);
    if (c == '[') return jp_array(p);
    if (c == '-' || isdigit((unsigned char)c)) return jp_number(p);
    if (jp_match_literal(p, "true",  4)) return obj_new_json_bool(true);
    if (jp_match_literal(p, "false", 5)) return obj_new_json_bool(false);
    if (jp_match_literal(p, "null",  4)) return obj_new_json(JSON_T_NULL);
    /* Caractere inattendu */
    {
        char buf[100];
        snprintf(buf, sizeof(buf),
            "value: caractere inattendu '%c' (valeur JSON attendue)",
            (int)(unsigned char)p->src[p->pos]);
        jp_set_error(p, buf);
    }
    return obj_new_json(JSON_T_NULL);
}

/* Parse une chaine JSON en ObjJson* en reutilisant le parser interne.
 * Retourne NULL si parse errone ou incomplet. Le resultat est un objet GC :
 * le caller doit le rooter (vm_push) s'il alloue ensuite avant de l'exploiter.
 * Utilise hors natives (ex. lecture d'un petit JSON de controle sur stdin,
 * ). */
ObjJson* json_parse_cstr(const char* src, int len) {
    JParser p = { .src = src, .pos = 0, .len = len };
    ObjJson* result = jp_value(&p);
    jp_ws(&p);
    if (result == NULL || p.hasError || p.pos < p.len) return NULL;
    return result;
}

/* ================================================================
 * JSON SERIALIZER
 * ================================================================ */

typedef struct { char* buf; int len; int cap; } JBuf;

static void jb_init(JBuf* b) { b->buf = malloc(1024); b->len = 0; b->cap = 1024; }
static void jb_grow(JBuf* b, int need) {
    while (b->len + need >= b->cap) { b->cap *= 2; b->buf = realloc(b->buf, b->cap); }
}
static void jb_str(JBuf* b, const char* s, int n) { jb_grow(b, n); memcpy(b->buf + b->len, s, n); b->len += n; }
static void jb_puts(JBuf* b, const char* s) { jb_str(b, s, (int)strlen(s)); }
static void jb_char(JBuf* b, char c) { jb_grow(b, 1); b->buf[b->len++] = c; }

static void jb_indent(JBuf* b, int depth, bool compact) {
    if (compact) return;
    jb_char(b, '\n');
    for (int i = 0; i < depth; i++) jb_char(b, '\t');
}

static void jb_json_string(JBuf* b, const char* s) {
    jb_char(b, '"');
    for (; *s; s++) {
        switch (*s) {
            case '"':  jb_puts(b, "\\\""); break;
            case '\\': jb_puts(b, "\\\\"); break;
            case '\n': jb_puts(b, "\\n"); break;
            case '\r': jb_puts(b, "\\r"); break;
            case '\t': jb_puts(b, "\\t"); break;
            default:
                if ((unsigned char)*s < 0x20) {
                    char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*s);
                    jb_puts(b, esc);
                } else {
                    jb_char(b, *s);
                }
        }
    }
    jb_char(b, '"');
}

static void jb_serialize(JBuf* b, ObjJson* j, int depth, bool compact) {
    if (!j) { jb_puts(b, "null"); return; }
    switch (j->jtype) {
        case JSON_T_NULL: jb_puts(b, "null"); break;
        case JSON_T_BOOL: jb_puts(b, j->as.boolVal ? "true" : "false"); break;
        case JSON_T_INT: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)j->as.intVal);
            jb_puts(b, tmp); break;
        }
        case JSON_T_REAL: {
            char tmp[40]; owl_real_to_str(j->as.realVal, tmp, sizeof(tmp));
            jb_puts(b, tmp); break;
        }
        case JSON_T_CURRENCY: {
            char tmp[32]; currency_to_str(j->as.currencyVal, tmp, sizeof(tmp));
            jb_puts(b, tmp); break;
        }
        case JSON_T_NUMERIC: {
            char* s = numeric_format_alloc(OBJ_VAL(j->as.numericVal), "f");
            if (s != NULL) { jb_puts(b, s); owl_numeric_free_str(s); }
            else jb_puts(b, "0");
            break;
        }
        case JSON_T_STRING:
            jb_json_string(b, j->as.strVal->chars); break;
        case JSON_T_ARRAY:
            jb_char(b, '[');
            for (int i = 0; i < j->as.arrayVal.count; i++) {
                if (i > 0) jb_char(b, ',');
                jb_indent(b, depth + 1, compact);
                jb_serialize(b, j->as.arrayVal.items[i], depth + 1, compact);
            }
            if (j->as.arrayVal.count > 0) jb_indent(b, depth, compact);
            jb_char(b, ']');
            break;
        case JSON_T_OBJECT:
            jb_char(b, '{');
            for (int i = 0; i < j->as.objectVal.count; i++) {
                if (i > 0) jb_char(b, ',');
                jb_indent(b, depth + 1, compact);
                jb_json_string(b, j->as.objectVal.keys[i]->chars);
                jb_char(b, ':');
                if (!compact) jb_char(b, ' ');
                jb_serialize(b, j->as.objectVal.values[i], depth + 1, compact);
            }
            if (j->as.objectVal.count > 0) jb_indent(b, depth, compact);
            jb_char(b, '}');
            break;
    }
}

/* ================================================================
 * NATIVE FUNCTIONS
 * ================================================================ */

/* __JsonNew() -- create empty JSON object (used by compiler for "est un JSON") */
static Value fn_json_new(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_json(JSON_T_OBJECT));
}

/* ChaineVersJSON(s) / StringToJSON(s) */
static Value fn_chaine_vers_json(int argCount, Value* args) {
    (void)argCount;
    /* Accepte chaine OU buffer : un Buffer est une chaine d'octets binaire, lue
     * telle quelle par le parser. Aligne l'argument inline sur la coercion
     * Buffer->chaine deja appliquee a l'affectation (`s est une chaine = buf`). */
    const unsigned char* bytes = NULL;
    int len = 0;
    if (!value_as_bytes(args[0], &bytes, &len)) return NIL_VAL;
    JParser p = { .src = (const char*)bytes, .pos = 0, .len = len };
    ObjJson* result = jp_value(&p);
    jp_ws(&p);
    if (result == NULL || p.pos < p.len) {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d", p.pos);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "ChaineVersJSON",
                              "native", "ChaineVersJSON",
                              "position", posbuf,
                              "detail", p.hasError ? p.errMsg : "parse incomplet",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    return OBJ_VAL(result);
}

/* __JSONCoerce — coercion implicite chaine -> JSON a l'assignement type
 *   `o est un JSON = <chaine>` (declaration) / `o = <chaine>` (reaffectation)
 * injecte par le compilateur au meme titre que __BufferVersChaine et consorts.
 *
 * Coercion STRUCTURELLE (pas scalaire) : seule une chaine dont le premier
 * caractere non-blanc est `{` ou `[` est parsee comme JSON. Ce choix evite de
 * casser `= "hello"` (texte libre) et de surprendre sur `= "42"` (scalaire
 * numerique) : ces chaines ne commencent pas par `{`/`[` et traversent donc
 * inchangees.
 *   - non-chaine (JSON deja construit, autre type)  -> passthrough
 *   - chaine ne debutant pas par `{`/`[` apres trim -> passthrough (inchangee)
 *   - chaine structurelle valide                    -> document JSON reel
 *   - chaine structurelle malformee                 -> erreur runtime
 *     (ErreurDetectee = Vrai), pas de stockage silencieux. */
static Value fn_json_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (!IS_STRING(v)) return v;
    ObjString* s = AS_STRING(v);
    const char* p   = s->chars;
    const char* end = s->chars + s->length;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
                       || *p == '\f' || *p == '\v')) {
        p++;
    }
    if (p >= end || (*p != '{' && *p != '[')) {
        return v;  /* pas de structure reconnaissable : chaine conservee telle quelle */
    }
    /* Parse via JParser direct (comme fn_chaine_vers_json) pour exposer la
     * position reelle de l'echec plutot qu'un 0 factice. */
    JParser jp = { .src = s->chars, .pos = 0, .len = s->length };
    ObjJson* result = jp_value(&jp);
    jp_ws(&jp);
    if (result == NULL || jp.hasError || jp.pos < jp.len) {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d", jp.pos);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSON",
                              "native", "JSON",
                              "position", posbuf,
                              "detail", jp.hasError ? jp.errMsg : "structure JSON malformée",
                              NULL);
        return v;  /* erreur signalee : le typo n'est pas avale silencieusement */
    }
    owl_clear_error();
    return OBJ_VAL(result);
}

/* Serialisation formatting enum — values acceptable as the `mode` param
 * of JSONVersChaîne. Sentinelles centralisees. */
#include "sentinels.h"
#define PSD_MISE_EN_FORME OWL_PSD_MISE_EN_FORME
#define PSD_MINIFIE       OWL_PSD_MINIFIE

static const char* const E_psdFormat_values[] = {
    "psdMiseEnForme", "psdMinifie", "psdJSON", NULL
};
static const OwlEnum E_psdFormat = {
    .name="psdFormat", .nameEN="psdFormat", .category="JSON",
    .descFR="Modes de sérialisation (psdJSON pour Sérialise/Désérialise, psdMiseEnForme/psdMinifie pour JSONVersChaîne).",
    .descEN="Serialisation modes (psdJSON for Serialize/Deserialize, psdMiseEnForme/psdMinifie for JSONToString).",
    .values=E_psdFormat_values, .combinable=false
};

/* JSONVersChaine(j [, psdMiseEnForme|psdMinifie]) / JSONToString
 *
 * Param non-nullable JSON. Si l'arg est OWL Null, on coerce vers le défaut
 * du type d'entrée — l'object vide `{}` — cohérent avec la règle « Null à
 * une affectation typée non-nullable prend le défaut du type » (ex.
 * `s est une chaine = Null` → `""`). Pour obtenir la chaîne `"null"`
 * passer un véritable JSON null, ex. `ChaîneVersJSON("null")`. */
static Value fn_json_vers_chaine(int argCount, Value* args) {
    if (!IS_JSON(args[0])) {
        if (IS_NIL(args[0])) return OBJ_VAL(obj_copy_string("{}", 2));
        return NIL_VAL;
    }
    ObjJson* json = AS_JSON(args[0]);
    bool compact = false;
    if (argCount >= 2) {
        int64_t mode = 0;
        if (IS_INTEGER(args[1])) mode = AS_INTEGER(args[1]);
        if (mode == PSD_MINIFIE) compact = true;
    }
    JBuf b; jb_init(&b);
    jb_serialize(&b, json, 0, compact);
    jb_char(&b, '\0');
    ObjString* result = obj_copy_string(b.buf, b.len - 1);
    free(b.buf);
    return OBJ_VAL(result);
}

/* JSONValide(chaine [, options]) / JSONValid(s)
 *
 * Strict RFC 8259 par defaut (options=jsonStrict=0). Refus :
 *   - empty / whitespace only
 *   - structures incompletes (object/array/string non fermes)
 *   - trailing commas (`[1,2,]`, `{"a":1,}`)
 *   - leading zeros (`01`)
 *   - NaN / Infinity
 *
 * Options (bitmask combinable avec '+') :
 *   jsonAvecCommentaires  -- tolere les commentaires C-style (bloc)
 *
 * En cas d'echec, ErreurDetectee est mise a Vrai et ErreurInfo() retourne
 * un message detaille avec la position de l'erreur. */
static Value fn_json_valide(int argCount, Value* args) {
    if (!IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "JSONValide",
                              "native", "JSONValide",
                              "arg", "1",
                              "expected", "chaine",
                              "actual", "non chaine",
                              NULL);
        return BOOL_VAL(false);
    }
    ObjString* s = AS_STRING(args[0]);
    int opts = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) opts = (int)AS_INTEGER(args[1]);

    /* Cas borderline avant meme d'invoquer le parser */
    if (s->length == 0) {
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSONValide",
                              "native", "JSONValide",
                              "position", "1",
                              "detail", "chaine vide",
                              NULL);
        return BOOL_VAL(false);
    }

    JParser p = {
        .src = s->chars, .pos = 0, .len = s->length,
        .hasError = false, .errPos = 0, .errMsg = {0},
        .allowBlockComments = (opts & OWL_JSON_AVEC_COMMENTAIRES) != 0,
    };
    jp_ws(&p);
    if (p.pos >= p.len) {
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSONValide",
                              "native", "JSONValide",
                              "position", "1",
                              "detail", "whitespace seul",
                              NULL);
        return BOOL_VAL(false);
    }
    ObjJson* result = jp_value(&p);
    jp_ws(&p);

    if (p.hasError) {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d", p.errPos);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSONValide",
                              "native", "JSONValide",
                              "position", posbuf,
                              "detail", p.errMsg,
                              NULL);
        return BOOL_VAL(false);
    }
    if (result == NULL) {
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSONValide",
                              "native", "JSONValide",
                              "position", "0",
                              "detail", "parse a echoue sans message detaille",
                              NULL);
        return BOOL_VAL(false);
    }
    if (p.pos < p.len) {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d", p.pos + 1);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSONValide",
                              "native", "JSONValide",
                              "position", posbuf,
                              "detail", "contenu en trop apres la fin du JSON",
                              NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* JSONExiste(j, cle) / JSONExists(j, key) */
static Value fn_json_existe(int argCount, Value* args) {
    (void)argCount;
    if (!IS_JSON(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);
    ObjJson* json = AS_JSON(args[0]);
    ObjString* key = AS_STRING(args[1]);
    if (json->jtype != JSON_T_OBJECT) return BOOL_VAL(false);
    return BOOL_VAL(obj_json_object_get(json, key) != NULL);
}

/* JSONSupprime(j, cle) / JSONDelete(j, key) */
static Value fn_json_supprime(int argCount, Value* args) {
    (void)argCount;
    if (!IS_JSON(args[0]) || !IS_STRING(args[1])) return NIL_VAL;
    ObjJson* json = AS_JSON(args[0]);
    ObjString* key = AS_STRING(args[1]);
    if (json->jtype != JSON_T_OBJECT) return NIL_VAL;
    for (int i = 0; i < json->as.objectVal.count; i++) {
        if (obj_json_key_eq_ci(json->as.objectVal.keys[i], key)) {
            /* Shift remaining */
            for (int k = i; k < json->as.objectVal.count - 1; k++) {
                json->as.objectVal.keys[k] = json->as.objectVal.keys[k + 1];
                json->as.objectVal.values[k] = json->as.objectVal.values[k + 1];
            }
            json->as.objectVal.count--;
            return BOOL_VAL(true);
        }
    }
    return BOOL_VAL(false);
}

/* ================================================================
 * SÉRIALISE / DÉSÉRIALISE — psdJSON
 *
 * Convertit n'importe quelle valeur OWL en JSON et inversement, en
 * respectant le schéma de la variable cible (structure, classe, tableau
 * de structure, tableau de classe, types primitifs). Le variant et les
 * conteneurs (file/pile/liste, tableau associatif) sont aussi gérés.
 *
 * Pas de support psdBinaire / psdBinaireFormat16 (formats propriétaires
 * externes) — psdXML viendra en Lot J.3 si demande.
 * ================================================================ */

#define PSD_JSON OWL_PSD_JSON

/* Récursif : sérialise une Value OWL en JSON dans le buffer `b`. */
static void value_to_json(JBuf* b, Value v, int depth, bool compact) {
    if (IS_NIL(v))      { jb_puts(b, "null"); return; }
    if (IS_BOOL(v))     { jb_puts(b, AS_BOOL(v) ? "true" : "false"); return; }
    if (IS_INTEGER(v))  {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)AS_INTEGER(v));
        jb_puts(b, tmp); return;
    }
    if (IS_REAL(v))     {
        char tmp[40]; owl_real_to_str(AS_REAL(v), tmp, sizeof(tmp));
        jb_puts(b, tmp); return;
    }
    if (IS_CURRENCY(v)) {
        char tmp[32]; currency_to_str(AS_CURRENCY(v), tmp, sizeof(tmp));
        jb_puts(b, tmp); return;
    }
    if (IS_NUMERIC(v))  {
        char* s = numeric_format_alloc(v, "f");
        if (s != NULL) { jb_puts(b, s); owl_numeric_free_str(s); }
        else jb_puts(b, "0");
        return;
    }
    if (IS_STRING(v))   { jb_json_string(b, AS_STRING(v)->chars); return; }
    if (IS_JSON(v))     { jb_serialize(b, AS_JSON(v), depth, compact); return; }
    if (IS_DATE(v))     {
        ObjDate* d = AS_DATE(v);
        char tmp[16]; snprintf(tmp, sizeof(tmp), "%04d%02d%02d", d->year, d->month, d->day);
        jb_json_string(b, tmp); return;
    }
    if (IS_TIME(v))     {
        ObjTime* t = AS_TIME(v);
        char tmp[16]; snprintf(tmp, sizeof(tmp), "%02d%02d%02d%03d",
                              t->hour, t->minute, t->second, t->ms);
        jb_json_string(b, tmp); return;
    }
    if (IS_DATETIME(v)) {
        ObjDateTime* dt = AS_DATETIME(v);
        char tmp[24]; snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d%03d",
                               dt->year, dt->month, dt->day,
                               dt->hour, dt->minute, dt->second, dt->ms);
        jb_json_string(b, tmp); return;
    }
    if (IS_DURATION(v)) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)AS_DURATION(v)->totalMs);
        jb_puts(b, tmp); return;
    }
    if (IS_ARRAY(v)) {
        ObjArray* a = AS_ARRAY(v);
        jb_char(b, '[');
        for (int i = 0; i < a->elements.count; i++) {
            if (i > 0) jb_char(b, ',');
            jb_indent(b, depth + 1, compact);
            value_to_json(b, a->elements.values[i], depth + 1, compact);
        }
        if (a->elements.count > 0) jb_indent(b, depth, compact);
        jb_char(b, ']');
        return;
    }
    if (IS_ASSOC_ARRAY(v)) {
        ObjAssocArray* m = AS_ASSOC_ARRAY(v);
        jb_char(b, '{');
        for (int i = 0; i < m->count; i++) {
            if (i > 0) jb_char(b, ',');
            jb_indent(b, depth + 1, compact);
            jb_json_string(b, m->keys[i]->chars);
            jb_char(b, ':'); if (!compact) jb_char(b, ' ');
            value_to_json(b, m->values[i], depth + 1, compact);
        }
        if (m->count > 0) jb_indent(b, depth, compact);
        jb_char(b, '}');
        return;
    }
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        const char* typeName = inst->structName ? inst->structName->chars : NULL;
        int typeLen = inst->structName ? inst->structName->length : 0;
        /*  : pour les types builtin avec OwlField.computeGetter
         * (Chrono..Valeur / ..Demarre, cf ), le storage de
         * l'instance est ignore — on appelle le getter pour obtenir
         * l'etat live. Sans ca, serialiser un chrono qui tourne rend
         * la valeur figee au dernier appel ChronoXxx. */
        const OwlTypeDef* td = (typeName != NULL)
            ? owl_registry_type_find(typeName) : NULL;
        const OwlField* td_fields = (td != NULL) ? td->fields : NULL;
        jb_char(b, '{');
        int written = 0;
        if (inst->fieldOrder != NULL) {
            int n = inst->fieldOrder->elements.count;
            for (int i = 0; i < n; i++) {
                Value k = inst->fieldOrder->elements.values[i];
                if (!IS_STRING(k)) continue;
                ObjString* keyStr = AS_STRING(k);
                /* Consulter les attributs <Sérialise=...> du champ. */
                bool skip = false;
                const char* rename = NULL;
                if (typeName) {
                    compiler_user_type_field_serialise(
                        typeName, typeLen,
                        keyStr->chars, keyStr->length,
                        &skip, &rename);
                }
                if (skip) continue;
                Value fv;
                bool have = false;
                if (td_fields != NULL) {
                    for (const OwlField* f = td_fields; f->name; f++) {
                        if (f->computeGetter == NULL) continue;
                        if (strcasecmp(keyStr->chars, f->name) == 0
                            || (f->nameAccented
                                && strcasecmp(keyStr->chars, f->nameAccented) == 0)
                            || (f->nameEN
                                && strcasecmp(keyStr->chars, f->nameEN) == 0)) {
                            fv = f->computeGetter(1, &v);
                            have = true;
                            break;
                        }
                    }
                }
                if (!have && !table_get(&inst->fields, keyStr, &fv)) continue;
                if (written++ > 0) jb_char(b, ',');
                jb_indent(b, depth + 1, compact);
                jb_json_string(b, rename ? rename : keyStr->chars);
                jb_char(b, ':'); if (!compact) jb_char(b, ' ');
                value_to_json(b, fv, depth + 1, compact);
            }
        } else {
            /* Pas d'ordre déclaré : itère la table de hash dans son ordre interne. */
            for (int i = 0; i < inst->fields.capacity; i++) {
                Entry* e = &inst->fields.entries[i];
                if (e->key == NULL) continue;
                bool skip = false;
                const char* rename = NULL;
                if (typeName) {
                    compiler_user_type_field_serialise(
                        typeName, typeLen,
                        e->key->chars, e->key->length,
                        &skip, &rename);
                }
                if (skip) continue;
                Value fv = e->value;
                if (td_fields != NULL) {
                    for (const OwlField* f = td_fields; f->name; f++) {
                        if (f->computeGetter == NULL) continue;
                        if (strcasecmp(e->key->chars, f->name) == 0
                            || (f->nameAccented
                                && strcasecmp(e->key->chars, f->nameAccented) == 0)
                            || (f->nameEN
                                && strcasecmp(e->key->chars, f->nameEN) == 0)) {
                            fv = f->computeGetter(1, &v);
                            break;
                        }
                    }
                }
                if (written++ > 0) jb_char(b, ',');
                jb_indent(b, depth + 1, compact);
                jb_json_string(b, rename ? rename : e->key->chars);
                jb_char(b, ':'); if (!compact) jb_char(b, ' ');
                value_to_json(b, fv, depth + 1, compact);
            }
        }
        if (written > 0) jb_indent(b, depth, compact);
        jb_char(b, '}');
        return;
    }
    if (IS_COLLECTION(v)) {
        ObjCollection* coll = AS_COLLECTION(v);
        jb_char(b, '[');
        for (int i = 0; i < coll->elements.count; i++) {
            if (i > 0) jb_char(b, ',');
            jb_indent(b, depth + 1, compact);
            value_to_json(b, coll->elements.values[i], depth + 1, compact);
        }
        if (coll->elements.count > 0) jb_indent(b, depth, compact);
        jb_char(b, ']');
        return;
    }
    /* Type non sérialisable → "null" silencieux. */
    jb_puts(b, "null");
}

/* Sérialise(<Variable>, <Buffer>, <Paramètres> [, <Racine>])
 *   args[0] = variable à sérialiser
 *   args[1] = OUT — buffer (chaîne) qui reçoit le JSON
 *   args[2] = mode (psdJSON / psdMiseEnForme / psdMinifie)
 *   args[3] = racine (optionnelle, ignorée pour psdJSON pur — utilisée
 *             par psdXML quand celui-ci sera implémenté)
 *
 * psdJSON et psdMiseEnForme produisent un JSON pretty-printed.
 * psdMinifie produit un JSON compact. Toute autre valeur est traitée
 * comme psdJSON.
 */
static Value fn_serialise(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "Sérialise",
                              "native", "Sérialise",
                              "arg", "variable, buffer, mode",
                              NULL);
        return NIL_VAL;
    }
    int64_t mode = IS_INTEGER(args[2]) ? AS_INTEGER(args[2]) : (int64_t)PSD_JSON;
    bool compact = (mode == PSD_MINIFIE);

    JBuf b; jb_init(&b);
    value_to_json(&b, args[0], 0, compact);
    ObjString* result = obj_copy_string(b.buf, b.len);
    free(b.buf);
    args[1] = OBJ_VAL(result);
    owl_clear_error();
    return NIL_VAL;
}

/* Valide qu'une chaîne JSON représente un nombre décimal complet (signe
 * optionnel, un seul point décimal, exposant e/E optionnel), espaces de bord
 * tolérés. Parse STRICT plein-chaîne : tout caractère résiduel non consommé
 * invalide ("12abc" -> false), contrairement à strtod qui s'arrêterait à 'a'.
 * En cas de succès, recopie la sous-chaîne nettoyée (sans espaces de bord)
 * dans out (buffer appelant, taille outSize) et renvoie true. Sinon false —
 * l'appelant retombe alors sur le fallback 0. */
static bool json_string_looks_numeric(const ObjString* s, char* out, int outSize) {
    const char* p = s->chars;
    const char* end = s->chars + s->length;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    if (p >= end) return false;  /* chaîne vide ou blancs seuls */
    const char* q = p;
    if (*q == '+' || *q == '-') q++;
    bool digits = false;
    while (q < end && *q >= '0' && *q <= '9') { q++; digits = true; }
    if (q < end && *q == '.') {
        q++;
        while (q < end && *q >= '0' && *q <= '9') { q++; digits = true; }
    }
    if (!digits) return false;  /* au moins un chiffre requis */
    if (q < end && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < end && (*q == '+' || *q == '-')) q++;
        bool expDigits = false;
        while (q < end && *q >= '0' && *q <= '9') { q++; expDigits = true; }
        if (!expDigits) return false;  /* exposant sans chiffre */
    }
    if (q != end) return false;  /* reste non consommé -> invalide */
    int len = (int)(end - p);
    if (len >= outSize) return false;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return true;
}

/* Coerce un ObjJson vers le type d'un template (pour respecter le schéma
 * d'une structure / classe / tableau typé). Si template est NIL, on choisit
 * le type Owl le plus naturel selon le tag JSON. */
static Value coerce_json_to_value(Value template, ObjJson* j) {
    if (j == NULL) return NIL_VAL;

    /* === Cas primitifs : on respecte le type cible === */
    if (IS_INTEGER(template)) {
        if (j->jtype == JSON_T_INT)  return INTEGER_VAL(j->as.intVal);
        if (j->jtype == JSON_T_REAL) return INTEGER_VAL((int64_t)j->as.realVal);
        if (j->jtype == JSON_T_BOOL) return INTEGER_VAL(j->as.boolVal ? 1 : 0);
        if (j->jtype == JSON_T_STRING) {
            char buf[64];
            if (json_string_looks_numeric(j->as.strVal, buf, sizeof(buf)))
                return INTEGER_VAL(numeric_to_i64(numeric_from_string(buf), NULL));
        }
        return INTEGER_VAL(0);
    }
    if (IS_REAL(template)) {
        if (j->jtype == JSON_T_REAL) return REAL_VAL(j->as.realVal);
        if (j->jtype == JSON_T_INT)  return REAL_VAL((double)j->as.intVal);
        if (j->jtype == JSON_T_STRING) {
            char buf[64];
            if (json_string_looks_numeric(j->as.strVal, buf, sizeof(buf)))
                return REAL_VAL(strtod(buf, NULL));
        }
        return REAL_VAL(0.0);
    }
    if (IS_CURRENCY(template)) {
        if (j->jtype == JSON_T_CURRENCY) return CURRENCY_VAL(j->as.currencyVal);
        /* Source exacte : on garde la valeur NumericVal telle quelle, comme le
         * fait l'affectation d'un litteral 0n a un champ monetaire. La precision
         * du nombre source depasse souvent la capacite int64 du scale-6 ; forcer
         * CURRENCY_VAL tronquerait. La forme exacte round-trip sans perte. */
        if (j->jtype == JSON_T_NUMERIC)  return OBJ_VAL(j->as.numericVal);
        if (j->jtype == JSON_T_REAL) return CURRENCY_VAL((__int128)(j->as.realVal * (double)CURRENCY_SCALE));
        if (j->jtype == JSON_T_INT)  return CURRENCY_VAL((__int128)j->as.intVal * CURRENCY_SCALE);
        if (j->jtype == JSON_T_STRING) {
            /* Source exacte via numeric_from_string (même voie que la branche
             * JSON_T_NUMERIC ci-dessus) : "1234.50" round-trip sans dérive
             * float, à la différence d'un strtod * CURRENCY_SCALE. */
            char buf[64];
            if (json_string_looks_numeric(j->as.strVal, buf, sizeof(buf)))
                return numeric_from_string(buf);
        }
        return CURRENCY_VAL(0);
    }
    if (IS_NUMERIC(template)) {
        if (j->jtype == JSON_T_NUMERIC)  return OBJ_VAL(j->as.numericVal);
        if (j->jtype == JSON_T_CURRENCY) { char b[40]; currency_to_str(j->as.currencyVal, b, sizeof(b)); return numeric_from_string(b); }
        if (j->jtype == JSON_T_INT)  return numeric_from_i64(j->as.intVal);
        if (j->jtype == JSON_T_REAL) return numeric_from_real(j->as.realVal);
        if (j->jtype == JSON_T_STRING) {
            char buf[64];
            if (json_string_looks_numeric(j->as.strVal, buf, sizeof(buf)))
                return numeric_from_string(buf);
        }
        return numeric_from_i64(0);
    }
    if (IS_BOOL(template)) {
        if (j->jtype == JSON_T_BOOL) return BOOL_VAL(j->as.boolVal);
        if (j->jtype == JSON_T_INT)  return BOOL_VAL(j->as.intVal != 0);
        return BOOL_VAL(false);
    }
    if (IS_STRING(template)) {
        if (j->jtype == JSON_T_STRING) return OBJ_VAL(j->as.strVal);
        if (j->jtype == JSON_T_INT) {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)j->as.intVal);
            return OBJ_VAL(obj_copy_string(tmp, (int)strlen(tmp)));
        }
        if (j->jtype == JSON_T_REAL) {
            char tmp[40]; int n = owl_real_to_str(j->as.realVal, tmp, sizeof(tmp));
            return OBJ_VAL(obj_copy_string(tmp, n));
        }
        if (j->jtype == JSON_T_BOOL) {
            const char* s = j->as.boolVal ? "Vrai" : "Faux";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (IS_DATE(template) && j->jtype == JSON_T_STRING && j->as.strVal->length >= 8) {
        const char* s = j->as.strVal->chars;
        int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
        int m = (s[4]-'0')*10 + (s[5]-'0');
        int d = (s[6]-'0')*10 + (s[7]-'0');
        return OBJ_VAL(obj_new_date(y, m, d));
    }
    if (IS_TIME(template) && j->jtype == JSON_T_STRING && j->as.strVal->length >= 6) {
        const char* s = j->as.strVal->chars;
        int hh = (s[0]-'0')*10 + (s[1]-'0');
        int mm = (s[2]-'0')*10 + (s[3]-'0');
        int ss = (s[4]-'0')*10 + (s[5]-'0');
        int ms = 0;
        if (j->as.strVal->length >= 9) {
            ms = (s[6]-'0')*100 + (s[7]-'0')*10 + (s[8]-'0');
        }
        return OBJ_VAL(obj_new_time(hh, mm, ss, ms));
    }
    if (IS_DATETIME(template) && j->jtype == JSON_T_STRING && j->as.strVal->length >= 14) {
        const char* s = j->as.strVal->chars;
        int y  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
        int mo = (s[4]-'0')*10 + (s[5]-'0');
        int d  = (s[6]-'0')*10 + (s[7]-'0');
        int hh = (s[8]-'0')*10 + (s[9]-'0');
        int mm = (s[10]-'0')*10 + (s[11]-'0');
        int ss = (s[12]-'0')*10 + (s[13]-'0');
        int ms = 0;
        if (j->as.strVal->length >= 17) {
            ms = (s[14]-'0')*100 + (s[15]-'0')*10 + (s[16]-'0');
        }
        return OBJ_VAL(obj_new_datetime(y, mo, d, hh, mm, ss, ms));
    }
    if (IS_DURATION(template) && j->jtype == JSON_T_INT) {
        return OBJ_VAL(obj_new_duration(j->as.intVal));
    }

    /* === Tableau cible (typé ou non) === */
    if (IS_ARRAY(template)) {
        if (j->jtype != JSON_T_ARRAY) return template;
        ObjArray* tpl = AS_ARRAY(template);
        ObjArray* out = obj_new_array();
        /*  : out doit etre push avant obj_new_instance + coerce_json_to_value
         * qui peuvent allouer et declencher GC → out collected (victime non
         * rootee). value_array_write(&out->elements, coerced) recevrait out
         * pointant sur freed memory. Aussi, coerced est une Value contenant
         * potentiellement un Obj — utiliser obj_array_push pour la safety
         * push interne (le lint Pattern 2 ne capture pas le cas Value variable). */
        vm_push(OBJ_VAL(out));
        if (tpl->elementStructType != NULL) {
            for (int i = 0; i < j->as.arrayVal.count; i++) {
                /*   : push sub avant coerce_json_to_value
                 * recursive qui alloue → GC → sub fresh collecte → coerced
                 * retourne le template poison → array remplit de NIL/poison. */
                ObjInstance* sub = obj_new_instance(tpl->elementStructType);
                vm_push(OBJ_VAL(sub));
                Value coerced = coerce_json_to_value(OBJ_VAL(sub), j->as.arrayVal.items[i]);
                vm_pop(); /* sub */
                if (IS_OBJ(coerced)) vm_push(coerced);
                obj_array_push(out, coerced);
                if (IS_OBJ(coerced)) vm_pop();
            }
            out->elementStructType = tpl->elementStructType;
        } else {
            for (int i = 0; i < j->as.arrayVal.count; i++) {
                Value v = coerce_json_to_value(NIL_VAL, j->as.arrayVal.items[i]);
                if (IS_OBJ(v)) vm_push(v);
                obj_array_push(out, v);
                if (IS_OBJ(v)) vm_pop();
            }
        }
        vm_pop(); /* out */
        return OBJ_VAL(out);
    }

    /* === Instance cible (structure / classe) === */
    if (IS_INSTANCE(template)) {
        if (j->jtype != JSON_T_OBJECT) return template;
        ObjInstance* tpl = AS_INSTANCE(template);
        const char* typeName = tpl->structName ? tpl->structName->chars : NULL;
        int typeLen = tpl->structName ? tpl->structName->length : 0;
        for (int k = 0; k < j->as.objectVal.count; k++) {
            ObjString* jsonKey = j->as.objectVal.keys[k];
            ObjJson* sub = j->as.objectVal.values[k];

            /* 1) Recherche par <Sérialise="rename"> : si la clé JSON
             *    correspond au rename d'un champ, on cible ce champ. */
            ObjString* resolvedKey = jsonKey;
            if (typeName) {
                int canonLen = 0;
                const char* canon = compiler_user_type_field_by_rename(
                    typeName, typeLen,
                    jsonKey->chars, jsonKey->length, &canonLen);
                if (canon) {
                    resolvedKey = obj_copy_string(canon, canonLen);
                }
            }

            /* 2) Si pas de rename, fallback sur le nom direct (case+accent
             *    insensible — comme tout le mapping JSON OWL). On cherche
             *    le champ canonique dans la table de l'instance. */
            Value existing = NIL_VAL;
            ObjString* matchedKey = NULL;
            if (table_get(&tpl->fields, resolvedKey, &existing)) {
                matchedKey = resolvedKey;
            } else {
                /* Lookup souple : itère les fieldOrder pour matcher
                 * case+accent insensible. */
                if (tpl->fieldOrder != NULL) {
                    char qStripped[64];
                    int ql = scanner_strip_accents(resolvedKey->chars,
                        resolvedKey->length, qStripped, sizeof(qStripped));
                    int n = tpl->fieldOrder->elements.count;
                    for (int fi = 0; fi < n; fi++) {
                        Value fk = tpl->fieldOrder->elements.values[fi];
                        if (!IS_STRING(fk)) continue;
                        ObjString* fname = AS_STRING(fk);
                        char fStripped[64];
                        int fl = scanner_strip_accents(fname->chars,
                            fname->length, fStripped, sizeof(fStripped));
                        if (fl == ql && strncasecmp(qStripped, fStripped, (size_t)ql) == 0) {
                            matchedKey = fname;
                            table_get(&tpl->fields, fname, &existing);
                            break;
                        }
                    }
                }
            }

            if (matchedKey) {
                Value coerced = coerce_json_to_value(existing, sub);
                /*  : delegue au canon obj_inst_set_kv. */
                obj_inst_set_kv(tpl, matchedKey, coerced);
            } else {
                /* Champ inconnu côté template : on l'ajoute quand même
                 * (la structure peut avoir été créée vide pour
                 * désérialiser un JSON ad-hoc). */
                Value v = coerce_json_to_value(NIL_VAL, sub);
                obj_inst_set_kv(tpl, jsonKey, v);
            }
        }
        return template;
    }

    /* === Tableau associatif cible === */
    if (IS_ASSOC_ARRAY(template)) {
        if (j->jtype != JSON_T_OBJECT) return template;
        ObjAssocArray* tpl = AS_ASSOC_ARRAY(template);
        for (int k = 0; k < j->as.objectVal.count; k++) {
            Value v = coerce_json_to_value(NIL_VAL, j->as.objectVal.values[k]);
            obj_assoc_set(tpl, j->as.objectVal.keys[k], v);
        }
        return template;
    }

    /* === Variant / NIL → choisir le type Owl naturel selon le tag JSON === */
    if (IS_NIL(template) || IS_JSON(template)) {
        switch (j->jtype) {
            case JSON_T_NULL:   return NIL_VAL;
            case JSON_T_BOOL:   return BOOL_VAL(j->as.boolVal);
            case JSON_T_INT:      return INTEGER_VAL(j->as.intVal);
            case JSON_T_REAL:     return REAL_VAL(j->as.realVal);
            case JSON_T_CURRENCY: return CURRENCY_VAL(j->as.currencyVal);
            case JSON_T_NUMERIC:  return OBJ_VAL(j->as.numericVal);
            case JSON_T_STRING:   return OBJ_VAL(j->as.strVal);
            case JSON_T_ARRAY:  {
                /*  : out doit etre push pendant coerce_json_to_value
                 * recursif qui peut allouer → GC → out collected (memes raisons
                 * que L919). obj_array_push gere la value Value-in-var safe. */
                ObjArray* out = obj_new_array();
                vm_push(OBJ_VAL(out));
                for (int i = 0; i < j->as.arrayVal.count; i++) {
                    Value v = coerce_json_to_value(NIL_VAL, j->as.arrayVal.items[i]);
                    obj_array_push(out, v);
                }
                vm_pop();
                return OBJ_VAL(out);
            }
            case JSON_T_OBJECT: return OBJ_VAL(j);  /* on garde le JSON comme variant */
        }
    }

    return template;
}

/* Désérialise(<Variable>, <Buffer>, <Paramètres>)
 *   args[0] = OUT — variable cible (déjà allouée par le caller, son type
 *             sert de schéma)
 *   args[1] = buffer (chaîne JSON)
 *   args[2] = mode (psdJSON pour l'instant — psdXML viendra)
 *
 * Retourne Vrai si la désérialisation a réussi, Faux + ErreurDétectée
 * si JSON invalide.
 */
static Value fn_deserialise(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "Désérialise",
                              "native", "Désérialise",
                              "arg", "variable, buffer, mode",
                              NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Désérialise",
                              "native", "Désérialise",
                              "arg", "2",
                              "expected", "chaîne JSON",
                              "actual", "non chaîne",
                              NULL);
        return BOOL_VAL(false);
    }
    ObjString* buf = AS_STRING(args[1]);
    JParser p = { .src = buf->chars, .pos = 0, .len = buf->length };
    ObjJson* parsed = jp_value(&p);
    jp_ws(&p);
    if (parsed == NULL || p.pos < p.len) {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d", p.pos);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "Désérialise",
                              "native", "Désérialise",
                              "position", posbuf,
                              "detail", "JSON invalide",
                              NULL);
        return BOOL_VAL(false);
    }
    /*   : push parsed pendant coerce_json_to_value qui alloue
     * recursivement → GC → parsed (ObjJson) collecte. Symptome : jtype lu
     * 0xDD, branches IS_ARRAY etc. echouent, retourne template inchange
     * (tab2 reste vide). */
    vm_push(OBJ_VAL(parsed));
    args[0] = coerce_json_to_value(args[0], parsed);
    vm_pop(); /* parsed */
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Forme moderne (extension OWL) : JSONSérialise / JSONDésérialise
 *   - retour-valeur direct au lieu d'out-param
 *   - alias prefix .VersJSON([mode]) sur les types complexes
 *
 * Equivalences :
 *   buf est une chaine = JSONSérialise(p, psdJSON)        // forme moderne
 *   Sérialise(p, buf, psdJSON)                            // forme legacy canon
 *
 *   p2 est un Personne = JSONDésérialise(buf, p, psdJSON) // forme moderne
 *   Désérialise(p2, buf, psdJSON)                         // forme legacy canon
 *
 * On préfère 2 natives séparées plutôt qu'un overload sur Sérialise pour
 * éviter une complication compiler (out-param conditionnel selon argCount).
 * ================================================================ */

/* JSONSérialise(var [, mode]) → chaine */
static Value fn_json_serialise(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "JSONSérialise",
                              "native", "JSONSérialise",
                              "arg", "variable",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    int64_t mode = (argCount >= 2 && IS_INTEGER(args[1])) ? AS_INTEGER(args[1])
                                                          : (int64_t)PSD_JSON;
    bool compact = (mode == PSD_MINIFIE);
    JBuf b; jb_init(&b);
    value_to_json(&b, args[0], 0, compact);
    ObjString* result = obj_copy_string(b.buf, b.len);
    free(b.buf);
    owl_clear_error();
    return OBJ_VAL(result);
}

/* Clone superficiel d'une Value pour servir de template à JSONDésérialise.
 * Pour les structures/classes, on crée une nouvelle instance avec le même
 * structName + fieldOrder pour que la coercion respecte le schéma sans
 * muter l'instance passée. */
static Value clone_template(Value v) {
    if (IS_INSTANCE(v)) {
        ObjInstance* src = AS_INSTANCE(v);
        /*  : dst victime potentielle pendant clone_template recursif
         * sur sub-instances. Push dst pendant la boucle. */
        ObjInstance* dst = obj_new_instance(src->structName);
        vm_push(OBJ_VAL(dst));
        dst->fieldOrder = src->fieldOrder;
        dst->klass = src->klass;
        /* Copier les valeurs initiales pour préserver les types des champs
         * (le coerce s'en sert comme schéma). */
        if (src->fieldOrder != NULL) {
            int n = src->fieldOrder->elements.count;
            for (int i = 0; i < n; i++) {
                Value k = src->fieldOrder->elements.values[i];
                if (!IS_STRING(k)) continue;
                ObjString* keyStr = AS_STRING(k);
                Value fv;
                if (table_get(&src->fields, keyStr, &fv)) {
                    /* Récursif : si c'est une sub-instance, cloner aussi.
                     *  : delegue au canon obj_inst_set_kv. */
                    Value to_set = IS_INSTANCE(fv) ? clone_template(fv) : fv;
                    obj_inst_set_kv(dst, keyStr, to_set);
                }
            }
        }
        vm_pop(); /* dst */
        return OBJ_VAL(dst);
    }
    if (IS_ARRAY(v)) {
        ObjArray* src = AS_ARRAY(v);
        ObjArray* dst = obj_new_array();
        dst->elementStructType = src->elementStructType;
        dst->growthMode = src->growthMode;
        return OBJ_VAL(dst);
    }
    /* Primitives, assoc array, collection : on retourne la valeur telle
     * quelle ; coerce_json_to_value reconstruira une nouvelle instance
     * pour les conteneurs vides. */
    return v;
}

/*  — désérialisation guidée pour le binding de service HTTP : parse
 * `body` (JSON) et le coerce dans un template frais du type `typeName`. Sépare
 * le JSON malformé (400) du type inconnu (bind impossible). Codes de retour :
 *   0 = succès (*out rempli),
 *   1 = JSON malformé,
 *   2 = type structuré introuvable. */
int owl_json_bind_body(const char* typeName, const char* body, size_t len,
                       Value* out) {
    if (typeName == NULL) return 2;
    /* Le template d'un type structure vit dans vm.types (types du registry) ou
     * vm.globals (structures utilisateur). On en clone le schema sans muter
     * l'original (clone_template preserve structName + fieldOrder). */
    ObjString* k = obj_copy_string(typeName, (int)strlen(typeName));
    vm_push(OBJ_VAL(k));
    Value tplRaw;
    bool found = table_get(&vm.types, k, &tplRaw)
                 || table_get(&vm.globals, k, &tplRaw);
    vm_pop();       /* k */
    if (!found || !IS_INSTANCE(tplRaw)) return 2;

    vm_push(tplRaw);            /* root pendant clone_template */
    Value tpl = clone_template(tplRaw);
    vm_pop();                   /* tplRaw */
    if (!IS_INSTANCE(tpl)) return 2;
    vm_push(tpl);   /* root : jp_value + coerce allouent */

    JParser p = { .src = body ? body : "", .pos = 0, .len = (int)len };
    ObjJson* parsed = jp_value(&p);
    jp_ws(&p);
    if (parsed == NULL || p.pos < p.len) {
        vm_pop();   /* tpl */
        return 1;
    }
    vm_push(OBJ_VAL(parsed));
    *out = coerce_json_to_value(tpl, parsed);
    vm_pop();       /* parsed */
    vm_pop();       /* tpl */
    owl_clear_error();
    return 0;
}

/*  — sérialisation d'une Value OWL en JSON pour le corps d'une réponse
 * de service. Retourne un buffer malloc'd (à libérer par l'appelant via free) ;
 * *outLen reçoit la longueur. NULL si allocation impossible. Format non compact
 * (pretty), cohérent avec Sérialise(psdJSON). value_to_json n'alloue pas d'objet
 * OWL (buffer C interne), mais l'appelant doit garder `v` enraciné au cas où. */
char* owl_json_serialize_value(Value v, size_t* outLen) {
    JBuf b; jb_init(&b);
    if (b.buf == NULL) { if (outLen) *outLen = 0; return NULL; }
    value_to_json(&b, v, 0, false);
    if (outLen) *outLen = (size_t)b.len;
    return b.buf;   /* propriété transférée à l'appelant */
}

/* Variante MINIFIÉE (mono-ligne, sans indentation ni espaces) du sérialiseur
 * ci-dessus — même cœur value_to_json, flag compact=true. Utilisée par FluxÉcrit
 * pour honorer `donnée : tout` en émettant une valeur structurée en JSON sur une
 * seule ligne (SSE propre). Buffer malloc'd à libérer par l'appelant (free). */
char* owl_json_serialize_value_compact(Value v, size_t* outLen) {
    JBuf b; jb_init(&b);
    if (b.buf == NULL) { if (outLen) *outLen = 0; return NULL; }
    value_to_json(&b, v, 0, true);
    if (outLen) *outLen = (size_t)b.len;
    return b.buf;   /* propriété transférée à l'appelant */
}

/* JSONDésérialise(buf, template [, mode]) → variant rempli
 * Le `template` n'est PAS muté : on en clone le schéma puis on remplit
 * la copie depuis le JSON. */
static Value fn_json_deserialise(int argCount, Value* args) {
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "JSONDésérialise",
                              "native", "JSONDésérialise",
                              "arg", "buffer, template",
                              NULL);
        return NIL_VAL;
    }
    if (!IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "JSONDésérialise",
                              "native", "JSONDésérialise",
                              "arg", "1",
                              "expected", "chaîne JSON",
                              "actual", "non chaîne",
                              NULL);
        return NIL_VAL;
    }
    ObjString* buf = AS_STRING(args[0]);
    JParser p = { .src = buf->chars, .pos = 0, .len = buf->length };
    ObjJson* parsed = jp_value(&p);
    jp_ws(&p);
    if (parsed == NULL || p.pos < p.len) {
        char posbuf[16];
        snprintf(posbuf, sizeof(posbuf), "%d", p.pos);
        owl_emit_native_error(E_NATIVE_JSON_PARSE_ERROR, "JSONDésérialise",
                              "native", "JSONDésérialise",
                              "position", posbuf,
                              "detail", "JSON invalide",
                              NULL);
        return NIL_VAL;
    }
    /*   : push parsed pendant clone_template + coerce, et
     * push tpl pendant coerce. parsed et tpl alloues pendant des recurses
     * qui declenchent GC → collectes → resultat vide. */
    vm_push(OBJ_VAL(parsed));
    Value tpl = clone_template(args[1]);
    if (IS_OBJ(tpl)) vm_push(tpl);
    Value result = coerce_json_to_value(tpl, parsed);
    if (IS_OBJ(tpl)) vm_pop();
    vm_pop(); /* parsed */
    owl_clear_error();
    return result;
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

/* Register a global integer constant */
static void define_constant(const char* name, int64_t value) {
    /*  : delegue au canon obj_globals_set. */
    obj_globals_set(name, INTEGER_VAL(value));
}

void stdlib_json_init(void) {
    /* Serialization constants (canon compatible) */
    define_constant("psdMiseEnForme", PSD_MISE_EN_FORME);
    define_constant("psdMinifie", PSD_MINIFIE);
    define_constant("psdJSON",      PSD_JSON);

    /* Options JSONValide (extension OWL, bitmask combinable) */
    define_constant("jsonStrict", OWL_JSON_STRICT);
    define_constant("jsonAvecCommentaires", OWL_JSON_AVEC_COMMENTAIRES);

    /* Also publish to the registry so --vscode-data enumerates them. */
    {
        static const OwlConstDef JSON_CONSTS[] = {
            { .name = "psdMiseEnForme", .category = "JSON", .nameEN = "psdFormatting",
              .value = INTEGER_VAL(PSD_MISE_EN_FORME),
              .descFR = "Sérialisation JSON formatée (avec retours à la ligne).",
              .descEN = "JSON serialization with pretty-printing." },
            { .name = "psdMinifie", .category = "JSON", .nameEN = "psdMinified",
              .value = INTEGER_VAL(PSD_MINIFIE),
              .descFR = "Sérialisation JSON compacte (sans espaces).",
              .descEN = "Minified JSON serialization (no whitespace)." },
            { .name = "psdJSON", .category = "JSON", .nameEN = "psdJSON",
              .value = INTEGER_VAL(PSD_JSON),
              .descFR = "Sérialise/Désérialise au format JSON (RFC 8259).",
              .descEN = "Serialize/Deserialize using JSON format (RFC 8259)." },
            { .name = "jsonStrict", .category = "JSON",
              .value = INTEGER_VAL(OWL_JSON_STRICT),
              .descFR = "Validation JSON stricte (RFC 8259) -- valeur par défaut.",
              .descEN = "Strict JSON validation (RFC 8259) -- default value.",
              .nameEN = "jsonStrict" },
            { .name = "jsonAvecCommentaires", .category = "JSON",
              .value = INTEGER_VAL(OWL_JSON_AVEC_COMMENTAIRES),
              .descFR = "Tolère les commentaires C-style /* ... */ (style JSONC).",
              .descEN = "Allow C-style /* ... */ comments (JSONC style).",
              .nameEN = "jsonWithComments" },
        };
        owl_registry_register_consts(JSON_CONSTS, (int)(sizeof(JSON_CONSTS)/sizeof(JSON_CONSTS[0])));
        static const OwlEnum* const JSON_ENUMS[] = { &E_psdFormat };
        owl_registry_register_enums(JSON_ENUMS, (int)(sizeof(JSON_ENUMS)/sizeof(JSON_ENUMS[0])));
    }

    owl_define_native("__JsonNew", fn_json_new, 0);
    owl_define_native("__JSONCoerce", fn_json_coerce, 1);

    /* ================================================================
     * Registry declarations — JSON family.
     * Canonical names: ChaîneVersJSON, JSONVersChaîne, JSONValide.
     * OWL extensions: JSONExiste (key presence), JSONSupprime (key removal).
     * ================================================================ */
    {
        static const OwlParam p_s[]    = { {"chaîne", "chaîne", "chaîne JSON à parser"}, {0} };
        static const OwlParam p_j[]    = {
            {"json", "JSON",   "objet JSON à sérialiser"},
            {"mode", "entier", "mode de sérialisation (optionnel)", .allowedBase=&E_psdFormat},
            {0}
        };
        static const OwlParam p_jv[]   = { {"chaîne", "chaîne", "chaîne à valider"},
                                           {"options", "entier", "jsonStrict (défaut) ou jsonAvecCommentaires"}, {0} };
        static const OwlParam p_jm[]   = { {"json", "JSON", "objet JSON"},
                                           {"membre", "chaîne", "nom du membre"}, {0} };
        static const OwlParam p_ser[]  = {
            {"variable", "tout", "variable à sérialiser (structure, classe, tableau, file, pile, liste, JSON)"},
            {"buffer",   "chaîne",  "out — chaîne qui reçoit le résultat sérialisé"},
            {"mode",     "entier",  "format de sérialisation", .allowedBase=&E_psdFormat},
            {"racine",   "chaîne",  "nom de la racine (optionnel, ignoré pour psdJSON)"},
            {0}
        };
        static const OwlParam p_des[]  = {
            {"variable", "tout", "out — variable cible déjà allouée (son type sert de schéma)"},
            {"buffer",   "chaîne",  "chaîne à désérialiser"},
            {"mode",     "entier",  "format de désérialisation", .allowedBase=&E_psdFormat},
            {0}
        };
        static const OwlParam p_jsonser[] = {
            {"variable", "tout", "variable à sérialiser"},
            {"mode",     "entier",  "format de sérialisation (optionnel)", .allowedBase=&E_psdFormat},
            {0}
        };
        static const OwlParam p_jsondes[] = {
            {"buffer",   "chaîne",  "chaîne JSON à désérialiser"},
            {"template", "tout", "variable cible servant de schéma (non mutée)"},
            {"mode",     "entier",  "format de désérialisation (optionnel)", .allowedBase=&E_psdFormat},
            {0}
        };

        static const OwlSignature sig_parse[]  = { {1, p_s,  "JSON", "objet JSON, ou Null si invalide"}, {0} };
        /* JSONVersChaîne(<Variable JSON> [, <Options>]) — doc : 1-2 args. */
        static const OwlSignature sig_serial[] = {
            {1, p_j, "chaîne", "représentation JSON"},
            {2, p_j, "chaîne", "représentation JSON (avec mode)"},
            {0}
        };
        /* JSONValide(<Chaîne JSON>) — doc : 1 arg ; OWL ajoute un 2e arg
         * optionnel (options jsonStrict / jsonAvecCommentaires). */
        static const OwlSignature sig_valid[]  = {
            {1, p_jv, "booléen", "Vrai si JSON valide"},
            {2, p_jv, "booléen", "Vrai si JSON valide (avec options)"},
            {0}
        };
        static const OwlSignature sig_key[]    = { {2, p_jm, "booléen", "présence du membre / succès"}, {0} };
        static const OwlSignature sig_del[]    = { {2, p_jm, "rien", "supprime le membre"}, {0} };
        /* Sérialise(<Variable>, <Buffer>, <Paramètres> [, <Nom de la racine>]) —
         * doc : 3-4 args. La racine est acceptée par conformité mais ignorée
         * pour le format psdJSON. */
        static const OwlSignature sig_ser[]    = {
            {3, p_ser, "rien", "remplit le buffer (out-param)"},
            {4, p_ser, "rien", "remplit le buffer (out-param, avec racine)"},
            {0}
        };
        static const OwlSignature sig_des[]    = { {3,  p_des, "booléen", "Vrai si succès, Faux + ErreurDétectée sinon"}, {0} };
        /* JSONSérialise(<Variable> [, <Mode>]) — variante à valeur de retour. */
        static const OwlSignature sig_jsonser[] = {
            {1, p_jsonser, "chaîne", "représentation JSON de la variable"},
            {2, p_jsonser, "chaîne", "représentation JSON de la variable (avec mode)"},
            {0}
        };
        /* JSONDésérialise(<Buffer>, <Template> [, <Mode>]) → 2-3 args. */
        static const OwlSignature sig_jsondes[] = {
            {2, p_jsondes, "tout", "valeur reconstruite depuis le buffer (clone du template, rempli)"},
            {3, p_jsondes, "tout", "valeur reconstruite depuis le buffer (clone du template, rempli, avec mode)"},
            {0}
        };

        static const OwlNativeDef DEFS[] = {
            {
                .canonical      = "Cha\xc3\xae" "neVersJSON",  /* ChaîneVersJSON */
                .canonicalAscii = "ChaineVersJSON",
                .nameEN         = "StringToJSON",
                .category       = "JSON",
                .descFR         = "Parse une chaîne JSON et retourne l'objet JSON correspondant (Null si invalide).",
                .descEN         = "Parse a JSON string and return the corresponding JSON object (Null if invalid).",
                .signatures     = sig_parse,
                .impl           = fn_chaine_vers_json,
            },
            {
                .canonical      = "JSONVersCha\xc3\xae" "ne",  /* JSONVersChaîne */
                .canonicalAscii = "JSONVersChaine",
                .nameEN         = "JSONToString",
                .category       = "JSON",
                .descFR         = "Sérialise un objet JSON en chaîne (psdMiseEnForme ou psdMinifie).",
                .descEN         = "Serialize a JSON object to string (psdMiseEnForme or psdMinifie).",
                .signatures     = sig_serial,
                .impl           = fn_json_vers_chaine,
            },
            {
                .canonical  = "JSONValide",
                .nameEN     = "JSONValid",
                .category   = "JSON",
                .descFR     = "Vérifie si une chaîne est un JSON valide.",
                .descEN     = "Check if a string is valid JSON.",
                .signatures = sig_valid,
                .impl       = fn_json_valide,
            },
            {
                .canonical  = "JSONExiste",
                .nameEN     = "JSONExists",
                .category   = "JSON",
                .descFR     = "Renvoie Vrai si le membre nommé est présent dans l'objet JSON, Faux sinon.",
                .descEN     = "Returns True if the named member is present in the JSON object, False otherwise.",
                .signatures = sig_key,
                .impl       = fn_json_existe,
            },
            {
                .canonical  = "JSONSupprime",
                .nameEN     = "JSONDelete",
                .category   = "JSON",
                .descFR     = "Supprime un membre nommé d'un objet JSON. Renvoie Vrai si le membre existait, Faux sinon.",
                .descEN     = "Remove a named member from a JSON object. Returns True if the member existed, False otherwise.",
                .signatures = sig_del,
                .impl       = fn_json_supprime,
            },
            {
                .canonical      = "S\xc3\xa9" "rialise",       /* Sérialise */
                .canonicalAscii = "Serialise",
                .nameEN         = "Serialize",
                .category       = "JSON",
                .descFR         = "Sérialise une variable (structure, classe, tableau, file, pile, liste, variant) au format JSON. Le 2e argument (buffer) est un out-param qui reçoit le résultat.",
                .descEN         = "Serialize a variable (structure, class, array, queue, stack, list, variant) to JSON format. The 2nd argument (buffer) is an out-param receiving the result.",
                .signatures     = sig_ser,
                .impl           = fn_serialise,
                .flags          = OWL_NAT_OUT_PARAMS,
                .firstOutParamArg = 1,
                .lastOutParamArg  = 1,   /* seul args[1] (buffer) est out */
            },
            {
                .canonical      = "D\xc3\xa9" "s\xc3\xa9" "rialise",  /* Désérialise */
                .canonicalAscii = "Deserialise",
                .nameEN         = "Deserialize",
                .category       = "JSON",
                .descFR         = "Désérialise un buffer JSON dans une variable (déjà allouée). Le 1er argument est out-param : son type sert de schéma pour la coercion.",
                .descEN         = "Deserialize a JSON buffer into a variable (already allocated). The 1st argument is an out-param: its type serves as schema for coercion.",
                .signatures     = sig_des,
                .impl           = fn_deserialise,
                .flags          = OWL_NAT_OUT_PARAMS,
                .firstOutParamArg = 0,
                .lastOutParamArg  = 0,   /* seul args[0] (variable) est out */
            },
            {
                .canonical      = "JSONS\xc3\xa9" "rialise",     /* JSONSérialise */
                .canonicalAscii = "JSONSerialise",
                .nameEN         = "JSONSerialize",
                .category       = "JSON",
                .descFR         = "Sérialise une variable (structure, classe, tableau, file, pile, liste, variant) au format JSON et renvoie directement la chaîne produite. Variante à valeur de retour de Sérialise.",
                .descEN         = "Serialize a variable (structure, class, array, queue, stack, list, variant) to JSON format and return the produced string directly. Return-value variant of Serialize.",
                .signatures     = sig_jsonser,
                .impl           = fn_json_serialise,
            },
            {
                .canonical      = "JSOND\xc3\xa9" "s\xc3\xa9" "rialise",  /* JSONDésérialise */
                .canonicalAscii = "JSONDeserialise",
                .nameEN         = "JSONDeserialize",
                .category       = "JSON",
                .descFR         = "Désérialise une chaîne JSON en s'appuyant sur le 2e argument comme template de schéma (non muté). Renvoie une nouvelle valeur remplie depuis le JSON.",
                .descEN         = "Deserialize a JSON string using the 2nd argument as a schema template (not mutated). Returns a fresh value populated from the JSON.",
                .signatures     = sig_jsondes,
                .impl           = fn_json_deserialise,
            },
        };
        owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    }

    /* === Extension methods on strings: s.VersJSON() === */
    owl_register_method(METHOD_TYPE_STRING, "VersJSON", fn_chaine_vers_json, 1);
    owl_register_method(METHOD_TYPE_STRING, "ToJSON",   fn_chaine_vers_json, 1);

    /* === Extension methods on JSON: json.VersChaine(), json.Existe(), etc. === */
    owl_register_method(METHOD_TYPE_JSON, "VersChaine", fn_json_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_JSON, "ToString",   fn_json_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_JSON, "Valide",     fn_json_valide,       -1);
    owl_register_method(METHOD_TYPE_JSON, "Valid",       fn_json_valide,       -1);
    owl_register_method(METHOD_TYPE_JSON, "Existe",     fn_json_existe,       2);
    owl_register_method(METHOD_TYPE_JSON, "Exists",     fn_json_existe,       2);
    owl_register_method(METHOD_TYPE_JSON, "Supprime",   fn_json_supprime,     2);
    owl_register_method(METHOD_TYPE_JSON, "Delete",     fn_json_supprime,     2);
}
