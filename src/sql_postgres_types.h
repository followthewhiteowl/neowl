// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_postgres_types_h
#define owl_sql_postgres_types_h

/* ================================================================
 * PostgreSQL type Oids + text-mode value encoders/decoders
 *
 * Convertit entre les representations text mode PG et les types C
 * fondamentaux (int64, double, bool, bytes, strings). Pas de
 * dependance Value OWL — le glue vers OWL est dans la couche
 * sql_postgres_conn (Phase 2.4).
 *
 * Text mode (format 0) : tout est string ASCII decodable. Ralenti
 * vs binary mode (format 1) pour les types numeriques, mais bien
 * plus simple pour V1. On peut migrer certains types en binaire
 * plus tard pour les perf (V2.1+).
 *
 * Reference : src/include/catalog/pg_type.dat dans le source PG.
 * ================================================================ */

#include "common.h"
#include <stddef.h>

/* Subset des Oids documentes et utilises en pratique. */
typedef enum {
    POSTGRES_OID_BOOL          = 16,
    POSTGRES_OID_BYTEA         = 17,
    POSTGRES_OID_CHAR          = 18,
    POSTGRES_OID_NAME          = 19,
    POSTGRES_OID_INT8          = 20,    /* bigint */
    POSTGRES_OID_INT2          = 21,    /* smallint */
    POSTGRES_OID_INT4          = 23,    /* integer */
    POSTGRES_OID_TEXT          = 25,
    POSTGRES_OID_OID           = 26,
    POSTGRES_OID_JSON          = 114,
    POSTGRES_OID_FLOAT4        = 700,   /* real */
    POSTGRES_OID_FLOAT8        = 701,   /* double precision */
    POSTGRES_OID_VARCHAR       = 1043,
    POSTGRES_OID_DATE          = 1082,
    POSTGRES_OID_TIME          = 1083,
    POSTGRES_OID_TIMESTAMP     = 1114,
    POSTGRES_OID_TIMESTAMPTZ   = 1184,
    POSTGRES_OID_NUMERIC       = 1700,
    POSTGRES_OID_UUID          = 2950,
    POSTGRES_OID_JSONB         = 3802,
} PostgresOid;

/* Categorie de mapping vers OWL — guide la conversion en Phase 2.4. */
typedef enum {
    POSTGRES_CAT_UNKNOWN,
    POSTGRES_CAT_BOOL,
    POSTGRES_CAT_INTEGER,
    POSTGRES_CAT_REAL,
    POSTGRES_CAT_STRING,        /* TEXT, VARCHAR, CHAR, NAME, NUMERIC, JSON, UUID, ... */
    POSTGRES_CAT_BYTEA,
    POSTGRES_CAT_DATE,
    POSTGRES_CAT_TIME,
    POSTGRES_CAT_TIMESTAMP,
} PostgresCategory;

/* Map Oid -> categorie. */
PostgresCategory postgres_oid_category(uint32_t oid);

/* Nom court pour debug / doc / LSP. NULL si Oid inconnu. */
const char* postgres_oid_name(uint32_t oid);

/* ===================================================================
 * Parsing text mode (depuis bytes recus en DataRow)
 *
 * Les bytes pointes par `in` peuvent NE PAS etre null-terminated —
 * `in_len` est la longueur exacte. Retournent true sur succes.
 * ===================================================================*/

/* Booleen PG text : 't' / 'true' / 'y' / 'yes' / '1' = Vrai. */
bool postgres_parse_bool(const char* in, size_t in_len, bool* out);

/* Entier signe 64-bit. */
bool postgres_parse_int64(const char* in, size_t in_len, int64_t* out);

/* Double precision. */
bool postgres_parse_double(const char* in, size_t in_len, double* out);

/* BYTEA hex format PG : "\\xDEADBEEF". Alloue *out_bytes via malloc —
 * caller doit free. Retourne true + *out_len rempli. */
bool postgres_parse_bytea_hex(const char* in, size_t in_len,
                         uint8_t** out_bytes, size_t* out_len);

/* ===================================================================
 * Formatting text mode (pour Bind cote client)
 *
 * Ecrit dans out (cap = out_size). Retourne le nb d'octets ecrits
 * (sans \0 terminal), -1 si out_size trop petit.
 * ===================================================================*/

int postgres_format_bool(bool v, char* out, size_t out_size);
int postgres_format_int64(int64_t v, char* out, size_t out_size);
int postgres_format_double(double v, char* out, size_t out_size);

/* BYTEA hex format : produit "\\xHHHHHH..." (longueur = 2 + 2*len). */
int postgres_format_bytea_hex(const uint8_t* bytes, size_t len,
                         char* out, size_t out_size);

#endif
