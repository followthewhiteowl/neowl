// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_odbc_query.h"
#include "sql_odbc_loader.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if OWL_HAS_ODBC

#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

/* ================================================================
 * Helpers UTF conversion (idem sql_odbc_conn.c, en static local pour
 * eviter d'exposer dans le header)
 * ================================================================ */

static size_t utf8_to_ucs2le(const char* utf8, SQLWCHAR* out, size_t out_cap) {
    if (!utf8) {
        if (out_cap > 0 && out) out[0] = 0;
        return 0;
    }
    size_t written = 0;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p && written + 1 < out_cap) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0) {
            cp = (uint32_t)(*p++ & 0x1F) << 6;
            if ((*p & 0xC0) != 0x80) { cp = '?'; continue; }
            cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (uint32_t)(*p++ & 0x0F) << 12;
            if ((*p & 0xC0) != 0x80) { cp = '?'; continue; }
            cp |= (uint32_t)(*p++ & 0x3F) << 6;
            if ((*p & 0xC0) != 0x80) { cp = '?'; continue; }
            cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF8) == 0xF0) {
            p++;
            if ((*p & 0xC0) == 0x80) p++;
            if ((*p & 0xC0) == 0x80) p++;
            if ((*p & 0xC0) == 0x80) p++;
            cp = '?';
        } else { p++; cp = '?'; }
        if (cp > 0xFFFF) cp = '?';
        out[written++] = (SQLWCHAR)cp;
    }
    if (out_cap > 0 && out) out[written] = 0;
    return written;
}

static size_t ucs2le_to_utf8(const SQLWCHAR* src, size_t n_units,
                               char* out, size_t out_cap) {
    if (out_cap == 0) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n_units && o + 4 < out_cap; ++i) {
        uint16_t cp = (uint16_t)src[i];
        if (cp == 0) break;
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
    out[o < out_cap ? o : out_cap - 1] = '\0';
    return o;
}

static char* ucs2le_to_utf8_alloc(const SQLWCHAR* src, size_t n_units) {
    size_t cap = n_units * 3 + 4;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    ucs2le_to_utf8(src, n_units, out, cap);
    return out;
}

/* ================================================================
 * Diag : memes pattern que sql_odbc_conn.c, sur un statement handle.
 * Toutes les fonctions internes recoivent maintenant le vt en parametre.
 * ================================================================ */

/* Mappe un SQLRETURN sur une chaine humaine pour les cas ou le driver
 * ne remplit pas de diag record (SQL_INVALID_HANDLE, SQL_NO_DATA,
 * codes non-standard...). */
static const char* sqlret_label(SQLRETURN rc) {
    switch (rc) {
        case SQL_SUCCESS:           return "SQL_SUCCESS(0)";
        case SQL_SUCCESS_WITH_INFO: return "SQL_SUCCESS_WITH_INFO(1)";
        case SQL_NO_DATA:           return "SQL_NO_DATA(100)";
        case SQL_ERROR:             return "SQL_ERROR(-1)";
        case SQL_INVALID_HANDLE:    return "SQL_INVALID_HANDLE(-2)";
        case SQL_STILL_EXECUTING:   return "SQL_STILL_EXECUTING(2)";
        case SQL_NEED_DATA:         return "SQL_NEED_DATA(99)";
        default:                    return "rc inconnu";
    }
}

