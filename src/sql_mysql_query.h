// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mysql_query_h
#define owl_sql_mysql_query_h

/* ================================================================
 * Simple Query MySQL : envoi COM_QUERY + parse ResultSet text mode.
 *
 * Pipeline reception :
 *   pkt 1   : ColumnCount (lenenc int)
 *   pkt 2..N+1 : ColumnDefinition41 (1 par colonne)
 *   pkt N+2 : EOF (sauf si DEPRECATE_EOF -> remplace par OK_eof apres rows)
 *   pkt N+3..: DataRow (lenenc str par colonne, 0xFB pour SQL NULL)
 *   pkt final : EOF ou OK_eof
 *
 * Sortie : OwlMysqlResult avec colonnes + lignes. Les valeurs pointent
 * zero-copy dans les payloads retenus dans `payload_slabs` (free par
 * owl_mysql_result_free).
 *
 * Pour V2.2 on ne fait que la simple query (pas de COM_STMT_PREPARE).
 * Les parametres bind viendront en V2.2.1 ou plus tard.
 * ================================================================ */

#include "common.h"
#include "sql_mysql_conn.h"
#include "sql_mysql_types.h"   /* OwlMysqlBindValue */

typedef struct {
    char*    name;       /* malloc, libere par result_free */
    uint8_t  type;       /* MYSQL_TYPE_* */
    uint16_t charset;
    uint32_t column_length;
    uint16_t flags;
} OwlMysqlColumn;

typedef struct {
    /* length == -1 -> SQL NULL.
     * Sinon : pointer zero-copy dans un slab retenu. */
    int32_t        length;
    const uint8_t* data;
} OwlMysqlValue;

typedef struct {
    /* Schema. */
    OwlMysqlColumn* columns;
    int             column_count;

    /* Donnees : rows[i] = tableau de column_count OwlMysqlValue. */
    OwlMysqlValue** rows;
    int             row_count;
    int             row_capacity;

    /* Slabs retenus pour que les pointeurs OwlMysqlValue.data restent
     * vivants. Libere dans result_free. */
    uint8_t**       payload_slabs;
    int             payload_count;
    int             payload_capacity;

    /* OK packet final : affected_rows / last_insert_id. */
    uint64_t        affected_rows;
    uint64_t        last_insert_id;

    /* Succes du flow. Si false, error_message rempli. */
    bool            success;
    char            error_message[256];
} OwlMysqlResult;

void owl_mysql_result_init(OwlMysqlResult* r);
void owl_mysql_result_free(OwlMysqlResult* r);

/* Lance une simple query (COM_QUERY) et collecte la reponse jusqu'a
 * EOF/OK_eof terminal. Retourne true si la query a reussi. */
bool owl_mysql_simple_query(OwlMysqlConn* c, const char* sql, OwlMysqlResult* out);

/* Requete preparee server-side : COM_STMT_PREPARE (template avec '?') puis
 * COM_STMT_EXECUTE (valeurs liees separees). Un INSERT/UPDATE/DELETE renvoie
 * un paquet OK (affected_rows) ; un SELECT renvoie un binary resultset,
 * decode ici. Les valeurs ne sont jamais concatenees dans le SQL. */
bool owl_mysql_prepared_query(OwlMysqlConn* c, const char* sql,
                              int n_params, const OwlMysqlBindValue* params,
                              OwlMysqlResult* out);

#endif
