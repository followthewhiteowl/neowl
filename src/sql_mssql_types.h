// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_types_h
#define owl_sql_mssql_types_h

/* ================================================================
 * MSSQL TDS 7.4 — type mapping cell bytes -> Value OWL
 *
 * Convertit une OwlMssqlCell (bytes bruts + descripteur colonne) en
 * Value OWL (INTEGER, REAL, BOOL, STRING, BUFFER, NIL).
 *
 * Mapping V1 :
 *   BIT, BITN                       -> BOOL
 *   TINYINT, SMALLINT, INT, BIGINT  -> INTEGER
 *   INTN (1/2/4/8 bytes)            -> INTEGER
 *   REAL, FLOAT                     -> REAL
 *   FLTN (4/8 bytes)                -> REAL
 *   MONEY, SMALLMONEY, MONEYN       -> REAL (value/10000)
 *   DATE                            -> STRING "YYYY-MM-DD"
 *   TIME                            -> STRING "HH:MM:SS.fffffff"
 *   DATETIME, SMALLDATETIME         -> STRING "YYYY-MM-DD HH:MM:SS.fff"
 *   DATETIMN                        -> STRING idem
 *   DATETIME2                       -> STRING "YYYY-MM-DD HH:MM:SS.fffffff"
 *   DATETIMEOFFSET                  -> STRING "YYYY-MM-DD HH:MM:SS.fff +HH:MM"
 *   GUID                            -> STRING "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
 *   DECIMAL, NUMERIC                -> STRING (preserve precision)
 *   CHAR, VARCHAR (BIG*)            -> STRING (ASCII / windows-1252 / utf-8
 *                                              tolere ; V1 traite comme UTF-8 brut)
 *   NCHAR, NVARCHAR                 -> STRING (UCS-2 LE -> UTF-8)
 *   BINARY, VARBINARY (BIG*)        -> BUFFER
 *
 * NULL -> NIL_VAL (detecte via cell.is_null).
 *
 * Types non supportes V1 -> NIL_VAL avec warning interne :
 *   TEXT, NTEXT, IMAGE (LONGLEN streamed)
 *   XML, UDT, VARIANT
 *   SQL_VARIANT
 * ================================================================ */

#include "common.h"
#include "value.h"
#include "sql_mssql_query.h"

/* Convertit une cellule en Value OWL. Si la cellule est NULL renvoie
 * NIL_VAL. Si le type n'est pas supporte renvoie NIL_VAL (logique
 * defensive : on prefere perdre la valeur plutot que planter). */
Value owl_mssql_cell_to_owl(const OwlMssqlColumn* col, const OwlMssqlCell* cell);

#endif /* owl_sql_mssql_types_h */
