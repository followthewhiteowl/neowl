// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mysql_wire.h"

#include <stdio.h>
#include <string.h>

/* ================================================================
 * Packet framing
 * ================================================================ */

size_t mysql_pkt_begin(OwlLeBuffer* buf, uint8_t seq_num) {
    size_t off = buf->length;
    owl_le_buffer_write_u24(buf, 0);  /* placeholder length */
    owl_le_buffer_write_u8 (buf, seq_num);
    return off;
}

void mysql_pkt_end(OwlLeBuffer* buf, size_t hdr_offset) {
    /* Payload length = bytes ecrits depuis hdr_offset + 4 (header size). */
    if (hdr_offset + MYSQL_HEADER_SIZE > buf->length) return;
    uint32_t payload_len = (uint32_t)(buf->length - hdr_offset - MYSQL_HEADER_SIZE);
    owl_le_buffer_patch_u24(buf, hdr_offset, payload_len);
}

bool mysql_read_header(OwlLeBuffer* in, uint32_t* out_payload_len, uint8_t* out_seq) {
    if (!owl_le_buffer_read_u24(in, out_payload_len)) return false;
    if (!owl_le_buffer_read_u8 (in, out_seq))         return false;
    return true;
}

/* ================================================================
 * Initial Handshake parsing
 * ================================================================ */

bool mysql_parse_handshake(const uint8_t* payload, size_t payload_len,
                            MysqlHandshake* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));

    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, payload_len);

    /* protocol_version */
    if (!owl_le_buffer_read_u8(&in, &out->protocol_version)) return false;
    if (out->protocol_version != MYSQL_PROTOCOL_VERSION) return false;

    /* server_version cstring */
    out->server_version = owl_le_buffer_read_cstring(&in);
    if (out->server_version == NULL) return false;

    /* thread_id u32 */
    if (!owl_le_buffer_read_u32(&in, &out->thread_id)) return false;

    /* auth_plugin_data_1 : 8 octets (premier morceau du salt) */
    const uint8_t* salt1 = in.data + in.pos;
    if (!owl_le_buffer_read_skip(&in, 8)) return false;

    /* filler 0x00 */
    uint8_t filler;
    if (!owl_le_buffer_read_u8(&in, &filler)) return false;

    /* capabilities low (u16) */
    uint16_t cap_low;
    if (!owl_le_buffer_read_u16(&in, &cap_low)) return false;
    out->capabilities = (uint32_t)cap_low;

    /* Si pas plus de bytes : protocole tres ancien, on s'arrete (incompatible). */
    if (owl_le_buffer_eof(&in)) {
        /* Pre-4.1, on ne supporte pas. */
        return false;
    }

    /* charset (u8) */
    if (!owl_le_buffer_read_u8(&in, &out->charset)) return false;

    /* status_flags (u16) */
    if (!owl_le_buffer_read_u16(&in, &out->status_flags)) return false;

    /* capabilities high (u16) */
    uint16_t cap_high;
    if (!owl_le_buffer_read_u16(&in, &cap_high)) return false;
    out->capabilities |= ((uint32_t)cap_high) << 16;

    /* auth_plugin_data_len (u8) — si PLUGIN_AUTH, sinon ignore et tjs 0 */
    uint8_t auth_data_len;
    if (!owl_le_buffer_read_u8(&in, &auth_data_len)) return false;

    /* reserved 10 octets */
    if (!owl_le_buffer_read_skip(&in, 10)) return false;

    /* auth_plugin_data_2 : max(13, auth_data_len - 8) octets.
     * En pratique le serveur envoie auth_data_len = 21 -> deuxieme moitie = 13.
     * Pour caching_sha2_password le salt total fait 20 octets (8+12) ou 32. */
    size_t part2_len = (auth_data_len >= 8 + 13) ? (size_t)(auth_data_len - 8) : 13;
    /* Borne par octets restants pour ne pas deborder. */
    size_t remaining = owl_le_buffer_remaining(&in);
    if (part2_len > remaining) part2_len = remaining;

    /* Le salt MySQL contient un trailing \0 dans la partie 2 — on l'exclut
     * du auth_data utile (les 20 ou 32 octets reels). En pratique
     * part2_len = 13 dont 12 utiles + 1 NUL. */
    size_t salt2_useful = (part2_len > 0) ? part2_len - 1 : 0;

    /* Reconstruit le salt complet en chainant salt1 + salt2. On stocke un
     * pointeur dans le payload : comme part1 et part2 ne sont PAS contigus
     * (filler+cap+charset+status entre), on doit retourner les 2 parts.
     * Pour simplifier : la spec garantit que les 2 parts SONT consecutifs
     * en memoire dans le payload — wait, non, elles ne le sont pas.
     *
     * Solution : on retourne le pointeur sur salt1 et auth_data_len total.
     * Le caller doit savoir qu'il faut concat. SAUF que le caller veut
     * un buffer contigu pour le hash. On copie alors lors de l'auth, pas ici.
     *
     * V1 : on expose la part 1 dans auth_data + une convention "le salt
     * total fait part1 + part2 mais en payload non-contigu". Le caller
     * (auth_mysql_*) saura reconstruire car il connait la structure.
     *
     * Mieux : pointe sur le debut du salt1, et store la longueur TOTAL
     * utile (8 + salt2_useful) en assumant que c'est contigu — c'est FAUX
     * mais la spec garantit que les bytes du salt sont uniques (8 puis
     * 13 max) et on peut les recoller via offsets connus.
     *
     * Pour V1 (mysql_native_password), on a juste besoin de 20 octets.
     * Les 8 premiers via salt1, les 12 suivants via salt2 (avant le NUL).
     * On copie dans un buffer dans le caller.
     *
     * Ici on expose salt1 dans auth_data + auth_data_len = 8, et le caller
     * doit relire le payload pour extraire salt2. Trop fragile.
     *
     * Final : on EXPOSE auth_data = pointeur sur un buffer alloue ? Non,
     * casse zero-copy. Pour eviter : on store dans MysqlHandshake un
     * petit buffer interne 32 octets. */
    (void)salt2_useful;

    /* Workaround : on copie le salt dans un buffer interne static — non,
     * pas thread-safe. On va plutot dire que MysqlHandshake n'est pas
     * une zero-copy struct mais une "extract once" — auth_data peut etre
     * un alias sur les premiers 8 octets, et le caller appelle un helper
     * dedie pour reconstruire le salt complet.
     *
     * Pour cette V1 : on stocke un pointeur sur salt1 + on extend mentalement
     * jusqu'a 20 octets en sachant qu'il y a 28 octets entre salt1 et salt2
     * dans le payload. Caller doit utiliser un helper. */
    out->auth_data = salt1;
    out->auth_data_len = 8;  /* part 1 uniquement, caller doit aller chercher part 2 */

    /* Pour exposer le salt complet sans complexite : on requiert que
     * caller passe par un helper mysql_handshake_extract_salt(...).
     * Pour l'instant on skip part 2 et plugin_name. */
    if (!owl_le_buffer_read_skip(&in, part2_len)) {
        /* fallback : pas assez d'octets, on sort proprement */
    }

    /* auth_plugin_name : cstring si PLUGIN_AUTH dans capabilities */
    if (out->capabilities & MYSQL_CAP_PLUGIN_AUTH) {
        out->auth_plugin_name = owl_le_buffer_read_cstring(&in);
        /* NULL toleré : certains serveurs n'ajoutent pas le \0 final */
    }
    return true;
}

