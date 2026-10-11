// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mysql_conn.h"

#include "sql_mysql_wire.h"
#include "auth_mysql_native.h"
#include "auth_mysql_caching.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Setup / teardown
 * ================================================================ */

void owl_mysql_conn_init(OwlMysqlConn* c) {
    memset(c, 0, sizeof(*c));
    owl_tls_init(&c->tls);
}

const char* owl_mysql_conn_last_error(const OwlMysqlConn* c) {
    return c->last_error;
}

static void mysql_err(OwlMysqlConn* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->last_error, sizeof(c->last_error), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * Transport unifie (clear / TLS)
 * ================================================================ */

bool owl_mysql_send(OwlMysqlConn* c, const OwlLeBuffer* out) {
    int n;
    if (c->use_tls) n = owl_tls_send(&c->tls,        out->data, out->length);
    else            n = owl_tcp_send(&c->tls.tcp,    out->data, out->length);
    if (n < 0 || (size_t)n != out->length) {
        mysql_err(c, "send tronque : %d/%zu", n, out->length);
        return false;
    }
    return true;
}

static bool recv_exact(OwlMysqlConn* c, uint8_t* buf, size_t n) {
    if (c->use_tls) return owl_tls_recv_exact(&c->tls, buf, n);
    return owl_tcp_recv_exact(&c->tls.tcp, buf, n);
}

bool owl_mysql_recv_packet(OwlMysqlConn* c,
                            uint8_t** out_payload, uint32_t* out_payload_len,
                            uint8_t* out_seq) {
    *out_payload = NULL;
    *out_payload_len = 0;
    *out_seq = 0;

    /* Header 4 octets = u24 length LE + u8 seq. */
    uint8_t hdr[MYSQL_HEADER_SIZE];
    if (!recv_exact(c, hdr, MYSQL_HEADER_SIZE)) {
        mysql_err(c, "recv header echec");
        return false;
    }
    uint32_t plen = ((uint32_t)hdr[0])
                  | ((uint32_t)hdr[1] << 8)
                  | ((uint32_t)hdr[2] << 16);
    uint8_t seq = hdr[3];

    if (plen == 0) {
        *out_payload = NULL;
        *out_payload_len = 0;
        *out_seq = seq;
        return true;
    }

    uint8_t* payload = (uint8_t*)malloc(plen);
    if (!payload) {
        mysql_err(c, "recv payload alloc echec (%u octets)", plen);
        return false;
    }
    if (!recv_exact(c, payload, plen)) {
        free(payload);
        mysql_err(c, "recv payload tronque (attendu %u)", plen);
        return false;
    }
    *out_payload = payload;
    *out_payload_len = plen;
    *out_seq = seq;
    return true;
}

/* ================================================================
 * Helper : si premier byte 0xFF -> parse ErrorPacket et remplit
 * last_error. Retourne true si c'etait un Error (caller doit avorter).
 * ================================================================ */

static bool handle_error_if_present(OwlMysqlConn* c,
                                     const uint8_t* payload, uint32_t plen) {
    if (plen < 1 || payload[0] != MYSQL_PKT_ERROR) return false;
    MysqlErrorPacket err;
    if (mysql_parse_error(payload + 1, plen - 1, &err)) {
        size_t copy = err.message_len;
        if (copy > sizeof(c->last_error) - 64) copy = sizeof(c->last_error) - 64;
        snprintf(c->last_error, sizeof(c->last_error),
                 "MySQL error %u (%.5s): %.*s",
                 (unsigned)err.error_code,
                 err.sql_state ? err.sql_state : "HY000",
                 (int)copy, err.message);
    } else {
        snprintf(c->last_error, sizeof(c->last_error), "MySQL ErrorPacket malformed");
    }
    return true;
}

/* ================================================================
 * Auth response builder
 * ================================================================ */

static bool build_auth_token(OwlMysqlConn* c,
                              const char* plugin_name,
                              const char* password,
                              const uint8_t* salt, size_t salt_len,
                              uint8_t** out_token, size_t* out_token_len) {
    *out_token = NULL;
    *out_token_len = 0;

    /* Password vide : token vide (auth_response_len = 0). */
    if (password == NULL || password[0] == '\0') {
        return true;
    }

    if (plugin_name == NULL) {
        plugin_name = "mysql_native_password";
    }

    if (strcmp(plugin_name, "mysql_native_password") == 0) {
        if (salt_len < MYSQL_NATIVE_SALT_LEN) {
            mysql_err(c, "salt trop court (%zu < 20) pour native_password", salt_len);
            return false;
        }
        uint8_t* tok = (uint8_t*)malloc(MYSQL_NATIVE_TOKEN_LEN);
        if (!tok) { mysql_err(c, "OOM auth token"); return false; }
        if (owl_mysql_native_compute_token(password, strlen(password),
                                            salt, tok) != 0) {
            free(tok);
            mysql_err(c, "compute native token echec");
            return false;
        }
        *out_token = tok;
        *out_token_len = MYSQL_NATIVE_TOKEN_LEN;
        return true;
    }

    if (strcmp(plugin_name, "caching_sha2_password") == 0) {
        if (salt_len < MYSQL_CACHING_SALT_LEN) {
            mysql_err(c, "salt trop court pour caching_sha2");
            return false;
        }
        uint8_t* tok = (uint8_t*)malloc(MYSQL_CACHING_TOKEN_LEN);
        if (!tok) { mysql_err(c, "OOM auth token"); return false; }
        if (owl_mysql_caching_compute_token(password, strlen(password),
                                             salt, tok) != 0) {
            free(tok);
            mysql_err(c, "compute caching token echec");
            return false;
        }
        *out_token = tok;
        *out_token_len = MYSQL_CACHING_TOKEN_LEN;
        return true;
    }

    mysql_err(c, "plugin auth non supporte : %s", plugin_name);
    return false;
}

/* ================================================================
 * State : post-HandshakeResponse, on attend OK / Error / AuthSwitch /
 *         AuthMoreData. Boucle jusqu'a OK ou Error.
 * ================================================================ */

static bool wait_auth_complete(OwlMysqlConn* c, const char* password) {
    /* Reutilise le salt initial pour le first plugin negocie ;
     * AuthSwitch fournit son propre salt. */
    while (1) {
        uint8_t* payload = NULL;
        uint32_t plen = 0;
        uint8_t  seq  = 0;
        if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) return false;
        if (plen == 0) { free(payload); mysql_err(c, "empty packet during auth"); return false; }

        c->seq_num = seq;
        uint8_t first = payload[0];

        if (first == MYSQL_PKT_OK) {
            free(payload);
            return true;  /* Auth complete. */
        }

        if (first == MYSQL_PKT_ERROR) {
            handle_error_if_present(c, payload, plen);
            free(payload);
            return false;
        }

        if (first == MYSQL_PKT_EOF_OR_AUTH_SWITCH) {
            /* AuthSwitchRequest si pas DEPRECATE_EOF ou si plen > 1.
             * Layout : FE plugin_name\0 plugin_data. */
            MysqlAuthSwitch as;
            if (!mysql_parse_auth_switch(payload + 1, plen - 1, &as)) {
                free(payload);
                mysql_err(c, "AuthSwitchRequest malforme");
                return false;
            }
            /* Recompute le token avec le nouveau plugin + nouveau salt. */
            uint8_t* tok = NULL; size_t toklen = 0;
            if (!build_auth_token(c, as.plugin_name, password,
                                   as.plugin_data, as.plugin_data_len,
                                   &tok, &toklen)) {
                free(payload);
                return false;
            }
            free(payload);
            /* Envoi du token. seq_num doit etre seq+1. */
            OwlLeBuffer out; owl_le_buffer_init(&out, 64);
            size_t off = mysql_pkt_begin(&out, c->seq_num + 1);
            if (tok && toklen > 0) {
                owl_le_buffer_write_bytes(&out, tok, toklen);
            }
            mysql_pkt_end(&out, off);
            bool sent = owl_mysql_send(c, &out);
            owl_le_buffer_free(&out);
            free(tok);
            if (!sent) return false;
            continue;  /* attend la prochaine reponse */
        }

        if (first == MYSQL_PKT_AUTH_MORE) {
            /* caching_sha2 : 2eme byte = 0x03 fast OK, 0x04 full auth needed. */
            if (plen >= 2) {
                uint8_t code = payload[1];
                if (code == 0x03) {
                    /* fast auth success — on attend OK packet ensuite. */
                    free(payload);
                    continue;
                }
                if (code == 0x04) {
                    free(payload);
                    mysql_err(c,
                        "caching_sha2_password: serveur demande full auth "
                        "(envoi password clair). Refusé pour ne pas "
                        "leaker le password. Solution : authentifier ce user "
                        "une premiere fois via outil officiel (mysql CLI) pour "
                        "remplir le cache serveur, OU utiliser mysql_native_password.");
                    return false;
                }
            }
            free(payload);
            mysql_err(c, "AuthMoreData inattendu");
            return false;
        }

        free(payload);
        mysql_err(c, "paquet inattendu durant auth (first=0x%02x)", first);
        return false;
    }
}

