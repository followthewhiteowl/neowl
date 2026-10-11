// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_odbc_loader_h
#define owl_sql_odbc_loader_h

/* ================================================================
 * ODBC vtable — chargement dynamique du Driver Manager ou direct du driver
 *
 * Deux modes :
 *   - default  : LoadLibrary("odbc32.dll") sur Windows (ou libodbc.so.2
 *                via unixODBC vendore plus tard). Mode classique : tous
 *                les drivers HKLM-registered sont accessibles.
 *   - HFSQL direct : LoadLibrary sur la bibliotheque embarquee
 *                directement. Utilise quand le pack sqlHFSQL est extrait
 *                par owl_hfsql_install : chargement direct, pas
 *                d'enregistrement systeme requis.
 *
 * La bibliotheque expose les memes symboles SQLxxxW (SQLAllocHandle,
 * SQLDriverConnectW, SQLExecDirectW, etc.) attendus par la couche ODBC.
 *
 * Les pointeurs sont caches par mode : une seule LoadLibrary par DLL,
 * persistee tout au long du process. Pas de FreeLibrary explicite.
 * ================================================================ */

#include "common.h"

#if OWL_HAS_ODBC

#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

typedef struct OwlOdbcVT {
    void* dll_handle;  /* HMODULE / void* — pour debug */
    const char* source_label;  /* "odbc32.dll" ou le driver ODBC embarque */

    /* Core / connection */
    SQLRETURN (SQL_API *AllocHandle)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
    SQLRETURN (SQL_API *FreeHandle)(SQLSMALLINT, SQLHANDLE);
    SQLRETURN (SQL_API *SetEnvAttr)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
    SQLRETURN (SQL_API *SetConnectAttrW)(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER);
    SQLRETURN (SQL_API *DriverConnectW)(SQLHDBC, SQLHWND, SQLWCHAR*, SQLSMALLINT,
                                         SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                         SQLUSMALLINT);
    SQLRETURN (SQL_API *Disconnect)(SQLHDBC);
    SQLRETURN (SQL_API *GetDiagRecW)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT,
                                      SQLWCHAR*, SQLINTEGER*, SQLWCHAR*,
                                      SQLSMALLINT, SQLSMALLINT*);
    SQLRETURN (SQL_API *EndTran)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT);

    /* Statement / query */
    SQLRETURN (SQL_API *ExecDirectW)(SQLHSTMT, SQLWCHAR*, SQLINTEGER);
    SQLRETURN (SQL_API *PrepareW)(SQLHSTMT, SQLWCHAR*, SQLINTEGER);
    SQLRETURN (SQL_API *Execute)(SQLHSTMT);
    SQLRETURN (SQL_API *BindParameter)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT,
                                        SQLSMALLINT, SQLSMALLINT, SQLULEN,
                                        SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*);
    SQLRETURN (SQL_API *NumResultCols)(SQLHSTMT, SQLSMALLINT*);
    SQLRETURN (SQL_API *DescribeColW)(SQLHSTMT, SQLUSMALLINT, SQLWCHAR*,
                                       SQLSMALLINT, SQLSMALLINT*, SQLSMALLINT*,
                                       SQLULEN*, SQLSMALLINT*, SQLSMALLINT*);
    SQLRETURN (SQL_API *ColAttributeW)(SQLHSTMT, SQLUSMALLINT, SQLUSMALLINT,
                                        SQLPOINTER, SQLSMALLINT, SQLSMALLINT*,
                                        SQLLEN*);
    SQLRETURN (SQL_API *Fetch)(SQLHSTMT);
    SQLRETURN (SQL_API *GetData)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT,
                                  SQLPOINTER, SQLLEN, SQLLEN*);
    SQLRETURN (SQL_API *RowCount)(SQLHSTMT, SQLLEN*);

    /* Driver Manager-only — NULL en mode direct */
    SQLRETURN (SQL_API *DriversW)(SQLHENV, SQLUSMALLINT, SQLWCHAR*,
                                   SQLSMALLINT, SQLSMALLINT*,
                                   SQLWCHAR*, SQLSMALLINT, SQLSMALLINT*);
} OwlOdbcVT;

/* Charge odbc32.dll (Windows) ou libodbc.so.2 (Linux, futur).
 * Cached : 2e appel renvoie le meme pointeur sans re-LoadLibrary.
 * Renvoie NULL si le Driver Manager systeme est introuvable. */
const OwlOdbcVT* owl_odbc_vt_load_default(char* err_out, size_t err_cap);

/* Charge la bibliotheque ODBC embarquee directement (pas d'enregistrement systeme).
 * Pre-requis : owl_hfsql_ensure_installed() a deja extrait la bibliotheque
 * dans %LOCALAPPDATA%/owl/hfsql_driver/v<ver>/. Cached. */
const OwlOdbcVT* owl_odbc_vt_load_hfsql_direct(char* err_out, size_t err_cap);

#endif  /* OWL_HAS_ODBC */

#endif
