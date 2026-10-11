// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_duckdb_types.h"

#include "object.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Validity bitmap : entry index = row >> 6, bit = row & 63.
 * Bit 1 = valid, 0 = NULL. validity NULL -> tous valides. */
static bool row_is_valid(uint64_t* validity, uint64_t row) {
    if (validity == NULL) return true;
    uint64_t entry = validity[row / 64];
    return (entry >> (row & 63)) & 1ULL;
}

static Value extract_varchar(duckdb_vector vec, uint64_t row) {
    duckdb_string_t* arr = (duckdb_string_t*)duckdb_vector_get_data(vec);
    duckdb_string_t s = arr[row];
    const char* data;
    uint32_t len;
    if (duckdb_string_is_inlined(s)) {
        data = s.value.inlined.inlined;
        len = s.value.inlined.length;
    } else {
        data = s.value.pointer.ptr;
        len = s.value.pointer.length;
    }
    ObjString* os = obj_copy_string(data, (int)len);
    return OBJ_VAL(os);
}

static Value extract_blob(duckdb_vector vec, uint64_t row) {
    duckdb_string_t* arr = (duckdb_string_t*)duckdb_vector_get_data(vec);
    duckdb_string_t s = arr[row];
    const uint8_t* data;
    uint32_t len;
    if (duckdb_string_is_inlined(s)) {
        data = (const uint8_t*)s.value.inlined.inlined;
        len = s.value.inlined.length;
    } else {
        data = (const uint8_t*)s.value.pointer.ptr;
        len = s.value.pointer.length;
    }
    ObjBuffer* b = obj_new_buffer(data, (int)len);
    return OBJ_VAL(b);
}

/* DATE : days since 1970-01-01 (int32). Algorithme Howard Hinnant. */
static void date_from_days(int32_t z, int* y, int* m, int* d) {
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

static Value extract_date(duckdb_vector vec, uint64_t row) {
    duckdb_date* arr = (duckdb_date*)duckdb_vector_get_data(vec);
    int y, m, d;
    date_from_days(arr[row].days, &y, &m, &d);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

static Value extract_timestamp(duckdb_vector vec, uint64_t row) {
    duckdb_timestamp* arr = (duckdb_timestamp*)duckdb_vector_get_data(vec);
    int64_t micros = arr[row].micros;
    int64_t secs = micros / 1000000;
    int us = (int)(micros % 1000000);
    if (us < 0) { us += 1000000; secs -= 1; }
    int64_t days = secs / 86400;
    int sec_in_day = (int)(secs % 86400);
    if (sec_in_day < 0) { sec_in_day += 86400; days -= 1; }
    int y, mo, d;
    date_from_days((int32_t)days, &y, &mo, &d);
    int hh = sec_in_day / 3600;
    int mm = (sec_in_day / 60) % 60;
    int ss = sec_in_day % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
             y, mo, d, hh, mm, ss, us);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

static Value extract_time(duckdb_vector vec, uint64_t row) {
    duckdb_time* arr = (duckdb_time*)duckdb_vector_get_data(vec);
    int64_t micros = arr[row].micros;
    int hh = (int)(micros / 3600000000LL);
    int mm = (int)((micros / 60000000LL) % 60);
    int ss = (int)((micros / 1000000LL) % 60);
    int us = (int)(micros % 1000000LL);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d", hh, mm, ss, us);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* Fallback : stringify via duckdb_value_varchar (API deprecated mais
 * fonctionnelle, utilisee pour LIST/STRUCT/MAP/UUID/DECIMAL/HUGEINT/...). */
static Value fallback_value_varchar(duckdb_result* result, uint64_t col,
                                      uint64_t row) {
    if (result == NULL) return NIL_VAL;
    char* s = duckdb_value_varchar(result, (idx_t)col, (idx_t)row);
    if (s == NULL) return NIL_VAL;
    Value v = OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
    duckdb_free(s);
    return v;
}

Value owl_duckdb_cell_to_owl(duckdb_vector vec, uint64_t row,
                              duckdb_type type,
                              duckdb_result* result, uint64_t col) {
    uint64_t* validity = duckdb_vector_get_validity(vec);
    if (!row_is_valid(validity, row)) return NIL_VAL;

    switch (type) {
        case DUCKDB_TYPE_BOOLEAN: {
            bool* arr = (bool*)duckdb_vector_get_data(vec);
            return BOOL_VAL(arr[row]);
        }
        case DUCKDB_TYPE_TINYINT: {
            int8_t* arr = (int8_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_SMALLINT: {
            int16_t* arr = (int16_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_INTEGER: {
            int32_t* arr = (int32_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_BIGINT: {
            int64_t* arr = (int64_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL(arr[row]);
        }
        case DUCKDB_TYPE_UTINYINT: {
            uint8_t* arr = (uint8_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_USMALLINT: {
            uint16_t* arr = (uint16_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_UINTEGER: {
            uint32_t* arr = (uint32_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_UBIGINT: {
            uint64_t* arr = (uint64_t*)duckdb_vector_get_data(vec);
            return INTEGER_VAL((int64_t)arr[row]);
        }
        case DUCKDB_TYPE_FLOAT: {
            float* arr = (float*)duckdb_vector_get_data(vec);
            return REAL_VAL((double)arr[row]);
        }
        case DUCKDB_TYPE_DOUBLE: {
            double* arr = (double*)duckdb_vector_get_data(vec);
            return REAL_VAL(arr[row]);
        }
        case DUCKDB_TYPE_VARCHAR:
            return extract_varchar(vec, row);
        case DUCKDB_TYPE_BLOB:
        case DUCKDB_TYPE_BIT:
            return extract_blob(vec, row);
        case DUCKDB_TYPE_DATE:
            return extract_date(vec, row);
        case DUCKDB_TYPE_TIME:
        case DUCKDB_TYPE_TIME_TZ:
            return extract_time(vec, row);
        case DUCKDB_TYPE_TIMESTAMP:
        case DUCKDB_TYPE_TIMESTAMP_S:
        case DUCKDB_TYPE_TIMESTAMP_MS:
        case DUCKDB_TYPE_TIMESTAMP_NS:
        case DUCKDB_TYPE_TIMESTAMP_TZ:
            return extract_timestamp(vec, row);

        /* Types complexes : fallback stringify. */
        case DUCKDB_TYPE_DECIMAL:
        case DUCKDB_TYPE_HUGEINT:
        case DUCKDB_TYPE_UHUGEINT:
        case DUCKDB_TYPE_INTERVAL:
        case DUCKDB_TYPE_UUID:
        case DUCKDB_TYPE_ENUM:
        case DUCKDB_TYPE_LIST:
        case DUCKDB_TYPE_STRUCT:
        case DUCKDB_TYPE_MAP:
        case DUCKDB_TYPE_ARRAY:
        case DUCKDB_TYPE_UNION:
            return fallback_value_varchar(result, col, row);

        case DUCKDB_TYPE_INVALID:
        case DUCKDB_TYPE_SQLNULL:
        default:
            return fallback_value_varchar(result, col, row);
    }
}
