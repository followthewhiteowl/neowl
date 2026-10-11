// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_odbc_types_h
#define owl_sql_odbc_types_h

/* ================================================================
 * ODBC — mapping OwlOdbcCell -> Value OWL
 *
 * Le gros du travail (type dispatch a partir du SQL type) est deja
 * fait dans sql_odbc_query.c qui materialise OwlOdbcCell avec une
 * union discriminee. Ici on fait juste la traduction triviale vers
 * Value OWL.
 *
 *   OWL_ODBC_VAL_NULL        -> NIL_VAL
 *   OWL_ODBC_VAL_INTEGER     -> INTEGER_VAL(i_val)
 *   OWL_ODBC_VAL_REAL        -> REAL_VAL(r_val)
 *   OWL_ODBC_VAL_BOOL        -> BOOL_VAL(b_val)
 *   OWL_ODBC_VAL_STRING_UTF8 -> OBJ_VAL(obj_copy_string(s_val, len))
 *   OWL_ODBC_VAL_BUFFER      -> OBJ_VAL(obj_new_buffer(bytes, len))
 * ================================================================ */

#include "common.h"
#include "value.h"
#include "sql_odbc_query.h"

/* Convertit une cellule ODBC en Value OWL. */
Value owl_odbc_cell_to_owl(const OwlOdbcCell* cell);

#endif
