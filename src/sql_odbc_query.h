// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_odbc_query_h
#define owl_sql_odbc_query_h

/* ================================================================
 * ODBC — query execution + result materialization
 *
 * Execute une requete SQL via SQLExecDirectW, recolte les metadonnees
 * via SQLDescribeColW, puis itere SQLFetch + SQLGetData pour materialiser
 * les cellules.
 *
 * Type dispatch en fetch :
 *   SQL_INTEGER/SMALLINT/TINYINT/BIGINT  -> SQL_C_SBIGINT -> int64
 *   SQL_REAL/FLOAT/DOUBLE                -> SQL_C_DOUBLE   -> double
 *   SQL_BIT                              -> SQL_C_BIT      -> bool
 *   SQL_DECIMAL/NUMERIC                  -> SQL_C_CHAR     -> string (precision)
 *   SQL_VARCHAR/CHAR/LONGVARCHAR         -> SQL_C_WCHAR    -> UTF-8
 *   SQL_WVARCHAR/WCHAR/WLONGVARCHAR      -> SQL_C_WCHAR    -> UTF-8
 *   SQL_TYPE_DATE                        -> SQL_C_TYPE_DATE      -> "YYYY-MM-DD"
 *   SQL_TYPE_TIME                        -> SQL_C_TYPE_TIME      -> "HH:MM:SS"
 *   SQL_TYPE_TIMESTAMP                   -> SQL_C_TYPE_TIMESTAMP -> ISO
 *   SQL_GUID                             -> SQL_C_GUID    -> "xxxx-xx-xx-xx-xxxxxxxxxxxx"
 *   SQL_BINARY/VARBINARY/LONGVARBINARY   -> SQL_C_BINARY  -> bytes buffer
 *   Autres                               -> SQL_C_WCHAR fallback (text)
 *
 * NULL handling : SQLGetData renvoie SQL_NULL_DATA dans le StrLen_or_Ind.
 * On le mappe sur OwlOdbcCell.kind = OWL_ODBC_VAL_NULL.
 *
 * Pour les strings/binaires de taille inconnue : dynamic grow via
 * SQLGetData en boucle (le driver indique combien il reste).
 * ================================================================ */

#include "common.h"
#include "sql_odbc_conn.h"

typedef enum {
    OWL_ODBC_VAL_NULL,
    OWL_ODBC_VAL_INTEGER,
    OWL_ODBC_VAL_REAL,
    OWL_ODBC_VAL_BOOL,
    OWL_ODBC_VAL_STRING_UTF8,   /* texte, decimaux, dates au format ISO */
    OWL_ODBC_VAL_BUFFER,        /* binaire raw */
} OwlOdbcCellKind;

typedef struct {
    OwlOdbcCellKind kind;
    int64_t  i_val;
    double   r_val;
    bool     b_val;
    char*    s_val;             /* alloue, free par owl_odbc_result_free */
    uint8_t* bytes;             /* alloue */
    size_t   bytes_len;
} OwlOdbcCell;

typedef struct {
    char     name[128];         /* nom colonne, UTF-8 */
    int16_t  sql_type;          /* SQL_INTEGER, SQL_VARCHAR, ... */
    uint32_t column_size;
    int16_t  decimal_digits;
    bool     nullable;
} OwlOdbcColumn;

typedef struct {
    bool success;                       /* false si execute a echoue */
    char error_message[512];

    OwlOdbcColumn* columns;
    size_t         column_count;

    OwlOdbcCell*   cells;               /* row_count * column_count, ordre row-major */
    size_t         row_count;

    int64_t        rows_affected;       /* SQLRowCount, utile DML */
} OwlOdbcResult;

void owl_odbc_result_init(OwlOdbcResult* r);
void owl_odbc_result_free(OwlOdbcResult* r);

/* Execute une requete SQL et materialise le resultat dans r.
 * r doit etre owl_odbc_result_init avant l'appel, et free apres usage.
 * Retourne true si execute OK (meme si 0 lignes). false sur erreur driver. */
bool owl_odbc_simple_query(OwlOdbcConn* c, const char* sql_utf8,
                             OwlOdbcResult* r);

/* ================================================================
 * Parametres
 *
 * On accepte les memes types Value-equivalents que les backends natifs
 * (PG, MySQL, MSSQL) : INT, REAL, BOOL, STRING UTF-8, BUFFER, NULL.
 * Le SQL doit utiliser les placeholders ODBC standard '?' (positionnel
 * 1-based) — la transformation {nom} -> ? est faite cote stdlib_sql.
 *
 * Pour chaque parametre :
 *   - SQLBindParameter avec le type C natif (SBIGINT/DOUBLE/BIT/CHAR/BINARY)
 *   - StrLen_or_Ind = SQL_NULL_DATA pour NULL, taille reelle sinon
 * Le buffer pointe directement dans OwlOdbcParam (caller-managed).
 * ================================================================ */

typedef enum {
    OWL_ODBC_PARAM_NULL,
    OWL_ODBC_PARAM_INTEGER,
    OWL_ODBC_PARAM_REAL,
    OWL_ODBC_PARAM_BOOL,
    OWL_ODBC_PARAM_STRING_UTF8,   /* UTF-8 brut */
    OWL_ODBC_PARAM_BUFFER,
} OwlOdbcParamKind;

typedef struct {
    OwlOdbcParamKind kind;
    int64_t        i_val;
    double         r_val;
    bool           b_val;
    const uint8_t* bytes;         /* STRING_UTF8 ou BUFFER. Pointeur non-owning */
    size_t         bytes_len;
} OwlOdbcParam;

/* Execute une requete parametree. Les params sont liees aux placeholders '?'
 * dans le SQL, dans l'ordre. */
bool owl_odbc_simple_query_params(OwlOdbcConn* c, const char* sql_utf8,
                                     const OwlOdbcParam* params, size_t n_params,
                                     OwlOdbcResult* r);

#endif