/* Helper public : reconstruit le salt complet de 20 octets a partir du
 * payload original + l'auth_data_len byte. Ecrit dans out_salt (taille
 * au moins 20 octets). Retourne true si extraction OK. */
bool mysql_handshake_extract_salt(const uint8_t* payload, size_t payload_len,
                                   uint8_t out_salt[20]);

bool mysql_handshake_extract_salt(const uint8_t* payload, size_t payload_len,
                                   uint8_t out_salt[20]) {
    /* Layout fixe du Initial Handshake v10 :
     *   [0]   protocol_version
     *   [1..] server_version cstring (variable)
     *         après le \0 :
     *   [+0..3]   thread_id u32
     *   [+4..11]  salt part 1 (8 octets)
     *   [+12]     filler
     *   [+13..14] cap_low
     *   [+15]     charset
     *   [+16..17] status
     *   [+18..19] cap_high
     *   [+20]     auth_data_len
     *   [+21..30] reserved (10 zeros)
     *   [+31..]   salt part 2 (max(13, auth_data_len - 8) octets, dont NUL final)
     */
    if (payload_len < 2) return false;
    if (payload[0] != MYSQL_PROTOCOL_VERSION) return false;

    /* Trouver fin de server_version cstring */
    size_t i = 1;
    while (i < payload_len && payload[i] != 0) i++;
    if (i >= payload_len) return false;
    size_t after_sv = i + 1;  /* après le \0 */

    /* Salt part 1 = octets [after_sv + 4 .. after_sv + 11] (8 octets) */
    if (after_sv + 12 > payload_len) return false;
    memcpy(out_salt, payload + after_sv + 4, 8);

    /* Salt part 2 = octets [after_sv + 31 .. after_sv + 31 + 12] (12 octets utiles) */
    if (after_sv + 31 + 12 > payload_len) return false;
    memcpy(out_salt + 8, payload + after_sv + 31, 12);
    return true;
}

/* ================================================================
 * HandshakeResponse41
 * ================================================================ */

