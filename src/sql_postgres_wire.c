// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_postgres_wire.h"

#include <string.h>

/* ================================================================
 * Write helpers
 * ================================================================ */

size_t postgres_msg_begin(OwlBeBuffer* buf, char type) {
    owl_be_buffer_write_u8(buf, (uint8_t)type);
    size_t len_offset = buf->length;
    owl_be_buffer_write_u32(buf, 0);  /* placeholder, patched in postgres_msg_end */
    return len_offset;
}

size_t postgres_msg_begin_no_type(OwlBeBuffer* buf) {
    size_t len_offset = buf->length;
    owl_be_buffer_write_u32(buf, 0);  /* placeholder */
    return len_offset;
}

void postgres_msg_end(OwlBeBuffer* buf, size_t len_offset) {
    /* Le champ length couvre depuis lui-meme jusqu'a la fin du message,
     * mais N'INCLUT PAS le byte de type qui le precede. Donc :
     *   length = buf.length - len_offset */
    uint32_t length = (uint32_t)(buf->length - len_offset);
    owl_be_buffer_patch_u32(buf, len_offset, length);
}

/* ----------------- Constructeurs ----------------- */

void postgres_write_startup(OwlBeBuffer* buf, const char* user, const char* database) {
    size_t off = postgres_msg_begin_no_type(buf);
    owl_be_buffer_write_u32(buf, POSTGRES_PROTOCOL_VERSION);
    /* Paramètres clé/valeur, terminés par un '\0' supplémentaire après
     * la dernière valeur. */
    if (user && *user) {
        owl_be_buffer_write_cstring(buf, "user");
        owl_be_buffer_write_cstring(buf, user);
    }
    if (database && *database) {
        owl_be_buffer_write_cstring(buf, "database");
        owl_be_buffer_write_cstring(buf, database);
    }
    /* Terminator : 1 byte '\0' final. */
    owl_be_buffer_write_u8(buf, 0);
    postgres_msg_end(buf, off);
}

void postgres_write_ssl_request(OwlBeBuffer* buf) {
    /* SSLRequest : length=8, code=80877103 (i32 BE). Pas de type byte. */
    owl_be_buffer_write_u32(buf, 8);
    owl_be_buffer_write_u32(buf, (uint32_t)POSTGRES_SSL_REQUEST_CODE);
}

void postgres_write_password(OwlBeBuffer* buf, const char* password) {
    /* PasswordMessage : type 'p', payload = cstring (cleartext ou
     * "md5XXXX...") suivant le sous-code d'authentication. */
    size_t off = postgres_msg_begin(buf, 'p');
    owl_be_buffer_write_cstring(buf, password ? password : "");
    postgres_msg_end(buf, off);
}

void postgres_write_sasl_initial_response(OwlBeBuffer* buf,
                                     const char* mechanism,
                                     const char* sasl_first_msg) {
    /* SASLInitialResponse : type 'p' (meme que PasswordMessage), payload :
     *   - mechanism name (cstring)
     *   - length du SASL first message (i32 BE)
     *   - SASL first message (bytes, sans \0 final)
     * Si pas de first_msg : length = -1 et pas de bytes. */
    size_t off = postgres_msg_begin(buf, 'p');
    owl_be_buffer_write_cstring(buf, mechanism ? mechanism : "");
    if (sasl_first_msg) {
        size_t len = strlen(sasl_first_msg);
        owl_be_buffer_write_i32(buf, (int32_t)len);
        owl_be_buffer_write_bytes(buf, sasl_first_msg, len);
    } else {
        owl_be_buffer_write_i32(buf, -1);
    }
    postgres_msg_end(buf, off);
}

void postgres_write_sasl_response(OwlBeBuffer* buf, const char* sasl_resp) {
    /* SASLResponse : type 'p', payload = bytes SASL (pas de cstring). */
    size_t off = postgres_msg_begin(buf, 'p');
    if (sasl_resp) {
        owl_be_buffer_write_bytes(buf, sasl_resp, strlen(sasl_resp));
    }
    postgres_msg_end(buf, off);
}

void postgres_write_query(OwlBeBuffer* buf, const char* sql) {
    size_t off = postgres_msg_begin(buf, 'Q');
    owl_be_buffer_write_cstring(buf, sql ? sql : "");
    postgres_msg_end(buf, off);
}

void postgres_write_terminate(OwlBeBuffer* buf) {
    /* Terminate : type 'X', payload vide (length=4, juste les 4 bytes). */
    size_t off = postgres_msg_begin(buf, 'X');
    postgres_msg_end(buf, off);
}

