// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_duckdb_query.h"
#include "sql_duckdb_types.h"

#include "object.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void owl_duckdb_result_init(OwlDuckdbResult* r) {
    memset(r, 0, sizeof(*r));
    r->gc_anchor = NIL_VAL;
}

void owl_duckdb_result_free(OwlDuckdbResult* r) {
    /* Libere l'ancrage GC que la query a laisse sur la pile VM (cf. gc_anchor).
     * Contrat : appeler quand l'ancrage est au sommet de la pile — le
     * consommateur doit avoir depile tout ce qu'il a pousse par-dessus. Les
     * valeurs Obj survivent via la structure de rendu (rep) qui les reference
     * desormais. Les chemins d'erreur de la query ont deja depile + remis NIL. */
    if (IS_OBJ(r->gc_anchor)) { vm_pop(); r->gc_anchor = NIL_VAL; }
    if (r->columns) {
        for (int i = 0; i < r->column_count; i++) free(r->columns[i].name);
        free(r->columns);
    }
    if (r->rows) {
        for (int i = 0; i < r->row_count; i++) free(r->rows[i]);
        free(r->rows);
    }
    memset(r, 0, sizeof(*r));
}

static bool append_row(OwlDuckdbResult* r, Value* row) {
    if (r->row_count >= r->row_capacity) {
        int newCap = r->row_capacity == 0 ? 16 : r->row_capacity * 2;
        Value** nr = (Value**)realloc(r->rows, sizeof(Value*) * newCap);
        if (!nr) return false;
        r->rows         = nr;
        r->row_capacity = newCap;
    }
    r->rows[r->row_count++] = row;
    return true;
}

bool owl_duckdb_simple_query(OwlDuckdbConn* c, const char* sql,
                              OwlDuckdbResult* out) {
    owl_duckdb_result_init(out);
    out->success = false;

    if (!c->connected) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "duckdb: connexion non ouverte");
        return false;
    }

    duckdb_result result;
    if (duckdb_query(c->conn, sql, &result) != DuckDBSuccess) {
        const char* msg = duckdb_result_error(&result);
        snprintf(out->error_message, sizeof(out->error_message),
                 "duckdb: %s", msg ? msg : "(erreur inconnue)");
        duckdb_destroy_result(&result);
        return false;
    }

    /* Metadata colonnes. */
    idx_t col_count = duckdb_column_count(&result);
    out->column_count = (int)col_count;
    out->columns = (OwlDuckdbColumn*)calloc(col_count > 0 ? col_count : 1,
                                              sizeof(OwlDuckdbColumn));
    if (!out->columns && col_count > 0) {
        snprintf(out->error_message, sizeof(out->error_message), "OOM cols");
        duckdb_destroy_result(&result);
        return false;
    }
    for (idx_t i = 0; i < col_count; i++) {
        const char* name = duckdb_column_name(&result, i);
        out->columns[i].name = name ? strdup(name) : strdup("");
        out->columns[i].type = duckdb_column_type(&result, i);
    }

    /* Racine GC du jeu de resultats : un tableau OWL rooté qui retient chaque
     * valeur Obj des que la cellule est materialisee. Attacher l'Obj a ce
     * conteneur rooté le protege des allocations suivantes (ligne i+1 dans la
     * query, puis construction du rendu OWL dans envoie_duckdb). L'ancrage est
     * pousse ici et LAISSE sur la pile VM au retour : le consommateur le depile
     * quand il a fini (borné a UNE slot de pile, croit sur le tas OWL). */
    ObjArray* anchor = obj_new_array();
    vm_push(OBJ_VAL(anchor));
    out->gc_anchor = OBJ_VAL(anchor);

    /* Boucle chunks (typ 2048 rows par chunk). */
    while (1) {
        duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
        if (chunk == NULL) break;
        idx_t chunk_size = duckdb_data_chunk_get_size(chunk);
        if (chunk_size == 0) {
            duckdb_destroy_data_chunk(&chunk);
            break;
        }
        duckdb_vector* vecs = NULL;
        if (col_count > 0) {
            vecs = (duckdb_vector*)calloc(col_count, sizeof(duckdb_vector));
            if (!vecs) {
                snprintf(out->error_message, sizeof(out->error_message), "OOM vectors");
                vm_pop(); out->gc_anchor = NIL_VAL; /* libere l'ancrage */
                duckdb_destroy_data_chunk(&chunk);
                duckdb_destroy_result(&result);
                return false;
            }
            for (idx_t i = 0; i < col_count; i++) {
                vecs[i] = duckdb_data_chunk_get_vector(chunk, i);
            }
        }

        for (idx_t r = 0; r < chunk_size; r++) {
            Value* row = (Value*)calloc(col_count > 0 ? col_count : 1,
                                          sizeof(Value));
            if (!row) {
                free(vecs);
                vm_pop(); out->gc_anchor = NIL_VAL; /* libere l'ancrage */
                duckdb_destroy_data_chunk(&chunk);
                duckdb_destroy_result(&result);
                snprintf(out->error_message, sizeof(out->error_message), "OOM row");
                return false;
            }
            for (idx_t cc = 0; cc < col_count; cc++) {
                Value v = owl_duckdb_cell_to_owl(vecs[cc], r,
                                                  out->columns[cc].type,
                                                  &result, cc);
                row[cc] = v;
                /* Ancre l'Obj des sa creation : attache a `anchor` (rooté), il
                 * survit a toute allocation ulterieure (lignes suivantes + rendu
                 * OWL en aval). obj_array_push est canon (protege v pendant grow). */
                if (IS_OBJ(v)) obj_array_push(anchor, v);
            }
            if (!append_row(out, row)) {
                free(row); free(vecs);
                vm_pop(); out->gc_anchor = NIL_VAL; /* libere l'ancrage */
                duckdb_destroy_data_chunk(&chunk);
                duckdb_destroy_result(&result);
                snprintf(out->error_message, sizeof(out->error_message), "append_row");
                return false;
            }
        }

        free(vecs);
        duckdb_destroy_data_chunk(&chunk);
    }

    out->affected_rows = duckdb_rows_changed(&result);
    out->changed_rows  = (duckdb_result_return_type(result)
                            == DUCKDB_RESULT_TYPE_CHANGED_ROWS);

    duckdb_statement_type st = duckdb_result_statement_type(result);
    out->write_statement = (st == DUCKDB_STATEMENT_TYPE_INSERT
                            || st == DUCKDB_STATEMENT_TYPE_UPDATE
                            || st == DUCKDB_STATEMENT_TYPE_DELETE);

    duckdb_destroy_result(&result);
    out->success = true;
    return true;
}
