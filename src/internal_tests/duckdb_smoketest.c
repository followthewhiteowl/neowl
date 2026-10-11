// SPDX-License-Identifier: AGPL-3.0-only
/* Smoke test de la lib vendoree DuckDB en profil musl-static universel.
 *
 * But : prouver que libduckdb_static.a (+ extensions) recompilee musl
 * s'execute reellement dans un binaire 100% statique (Alpine/scratch, sans
 * glibc). Ouvre une base en memoire, execute une requete, verifie le
 * resultat. Exit 0 = OK, non-zero = KO. Compile/linke via la cible
 * duckdb_smoketest du CMakeLists (link owl_duckdb).
 *
 * Ce n'est PAS un wrapper OWL : juste la validation d'integration de la lib
 * avant que les natives SQL OWL soient exercees par la suite de tests. */

#include <stdio.h>
#include "duckdb.h"

int main(void) {
    duckdb_database db;
    duckdb_connection con;
    duckdb_result res;

    if (duckdb_open(NULL, &db) != DuckDBSuccess) {
        fprintf(stderr, "duckdb_smoketest: open failed\n");
        return 1;
    }
    if (duckdb_connect(db, &con) != DuckDBSuccess) {
        fprintf(stderr, "duckdb_smoketest: connect failed\n");
        duckdb_close(&db);
        return 1;
    }
    if (duckdb_query(con, "SELECT 40 + 2 AS answer;", &res) != DuckDBSuccess) {
        fprintf(stderr, "duckdb_smoketest: query failed: %s\n",
                duckdb_result_error(&res));
        duckdb_disconnect(&con);
        duckdb_close(&db);
        return 1;
    }

    int64_t answer = duckdb_value_int64(&res, 0, 0);
    printf("duckdb_smoketest: SELECT 40 + 2 = %lld\n", (long long)answer);

    int rc = (answer == 42) ? 0 : 2;

    duckdb_destroy_result(&res);
    duckdb_disconnect(&con);
    duckdb_close(&db);
    return rc;
}
