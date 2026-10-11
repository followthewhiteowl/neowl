// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_conn.h"
#include "sql_mssql_packet.h"
#include "sql_mssql_prelogin.h"
#include "sql_mssql_login.h"
#include "net_tcp_tls.h"

#include <mbedtls/error.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

/* Callback verify accept-all : utilise en mode TLS_MODE_ACCEPTE_TOUT.
 * Force flags=0 (= validation OK) sans tester le cert. */
static int mssql_tls_accept_all_cb(void* data, mbedtls_x509_crt* crt,
                                    int depth, uint32_t* flags) {
    (void)data; (void)crt; (void)depth;
    if (flags) *flags = 0;
    return 0;
}

/* ================================================================
 * Modes TLS exposes cote OWL — alignes Postgres sslmode.
 * Reprise des valeurs de net_tcp_tls.h pour eviter cycle d'include.
 * ================================================================ */
#define TLS_MODE_DESACTIVE       0
#define TLS_MODE_ACCEPTE_TOUT    1
#define TLS_MODE_VERIFIE_CA      2
#define TLS_MODE_VERIFIE_COMPLET 3

/* ================================================================
 * Helpers
 * ================================================================ */

static void conn_err(OwlMssqlConn* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->last_error, sizeof(c->last_error), fmt, ap);
    va_end(ap);
}

static void conn_err_mbed(OwlMssqlConn* c, int rc, const char* prefix) {
    char buf[160];
    mbedtls_strerror(rc, buf, sizeof(buf));
    snprintf(c->last_error, sizeof(c->last_error),
             "%s: %s (code -0x%04x)", prefix, buf, -rc);
}

/* ================================================================
 * Transport raw TCP (utilise par le BIO TLS).
 *
 * Signatures alignees sur OwlTdsTcpSendFn / RecvFn.
 * ================================================================ */
static int conn_tcp_send_cb(void* ctx, const uint8_t* data, size_t len) {
    OwlTcpSocket* s = (OwlTcpSocket*)ctx;
    return owl_tcp_send(s, data, len);
}

static int conn_tcp_recv_cb(void* ctx, uint8_t* buf, size_t cap) {
    OwlTcpSocket* s = (OwlTcpSocket*)ctx;
    return owl_tcp_recv(s, buf, cap);
}

/* ================================================================
 * Send/recv unifies : passe par TLS si active, sinon TCP brut.
 * ================================================================ */