static void result_err_diag(const OwlOdbcVT* vt, SQLRETURN orig_rc,
                              OwlOdbcResult* r, const char* prefix,
                              SQLSMALLINT handle_type, SQLHANDLE handle) {
    if (!vt || !vt->GetDiagRecW) {
        snprintf(r->error_message, sizeof(r->error_message),
                 "%s: rc=%d (%s), pas de VT pour diag",
                 prefix, (int)orig_rc, sqlret_label(orig_rc));
        return;
    }
    /* Itere jusqu'a 3 diag records pour les drivers verbeux (HFSQL en
     * particulier empile parfois plusieurs records). */
    char accum[480];
    size_t accum_off = 0;
    accum[0] = '\0';

    SQLWCHAR sql_state[6];
    SQLWCHAR msg[400];
    SQLINTEGER native_err = 0;
    SQLSMALLINT msg_len = 0;
    bool got_any = false;
    for (SQLSMALLINT rec = 1; rec <= 3; ++rec) {
        SQLRETURN rc = vt->GetDiagRecW(handle_type, handle, rec,
                                        sql_state, &native_err,
                                        msg, (SQLSMALLINT)(sizeof(msg)/sizeof(msg[0])),
                                        &msg_len);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) break;
        got_any = true;
        char state_utf8[16];
        char msg_utf8[300];
        ucs2le_to_utf8(sql_state, 5, state_utf8, sizeof(state_utf8));
        ucs2le_to_utf8(msg, msg_len > 0 ? (size_t)msg_len : 0,
                        msg_utf8, sizeof(msg_utf8));
        int n = snprintf(accum + accum_off, sizeof(accum) - accum_off,
                         "%s[%s] %ld %s",
                         rec == 1 ? "" : " | ",
                         state_utf8, (long)native_err, msg_utf8);
        if (n < 0) break;
        accum_off += (size_t)n;
        if (accum_off >= sizeof(accum) - 1) break;
    }
    if (got_any) {
        snprintf(r->error_message, sizeof(r->error_message),
                 "%s: rc=%d (%s) %s", prefix,
                 (int)orig_rc, sqlret_label(orig_rc), accum);
    } else {
        snprintf(r->error_message, sizeof(r->error_message),
                 "%s: rc=%d (%s), driver muet (pas de diag record)",
                 prefix, (int)orig_rc, sqlret_label(orig_rc));
    }
}

/* ================================================================
 * Init / free
 * ================================================================ */

void owl_odbc_result_init(OwlOdbcResult* r) {
    memset(r, 0, sizeof(*r));
    r->success = false;
}

void owl_odbc_result_free(OwlOdbcResult* r) {
    if (r->cells) {
        size_t total = r->row_count * r->column_count;
        for (size_t i = 0; i < total; ++i) {
            free(r->cells[i].s_val);
            free(r->cells[i].bytes);
        }
        free(r->cells);
    }
    free(r->columns);
    memset(r, 0, sizeof(*r));
}

/* ================================================================
 * Fetch wide string : SQLGetData en boucle dynamic grow.
 * ================================================================ */
static char* fetch_string_utf8(const OwlOdbcVT* vt,
                                 SQLHSTMT hstmt, SQLUSMALLINT col_idx,
                                 size_t* len_out, bool* is_null,
                                 OwlOdbcResult* r) {
    *is_null = false;
    *len_out = 0;
    size_t cap_units = 1024;
    SQLWCHAR* buf = (SQLWCHAR*)malloc(cap_units * sizeof(SQLWCHAR));
    if (!buf) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM fetch string");
        return NULL;
    }
    size_t accumulated_units = 0;

    while (1) {
        SQLLEN ind = 0;
        SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_WCHAR,
                                    buf + accumulated_units,
                                    (SQLLEN)((cap_units - accumulated_units) * sizeof(SQLWCHAR)),
                                    &ind);
        /* SQL_NO_DATA = fin de stream selon spec ODBC. HFSQL le renvoie
         * en particulier des le 1er appel si la valeur est vide ou si
         * la cellule a deja ete consommee par un fetch precedent. On
         * accepte la donnee accumulee jusqu'ici. */
        if (rc == SQL_NO_DATA) break;
        /* NULL detection : ind == SQL_NULL_DATA (et seulement ind, pas rc :
         * SQL_NULL_DATA == -1 == SQL_ERROR donc rc==SQL_NULL_DATA est faux
         * et masque les vraies erreurs). */
        if (ind == SQL_NULL_DATA) {
            *is_null = true;
            free(buf);
            return NULL;
        }
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            result_err_diag(vt, rc, r, "SQLGetData (string)", SQL_HANDLE_STMT, hstmt);
            free(buf);
            return NULL;
        }

        size_t bytes_avail = (cap_units - accumulated_units) * sizeof(SQLWCHAR);
        size_t consumed_bytes;
        if (ind == SQL_NO_TOTAL || (size_t)ind >= bytes_avail) {
            consumed_bytes = bytes_avail - sizeof(SQLWCHAR);
        } else {
            consumed_bytes = (size_t)ind;
        }
        accumulated_units += consumed_bytes / sizeof(SQLWCHAR);

        if (rc == SQL_SUCCESS) break;
        if (ind != SQL_NO_TOTAL && (size_t)ind < bytes_avail) break;

        size_t new_cap = cap_units * 2;
        SQLWCHAR* nb = (SQLWCHAR*)realloc(buf, new_cap * sizeof(SQLWCHAR));
        if (!nb) {
            snprintf(r->error_message, sizeof(r->error_message), "OOM grow string");
            free(buf);
            return NULL;
        }
        buf = nb;
        cap_units = new_cap;
    }

    char* utf8 = ucs2le_to_utf8_alloc(buf, accumulated_units);
    free(buf);
    if (!utf8) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM convert utf8");
        return NULL;
    }
    *len_out = strlen(utf8);
    return utf8;
}