void postgres_write_parse(OwlBeBuffer* buf,
                          const char* stmt_name,
                          const char* sql,
                          int n_param_types,
                          const uint32_t* param_type_oids) {
    size_t off = postgres_msg_begin(buf, 'P');
    owl_be_buffer_write_cstring(buf, stmt_name ? stmt_name : "");
    owl_be_buffer_write_cstring(buf, sql ? sql : "");
    int n = n_param_types < 0 ? 0 : n_param_types;
    owl_be_buffer_write_i16(buf, (int16_t)n);
    for (int i = 0; i < n; i++) {
        owl_be_buffer_write_u32(buf, param_type_oids ? param_type_oids[i] : 0);
    }
    postgres_msg_end(buf, off);
}

void postgres_write_bind_text(OwlBeBuffer* buf,
                              const char* portal_name,
                              const char* stmt_name,
                              int n_params,
                              const char* const* param_values) {
    /* Bind, format text (code 0) pour tous params + tous results. */
    size_t off = postgres_msg_begin(buf, 'B');
    owl_be_buffer_write_cstring(buf, portal_name ? portal_name : "");
    owl_be_buffer_write_cstring(buf, stmt_name ? stmt_name : "");
    owl_be_buffer_write_i16(buf, 0);  /* nb format codes params : 0 = text global */
    int n = n_params < 0 ? 0 : n_params;
    owl_be_buffer_write_i16(buf, (int16_t)n);
    for (int i = 0; i < n; i++) {
        const char* v = param_values ? param_values[i] : NULL;
        if (v == NULL) {
            owl_be_buffer_write_i32(buf, -1);    /* SQL NULL */
        } else {
            size_t len = strlen(v);
            owl_be_buffer_write_i32(buf, (int32_t)len);
            if (len > 0) owl_be_buffer_write_bytes(buf, (const uint8_t*)v, len);
        }
    }
    owl_be_buffer_write_i16(buf, 0);  /* nb format codes results : 0 = text global */
    postgres_msg_end(buf, off);
}

void postgres_write_describe(OwlBeBuffer* buf, char kind, const char* name) {
    size_t off = postgres_msg_begin(buf, 'D');
    uint8_t k = (uint8_t)kind;
    owl_be_buffer_write_bytes(buf, &k, 1);
    owl_be_buffer_write_cstring(buf, name ? name : "");
    postgres_msg_end(buf, off);
}

void postgres_write_execute(OwlBeBuffer* buf, const char* portal_name, int32_t max_rows) {
    size_t off = postgres_msg_begin(buf, 'E');
    owl_be_buffer_write_cstring(buf, portal_name ? portal_name : "");
    owl_be_buffer_write_i32(buf, max_rows);  /* 0 = pas de limite */
    postgres_msg_end(buf, off);
}

void postgres_write_sync(OwlBeBuffer* buf) {
    size_t off = postgres_msg_begin(buf, 'S');
    postgres_msg_end(buf, off);
}

/* ================================================================
 * Read helpers
 * ================================================================ */

bool postgres_read_msg_header(OwlBeBuffer* in, char* out_type, uint32_t* out_payload_len) {
    uint8_t  t;
    uint32_t total_len;
    if (!owl_be_buffer_read_u8 (in, &t))         return false;
    if (!owl_be_buffer_read_u32(in, &total_len)) return false;
    /* La length inclut elle-même mais pas le type byte. Le payload
     * effectif est total_len - 4. Si total_len < 4 c'est un message
     * malformé. */
    if (total_len < 4) return false;
    *out_type        = (char)t;
    *out_payload_len = total_len - 4;
    return true;
}

int postgres_parse_auth(const uint8_t* payload, uint32_t payload_len,
                   const uint8_t** out_sasl_data, uint32_t* out_sasl_len) {
    if (payload_len < 4) return -1;
    uint32_t code = ((uint32_t)payload[0] << 24)
                  | ((uint32_t)payload[1] << 16)
                  | ((uint32_t)payload[2] <<  8)
                  | ((uint32_t)payload[3]);
    if (out_sasl_data) *out_sasl_data = payload + 4;
    if (out_sasl_len)  *out_sasl_len  = payload_len - 4;
    return (int)code;
}

const char* postgres_parse_error_message(const uint8_t* payload, uint32_t payload_len) {
    /* Format ErrorResponse : suite de [field-code:1 byte][value:cstring]
     * terminée par un byte 0. On cherche field-code = 'M'. */
    uint32_t i = 0;
    while (i < payload_len) {
        uint8_t code = payload[i++];
        if (code == 0) break;  /* terminator */
        if (i >= payload_len) break;
        const char* val = (const char*)(payload + i);
        /* Avance jusqu'au '\0' qui termine le champ. */
        while (i < payload_len && payload[i] != 0) i++;
        if (i >= payload_len) break;  /* malformé */
        if (code == 'M') return val;
        i++;  /* skip '\0' */
    }
    return NULL;
}