void mysql_write_handshake_response(OwlLeBuffer* buf, uint8_t seq,
                                     const MysqlHandshakeResponse* hr) {
    size_t off = mysql_pkt_begin(buf, seq);

    owl_le_buffer_write_u32(buf, hr->client_capabilities);
    owl_le_buffer_write_u32(buf, hr->max_packet_size);
    owl_le_buffer_write_u8 (buf, hr->charset);
    /* 23 octets reserves a 0 */
    for (int i = 0; i < 23; i++) owl_le_buffer_write_u8(buf, 0);
    /* username cstring */
    owl_le_buffer_write_cstring(buf, hr->user ? hr->user : "");
    /* auth_response : lenenc si PLUGIN_AUTH_LENENC_DATA, sinon u8 length + bytes */
    if (hr->client_capabilities & MYSQL_CAP_PLUGIN_AUTH_LENENC_DATA) {
        owl_le_buffer_write_lenenc_int(buf, (uint64_t)hr->auth_response_len);
    } else if (hr->client_capabilities & MYSQL_CAP_SECURE_CONNECTION) {
        owl_le_buffer_write_u8(buf, (uint8_t)hr->auth_response_len);
    } else {
        /* pre-4.1 : null-terminated, rare aujourd'hui */
    }
    if (hr->auth_response && hr->auth_response_len > 0) {
        owl_le_buffer_write_bytes(buf, hr->auth_response, hr->auth_response_len);
    }
    /* database cstring si CONNECT_WITH_DB */
    if (hr->client_capabilities & MYSQL_CAP_CONNECT_WITH_DB) {
        owl_le_buffer_write_cstring(buf, hr->database ? hr->database : "");
    }
    /* plugin_name cstring si PLUGIN_AUTH */
    if (hr->client_capabilities & MYSQL_CAP_PLUGIN_AUTH) {
        owl_le_buffer_write_cstring(buf, hr->auth_plugin_name ? hr->auth_plugin_name : "");
    }

    mysql_pkt_end(buf, off);
}

/* ================================================================
 * SSL Request
 * ================================================================ */

void mysql_write_ssl_request(OwlLeBuffer* buf, uint8_t seq,
                              uint32_t client_capabilities,
                              uint32_t max_packet_size,
                              uint8_t charset) {
    size_t off = mysql_pkt_begin(buf, seq);
    /* SSL Request a meme layout que HandshakeResponse41 mais s'arrete apres
     * les 23 octets reserves (pas de user / auth / db). */
    owl_le_buffer_write_u32(buf, client_capabilities | MYSQL_CAP_SSL);
    owl_le_buffer_write_u32(buf, max_packet_size);
    owl_le_buffer_write_u8 (buf, charset);
    for (int i = 0; i < 23; i++) owl_le_buffer_write_u8(buf, 0);
    mysql_pkt_end(buf, off);
}

/* ================================================================
 * Commands
 * ================================================================ */

void mysql_write_com_query(OwlLeBuffer* buf, uint8_t seq, const char* sql) {
    size_t off = mysql_pkt_begin(buf, seq);
    owl_le_buffer_write_u8(buf, MYSQL_COM_QUERY);
    if (sql) {
        owl_le_buffer_write_bytes(buf, sql, strlen(sql));
    }
    mysql_pkt_end(buf, off);
}

void mysql_write_com_quit(OwlLeBuffer* buf, uint8_t seq) {
    size_t off = mysql_pkt_begin(buf, seq);
    owl_le_buffer_write_u8(buf, MYSQL_COM_QUIT);
    mysql_pkt_end(buf, off);
}

void mysql_write_com_ping(OwlLeBuffer* buf, uint8_t seq) {
    size_t off = mysql_pkt_begin(buf, seq);
    owl_le_buffer_write_u8(buf, MYSQL_COM_PING);
    mysql_pkt_end(buf, off);
}

/* ================================================================
 * Requetes preparees
 * ================================================================ */

void mysql_write_com_stmt_prepare(OwlLeBuffer* buf, uint8_t seq, const char* sql) {
    size_t off = mysql_pkt_begin(buf, seq);
    owl_le_buffer_write_u8(buf, MYSQL_COM_STMT_PREPARE);
    if (sql) {
        owl_le_buffer_write_bytes(buf, sql, strlen(sql));
    }
    mysql_pkt_end(buf, off);
}