static char* fetch_char_utf8(const OwlOdbcVT* vt,
                               SQLHSTMT hstmt, SQLUSMALLINT col_idx,
                               size_t* len_out, bool* is_null,
                               OwlOdbcResult* r) {
    *is_null = false;
    *len_out = 0;
    size_t cap = 256;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM fetch char");
        return NULL;
    }
    size_t accumulated = 0;
    while (1) {
        SQLLEN ind = 0;
        SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_CHAR,
                                    buf + accumulated,
                                    (SQLLEN)(cap - accumulated),
                                    &ind);
        if (rc == SQL_NO_DATA) break;
        if (ind == SQL_NULL_DATA) {
            *is_null = true;
            free(buf);
            return NULL;
        }
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            result_err_diag(vt, rc, r, "SQLGetData (char)", SQL_HANDLE_STMT, hstmt);
            free(buf);
            return NULL;
        }
        size_t avail = cap - accumulated;
        size_t consumed;
        if (ind == SQL_NO_TOTAL || (size_t)ind >= avail) {
            consumed = avail - 1;
        } else {
            consumed = (size_t)ind;
        }
        accumulated += consumed;
        if (rc == SQL_SUCCESS) break;
        size_t new_cap = cap * 2;
        char* nb = (char*)realloc(buf, new_cap);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
        cap = new_cap;
    }
    buf[accumulated] = '\0';
    *len_out = accumulated;
    return buf;
}

static uint8_t* fetch_binary(const OwlOdbcVT* vt,
                              SQLHSTMT hstmt, SQLUSMALLINT col_idx,
                              size_t* len_out, bool* is_null,
                              OwlOdbcResult* r) {
    *is_null = false;
    *len_out = 0;
    size_t cap = 1024;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM fetch binary");
        return NULL;
    }
    size_t accumulated = 0;
    while (1) {
        SQLLEN ind = 0;
        SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_BINARY,
                                    buf + accumulated,
                                    (SQLLEN)(cap - accumulated),
                                    &ind);
        if (rc == SQL_NO_DATA) break;
        if (ind == SQL_NULL_DATA) {
            *is_null = true;
            free(buf);
            return NULL;
        }
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            result_err_diag(vt, rc, r, "SQLGetData (binary)", SQL_HANDLE_STMT, hstmt);
            free(buf);
            return NULL;
        }
        size_t avail = cap - accumulated;
        size_t consumed;
        if (ind == SQL_NO_TOTAL || (size_t)ind > avail) {
            consumed = avail;
        } else {
            consumed = (size_t)ind;
        }
        accumulated += consumed;
        if (rc == SQL_SUCCESS) break;
        size_t new_cap = cap * 2;
        uint8_t* nb = (uint8_t*)realloc(buf, new_cap);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
        cap = new_cap;
    }
    *len_out = accumulated;
    return buf;
}

/* ================================================================
 * Fetch d'une cellule selon le SQL type
 * ================================================================ */

