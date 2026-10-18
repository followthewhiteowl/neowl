// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mysql_types.h"

#include "object.h"
#include "vm.h"
#include "net_le_buffer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Convertit une string decimale (avec '-' optionnel) en int64. Tolere
 * les espaces et le '.0' final (MySQL peut retourner "42.0" pour un
 * NUMERIC entier — rare mais defensif). */
static int64_t parse_int_safe(const uint8_t* bytes, int32_t len) {
    if (len <= 0 || bytes == NULL) return 0;
    char buf[32];
    int copy = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, bytes, copy);
    buf[copy] = '\0';
    return (int64_t)strtoll(buf, NULL, 10);
}

static double parse_real_safe(const uint8_t* bytes, int32_t len) {
    if (len <= 0 || bytes == NULL) return 0.0;
    char buf[64];
    int copy = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, bytes, copy);
    buf[copy] = '\0';
    return strtod(buf, NULL);
}

Value owl_mysql_value_to_owl(uint8_t mysql_type, uint16_t flags,
                               const uint8_t* value_bytes, int32_t value_len) {
    /* NULL SQL */
    if (value_len < 0) return NIL_VAL;

    (void)flags;  /* UNSIGNED peut etre utilise plus tard pour overflow detect */

    switch (mysql_type) {
        /* ----- Entiers ----- */
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_BIT: {
            /* BIT en text mode arrive comme un caractere par bit groupe (rare).
             * On accepte la conversion comme entier — l'user pourra masquer. */
            return INTEGER_VAL(parse_int_safe(value_bytes, value_len));
        }

        /* ----- Reels ----- */
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
            return REAL_VAL(parse_real_safe(value_bytes, value_len));
        }

        /* ----- Strings (text + dates + enum/set + json) ----- */
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
        case MYSQL_TYPE_JSON:
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP: {
            ObjString* s = obj_copy_string((const char*)value_bytes, value_len);
            return OBJ_VAL(s);
        }

        /* ----- Binaires (BLOB families) ----- */
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_GEOMETRY: {
            ObjBuffer* b = obj_new_buffer(value_bytes, value_len);
            return OBJ_VAL(b);
        }

        case MYSQL_TYPE_NULL:
            return NIL_VAL;

        default: {
            /* Type inconnu : on retourne la string brute (graceful fallback). */
            ObjString* s = obj_copy_string((const char*)value_bytes, value_len);
            return OBJ_VAL(s);
        }
    }
}

/* ================================================================
 * Requetes preparees : encodage / decodage binaire
 * ================================================================ */

uint16_t owl_mysql_bind_type_code(const OwlMysqlBindValue* b) {
    uint8_t type;
    switch (b->kind) {
        case OWL_MYSQL_BIND_NULL:   type = MYSQL_TYPE_NULL;       break;
        case OWL_MYSQL_BIND_INT:    type = MYSQL_TYPE_LONGLONG;   break;
        case OWL_MYSQL_BIND_REAL:   type = MYSQL_TYPE_DOUBLE;     break;
        case OWL_MYSQL_BIND_BOOL:   type = MYSQL_TYPE_TINY;       break;
        case OWL_MYSQL_BIND_STRING: type = MYSQL_TYPE_VAR_STRING; break;
        default:                    type = MYSQL_TYPE_VAR_STRING; break;
    }
    /* Octet haut (flag) = 0 : toutes nos valeurs liees sont signees. */
    return (uint16_t)type;
}

void owl_mysql_bind_write_value(OwlLeBuffer* buf, const OwlMysqlBindValue* b) {
    switch (b->kind) {
        case OWL_MYSQL_BIND_NULL:
            /* Rien : presence signalee par le null-bitmap. */
            break;
        case OWL_MYSQL_BIND_INT:
            owl_le_buffer_write_u64(buf, (uint64_t)b->i_val);
            break;
        case OWL_MYSQL_BIND_BOOL:
            owl_le_buffer_write_u8(buf, b->b_val ? 1 : 0);
            break;
        case OWL_MYSQL_BIND_REAL: {
            /* IEEE-754 double en little-endian (hote x86 LE). */
            double d = b->r_val;
            owl_le_buffer_write_bytes(buf, &d, sizeof(d));
            break;
        }
        case OWL_MYSQL_BIND_STRING:
            owl_le_buffer_write_lenenc_str(buf,
                                           (const char*)b->s_bytes, b->s_len);
            break;
        default:
            owl_le_buffer_write_lenenc_str(buf, "", 0);
            break;
    }
}

