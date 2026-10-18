// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mysql_query.h"

#include "sql_mysql_wire.h"
#include "sql_mysql_types.h"
#include "net_le_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void owl_mysql_result_init(OwlMysqlResult* r) {
    memset(r, 0, sizeof(*r));
}

void owl_mysql_result_free(OwlMysqlResult* r) {
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
 * Parse ColumnDefinition41
 * ================================================================ */

static bool parse_column_def(uint8_t* payload, uint32_t plen,
                              OwlMysqlColumn* out) {
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, plen);
    size_t L;
    /* Skip catalog, schema, table, org_table (4 lenenc strings).
     * NB : lenenc_str retourne NULL pour len==0 — pas une erreur. */
    for (int i = 0; i < 4; i++) {
        (void)owl_le_buffer_read_lenenc_str(&in, &L);
    }
    /* name */
    const uint8_t* name_bytes = owl_le_buffer_read_lenenc_str(&in, &L);
    out->name = (char*)malloc(L + 1);
    if (!out->name) return false;
    if (L > 0 && name_bytes) memcpy(out->name, name_bytes, L);
    out->name[L] = '\0';
    /* org_name */
    (void)owl_le_buffer_read_lenenc_str(&in, &L);

    /* Fixed-length fields block : lenenc int 0x0c puis 12 octets. */
    uint64_t fixed_len;
    if (!owl_le_buffer_read_lenenc_int(&in, &fixed_len)) { free(out->name); return false; }
    if (fixed_len < 12) { free(out->name); return false; }
    uint16_t charset; uint32_t col_len; uint8_t type;
    uint16_t flags;   uint8_t decimals; uint16_t filler;
    if (!owl_le_buffer_read_u16(&in, &charset)) { free(out->name); return false; }
    if (!owl_le_buffer_read_u32(&in, &col_len)) { free(out->name); return false; }
    if (!owl_le_buffer_read_u8 (&in, &type))    { free(out->name); return false; }
    if (!owl_le_buffer_read_u16(&in, &flags))   { free(out->name); return false; }
    if (!owl_le_buffer_read_u8 (&in, &decimals)){ free(out->name); return false; }
    if (!owl_le_buffer_read_u16(&in, &filler))  { free(out->name); return false; }
    (void)decimals; (void)filler;

    out->charset       = charset;
    out->column_length = col_len;
    out->type          = type;
    out->flags         = flags;
    return true;
}

/* ================================================================
 * Parse DataRow (text mode)
 *
 * Layout : succession de lenenc_str. 0xFB seul = NULL.
 * On retient le payload entier dans payload_slabs (ownership transfere).
 * ================================================================ */

static bool parse_data_row(uint8_t* payload, uint32_t plen,
                            OwlMysqlResult* out) {
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, plen);

    OwlMysqlValue* values = (OwlMysqlValue*)calloc(out->column_count,
                                                     sizeof(OwlMysqlValue));
    if (!values && out->column_count > 0) return false;

    for (int i = 0; i < out->column_count; i++) {
        if (owl_le_buffer_eof(&in)) { free(values); return false; }
        uint8_t peek = in.data[in.pos];
        if (peek == MYSQL_PKT_NULL_IN_ROW) {
            /* SQL NULL : skip 1 octet. */
            owl_le_buffer_read_skip(&in, 1);
            values[i].length = -1;
            values[i].data   = NULL;
            continue;
        }
        size_t L;
        const uint8_t* d = owl_le_buffer_read_lenenc_str(&in, &L);
        if (d == NULL && L != 0) { free(values); return false; }
        values[i].length = (int32_t)L;
        values[i].data   = d;
    }

    /* Stocke le slab : on transfere l'ownership du payload. */
    if (out->payload_count >= out->payload_capacity) {
        int newCap = out->payload_capacity == 0 ? 16 : out->payload_capacity * 2;
        uint8_t** ns = (uint8_t**)realloc(out->payload_slabs,
                                            sizeof(uint8_t*) * newCap);
        if (!ns) { free(values); return false; }
        out->payload_slabs    = ns;
        out->payload_capacity = newCap;
    }
    out->payload_slabs[out->payload_count++] = payload;

    /* Append la row. */
    if (out->row_count >= out->row_capacity) {
        int newCap = out->row_capacity == 0 ? 16 : out->row_capacity * 2;
        OwlMysqlValue** nr = (OwlMysqlValue**)realloc(out->rows,
                                                       sizeof(OwlMysqlValue*) * newCap);
        if (!nr) { free(values); return false; }
        out->rows         = nr;
        out->row_capacity = newCap;
    }
    out->rows[out->row_count++] = values;
    return true;
}