static void format_date(SQL_DATE_STRUCT* d, char* out, size_t cap) {
    snprintf(out, cap, "%04d-%02d-%02d",
             (int)d->year, (int)d->month, (int)d->day);
}
static void format_time(SQL_TIME_STRUCT* t, char* out, size_t cap) {
    snprintf(out, cap, "%02d:%02d:%02d",
             (int)t->hour, (int)t->minute, (int)t->second);
}
static void format_timestamp(SQL_TIMESTAMP_STRUCT* ts, char* out, size_t cap) {
    if (ts->fraction == 0) {
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
                 (int)ts->year, (int)ts->month, (int)ts->day,
                 (int)ts->hour, (int)ts->minute, (int)ts->second);
    } else {
        unsigned ms = (unsigned)(ts->fraction / 1000000);
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                 (int)ts->year, (int)ts->month, (int)ts->day,
                 (int)ts->hour, (int)ts->minute, (int)ts->second, ms);
    }
}

static bool fetch_cell(const OwlOdbcVT* vt,
                        SQLHSTMT hstmt, SQLUSMALLINT col_idx,
                        SQLSMALLINT sql_type, OwlOdbcCell* cell,
                        OwlOdbcResult* r) {
    cell->kind = OWL_ODBC_VAL_NULL;
    bool is_null = false;

    switch (sql_type) {
        case SQL_INTEGER:
        case SQL_SMALLINT:
        case SQL_TINYINT:
        case SQL_BIGINT: {
            SQLBIGINT v;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_SBIGINT,
                                        &v, sizeof(v), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData int", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; }
            else { cell->kind = OWL_ODBC_VAL_INTEGER; cell->i_val = (int64_t)v; }
            return true;
        }
        case SQL_REAL:
        case SQL_FLOAT:
        case SQL_DOUBLE: {
            double v;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_DOUBLE,
                                        &v, sizeof(v), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData real", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; }
            else { cell->kind = OWL_ODBC_VAL_REAL; cell->r_val = v; }
            return true;
        }
        case SQL_BIT: {
            SQLCHAR v;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_BIT,
                                        &v, sizeof(v), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData bit", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; }
            else { cell->kind = OWL_ODBC_VAL_BOOL; cell->b_val = (v != 0); }
            return true;
        }
        case SQL_DECIMAL:
        case SQL_NUMERIC: {
            size_t len = 0;
            char* s = fetch_char_utf8(vt, hstmt, col_idx, &len, &is_null, r);
            if (is_null) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            if (!s) return false;
            cell->kind = OWL_ODBC_VAL_STRING_UTF8;
            cell->s_val = s;
            cell->bytes_len = len;
            return true;
        }
        case SQL_TYPE_DATE:
        case SQL_DATE: {
            SQL_DATE_STRUCT d;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_TYPE_DATE,
                                        &d, sizeof(d), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData date", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            char buf[32];
            format_date(&d, buf, sizeof(buf));
            cell->kind = OWL_ODBC_VAL_STRING_UTF8;
            cell->s_val = (char*)malloc(strlen(buf) + 1);
            if (!cell->s_val) return false;
            strcpy(cell->s_val, buf);
            cell->bytes_len = strlen(buf);
            return true;
        }
        case SQL_TYPE_TIME:
        case SQL_TIME: {
            SQL_TIME_STRUCT t;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_TYPE_TIME,
                                        &t, sizeof(t), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData time", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            char buf[32];
            format_time(&t, buf, sizeof(buf));
            cell->kind = OWL_ODBC_VAL_STRING_UTF8;
            cell->s_val = (char*)malloc(strlen(buf) + 1);
            if (!cell->s_val) return false;
            strcpy(cell->s_val, buf);
            cell->bytes_len = strlen(buf);
            return true;
        }
        case SQL_TYPE_TIMESTAMP:
        case SQL_TIMESTAMP: {
            SQL_TIMESTAMP_STRUCT ts;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_TYPE_TIMESTAMP,
                                        &ts, sizeof(ts), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData timestamp", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            char buf[40];
            format_timestamp(&ts, buf, sizeof(buf));
            cell->kind = OWL_ODBC_VAL_STRING_UTF8;
            cell->s_val = (char*)malloc(strlen(buf) + 1);
            if (!cell->s_val) return false;
            strcpy(cell->s_val, buf);
            cell->bytes_len = strlen(buf);
            return true;
        }
        case SQL_GUID: {
            SQLGUID g;
            SQLLEN ind = 0;
            SQLRETURN rc = vt->GetData(hstmt, col_idx, SQL_C_GUID,
                                        &g, sizeof(g), &ind);
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                result_err_diag(vt, rc, r, "SQLGetData guid", SQL_HANDLE_STMT, hstmt);
                return false;
            }
            if (ind == SQL_NULL_DATA) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            char buf[40];
            snprintf(buf, sizeof(buf),
                "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                (unsigned long)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
            cell->kind = OWL_ODBC_VAL_STRING_UTF8;
            cell->s_val = (char*)malloc(strlen(buf) + 1);
            if (!cell->s_val) return false;
            strcpy(cell->s_val, buf);
            cell->bytes_len = strlen(buf);
            return true;
        }
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY: {
            size_t len = 0;
            uint8_t* b = fetch_binary(vt, hstmt, col_idx, &len, &is_null, r);
            if (is_null) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            if (!b && len == 0) {
                cell->kind = OWL_ODBC_VAL_BUFFER;
                cell->bytes = NULL;
                cell->bytes_len = 0;
                return true;
            }
            if (!b) return false;
            cell->kind = OWL_ODBC_VAL_BUFFER;
            cell->bytes = b;
            cell->bytes_len = len;
            return true;
        }
        case SQL_VARCHAR:
        case SQL_CHAR:
        case SQL_LONGVARCHAR:
        case SQL_WVARCHAR:
        case SQL_WCHAR:
        case SQL_WLONGVARCHAR:
        default: {
            size_t len = 0;
            char* s = fetch_string_utf8(vt, hstmt, col_idx, &len, &is_null, r);
            if (is_null) { cell->kind = OWL_ODBC_VAL_NULL; return true; }
            if (!s) return false;
            cell->kind = OWL_ODBC_VAL_STRING_UTF8;
            cell->s_val = s;
            cell->bytes_len = len;
            return true;
        }
    }
}