/* Append une chaine formatee (snprintf deja fait) dans textbuf. */
static bool textbuf_append(OwlLeBuffer* textbuf, const char* s, size_t len,
                           size_t start, size_t* out_off, size_t* out_len) {
    owl_le_buffer_write_bytes(textbuf, s, len);
    *out_off = start;
    *out_len = textbuf->length - start;
    return true;
}

bool owl_mysql_binary_read_to_text(OwlLeBuffer* in, uint8_t mysql_type,
                                   uint16_t flags, OwlLeBuffer* textbuf,
                                   size_t* out_off, size_t* out_len) {
    size_t start = textbuf->length;
    bool is_unsigned = (flags & MYSQL_FLAG_UNSIGNED) != 0;
    char buf[64];

    switch (mysql_type) {
        case MYSQL_TYPE_TINY: {
            uint8_t v;
            if (!owl_le_buffer_read_u8(in, &v)) return false;
            int n = is_unsigned
                  ? snprintf(buf, sizeof(buf), "%u", (unsigned)v)
                  : snprintf(buf, sizeof(buf), "%d", (int)(int8_t)v);
            return textbuf_append(textbuf, buf, (size_t)n, start, out_off, out_len);
        }
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_YEAR: {
            uint16_t v;
            if (!owl_le_buffer_read_u16(in, &v)) return false;
            int n = is_unsigned
                  ? snprintf(buf, sizeof(buf), "%u", (unsigned)v)
                  : snprintf(buf, sizeof(buf), "%d", (int)(int16_t)v);
            return textbuf_append(textbuf, buf, (size_t)n, start, out_off, out_len);
        }
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24: {
            uint32_t v;
            if (!owl_le_buffer_read_u32(in, &v)) return false;
            int n = is_unsigned
                  ? snprintf(buf, sizeof(buf), "%u", (unsigned)v)
                  : snprintf(buf, sizeof(buf), "%d", (int)(int32_t)v);
            return textbuf_append(textbuf, buf, (size_t)n, start, out_off, out_len);
        }
        case MYSQL_TYPE_LONGLONG: {
            uint64_t v;
            if (!owl_le_buffer_read_u64(in, &v)) return false;
            int n = is_unsigned
                  ? snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v)
                  : snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)v);
            return textbuf_append(textbuf, buf, (size_t)n, start, out_off, out_len);
        }
        case MYSQL_TYPE_FLOAT: {
            uint8_t raw[4]; float f;
            if (!owl_le_buffer_read_bytes(in, raw, 4)) return false;
            memcpy(&f, raw, 4);  /* hote LE */
            int n = snprintf(buf, sizeof(buf), "%.9g", (double)f);
            return textbuf_append(textbuf, buf, (size_t)n, start, out_off, out_len);
        }
        case MYSQL_TYPE_DOUBLE: {
            uint8_t raw[8]; double d;
            if (!owl_le_buffer_read_bytes(in, raw, 8)) return false;
            memcpy(&d, raw, 8);  /* hote LE */
            int n = snprintf(buf, sizeof(buf), "%.17g", d);
            return textbuf_append(textbuf, buf, (size_t)n, start, out_off, out_len);
        }
        default: {
            /* Strings, blobs, decimal, dates, enum/set, json : lenenc string.
             * Les octets sont recopies tels quels dans textbuf. */
            size_t L;
            const uint8_t* d = owl_le_buffer_read_lenenc_str(in, &L);
            if (d == NULL && L != 0) return false;
            if (L > 0 && d) owl_le_buffer_write_bytes(textbuf, d, L);
            *out_off = start;
            *out_len = textbuf->length - start;
            return true;
        }
    }
}