static bool conn_send_all(OwlMssqlConn* c, const uint8_t* data, size_t len) {
    if (c->tls_active) {
        size_t sent = 0;
        while (sent < len) {
            int n = mbedtls_ssl_write(&c->ssl, data + sent, len - sent);
            if (n <= 0) {
                /* WANT_READ/WANT_WRITE en mode blocking ne devraient pas
                 * apparaitre mais on les boucle pour robustesse. */
                if (n == MBEDTLS_ERR_SSL_WANT_READ
                 || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                conn_err_mbed(c, n, "ssl_write");
                return false;
            }
            sent += (size_t)n;
        }
        return true;
    }
    size_t sent = 0;
    while (sent < len) {
        int n = owl_tcp_send(&c->tcp, data + sent, len - sent);
        if (n <= 0) {
            conn_err(c, "tcp_send echec apres %zu/%zu", sent, len);
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool conn_recv_exact(OwlMssqlConn* c, uint8_t* buf, size_t len) {
    if (c->tls_active) {
        size_t got = 0;
        while (got < len) {
            int n = mbedtls_ssl_read(&c->ssl, buf + got, len - got);
            if (n <= 0) {
                if (n == MBEDTLS_ERR_SSL_WANT_READ
                 || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                conn_err_mbed(c, n, "ssl_read");
                return false;
            }
            got += (size_t)n;
        }
        return true;
    }
    return owl_tcp_recv_exact(&c->tcp, buf, len);
}

/* ================================================================
 * send_message / recv_message : couches TDS packet (chunking + EOM)
 * ================================================================ */

static bool send_message(OwlMssqlConn* c, uint8_t type,
                          const uint8_t* msg, size_t len) {
    OwlTdsPacketSender s;
    uint16_t max_payload = (uint16_t)(c->packet_size - TDS_HEADER_SIZE);
    if (c->packet_size == 0) max_payload = TDS_DEFAULT_PKT_SIZE - TDS_HEADER_SIZE;
    owl_tds_packet_sender_init(&s, type, max_payload);

    size_t pkt_cap = TDS_HEADER_SIZE + max_payload;
    uint8_t* scratch = (uint8_t*)malloc(pkt_cap);
    if (!scratch) { conn_err(c, "OOM scratch send"); return false; }

    size_t cursor = 0;
    bool ok = true;
    if (len == 0) {
        size_t n = owl_tds_packet_sender_emit(&s, scratch, pkt_cap, NULL, 0, true);
        ok = (n > 0) && conn_send_all(c, scratch, n);
    }
    while (ok && cursor < len) {
        size_t chunk = (len - cursor) > max_payload ? max_payload : (len - cursor);
        bool is_last = (cursor + chunk == len);
        size_t n = owl_tds_packet_sender_emit(&s, scratch, pkt_cap,
                                                msg + cursor, chunk, is_last);
        if (n == 0) { ok = false; conn_err(c, "emit packet echec"); break; }
        if (!conn_send_all(c, scratch, n)) { ok = false; break; }
        cursor += chunk;
    }
    free(scratch);
    return ok;
}

static bool recv_message(OwlMssqlConn* c,
                          uint8_t* out_type,
                          uint8_t** out_payload, size_t* out_len) {
    OwlTdsPacketReassembler rea;
    owl_tds_packet_reassembler_init(&rea);

    size_t pkt_cap = 32768;
    uint8_t* scratch = (uint8_t*)malloc(pkt_cap);
    if (!scratch) {
        conn_err(c, "OOM scratch recv");
        owl_tds_packet_reassembler_free(&rea);
        return false;
    }

    bool ok = true;
    while (!rea.complete) {
        if (!conn_recv_exact(c, scratch, TDS_HEADER_SIZE)) {
            ok = false; break;
        }
        OwlTdsPacketHeader h;
        if (!owl_tds_packet_read_header(scratch, TDS_HEADER_SIZE, &h)) {
            conn_err(c, "header TDS invalide");
            ok = false; break;
        }
        if (h.length > pkt_cap) {
            conn_err(c, "paquet recu %u > scratch %zu", h.length, pkt_cap);
            ok = false; break;
        }
        size_t payload_len = (size_t)h.length - TDS_HEADER_SIZE;
        if (payload_len > 0) {
            if (!conn_recv_exact(c, scratch + TDS_HEADER_SIZE, payload_len)) {
                ok = false; break;
            }
        }
        if (!owl_tds_packet_reassembler_push(&rea, scratch, (size_t)h.length)) {
            conn_err(c, "reassembler: %s", rea.error);
            ok = false; break;
        }
    }
    free(scratch);

    if (!ok) {
        owl_tds_packet_reassembler_free(&rea);
        return false;
    }

    *out_type    = rea.msg_type;
    *out_len     = rea.payload.length;
    *out_payload = (uint8_t*)malloc(rea.payload.length);
    if (!*out_payload) {
        conn_err(c, "OOM out_payload");
        owl_tds_packet_reassembler_free(&rea);
        return false;
    }
    memcpy(*out_payload, rea.payload.data, rea.payload.length);
    owl_tds_packet_reassembler_free(&rea);
    return true;
}

/* ================================================================
 * TLS handshake (wrappe en TDS 0x12) via mbedTLS
 *
 * Sequence :
 *   1. mbedtls_ssl_config_defaults + RNG/CA (via owl_tls_get_*)
 *   2. mbedtls_ssl_set_verify selon mode
 *   3. mbedtls_ssl_setup
 *   4. owl_tds_tls_bio_init avec wrap=true, transport=&c->tcp
 *   5. mbedtls_ssl_set_bio avec callbacks owl_tds_tls_bio_send/recv
 *   6. mbedtls_ssl_handshake (les records TLS sont wrappes en TDS 0x12
 *      via notre BIO, transparent pour mbedTLS)
 *   7. owl_tds_tls_bio_set_wrap(false) — desormais raw TCP
 *   8. c->tls_active = true ; les conn_send_all/conn_recv_exact passent
 *      par mbedtls_ssl_write/read.
 * ================================================================ */
static bool perform_tls_handshake(OwlMssqlConn* c, const char* host, int tls_mode,
                                    const char* extra_ca_path) {
    if (!owl_tls_global_init()) {
        conn_err(c, "tls_global_init echec");
        return false;
    }
    void* drbg = owl_tls_get_drbg();
    void* cacert = owl_tls_get_cacert();
    if (!drbg || !cacert) {
        conn_err(c, "tls global non initialise");
        return false;
    }

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);

    int rc = mbedtls_ssl_config_defaults(&c->ssl_conf,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { conn_err_mbed(c, rc, "ssl_config_defaults"); return false; }

    /* Verification : selon mode. ACCEPTE_TOUT = on whitelist tout.
     * VERIFIE_CA / VERIFIE_COMPLET utilisent le CA bundle. La verification
     * du hostname est implicite si on appelle mbedtls_ssl_set_hostname,
     * sinon seule la chaine est verifiee. */
    /* CA chain : par defaut, le bundle Mozilla embedded. Si l'utilisateur
     * fournit CertificatCA (path vers PEM), on charge en plus dans
     * extra_ca et on l'utilise comme racine de confiance UNIQUE (le
     * user est explicite sur sa CA, on ne mixe pas avec Mozilla pour
     * eviter les surprises).
     *
     * Pour AcceptAll, mbedTLS exige le ca_chain meme si verify=NONE
     * (sinon -0x7680 CA_CHAIN_REQUIRED). */
    mbedtls_x509_crt* chain_to_use = (mbedtls_x509_crt*)cacert;
    if (extra_ca_path && extra_ca_path[0] != '\0') {
        mbedtls_x509_crt_init(&c->extra_ca);
        int parse_rc = mbedtls_x509_crt_parse_file(&c->extra_ca, extra_ca_path);
        if (parse_rc < 0) {
            conn_err_mbed(c, parse_rc, "CertificatCA: parse PEM echec");
            return false;
        }
        c->extra_ca_loaded = true;
        chain_to_use = &c->extra_ca;
    }
    mbedtls_ssl_conf_ca_chain(&c->ssl_conf, chain_to_use, NULL);

    if (tls_mode == TLS_MODE_ACCEPTE_TOUT) {
        /* Bug mbedTLS 3.x : en TLS 1.3, authmode=NONE ne court-circuite pas
         * correctement la verification, le state machine renvoie -0x6c00
         * (INTERNAL_ERROR) ou -0x0001 (GENERIC_ERROR) sur un cert non
         * trustable. Fix identique a Postgres (net_tcp_tls.c) :
         *  1. authmode=NONE + verify callback qui efface les flags
         *     (defense en profondeur : si TLS 1.3 court-circuite mal,
         *     le callback aide)
         *  2. force TLS 1.2 max (le state machine 1.2 honore correctement
         *     l'authmode=NONE). TLS 1.2 reste robuste pour encrypt
         *     sans validation. */
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_verify(&c->ssl_conf, mssql_tls_accept_all_cb, NULL);
        mbedtls_ssl_conf_min_tls_version(&c->ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&c->ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    } else {
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    mbedtls_ssl_conf_rng(&c->ssl_conf, mbedtls_ctr_drbg_random,
                          (mbedtls_ctr_drbg_context*)drbg);

    rc = mbedtls_ssl_setup(&c->ssl, &c->ssl_conf);
    if (rc != 0) { conn_err_mbed(c, rc, "ssl_setup"); return false; }

    if (tls_mode == TLS_MODE_VERIFIE_COMPLET) {
        rc = mbedtls_ssl_set_hostname(&c->ssl, host);
        if (rc != 0) { conn_err_mbed(c, rc, "ssl_set_hostname"); return false; }
    }

    /* BIO : wrap=true au depart (handshake en TDS 0x12). */
    owl_tds_tls_bio_init(&c->bio,
                          conn_tcp_send_cb, conn_tcp_recv_cb,
                          &c->tcp, 16384);
    mbedtls_ssl_set_bio(&c->ssl, &c->bio,
                         owl_tds_tls_bio_send, owl_tds_tls_bio_recv, NULL);

    /* Handshake : peut boucler plusieurs round-trips. */
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* Si echec verification cert, mbedTLS donne un bitmask detaille
             * (NOT_TRUSTED, EXPIRED, CN_MISMATCH, ...). On l'extrait via
             * verify_info qui rend une string humaine. */
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
                char details[400];
                int n = mbedtls_x509_crt_verify_info(details, sizeof(details),
                                                      "    ", flags);
                if (n > 0 && (size_t)n < sizeof(details)) {
                    /* Nettoyer le retour de ligne final pour rester lisible. */
                    while (n > 0 && (details[n-1] == '\n' || details[n-1] == ' ')) {
                        details[--n] = '\0';
                    }
                }
                snprintf(c->last_error, sizeof(c->last_error),
                         "ssl_handshake: cert refuse (flags=0x%08x) :\n%s",
                         flags, details);
            } else {
                conn_err_mbed(c, rc, "ssl_handshake");
            }
            return false;
        }
    }

    /* Apres handshake : on passe en raw TCP (les records TLS app data
     * sont transmis tels quels sans wrapping TDS supplementaire). */
    owl_tds_tls_bio_set_wrap(&c->bio, false);
    c->tls_active = true;
    return true;
}

/* ================================================================
 * Init / close
 * ================================================================ */

void owl_mssql_conn_init(OwlMssqlConn* c) {
    memset(c, 0, sizeof(*c));
    c->packet_size = TDS_DEFAULT_PKT_SIZE;
}

void owl_mssql_conn_close(OwlMssqlConn* c) {
    if (c->tls_active) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->ssl_conf);
        owl_tds_tls_bio_free(&c->bio);
        c->tls_active = false;
    }
    if (c->extra_ca_loaded) {
        mbedtls_x509_crt_free(&c->extra_ca);
        c->extra_ca_loaded = false;
    }
    if (c->connected) {
        owl_tcp_close(&c->tcp);
    }
    c->connected = false;
}

const char* owl_mssql_conn_last_error(const OwlMssqlConn* c) {
    return c->last_error;
}

/* ================================================================
 * Open
 * ================================================================ */

bool owl_mssql_conn_open(OwlMssqlConn* c,
                           const char* host, int port,
                           const char* user, const char* password,
                           const char* database,
                           int tls_mode,
                           const char* extra_ca_path,
                           int timeout_ms) {
    if (!owl_tcp_global_init()) {
        conn_err(c, "tcp_global_init echec");
        return false;
    }

    if (port <= 0) port = OWL_MSSQL_DEFAULT_PORT;
    if (!owl_tcp_connect(&c->tcp, host, port, timeout_ms > 0 ? timeout_ms : 5000)) {
        conn_err(c, "tcp connect %s:%d: %s", host, port, owl_tcp_last_error(&c->tcp));
        return false;
    }

    OwlTdsEncryptionMode client_enc =
        (tls_mode == TLS_MODE_DESACTIVE) ? TDS_ENCRYPT_NOT_SUP : TDS_ENCRYPT_ON;

    /* PreLogin. */
    {
        OwlLeBuffer pl; owl_le_buffer_init(&pl, 64);
        OwlTdsPreloginRequest req = {
            .version_major   = 1,
            .version_minor   = 0,
            .encryption      = client_enc,
            .instance        = NULL,
            .thread_id       = 0,
            .mars            = false,
        };
        owl_tds_prelogin_pack(&pl, &req);
        bool ok = send_message(c, TDS_PKT_PRELOGIN, pl.data, pl.length);
        owl_le_buffer_free(&pl);
        if (!ok) {
            owl_tcp_close(&c->tcp);
            return false;
        }
    }

    /* Recv PreLogin response : decide si on fait TLS. */
    OwlTdsEncryptionMode server_enc = TDS_ENCRYPT_NOT_SUP;
    {
        uint8_t  resp_type;
        uint8_t* resp_payload = NULL;
        size_t   resp_len     = 0;
        if (!recv_message(c, &resp_type, &resp_payload, &resp_len)) {
            owl_tcp_close(&c->tcp);
            return false;
        }
        OwlTdsPreloginResponse pr;
        char err[128];
        bool ok = owl_tds_prelogin_parse(resp_payload, resp_len, &pr, err);
        free(resp_payload);
        if (!ok) {
            conn_err(c, "parse PreLogin response: %s", err);
            owl_tcp_close(&c->tcp);
            return false;
        }
        if (pr.encryption_present) server_enc = pr.encryption;
    }

    /* Decision encryption :
     *   client OFF + server NOT_SUP/OFF -> plain (OK)
     *   client OFF + server REQ        -> echec (serveur exige TLS)
     *   client ON  + server ON/REQ     -> handshake TLS
     *   client ON  + server OFF        -> TLS login-only — non supporté
     *   client ON  + server NOT_SUP    -> echec (serveur ne sait pas TLS)
     */
    bool do_handshake = false;
    if (client_enc == TDS_ENCRYPT_NOT_SUP) {
        if (server_enc == TDS_ENCRYPT_REQ) {
            conn_err(c, "serveur exige TLS (ENCRYPT_REQ) mais ModeTLS=tlsDesactive");
            owl_tcp_close(&c->tcp);
            return false;
        }
    } else {
        /* On a demande ENCRYPT_ON. */
        if (server_enc == TDS_ENCRYPT_ON || server_enc == TDS_ENCRYPT_REQ) {
            do_handshake = true;
        } else if (server_enc == TDS_ENCRYPT_OFF) {
            conn_err(c, "serveur propose TLS login-only (ENCRYPT_OFF) non supporté.5");
            owl_tcp_close(&c->tcp);
            return false;
        } else {
            conn_err(c, "serveur ne supporte pas TLS (ENCRYPT_NOT_SUP)");
            owl_tcp_close(&c->tcp);
            return false;
        }
    }

    if (do_handshake) {
        if (!perform_tls_handshake(c, host, tls_mode, extra_ca_path)) {
            owl_tcp_close(&c->tcp);
            return false;
        }
    }

    /* Login7. */
    {
        OwlLeBuffer login; owl_le_buffer_init(&login, 256);
        OwlMssqlLoginRequest lreq = {
            .hostname     = "OWL-CLIENT",
            .username     = user ? user : "",
            .password     = password ? password : "",
            .appname      = "OWL",
            .servername   = host,
            .database     = database ? database : "",
            .language     = "",
            .clt_int_name = "owl-mssql",
            .packet_size  = c->packet_size,
            .client_pid   = 0,
        };
        memset(lreq.client_id, 0, sizeof(lreq.client_id));
        owl_mssql_login_pack(&login, &lreq);
        bool ok = send_message(c, TDS_PKT_LOGIN7, login.data, login.length);
        owl_le_buffer_free(&login);
        if (!ok) {
            owl_mssql_conn_close(c);
            return false;
        }
    }

    {
        uint8_t  resp_type;
        uint8_t* resp_payload = NULL;
        size_t   resp_len     = 0;
        if (!recv_message(c, &resp_type, &resp_payload, &resp_len)) {
            owl_mssql_conn_close(c);
            return false;
        }
        OwlMssqlLoginResponse lr;
        char err[128];
        bool parse_ok = owl_mssql_login_parse_response(resp_payload, resp_len, &lr, err);
        free(resp_payload);
        if (!parse_ok) {
            conn_err(c, "parse Login7 response: %s", err);
            owl_mssql_conn_close(c);
            return false;
        }
        if (!lr.success) {
            conn_err(c, "Login refuse (%u) : %s",
                     (unsigned)lr.error_number, lr.error_message);
            owl_mssql_conn_close(c);
            return false;
        }
        c->server_version_major = lr.server_version_major;
        c->server_version_minor = lr.server_version_minor;
        c->server_version_build = lr.server_version_build;
        if (lr.packet_size_changed) {
            c->packet_size = lr.negotiated_packet_size;
        }
    }

    c->connected = true;
    return true;
}

/* ================================================================
 * Query
 * ================================================================ */

bool owl_mssql_conn_query_rpc(OwlMssqlConn* c,
                                const char* sql_utf8,
                                const OwlMssqlRpcParam* params, size_t n_params,
                                OwlMssqlResult* r) {
    if (!c->connected) {
        conn_err(c, "query_rpc: pas connecte");
        return false;
    }
    OwlLeBuffer pkt; owl_le_buffer_init(&pkt, 512);
    if (!owl_mssql_query_pack_rpc(&pkt, sql_utf8, params, n_params, 0)) {
        conn_err(c, "rpc pack echec");
        owl_le_buffer_free(&pkt);
        return false;
    }
    bool ok = send_message(c, TDS_PKT_RPC, pkt.data, pkt.length);
    owl_le_buffer_free(&pkt);
    if (!ok) return false;

    uint8_t  resp_type;
    uint8_t* resp_payload = NULL;
    size_t   resp_len     = 0;
    if (!recv_message(c, &resp_type, &resp_payload, &resp_len)) {
        return false;
    }
    char err[128];
    bool parse_ok = owl_mssql_query_parse_response(resp_payload, resp_len, r, err);
    free(resp_payload);
    if (!parse_ok) {
        conn_err(c, "parse response: %s", err);
        return false;
    }
    return true;
}

bool owl_mssql_conn_query(OwlMssqlConn* c, const char* sql_utf8,
                            OwlMssqlResult* r) {
    if (!c->connected) {
        conn_err(c, "query: pas connecte");
        return false;
    }

    OwlLeBuffer pkt; owl_le_buffer_init(&pkt, 256);
    if (!owl_mssql_query_pack(&pkt, sql_utf8, 0)) {
        conn_err(c, "query pack echec");
        owl_le_buffer_free(&pkt);
        return false;
    }
    bool ok = send_message(c, TDS_PKT_SQL_BATCH, pkt.data, pkt.length);
    owl_le_buffer_free(&pkt);
    if (!ok) return false;

    uint8_t  resp_type;
    uint8_t* resp_payload = NULL;
    size_t   resp_len     = 0;
    if (!recv_message(c, &resp_type, &resp_payload, &resp_len)) {
        return false;
    }
    char err[128];
    bool parse_ok = owl_mssql_query_parse_response(resp_payload, resp_len, r, err);
    free(resp_payload);
    if (!parse_ok) {
        conn_err(c, "parse response: %s", err);
        return false;
    }
    return true;
}