/* ================================================================
 * Bind d'un parametre via SQLBindParameter
 * ================================================================ */
typedef struct {
    SQLBIGINT  ibuf;
    SQLDOUBLE  rbuf;
    SQLCHAR    bbuf;
    SQLLEN     ind;
    SQLWCHAR*  wide_buf;
    size_t     wide_units;
} BindStorage;

static bool bind_one_param(const OwlOdbcVT* vt,
                             SQLHSTMT hstmt, SQLUSMALLINT idx_1based,
                             const OwlOdbcParam* p, BindStorage* st,
                             OwlOdbcResult* r) {
    memset(st, 0, sizeof(*st));
    SQLRETURN rc;
    switch (p->kind) {
        case OWL_ODBC_PARAM_NULL:
            st->ind = SQL_NULL_DATA;
            rc = vt->BindParameter(hstmt, idx_1based, SQL_PARAM_INPUT,
                                    SQL_C_CHAR, SQL_VARCHAR, 1, 0,
                                    &st->bbuf, 0, &st->ind);
            break;
        case OWL_ODBC_PARAM_INTEGER:
            st->ibuf = (SQLBIGINT)p->i_val;
            st->ind = sizeof(SQLBIGINT);
            rc = vt->BindParameter(hstmt, idx_1based, SQL_PARAM_INPUT,
                                    SQL_C_SBIGINT, SQL_BIGINT,
                                    19, 0,
                                    &st->ibuf, sizeof(SQLBIGINT), &st->ind);
            break;
        case OWL_ODBC_PARAM_REAL:
            st->rbuf = (SQLDOUBLE)p->r_val;
            st->ind = sizeof(SQLDOUBLE);
            rc = vt->BindParameter(hstmt, idx_1based, SQL_PARAM_INPUT,
                                    SQL_C_DOUBLE, SQL_DOUBLE,
                                    15, 0,
                                    &st->rbuf, sizeof(SQLDOUBLE), &st->ind);
            break;
        case OWL_ODBC_PARAM_BOOL:
            st->bbuf = p->b_val ? 1 : 0;
            st->ind = 1;
            rc = vt->BindParameter(hstmt, idx_1based, SQL_PARAM_INPUT,
                                    SQL_C_BIT, SQL_BIT,
                                    1, 0,
                                    &st->bbuf, 1, &st->ind);
            break;
        case OWL_ODBC_PARAM_STRING_UTF8: {
            size_t in_len = p->bytes_len;
            size_t cap = in_len + 1;
            SQLWCHAR* w = (SQLWCHAR*)malloc(cap * sizeof(SQLWCHAR));
            if (!w) {
                snprintf(r->error_message, sizeof(r->error_message),
                         "OOM bind string");
                return false;
            }
            const unsigned char* pp = p->bytes;
            size_t consumed = 0;
            size_t written = 0;
            while (consumed < in_len && written < cap - 1) {
                uint32_t cp;
                if (pp[consumed] < 0x80) { cp = pp[consumed++]; }
                else if ((pp[consumed] & 0xE0) == 0xC0 && consumed + 1 < in_len) {
                    cp = (uint32_t)(pp[consumed] & 0x1F) << 6;
                    cp |= (pp[consumed+1] & 0x3F);
                    consumed += 2;
                } else if ((pp[consumed] & 0xF0) == 0xE0 && consumed + 2 < in_len) {
                    cp = (uint32_t)(pp[consumed] & 0x0F) << 12;
                    cp |= (uint32_t)(pp[consumed+1] & 0x3F) << 6;
                    cp |= (pp[consumed+2] & 0x3F);
                    consumed += 3;
                } else if ((pp[consumed] & 0xF8) == 0xF0 && consumed + 3 < in_len) {
                    consumed += 4;
                    cp = '?';
                } else { consumed += 1; cp = '?'; }
                if (cp > 0xFFFF) cp = '?';
                w[written++] = (SQLWCHAR)cp;
            }
            w[written] = 0;
            st->wide_buf = w;
            st->wide_units = written;
            st->ind = (SQLLEN)(written * sizeof(SQLWCHAR));
            SQLULEN col_size = written > 0 ? written : 1;
            rc = vt->BindParameter(hstmt, idx_1based, SQL_PARAM_INPUT,
                                    SQL_C_WCHAR, SQL_WVARCHAR,
                                    col_size, 0,
                                    w, (SQLLEN)((cap) * sizeof(SQLWCHAR)),
                                    &st->ind);
            break;
        }
        case OWL_ODBC_PARAM_BUFFER:
            st->ind = (SQLLEN)p->bytes_len;
            rc = vt->BindParameter(hstmt, idx_1based, SQL_PARAM_INPUT,
                                    SQL_C_BINARY, SQL_VARBINARY,
                                    p->bytes_len > 0 ? p->bytes_len : 1, 0,
                                    (SQLPOINTER)(uintptr_t)p->bytes,
                                    (SQLLEN)p->bytes_len, &st->ind);
            break;
        default:
            snprintf(r->error_message, sizeof(r->error_message),
                     "bind: param kind invalide");
            return false;
    }
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        result_err_diag(vt, rc, r, "SQLBindParameter", SQL_HANDLE_STMT, hstmt);
        free(st->wide_buf);
        return false;
    }
    return true;
}

