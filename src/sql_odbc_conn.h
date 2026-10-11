// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_odbc_conn_h
#define owl_sql_odbc_conn_h

/* ================================================================
 * ODBC — couche connexion (partagee entre backends sqlODBC et sqlHFSQL)
 *
 * Implementation de l'API ODBC 3.x, abstraite via une vtable
 * (OwlOdbcVT). Deux loaders alimentent cette VT :
 *   - owl_odbc_vt_load_default        -> odbc32.dll (Windows) ou
 *                                        libodbc.so.2 (Linux unixODBC,
 *                                        a venir). Le backend sqlODBC
 *                                        passe par la.
 *   - owl_odbc_vt_load_hfsql_direct   -> LoadLibrary direct sur le
 *                                        driver HFSQL embarque (bypass
 *                                        complet du Driver Manager). Le
 *                                        backend sqlHFSQL passe par la.
 *
 * Cette couche est generique : elle ne sait pas *quelle* VT lui est
 * fournie. Le caller (stdlib_sql.c) instancie la bonne VT selon le
 * Provider de la sqlConnexion avant d'appeler owl_odbc_conn_open().
 *
 * Encoding : SQL_C_WCHAR partout. SQLWCHAR = 2 octets sur Windows
 * (UTF-16 LE) et sur Linux/unixODBC. Sur Linux iODBC, SQLWCHAR = 4
 * octets (UCS-4 / wchar_t) — gere conditionnellement (futur, Phase 2).
 *
 * V1 capacite : SQL_C_WCHAR pour les strings, SQL_C_SBIGINT/SLONG pour
 * les entiers, SQL_C_DOUBLE pour les reels, SQL_C_BIT pour les bool,
 * SQL_C_BINARY pour les BLOB.
 * ================================================================ */

#include "common.h"

#define OWL_ODBC_LAST_ERROR_SIZE 512

/* Forward declaration : OwlOdbcVT defini dans sql_odbc_loader.h. On
 * evite l'include ici pour ne pas exposer sql.h aux callers (qui
 * passent juste le pointeur opaque). */
struct OwlOdbcVT;

typedef struct {
    /* ODBC handles : void* pour ne pas exposer sql.h dans ce header.
     * Sur Windows : SQLHENV / SQLHDBC. Cf sql_odbc_conn.c pour les
     * casts. */
    void* henv;             /* environment */
    void* hdbc;             /* connection */
    bool  connected;

    /* VT installee par le caller avant conn_open. Non-NULL obligatoire. */
    const struct OwlOdbcVT* vt;

    char  last_error[OWL_ODBC_LAST_ERROR_SIZE];
} OwlOdbcConn;

void owl_odbc_conn_init(OwlOdbcConn* c);

/* Ouvre une connexion ODBC depuis une connection string.
 *
 *   vt            : VT a utiliser (resolue par le caller via le loader
 *                   approprie). Non-NULL. Sera stockee dans c->vt.
 *   conn_str_utf8 : connection string complete, format dependant du
 *                   driver. Ex sqlHFSQL :
 *                     "DRIVER={HFSQL};Server Name=...;Server Port=4900;..."
 *                   Ex sqlODBC vers un autre driver :
 *                     "DRIVER={Oracle};SERVER=...;..."
 *   timeout_s     : timeout en secondes pour la connexion. 0 = pas de
 *                   timeout (defaut driver).
 *
 * Retourne true si connecte. Sinon last_error rempli avec le SQLSTATE +
 * message diagnostique du driver. */
bool owl_odbc_conn_open(OwlOdbcConn* c,
                          const struct OwlOdbcVT* vt,
                          const char* conn_str_utf8,
                          int timeout_s);

void owl_odbc_conn_close(OwlOdbcConn* c);

const char* owl_odbc_conn_last_error(const OwlOdbcConn* c);

/* Liste les drivers ODBC installes sur le poste (via SQLDrivers).
 * Utile pour diagnostic ("est-ce que HFSQL est installé ?").
 * Renvoie un tableau alloue de chaines (caller libere chaque + le tableau).
 * out_count rempli avec le nombre. */
char** owl_odbc_list_drivers(int* out_count);

#endif
