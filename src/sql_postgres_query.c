// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_postgres_query.h"

#include "sql_postgres_wire.h"
#include "net_be_buffer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void owl_postgres_result_init(OwlPostgresResult* r) {
    memset(r, 0, sizeof(*r));
}

void owl_postgres_result_free(OwlPostgresResult* r) {
    if (r->columns) {
        for (int i = 0; i < r->column_count; i++) free(r->columns[i].name);
        free(r->columns);
    }
    if (r->rows) {
        for (int i = 0; i < r->row_count; i++) free(r->rows[i]);
        free(r->rows);
    }
    if (r->payload_slabs) {
        for (int i = 0; i < r->payload_count; i++) free(r->payload_slabs[i]);
        free(r->payload_slabs);
    }
    memset(r, 0, sizeof(*r));
}

/* ================================================================
 * Parsers de RowDescription / DataRow
 * ================================================================ */

/* RowDescription payload :
 *   i16 nFields
 *   N × (cstring name, i32 tableOid, i16 colNum, i32 typeOid,
 *        i16 typeLen, i32 typeMod, i16 formatCode)
 */
static bool parse_row_description(const uint8_t* payload, uint32_t plen,
                                    OwlPostgresResult* out) {
    OwlBeBuffer in; owl_be_buffer_attach_read(&in, payload, plen);
    uint16_t n;
    if (!owl_be_buffer_read_u16(&in, &n)) return false;

    OwlPostgresColumn* cols = (OwlPostgresColumn*)calloc(n, sizeof(OwlPostgresColumn));
    if (!cols && n > 0) return false;

    for (uint16_t i = 0; i < n; i++) {
        const char* name = owl_be_buffer_read_cstring(&in);
        if (!name) { free(cols); return false; }
        cols[i].name = strdup(name);

        uint32_t tableOid; int16_t colNum;
        uint32_t typeOid;  int16_t typeLen;
        uint32_t typeMod;  int16_t fmt;
        if (!owl_be_buffer_read_u32(&in, &tableOid))  { free(cols); return false; }
        if (!owl_be_buffer_read_i16(&in, &colNum))    { free(cols); return false; }
        if (!owl_be_buffer_read_u32(&in, &typeOid))   { free(cols); return false; }
        if (!owl_be_buffer_read_i16(&in, &typeLen))   { free(cols); return false; }
        if (!owl_be_buffer_read_u32(&in, &typeMod))   { free(cols); return false; }
        if (!owl_be_buffer_read_i16(&in, &fmt))       { free(cols); return false; }
        (void)tableOid; (void)colNum; (void)typeLen; (void)typeMod;

        cols[i].type_oid = typeOid;
        cols[i].format   = fmt;
    }

    out->columns      = cols;
    out->column_count = n;
    return true;
}

/* DataRow payload :
 *   i16 nValues
 *   N × (i32 length OR -1 si NULL, then `length` bytes)
 *
 * On garde le payload entier dans payload_slabs pour que les
 * pointeurs data des values restent valides apres retour.
 */
static bool parse_data_row(uint8_t* payload, uint32_t plen,
                            OwlPostgresResult* out) {
    OwlBeBuffer in; owl_be_buffer_attach_read(&in, payload, plen);
    uint16_t n;
    if (!owl_be_buffer_read_u16(&in, &n)) return false;
    if ((int)n != out->column_count) return false;  /* incohérence schema */

    OwlPostgresValue* values = (OwlPostgresValue*)calloc(n, sizeof(OwlPostgresValue));
    if (!values && n > 0) return false;

    for (uint16_t i = 0; i < n; i++) {
        int32_t length;
        if (!owl_be_buffer_read_i32(&in, &length)) { free(values); return false; }
        values[i].length = length;
        if (length < 0) {
            values[i].data = NULL;
        } else {
            /* Pointeur zero-copy dans payload. */
            if (in.pos + (size_t)length > in.length) { free(values); return false; }
            values[i].data = payload + in.pos;
            in.pos += (size_t)length;
        }
    }

    /* Stocke le slab pour que les data pointers survivent. */
    if (out->payload_count >= out->payload_capacity) {
        int newCap = out->payload_capacity == 0 ? 16 : out->payload_capacity * 2;
        uint8_t** ns = (uint8_t**)realloc(out->payload_slabs,
                                           sizeof(uint8_t*) * newCap);
        if (!ns) { free(values); return false; }
        out->payload_slabs    = ns;
        out->payload_capacity = newCap;
    }
    out->payload_slabs[out->payload_count++] = payload;
    /* Note : on transfère ownership du payload — caller ne doit plus
     * le free. */

    /* Append la row au vecteur rows. */
    if (out->row_count >= out->row_capacity) {
        int newCap = out->row_capacity == 0 ? 16 : out->row_capacity * 2;
        OwlPostgresValue** nr = (OwlPostgresValue**)realloc(out->rows,
                                                  sizeof(OwlPostgresValue*) * newCap);
        if (!nr) { free(values); return false; }
        out->rows         = nr;
        out->row_capacity = newCap;
    }
    out->rows[out->row_count++] = values;
    return true;
}