static bool materialize_result(const OwlOdbcVT* vt,
                                SQLHSTMT hstmt, OwlOdbcResult* r) {
    SQLLEN row_count_dml = 0;
    vt->RowCount(hstmt, &row_count_dml);
    r->rows_affected = (int64_t)row_count_dml;

    SQLSMALLINT n_cols = 0;
    vt->NumResultCols(hstmt, &n_cols);
    if (n_cols == 0) {
        r->success = true;
        return true;
    }

    r->columns = (OwlOdbcColumn*)calloc((size_t)n_cols, sizeof(OwlOdbcColumn));
    if (!r->columns) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM columns");
        return false;
    }
    r->column_count = (size_t)n_cols;

    for (SQLSMALLINT i = 0; i < n_cols; ++i) {
        SQLWCHAR    name_w[128];
        SQLSMALLINT name_len = 0;
        SQLSMALLINT sql_type_dyn = 0;
        SQLULEN     col_size = 0;
        SQLSMALLINT dec_dig = 0;
        SQLSMALLINT nullable = 0;
        SQLRETURN rc = vt->DescribeColW(hstmt, (SQLUSMALLINT)(i + 1),
                                         name_w, (SQLSMALLINT)(sizeof(name_w)/sizeof(name_w[0])),
                                         &name_len, &sql_type_dyn,
                                         &col_size, &dec_dig, &nullable);
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
            result_err_diag(vt, rc, r, "SQLDescribeCol", SQL_HANDLE_STMT, hstmt);
            return false;
        }
        ucs2le_to_utf8(name_w, name_len > 0 ? (size_t)name_len : 0,
                        r->columns[i].name, sizeof(r->columns[i].name));
        r->columns[i].sql_type       = (int16_t)sql_type_dyn;
        r->columns[i].column_size    = (uint32_t)col_size;
        r->columns[i].decimal_digits = (int16_t)dec_dig;
        r->columns[i].nullable       = (nullable == SQL_NULLABLE);
    }

    size_t row_cap = 16;
    r->cells = (OwlOdbcCell*)calloc(row_cap * (size_t)n_cols, sizeof(OwlOdbcCell));
    if (!r->cells) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM cells");
        return false;
    }
    SQLRETURN rc;
    while ((rc = vt->Fetch(hstmt)) == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        if (r->row_count == row_cap) {
            size_t new_cap = row_cap * 2;
            OwlOdbcCell* nc = (OwlOdbcCell*)realloc(r->cells,
                new_cap * (size_t)n_cols * sizeof(OwlOdbcCell));
            if (!nc) {
                snprintf(r->error_message, sizeof(r->error_message),
                         "OOM cells grow");
                return false;
            }
            memset(nc + row_cap * (size_t)n_cols, 0,
                    (new_cap - row_cap) * (size_t)n_cols * sizeof(OwlOdbcCell));
            r->cells = nc;
            row_cap = new_cap;
        }
        OwlOdbcCell* row = r->cells + r->row_count * (size_t)n_cols;
        for (SQLSMALLINT i = 0; i < n_cols; ++i) {
            if (!fetch_cell(vt, hstmt, (SQLUSMALLINT)(i + 1),
                              r->columns[i].sql_type, &row[i], r)) {
                return false;
            }
        }
        r->row_count++;
    }
    if (rc != SQL_NO_DATA && rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        result_err_diag(vt, rc, r, "SQLFetch", SQL_HANDLE_STMT, hstmt);
        return false;
    }
    r->success = true;
    return true;
}