/* ================================================================
 * Public API : owl_mysql_conn_open
 * ================================================================ */

bool owl_mysql_conn_open(OwlMysqlConn* c,
                          const char* host, int port,
                          const char* user, const char* password,
                          const char* database,
                          int tls_mode,
                          const char* extra_ca_path) {
    if (!owl_tcp_global_init()) {
        mysql_err(c, "tcp_global_init echec");
        return false;
    }

    /* 1. TCP connect (en clair, MySQL ne fait pas de pre-auth comme PG SSL preamble). */
    if (!owl_tcp_connect(&c->tls.tcp, host, port, 5000)) {
        mysql_err(c, "tcp connect: %s", owl_tcp_last_error(&c->tls.tcp));
        return false;
    }

    /* 2. Recv Initial Handshake (server parle en premier). */
    uint8_t* payload = NULL;
    uint32_t plen = 0;
    uint8_t  seq  = 0;
    if (!owl_mysql_recv_packet(c, &payload, &plen, &seq)) {
        owl_tcp_close(&c->tls.tcp);
        return false;
    }
    if (handle_error_if_present(c, payload, plen)) {
        free(payload);
        owl_tcp_close(&c->tls.tcp);
        return false;
    }
    MysqlHandshake hs;
    if (!mysql_parse_handshake(payload, plen, &hs)) {
        free(payload);
        mysql_err(c, "parse handshake echec");
        owl_tcp_close(&c->tls.tcp);
        return false;
    }
    c->thread_id = hs.thread_id;
    uint8_t salt[MYSQL_NATIVE_SALT_LEN];
    if (!mysql_handshake_extract_salt(payload, plen, salt)) {
        free(payload);
        mysql_err(c, "extract salt echec");
        owl_tcp_close(&c->tls.tcp);
        return false;
    }
    /* Capabilities client : on prend l'intersection avec le serveur. */
    uint32_t client_caps =
        MYSQL_CAP_PROTOCOL_41         |
        MYSQL_CAP_SECURE_CONNECTION   |
        MYSQL_CAP_PLUGIN_AUTH         |
        MYSQL_CAP_TRANSACTIONS        |
        MYSQL_CAP_LONG_FLAG           |
        MYSQL_CAP_LONG_PASSWORD;
    if (database && database[0]) {
        client_caps |= MYSQL_CAP_CONNECT_WITH_DB;
    }
    if (tls_mode != OWL_SQL_TLS_DESACTIVE) {
        if ((hs.capabilities & MYSQL_CAP_SSL) == 0) {
            free(payload);
            mysql_err(c, "TLS demande mais serveur n'annonce pas CAP_SSL");
            owl_tcp_close(&c->tls.tcp);
            return false;
        }
        client_caps |= MYSQL_CAP_SSL;
    }
    c->capabilities = client_caps & hs.capabilities;

    /* Plugin choisi : prefere ce que le serveur annonce, sinon native. */
    const char* plugin_name = hs.auth_plugin_name
                                ? hs.auth_plugin_name
                                : "mysql_native_password";
    /* Copie le nom du plugin avant de free le payload. */
    char plugin_buf[64];
    snprintf(plugin_buf, sizeof(plugin_buf), "%s", plugin_name);
    plugin_name = plugin_buf;
    free(payload);

    /* 3. TLS upgrade si demande. */
    c->seq_num = seq;  /* seq apres Handshake server = 0 */
    if (tls_mode != OWL_SQL_TLS_DESACTIVE) {
        OwlLeBuffer ssl_req; owl_le_buffer_init(&ssl_req, 64);
        mysql_write_ssl_request(&ssl_req, (uint8_t)(c->seq_num + 1),
                                 c->capabilities,
                                 16 * 1024 * 1024,
                                 hs.charset);
        bool sent = owl_mysql_send(c, &ssl_req);
        owl_le_buffer_free(&ssl_req);
        if (!sent) {
            owl_tcp_close(&c->tls.tcp);
            return false;
        }
        /* Negocie le TLS. Mapping tls_mode -> verify_level. */
        int verify_level;
        switch (tls_mode) {
            case OWL_SQL_TLS_ACCEPTE_TOUT:    verify_level = OWL_TLS_VERIFY_NONE;  break;
            case OWL_SQL_TLS_VERIFIE_CA:      verify_level = OWL_TLS_VERIFY_CHAIN; break;
            case OWL_SQL_TLS_VERIFIE_COMPLET:
            default:                          verify_level = OWL_TLS_VERIFY_FULL;  break;
        }
        if (!owl_tls_start_tls(&c->tls, host, verify_level, extra_ca_path)) {
            mysql_err(c, "TLS handshake echec : %s", owl_tls_last_error(&c->tls));
            owl_tcp_close(&c->tls.tcp);
            return false;
        }
        c->use_tls = true;
        c->seq_num = c->seq_num + 1;  /* SSLRequest a ete envoye */
    }

    /* 4. Send HandshakeResponse41 avec auth token. */
    uint8_t* tok = NULL;
    size_t   toklen = 0;
    if (!build_auth_token(c, plugin_name, password, salt, sizeof(salt),
                           &tok, &toklen)) {
        owl_mysql_conn_close(c);
        return false;
    }
    MysqlHandshakeResponse hr = {
        .client_capabilities = c->capabilities,
        .max_packet_size     = 16 * 1024 * 1024,
        .charset             = hs.charset ? hs.charset : 0x2D,  /* utf8mb4 */
        .user                = user ? user : "",
        .auth_response       = tok,
        .auth_response_len   = toklen,
        .database            = (c->capabilities & MYSQL_CAP_CONNECT_WITH_DB) ? database : NULL,
        .auth_plugin_name    = plugin_name,
    };
    OwlLeBuffer out; owl_le_buffer_init(&out, 256);
    mysql_write_handshake_response(&out, (uint8_t)(c->seq_num + 1), &hr);
    bool sent = owl_mysql_send(c, &out);
    owl_le_buffer_free(&out);
    free(tok);
    if (!sent) {
        owl_mysql_conn_close(c);
        return false;
    }
    c->seq_num = c->seq_num + 1;

    /* 5. Boucle auth (OK / AuthSwitch / AuthMoreData / Error). */
    if (!wait_auth_complete(c, password)) {
        owl_mysql_conn_close(c);
        return false;
    }

    c->connected = true;
    return true;
}

void owl_mysql_conn_close(OwlMysqlConn* c) {
    if (c->tls.tcp.connected) {
        /* Best-effort COM_QUIT (ignore les erreurs). */
        if (c->connected) {
            OwlLeBuffer out; owl_le_buffer_init(&out, 16);
            mysql_write_com_quit(&out, 0);
            (void)owl_mysql_send(c, &out);
            owl_le_buffer_free(&out);
        }
        if (c->use_tls) owl_tls_close(&c->tls);
        else            owl_tcp_close(&c->tls.tcp);
    }
    c->connected = false;
    c->use_tls   = false;
    c->seq_num   = 0;
}
