// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_types.h"

#include "object.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * Helpers : lecture LE depuis un buffer
 * ================================================================ */

static int16_t  read_i16_le(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }
static int32_t  read_i32_le(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static int64_t  read_i64_le(const uint8_t* p) {
    return (int64_t)((uint64_t)p[0]
                   | ((uint64_t)p[1] << 8)
                   | ((uint64_t)p[2] << 16)
                   | ((uint64_t)p[3] << 24)
                   | ((uint64_t)p[4] << 32)
                   | ((uint64_t)p[5] << 40)
                   | ((uint64_t)p[6] << 48)
                   | ((uint64_t)p[7] << 56));
}
static uint32_t read_u32_le(const uint8_t* p) { return (uint32_t)read_i32_le(p); }
static uint64_t read_u24_le(const uint8_t* p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16);
}
static uint64_t read_u40_le(const uint8_t* p) {
    return read_u24_le(p) | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32);
}

/* ================================================================
 * Helpers : date<->y/m/d, Hinnant
 *
 * date_from_days_1970 : entree = days depuis 1970-01-01, sortie y/m/d.
 * Pour SQL Server :
 *   - DATETIME : days depuis 1900-01-01 -> +25567 = days depuis 1970
 *   - DATE     : days depuis 0001-01-01 -> -719162 = days depuis 1970
 * ================================================================ */
static void date_from_days_1970(int32_t z, int* y, int* m, int* d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int yy = (int)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2)/153;
    *d = (int)(doy - (153*mp+2)/5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2 ? 1 : 0);
}

/* ================================================================
 * Mantissa bytes -> decimal string (precision arbitraire jusqu'a 16 octets)
 *
 * Algo : long division par 10. Chaque pass extrait un digit decimal de
 * poids faible. On reverse a la fin.
 * ================================================================ */
static size_t bytes_le_to_decimal_str(const uint8_t* bytes, size_t n_bytes,
                                       char* out, size_t out_cap) {
    uint8_t tmp[16];
    if (n_bytes > sizeof(tmp)) n_bytes = sizeof(tmp);
    memcpy(tmp, bytes, n_bytes);

    char digits[40];
    size_t d_count = 0;
    bool all_zero;
    do {
        uint32_t rem = 0;
        all_zero = true;
        for (int i = (int)n_bytes - 1; i >= 0; --i) {
            uint32_t cur = (rem << 8) | tmp[i];
            tmp[i] = (uint8_t)(cur / 10);
            rem = cur % 10;
            if (tmp[i]) all_zero = false;
        }
        if (d_count < sizeof(digits)) {
            digits[d_count++] = (char)('0' + rem);
        }
    } while (!all_zero && d_count < sizeof(digits));

    /* Cas valeur = 0 : on a un seul digit '0' deja. */
    /* Reverse + write */
    size_t o = 0;
    for (size_t i = 0; i < d_count && o + 1 < out_cap; ++i) {
        out[o++] = digits[d_count - 1 - i];
    }
    out[o] = '\0';
    return o;
}

/* Format complet DECIMAL : [-]integer.fractional avec scale. */
static size_t format_decimal_str(const uint8_t* mantissa, size_t n_bytes,
                                   uint8_t sign, uint8_t scale,
                                   char* out, size_t out_cap) {
    char raw[40];
    size_t n_digits = bytes_le_to_decimal_str(mantissa, n_bytes, raw, sizeof(raw));
    if (n_digits == 0) {
        if (out_cap >= 2) { out[0] = '0'; out[1] = '\0'; return 1; }
        return 0;
    }
    /* Pad gauche si scale > n_digits-1 (pour pouvoir mettre 0.xxx). */
    if ((size_t)scale + 1 > n_digits) {
        size_t pad = (size_t)scale + 1 - n_digits;
        if (n_digits + pad >= sizeof(raw)) pad = sizeof(raw) - 1 - n_digits;
        memmove(raw + pad, raw, n_digits + 1);  /* +1 pour le \0 */
        memset(raw, '0', pad);
        n_digits += pad;
    }

    size_t o = 0;
    /* SQL Server convention : sign=0 -> negative, sign=1 -> positive. */
    if (sign == 0) {
        if (o + 1 < out_cap) out[o++] = '-';
    }
    size_t int_len = n_digits - scale;
    for (size_t i = 0; i < int_len && o + 1 < out_cap; ++i) {
        out[o++] = raw[i];
    }
    if (scale > 0 && o + 1 < out_cap) {
        out[o++] = '.';
        for (size_t i = int_len; i < n_digits && o + 1 < out_cap; ++i) {
            out[o++] = raw[i];
        }
    }
    out[o] = '\0';
    return o;
}