void mysql_write_com_stmt_execute(OwlLeBuffer* buf, uint8_t seq,
                                  uint32_t statement_id,
                                  int n_params,
                                  const OwlMysqlBindValue* params) {
    size_t off = mysql_pkt_begin(buf, seq);
    owl_le_buffer_write_u8 (buf, MYSQL_COM_STMT_EXECUTE);
    owl_le_buffer_write_u32(buf, statement_id);
    owl_le_buffer_write_u8 (buf, 0x00);   /* flags : CURSOR_TYPE_NO_CURSOR */
    owl_le_buffer_write_u32(buf, 1);      /* iteration_count (toujours 1) */

    if (n_params > 0) {
        /* null-bitmap : 1 bit par parametre, set si NULL. */
        int nb_bytes = (n_params + 7) / 8;
        for (int i = 0; i < nb_bytes; i++) {
            uint8_t byte = 0;
            for (int b = 0; b < 8; b++) {
                int idx = i * 8 + b;
                if (idx < n_params && params[idx].kind == OWL_MYSQL_BIND_NULL) {
                    byte |= (uint8_t)(1u << b);
                }
            }
            owl_le_buffer_write_u8(buf, byte);
        }

        /* new_params_bound_flag : 1 -> les types suivent. */
        owl_le_buffer_write_u8(buf, 0x01);

        /* Un u16 (type + flag) par parametre. */
        for (int i = 0; i < n_params; i++) {
            owl_le_buffer_write_u16(buf, owl_mysql_bind_type_code(&params[i]));
        }

        /* Les valeurs liees (hors NULL). */
        for (int i = 0; i < n_params; i++) {
            if (params[i].kind != OWL_MYSQL_BIND_NULL) {
                owl_mysql_bind_write_value(buf, &params[i]);
            }
        }
    }

    mysql_pkt_end(buf, off);
}

bool mysql_parse_stmt_prepare_ok(const uint8_t* payload, size_t payload_len,
                                 MysqlStmtPrepareOk* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, payload_len);

    uint8_t status;
    if (!owl_le_buffer_read_u8(&in, &status)) return false;
    if (status != 0x00) return false;   /* COM_STMT_PREPARE_OK attendu */
    if (!owl_le_buffer_read_u32(&in, &out->statement_id)) return false;
    if (!owl_le_buffer_read_u16(&in, &out->num_columns))  return false;
    if (!owl_le_buffer_read_u16(&in, &out->num_params))   return false;
    /* reserved_1 (0x00) puis warning_count : tolerants si tronque. */
    uint8_t reserved;
    (void)owl_le_buffer_read_u8(&in, &reserved);
    (void)owl_le_buffer_read_u16(&in, &out->warnings);
    return true;
}

/* ================================================================
 * OK packet parsing
 * ================================================================ */

bool mysql_parse_ok(const uint8_t* payload, size_t payload_len,
                     uint32_t client_capabilities, MysqlOkPacket* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, payload_len);

    if (!owl_le_buffer_read_lenenc_int(&in, &out->affected_rows)) return false;
    if (!owl_le_buffer_read_lenenc_int(&in, &out->last_insert_id)) return false;

    if (client_capabilities & MYSQL_CAP_PROTOCOL_41) {
        if (!owl_le_buffer_read_u16(&in, &out->status_flags)) return false;
        if (!owl_le_buffer_read_u16(&in, &out->warnings))     return false;
    } else if (client_capabilities & MYSQL_CAP_TRANSACTIONS) {
        if (!owl_le_buffer_read_u16(&in, &out->status_flags)) return false;
    }

    /* info string : reste du packet (EOF-terminated) */
    out->info     = (const char*)(in.data + in.pos);
    out->info_len = owl_le_buffer_remaining(&in);
    return true;
}

/* ================================================================
 * Error packet parsing
 * ================================================================ */

bool mysql_parse_error(const uint8_t* payload, size_t payload_len,
                        MysqlErrorPacket* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, payload_len);

    if (!owl_le_buffer_read_u16(&in, &out->error_code)) return false;

    /* Si protocole 4.1 : sql_state_marker '#' + 5 chars sql_state */
    if (owl_le_buffer_remaining(&in) >= 6 && in.data[in.pos] == '#') {
        owl_le_buffer_read_skip(&in, 1);  /* '#' */
        out->sql_state = (const char*)(in.data + in.pos);
        owl_le_buffer_read_skip(&in, 5);
    }

    out->message     = (const char*)(in.data + in.pos);
    out->message_len = owl_le_buffer_remaining(&in);
    return true;
}

/* ================================================================
 * AuthSwitchRequest
 * ================================================================ */

bool mysql_parse_auth_switch(const uint8_t* payload, size_t payload_len,
                              MysqlAuthSwitch* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, payload_len);

    out->plugin_name = owl_le_buffer_read_cstring(&in);
    if (out->plugin_name == NULL) return false;
    /* Reste = plugin_data (typiquement le salt 20 octets pour mysql_native). */
    out->plugin_data     = in.data + in.pos;
    out->plugin_data_len = owl_le_buffer_remaining(&in);
    /* Le serveur peut ajouter un \0 final au salt ; on retire si present. */
    if (out->plugin_data_len > 0 &&
        out->plugin_data[out->plugin_data_len - 1] == 0) {
        out->plugin_data_len--;
    }
    return true;
}