bool owl_odbc_simple_query_params(OwlOdbcConn* c, const char* sql_utf8,
                                     const OwlOdbcParam* params, size_t n_params,
                                     OwlOdbcResult* r) {
    if (!c->connected) {
        snprintf(r->error_message, sizeof(r->error_message),
                 "ODBC query: connexion non ouverte");
        return false;
    }
    const OwlOdbcVT* vt = c->vt;
    if (!vt) {
        snprintf(r->error_message, sizeof(r->error_message),
                 "ODBC query: VT non initialisee (conn ouverte incorrectement ?)");
        return false;
    }

    SQLHSTMT hstmt = NULL;
    SQLRETURN rc = vt->AllocHandle(SQL_HANDLE_STMT, (SQLHDBC)c->hdbc, &hstmt);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        result_err_diag(vt, rc, r, "SQLAllocHandle(STMT)", SQL_HANDLE_DBC, (SQLHDBC)c->hdbc);
        return false;
    }

    size_t sql_len = strlen(sql_utf8);
    SQLWCHAR* sql_wide = (SQLWCHAR*)malloc((sql_len + 1) * sizeof(SQLWCHAR));
    if (!sql_wide) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM sql wide");
        vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
        return false;
    }
    utf8_to_ucs2le(sql_utf8, sql_wide, sql_len + 1);
    rc = vt->PrepareW(hstmt, sql_wide, SQL_NTS);
    free(sql_wide);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        result_err_diag(vt, rc, r, "SQLPrepare", SQL_HANDLE_STMT, hstmt);
        vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
        return false;
    }

    BindStorage* storage = NULL;
    if (n_params > 0) {
        storage = (BindStorage*)calloc(n_params, sizeof(BindStorage));
        if (!storage) {
            snprintf(r->error_message, sizeof(r->error_message), "OOM storage");
            vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
            return false;
        }
        for (size_t i = 0; i < n_params; ++i) {
            if (!bind_one_param(vt, hstmt, (SQLUSMALLINT)(i + 1),
                                  &params[i], &storage[i], r)) {
                for (size_t j = 0; j <= i; ++j) free(storage[j].wide_buf);
                free(storage);
                vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
                return false;
            }
        }
    }

    rc = vt->Execute(hstmt);
    bool ok = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO || rc == SQL_NO_DATA);
    if (!ok) {
        result_err_diag(vt, rc, r, "SQLExecute", SQL_HANDLE_STMT, hstmt);
    } else {
        ok = materialize_result(vt, hstmt, r);
    }

    for (size_t i = 0; i < n_params; ++i) free(storage[i].wide_buf);
    free(storage);

    vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
    return ok;
}

