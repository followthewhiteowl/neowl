// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_postgres_conn.h"

#include "net_be_buffer.h"
#include "sql_postgres_wire.h"
#include "auth_scram.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include <mbedtls/md5.h>

/* ================================================================
 * Helpers d'erreur
 * ================================================================ */

static void pg_err(OwlPostgresConn* c, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->last_error, sizeof(c->last_error), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * Transport unifie : send/recv qui aiguille selon use_tls
 * ================================================================ */

void owl_postgres_conn_init(OwlPostgresConn* c) {
    memset(c, 0, sizeof(*c));
    owl_tls_init(&c->tls);
}

static int conn_send_raw(OwlPostgresConn* c, const void* buf, size_t len) {
    if (c->use_tls) return owl_tls_send(&c->tls,        buf, len);
    else            return owl_tcp_send(&c->tls.tcp,    buf, len);
}

static int conn_recv_raw(OwlPostgresConn* c, void* buf, size_t len) {
    if (c->use_tls) return owl_tls_recv(&c->tls,        buf, len);
    else            return owl_tcp_recv(&c->tls.tcp,    buf, len);
}

static bool conn_recv_exact(OwlPostgresConn* c, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = conn_recv_raw(c, p + total, len - total);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

bool owl_postgres_send(OwlPostgresConn* c, const OwlBeBuffer* out) {
    if (out->length == 0) return true;
    int n = conn_send_raw(c, out->data, out->length);
    if (n != (int)out->length) {
        pg_err(c, "send echec : %d/%zu octets envoyes", n, out->length);
        return false;
    }
    return true;
}

bool owl_postgres_recv_msg(OwlPostgresConn* c,
                      char* out_type,
                      uint8_t** out_payload,
                      uint32_t* out_payload_len) {
    /* Header = 1 byte type + 4 bytes length BE. */
    uint8_t hdr[5];
    if (!conn_recv_exact(c, hdr, 5)) {
        pg_err(c, "recv header tronque");
        return false;
    }
    char type = (char)hdr[0];
    uint32_t total_len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16)
                       | ((uint32_t)hdr[3] <<  8) |  (uint32_t)hdr[4];
    if (total_len < 4) {
        pg_err(c, "recv length invalide : %u", total_len);
        return false;
    }
    uint32_t payload_len = total_len - 4;
    uint8_t* payload = NULL;
    if (payload_len > 0) {
        payload = (uint8_t*)malloc(payload_len);
        if (!payload) { pg_err(c, "OOM payload %u bytes", payload_len); return false; }
        if (!conn_recv_exact(c, payload, payload_len)) {
            pg_err(c, "recv payload tronque (%u bytes attendus)", payload_len);
            free(payload);
            return false;
        }
    }
    *out_type        = type;
    *out_payload     = payload;
    *out_payload_len = payload_len;
    return true;
}

/* ================================================================
 * SSL negotiation
 *
 *   Send SSLRequest (8 bytes, code 80877103)
 *   Recv 1 byte :
 *     'S' = OK, upgrade vers TLS via STARTTLS
 *     'N' = pas de TLS sur ce serveur ; on echoue (anti-downgrade)
 * ================================================================ */

static bool conn_ssl_negotiate(OwlPostgresConn* c, const char* host, int tls_mode,
                                 const char* extra_ca_path) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 16);
    postgres_write_ssl_request(&out);
    bool sent = owl_postgres_send(c, &out);
    owl_be_buffer_free(&out);
    if (!sent) return false;

    /* La reponse a SSLRequest est UN SEUL byte (pas un message PG standard). */
    char resp;
    if (!conn_recv_exact(c, &resp, 1)) {
        pg_err(c, "ssl: reponse SSLRequest tronquee");
        return false;
    }
    if (resp == 'N') {
        pg_err(c, "ssl: serveur refuse TLS (anti-downgrade : on echoue)");
        return false;
    }
    if (resp != 'S') {
        pg_err(c, "ssl: reponse SSLRequest inattendue '%c' (0x%02x)", resp, resp);
        return false;
    }

    /* STARTTLS : on a deja le TCP connecte dans c->tls.tcp.
     * Mapping tls_mode (cote OWL) -> verify_level (cote net_tcp_tls). */
    int verify_level;
    switch (tls_mode) {
        case OWL_SQL_TLS_ACCEPTE_TOUT:    verify_level = OWL_TLS_VERIFY_NONE;  break;
        case OWL_SQL_TLS_VERIFIE_CA:      verify_level = OWL_TLS_VERIFY_CHAIN; break;
        case OWL_SQL_TLS_VERIFIE_COMPLET:
        default:                          verify_level = OWL_TLS_VERIFY_FULL;  break;
    }
    if (!owl_tls_start_tls(&c->tls, host, verify_level, extra_ca_path)) {
        pg_err(c, "ssl: handshake TLS echec : %s", owl_tls_last_error(&c->tls));
        return false;
    }
    c->use_tls = true;
    return true;
}

