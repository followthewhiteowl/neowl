// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "net_tcp_tls.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#ifdef _WIN32
  #include <windows.h>
  #include <bcrypt.h>
#else
  #include <stdio.h>
#endif

/* CA bundle embed (Mozilla) — partage avec stdlib_http.c. */
extern const unsigned char owl_ca_bundle[];
/* MUST match vendor/ca/cacert_embed.c — defini en `unsigned int` (4 octets).
 * Une declaration `size_t` (8 octets) etait silencieusement undefined behavior :
 * GCC lisait les 4 octets adjacents (== 0) et obtenait par chance la bonne
 * valeur. zig/clang/lld place la rodata differemment -> high bytes = poison
 * 0xaaaaaaaa et segfault dans mbedtls_x509_crt_parse. */
extern const unsigned int owl_ca_bundle_len;

/* ================================================================
 * Global RNG + CA chain (thread-safe lazy init).
 *
 * Duplication temporaire avec stdlib_http.c (qui a son propre
 * g_cacert/g_drbg). Refactoring pour partager prevue plus tard
 * (sortir dans net_tls_global.c).
 * ================================================================ */

static int                       g_tls_initialized = 0;
static mbedtls_x509_crt          g_cacert;
static mbedtls_entropy_context   g_entropy;
static mbedtls_ctr_drbg_context  g_drbg;

bool owl_tls_global_init(void) {
    if (g_tls_initialized) return true;

    mbedtls_x509_crt_init(&g_cacert);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);

    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char*)"owl-net-tls", 11) != 0) {
        return false;
    }
    if (mbedtls_x509_crt_parse(&g_cacert, owl_ca_bundle, owl_ca_bundle_len) < 0) {
        return false;
    }
    g_tls_initialized = 1;
    return true;
}

void* owl_tls_get_drbg(void) {
    if (!g_tls_initialized) return NULL;
    return &g_drbg;
}

void* owl_tls_get_cacert(void) {
    if (!g_tls_initialized) return NULL;
    return &g_cacert;
}

/* ================================================================
 * Helpers : mbedtls error -> string + error reporting
 * ================================================================ */

static void set_tls_error_mbed(OwlTlsSocket* s, int rc, const char* prefix) {
    char buf[160];
    mbedtls_strerror(rc, buf, sizeof(buf));
    snprintf(s->last_error, sizeof(s->last_error),
             "%s: %s (code -0x%04x)", prefix, buf, -rc);
}

