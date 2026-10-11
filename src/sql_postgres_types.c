// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_postgres_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ================================================================
 * Oid -> category / name
 * ================================================================ */

PostgresCategory postgres_oid_category(uint32_t oid) {
    switch (oid) {
        case POSTGRES_OID_BOOL:                       return POSTGRES_CAT_BOOL;
        case POSTGRES_OID_INT2:
        case POSTGRES_OID_INT4:
        case POSTGRES_OID_INT8:
        case POSTGRES_OID_OID:                        return POSTGRES_CAT_INTEGER;
        case POSTGRES_OID_FLOAT4:
        case POSTGRES_OID_FLOAT8:                     return POSTGRES_CAT_REAL;
        case POSTGRES_OID_TEXT:
        case POSTGRES_OID_VARCHAR:
        case POSTGRES_OID_CHAR:
        case POSTGRES_OID_NAME:
        case POSTGRES_OID_NUMERIC:                    /* NUMERIC = arbitrary precision : on garde en string */
        case POSTGRES_OID_JSON:
        case POSTGRES_OID_JSONB:
        case POSTGRES_OID_UUID:                       return POSTGRES_CAT_STRING;
        case POSTGRES_OID_BYTEA:                      return POSTGRES_CAT_BYTEA;
        case POSTGRES_OID_DATE:                       return POSTGRES_CAT_DATE;
        case POSTGRES_OID_TIME:                       return POSTGRES_CAT_TIME;
        case POSTGRES_OID_TIMESTAMP:
        case POSTGRES_OID_TIMESTAMPTZ:                return POSTGRES_CAT_TIMESTAMP;
        default:                                return POSTGRES_CAT_UNKNOWN;
    }
}

const char* postgres_oid_name(uint32_t oid) {
    switch (oid) {
        case POSTGRES_OID_BOOL:        return "bool";
        case POSTGRES_OID_BYTEA:       return "bytea";
        case POSTGRES_OID_CHAR:        return "char";
        case POSTGRES_OID_NAME:        return "name";
        case POSTGRES_OID_INT8:        return "int8";
        case POSTGRES_OID_INT2:        return "int2";
        case POSTGRES_OID_INT4:        return "int4";
        case POSTGRES_OID_TEXT:        return "text";
        case POSTGRES_OID_OID:         return "oid";
        case POSTGRES_OID_JSON:        return "json";
        case POSTGRES_OID_FLOAT4:      return "float4";
        case POSTGRES_OID_FLOAT8:      return "float8";
        case POSTGRES_OID_VARCHAR:     return "varchar";
        case POSTGRES_OID_DATE:        return "date";
        case POSTGRES_OID_TIME:        return "time";
        case POSTGRES_OID_TIMESTAMP:   return "timestamp";
        case POSTGRES_OID_TIMESTAMPTZ: return "timestamptz";
        case POSTGRES_OID_NUMERIC:     return "numeric";
        case POSTGRES_OID_UUID:        return "uuid";
        case POSTGRES_OID_JSONB:       return "jsonb";
        default:                 return NULL;
    }
}

/* ================================================================
 * Parsing text mode
 *
 * Les bytes peuvent ne pas etre null-terminated. On utilise des
 * buffers locaux pour les conversions strto* qui attendent un C
 * string ; pour les types compacts (bool, int, double) c'est OK.
 * ================================================================ */

bool postgres_parse_bool(const char* in, size_t in_len, bool* out) {
    if (!in || in_len == 0) return false;
    /* PG envoie 't' ou 'f' en text mode. Tolere "true"/"false"/"1"/"0"/
     * "yes"/"no" + accept majuscules. */
    if (in_len == 1) {
        char c = (char)tolower((unsigned char)in[0]);
        if (c == 't' || c == 'y' || c == '1') { *out = true;  return true; }
        if (c == 'f' || c == 'n' || c == '0') { *out = false; return true; }
        return false;
    }
    /* Cas multi-chars. */
    char buf[8] = {0};
    if (in_len >= sizeof(buf)) return false;
    for (size_t i = 0; i < in_len; i++) buf[i] = (char)tolower((unsigned char)in[i]);
    if (strcmp(buf, "true")  == 0 || strcmp(buf, "yes") == 0) { *out = true;  return true; }
    if (strcmp(buf, "false") == 0 || strcmp(buf, "no")  == 0) { *out = false; return true; }
    return false;
}

bool postgres_parse_int64(const char* in, size_t in_len, int64_t* out) {
    if (!in || in_len == 0) return false;
    char buf[32];
    if (in_len >= sizeof(buf)) return false;
    memcpy(buf, in, in_len);
    buf[in_len] = '\0';
    char* end = NULL;
    errno = 0;
    long long v = strtoll(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

bool postgres_parse_double(const char* in, size_t in_len, double* out) {
    if (!in || in_len == 0) return false;
    char buf[64];
    if (in_len >= sizeof(buf)) return false;
    memcpy(buf, in, in_len);
    buf[in_len] = '\0';
    char* end = NULL;
    errno = 0;
    double v = strtod(buf, &end);
    if (errno != 0 || end == buf || *end != '\0') return false;
    *out = v;
    return true;
}

/* Helper : decode un char hex en valeur 0-15, ou -1 si invalide. */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool postgres_parse_bytea_hex(const char* in, size_t in_len,
                         uint8_t** out_bytes, size_t* out_len) {
    /* Format PG canonique depuis PG 9.0+ : "\xDEADBEEF" (prefix backslash-x). */
    if (in_len < 2 || in[0] != '\\' || in[1] != 'x') return false;
    /* Le reste doit etre des paires hex. */
    size_t hex_chars = in_len - 2;
    if (hex_chars % 2 != 0) return false;
    size_t out_bytes_len = hex_chars / 2;
    uint8_t* buf = (uint8_t*)malloc(out_bytes_len > 0 ? out_bytes_len : 1);
    if (!buf) return false;
    for (size_t i = 0; i < out_bytes_len; i++) {
        int hi = hex_val(in[2 + i*2]);
        int lo = hex_val(in[2 + i*2 + 1]);
        if (hi < 0 || lo < 0) { free(buf); return false; }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_bytes = buf;
    *out_len   = out_bytes_len;
    return true;
}

/* ================================================================
 * Formatting text mode
 * ================================================================ */

int postgres_format_bool(bool v, char* out, size_t out_size) {
    if (out_size < 2) return -1;
    out[0] = v ? 't' : 'f';
    out[1] = '\0';
    return 1;
}

int postgres_format_int64(int64_t v, char* out, size_t out_size) {
    int n = snprintf(out, out_size, "%lld", (long long)v);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return n;
}

int postgres_format_double(double v, char* out, size_t out_size) {
    /* %.17g preserve la precision IEEE 754 double round-trip. */
    int n = snprintf(out, out_size, "%.17g", v);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return n;
}

int postgres_format_bytea_hex(const uint8_t* bytes, size_t len,
                         char* out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    size_t need = 2 + 2 * len + 1;  /* "\x" + 2 hex per byte + '\0' */
    if (out_size < need) return -1;
    out[0] = '\\';
    out[1] = 'x';
    for (size_t i = 0; i < len; i++) {
        out[2 + i*2    ] = hex[(bytes[i] >> 4) & 0xF];
        out[2 + i*2 + 1] = hex[ bytes[i]       & 0xF];
    }
    out[2 + 2*len] = '\0';
    return (int)(2 + 2 * len);
}