/* ================================================================
 * Auth : helpers MD5 + SCRAM
 * ================================================================ */

/* PG MD5 password : "md5" + md5_hex(md5_hex(password + user) + salt) */
static bool compute_md5_password(const char* user, const char* password,
                                  const uint8_t salt[4],
                                  char out[36]) {
    unsigned char hash1[16];
    unsigned char hash2[16];
    char hex1[33];

    /* Step 1 : md5(password + user) */
    char buf1[512];
    int n1 = snprintf(buf1, sizeof(buf1), "%s%s",
                      password ? password : "",
                      user ? user : "");
    if (n1 < 0 || (size_t)n1 >= sizeof(buf1)) return false;
    if (mbedtls_md5((const unsigned char*)buf1, (size_t)n1, hash1) != 0) return false;

    /* Hex encode hash1. */
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        hex1[i*2    ] = hex[(hash1[i] >> 4) & 0xF];
        hex1[i*2 + 1] = hex[ hash1[i]       & 0xF];
    }
    hex1[32] = '\0';

    /* Step 2 : md5(hex1 + salt) */
    unsigned char step2[36];
    memcpy(step2, hex1, 32);
    memcpy(step2 + 32, salt, 4);
    if (mbedtls_md5(step2, 36, hash2) != 0) return false;

    /* Result : "md5" + hex(hash2) */
    out[0] = 'm'; out[1] = 'd'; out[2] = '5';
    for (int i = 0; i < 16; i++) {
        out[3 + i*2    ] = hex[(hash2[i] >> 4) & 0xF];
        out[3 + i*2 + 1] = hex[ hash2[i]       & 0xF];
    }
    out[35] = '\0';
    return true;
}

/* SCRAM-SHA-256 : sequence complete dans la boucle auth.
 * Le payload de AuthenticationSASL contient la liste des mechanisms
 * supportes (sequence de cstrings, terminee par '\0' suplementaire).
 * On verifie que "SCRAM-SHA-256" est present. */
static bool sasl_supports_scram_sha256(const uint8_t* mechs, uint32_t mechs_len) {
    /* Parcourt les cstrings. */
    uint32_t i = 0;
    while (i < mechs_len) {
        const char* m = (const char*)(mechs + i);
        if (*m == '\0') break;  /* terminator */
        if (strcmp(m, "SCRAM-SHA-256") == 0) return true;
        size_t l = strlen(m);
        if (i + l + 1 > mechs_len) break;
        i += (uint32_t)l + 1;
    }
    return false;
}

/* ================================================================
 * Auth main loop
 * ================================================================ */