/* ================================================================
 * Detection helpers : EOF, OK, Error
 * ================================================================ */

/* Un paquet de longueur courte commencant par 0xFE est un EOF legacy
 * (5 octets : FE + warnings + status). Sinon (>= 7 octets et 1er byte
 * 0xFE) c'est un OK_eof. */
static bool is_eof_packet(const uint8_t* payload, uint32_t plen) {
    if (plen < 1) return false;
    return payload[0] == MYSQL_PKT_EOF_OR_AUTH_SWITCH && plen < 9;
}

static bool is_ok_packet(const uint8_t* payload, uint32_t plen) {
    if (plen < 1) return false;
    /* 0x00 = OK direct, ou 0xFE + plen >= 9 = OK_eof. */
    if (payload[0] == MYSQL_PKT_OK) return true;
    if (payload[0] == MYSQL_PKT_EOF_OR_AUTH_SWITCH && plen >= 9) return true;
    return false;
}

/* ================================================================
 * API publique : simple_query
 * ================================================================ */

bool owl_mysql_simple_query(OwlMysqlConn* c, const char* sql, OwlMysqlResult* out) {
    owl_mysql_result_init(out);
    out->success = false;

    /* Reset seq_num pour la nouvelle conversation. */
    c->seq_num = 0;

    /* Send COM_QUERY (seq=0). */
    OwlLeBuffer q; owl_le_buffer_init(&q, 64);
    mysql_write_com_query(&q, 0, sql);
    bool sent = owl_mysql_send(c, &q);
    owl_le_buffer_free(&q);
    if (!sent) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_mysql_conn_last_error(c));
        return false;
    }

    /* Premier paquet de reponse. */
    uint8_t* payload = NULL; uint32_t plen = 0; uint8_t seq = 0;
    if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_mysql_conn_last_error(c));
        return false;
    }
    if (plen < 1) {
        free(payload);
        snprintf(out->error_message, sizeof(out->error_message), "empty packet");
        return false;
    }

    /* Premier byte : ErrorPacket / OK / ColumnCount lenenc. */
    if (payload[0] == MYSQL_PKT_ERROR) {
        MysqlErrorPacket err;
        if (mysql_parse_error(payload + 1, plen - 1, &err)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "MySQL error %u: %.*s",
                     (unsigned)err.error_code,
                     (int)err.message_len, err.message);
        }
        free(payload);
        return false;
    }
    if (is_ok_packet(payload, plen)) {
        /* INSERT/UPDATE/DELETE : pas de ResultSet, juste OK. */
        MysqlOkPacket ok;
        size_t header_skip = (payload[0] == MYSQL_PKT_OK) ? 1 : 1;
        if (mysql_parse_ok(payload + header_skip, plen - header_skip,
                            c->capabilities, &ok)) {
            out->affected_rows  = ok.affected_rows;
            out->last_insert_id = ok.last_insert_id;
        }
        free(payload);
        out->success = true;
        return true;
    }

    /* Sinon : ColumnCount (lenenc int). */
    uint64_t col_count = 0;
    {
        OwlLeBuffer in; owl_le_buffer_attach_read(&in, payload, plen);
        if (!owl_le_buffer_read_lenenc_int(&in, &col_count)) {
            free(payload);
            snprintf(out->error_message, sizeof(out->error_message),
                     "ColumnCount parse echec");
            return false;
        }
    }
    free(payload);
    out->column_count = (int)col_count;
    out->columns = (OwlMysqlColumn*)calloc(out->column_count,
                                            sizeof(OwlMysqlColumn));
    if (!out->columns && out->column_count > 0) {
        snprintf(out->error_message, sizeof(out->error_message), "alloc cols");
        return false;
    }

    /* Lit N ColumnDefinition41. */
    for (int i = 0; i < out->column_count; i++) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv ColumnDef[%d] echec", i);
            return false;
        }
        bool ok = parse_column_def(payload, plen, &out->columns[i]);
        free(payload);
        if (!ok) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "parse ColumnDef[%d] echec", i);
            return false;
        }
    }

    /* Si pas DEPRECATE_EOF : 1 EOF entre cols et rows. */
    if (!(c->capabilities & MYSQL_CAP_DEPRECATE_EOF)) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv EOF intermediaire echec");
            return false;
        }
        if (!is_eof_packet(payload, plen)) {
            free(payload);
            snprintf(out->error_message, sizeof(out->error_message),
                     "EOF intermediaire attendu, recu 0x%02x",
                     plen > 0 ? payload[0] : 0xFF);
            return false;
        }
        free(payload);
    }

    /* Boucle DataRow jusqu'a EOF/OK_eof/Error. */
    while (1) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv DataRow echec");
            return false;
        }
        if (plen < 1) { free(payload); break; }

        if (payload[0] == MYSQL_PKT_ERROR) {
            MysqlErrorPacket err;
            if (mysql_parse_error(payload + 1, plen - 1, &err)) {
                snprintf(out->error_message, sizeof(out->error_message),
                         "MySQL error mid-result %u: %.*s",
                         (unsigned)err.error_code,
                         (int)err.message_len, err.message);
            }
            free(payload);
            return false;
        }
        if (is_eof_packet(payload, plen) || is_ok_packet(payload, plen)) {
            /* OK_eof : on peut extraire affected_rows + last_insert_id. */
            if (is_ok_packet(payload, plen) &&
                payload[0] == MYSQL_PKT_EOF_OR_AUTH_SWITCH) {
                MysqlOkPacket ok;
                if (mysql_parse_ok(payload + 1, plen - 1, c->capabilities, &ok)) {
                    out->affected_rows  = ok.affected_rows;
                    out->last_insert_id = ok.last_insert_id;
                }
            }
            free(payload);
            break;
        }

        /* DataRow — ownership transfere a out->payload_slabs. */
        if (!parse_data_row(payload, plen, out)) {
            free(payload);
            snprintf(out->error_message, sizeof(out->error_message),
                     "parse DataRow echec");
            return false;
        }
        /* PAS de free(payload) ici. */
    }

    out->success = true;
    return true;
}

