// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_postgres_query_h
#define owl_sql_postgres_query_h

/* ================================================================
 * Simple Query côté client.
 *
 * Envoie une Query (message 'Q') et collecte toute la réponse
 * jusqu'au ReadyForQuery final :
 *   Query 'Q'   ─→ envoyée
 *   RowDescription 'T' (1×)
 *   DataRow 'D' (N×)
 *   CommandComplete 'C'
 *   ReadyForQuery 'Z'   (terminal)
 *   OU ErrorResponse 'E' (échec)
 *
 * Pour V1 on fait juste la "simple query" (pas de paramètres bind).
 * Extended query (Parse/Bind/Execute/Sync) sera ajouté en Phase 3
 * quand on connectera ça à SQLEnvoie qui accepte des Paramètres.
 *
 * Mémoire : OwlPostgresResult contient des copies des column names (strdup)
 * et des slabs de bytes pour les payloads DataRow. Les values
 * pointent zero-copy dans ces slabs. Appeler owl_postgres_result_free
 * libère le tout.
 * ================================================================ */

#include "common.h"
#include "sql_postgres_conn.h"

typedef struct {
    char*    name;       /* malloc, libéré par result_free */
    uint32_t type_oid;   /* PostgresOid */
    int16_t  format;     /* 0 = text, 1 = binary */
} OwlPostgresColumn;

typedef struct {
    /* length == -1 → SQL NULL.
     * Sinon : 0..N bytes, data zero-copy dans le slab de la ligne. */
    int32_t        length;
    const uint8_t* data;
} OwlPostgresValue;

typedef struct {
    /* Schema. */
    OwlPostgresColumn* columns;
    int          column_count;

    /* Données : rows[i] = tableau de column_count OwlPostgresValue. */
    OwlPostgresValue** rows;
    int          row_count;
    int          row_capacity;

    /* Pool d'allocations : on garde les payloads bruts owned ici
     * pour que les pointeurs OwlPostgresValue.data restent valides. */
    uint8_t**    payload_slabs;
    int          payload_count;
    int          payload_capacity;

    /* CommandComplete tag, ex "SELECT 1", "INSERT 0 1", "UPDATE 3" */
    char         command_tag[64];

    /* Succès ou non. Si false, error_message est rempli. */
    bool         success;
    char         error_message[256];
} OwlPostgresResult;

void owl_postgres_result_init(OwlPostgresResult* r);
void owl_postgres_result_free(OwlPostgresResult* r);

/* Lance une simple query et bloque jusqu'à ReadyForQuery.
 * Remplit out (init est fait en interne — passer un struct vide).
 * Retourne true si la query a réussi (success=true). En cas
 * d'ErrorResponse, retourne false et out.error_message est rempli. */
bool owl_postgres_simple_query(OwlPostgresConn* c, const char* sql, OwlPostgresResult* out);

/* Extended query : Parse + Bind + Describe + Execute + Sync.
 * Pour requetes parametrees ($1, $2, ...). Tous params sont passes
 * en text mode (chaque param est deja une chaine OWL formattee).
 * param_values[i] == NULL -> SQL NULL.
 * Reponse drainee jusqu'a ReadyForQuery. */
bool owl_postgres_extended_query(OwlPostgresConn* c,
                                  const char* sql,
                                  int n_params,
                                  const char* const* param_values,
                                  OwlPostgresResult* out);

#endif