bool owl_odbc_simple_query(OwlOdbcConn* c, const char* sql_utf8,
                             OwlOdbcResult* r) {
    if (!c->connected) {
        snprintf(r->error_message, sizeof(r->error_message),
                 "ODBC query: connexion non ouverte");
        return false;
    }
    const OwlOdbcVT* vt = c->vt;
    if (!vt) {
        snprintf(r->error_message, sizeof(r->error_message),
                 "ODBC query: VT non initialisee");
        return false;
    }

    SQLHSTMT hstmt = NULL;
    SQLRETURN rc = vt->AllocHandle(SQL_HANDLE_STMT, (SQLHDBC)c->hdbc, &hstmt);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        result_err_diag(vt, rc, r, "SQLAllocHandle(STMT)", SQL_HANDLE_DBC, (SQLHDBC)c->hdbc);
        return false;
    }

    size_t sql_len = strlen(sql_utf8);
    SQLWCHAR* sql_wide = (SQLWCHAR*)malloc((sql_len + 1) * sizeof(SQLWCHAR));
    if (!sql_wide) {
        snprintf(r->error_message, sizeof(r->error_message), "OOM sql wide");
        vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
        return false;
    }
    utf8_to_ucs2le(sql_utf8, sql_wide, sql_len + 1);
    rc = vt->ExecDirectW(hstmt, sql_wide, SQL_NTS);
    free(sql_wide);

    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA) {
        result_err_diag(vt, rc, r, "SQLExecDirect", SQL_HANDLE_STMT, hstmt);
        vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
        return false;
    }

    bool ok = materialize_result(vt, hstmt, r);
    vt->FreeHandle(SQL_HANDLE_STMT, hstmt);
    return ok;
}

#else /* OWL_HAS_ODBC == 0 */

void owl_odbc_result_init(OwlOdbcResult* r) {
    memset(r, 0, sizeof(*r));
}
void owl_odbc_result_free(OwlOdbcResult* r) {
    (void)r;
    memset(r, 0, sizeof(*r));
}
bool owl_odbc_simple_query(OwlOdbcConn* c, const char* sql_utf8,
                             OwlOdbcResult* r) {
    (void)c; (void)sql_utf8;
    snprintf(r->error_message, sizeof(r->error_message),
             "ODBC: pas encore supporte sur cet OS");
    return false;
}
bool owl_odbc_simple_query_params(OwlOdbcConn* c, const char* sql_utf8,
                                     const OwlOdbcParam* params, size_t n_params,
                                     OwlOdbcResult* r) {
    (void)c; (void)sql_utf8; (void)params; (void)n_params;
    snprintf(r->error_message, sizeof(r->error_message),
             "ODBC: pas encore supporte sur cet OS");
    return false;
}

#endif