static bool conn_run_auth_and_ready(OwlPostgresConn* c,
                                     const char* user,
                                     const char* password) {
    OwlScramSha256 scram;
    bool scram_started = false;

    while (1) {
        char type;
        uint8_t* payload = NULL;
        uint32_t plen = 0;
        if (!owl_postgres_recv_msg(c, &type, &payload, &plen)) return false;

        if (type == POSTGRES_MSG_AUTH) {
            const uint8_t* sasl_data = NULL;
            uint32_t sasl_len = 0;
            int code = postgres_parse_auth(payload, plen, &sasl_data, &sasl_len);
            switch (code) {
                case POSTGRES_AUTH_OK:
                    free(payload);
                    /* On attend les ParameterStatus / BackendKeyData /
                     * ReadyForQuery dans les iterations suivantes. */
                    break;

                case POSTGRES_AUTH_CLEARTEXT_PWD: {
                    free(payload);
                    OwlBeBuffer out; owl_be_buffer_init(&out, 64);
                    postgres_write_password(&out, password ? password : "");
                    bool ok = owl_postgres_send(c, &out);
                    owl_be_buffer_free(&out);
                    if (!ok) return false;
                    break;
                }

                case POSTGRES_AUTH_MD5_PWD: {
                    /* Payload (apres les 4 bytes code) = salt 4 bytes. */
                    if (sasl_len != 4) {
                        pg_err(c, "auth MD5: salt taille inattendue %u", sasl_len);
                        free(payload);
                        return false;
                    }
                    char md5pwd[36];
                    bool ok = compute_md5_password(user, password,
                                                    sasl_data, md5pwd);
                    free(payload);
                    if (!ok) {
                        pg_err(c, "auth MD5: echec computation");
                        return false;
                    }
                    OwlBeBuffer out; owl_be_buffer_init(&out, 64);
                    postgres_write_password(&out, md5pwd);
                    ok = owl_postgres_send(c, &out);
                    owl_be_buffer_free(&out);
                    if (!ok) return false;
                    break;
                }

                case POSTGRES_AUTH_SASL: {
                    /* Payload = liste de mechanisms cstrings. */
                    if (!sasl_supports_scram_sha256(sasl_data, sasl_len)) {
                        pg_err(c, "auth SASL: serveur n'offre pas SCRAM-SHA-256");
                        free(payload);
                        return false;
                    }
                    free(payload);
                    owl_scram_init(&scram, user, password ? password : "");
                    char first_msg[512];
                    if (!owl_scram_client_first(&scram, NULL,
                                                 first_msg, sizeof(first_msg))) {
                        pg_err(c, "auth SASL: client_first echec : %s",
                               owl_scram_last_error(&scram));
                        return false;
                    }
                    OwlBeBuffer out; owl_be_buffer_init(&out, 256);
                    postgres_write_sasl_initial_response(&out,
                        "SCRAM-SHA-256", first_msg);
                    bool ok = owl_postgres_send(c, &out);
                    owl_be_buffer_free(&out);
                    if (!ok) return false;
                    scram_started = true;
                    break;
                }

                case POSTGRES_AUTH_SASL_CONTINUE: {
                    if (!scram_started) {
                        pg_err(c, "auth SASL_CONTINUE recu sans SASL init");
                        free(payload);
                        return false;
                    }
                    /* sasl_data = server-first-message (raw bytes, pas '\0' final). */
                    char server_first[1024];
                    if (sasl_len >= sizeof(server_first)) {
                        pg_err(c, "auth SASL: server-first trop gros (%u)", sasl_len);
                        free(payload);
                        return false;
                    }
                    memcpy(server_first, sasl_data, sasl_len);
                    server_first[sasl_len] = '\0';
                    free(payload);

                    char final_msg[1024];
                    if (!owl_scram_client_final(&scram, server_first,
                                                 final_msg, sizeof(final_msg))) {
                        pg_err(c, "auth SASL client_final: %s",
                               owl_scram_last_error(&scram));
                        return false;
                    }
                    OwlBeBuffer out; owl_be_buffer_init(&out, 256);
                    postgres_write_sasl_response(&out, final_msg);
                    bool ok = owl_postgres_send(c, &out);
                    owl_be_buffer_free(&out);
                    if (!ok) return false;
                    break;
                }

                case POSTGRES_AUTH_SASL_FINAL: {
                    if (!scram_started) {
                        pg_err(c, "auth SASL_FINAL recu sans SASL init");
                        free(payload);
                        return false;
                    }
                    /* sasl_data = server-final-message "v=..." */
                    char server_final[256];
                    if (sasl_len >= sizeof(server_final)) {
                        pg_err(c, "auth SASL: server-final trop gros (%u)", sasl_len);
                        free(payload);
                        return false;
                    }
                    memcpy(server_final, sasl_data, sasl_len);
                    server_final[sasl_len] = '\0';
                    free(payload);

                    if (!owl_scram_verify_server_final(&scram, server_final)) {
                        pg_err(c, "auth SASL verify: %s",
                               owl_scram_last_error(&scram));
                        return false;
                    }
                    /* Pas de send : le serveur va envoyer AuthOK ensuite. */
                    break;
                }

                default:
                    pg_err(c, "auth: sub-code %d non supporte", code);
                    free(payload);
                    return false;
            }
            continue;
        }

        if (type == POSTGRES_MSG_PARAM_STATUS) {
            /* Format : key cstring + value cstring. On ignore en V1. */
            free(payload);
            continue;
        }

        if (type == POSTGRES_MSG_BACKEND_KEY) {
            if (plen >= 8) {
                c->backend_pid = ((int32_t)payload[0] << 24) | ((int32_t)payload[1] << 16)
                               | ((int32_t)payload[2] <<  8) |  (int32_t)payload[3];
                c->cancel_key  = ((int32_t)payload[4] << 24) | ((int32_t)payload[5] << 16)
                               | ((int32_t)payload[6] <<  8) |  (int32_t)payload[7];
            }
            free(payload);
            continue;
        }

        if (type == POSTGRES_MSG_READY) {
            free(payload);
            return true;  /* connection prete pour queries */
        }

        if (type == POSTGRES_MSG_ERROR) {
            const char* m = postgres_parse_error_message(payload, plen);
            pg_err(c, "serveur ErrorResponse: %s", m ? m : "(pas de M)");
            free(payload);
            return false;
        }

        if (type == POSTGRES_MSG_NOTICE) {
            /* NoticeResponse : log et continue. V1 ignore. */
            free(payload);
            continue;
        }

        /* Type inattendu pendant le handshake. */
        pg_err(c, "auth: message inattendu '%c' (0x%02x)", type, (uint8_t)type);
        free(payload);
        return false;
    }
}

