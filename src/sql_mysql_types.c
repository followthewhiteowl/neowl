// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mysql_types.h"

#include "object.h"
#include "vm.h"

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
