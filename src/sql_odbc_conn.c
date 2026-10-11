// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_odbc_conn.h"
#include "sql_odbc_loader.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#if OWL_HAS_ODBC

/* Windows : sqltypes.h utilise les types Win32 (HWND, DWORD, INT64...).
 * Inclure windows.h en premier sinon "unknown type name" cascades. */
#ifdef _WIN32
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>

/* ================================================================
 * Helpers UTF-8 <-> UTF-16 (SQLWCHAR)
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
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
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
        } else {
            p++;
            cp = '?';
        }
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

/* ================================================================
 * Diagnostic via SQLGetDiagRecW (passe par la VT)
 * ================================================================ */

static void odbc_err(OwlOdbcConn* c, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->last_error, sizeof(c->last_error), fmt, ap);
    va_end(ap);
}

static void odbc_err_diag(OwlOdbcConn* c, const char* prefix,
                            SQLSMALLINT handle_type, SQLHANDLE handle) {
    if (!c->vt || !c->vt->GetDiagRecW) {
        snprintf(c->last_error, sizeof(c->last_error),
                 "%s: erreur ODBC (pas de VT pour diag)", prefix);
        return;
    }
    SQLWCHAR  sql_state[6];
    SQLWCHAR  msg[512];
    SQLINTEGER native_err = 0;
    SQLSMALLINT msg_len = 0;
    SQLRETURN rc = c->vt->GetDiagRecW(handle_type, handle, 1,
                                       sql_state, &native_err,
                                       msg, (SQLSMALLINT)(sizeof(msg)/sizeof(msg[0])),
                                       &msg_len);
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        char state_utf8[16];
        char msg_utf8[512];
        ucs2le_to_utf8(sql_state, 5, state_utf8, sizeof(state_utf8));
        ucs2le_to_utf8(msg, msg_len > 0 ? (size_t)msg_len : 0,
                        msg_utf8, sizeof(msg_utf8));
        snprintf(c->last_error, sizeof(c->last_error),
                 "%s: [%s] %ld %s", prefix, state_utf8,
                 (long)native_err, msg_utf8);
    } else {
        snprintf(c->last_error, sizeof(c->last_error),
                 "%s: erreur ODBC sans diagnostic", prefix);
    }
}

/* ================================================================
 * API publique
 * ================================================================ */

void owl_odbc_conn_init(OwlOdbcConn* c) {
    memset(c, 0, sizeof(*c));
}

bool owl_odbc_conn_open(OwlOdbcConn* c,
                          const struct OwlOdbcVT* vt,
                          const char* conn_str_utf8,
                          int timeout_s) {
    if (!conn_str_utf8 || !conn_str_utf8[0]) {
        odbc_err(c, "SQLConnexionOuvre (ODBC): Source (connection string) vide");
        return false;
    }
    if (!vt) {
        odbc_err(c, "SQLConnexionOuvre (ODBC): VT non fournie par le caller");
        return false;
    }
    c->vt = vt;

    SQLHENV henv = NULL;
    SQLHDBC hdbc = NULL;
    SQLRETURN rc;

    /* 1. Allocate environment handle. */
    rc = c->vt->AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        odbc_err(c, "SQLAllocHandle(ENV) echec");
        return false;
    }

    /* 2. Set ODBC version 3.x. */
    rc = c->vt->SetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                            (SQLPOINTER)SQL_OV_ODBC3, 0);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        odbc_err_diag(c, "SQLSetEnvAttr(ODBC_VERSION)", SQL_HANDLE_ENV, henv);
        c->vt->FreeHandle(SQL_HANDLE_ENV, henv);
        return false;
    }

    /* 3. Allocate connection handle. */
    rc = c->vt->AllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        odbc_err_diag(c, "SQLAllocHandle(DBC)", SQL_HANDLE_ENV, henv);
        c->vt->FreeHandle(SQL_HANDLE_ENV, henv);
        return false;
    }

    /* 4. Connect timeout (en secondes). 0 = pas de timeout. */
    if (timeout_s > 0) {
        rc = c->vt->SetConnectAttrW(hdbc, SQL_ATTR_LOGIN_TIMEOUT,
                                     (SQLPOINTER)(intptr_t)timeout_s, 0);
        /* Pas fatal si le driver ne supporte pas. */
        (void)rc;
    }

    /* 5. Convertit la connection string en UTF-16 + appelle SQLDriverConnectW. */
    size_t cs_len = strlen(conn_str_utf8);
    SQLWCHAR* cs_wide = (SQLWCHAR*)malloc((cs_len + 1) * sizeof(SQLWCHAR));
    if (!cs_wide) {
        odbc_err(c, "SQLConnexionOuvre (ODBC): OOM");
        c->vt->FreeHandle(SQL_HANDLE_DBC, hdbc);
        c->vt->FreeHandle(SQL_HANDLE_ENV, henv);
        return false;
    }
    utf8_to_ucs2le(conn_str_utf8, cs_wide, cs_len + 1);

    SQLWCHAR out_cs[1024];
    SQLSMALLINT out_cs_len = 0;
    rc = c->vt->DriverConnectW(hdbc, NULL,
                                cs_wide, SQL_NTS,
                                out_cs, (SQLSMALLINT)(sizeof(out_cs)/sizeof(out_cs[0])),
                                &out_cs_len,
                                SQL_DRIVER_NOPROMPT);
    free(cs_wide);

    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        odbc_err_diag(c, "SQLDriverConnect", SQL_HANDLE_DBC, hdbc);
        c->vt->FreeHandle(SQL_HANDLE_DBC, hdbc);
        c->vt->FreeHandle(SQL_HANDLE_ENV, henv);
        return false;
    }

    c->henv = (void*)henv;
    c->hdbc = (void*)hdbc;
    c->connected = true;
    c->last_error[0] = '\0';
    return true;
}