/* ================================================================
 * API publique
 * ================================================================ */

bool owl_postgres_conn_open(OwlPostgresConn* c,
                       const char* host, int port,
                       const char* user, const char* password,
                       const char* database,
                       int tls_mode,
                       const char* extra_ca_path) {
    if (!owl_tcp_global_init()) {
        pg_err(c, "tcp_global_init echec");
        return false;
    }

    /* Connect TCP en clair. */
    if (!owl_tcp_connect(&c->tls.tcp, host, port, 5000)) {
        pg_err(c, "tcp connect: %s", owl_tcp_last_error(&c->tls.tcp));
        return false;
    }

    c->use_tls = false;

    /* SSL upgrade si demande (tout mode autre que DESACTIVE). */
    if (tls_mode != OWL_SQL_TLS_DESACTIVE) {
        if (!conn_ssl_negotiate(c, host, tls_mode, extra_ca_path)) {
            owl_tcp_close(&c->tls.tcp);
            return false;
        }
        /* Apres ce point c->use_tls == true ; toute communication
         * passe par owl_tls_*. */
    }

    /* StartupMessage. */
    {
        OwlBeBuffer out; owl_be_buffer_init(&out, 128);
        postgres_write_startup(&out, user, database);
        bool ok = owl_postgres_send(c, &out);
        owl_be_buffer_free(&out);
        if (!ok) {
            owl_postgres_conn_close(c);
            return false;
        }
    }

    /* Auth loop + parameter status + ready. */
    if (!conn_run_auth_and_ready(c, user, password)) {
        owl_postgres_conn_close(c);
        return false;
    }

    c->connected = true;
    return true;
}

void owl_postgres_conn_close(OwlPostgresConn* c) {
    if (c->connected) {
        /* Terminate proprement. Ignore les erreurs de send : on ferme. */
        OwlBeBuffer out; owl_be_buffer_init(&out, 8);
        postgres_write_terminate(&out);
        (void)owl_postgres_send(c, &out);
        owl_be_buffer_free(&out);
    }
    if (c->use_tls) owl_tls_close(&c->tls);
    else            owl_tcp_close(&c->tls.tcp);
    c->connected = false;
    c->use_tls   = false;
}

const char* owl_postgres_conn_last_error(const OwlPostgresConn* c) {
    return c->last_error;
}