static void set_tls_error_fmt(OwlTlsSocket* s, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * BIO callbacks : mbedTLS lit/ecrit via notre OwlTcpSocket.
 * mbedTLS attend des entiers retours specifiques :
 *   > 0           : bytes transferes
 *   MBEDTLS_ERR_SSL_WANT_READ : pas d'erreur, retenter (non-blocking)
 *   < 0           : erreur fatale
 * Comme notre socket est blocante, on retourne juste >0 ou <0.
 * ================================================================ */

/* Erreurs retournees aux callbacks BIO de mbedTLS. Les constantes
 * MBEDTLS_ERR_NET_*_FAILED vivent dans mbedtls/net_sockets.h, qu'on
 * n'inclut pas (on a notre propre couche TCP). On utilise
 * MBEDTLS_ERR_SSL_INTERNAL_ERROR : assez generique pour signaler
 * "transport echec" sans coupler au header net_sockets. */
/* Accept-all verify callback : force flags=0, return 0 (OK) sans tester. */
static int tls_accept_all_verify_cb(void* data, mbedtls_x509_crt* crt,
                                     int depth, uint32_t* flags) {
    (void)data; (void)crt; (void)depth;
    if (flags) *flags = 0;
    return 0;
}

static int bio_send_cb(void* ctx, const unsigned char* buf, size_t len) {
    OwlTcpSocket* tcp = (OwlTcpSocket*)ctx;
    int n = owl_tcp_send(tcp, buf, len);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return n;
}

static int bio_recv_cb(void* ctx, unsigned char* buf, size_t len) {
    OwlTcpSocket* tcp = (OwlTcpSocket*)ctx;
    int n = owl_tcp_recv(tcp, buf, len);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return n;
}

/* ================================================================
 * Instance API
 * ================================================================ */

void owl_tls_init(OwlTlsSocket* s) {
    owl_tcp_init(&s->tcp);
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    s->ssl_active = false;
    s->verify_level = OWL_TLS_VERIFY_FULL;
    s->extra_ca_loaded = false;
    s->last_error[0] = '\0';
    s->cipher_chosen[0] = '\0';
    s->version_chosen[0] = '\0';
}

/* Configure mbedTLS context + lance handshake. Pre-requis : tcp deja
 * connecte.
 * extra_ca_path : NULL ou "" -> ca_chain = bundle Mozilla (g_cacert).
 *                  Sinon : parse le PEM et utilise comme racine de
 *                  confiance UNIQUE (le caller est explicite). */
static bool tls_setup_and_handshake(OwlTlsSocket* s, const char* host,
                                     int verify_level,
                                     const char* extra_ca_path) {
    if (!owl_tls_global_init()) {
        set_tls_error_fmt(s, "tls_setup: global init failed");
        return false;
    }

    int rc = mbedtls_ssl_config_defaults(&s->conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { set_tls_error_mbed(s, rc, "ssl_config_defaults"); return false; }

    s->verify_level = verify_level;
    mbedtls_ssl_conf_authmode(&s->conf,
        (verify_level == OWL_TLS_VERIFY_NONE)
            ? MBEDTLS_SSL_VERIFY_NONE
            : MBEDTLS_SSL_VERIFY_REQUIRED);

    /* CA chain : extra_ca_path si fourni, sinon bundle Mozilla. */
    mbedtls_x509_crt* chain_to_use = &g_cacert;
    if (extra_ca_path && extra_ca_path[0] != '\0') {
        mbedtls_x509_crt_init(&s->extra_ca);
        int parse_rc = mbedtls_x509_crt_parse_file(&s->extra_ca, extra_ca_path);
        if (parse_rc < 0) {
            set_tls_error_mbed(s, parse_rc, "CertificatCA: parse PEM echec");
            return false;
        }
        s->extra_ca_loaded = true;
        chain_to_use = &s->extra_ca;
    }
    mbedtls_ssl_conf_ca_chain(&s->conf, chain_to_use, NULL);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &g_drbg);

    if (verify_level == OWL_TLS_VERIFY_NONE) {
        /* AcceptAll : mbedTLS 3.x TLS 1.3 retourne -0x6c00 (INTERNAL_ERROR)
         * quand le cert serveur n'est pas trustable, meme avec authmode=NONE
         * — bug confirme repro AWS RDS rotated CA + TLS 1.3. Le state machine
         * TLS 1.3 court-circuite mal le verify callback en pratique.
         * Fallback : force TLS 1.2 pour ce mode + callback verify qui efface
         * les flags. TLS 1.2 reste robuste pour encrypt-sans-validation. */
        mbedtls_ssl_conf_verify(&s->conf, tls_accept_all_verify_cb, NULL);
        mbedtls_ssl_conf_min_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    }

    rc = mbedtls_ssl_setup(&s->ssl, &s->conf);
    if (rc != 0) { set_tls_error_mbed(s, rc, "ssl_setup"); return false; }

    /* SNI + hostname verification :
     *   FULL  -> set_hostname(host) : SNI + cert.CN/SAN doit matcher host.
     *   CHAIN -> set_hostname(NULL) : pas de SNI, hostname ignore (cert
     *            doit etre signe par une CA trustee mais peut etre delivre
     *            pour n'importe quel name).
     *   NONE  -> set_hostname(host) pour SNI (utile pour servers multi-tenant)
     *            mais authmode=NONE court-circuite le check. */
    const char* hostname_for_check = (verify_level == OWL_TLS_VERIFY_CHAIN) ? NULL : host;
    rc = mbedtls_ssl_set_hostname(&s->ssl, hostname_for_check);
    if (rc != 0) { set_tls_error_mbed(s, rc, "set_hostname"); return false; }

    /* Bind BIO callbacks vers notre OwlTcpSocket. */
    mbedtls_ssl_set_bio(&s->ssl, &s->tcp, bio_send_cb, bio_recv_cb, NULL);

    /* Handshake loop (blocking : pas de WANT_READ/WRITE attendu). */
    while ((rc = mbedtls_ssl_handshake(&s->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
            rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            set_tls_error_mbed(s, rc, "ssl_handshake");
            return false;
        }
    }
    s->ssl_active = true;

    /* Snapshot cipher + version negocies. */
    const char* cipher = mbedtls_ssl_get_ciphersuite(&s->ssl);
    const char* ver    = mbedtls_ssl_get_version(&s->ssl);
    if (cipher) snprintf(s->cipher_chosen,  sizeof(s->cipher_chosen),  "%s", cipher);
    if (ver)    snprintf(s->version_chosen, sizeof(s->version_chosen), "%s", ver);
    return true;
}

bool owl_tls_connect(OwlTlsSocket* s, const char* host, int port,
                      int verify_level, int timeout_ms,
                      const char* extra_ca_path) {
    if (!owl_tcp_connect(&s->tcp, host, port, timeout_ms)) {
        snprintf(s->last_error, sizeof(s->last_error),
                 "tls_connect/tcp: %s", owl_tcp_last_error(&s->tcp));
        return false;
    }
    if (!tls_setup_and_handshake(s, host, verify_level, extra_ca_path)) {
        owl_tcp_close(&s->tcp);
        return false;
    }
    return true;
}

bool owl_tls_start_tls(OwlTlsSocket* s, const char* host, int verify_level,
                        const char* extra_ca_path) {
    if (!s->tcp.connected) {
        set_tls_error_fmt(s, "start_tls: tcp non connecte");
        return false;
    }
    return tls_setup_and_handshake(s, host, verify_level, extra_ca_path);
}

int owl_tls_send(OwlTlsSocket* s, const void* buf, size_t len) {
    if (!s->ssl_active) {
        set_tls_error_fmt(s, "tls_send: SSL non actif");
        return -1;
    }
    const unsigned char* p = (const unsigned char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = mbedtls_ssl_write(&s->ssl, p + total, len - total);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n < 0) { set_tls_error_mbed(s, n, "tls_send"); return -1; }
        total += (size_t)n;
    }
    return (int)total;
}

int owl_tls_recv(OwlTlsSocket* s, void* buf, size_t len) {
    if (!s->ssl_active) {
        set_tls_error_fmt(s, "tls_recv: SSL non actif");
        return -1;
    }
    int n;
    do {
        n = mbedtls_ssl_read(&s->ssl, (unsigned char*)buf, len);
    } while (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (n < 0) { set_tls_error_mbed(s, n, "tls_recv"); return -1; }
    return n;
}

bool owl_tls_recv_exact(OwlTlsSocket* s, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = owl_tls_recv(s, p + total, len - total);
        if (n <= 0) {
            if (n == 0) set_tls_error_fmt(s,
                "tls_recv_exact: peer ferme (%zu/%zu)", total, len);
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

void owl_tls_close(OwlTlsSocket* s) {
    if (s->ssl_active) {
        /* close_notify : 2 essais max (mbedTLS doc), puis on tue le TCP. */
        int rc;
        for (int i = 0; i < 2; i++) {
            rc = mbedtls_ssl_close_notify(&s->ssl);
            if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
                rc != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        }
        s->ssl_active = false;
    }
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    if (s->extra_ca_loaded) {
        mbedtls_x509_crt_free(&s->extra_ca);
        s->extra_ca_loaded = false;
    }
    mbedtls_ssl_init(&s->ssl);          /* re-init pour pouvoir reutiliser */
    mbedtls_ssl_config_init(&s->conf);
    owl_tcp_close(&s->tcp);
}

const char* owl_tls_last_error(const OwlTlsSocket* s) {
    return s->last_error;
}