/* ================================================================
 * API publique
 * ================================================================ */

/* Drain la reponse serveur jusqu'a ReadyForQuery. Identique pour
 * simple query et extended query (les seules differences sont les
 * messages ParseComplete '1' / BindComplete '2' que extended produit
 * en plus — on les ignore ici). */
static bool drain_query_response(OwlPostgresConn* c, OwlPostgresResult* out) {
    while (1) {
        char type;
        uint8_t* payload = NULL;
        uint32_t plen = 0;
        if (!owl_postgres_recv_msg(c, &type, &payload, &plen)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "%s", owl_postgres_conn_last_error(c));
            return false;
        }

        switch (type) {
            case 'T': {  /* RowDescription */
                bool ok = parse_row_description(payload, plen, out);
                free(payload);
                if (!ok) {
                    snprintf(out->error_message, sizeof(out->error_message),
                             "parse RowDescription echec");
                    return false;
                }
                break;
            }
            case 'D': {  /* DataRow — ownership transferé */
                if (!parse_data_row(payload, plen, out)) {
                    free(payload);
                    snprintf(out->error_message, sizeof(out->error_message),
                             "parse DataRow echec");
                    return false;
                }
                /* PAS de free(payload) ici : transféré dans payload_slabs */
                break;
            }
            case 'C': {  /* CommandComplete : cstring tag */
                if (plen > 0) {
                    size_t cap = sizeof(out->command_tag) - 1;
                    size_t cp = plen - 1;
                    if (cp > cap) cp = cap;
                    memcpy(out->command_tag, payload, cp);
                    out->command_tag[cp] = '\0';
                }
                free(payload);
                break;
            }
            case 'I':    /* EmptyQueryResponse */
                free(payload);
                break;
            case 'E': {  /* ErrorResponse */
                const char* m = postgres_parse_error_message(payload, plen);
                snprintf(out->error_message, sizeof(out->error_message),
                         "%s", m ? m : "(server error)");
                free(payload);
                /* On continue à drainer jusqu'au ReadyForQuery pour ne pas
                 * laisser la conn dans un état incohérent. */
                continue;
            }
            case 'N':    /* NoticeResponse — log only, on ignore */
                free(payload);
                break;
            case '1':    /* ParseComplete (extended query) */
            case '2':    /* BindComplete (extended query) */
            case 'n':    /* NoData (Describe sans result set) */
            case 't':    /* ParameterDescription */
                free(payload);
                break;
            case 'Z': {  /* ReadyForQuery → end of query response */
                free(payload);
                /* Si error_message rempli, on a eu une ErrorResponse. */
                if (out->error_message[0] == '\0') out->success = true;
                return out->success;
            }
            default:
                /* Type non géré — on log et ignore. */
                free(payload);
                break;
        }
    }
}

bool owl_postgres_simple_query(OwlPostgresConn* c, const char* sql, OwlPostgresResult* out) {
    owl_postgres_result_init(out);
    out->success = false;

    OwlBeBuffer q; owl_be_buffer_init(&q, 64);
    postgres_write_query(&q, sql);
    bool sent = owl_postgres_send(c, &q);
    owl_be_buffer_free(&q);
    if (!sent) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_postgres_conn_last_error(c));
        return false;
    }
    return drain_query_response(c, out);
}

bool owl_postgres_extended_query(OwlPostgresConn* c,
                                  const char* sql,
                                  int n_params,
                                  const char* const* param_values,
                                  OwlPostgresResult* out) {
    owl_postgres_result_init(out);
    out->success = false;

    /* Pipeline : Parse + Bind + Describe(portal) + Execute + Sync. */
    OwlBeBuffer q; owl_be_buffer_init(&q, 128);
    postgres_write_parse(&q, "", sql, 0, NULL);  /* PG infere les types */
    postgres_write_bind_text(&q, "", "", n_params, param_values);
    postgres_write_describe(&q, 'P', "");        /* describe portal */
    postgres_write_execute(&q, "", 0);
    postgres_write_sync(&q);
    bool sent = owl_postgres_send(c, &q);
    owl_be_buffer_free(&q);
    if (!sent) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_postgres_conn_last_error(c));
        return false;
    }
    return drain_query_response(c, out);
}