/* ================================================================
 * Requete preparee : COM_STMT_PREPARE + COM_STMT_EXECUTE (binary)
 * ================================================================ */

/* Parse une binary resultset row (reponse COM_STMT_EXECUTE).
 * Layout : u8 header (0x00) + null-bitmap ((n_cols + 9) / 8 octets, offset
 * de 2 bits) + valeurs binaires non-NULL. Chaque valeur est decodee vers sa
 * forme texte canonique, concatenee dans un slab par ligne ; les pointeurs
 * OwlMysqlValue.data pointent dedans. Le `payload` d'origine n'est PAS
 * retenu (contrairement au text mode) : caller le free apres retour. */
static bool parse_binary_data_row(uint8_t* payload, uint32_t plen,
                                  OwlMysqlResult* out) {
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, plen);

    uint8_t header;
    if (!owl_le_buffer_read_u8(&in, &header)) return false;  /* 0x00 */

    int ncol = out->column_count;
    size_t nb_bytes = (size_t)(ncol + 7 + 2) / 8;
    const uint8_t* nullmap = in.data + in.pos;
    if (!owl_le_buffer_read_skip(&in, nb_bytes)) return false;

    OwlMysqlValue* values = (OwlMysqlValue*)calloc(ncol ? ncol : 1,
                                                   sizeof(OwlMysqlValue));
    if (!values) return false;

    size_t* offs = (size_t*)calloc(ncol ? ncol : 1, sizeof(size_t));
    size_t* lens = (size_t*)calloc(ncol ? ncol : 1, sizeof(size_t));
    if (!offs || !lens) { free(values); free(offs); free(lens); return false; }

    OwlLeBuffer textbuf;
    owl_le_buffer_init(&textbuf, 64);

    for (int i = 0; i < ncol; i++) {
        int bit = i + 2;
        bool isnull = (nullmap[bit / 8] >> (bit % 8)) & 1u;
        if (isnull) {
            values[i].length = -1;
            values[i].data   = NULL;
            continue;
        }
        size_t off = 0, len = 0;
        if (!owl_mysql_binary_read_to_text(&in, out->columns[i].type,
                                           out->columns[i].flags,
                                           &textbuf, &off, &len)) {
            free(values); free(offs); free(lens);
            owl_le_buffer_free(&textbuf);
            return false;
        }
        offs[i] = off;
        lens[i] = len;
        values[i].length = 0;  /* marque "non-NULL", data resolu plus bas */
    }

    /* textbuf.data est stable : resout les pointeurs. */
    for (int i = 0; i < ncol; i++) {
        if (values[i].length != -1) {
            values[i].data   = textbuf.data + offs[i];
            values[i].length = (int32_t)lens[i];
        }
    }
    free(offs);
    free(lens);

    /* Transfere l'ownership de textbuf.data dans payload_slabs (steal). */
    if (out->payload_count >= out->payload_capacity) {
        int newCap = out->payload_capacity == 0 ? 16 : out->payload_capacity * 2;
        uint8_t** ns = (uint8_t**)realloc(out->payload_slabs,
                                          sizeof(uint8_t*) * newCap);
        if (!ns) { free(values); owl_le_buffer_free(&textbuf); return false; }
        out->payload_slabs    = ns;
        out->payload_capacity = newCap;
    }
    out->payload_slabs[out->payload_count++] = textbuf.data;
    /* PAS de owl_le_buffer_free(&textbuf) : ownership transfere. */

    if (out->row_count >= out->row_capacity) {
        int newCap = out->row_capacity == 0 ? 16 : out->row_capacity * 2;
        OwlMysqlValue** nr = (OwlMysqlValue**)realloc(out->rows,
                                                      sizeof(OwlMysqlValue*) * newCap);
        if (!nr) { free(values); return false; }
        out->rows         = nr;
        out->row_capacity = newCap;
    }
    out->rows[out->row_count++] = values;
    return true;
}

