// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_duckdb_query_h
#define owl_sql_duckdb_query_h

/* Simple query DuckDB : envoie SQL et collecte le resultat row-oriente. */

#include "common.h"
#include "value.h"
#include "sql_duckdb_conn.h"

typedef struct {
    char*        name;
    duckdb_type  type;
} OwlDuckdbColumn;

typedef struct {
    OwlDuckdbColumn* columns;
    int              column_count;

    /* rows[i] = tableau de column_count Value (materializees, GC-trackable). */
    Value**          rows;
    int              row_count;
    int              row_capacity;

    uint64_t         affected_rows;

    /* Le moteur a rendu un compteur de lignes modifiees (une ligne de service
     * "Count") au lieu d'un jeu de resultats : requete d'ecriture. */
    bool             changed_rows;

    /* Le moteur classe l'instruction comme INSERT / UPDATE / DELETE. Signal
     * independant de changed_rows : une clause RETURNING produit un jeu de
     * resultats (donc changed_rows faux) tout en ecrivant. */
    bool             write_statement;

    bool             success;
    char             error_message[256];

    /* Racine GC : tableau OWL qui retient toutes les valeurs Obj du jeu de
     * resultats. Les Value stockees dans `rows` sont des Obj (chaines, buffers)
     * que le GC ne voit pas depuis un tableau C. Sans ancrage, la
     * materialisation d'une ligne collecte les Obj des lignes precedentes (UAF
     * result-set). La query pousse cet ancrage sur la pile VM et l'y LAISSE :
     * il traverse la frontiere jusqu'au consommateur, qui le depile une fois le
     * rendu OWL termine (cf. envoie_duckdb). NIL tant qu'aucune ligne. */
    Value            gc_anchor;
} OwlDuckdbResult;

void owl_duckdb_result_init(OwlDuckdbResult* r);
void owl_duckdb_result_free(OwlDuckdbResult* r);

bool owl_duckdb_simple_query(OwlDuckdbConn* c, const char* sql,
                              OwlDuckdbResult* out);

#endif
