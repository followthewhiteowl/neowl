// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_odbc_types.h"

#include "object.h"

Value owl_odbc_cell_to_owl(const OwlOdbcCell* cell) {
    if (!cell) return NIL_VAL;
    switch (cell->kind) {
        case OWL_ODBC_VAL_NULL:
            return NIL_VAL;
        case OWL_ODBC_VAL_INTEGER:
            return INTEGER_VAL(cell->i_val);
        case OWL_ODBC_VAL_REAL:
            return REAL_VAL(cell->r_val);
        case OWL_ODBC_VAL_BOOL:
            return BOOL_VAL(cell->b_val);
        case OWL_ODBC_VAL_STRING_UTF8: {
            if (!cell->s_val) return OBJ_VAL(obj_copy_string("", 0));
            return OBJ_VAL(obj_copy_string(cell->s_val, (int)cell->bytes_len));
        }
        case OWL_ODBC_VAL_BUFFER: {
            if (!cell->bytes || cell->bytes_len == 0) {
                return OBJ_VAL(obj_new_buffer(NULL, 0));
            }
            return OBJ_VAL(obj_new_buffer(cell->bytes, (int)cell->bytes_len));
        }
    }
    return NIL_VAL;
}