void owl_odbc_conn_close(OwlOdbcConn* c) {
    if (!c->vt) {
        /* Pas de VT : rien a faire, handles n'ont jamais ete alloues. */
        c->henv = c->hdbc = NULL;
        c->connected = false;
        return;
    }
    if (c->hdbc) {
        if (c->connected) {
            c->vt->Disconnect((SQLHDBC)c->hdbc);
        }
        c->vt->FreeHandle(SQL_HANDLE_DBC, (SQLHDBC)c->hdbc);
        c->hdbc = NULL;
    }
    if (c->henv) {
        c->vt->FreeHandle(SQL_HANDLE_ENV, (SQLHENV)c->henv);
        c->henv = NULL;
    }
    c->connected = false;
}

const char* owl_odbc_conn_last_error(const OwlOdbcConn* c) {
    return c->last_error;
}

char** owl_odbc_list_drivers(int* out_count) {
    *out_count = 0;

    /* Pour lister les drivers on doit passer par le Driver Manager
     * (SQLDriversW indisponible quand on appelle directement un driver). */
    char err[256];
    const OwlOdbcVT* vt = owl_odbc_vt_load_default(err, sizeof(err));
    if (!vt || !vt->DriversW) return NULL;

    SQLHENV henv = NULL;
    if (vt->AllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv) != SQL_SUCCESS) {
        return NULL;
    }
    if (vt->SetEnvAttr(henv, SQL_ATTR_ODBC_VERSION,
                       (SQLPOINTER)SQL_OV_ODBC3, 0) != SQL_SUCCESS) {
        vt->FreeHandle(SQL_HANDLE_ENV, henv);
        return NULL;
    }

    char** list = NULL;
    int    cap  = 0;
    int    cnt  = 0;
    SQLWCHAR  driver[256];
    SQLWCHAR  attr[1024];
    SQLSMALLINT driver_len = 0, attr_len = 0;
    SQLUSMALLINT direction = SQL_FETCH_FIRST;

    while (vt->DriversW(henv, direction,
                        driver, (SQLSMALLINT)(sizeof(driver)/sizeof(driver[0])),
                        &driver_len,
                        attr, (SQLSMALLINT)(sizeof(attr)/sizeof(attr[0])),
                        &attr_len) == SQL_SUCCESS) {
        direction = SQL_FETCH_NEXT;
        if (cnt >= cap) {
            int new_cap = cap ? cap * 2 : 16;
            char** nl = (char**)realloc(list, new_cap * sizeof(char*));
            if (!nl) break;
            list = nl;
            cap = new_cap;
        }
        char buf[256];
        ucs2le_to_utf8(driver, driver_len > 0 ? (size_t)driver_len : 0,
                        buf, sizeof(buf));
        list[cnt] = (char*)malloc(strlen(buf) + 1);
        if (list[cnt]) {
            strcpy(list[cnt], buf);
            cnt++;
        }
    }

    vt->FreeHandle(SQL_HANDLE_ENV, henv);
    *out_count = cnt;
    return list;
}

#else  /* OWL_HAS_ODBC == 0 */

void owl_odbc_conn_init(OwlOdbcConn* c) {
    memset(c, 0, sizeof(*c));
}

bool owl_odbc_conn_open(OwlOdbcConn* c, const char* conn_str_utf8,
                          int timeout_s) {
    (void)conn_str_utf8; (void)timeout_s;
    snprintf(c->last_error, sizeof(c->last_error),
             "ODBC: pas supporte sur cet OS "
             "(cf docs/public_documentation/14_sql_connecteurs.md, section ODBC)");
    return false;
}

void owl_odbc_conn_close(OwlOdbcConn* c) {
    (void)c;
}

const char* owl_odbc_conn_last_error(const OwlOdbcConn* c) {
    return c->last_error;
}

char** owl_odbc_list_drivers(int* out_count) {
    *out_count = 0;
    return NULL;
}

#endif