/* Lit et jette N ColumnDefinition41 + l'EOF intermediaire (si !DEPRECATE_EOF).
 * Utilise pour drainer les defs de params/colonnes de COM_STMT_PREPARE_OK. */
static bool skip_column_defs(OwlMysqlConn* c, int n, OwlMysqlResult* out) {
    uint8_t* payload = NULL; uint32_t plen = 0; uint8_t seq = 0;
    for (int i = 0; i < n; i++) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv def[%d] (prepare) echec", i);
            return false;
        }
        free(payload);
    }
    if (n > 0 && !(c->capabilities & MYSQL_CAP_DEPRECATE_EOF)) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv EOF defs (prepare) echec");
            return false;
        }
        free(payload);
    }
    return true;
}

bool owl_mysql_prepared_query(OwlMysqlConn* c, const char* sql,
                              int n_params, const OwlMysqlBindValue* params,
                              OwlMysqlResult* out) {
    owl_mysql_result_init(out);
    out->success = false;

    /* ---- COM_STMT_PREPARE ---- */
    c->seq_num = 0;
    OwlLeBuffer q; owl_le_buffer_init(&q, 64);
    mysql_write_com_stmt_prepare(&q, 0, sql);
    bool sent = owl_mysql_send(c, &q);
    owl_le_buffer_free(&q);
    if (!sent) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_mysql_conn_last_error(c));
        return false;
    }

    uint8_t* payload = NULL; uint32_t plen = 0; uint8_t seq = 0;
    if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_mysql_conn_last_error(c));
        return false;
    }
    if (plen < 1) {
        free(payload);
        snprintf(out->error_message, sizeof(out->error_message),
                 "prepare : paquet vide");
        return false;
    }
    if (payload[0] == MYSQL_PKT_ERROR) {
        MysqlErrorPacket err;
        if (mysql_parse_error(payload + 1, plen - 1, &err)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "MySQL error %u: %.*s", (unsigned)err.error_code,
                     (int)err.message_len, err.message);
        }
        free(payload);
        return false;
    }
    MysqlStmtPrepareOk prep;
    if (!mysql_parse_stmt_prepare_ok(payload, plen, &prep)) {
        free(payload);
        snprintf(out->error_message, sizeof(out->error_message),
                 "parse COM_STMT_PREPARE_OK echec");
        return false;
    }
    free(payload);

    /* Draine les defs de parametres puis de colonnes decrites a la prepare. */
    if (!skip_column_defs(c, prep.num_params, out)) return false;
    if (!skip_column_defs(c, prep.num_columns, out)) return false;

    /* ---- COM_STMT_EXECUTE ---- */
    c->seq_num = 0;
    OwlLeBuffer e; owl_le_buffer_init(&e, 128);
    mysql_write_com_stmt_execute(&e, 0, prep.statement_id, n_params, params);
    sent = owl_mysql_send(c, &e);
    owl_le_buffer_free(&e);
    if (!sent) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_mysql_conn_last_error(c));
        return false;
    }

    /* Premier paquet de la reponse EXECUTE. */
    if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
        snprintf(out->error_message, sizeof(out->error_message),
                 "%s", owl_mysql_conn_last_error(c));
        return false;
    }
    if (plen < 1) {
        free(payload);
        snprintf(out->error_message, sizeof(out->error_message),
                 "execute : paquet vide");
        return false;
    }
    if (payload[0] == MYSQL_PKT_ERROR) {
        MysqlErrorPacket err;
        if (mysql_parse_error(payload + 1, plen - 1, &err)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "MySQL error %u: %.*s", (unsigned)err.error_code,
                     (int)err.message_len, err.message);
        }
        free(payload);
        return false;
    }
    if (is_ok_packet(payload, plen)) {
        /* INSERT/UPDATE/DELETE : paquet OK, pas de resultset. */
        MysqlOkPacket ok;
        if (mysql_parse_ok(payload + 1, plen - 1, c->capabilities, &ok)) {
            out->affected_rows  = ok.affected_rows;
            out->last_insert_id = ok.last_insert_id;
        }
        free(payload);
        out->success = true;
        return true;
    }

    /* Sinon : binary resultset. 1er paquet = ColumnCount (lenenc int). */
    uint64_t col_count = 0;
    {
        OwlLeBuffer cc; owl_le_buffer_attach_read(&cc, payload, plen);
        if (!owl_le_buffer_read_lenenc_int(&cc, &col_count)) {
            free(payload);
            snprintf(out->error_message, sizeof(out->error_message),
                     "ColumnCount (execute) parse echec");
            return false;
        }
    }
    free(payload);
    out->column_count = (int)col_count;
    out->columns = (OwlMysqlColumn*)calloc(out->column_count ? out->column_count : 1,
                                           sizeof(OwlMysqlColumn));
    if (!out->columns) {
        snprintf(out->error_message, sizeof(out->error_message), "alloc cols");
        return false;
    }

    for (int i = 0; i < out->column_count; i++) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv ColumnDef[%d] (execute) echec", i);
            return false;
        }
        bool okc = parse_column_def(payload, plen, &out->columns[i]);
        free(payload);
        if (!okc) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "parse ColumnDef[%d] (execute) echec", i);
            return false;
        }
    }

    if (!(c->capabilities & MYSQL_CAP_DEPRECATE_EOF)) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv EOF intermediaire (execute) echec");
            return false;
        }
        if (!is_eof_packet(payload, plen)) {
            free(payload);
            snprintf(out->error_message, sizeof(out->error_message),
                     "EOF intermediaire attendu (execute), recu 0x%02x",
                     plen > 0 ? payload[0] : 0xFF);
            return false;
        }
        free(payload);
    }

    /* Boucle des binary rows jusqu'a EOF / OK_eof / Error.
     * En binary protocol une row commence par 0x00 (donc on ne peut PAS
     * reutiliser is_ok_packet ici : il confondrait une row avec un OK). */
    while (1) {
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "recv binary DataRow echec");
            return false;
        }
        if (plen < 1) { free(payload); break; }
        uint8_t b = payload[0];
        if (b == MYSQL_PKT_ERROR) {
            MysqlErrorPacket err;
            if (mysql_parse_error(payload + 1, plen - 1, &err)) {
                snprintf(out->error_message, sizeof(out->error_message),
                         "MySQL error mid-result %u: %.*s",
                         (unsigned)err.error_code,
                         (int)err.message_len, err.message);
            }
            free(payload);
            return false;
        }
        if (b == MYSQL_PKT_EOF_OR_AUTH_SWITCH) {
            /* EOF terminal (plen<9) ou OK_eof (plen>=9). */
            if (plen >= 9) {
                MysqlOkPacket ok;
                if (mysql_parse_ok(payload + 1, plen - 1, c->capabilities, &ok)) {
                    out->affected_rows  = ok.affected_rows;
                    out->last_insert_id = ok.last_insert_id;
                }
            }
            free(payload);
            break;
        }
        /* b == 0x00 : binary row. Les valeurs sont copiees en texte dans un
         * slab ; le payload n'est pas retenu. */
        bool okr = parse_binary_data_row(payload, plen, out);
        free(payload);
        if (!okr) {
            snprintf(out->error_message, sizeof(out->error_message),
                     "parse binary DataRow echec");
            return false;
        }
    }

    out->success = true;
    return true;
}
