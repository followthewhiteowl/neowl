// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mysql_types_h
#define owl_sql_mysql_types_h

/* ================================================================
 * Mapping MYSQL_TYPE_* -> Value OWL (text mode COM_QUERY)
 *
 * En text mode, toutes les valeurs arrivent comme des strings (meme
 * les INT). Le `type` de la colonne indique comment les convertir :
 *   entiers  : atoi/strtoll
 *   reels    : strtod
 *   strings  : passe-through
 *   blobs    : passe-through en buffer
 *   dates    : passe-through en string (user convertit si besoin)
 *   NULL     : NIL_VAL
 *
 * Pour V2.2 : text mode uniquement (COM_QUERY).
 * V2.3 : binary mode via COM_STMT_PREPARE/EXECUTE.
 * ================================================================ */

#include "common.h"
#include "value.h"

/* Type bytes MySQL — extrait de mysql_com.h. */
typedef enum {
    MYSQL_TYPE_DECIMAL     = 0x00,
    MYSQL_TYPE_TINY        = 0x01,
    MYSQL_TYPE_SHORT       = 0x02,
    MYSQL_TYPE_LONG        = 0x03,
    MYSQL_TYPE_FLOAT       = 0x04,
    MYSQL_TYPE_DOUBLE      = 0x05,
    MYSQL_TYPE_NULL        = 0x06,
    MYSQL_TYPE_TIMESTAMP   = 0x07,
    MYSQL_TYPE_LONGLONG    = 0x08,
    MYSQL_TYPE_INT24       = 0x09,
    MYSQL_TYPE_DATE        = 0x0A,
    MYSQL_TYPE_TIME        = 0x0B,
    MYSQL_TYPE_DATETIME    = 0x0C,
    MYSQL_TYPE_YEAR        = 0x0D,
    MYSQL_TYPE_VARCHAR     = 0x0F,
    MYSQL_TYPE_BIT         = 0x10,
    MYSQL_TYPE_JSON        = 0xF5,
    MYSQL_TYPE_NEWDECIMAL  = 0xF6,
    MYSQL_TYPE_ENUM        = 0xF7,
    MYSQL_TYPE_SET         = 0xF8,
    MYSQL_TYPE_TINY_BLOB   = 0xF9,
    MYSQL_TYPE_MEDIUM_BLOB = 0xFA,
    MYSQL_TYPE_LONG_BLOB   = 0xFB,
    MYSQL_TYPE_BLOB        = 0xFC,
    MYSQL_TYPE_VAR_STRING  = 0xFD,
    MYSQL_TYPE_STRING      = 0xFE,
    MYSQL_TYPE_GEOMETRY    = 0xFF,
} MysqlType;

/* Flag bits utiles dans ColumnDefinition41.flags. */
#define MYSQL_FLAG_NOT_NULL       0x0001
#define MYSQL_FLAG_PRI_KEY        0x0002
#define MYSQL_FLAG_UNIQUE_KEY     0x0004
#define MYSQL_FLAG_BLOB           0x0010
#define MYSQL_FLAG_UNSIGNED       0x0020
#define MYSQL_FLAG_AUTO_INCREMENT 0x0200

/* Convertit une valeur text-mode (bytes + length + type) en Value OWL.
 * value_len == -1 -> NIL_VAL.
 * Les bytes ne sont PAS retenus : on copie si besoin (string/buffer alloues
 * sur le heap OWL via obj_copy_string / obj_new_buffer). */
Value owl_mysql_value_to_owl(uint8_t mysql_type, uint16_t flags,
                               const uint8_t* value_bytes, int32_t value_len);

#endif