/* ================================================================
 * UCS-2 LE -> UTF-8
 * ================================================================ */
static size_t ucs2le_to_utf8_bytes(const uint8_t* src, size_t n_bytes_in,
                                     char* out, size_t out_cap) {
    size_t n_units = n_bytes_in / 2;
    size_t o = 0;
    for (size_t i = 0; i < n_units && o + 4 < out_cap; ++i) {
        uint16_t cp = (uint16_t)(src[i*2] | (src[i*2+1] << 8));
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    if (o < out_cap) out[o] = '\0';
    return o;
}

/* ================================================================
 * Conversions specifiques par type
 * ================================================================ */

/* INTN/BITN/FLTN/MONEYN : on connait le len pour discriminer la sous-taille. */

static Value to_int_le(const uint8_t* p, size_t len) {
    switch (len) {
        case 1: return INTEGER_VAL((int64_t)(int8_t)p[0]);
        case 2: return INTEGER_VAL((int64_t)read_i16_le(p));
        case 4: return INTEGER_VAL((int64_t)read_i32_le(p));
        case 8: return INTEGER_VAL(read_i64_le(p));
        default: return NIL_VAL;
    }
}

static Value to_float(const uint8_t* p, size_t len) {
    if (len == 4) {
        uint32_t bits = read_u32_le(p);
        float f;
        memcpy(&f, &bits, 4);
        return REAL_VAL((double)f);
    } else if (len == 8) {
        uint64_t bits = (uint64_t)read_i64_le(p);
        double d;
        memcpy(&d, &bits, 8);
        return REAL_VAL(d);
    }
    return NIL_VAL;
}

static Value to_money(const uint8_t* p, size_t len) {
    int64_t raw;
    if (len == 4) {
        /* SMALLMONEY : i32 LE */
        raw = (int64_t)read_i32_le(p);
    } else if (len == 8) {
        /* MONEY : high u32 LE, low u32 LE -> i64 (high << 32 | low) */
        uint32_t high = read_u32_le(p);
        uint32_t low  = read_u32_le(p + 4);
        raw = (int64_t)(((uint64_t)high << 32) | (uint64_t)low);
    } else {
        return NIL_VAL;
    }
    return REAL_VAL((double)raw / 10000.0);
}

static Value to_guid(const uint8_t* p) {
    /* GUID 16 octets : 4 LE + 2 LE + 2 LE + 8 BE.
     * Format affiche : XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX */
    char buf[40];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        p[3], p[2], p[1], p[0],
        p[5], p[4],
        p[7], p[6],
        p[8], p[9],
        p[10], p[11], p[12], p[13], p[14], p[15]);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* SMALLDATETIME : 4 bytes (u16 days from 1900 + u16 minutes from midnight). */
static Value to_smalldatetime(const uint8_t* p) {
    uint16_t days = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t mins = (uint16_t)(p[2] | (p[3] << 8));
    int y, mo, d;
    date_from_days_1970((int32_t)days - 25567, &y, &mo, &d);
    int hh = mins / 60;
    int mm = mins % 60;
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:00", y, mo, d, hh, mm);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* DATETIME (legacy 8 bytes) : i32 days from 1900 + u32 ticks (1/300 sec). */
static Value to_datetime(const uint8_t* p) {
    int32_t  days  = read_i32_le(p);
    uint32_t ticks = read_u32_le(p + 4);
    int y, mo, d;
    date_from_days_1970(days - 25567, &y, &mo, &d);
    /* ticks: 1 unit = 1/300 sec = 3.333 ms. total ms = ticks * 10 / 3. */
    uint32_t total_ms = (uint32_t)((uint64_t)ticks * 10 / 3);
    uint32_t hh = (total_ms / 3600000) % 24;
    uint32_t mm = (total_ms / 60000) % 60;
    uint32_t ss = (total_ms / 1000) % 60;
    uint32_t ms = total_ms % 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03u",
             y, mo, d, hh, mm, ss, ms);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* DATE (TDS 7.3+) : 3 bytes u24 LE days since 0001-01-01. */
static Value to_date(const uint8_t* p) {
    uint64_t days_from_0001 = read_u24_le(p);
    /* 1970-01-01 = day 719162 since 0001-01-01.
     * NB : 0001-01-01 = day 0 dans SQL Server (Gregorian). */
    int32_t days_from_1970 = (int32_t)days_from_0001 - 719162;
    int y, mo, d;
    date_from_days_1970(days_from_1970, &y, &mo, &d);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, d);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* TIME (TDS 7.3+) : 3/4/5 bytes LE = ticks (100ns) since midnight, taille
 * depend du scale. On reconstruit jusqu'au scale donne (0-7). */
static uint64_t read_time_ticks(const uint8_t* p, size_t len) {
    switch (len) {
        case 3: return read_u24_le(p);
        case 4: return read_u24_le(p) | ((uint64_t)p[3] << 24);
        case 5: return read_u40_le(p);
        default: return 0;
    }
}

static Value to_time(const uint8_t* p, size_t len, uint8_t scale) {
    uint64_t ticks_in_unit = read_time_ticks(p, len);
    /* Le tick "naturel" est en 10^-scale secondes. Pour scale=7 c'est 100ns. */
    /* Pour normaliser, on convertit en nanosecondes en multipliant par
     * 10^(9 - 7 + (7 - scale)) = 10^(9 - scale). Equivalent : on remet en
     * unites de 100ns en multipliant par 10^(7-scale). */
    uint64_t mul = 1;
    for (int i = 0; i < 7 - (int)scale; ++i) mul *= 10;
    uint64_t ticks_100ns = ticks_in_unit * mul;
    /* 10^7 ticks_100ns par seconde. */
    uint64_t secs_total = ticks_100ns / 10000000ULL;
    uint64_t frac_100ns = ticks_100ns % 10000000ULL;
    uint32_t hh = (uint32_t)((secs_total / 3600) % 24);
    uint32_t mm = (uint32_t)((secs_total / 60) % 60);
    uint32_t ss = (uint32_t)(secs_total % 60);
    char buf[24];
    if (scale == 0) {
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hh, mm, ss);
    } else {
        /* On affiche scale digits de fraction. */
        uint64_t frac = frac_100ns;
        for (int i = 0; i < 7 - (int)scale; ++i) frac /= 10;
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%0*llu",
                 hh, mm, ss, (int)scale, (unsigned long long)frac);
    }
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* DATETIME2 : N bytes TIME + 3 bytes DATE. N selon scale. */
static Value to_datetime2(const uint8_t* p, size_t len, uint8_t scale) {
    /* N_time bytes : scale 0-2 -> 3, scale 3-4 -> 4, scale 5-7 -> 5. */
    size_t n_time = (scale <= 2) ? 3 : (scale <= 4) ? 4 : 5;
    if (len != n_time + 3) return NIL_VAL;
    /* Date a la fin */
    Value date_v = to_date(p + n_time);
    Value time_v = to_time(p, n_time, scale);
    if (!IS_STRING(date_v) || !IS_STRING(time_v)) return NIL_VAL;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %s",
             AS_STRING(date_v)->chars, AS_STRING(time_v)->chars);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* DATETIMEOFFSET : DATETIME2 + 2 bytes i16 LE offset minutes. */
static Value to_datetimeoffset(const uint8_t* p, size_t len, uint8_t scale) {
    if (len < 5) return NIL_VAL;
    Value dt2 = to_datetime2(p, len - 2, scale);
    if (!IS_STRING(dt2)) return NIL_VAL;
    int16_t off_min = read_i16_le(p + len - 2);
    char sign = off_min >= 0 ? '+' : '-';
    int abs_min = off_min >= 0 ? off_min : -off_min;
    int hh = abs_min / 60;
    int mm = abs_min % 60;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %c%02d:%02d",
             AS_STRING(dt2)->chars, sign, hh, mm);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* DECIMAL/NUMERIC : 1 octet sign + mantissa LE. */
static Value to_decimal(const uint8_t* p, size_t len, uint8_t scale) {
    if (len < 2) return NIL_VAL;
    uint8_t sign = p[0];
    char buf[64];
    format_decimal_str(p + 1, len - 1, sign, scale, buf, sizeof(buf));
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* ================================================================
 * Dispatch principal
 * ================================================================ */

Value owl_mssql_cell_to_owl(const OwlMssqlColumn* col, const OwlMssqlCell* cell) {
    if (cell->is_null) return NIL_VAL;
    const uint8_t* p = cell->data;
    size_t len = cell->len;

    switch (col->type_code) {
        /* === Integer === */
        case 0x30:   /* TINYINT u8 */
            if (len < 1) return NIL_VAL;
            return INTEGER_VAL((int64_t)(uint8_t)p[0]);
        case 0x34:   /* SMALLINT i16 LE */
            if (len < 2) return NIL_VAL;
            return INTEGER_VAL((int64_t)read_i16_le(p));
        case 0x38:   /* INT4 */
            if (len < 4) return NIL_VAL;
            return INTEGER_VAL((int64_t)read_i32_le(p));
        case 0x7F:   /* BIGINT */
            if (len < 8) return NIL_VAL;
            return INTEGER_VAL(read_i64_le(p));
        case 0x26:   /* INTN variable */
            return to_int_le(p, len);

        /* === Bool === */
        case 0x32:   /* BIT */
        case 0x68:   /* BITN */
            if (len < 1) return NIL_VAL;
            return BOOL_VAL(p[0] != 0);

        /* === Float === */
        case 0x3B:   /* FLT4 (REAL) */
            return to_float(p, 4);
        case 0x3E:   /* FLT8 (FLOAT) */
            return to_float(p, 8);
        case 0x6D:   /* FLTN */
            return to_float(p, len);

        /* === Money === */
        case 0x7A:   /* SMALLMONEY */
            return to_money(p, 4);
        case 0x3C:   /* MONEY */
            return to_money(p, 8);
        case 0x6E:   /* MONEYN */
            return to_money(p, len);

        /* === Datetime legacy === */
        case 0x3A:   /* SMALLDATETIME */
            if (len < 4) return NIL_VAL;
            return to_smalldatetime(p);
        case 0x3D:   /* DATETIME8 */
            if (len < 8) return NIL_VAL;
            return to_datetime(p);
        case 0x6F:   /* DATETIMN */
            if (len == 4) return to_smalldatetime(p);
            if (len == 8) return to_datetime(p);
            return NIL_VAL;

        /* === Date/time TDS 7.3+ === */
        case 0x28:   /* DATE */
            if (len < 3) return NIL_VAL;
            return to_date(p);
        case 0x29:   /* TIME */
            return to_time(p, len, col->scale);
        case 0x2A:   /* DATETIME2 */
            return to_datetime2(p, len, col->scale);
        case 0x2B:   /* DATETIMEOFFSET */
            return to_datetimeoffset(p, len, col->scale);

        /* === GUID === */
        case 0x24:   /* UNIQUEIDENTIFIER */
            if (len < 16) return NIL_VAL;
            return to_guid(p);

        /* === Decimal === */
        case 0x6A:   /* DECIMALN */
        case 0x6C:   /* NUMERICN */
            return to_decimal(p, len, col->scale);

        /* === Strings === */
        case 0xE7:   /* NVARCHAR (UCS-2 LE) */
        case 0xEF:   /* NCHAR */
        {
            /* Buffer scratch : 3 octets UTF-8 max par char UCS-2 BMP. */
            size_t out_cap = len * 3 / 2 + 4;
            char* buf = (char*)malloc(out_cap);
            if (!buf) return NIL_VAL;
            size_t n = ucs2le_to_utf8_bytes(p, len, buf, out_cap);
            ObjString* s = obj_copy_string(buf, (int)n);
            free(buf);
            return OBJ_VAL(s);
        }
        case 0xA7:   /* BIGVARCHAR (single-byte avec collation) */
        case 0xAF:   /* BIGCHAR */
            /* V1 : on traite les bytes comme ASCII/UTF-8 brut. Pas de mapping
             * windows-1252 -> UTF-8. Pour des donnees ASCII pures (cas le plus
             * frequent) c'est fonctionnel. Mapping complet en V2. */
            return OBJ_VAL(obj_copy_string((const char*)p, (int)len));

        /* === Binary === */
        case 0xA5:   /* BIGVARBINARY */
        case 0xAD:   /* BIGBINARY */
            return OBJ_VAL(obj_new_buffer(p, (int)len));

        /* === Non supportes V1 === */
        case 0x22:   /* IMAGE */
        case 0x23:   /* TEXT */
        case 0x63:   /* NTEXT */
        default:
            return NIL_VAL;
    }
}
