// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_duckdb_types_h
#define owl_sql_duckdb_types_h

/* Mapping cellule DuckDB (vector + row index + type) -> Value OWL. */

#include "common.h"
#include "value.h"
#include "duckdb.h"

Value owl_duckdb_cell_to_owl(duckdb_vector vec, uint64_t row_idx,
                              duckdb_type type,
                              duckdb_result* result, uint64_t col_idx);

#endif
