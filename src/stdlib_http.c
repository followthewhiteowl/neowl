// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL HTTP/HTTPS client stdlib.
 *
 * HTTP: pure-C sockets (Winsock2 on Windows, BSD sockets elsewhere).
 * HTTPS: TLS 1.2 via mbedTLS, with the Mozilla CA bundle embedded at
 * compile time (vendor/ca/cacert_embed.c).
 *
 * Call forms:
 *   HTTPRequete(url)                                   -> simple GET
 *   HTTPRequete(url, options)                          -> options is a JSON object
 *   HTTPRequete(url, method, body, headers)            -> legacy positional
 *
 * JSON options (all optional):
 *   method             : "GET" | "POST" | "PUT" | "DELETE" | "PATCH" | ...
 *   body               : string
 *   content_type       : "application/json" (shortcut for a header)
 *   user_agent         : "MyApp/1.0"
 *   headers            : "X-Foo: bar\r\n" (raw, appended verbatim)
 *   headers_obj        : { "X-Foo": "bar", ... } (JSON object, key/value)
 *   auth_basic_user    : "alice"
 *   auth_basic_password: "secret"  (sends Authorization: Basic base64(user:pass))
 *   auth_bearer        : "TOKEN"   (sends Authorization: Bearer TOKEN)
 *   timeout            : 30   (seconds, socket recv timeout)
 *   follow_redirects   : true (default true)
 *   max_redirects      : 10   (default 10)
 *   verify_tls         : true (default true — set false for dev only)
 *
 * Companion functions:
 *   HTTPDernierStatut()       -> int HTTP status of last request (0 on error)
 *   HTTPDernierEntete(nom)    -> response header value (string) or ""
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
/* Guard requise : ce module include winsock2.h/windows.h plus bas et winnt.h
 * definit un TokenType qui colliderait avec scanner.h tire par owl_errors.h
 * via son API compile-aware. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "sentinels.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET owl_socket_t;
#define OWL_INVALID_SOCKET INVALID_SOCKET
#define owl_close_socket(s) closesocket(s)
static int g_wsa_initialized = 0;
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
typedef int owl_socket_t;
#define OWL_INVALID_SOCKET (-1)
#define owl_close_socket(s) close(s)
#endif

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/base64.h"

#include "miniz.h"

/* Parsing des reponses : machine a etats stricte, aucun flag LENIENT_*
 * (cf. la note llhttp de CMakeLists.txt et src/internal_tests/llhttp_smoketest.c
 * qui asserte lenient_flags == 0). */
#include "llhttp.h"

#ifdef _WIN32
#include <bcrypt.h>
#ifndef BCRYPT_USE_SYSTEM_PREFERRED_RNG
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#endif
#endif

/* Embedded Mozilla CA bundle */
extern const unsigned char owl_ca_bundle[];
extern const unsigned int  owl_ca_bundle_len;

/* ================================================================
 * Module state
 * ================================================================ */

#define MAX_HEADER_BUF 16384
#define DEFAULT_TIMEOUT_SEC 30
#define DEFAULT_MAX_REDIRECTS 10
#define MAX_COOKIES 128

/* ---- Global HTTP config (set via HTTPParametre) ----
 * Applies to all HTTP calls unless overridden by per-request options/struct. */
typedef struct {
    char  proxy[256];       int proxy_len;
    char  no_proxy[512];    int no_proxy_len;
    char  user_agent[256];  int user_agent_len;
    int   timeout_sec;       /* default 30 */
    int   follow_redirects;  /* default 1 */
    int   max_redirects;     /* default 10 */
    int   accept_gzip;       /* default 1 */
    int   verify_tls;        /* default 1 */
    int   keep_alive;        /* default 0 */
} HttpGlobals;

static HttpGlobals g_http_params = {
    {0}, 0,  /* proxy */
    {0}, 0,  /* no_proxy */
    {0}, 0,  /* user_agent */
    DEFAULT_TIMEOUT_SEC,
    1,   /* follow_redirects */
    DEFAULT_MAX_REDIRECTS,
    1,   /* accept_gzip */
    1,   /* verify_tls */
    0,   /* keep_alive */
};

/* Forward declaration — implementation further down, after HttpOpts is known. */
typedef struct HttpOpts HttpOpts;
static void apply_http_globals(HttpOpts* o);

static int  g_last_status = 0;
/* Bloc d'en-tetes normalise, reconstruit depuis les callbacks du parseur : OWS
 * retire, une ligne par en-tete. C'est la forme que consomment find_header, le
 * jar de cookies et le tableau associatif Entete. */
static char g_last_headers[MAX_HEADER_BUF];
static int  g_last_headers_len = 0;
/* Bloc d'en-tetes tel qu'il est arrive sur le fil, octet pour octet, ligne de
 * statut et CRLF final compris. Distinct du precedent : une empreinte ou une
 * signature calculee dessus doit rester valide, donc aucune normalisation. */
static char g_last_headers_brut[MAX_HEADER_BUF];
static int  g_last_headers_brut_len = 0;

/* Motif de rejet de la derniere reponse recue, quand elle a ete refusee parce
 * que ses octets ne respectent pas le protocole. Vide = pas de rejet de
 * parsing, l'echec (s'il y en a un) est reseau. C'est ce qui permet aux
 * natives HTTP* de distinguer E_NATIVE_HTTP_MALFORMED_RESPONSE (le serveur a
 * repondu, mal) de E_NATIVE_HTTP_REQUEST_FAILURE (pas de reponse exploitable). */
static char g_resp_malformee[160];

/* Debug: raw request sent / raw response received. Capped at MAX_DEBUG_BUF,
 * truncated silently if traffic exceeds the cap. */
#define MAX_DEBUG_BUF (128 * 1024)
static char g_last_request_raw[MAX_DEBUG_BUF];
static int  g_last_request_raw_len = 0;
static char g_last_response_raw[MAX_DEBUG_BUF];
static int  g_last_response_raw_len = 0;

/* Global toggle for automatic cookie jar management (can be overridden per-request). */
static int  g_cookies_auto = 1;

/* ---- Cookie jar (in-memory, lifetime = OWL process) ---- */
typedef struct {
    char name[128];
    char value[1024];
    char domain[256];   /* effective domain (lowercase) */
    char path[256];
    int  secure;
    int  is_set;        /* 1 if slot is occupied */
    /* Expiration (RFC 6265 §5.3 etape 3). a_expiration = 0 : cookie de
     * session, il vit tant que le processus vit. Un drapeau separe plutot
     * qu'une valeur sentinelle : « Expires=Thu, 01 Jan 1970 00:00:00 GMT »
     * est une forme d'effacement courante, et vaut exactement 0. */
    int     a_expiration;
    int64_t expiration;  /* secondes depuis l'epoque, UTC */
} Cookie;
static Cookie g_jar[MAX_COOKIES];

static int                      g_tls_initialized = 0;
static mbedtls_x509_crt         g_cacert;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;

/* OS-backed CSPRNG. Returns 0 on success. */
static int random_bytes(unsigned char* buf, size_t len) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
#endif
}

static int tls_ensure_global(void) {
    if (g_tls_initialized) return 0;
    mbedtls_x509_crt_init(&g_cacert);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char*)"owl-http-client", 15) != 0) {
        return -1;
    }
    if (mbedtls_x509_crt_parse(&g_cacert, owl_ca_bundle, owl_ca_bundle_len) < 0) {
        return -1;
    }
    g_tls_initialized = 1;
    return 0;
}

/* ================================================================
 * URL parsing (supports absolute + same-host path redirects)
 * ================================================================ */

typedef struct {
    int  is_https;
    char host[256];
    int  port;
    /* Le port figurait-il dans l'URL ? Distinguer "80 parce qu'ecrit" de
     * "80 parce que defaut de schema" est ce qui permet de reconstruire une
     * URL fidele : on ne perd pas un port explicite et on n'en invente pas
     * un absent. */
    int  port_explicite;
    char path[2048];
} ParsedURL;

static int parse_url(const char* url, int urllen, ParsedURL* out) {
    out->is_https = 0;
    out->port = 80;
    out->port_explicite = 0;
    out->host[0] = '\0';
    out->path[0] = '/';
    out->path[1] = '\0';

    const char* p = url;
    const char* end = url + urllen;

    if (urllen >= 8 && strncmp(p, "https://", 8) == 0) {
        out->is_https = 1; out->port = 443; p += 8;
    } else if (urllen >= 7 && strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        return -1;
    }

    const char* host_start = p;
    while (p < end && *p != ':' && *p != '/' && *p != '?') p++;
    int hlen = (int)(p - host_start);
    if (hlen <= 0 || hlen >= (int)sizeof(out->host)) return -1;
    memcpy(out->host, host_start, hlen);
    out->host[hlen] = '\0';

    if (p < end && *p == ':') {
        p++;
        int port = 0;
        while (p < end && *p >= '0' && *p <= '9') { port = port*10 + (*p - '0'); p++; }
        if (port > 0 && port <= 65535) { out->port = port; out->port_explicite = 1; }
        else return -1;
    }

    if (p < end) {
        int plen = (int)(end - p);
        if (plen >= (int)sizeof(out->path)) return -1;
        memcpy(out->path, p, plen);
        out->path[plen] = '\0';
    }
    return 0;
}

/* Recompose l'URL rendue dans httpReponse..URL. Le port n'apparait que s'il
 * etait ecrit dans l'URL d'origine (ou dans la Location d'une redirection
 * absolue) : une URL sans port doit le rester, sinon on rend une chaine
 * aussi fausse que celle qui perdait le port. */
static int build_url_finale(const ParsedURL* u, char* out, size_t out_size) {
    const char* schema = u->is_https ? "https" : "http";
    if (u->port_explicite)
        return snprintf(out, out_size, "%s://%s:%d%s", schema, u->host, u->port, u->path);
    return snprintf(out, out_size, "%s://%s%s", schema, u->host, u->path);
}

/* Retire le dernier segment de `out` (et le '/' qui le precede), pour la
 * regle ".." de RFC 3986 §5.2.4. */
static size_t chemin_retire_segment(const char* out, size_t o) {
    while (o > 0 && out[o - 1] != '/') o--;
    if (o > 0) o--;
    return o;
}

/* RFC 3986 §5.2.4 — reduit les segments "." et ".." d'un chemin. `in` est
 * suppose commencer par '/' (chemin d'origine d'une requete HTTP) ; le
 * resultat ne peut qu'etre plus court, donc `cap` >= strlen(in)+1 suffit. */
static void remove_dot_segments(const char* in, char* out, size_t cap) {
    size_t o = 0;
    const char* p = in;
    while (*p && o + 1 < cap) {
        if (strncmp(p, "../", 3) == 0)      { p += 3; continue; }
        if (strncmp(p, "./", 2) == 0)       { p += 2; continue; }
        if (strncmp(p, "/./", 3) == 0)      { p += 2; continue; }
        if (strcmp(p, "/.") == 0)           { out[o++] = '/'; break; }
        if (strncmp(p, "/../", 4) == 0)     { p += 3; o = chemin_retire_segment(out, o); continue; }
        if (strcmp(p, "/..") == 0)          { o = chemin_retire_segment(out, o); out[o++] = '/'; break; }
        if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) break;

        const char* q = (*p == '/') ? p + 1 : p;
        while (*q && *q != '/') q++;
        size_t n = (size_t)(q - p);
        if (o + n + 1 >= cap) break;
        memcpy(out + o, p, n);
        o += n;
        p = q;
    }
    if (o == 0 && cap > 1) out[o++] = '/';
    out[o] = '\0';
}

/* Resolve a Location header (absolute URL or "/path") against a base URL. */
static int resolve_redirect(const ParsedURL* base, const char* loc, int loc_len,
                             ParsedURL* out) {
    if (loc_len <= 0) return -1;
    if (loc_len >= 8 && strncmp(loc, "https://", 8) == 0) return parse_url(loc, loc_len, out);
    if (loc_len >= 7 && strncmp(loc, "http://", 7) == 0)  return parse_url(loc, loc_len, out);
    /* Relative: same host, replace path */
    *out = *base;
    if (loc[0] == '/') {
        if (loc_len >= (int)sizeof(out->path)) return -1;
        memcpy(out->path, loc, loc_len);
        out->path[loc_len] = '\0';
    } else {
        /* RFC 3986 §5.2.2 : une reference relative sans '/' initial se
         * resout dans le REPERTOIRE du chemin de base (/a/b + "c" ->
         * /a/c), pas a la racine. La chaine de requete de la base ne
         * fait pas partie du chemin, elle est donc coupee avant fusion.
         * Les segments "." et ".." de la fusion sont ensuite reduits
         * (§5.2.4) : ils n'ont pas a partir sur le fil. */
        const char* bp = base->path;
        const char* coupe = strpbrk(bp, "?#");
        size_t blen = coupe ? (size_t)(coupe - bp) : strlen(bp);
        while (blen > 0 && bp[blen - 1] != '/') blen--;
        char fusion[sizeof(out->path)];
        if (blen + (size_t)loc_len + 1 >= sizeof(fusion)) return -1;
        if (blen > 0) memcpy(fusion, bp, blen);
        else          fusion[blen++] = '/';
        memcpy(fusion + blen, loc, (size_t)loc_len);
        fusion[blen + (size_t)loc_len] = '\0';
        remove_dot_segments(fusion, out->path, sizeof(out->path));
    }
    return 0;
}

/* ================================================================
 * Connection abstraction: plain socket or TLS-wrapped
 * ================================================================ */

typedef struct {
    owl_socket_t        sock;
    int                 is_tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  ssl_conf;
    /* mTLS: owned client cert + key (NULL when not using mTLS). */
    mbedtls_x509_crt*   client_cert;
    mbedtls_pk_context* client_key;
} HttpConn;

static void conn_init(HttpConn* c) {
    c->sock = OWL_INVALID_SOCKET;
    c->is_tls = 0;
    c->client_cert = NULL;
    c->client_key  = NULL;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);
}

static void conn_close(HttpConn* c) {
    if (c->is_tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->ssl_conf);
    }
    if (c->client_cert) {
        mbedtls_x509_crt_free(c->client_cert);
        free(c->client_cert);
        c->client_cert = NULL;
    }
    if (c->client_key) {
        mbedtls_pk_free(c->client_key);
        free(c->client_key);
        c->client_key = NULL;
    }
    if (c->sock != OWL_INVALID_SOCKET) {
        owl_close_socket(c->sock);
        c->sock = OWL_INVALID_SOCKET;
    }
}

/* ---- LRU connection pool (N slots, keyed by host:port:tls) ----
 * Reuses the right cached connection per host. Least-recently-used slot is
 * evicted when full, so alternating between many hosts still benefits. */
#define POOL_SIZE 4

typedef struct {
    HttpConn conn;
    char     host[256];
    int      port;
    int      is_tls;
    int      valid;
    uint64_t last_used;
} PooledConn;

static PooledConn g_pool[POOL_SIZE];
static uint64_t   g_pool_clock = 0;

/* Returns slot index of a matching cached conn, or -1.
 * The caller acquires the conn (value-copy + mark invalid). */
static int pool_acquire(HttpConn* dst, const char* host, int port, int is_tls) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!g_pool[i].valid) continue;
        if (g_pool[i].port != port || g_pool[i].is_tls != is_tls) continue;
        if (strcasecmp(g_pool[i].host, host) != 0) continue;
        *dst = g_pool[i].conn;          /* take ownership via struct copy */
        g_pool[i].valid = 0;
        return i;
    }
    return -1;
}

/* Drop one slot. */
static void pool_invalidate_idx(int idx) {
    if (idx < 0 || idx >= POOL_SIZE) return;
    if (g_pool[idx].valid) {
        conn_close(&g_pool[idx].conn);
        g_pool[idx].valid = 0;
    }
}

/* Drop all cached connections. */
static void pool_invalidate(void) {
    for (int i = 0; i < POOL_SIZE; i++) pool_invalidate_idx(i);
}

/* Store conn in pool. Picks an empty slot, or evicts the LRU one if full.
 * Caller's HttpConn becomes inert (must not be used after). */
static void pool_keep(HttpConn* c, const char* host, int port, int is_tls) {
    /* Find empty slot, or LRU if all full */
    int slot = -1;
    uint64_t oldest = (uint64_t)-1;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!g_pool[i].valid) { slot = i; break; }
        if (g_pool[i].last_used < oldest) { oldest = g_pool[i].last_used; slot = i; }
    }
    if (g_pool[slot].valid) {
        conn_close(&g_pool[slot].conn);
        g_pool[slot].valid = 0;
    }
    g_pool[slot].conn = *c;
    int hl = (int)strlen(host);
    if (hl >= (int)sizeof(g_pool[slot].host)) hl = (int)sizeof(g_pool[slot].host) - 1;
    memcpy(g_pool[slot].host, host, hl);
    g_pool[slot].host[hl] = '\0';
    g_pool[slot].port      = port;
    g_pool[slot].is_tls    = is_tls;
    g_pool[slot].valid     = 1;
    g_pool[slot].last_used = ++g_pool_clock;

    /* Make caller's HttpConn inert */
    c->sock = OWL_INVALID_SOCKET;
    c->is_tls = 0;
}

static int conn_write(HttpConn* c, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n;
        if (c->is_tls) {
            n = mbedtls_ssl_write(&c->ssl, (const unsigned char*)(buf + sent), (size_t)(len - sent));
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            n = send(c->sock, buf + sent, len - sent, 0);
        }
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int conn_read(HttpConn* c, char* buf, int cap) {
    int n;
    if (c->is_tls) {
        for (;;) {
            n = mbedtls_ssl_read(&c->ssl, (unsigned char*)buf, (size_t)cap);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            break;
        }
    } else {
        n = recv(c->sock, buf, cap, 0);
    }
    return n;
}

#ifdef _WIN32
static int wsa_ensure(void) {
    if (g_wsa_initialized) return 0;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
    g_wsa_initialized = 1;
    return 0;
}
#endif

static void set_socket_timeout(owl_socket_t s, int seconds) {
#ifdef _WIN32
    DWORD tv = (DWORD)(seconds * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv; tv.tv_sec = seconds; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

static owl_socket_t connect_tcp(const char* host, int port, int timeout_sec) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return OWL_INVALID_SOCKET;

    owl_socket_t s = OWL_INVALID_SOCKET;
    for (struct addrinfo* it = res; it != NULL; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == OWL_INVALID_SOCKET) continue;
        if (connect(s, it->ai_addr, (int)it->ai_addrlen) == 0) break;
        owl_close_socket(s);
        s = OWL_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != OWL_INVALID_SOCKET) set_socket_timeout(s, timeout_sec);
    return s;
}

static int tls_send_cb(void* ctx, const unsigned char* buf, size_t len) {
    owl_socket_t s = *(owl_socket_t*)ctx;
    int n = send(s, (const char*)buf, (int)len, 0);
    return n <= 0 ? -1 : n;
}
static int tls_recv_cb(void* ctx, unsigned char* buf, size_t len) {
    owl_socket_t s = *(owl_socket_t*)ctx;
    int n = recv(s, (char*)buf, (int)len, 0);
    return n < 0 ? -1 : n;
}

/* Load mTLS client cert + key from files into c->client_cert/key. */
static int tls_load_client_cert(HttpConn* c,
                                 const char* cert_path, const char* key_path,
                                 const char* key_password) {
    /* Self-contained error handling: any failure frees our own partial
     * state and leaves c->client_cert/key NULL. Keeps the function safe
     * to call independently of caller's conn_close discipline. */
    c->client_cert = malloc(sizeof(*c->client_cert));
    c->client_key  = malloc(sizeof(*c->client_key));
    if (!c->client_cert || !c->client_key) goto fail;
    mbedtls_x509_crt_init(c->client_cert);
    mbedtls_pk_init(c->client_key);
    if (mbedtls_x509_crt_parse_file(c->client_cert, cert_path) != 0) {
        mbedtls_x509_crt_free(c->client_cert);
        mbedtls_pk_free(c->client_key);
        goto fail;
    }
    size_t pwd_len = key_password ? strlen(key_password) : 0;
    (void)pwd_len;
    if (mbedtls_pk_parse_keyfile(c->client_key, key_path,
                                  key_password, mbedtls_ctr_drbg_random, &g_drbg) != 0) {
        mbedtls_x509_crt_free(c->client_cert);
        mbedtls_pk_free(c->client_key);
        goto fail;
    }
    return 0;
fail:
    free(c->client_cert); c->client_cert = NULL;
    free(c->client_key);  c->client_key  = NULL;
    return -1;
}

/* Globals set by tls_handshake_on after a successful handshake, read back
 * into the httpRequête instance by fn_http_envoie (CipherChoisi +
 * VersionSSLChoisie fields). */
static char    g_last_cipher_name[128] = "";
static int     g_last_tls_version_bits = 0;

/* Map a bitmask of E_protocoleVersions flags to (min, max) TLS versions
 * for mbedtls. Returns 1 if the bitmask declares anything, 0 to keep
 * mbedtls defaults. */
static int tls_bits_to_range(int bits, int* minVer, int* maxVer) {
    /* bitflags: TLS1=1, TLS1_1=2, TLS1_2=4, TLS1_3=8 */
    int lowest = 0, highest = 0;
    if (bits & 1) { lowest = lowest ? lowest : 1; highest = 1; }
    if (bits & 2) { lowest = lowest ? lowest : 2; highest = 2; }
    if (bits & 4) { lowest = lowest ? lowest : 3; highest = 3; }
    if (bits & 8) { lowest = lowest ? lowest : 4; highest = 4; }
    if (!lowest) return 0;
    /* mbedTLS uses the MBEDTLS_SSL_VERSION_* enum (TLS_1_2=0x303, TLS_1_3=0x304).
     * Pre-TLS1.2 isn't in modern mbedTLS builds — clamp to TLS1.2 as floor. */
    static const int map[] = { 0,
        MBEDTLS_SSL_VERSION_TLS1_2,   /* TLS1.0 → clamp to 1.2 (legacy unsupported) */
        MBEDTLS_SSL_VERSION_TLS1_2,   /* TLS1.1 → clamp to 1.2 */
        MBEDTLS_SSL_VERSION_TLS1_2,
        MBEDTLS_SSL_VERSION_TLS1_3
    };
    *minVer = map[lowest];
    *maxVer = map[highest];
    return 1;
}

/* Parse the string returned by mbedtls_ssl_get_version and map to our
 * E_protocoleVersions bitflag. */
static int tls_version_string_to_bits(const char* v) {
    if (!v) return 0;
    if (strstr(v, "1.3")) return 8;
    if (strstr(v, "1.2")) return 4;
    if (strstr(v, "1.1")) return 2;
    if (strstr(v, "1.0")) return 1;
    return 0;
}

/* Run TLS handshake on an already-connected socket (c->sock).
 * Used for both direct HTTPS connect and HTTPS-via-proxy after CONNECT. */
static int tls_handshake_on(HttpConn* c, const char* host, int verify,
                             const char* client_cert_path,
                             const char* client_key_path,
                             const char* client_key_password,
                             int tls_version_bits) {
    if (tls_ensure_global() != 0) return -1;

    if (mbedtls_ssl_config_defaults(&c->ssl_conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) return -1;
    mbedtls_ssl_conf_authmode(&c->ssl_conf,
        verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&c->ssl_conf, &g_cacert, NULL);
    mbedtls_ssl_conf_rng(&c->ssl_conf, mbedtls_ctr_drbg_random, &g_drbg);

    /* Optional min/max TLS version (VersionSSL field). */
    {
        int minV = 0, maxV = 0;
        if (tls_bits_to_range(tls_version_bits, &minV, &maxV)) {
            mbedtls_ssl_conf_min_tls_version(&c->ssl_conf, minV);
            mbedtls_ssl_conf_max_tls_version(&c->ssl_conf, maxV);
        }
    }

    /* mTLS: attach client cert+key if provided */
    if (client_cert_path && client_key_path) {
        if (tls_load_client_cert(c, client_cert_path, client_key_path,
                                  client_key_password) != 0) return -1;
        if (mbedtls_ssl_conf_own_cert(&c->ssl_conf, c->client_cert, c->client_key) != 0)
            return -1;
    }

    if (mbedtls_ssl_setup(&c->ssl, &c->ssl_conf) != 0) return -1;
    if (mbedtls_ssl_set_hostname(&c->ssl, host) != 0) return -1;
    mbedtls_ssl_set_bio(&c->ssl, &c->sock, tls_send_cb, tls_recv_cb, NULL);

    int rc;
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
    }
    c->is_tls = 1;

    /* Snapshot negotiated parameters for CipherChoisi + VersionSSLChoisie
     * writeback (consumed by fn_http_envoie). */
    {
        const char* cipher = mbedtls_ssl_get_ciphersuite(&c->ssl);
        const char* ver    = mbedtls_ssl_get_version(&c->ssl);
        if (cipher) {
            snprintf(g_last_cipher_name, sizeof(g_last_cipher_name), "%s", cipher);
        } else {
            g_last_cipher_name[0] = '\0';
        }
        g_last_tls_version_bits = tls_version_string_to_bits(ver);
    }
    return 0;
}

static int tls_connect(HttpConn* c, const char* host, int port, int timeout_sec,
                        int verify,
                        const char* client_cert_path,
                        const char* client_key_path,
                        const char* client_key_password,
                        int tls_version_bits) {
    c->sock = connect_tcp(host, port, timeout_sec);
    if (c->sock == OWL_INVALID_SOCKET) return -1;
    return tls_handshake_on(c, host, verify, client_cert_path, client_key_path,
                             client_key_password, tls_version_bits);
}

/* ================================================================
 * Byte buffer
 * ================================================================ */

typedef struct { char* data; int len; int cap; } ByteBuf;
static void bb_init(ByteBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void bb_free(ByteBuf* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
static int  bb_append(ByteBuf* b, const char* src, int n);

/* Sink abstraction for response body: either a growing ByteBuf (for in-memory
 * responses returned to the caller) or a FILE* for streaming downloads. */
typedef struct {
    ByteBuf* buf;     /* non-NULL for buffered mode */
    FILE*    file;    /* non-NULL for streaming mode */
    int      total;   /* running byte count (streaming stats) */
} BodySink;

static int sink_write(BodySink* s, const char* data, int n) {
    if (s->buf)  return bb_append(s->buf, data, n);
    if (s->file) {
        size_t w = fwrite(data, 1, (size_t)n, s->file);
        if (w != (size_t)n) return -1;
        s->total += n;
        return 0;
    }
    return -1;
}

static int bb_append(ByteBuf* b, const char* src, int n) {
    if (b->len + n + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char* nd = realloc(b->data, nc);
        if (!nd) return -1;
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n; b->data[b->len] = '\0';
    return 0;
}

static int bb_printf(ByteBuf* b, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    return bb_append(b, buf, n);
}

/* ================================================================
 * Proxy support: parse proxy URL, check no_proxy, CONNECT tunnel
 * ================================================================ */

typedef struct {
    int  valid;
    char host[256];
    int  port;
    char user[128];
    char pass[128];
} ProxyInfo;

/* Parse "http://[user:pass@]host:port" into ProxyInfo. Returns 0 on success. */
static int parse_proxy(const char* url, int url_len, ProxyInfo* out) {
    memset(out, 0, sizeof(*out));
    if (url == NULL || url_len <= 0) return -1;

    const char* p = url;
    const char* end = url + url_len;

    /* Optional scheme */
    if (url_len >= 7 && strncmp(p, "http://", 7) == 0) p += 7;

    /* Optional [user:pass@] */
    const char* at = memchr(p, '@', end - p);
    if (at != NULL) {
        const char* colon = memchr(p, ':', at - p);
        int u_len = colon ? (int)(colon - p) : (int)(at - p);
        int p_len = colon ? (int)(at - colon - 1) : 0;
        if (u_len < (int)sizeof(out->user)) {
            memcpy(out->user, p, u_len);
            out->user[u_len] = '\0';
        }
        if (colon && p_len < (int)sizeof(out->pass)) {
            memcpy(out->pass, colon + 1, p_len);
            out->pass[p_len] = '\0';
        }
        p = at + 1;
    }

    /* host[:port] */
    const char* slash = memchr(p, '/', end - p);
    const char* hp_end = slash ? slash : end;
    const char* colon = memchr(p, ':', hp_end - p);
    int h_len = colon ? (int)(colon - p) : (int)(hp_end - p);
    if (h_len <= 0 || h_len >= (int)sizeof(out->host)) return -1;
    memcpy(out->host, p, h_len);
    out->host[h_len] = '\0';

    out->port = 8080; /* common default for HTTP proxies */
    if (colon) {
        int port = 0;
        const char* pp = colon + 1;
        while (pp < hp_end && *pp >= '0' && *pp <= '9') { port = port*10 + (*pp - '0'); pp++; }
        if (port > 0 && port <= 65535) out->port = port;
    }
    out->valid = 1;
    return 0;
}

/* Returns true if `host` matches an entry in `no_proxy` (comma-separated).
 * Match: exact, suffix (".example.com" matches "x.example.com"), or "*". */
static int host_in_no_proxy(const char* host, const char* nopx, int nopx_len) {
    if (nopx == NULL || nopx_len <= 0) return 0;
    int hlen = (int)strlen(host);
    const char* p = nopx;
    const char* end = nopx + nopx_len;
    while (p < end) {
        const char* comma = memchr(p, ',', end - p);
        const char* tok_end = comma ? comma : end;
        const char* tok = p;
        int tlen = (int)(tok_end - tok);
        while (tlen > 0 && (*tok == ' ' || *tok == '\t')) { tok++; tlen--; }
        while (tlen > 0 && (tok[tlen-1] == ' ' || tok[tlen-1] == '\t')) tlen--;
        if (tlen > 0) {
            if (tlen == 1 && tok[0] == '*') return 1;
            if (tlen == hlen && strncasecmp(host, tok, tlen) == 0) return 1;
            if (tok[0] == '.' && hlen > tlen &&
                strncasecmp(host + (hlen - tlen), tok, tlen) == 0) return 1;
            if (tok[0] != '.' && hlen > tlen + 1 &&
                host[hlen - tlen - 1] == '.' &&
                strncasecmp(host + (hlen - tlen), tok, tlen) == 0) return 1;
        }
        p = comma ? comma + 1 : end;
    }
    return 0;
}

/* Resolve proxy. Returns 0 if a proxy is active.
 *
 * PRECEDENCE — l'ordre des tests EST la semantique, chaque
 * etage est confronte a celui du dessous :
 *
 *   1. no_proxy EXPLICITE       exclusion posee dans le code : elle bat
 *                               tout, y compris un proxy explicite (c'est
 *                               sa raison d'etre, l'appelant a pose les
 *                               deux sciemment) ;
 *   2. proxy EXPLICITE          bat TOUT l'environnement — no_proxy y
 *                               compris ;
 *   3. no_proxy d'ENVIRONNEMENT ne peut plus desactiver qu'un proxy de
 *                               meme origine ;
 *   4. proxy d'ENVIRONNEMENT    dernier recours.
 *
 * L'ordre precedent testait 3 avant 2 : le proxy explicite battait
 * http_proxy mais se faisait battre par no_proxy, deux precedences
 * contradictoires dans le meme corps de fonction. Consequence concrete,
 * une variable que le developpeur ne controle pas rendait contournable en
 * silence un proxy pose en dur. Le canon ne dit rien sur cette
 * precedence (la fonction de parametrage HTTP canon n'expose aucune
 * option de proxy et ignore les variables d'environnement) : on suit la
 * convention du secteur, celle de curl dont --noproxy l'emporte sur
 * NO_PROXY. */
static int resolve_proxy(const char* opt_proxy, int opt_proxy_len,
                          const char* opt_noproxy, int opt_noproxy_len,
                          int is_https, const char* host, ProxyInfo* px) {
    if (opt_noproxy_len > 0 && host_in_no_proxy(host, opt_noproxy, opt_noproxy_len)) {
        px->valid = 0; return -1;
    }

    if (opt_proxy_len > 0) return parse_proxy(opt_proxy, opt_proxy_len, px);

    const char* env_no = getenv("no_proxy");
    if (env_no == NULL) env_no = getenv("NO_PROXY");
    if (env_no && host_in_no_proxy(host, env_no, (int)strlen(env_no))) {
        px->valid = 0; return -1;
    }

    const char* env = NULL;
    if (is_https) {
        env = getenv("https_proxy");
        if (!env) env = getenv("HTTPS_PROXY");
    }
    if (!env) {
        env = getenv("http_proxy");
        if (!env) env = getenv("HTTP_PROXY");
    }
    if (!env || !*env) { px->valid = 0; return -1; }
    return parse_proxy(env, (int)strlen(env), px);
}

/* Build "Proxy-Authorization: Basic <b64>\r\n" if px has creds. Returns 0/-1. */
static int append_proxy_auth(ByteBuf* req, const ProxyInfo* px) {
    if (px->user[0] == '\0' && px->pass[0] == '\0') return 0;
    char up[260];
    int upl = snprintf(up, sizeof(up), "%s:%s", px->user, px->pass);
    if (upl <= 0 || upl >= (int)sizeof(up)) return -1;
    unsigned char b64[400];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                               (unsigned char*)up, (size_t)upl) != 0) return -1;
    return bb_printf(req, "Proxy-Authorization: Basic %.*s\r\n",
                     (int)b64_len, (char*)b64);
}

/* ---- Reponse au CONNECT : parsee par llhttp, comme le chemin principal ----
 *
 * Le scan maison qui vivait ici cherchait un CRLFCRLF puis avalait des
 * chiffres apres le premier espace. Mesure faite : il acceptait
 * une ligne de statut SANS version HTTP, et un statut a dix chiffres qui
 * deborde l'int et retombe sur 200 — cousin exact du "Content-Length: 1x2"
 * qui valait 12. Le proxy est justement le pair le mieux place
 * pour mentir : c'est derriere lui qu'on chiffre.
 *
 * llhttp donne en prime l'offset exact de la fin des en-tetes, sans quoi les
 * octets qui suivent la ligne vide ne sont meme pas OBSERVABLES. */
typedef struct {
    int statut;
    int entetes_finis;
} ConnectParse;

static int cx_on_headers_complete(llhttp_t* p) {
    ConnectParse* cx = (ConnectParse*)p->data;
    int statut = llhttp_get_status_code(p);
    /* Reponse informationnelle : ce n'est pas encore le verdict du proxy.
     * Rendre 0 laisse llhttp enchainer sur la reponse finale. */
    if (statut >= 100 && statut < 200) return 0;
    cx->statut        = statut;
    cx->entetes_finis = 1;
    /* HPE_PAUSED rend la main a llhttp_execute pile sur la ligne vide :
     * c'est ce qui donne la frontiere exacte du tunnel. */
    return HPE_PAUSED;
}

static llhttp_settings_t g_connect_settings;
static int               g_connect_settings_pret = 0;

static const llhttp_settings_t* connect_settings(void) {
    if (!g_connect_settings_pret) {
        llhttp_settings_init(&g_connect_settings);
        g_connect_settings.on_headers_complete = cx_on_headers_complete;
        g_connect_settings_pret = 1;
    }
    return &g_connect_settings;
}

/* llhttp ne plafonne rien : borne le bloc d'en-tetes d'un proxy qui
 * distillerait sans jamais conclure. */
#define MAX_CONNECT_HEADERS (16 * 1024)

/* Send CONNECT request through plain TCP socket and read 200 response.
 * Returns 0 on success. */
static int proxy_connect_tunnel(owl_socket_t s, const ProxyInfo* px,
                                 const char* target_host, int target_port) {
    ByteBuf req; bb_init(&req);
    if (bb_printf(&req, "CONNECT %s:%d HTTP/1.1\r\n", target_host, target_port) != 0 ||
        bb_printf(&req, "Host: %s:%d\r\n", target_host, target_port) != 0 ||
        append_proxy_auth(&req, px) != 0 ||
        bb_append(&req, "\r\n", 2) != 0) {
        bb_free(&req); return -1;
    }
    int sent = 0;
    while (sent < req.len) {
        int n = send(s, req.data + sent, req.len - sent, 0);
        if (n <= 0) { bb_free(&req); return -1; }
        sent += n;
    }
    bb_free(&req);

    ConnectParse cx;
    cx.statut = 0;
    cx.entetes_finis = 0;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_RESPONSE, connect_settings());
    parser.data = &cx;

    char buf[2048];
    int lus = 0;
    while (!cx.entetes_finis) {
        if (lus >= MAX_CONNECT_HEADERS) return -1;
        int n = recv(s, buf, (int)sizeof(buf), 0);
        if (n <= 0) return -1;
        lus += n;

        llhttp_errno_t err = llhttp_execute(&parser, buf, (size_t)n);
        if (err == HPE_PAUSED) {
            /* Un CONNECT reussi bascule en tunnel juste apres la ligne vide
             * (RFC 9110 §9.3.6), et le client n'a encore rien envoye a
             * tunneler : un proxy conforme n'a donc RIEN a emettre ici. Ce
             * qui traine au-dela est soit du bourrage, soit une injection
             * dans le flux qu'on s'apprete a chiffrer. L'ancien scan les
             * jetait en silence, ce qui rendait le verdict dependant d'une
             * frontiere de segment TCP. */
            if (llhttp_get_error_pos(&parser) - buf < n) return -1;
            break;
        }
        if (err != HPE_OK) return -1;
    }
    return (cx.statut >= 200 && cx.statut < 300) ? 0 : -1;
}

/* ================================================================
 * Cookie jar: parse Set-Cookie, build Cookie header
 * ================================================================ */

static void str_lowercase_copy(char* dst, int dst_cap, const char* src, int src_len) {
    int i = 0;
    while (i < src_len && i + 1 < dst_cap) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

/* Trim ASCII whitespace from both ends of [s, s+len). */
static void trim_view(const char** s_p, int* len_p) {
    const char* s = *s_p;
    int n = *len_p;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    *s_p = s; *len_p = n;
}

static int cookie_domain_matches(const char* req_host, const char* cdomain);

/* ---- Dates de cookie (RFC 6265 §5.1.1) ----
 *
 * Pourquoi un analyseur ici plutot que celui de http_politique.c
 * (pol_lit_date_http) : celui-la lit l'IMF-fixdate de RFC 9110, un
 * format FIXE, et refuse tout le reste — il exige 29 caracteres et une
 * virgule en 4e position. §5.1.1 impose au contraire un decoupage en
 * jetons qui accepte les formes heritees encore emises en production :
 * RFC 850 (« Sunday, 06-Nov-94 08:49:37 GMT », annee sur 2 chiffres,
 * separateurs '-') et asctime (« Sun Nov  6 08:49:37 1994 », sans
 * fuseau, annee en dernier). Reutiliser le lecteur strict reviendrait a
 * ignorer silencieusement ces Expires, donc a ne pas revoquer.
 *
 * Le format des dates OWL (stdlib_date.c) est un autre sujet : ses
 * entrees sont AAAAMMJJ et ISO, pas les formats du fil HTTP. */

static int cd_delimiteur(unsigned char c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

static int cd_chiffre(unsigned char c) { return c >= '0' && c <= '9'; }

static const char* const CD_MOIS[12] = {
    "jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"
};

/* Nombre de jours ecoules depuis 1970-01-01 pour une date civile (UTC).
 * Algorithme « days from civil » : pas de timegm, absent de MSVCRT. */
static int64_t cd_jours_depuis_epoque(int annee, int mois, int jour) {
    int64_t y = annee - (mois <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (mois + (mois > 2 ? -3 : 9)) + 2) / 5 + jour - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int cd_jours_dans_le_mois(int annee, int mois) {
    static const int n[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mois == 2) {
        int bis = (annee % 4 == 0 && annee % 100 != 0) || annee % 400 == 0;
        return bis ? 29 : 28;
    }
    return n[mois - 1];
}

/* Rend 1 et pose *out (secondes depuis l'epoque, UTC) si la valeur se lit
 * comme une cookie-date, 0 sinon. Un echec n'est pas anodin : §5.2.1
 * impose alors d'IGNORER l'attribut, donc de garder le cookie — surtout
 * pas de le traiter comme expire sur une faute de frappe du serveur. */
static int cookie_date_parse(const char* s, int len, int64_t* out) {
    int trouve_heure = 0, trouve_jour = 0, trouve_mois = 0, trouve_annee = 0;
    int h = 0, mi = 0, sec = 0, jour = 0, mois = 0, annee = 0;

    int i = 0;
    while (i < len) {
        while (i < len && cd_delimiteur((unsigned char)s[i])) i++;
        int deb = i;
        while (i < len && !cd_delimiteur((unsigned char)s[i])) i++;
        int tlen = i - deb;
        if (tlen <= 0) continue;
        const char* t = s + deb;

        /* time = 1*2DIGIT ":" 1*2DIGIT ":" 1*2DIGIT, puis n'importe quoi
         * qui ne soit pas un chiffre. */
        if (!trouve_heure) {
            int j = 0, ok = 1, champ[3] = {0,0,0};
            for (int c = 0; c < 3 && ok; c++) {
                if (c > 0) { if (j < tlen && t[j] == ':') j++; else ok = 0; }
                int nd = 0;
                while (ok && j < tlen && nd < 2 && cd_chiffre((unsigned char)t[j])) {
                    champ[c] = champ[c] * 10 + (t[j] - '0'); j++; nd++;
                }
                if (nd == 0) ok = 0;
            }
            if (ok && (j == tlen || !cd_chiffre((unsigned char)t[j]))) {
                trouve_heure = 1; h = champ[0]; mi = champ[1]; sec = champ[2];
                continue;
            }
        }
        /* day-of-month = 1*2DIGIT ( non-digit *OCTET ) — la contrainte de
         * non-chiffre est ce qui empeche « 1994 » de passer pour un jour. */
        if (!trouve_jour && cd_chiffre((unsigned char)t[0])) {
            int j = 0, v = 0;
            while (j < tlen && j < 2 && cd_chiffre((unsigned char)t[j])) { v = v * 10 + (t[j]-'0'); j++; }
            if (j == tlen || !cd_chiffre((unsigned char)t[j])) {
                trouve_jour = 1; jour = v;
                continue;
            }
        }
        if (!trouve_mois && tlen >= 3) {
            char abr[4] = {0};
            for (int k = 0; k < 3; k++) abr[k] = (char)tolower((unsigned char)t[k]);
            int m = -1;
            for (int k = 0; k < 12; k++) if (strcmp(abr, CD_MOIS[k]) == 0) { m = k + 1; break; }
            if (m > 0) { trouve_mois = 1; mois = m; continue; }
        }
        if (!trouve_annee && cd_chiffre((unsigned char)t[0])) {
            int j = 0, v = 0;
            while (j < tlen && j < 4 && cd_chiffre((unsigned char)t[j])) { v = v * 10 + (t[j]-'0'); j++; }
            if (j >= 2 && (j == tlen || !cd_chiffre((unsigned char)t[j]))) {
                trouve_annee = 1; annee = v;
                continue;
            }
        }
    }

    if (annee >= 70 && annee <= 99) annee += 1900;
    else if (annee >= 0 && annee <= 69) annee += 2000;

    if (!trouve_heure || !trouve_jour || !trouve_mois || !trouve_annee) return 0;
    if (jour < 1 || jour > 31) return 0;
    if (annee < 1601) return 0;
    if (h > 23 || mi > 59 || sec > 59) return 0;
    if (jour > cd_jours_dans_le_mois(annee, mois)) return 0;   /* §5.1.1 etape 6 */

    *out = cd_jours_depuis_epoque(annee, mois, jour) * 86400
         + (int64_t)h * 3600 + (int64_t)mi * 60 + sec;
    return 1;
}

/* ----------------------------------------------------------------
 * Horloge du jar — point d'injection reserve aux self-tests C.
 *
 * L'etat « cookie stocke, echeance passee » n'est pas atteignable par un
 * test qui refuse d'attendre : cookie_jar_set ecarte deja tout cookie
 * expire A LA POSE, donc toutes les valeurs limites (Max-Age nul ou
 * negatif, Expires passee) sont interceptees a l'ingestion. Seul le
 * temps qui passe pendant la vie du processus y mene — c'est justement
 * le cas d'un serveur qui tourne des heures.
 *
 * D'ou ce decalage, en secondes, ajoute a la lecture du temps courant
 * de la logique d'expiration. Volontairement invisible depuis le
 * langage : aucune native ne l'expose (une manipulation du temps serait
 * une surface publique permanente pour un besoin de test).
 *
 * Etancheite : g_http_params survit a vm_free(), donc au self-test
 * voisin. Le decalage est remis a zero par httpParamReinitialise —
 * le verrouillage d'isolation que chaque pilote de test emet deja
 * avant sa requete (cf. ). Une horloge armee ne peut donc pas
 * fuir vers un voisin sans que celui-ci la desarme de lui-meme.
 * ---------------------------------------------------------------- */
static int64_t g_horloge_decalage_test = 0;

void owl_http_test_horloge_decale(int64_t secondes) {
    g_horloge_decalage_test = secondes;
}

int64_t owl_http_test_horloge_decalage(void) {
    return g_horloge_decalage_test;
}

/* Lecture UNIQUE du temps courant de la logique d'expiration (ingestion,
 * emission, lecture par HTTPCookie). Une seconde lecture directe de
 * time(NULL) quelque part dans cette logique rendrait le point
 * d'injection partiellement inerte. */
static int64_t cookie_maintenant(void) {
    return (int64_t)time(NULL) + g_horloge_decalage_test;
}

/* RFC 6265 §5.3 : « The user agent MUST evict all expired cookies from
 * the cookie store if, at any time, an expired cookie exists ». Point de
 * decision UNIQUE de l'expiration : ingestion, emission et lecture par
 * HTTPCookie passent tous par ici. */
static int cookie_est_expire(const Cookie* c, int64_t maintenant) {
    return c->a_expiration && c->expiration <= maintenant;
}

static void cookie_jar_evince_expires(void) {
    int64_t maintenant = cookie_maintenant();
    for (int i = 0; i < MAX_COOKIES; i++) {
        if (g_jar[i].is_set && cookie_est_expire(&g_jar[i], maintenant)) g_jar[i].is_set = 0;
    }
}

/* Parse one Set-Cookie value and store in jar. host is the request host,
 * deja en minuscules (cf. cookie_jar_ingest). */
static void cookie_jar_set(const char* host, const char* val, int val_len) {
    /* Split on ';' — first token is name=value, rest are attributes. */
    const char* p = val;
    const char* end = val + val_len;

    /* name=value */
    const char* tok_end = memchr(p, ';', end - p);
    int tok_len = tok_end ? (int)(tok_end - p) : (int)(end - p);
    const char* eq = memchr(p, '=', tok_len);
    if (!eq) return;
    int name_len  = (int)(eq - p);
    int value_len = (int)(p + tok_len - (eq + 1));
    const char* name_s  = p;
    const char* value_s = eq + 1;
    trim_view(&name_s, &name_len);
    trim_view(&value_s, &value_len);
    if (name_len <= 0 || name_len >= 128) return;
    if (value_len < 0 || value_len >= 1024) return;

    char domain[256] = {0};
    char path[256]   = "/";
    int  secure = 0;

    /* Expiration. Deux candidats independants : §5.3 etape 3 tranche
     * apres coup, pas au fil de la lecture — l'ordre d'ecriture des
     * attributs dans l'entete ne doit rien decider. */
    int64_t maintenant  = cookie_maintenant();
    int     a_max_age   = 0;   int64_t exp_max_age = 0;
    int     a_expires   = 0;   int64_t exp_expires = 0;

    /* Walk attributes */
    p = tok_end ? tok_end + 1 : end;
    while (p < end) {
        const char* nxt = memchr(p, ';', end - p);
        const char* a_end = nxt ? nxt : end;
        const char* a_eq  = memchr(p, '=', a_end - p);
        const char* k_s   = p;
        int k_len = a_eq ? (int)(a_eq - p) : (int)(a_end - p);
        const char* v_s = a_eq ? a_eq + 1 : NULL;
        int v_len       = a_eq ? (int)(a_end - (a_eq + 1)) : 0;
        trim_view(&k_s, &k_len);
        if (v_s) trim_view(&v_s, &v_len);

        if (k_len == 6 && strncasecmp(k_s, "domain", 6) == 0 && v_s && v_len > 0) {
            int dst_cap = (int)sizeof(domain);
            int copy_len = v_len < dst_cap - 1 ? v_len : dst_cap - 2;
            /* Strip leading '.' if present */
            if (copy_len > 0 && v_s[0] == '.') { v_s++; copy_len--; }
            str_lowercase_copy(domain, dst_cap, v_s, copy_len);
        } else if (k_len == 4 && strncasecmp(k_s, "path", 4) == 0 && v_s && v_len > 0) {
            int copy_len = v_len < 255 ? v_len : 255;
            memcpy(path, v_s, copy_len);
            path[copy_len] = '\0';
        } else if (k_len == 6 && strncasecmp(k_s, "secure", 6) == 0) {
            secure = 1;
        } else if (k_len == 7 && strncasecmp(k_s, "max-age", 7) == 0 && v_s && v_len > 0) {
            /* §5.2.2 : premier caractere DIGIT ou '-', le reste que des
             * DIGIT, sinon l'attribut est IGNORE. Ignore veut dire absent
             * de la liste d'attributs : la precedence de l'etape 3 ne
             * joue alors pas, et un Expires present reprend la main. */
            int ok = (v_s[0] == '-' || cd_chiffre((unsigned char)v_s[0]));
            for (int k = 1; ok && k < v_len; k++) {
                if (!cd_chiffre((unsigned char)v_s[k])) ok = 0;
            }
            if (ok && !(v_len == 1 && v_s[0] == '-')) {
                int64_t delta = 0;
                int neg = v_s[0] == '-';
                for (int k = neg ? 1 : 0; k < v_len; k++) {
                    if (delta > 1000000000LL) break;          /* garde d'overflow */
                    delta = delta * 10 + (v_s[k] - '0');
                }
                if (neg) delta = -delta;
                a_max_age = 1;
                /* delta <= 0 -> date representable la plus ancienne : le
                 * cookie est expire des sa reception, c'est la revocation. */
                exp_max_age = delta <= 0 ? INT64_MIN / 2 : maintenant + delta;
            }
        } else if (k_len == 7 && strncasecmp(k_s, "expires", 7) == 0 && v_s && v_len > 0) {
            int64_t t;
            if (cookie_date_parse(v_s, v_len, &t)) { a_expires = 1; exp_expires = t; }
        }
        /* httponly / samesite : sans porteur observable cote client
         * programmatique (pas de DOM, pas de notion de requete tierce). */

        p = nxt ? nxt + 1 : end;
    }

    /* RFC 6265 §5.3 etape 6 : un attribut Domain que l'hote emetteur ne
     * couvre pas fait rejeter le cookie ENTIEREMENT — pas de repli en
     * host-only. C'est ce qui empeche un hote d'en cookie-iser un autre,
     * en particulier a travers une redirection.
     * Non couvert ici : l'etape 5 (Domain valant un suffixe public),
     * faute de liste de suffixes publics embarquee. */
    if (domain[0] != '\0' && !cookie_domain_matches(host, domain)) return;

    /* Default domain = request host (host-only cookie) */
    if (domain[0] == '\0') {
        str_lowercase_copy(domain, sizeof(domain), host, (int)strlen(host));
    }

    /* Find existing slot (same name + domain + path) or empty slot */
    int slot = -1;
    for (int i = 0; i < MAX_COOKIES; i++) {
        if (g_jar[i].is_set &&
            strncmp(g_jar[i].name, name_s, name_len) == 0 &&
            g_jar[i].name[name_len] == '\0' &&
            strcmp(g_jar[i].domain, domain) == 0 &&
            strcmp(g_jar[i].path, path) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_COOKIES; i++) {
            if (!g_jar[i].is_set) { slot = i; break; }
        }
    }
    if (slot < 0) return;  /* jar full, drop silently */

    /* If value is empty AND it's an existing cookie -> deletion request */
    if (value_len == 0) {
        g_jar[slot].is_set = 0;
        return;
    }

    g_jar[slot].is_set = 1;
    memcpy(g_jar[slot].name, name_s, name_len);
    g_jar[slot].name[name_len] = '\0';
    memcpy(g_jar[slot].value, value_s, value_len);
    g_jar[slot].value[value_len] = '\0';
    strcpy(g_jar[slot].domain, domain);
    strcpy(g_jar[slot].path, path);
    g_jar[slot].secure = secure;

    /* §5.3 etape 3 : Max-Age l'emporte sur Expires quand les deux sont
     * presents (« Otherwise, if the cookie-attribute-list contains an
     * attribute with an attribute-name of Expires »). Aucun des deux :
     * cookie de session. */
    g_jar[slot].a_expiration = a_max_age || a_expires;
    g_jar[slot].expiration   = a_max_age ? exp_max_age : exp_expires;

    /* Cookie deja expire a la reception (Max-Age<=0, Expires passee) :
     * c'est une revocation. Il n'est pas stocke, et le cookie de meme
     * cle qui occupait le slot part avec lui. */
    if (cookie_est_expire(&g_jar[slot], maintenant)) g_jar[slot].is_set = 0;
}

/* Walk all "Set-Cookie:" headers in response and update jar. */
static void cookie_jar_ingest(const char* host, const char* hdr, int hdr_len) {
    int i = 0;
    /* Skip status line */
    while (i < hdr_len && hdr[i] != '\n') i++;
    i++;
    while (i < hdr_len) {
        if (i + 11 < hdr_len && strncasecmp(hdr + i, "set-cookie:", 11) == 0) {
            i += 11;
            while (i < hdr_len && hdr[i] == ' ') i++;
            int start = i;
            while (i < hdr_len && hdr[i] != '\r' && hdr[i] != '\n') i++;
            cookie_jar_set(host, hdr + start, i - start);
        }
        while (i < hdr_len && hdr[i] != '\n') i++;
        i++;
    }
}

/* Un hote qui est une adresse litterale (IPv4 pointee ou IPv6) n'a pas de
 * domaine parent : RFC 6265 §5.1.3 reserve la regle de suffixe aux NOMS
 * d'hote (« the string is a host name (i.e., not an IP address) »). Sans
 * ce garde, "127.0.0.1" se terminant par ".0.0.1", un serveur pourrait
 * revendiquer "0.0.1" comme domaine couvrant. */
static int host_is_ip_literal(const char* host) {
    int a_chiffre = 0;
    for (const char* p = host; *p; p++) {
        if (*p == ':') return 1;                    /* IPv6 */
        if (*p >= '0' && *p <= '9') { a_chiffre = 1; continue; }
        if (*p != '.') return 0;
    }
    return a_chiffre;
}

/* RFC 6265 §5.1.3 — domain-match : chaines identiques, ou cdomain suffixe
 * de req_host precede d'un '.' et req_host est un nom d'hote. Sert deux
 * fois : a l'ingestion pour valider l'attribut Domain contre l'hote
 * emetteur (§5.3 etape 6), et a l'emission pour selectionner les cookies. */
static int cookie_domain_matches(const char* req_host, const char* cdomain) {
    int rh_len = (int)strlen(req_host);
    int cd_len = (int)strlen(cdomain);
    if (cd_len == 0 || rh_len < cd_len) return 0;
    if (rh_len == cd_len) return strcmp(req_host, cdomain) == 0;
    if (host_is_ip_literal(req_host)) return 0;
    /* sous-domaine : le suffixe doit tomber sur une frontiere de label */
    if (strcmp(req_host + (rh_len - cd_len), cdomain) != 0) return 0;
    return req_host[rh_len - cd_len - 1] == '.';
}

/* RFC 6265 §5.1.4 — path-match : chemins identiques, ou cpath prefixe de
 * req_path avec une frontiere, c'est-a-dire cpath finissant par '/' ou
 * premier caractere de req_path au-dela du prefixe valant '/'. Sans la
 * frontiere, /admin couvrirait /administrateur, qui est une autre
 * ressource.
 * req_path est ici le chemin seul : la chaine de requete et le fragment
 * ne font pas partie du request-path apparie (§5.4). */
static int cookie_path_matches(const char* req_path, const char* cpath) {
    int rp_len = (int)strlen(req_path);
    int cp_len = (int)strlen(cpath);
    if (cp_len == 0) return 1;
    if (rp_len < cp_len) return 0;
    if (strncmp(req_path, cpath, cp_len) != 0) return 0;
    if (rp_len == cp_len) return 1;
    if (cpath[cp_len - 1] == '/') return 1;
    return req_path[cp_len] == '/';
}

/* Build Cookie header and append to req. host is lowercase, is_https selects secure cookies. */
static void cookie_jar_build(ByteBuf* req, const char* host, const char* path, int is_https) {
    /* `path` vient de l'URL et porte encore la chaine de requete et le
     * fragment ; le request-path apparie s'arrete avant (RFC 6265 §5.4).
     * Sans cette coupe, la frontiere de §5.1.4 refuserait /admin?x=1 a un
     * cookie de portee /admin. */
    char req_path[2048];
    int  rp_len = 0;
    while (path[rp_len] && path[rp_len] != '?' && path[rp_len] != '#' &&
           rp_len < (int)sizeof(req_path) - 1) rp_len++;
    memcpy(req_path, path, (size_t)rp_len);
    req_path[rp_len] = '\0';
    if (rp_len == 0) { req_path[0] = '/'; req_path[1] = '\0'; }
    path = req_path;

    /* Un cookie peut expirer entre deux requetes du meme processus : la
     * selection ne peut pas se contenter de ce qui a ete decide a
     * l'ingestion. */
    cookie_jar_evince_expires();

    int first = 1;
    for (int i = 0; i < MAX_COOKIES; i++) {
        Cookie* c = &g_jar[i];
        if (!c->is_set) continue;
        if (c->secure && !is_https) continue;
        if (!cookie_domain_matches(host, c->domain)) continue;
        if (!cookie_path_matches(path, c->path)) continue;
        if (first) {
            bb_append(req, "Cookie: ", 8);
            first = 0;
        } else {
            bb_append(req, "; ", 2);
        }
        bb_append(req, c->name, (int)strlen(c->name));
        bb_append(req, "=", 1);
        bb_append(req, c->value, (int)strlen(c->value));
    }
    if (!first) bb_append(req, "\r\n", 2);
}

static void cookie_jar_clear(void) {
    for (int i = 0; i < MAX_COOKIES; i++) g_jar[i].is_set = 0;
}

/* ================================================================
 * gzip / deflate decompression (via miniz)
 * ================================================================ */

/* Inflate a raw deflate stream (or zlib-wrapped if window_bits > 0). */
static int inflate_to(ByteBuf* out, const unsigned char* in, int in_len, int window_bits) {
    mz_stream s;
    memset(&s, 0, sizeof(s));
    if (mz_inflateInit2(&s, window_bits) != MZ_OK) return -1;
    s.next_in = in;
    s.avail_in = (mz_uint32)in_len;

    unsigned char chunk[8192];
    while (1) {
        s.next_out = chunk;
        s.avail_out = sizeof(chunk);
        int rc = mz_inflate(&s, MZ_NO_FLUSH);
        int got = (int)(sizeof(chunk) - s.avail_out);
        if (got > 0 && bb_append(out, (char*)chunk, got) != 0) {
            mz_inflateEnd(&s);
            return -1;
        }
        if (rc == MZ_STREAM_END) break;
        if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
            mz_inflateEnd(&s);
            return -1;
        }
        if (s.avail_in == 0 && s.avail_out > 0 && rc == MZ_BUF_ERROR) break;
    }
    mz_inflateEnd(&s);
    return 0;
}

/* gzip header parser. Returns offset of deflate stream in `in`, or -1 on error.
 * RFC 1952 format: magic(2) method(1) flags(1) mtime(4) xflags(1) OS(1)
 * then optional FEXTRA / FNAME / FCOMMENT / FHCRC depending on flags. */
static int parse_gzip_header(const unsigned char* in, int in_len) {
    if (in_len < 10) return -1;
    if (in[0] != 0x1f || in[1] != 0x8b) return -1;     /* magic */
    if (in[2] != 8) return -1;                          /* method = deflate */
    int flags = in[3];
    int p = 10;
    if (flags & 0x04) { /* FEXTRA */
        if (p + 2 > in_len) return -1;
        int xlen = in[p] | (in[p+1] << 8);
        p += 2 + xlen;
    }
    if (flags & 0x08) { /* FNAME (zero-terminated) */
        while (p < in_len && in[p] != 0) p++;
        p++;
    }
    if (flags & 0x10) { /* FCOMMENT */
        while (p < in_len && in[p] != 0) p++;
        p++;
    }
    if (flags & 0x02) { /* FHCRC */
        p += 2;
    }
    if (p > in_len) return -1;
    return p;
}

/* Decompress gzip-encoded input into out (clears out first). */
static int gunzip(ByteBuf* out, const unsigned char* in, int in_len) {
    int hdr = parse_gzip_header(in, in_len);
    if (hdr < 0) return -1;
    /* Last 8 bytes = CRC32 + ISIZE; we don't verify (trust the stream). */
    int payload_len = in_len - hdr - 8;
    if (payload_len < 0) return -1;
    bb_free(out);
    bb_init(out);
    return inflate_to(out, in + hdr, payload_len, -MZ_DEFAULT_WINDOW_BITS);
}

/* Decompress deflate (zlib-wrapped). Some servers send raw — try both. */
static int inflate_deflate(ByteBuf* out, const unsigned char* in, int in_len) {
    bb_free(out);
    bb_init(out);
    if (inflate_to(out, in, in_len, MZ_DEFAULT_WINDOW_BITS) == 0) return 0;
    bb_free(out);
    bb_init(out);
    return inflate_to(out, in, in_len, -MZ_DEFAULT_WINDOW_BITS);
}

/* Apply Content-Encoding decompression in-place on body if needed.
 * Returns 0 on success or no decoding needed, -1 on error. */
static int apply_content_encoding(ByteBuf* body, const char* enc, int enc_len) {
    if (enc == NULL || enc_len <= 0) return 0;
    /* Trim whitespace */
    while (enc_len > 0 && (*enc == ' ' || *enc == '\t')) { enc++; enc_len--; }
    while (enc_len > 0 && (enc[enc_len-1] == ' ' || enc[enc_len-1] == '\t')) enc_len--;

    int is_gzip    = (enc_len >= 4 && (strncmp(enc, "gzip", 4) == 0    || strncmp(enc, "Gzip", 4) == 0    || strncmp(enc, "GZIP", 4) == 0));
    int is_deflate = (enc_len >= 7 && (strncmp(enc, "deflate", 7) == 0 || strncmp(enc, "Deflate", 7) == 0 || strncmp(enc, "DEFLATE", 7) == 0));

    if (!is_gzip && !is_deflate) return 0;

    ByteBuf decoded;
    bb_init(&decoded);
    int rc = is_gzip
        ? gunzip(&decoded, (const unsigned char*)body->data, body->len)
        : inflate_deflate(&decoded, (const unsigned char*)body->data, body->len);
    if (rc != 0) { bb_free(&decoded); return -1; }

    bb_free(body);
    *body = decoded;
    return 0;
}

/* ================================================================
 * HTTP response parsing — pilote par llhttp
 * ================================================================ */

/* Recherche d'un en-tete par nom. Ne lit PAS le flux reseau : uniquement le
 * bloc d'en-tetes reconstruit par la couche llhttp ci-dessous, dont les octets
 * ont deja ete valides et l'OWS retire. */
static const char* find_header(const char* hdr, int hdr_len,
                                const char* name, int* out_val_len) {
    int nlen = (int)strlen(name);
    int i = 0;
    while (i < hdr_len && hdr[i] != '\n') i++;
    i++;
    while (i < hdr_len) {
        int j = 0;
        while (j < nlen && i + j < hdr_len) {
            char a = hdr[i + j], b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            j++;
        }
        if (j == nlen && i + j < hdr_len && hdr[i + j] == ':') {
            i += j + 1;
            while (i < hdr_len && hdr[i] == ' ') i++;
            int start = i;
            while (i < hdr_len && hdr[i] != '\r' && hdr[i] != '\n') i++;
            if (out_val_len) *out_val_len = i - start;
            return hdr + start;
        }
        while (i < hdr_len && hdr[i] != '\n') i++;
        i++;
    }
    return NULL;
}

/* llhttp n'impose aucun plafond : ces deux gardes remplacent celles qui
 * vivaient dans read_headers (1 Mo) et read_body_until_close (32 Mo). */
#define MAX_RESP_HEADERS (1024 * 1024)
#define MAX_RESP_BODY    (32 * 1024 * 1024)

/* Etat d'une reponse en cours de parsing.
 *
 * Les callbacks de span de llhttp (on_status, on_header_field,
 * on_header_value, on_body) sont appeles PAR MORCEAUX : n appels pour une
 * meme valeur des que les octets arrivent fragmentes sur le reseau. D'ou
 * les accumulateurs ci-dessous — jamais "un appel = une valeur". */
typedef struct {
    /* Bloc d'en-tetes reconstruit depuis les callbacks, sous la forme
     * "HTTP/1.1 200 OK\r\nNom: valeur\r\n...\r\n". Reconstruit et non
     * capture brut : llhttp livre les en-tetes par callbacks et ne donne
     * pas d'offset de fin de bloc ; re-scanner un CRLFCRLF sur les octets
     * bruts reintroduirait un second parseur (et se tromperait sur une
     * reponse 1xx intercalee). C'est ce bloc que lisent find_header, le
     * jar de cookies et HTTPDonneEntete. */
    ByteBuf   entetes;
    /* Bloc d'en-tetes BRUT : les octets recus tels quels, coupes a la fin du
     * CRLF CRLF. L'offset de coupe est celui rendu par llhttp lui-meme
     * (llhttp_get_error_pos a la pause de fin d'en-tetes) : aucun second
     * parseur, donc aucun risque de se tromper sur une reponse 1xx. */
    ByteBuf   brut;
    int       brut_coupe;  /* -1 tant que la fin du bloc n'est pas connue */
    ByteBuf   raison;      /* raison de la ligne de statut */
    ByteBuf   champ;       /* nom d'en-tete courant */
    ByteBuf   valeur;      /* valeur d'en-tete courante */
    int       ligne_ecrite;
    int       champ_pret;  /* un nom attend sa valeur */
    BodySink* sink;
    int       statut;
    /* p->flags releve DANS on_headers_complete : llhttp les remet a zero en
     * fin de message, F_CHUNKED disparait apres le dernier chunk. */
    uint16_t  drapeaux;
    int       entetes_finis;
    int       message_fini;
} RespParse;

static void rp_init(RespParse* rp, BodySink* sink) {
    memset(rp, 0, sizeof(*rp));
    rp->sink = sink;
    rp->brut_coupe = -1;
}

static void rp_free(RespParse* rp) {
    bb_free(&rp->entetes);
    bb_free(&rp->brut);
    bb_free(&rp->raison);
    bb_free(&rp->champ);
    bb_free(&rp->valeur);
}

/* Ecrit "HTTP/M.m <code> <raison>" une seule fois, en tete du bloc. Appelee
 * au premier en-tete (ou a headers_complete si la reponse n'en a aucun) :
 * a ce moment la ligne de statut est entierement parsee et `raison`
 * entierement accumulee. */
static int rp_ligne_statut(RespParse* rp, llhttp_t* p) {
    if (rp->ligne_ecrite) return 0;
    rp->ligne_ecrite = 1;
    char tete[64];
    int n = snprintf(tete, sizeof(tete), "HTTP/%d.%d %d",
                     (int)p->http_major, (int)p->http_minor, (int)p->status_code);
    if (n < 0 || n >= (int)sizeof(tete)) return -1;
    if (bb_append(&rp->entetes, tete, n) != 0) return -1;
    if (rp->raison.len > 0 &&
        (bb_append(&rp->entetes, " ", 1) != 0 ||
         bb_append(&rp->entetes, rp->raison.data, rp->raison.len) != 0)) return -1;
    return bb_append(&rp->entetes, "\r\n", 2);
}

/* Solde le couple nom/valeur en attente. Les en-tetes repetes le restent
 * (une ligne par occurrence) : c'est ce dont le jar de cookies a besoin. */
static int rp_flush_entete(RespParse* rp, llhttp_t* p) {
    if (rp_ligne_statut(rp, p) != 0) return -1;
    if (!rp->champ_pret) return 0;
    if (rp->entetes.len + rp->champ.len + rp->valeur.len > MAX_RESP_HEADERS) return -1;
    if (bb_append(&rp->entetes, rp->champ.len ? rp->champ.data : "", rp->champ.len) != 0 ||
        bb_append(&rp->entetes, ": ", 2) != 0 ||
        bb_append(&rp->entetes, rp->valeur.len ? rp->valeur.data : "", rp->valeur.len) != 0 ||
        bb_append(&rp->entetes, "\r\n", 2) != 0) return -1;
    rp->champ.len  = 0;
    rp->valeur.len = 0;
    rp->champ_pret = 0;
    return 0;
}

static int rp_on_status(llhttp_t* p, const char* at, size_t len) {
    RespParse* rp = (RespParse*)p->data;
    if (rp->raison.len + (int)len > MAX_RESP_HEADERS) return -1;
    return bb_append(&rp->raison, at, (int)len);
}

static int rp_on_header_field(llhttp_t* p, const char* at, size_t len) {
    RespParse* rp = (RespParse*)p->data;
    if (rp->entetes.len + rp->champ.len + (int)len > MAX_RESP_HEADERS) return -1;
    return bb_append(&rp->champ, at, (int)len);
}

static int rp_on_header_field_complete(llhttp_t* p) {
    ((RespParse*)p->data)->champ_pret = 1;
    return 0;
}

static int rp_on_header_value(llhttp_t* p, const char* at, size_t len) {
    RespParse* rp = (RespParse*)p->data;
    if (rp->entetes.len + rp->valeur.len + (int)len > MAX_RESP_HEADERS) return -1;
    return bb_append(&rp->valeur, at, (int)len);
}

static int rp_on_header_value_complete(llhttp_t* p) {
    return rp_flush_entete((RespParse*)p->data, p);
}

static int rp_on_headers_complete(llhttp_t* p) {
    RespParse* rp = (RespParse*)p->data;
    /* En-tete a valeur vide ("X-Vide:\r\n") : on_header_value peut ne jamais
     * passer, le couple en attente est solde ici. */
    if (rp_flush_entete(rp, p) != 0) return -1;
    if (bb_append(&rp->entetes, "\r\n", 2) != 0) return -1;
    rp->statut        = llhttp_get_status_code(p);
    rp->drapeaux      = p->flags;
    rp->entetes_finis = 1;
    /* HPE_PAUSED fait rendre la main a llhttp_execute pile a la fin du bloc
     * d'en-tetes : c'est ce qui donne l'offset exact de sa derniere ligne, et
     * donc la capture brute. Le parsing du corps reprend par llhttp_resume. */
    return HPE_PAUSED;
}

static int rp_on_body(llhttp_t* p, const char* at, size_t len) {
    RespParse* rp = (RespParse*)p->data;
    if (rp->sink->buf && rp->sink->buf->len + (int)len > MAX_RESP_BODY) return -1;
    return sink_write(rp->sink, at, (int)len);
}

/* HPE_PAUSED arrete llhttp_execute net, et cette fois-ci definitivement (pas
 * de llhttp_resume derriere). Sans ca, des octets restants (reponse pipelinee
 * sur une connexion keep-alive) seraient parses comme un second message et
 * ecraseraient le premier. */
static int rp_on_message_complete(llhttp_t* p) {
    ((RespParse*)p->data)->message_fini = 1;
    return HPE_PAUSED;
}

/* La llhttp_settings_t doit survivre au parseur (contrat de llhttp_init) :
 * statique, initialisee une fois. */
static llhttp_settings_t g_resp_settings;
static int               g_resp_settings_pret = 0;

static const llhttp_settings_t* resp_settings(void) {
    if (!g_resp_settings_pret) {
        llhttp_settings_init(&g_resp_settings);
        g_resp_settings.on_status                = rp_on_status;
        g_resp_settings.on_header_field          = rp_on_header_field;
        g_resp_settings.on_header_field_complete = rp_on_header_field_complete;
        g_resp_settings.on_header_value          = rp_on_header_value;
        g_resp_settings.on_header_value_complete = rp_on_header_value_complete;
        g_resp_settings.on_headers_complete      = rp_on_headers_complete;
        g_resp_settings.on_body                  = rp_on_body;
        g_resp_settings.on_message_complete      = rp_on_message_complete;
        g_resp_settings_pret = 1;
    }
    return &g_resp_settings;
}

/* Note le motif protocolaire du rejet en cours. Renseigne uniquement quand
 * les octets recus sont en cause : un echec socket laisse le motif vide.
 *
 * Le motif est conserve sous la forme d'origine du parseur ("Duplicate
 * Content-Length"...) ; les motifs poses par les gardes OWL suivent la meme
 * forme, sinon le meme champ changerait de registre selon le cas rencontre.
 * La traduction se fait au rendu du message, cf. motif_malforme_fr. */
static void note_reponse_malformee(const char* motif) {
    if (motif == NULL || *motif == '\0') motif = "Malformed response";
    snprintf(g_resp_malformee, sizeof(g_resp_malformee), "%s", motif);
}

/* Lit la reponse du socket et l'alimente a llhttp jusqu'au message complet.
 * Le parseur est un membre de pile : zero malloc, pas de llhttp_alloc.
 * Retourne 0, ou -1 sur rejet du parseur, erreur socket ou corps tronque. */
static int lire_reponse(HttpConn* c, RespParse* rp) {
    llhttp_t parser;
    llhttp_init(&parser, HTTP_RESPONSE, resp_settings());
    parser.data = rp;

    char tampon[4096];
    while (!rp->message_fini) {
        int n = conn_read(c, tampon, sizeof(tampon));
        if (n < 0) return -1;
        llhttp_errno_t err;
        if (n == 0) {
            /* Fermeture du pair : legitime pour un corps delimite par
             * fermeture, troncature sinon. C'est llhttp_finish qui tranche. */
            err = llhttp_finish(&parser);
            if (err != HPE_OK && err != HPE_PAUSED) {
                note_reponse_malformee(llhttp_get_error_reason(&parser));
                return -1;
            }
            break;
        }

        /* Tant que la fin du bloc d'en-tetes n'est pas connue, on garde les
         * octets recus : la coupe exacte arrivera avec la pause ci-dessous. */
        int base = rp->brut.len;
        if (rp->brut_coupe < 0) {
            if (rp->brut.len + n > MAX_RESP_HEADERS) {
                note_reponse_malformee("Header block too large");
                return -1;
            }
            if (bb_append(&rp->brut, tampon, n) != 0) return -1;
        }

        /* Deux pauses possibles : fin d'en-tetes (on reprend) et fin de
         * message (on s'arrete). Une meme lecture peut declencher les deux. */
        const char* cur = tampon;
        int reste = n;
        for (;;) {
            err = llhttp_execute(&parser, cur, (size_t)reste);
            if (err != HPE_PAUSED || rp->message_fini) break;
            int consomme = (int)(llhttp_get_error_pos(&parser) - cur);
            if (rp->brut_coupe < 0) {
                rp->brut_coupe = base + (int)(cur - tampon) + consomme;
                rp->brut.len   = rp->brut_coupe;
            }
            llhttp_resume(&parser);
            cur   += consomme;
            reste -= consomme;
            /* On re-execute meme avec zero octet restant : c'est ce qui laisse
             * llhttp conclure un message sans corps (204, 304, longueur nulle)
             * dont les en-tetes tombaient pile en fin de lecture. */
        }
        if (err != HPE_OK && err != HPE_PAUSED) {
            note_reponse_malformee(llhttp_get_error_reason(&parser));
            return -1;
        }
    }
    if (!rp->message_fini) return -1;

    /* Transfer-Encoding sans `chunked` final. llhttp l'accepte pour une
     * REPONSE (RFC 9112 6.1 : la longueur bascule sur "jusqu'a fermeture")
     * et rend le corps NON decode — le rejet asserte par llhttp_smoketest.c
     * ne porte que sur une REQUETE. On refuse : vecteur de
     * desynchronisation, et le corps rendu serait faux. */
    if ((rp->drapeaux & F_TRANSFER_ENCODING) && !(rp->drapeaux & F_CHUNKED)) {
        note_reponse_malformee("Transfer-Encoding not ending with chunked");
        return -1;
    }

    return 0;
}

/*  — GET HTTP(S) synchrone BORNE, dedie au fetch JWKS sur le chemin
 * d'auth (thread VM, aucune concurrence). Connexion + lecture plafonnees par
 * `timeout_ms` (SO_RCVTIMEO/SO_SNDTIMEO), TLS verifie PLEIN (chaine CA embarquee
 * + hostname, aucun knob off), chemin minimal : pas de pool/keep-alive, pas de
 * cookies, pas de redirection, pas de gzip. En https:// n'importe quel hote ;
 * en http:// l'appelant a deja restreint au loopback (validation compile-time).
 * Ecrit le corps dans *out_body (malloc, NUL-termine, *out_len exclut le NUL) et
 * le statut HTTP dans *out_status. Retourne 0 si une reponse est arrivee (quel
 * que soit le statut), <0 sur echec transport (connexion refusee, timeout, TLS,
 * reponse malformee). L'appelant libere *out_body. Ses globals (pool, jar) ne
 * sont surs que sur le thread VM — c'est exactement d'ou on appelle. */
int owl_http_get_borne(const char* url, int timeout_ms,
                       char** out_body, size_t* out_len, int* out_status) {
    if (out_body)   *out_body = NULL;
    if (out_len)    *out_len = 0;
    if (out_status) *out_status = 0;
    if (url == NULL) return -1;

#ifdef _WIN32
    if (wsa_ensure() != 0) return -1;
#endif

    ParsedURL u;
    if (parse_url(url, (int)strlen(url), &u) != 0) return -1;

    int timeout_sec = (timeout_ms + 999) / 1000;
    if (timeout_sec < 1) timeout_sec = 1;

    HttpConn c;
    conn_init(&c);
    c.sock = connect_tcp(u.host, u.port, timeout_sec);
    if (c.sock == OWL_INVALID_SOCKET) { conn_close(&c); return -1; }

    if (u.is_https) {
        if (tls_handshake_on(&c, u.host, 1 /* verify plein */,
                             NULL, NULL, NULL, 0) != 0) {
            conn_close(&c);
            return -1;
        }
    }

    char reqbuf[3072];
    int rn = snprintf(reqbuf, sizeof(reqbuf),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Accept: application/json\r\n"
                      "User-Agent: owl\r\n"
                      "Connection: close\r\n\r\n",
                      u.path, u.host);
    if (rn <= 0 || rn >= (int)sizeof(reqbuf) ||
        conn_write(&c, reqbuf, rn) != 0) {
        conn_close(&c);
        return -1;
    }

    ByteBuf body; bb_init(&body);
    BodySink sink; memset(&sink, 0, sizeof(sink)); sink.buf = &body;
    RespParse rp; rp_init(&rp, &sink);
    int rc = lire_reponse(&c, &rp);
    int statut = rp.statut;
    rp_free(&rp);
    conn_close(&c);
    if (rc != 0) { bb_free(&body); return -1; }

    if (out_status) *out_status = statut;
    if (body.data == NULL) {
        char* vide = (char*)malloc(1);
        if (vide == NULL) return -1;
        vide[0] = '\0';
        if (out_body) *out_body = vide; else free(vide);
    } else {
        if (out_body) *out_body = body.data; else bb_free(&body);
        if (out_len)  *out_len  = (size_t)body.len;
    }
    return 0;
}

/* ================================================================
 * Options struct (filled from JSON or positional args)
 * ================================================================ */

struct HttpOpts {
    const char* method;          int method_len;
    const char* body;            int body_len;
    const char* content_type;    int content_type_len;
    const char* user_agent;      int user_agent_len;
    const char* headers_raw;     int headers_raw_len;
    ObjJson*    headers_obj;     /* may be NULL */
    const char* auth_basic_user; int auth_basic_user_len;
    const char* auth_basic_pass; int auth_basic_pass_len;
    const char* auth_bearer;     int auth_bearer_len;
    ObjJson*    form;            /* JSON object: URL-encoded form */
    ObjJson*    multipart;       /* JSON object: {fields:{}, files:[]} */
    int         timeout_sec;
    int         follow_redirects;
    int         max_redirects;
    int         verify_tls;
    int         accept_gzip;
    int         cookies;        /* enable cookie jar (default true) */
    int         keep_alive;     /* reuse last conn if same host:port:tls (default false) */
    const char* proxy;          int proxy_len;        /* "http://[user:pass@]host:port" or NULL */
    const char* no_proxy;       int no_proxy_len;     /* comma-separated bypass list */
    /* mTLS: client cert + key. Paths are zero-terminated when copied in. */
    const char* client_cert_path;     int client_cert_path_len;
    const char* client_key_path;      int client_key_path_len;
    const char* client_key_password;  int client_key_password_len;
    /* Streaming: if set, body is written directly to this path. */
    const char* save_to;              int save_to_len;
    /* TLS version range (bitmask of E_protocoleVersions members). 0 = mbedtls defaults. */
    int         tls_version_bits;
    /* IgnoreErreur bitmask — which HTTP/TLS errors to silently ignore. */
    int         ignore_errors;
    /* Redirect downgrade policies (set from IgnoreErreur bits in fn_http_envoie). */
    int         block_downgrade_http;   /* HTTPS→HTTP cross-scheme redirect */
    int         block_downgrade_https;  /* HTTP→HTTPS cross-scheme redirect */
    /* Owned buffer for body assembled from form/multipart (must be freed). */
    char*       owned_body;
    /* Owned buffer for content_type when set by form/multipart builders. */
    char*       owned_ct;
    /* Owned buffer for headers_raw when synthesized (e.g. SOAPAction). */
    char*       owned_headers_raw;
};

static void http_opts_default(HttpOpts* o) {
    memset(o, 0, sizeof(*o));
    o->method = "GET";
    o->method_len = 3;
    o->timeout_sec = DEFAULT_TIMEOUT_SEC;
    o->follow_redirects = 1;
    o->max_redirects = DEFAULT_MAX_REDIRECTS;
    o->verify_tls = 1;
    o->accept_gzip = 1;
    o->cookies = g_cookies_auto;
}

static void http_opts_free(HttpOpts* o) {
    free(o->owned_body);         o->owned_body = NULL;
    free(o->owned_ct);            o->owned_ct = NULL;
    free(o->owned_headers_raw);   o->owned_headers_raw = NULL;
}

/* Apply global HTTPParametre settings to the options struct — called after
 * http_opts_default() so caller can still override with per-request fields. */
static void apply_http_globals(HttpOpts* o) {
    if (g_http_params.proxy_len > 0) {
        o->proxy = g_http_params.proxy;
        o->proxy_len = g_http_params.proxy_len;
    }
    if (g_http_params.no_proxy_len > 0) {
        o->no_proxy = g_http_params.no_proxy;
        o->no_proxy_len = g_http_params.no_proxy_len;
    }
    if (g_http_params.user_agent_len > 0) {
        o->user_agent = g_http_params.user_agent;
        o->user_agent_len = g_http_params.user_agent_len;
    }
    o->timeout_sec      = g_http_params.timeout_sec;
    o->follow_redirects = g_http_params.follow_redirects;
    o->max_redirects    = g_http_params.max_redirects;
    o->accept_gzip      = g_http_params.accept_gzip;
    o->verify_tls       = g_http_params.verify_tls;
    o->keep_alive       = g_http_params.keep_alive;
}

/* Helpers to extract fields from ObjJson by key. */
static const char* json_get_string(ObjJson* obj, const char* key, int* out_len) {
    ObjString k; /* fake key for lookup */
    ObjJson* v;
    ObjString* ks = obj_copy_string(key, (int)strlen(key));
    v = obj_json_object_get(obj, ks);
    (void)k;
    if (v == NULL) return NULL;
    if (v->jtype != JSON_T_STRING) return NULL;
    if (out_len) *out_len = v->as.strVal->length;
    return v->as.strVal->chars;
}

static int json_get_int(ObjJson* obj, const char* key, int defv) {
    ObjString* ks = obj_copy_string(key, (int)strlen(key));
    ObjJson* v = obj_json_object_get(obj, ks);
    if (v == NULL) return defv;
    if (v->jtype == JSON_T_INT) return (int)v->as.intVal;
    if (v->jtype == JSON_T_REAL) return (int)v->as.realVal;
    return defv;
}

static int json_get_bool(ObjJson* obj, const char* key, int defv) {
    ObjString* ks = obj_copy_string(key, (int)strlen(key));
    ObjJson* v = obj_json_object_get(obj, ks);
    if (v == NULL) return defv;
    if (v->jtype == JSON_T_BOOL) return v->as.boolVal ? 1 : 0;
    if (v->jtype == JSON_T_INT) return v->as.intVal != 0;
    return defv;
}

/* ================================================================
 * URL-encoding + form + multipart body builders
 * ================================================================ */

/* Forward decl: defined later in JSON helpers section */
static ObjJson* json_get_object(ObjJson* obj, const char* key);

static int url_encode_append(ByteBuf* b, const char* s, int len) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
             c == '-' || c == '_' || c == '.' || c == '~') {
            if (bb_append(b, (char*)&c, 1) != 0) return -1;
        } else {
            char enc[3] = { '%', hex[c >> 4], hex[c & 0xF] };
            if (bb_append(b, enc, 3) != 0) return -1;
        }
    }
    return 0;
}

/* Build URL-encoded body from {key: "value", ...}.
 * Returns malloc'd buffer (caller frees), sets *out_len. */
static char* build_form_body(ObjJson* form, int* out_len) {
    ByteBuf b; bb_init(&b);
    for (int i = 0; i < form->as.objectVal.count; i++) {
        ObjString* k = form->as.objectVal.keys[i];
        ObjJson*   v = form->as.objectVal.values[i];
        if (i > 0 && bb_append(&b, "&", 1) != 0) { bb_free(&b); return NULL; }
        if (url_encode_append(&b, k->chars, k->length) != 0) { bb_free(&b); return NULL; }
        if (bb_append(&b, "=", 1) != 0) { bb_free(&b); return NULL; }
        if (v->jtype == JSON_T_STRING) {
            if (url_encode_append(&b, v->as.strVal->chars, v->as.strVal->length) != 0) {
                bb_free(&b); return NULL;
            }
        } else if (v->jtype == JSON_T_INT) {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v->as.intVal);
            if (url_encode_append(&b, tmp, n) != 0) { bb_free(&b); return NULL; }
        } else if (v->jtype == JSON_T_BOOL) {
            const char* s = v->as.boolVal ? "true" : "false";
            if (url_encode_append(&b, s, (int)strlen(s)) != 0) { bb_free(&b); return NULL; }
        }
        /* Skip arrays/objects/null silently */
    }
    *out_len = b.len;
    return b.data;
}

/* Generate a random multipart boundary like "----OWLBoundary<32 hex chars>" */
static void make_boundary(char* out, int cap) {
    static const char hex[] = "0123456789abcdef";
    unsigned char rnd[16];
    if (random_bytes(rnd, sizeof(rnd)) != 0) {
        /* fallback: time-based, low quality but never blocks */
        for (size_t i = 0; i < sizeof(rnd); i++) rnd[i] = (unsigned char)(i * 17 + 3);
    }
    int n = snprintf(out, cap, "----OWLBoundary");
    for (int i = 0; i < 16 && n + 2 < cap; i++) {
        out[n++] = hex[rnd[i] >> 4];
        out[n++] = hex[rnd[i] & 0xF];
    }
    out[n] = '\0';
}

/* Read full file into a malloc'd buffer. Returns NULL on error. */
static char* read_file_all(const char* path, int* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 100 * 1024 * 1024) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = (int)got;
    return buf;
}

/* Build multipart/form-data body.
 * `mp` is a JSON object: {fields: {k:v, ...}, files: [{name, filename, content_type, body|path}, ...]}
 * Returns malloc'd body bytes + sets *out_len, *out_boundary (caller frees body and boundary). */
static char* build_multipart_body(ObjJson* mp, int* out_len, char** out_boundary) {
    char boundary[80];
    make_boundary(boundary, sizeof(boundary));

    ByteBuf b; bb_init(&b);

    /* Fields */
    ObjJson* fields = json_get_object(mp, "fields");
    if (fields != NULL) {
        for (int i = 0; i < fields->as.objectVal.count; i++) {
            ObjString* k = fields->as.objectVal.keys[i];
            ObjJson*   v = fields->as.objectVal.values[i];
            if (bb_printf(&b, "--%s\r\n", boundary) != 0) goto fail;
            if (bb_printf(&b, "Content-Disposition: form-data; name=\"%.*s\"\r\n\r\n",
                          k->length, k->chars) != 0) goto fail;
            if (v->jtype == JSON_T_STRING) {
                if (bb_append(&b, v->as.strVal->chars, v->as.strVal->length) != 0) goto fail;
            } else if (v->jtype == JSON_T_INT) {
                if (bb_printf(&b, "%lld", (long long)v->as.intVal) != 0) goto fail;
            } else if (v->jtype == JSON_T_BOOL) {
                if (bb_append(&b, v->as.boolVal ? "true" : "false",
                              v->as.boolVal ? 4 : 5) != 0) goto fail;
            }
            if (bb_append(&b, "\r\n", 2) != 0) goto fail;
        }
    }

    /* Files */
    ObjString* fk = obj_copy_string("files", 5);
    ObjJson* files = obj_json_object_get(mp, fk);
    if (files != NULL && files->jtype == JSON_T_ARRAY) {
        for (int i = 0; i < files->as.arrayVal.count; i++) {
            ObjJson* fobj = files->as.arrayVal.items[i];
            if (fobj->jtype != JSON_T_OBJECT) continue;

            int nl = 0, fnl = 0, ctl = 0;
            const char* name     = json_get_string(fobj, "name", &nl);
            const char* filename = json_get_string(fobj, "filename", &fnl);
            const char* ctype    = json_get_string(fobj, "content_type", &ctl);
            int blen = 0;
            const char* body     = json_get_string(fobj, "body", &blen);
            int plen = 0;
            const char* path     = json_get_string(fobj, "path", &plen);

            char* loaded = NULL;
            if (path != NULL && body == NULL) {
                char zpath[512];
                if (plen >= (int)sizeof(zpath)) continue;
                memcpy(zpath, path, plen); zpath[plen] = '\0';
                loaded = read_file_all(zpath, &blen);
                if (!loaded) continue;
                body = loaded;
                if (filename == NULL) {
                    /* Use basename of path as filename */
                    const char* slash = strrchr(zpath, '/');
                    const char* bs    = strrchr(zpath, '\\');
                    const char* base  = slash > bs ? slash : bs;
                    filename = base ? base + 1 : zpath;
                    fnl = (int)strlen(filename);
                }
            }
            if (name == NULL) { name = "file"; nl = 4; }
            if (filename == NULL) { filename = "file"; fnl = 4; }
            if (ctype == NULL) { ctype = "application/octet-stream"; ctl = 24; }
            if (body == NULL) { free(loaded); continue; }

            if (bb_printf(&b, "--%s\r\n", boundary) != 0) { free(loaded); goto fail; }
            if (bb_printf(&b, "Content-Disposition: form-data; name=\"%.*s\"; filename=\"%.*s\"\r\n",
                          nl, name, fnl, filename) != 0) { free(loaded); goto fail; }
            if (bb_printf(&b, "Content-Type: %.*s\r\n\r\n", ctl, ctype) != 0) { free(loaded); goto fail; }
            if (bb_append(&b, body, blen) != 0) { free(loaded); goto fail; }
            if (bb_append(&b, "\r\n", 2) != 0) { free(loaded); goto fail; }
            free(loaded);
        }
    }

    if (bb_printf(&b, "--%s--\r\n", boundary) != 0) goto fail;

    *out_len = b.len;
    *out_boundary = strdup(boundary);
    return b.data;

fail:
    bb_free(&b);
    return NULL;
}

static ObjJson* json_get_object(ObjJson* obj, const char* key) {
    ObjString* ks = obj_copy_string(key, (int)strlen(key));
    ObjJson* v = obj_json_object_get(obj, ks);
    if (v == NULL || v->jtype != JSON_T_OBJECT) return NULL;
    return v;
}

static void parse_json_options(ObjJson* opts, HttpOpts* o) {
    int n;
    const char* s;
    if ((s = json_get_string(opts, "method", &n)) != NULL) { o->method = s; o->method_len = n; }
    if ((s = json_get_string(opts, "body", &n)) != NULL)   { o->body = s;   o->body_len = n; }
    if ((s = json_get_string(opts, "content_type", &n)) != NULL) { o->content_type = s; o->content_type_len = n; }
    if ((s = json_get_string(opts, "user_agent", &n)) != NULL)   { o->user_agent = s;   o->user_agent_len = n; }
    if ((s = json_get_string(opts, "headers", &n)) != NULL)      { o->headers_raw = s;  o->headers_raw_len = n; }
    if ((s = json_get_string(opts, "auth_basic_user", &n)) != NULL) { o->auth_basic_user = s; o->auth_basic_user_len = n; }
    if ((s = json_get_string(opts, "auth_basic_password", &n)) != NULL) { o->auth_basic_pass = s; o->auth_basic_pass_len = n; }
    if ((s = json_get_string(opts, "auth_bearer", &n)) != NULL)  { o->auth_bearer = s;  o->auth_bearer_len = n; }
    o->headers_obj      = json_get_object(opts, "headers_obj");
    o->form             = json_get_object(opts, "form");
    o->multipart        = json_get_object(opts, "multipart");
    o->timeout_sec      = json_get_int(opts, "timeout", o->timeout_sec);
    o->follow_redirects = json_get_bool(opts, "follow_redirects", o->follow_redirects);
    o->max_redirects    = json_get_int(opts, "max_redirects", o->max_redirects);
    o->verify_tls       = json_get_bool(opts, "verify_tls", o->verify_tls);
    o->accept_gzip      = json_get_bool(opts, "accept_gzip", o->accept_gzip);
    o->cookies          = json_get_bool(opts, "cookies", o->cookies);
    o->keep_alive       = json_get_bool(opts, "keep_alive", o->keep_alive);
    if ((s = json_get_string(opts, "proxy", &n)) != NULL)    { o->proxy = s;    o->proxy_len = n; }
    if ((s = json_get_string(opts, "no_proxy", &n)) != NULL) { o->no_proxy = s; o->no_proxy_len = n; }
    if ((s = json_get_string(opts, "client_cert_path", &n)) != NULL)    { o->client_cert_path = s;    o->client_cert_path_len = n; }
    if ((s = json_get_string(opts, "client_key_path", &n)) != NULL)     { o->client_key_path = s;     o->client_key_path_len = n; }
    if ((s = json_get_string(opts, "client_key_password", &n)) != NULL) { o->client_key_password = s; o->client_key_password_len = n; }
    if ((s = json_get_string(opts, "save_to", &n)) != NULL)             { o->save_to = s;             o->save_to_len = n; }
}

/* Apply form/multipart builders. Returns 0 on success. */
static int finalize_body(HttpOpts* o) {
    if (o->multipart != NULL) {
        char* boundary = NULL;
        int blen = 0;
        char* body = build_multipart_body(o->multipart, &blen, &boundary);
        if (!body || !boundary) { free(body); free(boundary); return -1; }
        o->owned_body = body;
        o->body = body;
        o->body_len = blen;
        /* Build content_type "multipart/form-data; boundary=..." */
        int ct_cap = (int)strlen(boundary) + 32;
        char* ct = malloc(ct_cap);
        if (!ct) { free(boundary); return -1; }
        int ct_len = snprintf(ct, ct_cap, "multipart/form-data; boundary=%s", boundary);
        free(boundary);
        o->owned_ct = ct;
        o->content_type = ct;
        o->content_type_len = ct_len;
        /* Force POST if method is still default GET */
        if (o->method_len == 3 && strncmp(o->method, "GET", 3) == 0) {
            o->method = "POST"; o->method_len = 4;
        }
    } else if (o->form != NULL) {
        int blen = 0;
        char* body = build_form_body(o->form, &blen);
        if (!body) return -1;
        o->owned_body = body;
        o->body = body;
        o->body_len = blen;
        if (o->content_type == NULL) {
            o->content_type = "application/x-www-form-urlencoded";
            o->content_type_len = 33;
        }
        if (o->method_len == 3 && strncmp(o->method, "GET", 3) == 0) {
            o->method = "POST"; o->method_len = 4;
        }
    }
    return 0;
}

/* ================================================================
 * Core HTTP request (single call, no redirect handling)
 * ================================================================ */

static int do_http_request(const ParsedURL* u, HttpOpts* o, ByteBuf* resp_body) {
    /* Une tentative = un verdict : le motif d'une reponse malformee ne doit
     * pas survivre a la requete suivante, ni a un maillon de redirection. */
    g_resp_malformee[0] = '\0';
#ifdef _WIN32
    if (wsa_ensure() != 0) return -1;
#endif
    HttpConn c; conn_init(&c);

    /* Streaming mode: body goes to save_to file rather than memory.
     * Auto-disable gzip (we can't decompress on-the-fly yet). */
    int streaming = (o->save_to_len > 0);
    if (streaming) o->accept_gzip = 0;

    /* Precedence detaillee au-dessus de resolve_proxy. */
    ProxyInfo px;
    int use_proxy = (resolve_proxy(o->proxy, o->proxy_len,
                                    o->no_proxy, o->no_proxy_len,
                                    u->is_https, u->host, &px) == 0 && px.valid);

    /* Try cached connection if keep_alive is requested.
     * (Pool only caches direct connections — proxy paths are not pooled to keep
     * the matching logic simple.) */
    int reused_pool = 0;
    if (o->keep_alive && !use_proxy &&
        pool_acquire(&c, u->host, u->port, u->is_https) >= 0) {
        reused_pool = 1;
    } else if (use_proxy) {
        c.sock = connect_tcp(px.host, px.port, o->timeout_sec);
        if (c.sock == OWL_INVALID_SOCKET) { conn_close(&c); return -1; }
        if (u->is_https) {
            if (proxy_connect_tunnel(c.sock, &px, u->host, u->port) != 0) {
                conn_close(&c); return -1;
            }
            if (tls_handshake_on(&c, u->host, o->verify_tls,
                                  o->client_cert_path, o->client_key_path,
                                  o->client_key_password,
                                  o->tls_version_bits) != 0) {
                conn_close(&c); return -1;
            }
        }
    } else if (u->is_https) {
        if (tls_connect(&c, u->host, u->port, o->timeout_sec, o->verify_tls,
                         o->client_cert_path, o->client_key_path,
                         o->client_key_password, o->tls_version_bits) != 0) {
            conn_close(&c); return -1;
        }
    } else {
        c.sock = connect_tcp(u->host, u->port, o->timeout_sec);
        if (c.sock == OWL_INVALID_SOCKET) { conn_close(&c); return -1; }
    }

    /* Build request headers in a ByteBuf.
     * For HTTP via proxy, the request line uses absolute URL; the Host header
     * stays as the target. For HTTPS via proxy (after CONNECT), it's identical
     * to a direct connection. */
    ByteBuf req; bb_init(&req);

    int proxy_http = (use_proxy && !u->is_https);
    int rc = 0;
    if (proxy_http) {
        if (u->port == 80)
            rc = bb_printf(&req, "%.*s http://%s%s HTTP/1.1\r\n",
                           o->method_len, o->method, u->host, u->path);
        else
            rc = bb_printf(&req, "%.*s http://%s:%d%s HTTP/1.1\r\n",
                           o->method_len, o->method, u->host, u->port, u->path);
    } else {
        rc = bb_printf(&req, "%.*s %s HTTP/1.1\r\n",
                       o->method_len, o->method, u->path);
    }
    /* Host porte le port quand l'URL en portait un (RFC 7230 §5.4). Meme
     * critere que build_url_finale : on restitue ce qui a ete ecrit, on
     * n'ajoute pas un port par defaut absent de l'URL d'origine. */
    if (rc != 0 ||
        (u->port_explicite
             ? bb_printf(&req, "Host: %s:%d\r\n", u->host, u->port)
             : bb_printf(&req, "Host: %s\r\n", u->host)) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }
    /* Connection header: keep-alive only if explicitly requested AND no proxy
     * (pool doesn't cache proxy paths) */
    const char* conn_hdr = (o->keep_alive && !use_proxy)
                          ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    if (bb_append(&req, conn_hdr, (int)strlen(conn_hdr)) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }

    /* For HTTP via proxy, send Proxy-Authorization. (Tunnel CONNECT already
     * carries it for HTTPS.) */
    if (proxy_http && append_proxy_auth(&req, &px) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }

    const char* ua = o->user_agent ? o->user_agent : "OWL/0.1";
    int ua_len = o->user_agent ? o->user_agent_len : 7;
    if (bb_printf(&req, "User-Agent: %.*s\r\n", ua_len, ua) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }
    if (bb_append(&req, "Accept: */*\r\n", 13) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }

    /* Accept-Encoding (advertise compression support so server can compress) */
    if (o->accept_gzip) {
        if (bb_append(&req, "Accept-Encoding: gzip, deflate\r\n", 32) != 0) {
            bb_free(&req); conn_close(&c); return -1;
        }
    }

    /* Cookie header from jar */
    if (o->cookies) {
        char host_lc[256];
        str_lowercase_copy(host_lc, sizeof(host_lc), u->host, (int)strlen(u->host));
        cookie_jar_build(&req, host_lc, u->path, u->is_https);
    }

    /* Content-Type shortcut */
    if (o->content_type_len > 0) {
        if (bb_printf(&req, "Content-Type: %.*s\r\n",
                      o->content_type_len, o->content_type) != 0) {
            bb_free(&req); conn_close(&c); return -1;
        }
    }

    /* Content-Length for body */
    if (o->body_len > 0) {
        if (bb_printf(&req, "Content-Length: %d\r\n", o->body_len) != 0) {
            bb_free(&req); conn_close(&c); return -1;
        }
    }

    /* Basic auth */
    if (o->auth_basic_user_len > 0 || o->auth_basic_pass_len > 0) {
        char userpass[512];
        int upl = snprintf(userpass, sizeof(userpass), "%.*s:%.*s",
                           o->auth_basic_user_len, o->auth_basic_user ? o->auth_basic_user : "",
                           o->auth_basic_pass_len, o->auth_basic_pass ? o->auth_basic_pass : "");
        if (upl > 0 && upl < (int)sizeof(userpass)) {
            unsigned char b64[768];
            size_t b64_len = 0;
            if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                                       (unsigned char*)userpass, (size_t)upl) == 0) {
                if (bb_printf(&req, "Authorization: Basic %.*s\r\n",
                              (int)b64_len, (char*)b64) != 0) {
                    bb_free(&req); conn_close(&c); return -1;
                }
            }
        }
    }

    /* Bearer auth (overrides Basic if both set — by design) */
    if (o->auth_bearer_len > 0) {
        if (bb_printf(&req, "Authorization: Bearer %.*s\r\n",
                      o->auth_bearer_len, o->auth_bearer) != 0) {
            bb_free(&req); conn_close(&c); return -1;
        }
    }

    /* headers_obj (key-value object) */
    if (o->headers_obj != NULL) {
        ObjJson* h = o->headers_obj;
        for (int i = 0; i < h->as.objectVal.count; i++) {
            ObjString* k = h->as.objectVal.keys[i];
            ObjJson*   v = h->as.objectVal.values[i];
            if (v->jtype != JSON_T_STRING) continue;
            if (bb_printf(&req, "%.*s: %.*s\r\n",
                          k->length, k->chars,
                          v->as.strVal->length, v->as.strVal->chars) != 0) {
                bb_free(&req); conn_close(&c); return -1;
            }
        }
    }

    /* Raw headers */
    if (o->headers_raw_len > 0) {
        if (bb_append(&req, o->headers_raw, o->headers_raw_len) != 0) {
            bb_free(&req); conn_close(&c); return -1;
        }
        if (req.len < 2 ||
            req.data[req.len-2] != '\r' || req.data[req.len-1] != '\n') {
            if (bb_append(&req, "\r\n", 2) != 0) {
                bb_free(&req); conn_close(&c); return -1;
            }
        }
    }

    if (bb_append(&req, "\r\n", 2) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }

    /* Capture raw request for debug (headers + body, truncated at MAX_DEBUG_BUF) */
    {
        int hlen = req.len < MAX_DEBUG_BUF ? req.len : MAX_DEBUG_BUF;
        memcpy(g_last_request_raw, req.data, hlen);
        g_last_request_raw_len = hlen;
        if (o->body_len > 0 && g_last_request_raw_len + o->body_len < MAX_DEBUG_BUF) {
            int cap = MAX_DEBUG_BUF - g_last_request_raw_len;
            int blen = o->body_len < cap ? o->body_len : cap;
            memcpy(g_last_request_raw + g_last_request_raw_len, o->body, blen);
            g_last_request_raw_len += blen;
        }
    }

    /* Send headers + body */
    if (conn_write(&c, req.data, req.len) != 0) {
        bb_free(&req); conn_close(&c); return -1;
    }
    bb_free(&req);
    if (o->body_len > 0 && conn_write(&c, o->body, o->body_len) != 0) {
        conn_close(&c); return -1;
    }

    /* Set up sink: buffered ByteBuf or streaming FILE depending on save_to */
    BodySink sink;
    sink.buf = NULL; sink.file = NULL; sink.total = 0;
    FILE* save_fp = NULL;
    if (streaming) {
        char zpath[1024];
        if (o->save_to_len >= (int)sizeof(zpath)) { conn_close(&c); return -1; }
        memcpy(zpath, o->save_to, o->save_to_len);
        zpath[o->save_to_len] = '\0';
        save_fp = fopen(zpath, "wb");
        if (!save_fp) { conn_close(&c); return -1; }
        sink.file = save_fp;
    } else {
        sink.buf = resp_body;
    }

    /* Receive + parse the response: status line, headers, body (chunked,
     * Content-Length or connection-close delimited) all through llhttp. */
    RespParse rp;
    rp_init(&rp, &sink);
    rc = lire_reponse(&c, &rp);
    if (save_fp) fclose(save_fp);

    if (rp.entetes_finis) {
        int hdr_len = rp.entetes.len < MAX_HEADER_BUF ? rp.entetes.len : MAX_HEADER_BUF;
        memcpy(g_last_headers, rp.entetes.data, hdr_len);
        g_last_headers_len = hdr_len;
        int brut_len = rp.brut.len < MAX_HEADER_BUF ? rp.brut.len : MAX_HEADER_BUF;
        if (brut_len > 0) memcpy(g_last_headers_brut, rp.brut.data, brut_len);
        g_last_headers_brut_len = brut_len;
        g_last_status = rp.statut;

        /* Ingest Set-Cookie headers into the global jar */
        if (o->cookies) {
            char host_lc[256];
            str_lowercase_copy(host_lc, sizeof(host_lc), u->host, (int)strlen(u->host));
            cookie_jar_ingest(host_lc, rp.entetes.data, rp.entetes.len);
        }
    }

    /* Apply Content-Encoding decompression (gzip/deflate) if present */
    if (rc == 0 && o->accept_gzip) {
        int ce_len = 0;
        const char* ce = find_header(rp.entetes.data, rp.entetes.len,
                                      "Content-Encoding", &ce_len);
        if (ce != NULL && ce_len > 0) {
            (void)apply_content_encoding(resp_body, ce, ce_len);
        }
    }

    /* Decide whether to cache the connection. Caching requires:
     *  - keep_alive requested
     *  - direct connection (no proxy)
     *  - body length was bounded (chunked or Content-Length)
     *  - server didn't say "Connection: close"
     *  - request succeeded */
    int can_pool = 0;
    if (rc == 0 && o->keep_alive && !use_proxy &&
        (rp.drapeaux & (F_CHUNKED | F_CONTENT_LENGTH)) != 0) {
        int sc_len = 0;
        const char* sc = find_header(rp.entetes.data, rp.entetes.len,
                                      "Connection", &sc_len);
        int server_close = (sc != NULL && sc_len >= 5 &&
                            (strncasecmp(sc, "close", 5) == 0));
        if (!server_close) can_pool = 1;
    }

    /* Capture raw response for debug: headers + (decompressed) body.
     * If streaming to disk, body is empty in resp_body so only headers appear. */
    {
        int hl = rp.brut.len < MAX_DEBUG_BUF ? rp.brut.len : MAX_DEBUG_BUF;
        if (hl > 0) memcpy(g_last_response_raw, rp.brut.data, hl);
        g_last_response_raw_len = hl;
        if (resp_body && resp_body->len > 0 &&
            g_last_response_raw_len < MAX_DEBUG_BUF) {
            int cap = MAX_DEBUG_BUF - g_last_response_raw_len;
            int blen = resp_body->len < cap ? resp_body->len : cap;
            memcpy(g_last_response_raw + g_last_response_raw_len,
                   resp_body->data, blen);
            g_last_response_raw_len += blen;
        }
    }

    rp_free(&rp);
    if (can_pool) pool_keep(&c, u->host, u->port, u->is_https);
    else          conn_close(&c);
    (void)reused_pool;
    return rc;
}

/* ================================================================
 * Redirect-aware request driver
 * ================================================================ */

static int is_redirect_status(int s) {
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

static int do_http_request_with_redirects(ParsedURL* u, HttpOpts* o, ByteBuf* resp) {
    int redirects = 0;
    while (1) {
        /* Reset response buffer between attempts */
        bb_free(resp);
        bb_init(resp);

        int rc = do_http_request(u, o, resp);
        if (rc != 0) return rc;

        if (!o->follow_redirects || !is_redirect_status(g_last_status)) return 0;
        if (redirects >= o->max_redirects) return 0; /* give up, return last */

        int loc_len = 0;
        const char* loc = find_header(g_last_headers, g_last_headers_len,
                                       "Location", &loc_len);
        if (loc == NULL || loc_len == 0) return 0;

        ParsedURL next;
        if (resolve_redirect(u, loc, loc_len, &next) != 0) return 0;

        /* IgnoreErreur — block cross-scheme redirects when requested. */
        if (o->block_downgrade_http  && u->is_https && !next.is_https) return 0;
        if (o->block_downgrade_https && !u->is_https && next.is_https) return 0;

        *u = next;

        /* RFC 7231: 301/302/303 rewrite to GET with empty body;
         * 307/308 preserve method and body. */
        if (g_last_status == 301 || g_last_status == 302 || g_last_status == 303) {
            o->method = "GET";
            o->method_len = 3;
            o->body = NULL;
            o->body_len = 0;
            o->content_type = NULL;
            o->content_type_len = 0;
        }
        redirects++;
    }
}

/* Le motif de rejet est stocke sous sa forme d'origine (anglaise) : c'est la
 * forme rendue par le parseur, et la garde OWL la suit pour que le champ ne
 * change pas de registre selon le cas. Il est traduit ici, au rendu, sinon le
 * message francais contiendrait un fragment anglais.
 *
 * Repli sur la forme d'origine si un motif inconnu apparait : un motif non
 * traduit reste lisible, un motif perdu ne le serait pas. */
static const char* motif_malforme_fr(const char* motif) {
    static const struct { const char* en; const char* fr; } table[] = {
        { "Transfer-Encoding can't be present with Content-Length",
          "Transfer-Encoding et Content-Length ne peuvent pas coexister" },
        { "Duplicate Content-Length",
          "Content-Length présent en double" },
        { "Invalid character in Content-Length",
          "caractère non numérique dans Content-Length" },
        { "Invalid header field char",
          "caractère interdit dans un nom d'entête" },
        { "Missing expected LF after header value",
          "fin de ligne incomplète après une valeur d'entête" },
        { "Transfer-Encoding not ending with chunked",
          "Transfer-Encoding ne se termine pas par chunked" },
        { "Invalid header value char",
          "caractère interdit dans une valeur d'entête" },
        { "Header block too large",
          "bloc d'entêtes trop volumineux" },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(motif, table[i].en) == 0) return table[i].fr;
    return motif;
}

/* Emet l'erreur d'une requete qui a echoue. Deux causes distinctes, deux
 * codes : le serveur a repondu mais sa reponse viole le protocole, ou aucune
 * reponse exploitable n'est parvenue (connexion, timeout, TLS). */
static void emet_erreur_requete(const char* native) {
    if (g_resp_malformee[0] != '\0') {
        const char* detail = owl_error_render_lang_is_en()
                           ? g_resp_malformee
                           : motif_malforme_fr(g_resp_malformee);
        owl_emit_native_error(E_NATIVE_HTTP_MALFORMED_RESPONSE, native,
                              "native", native,
                              "detail", detail,
                              NULL);
        return;
    }
    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", g_last_status);
    owl_emit_native_error(E_NATIVE_HTTP_REQUEST_FAILURE, native,
                          "native", native,
                          "status", status_buf,
                          NULL);
}

/* ================================================================
 * Native bindings
 * ================================================================ */

static Value fn_http_requete(int argCount, Value* args) {
    g_last_status = 0;
    g_last_headers_len = 0;
    g_last_headers_brut_len = 0;

    if (argCount < 1 || !IS_STRING(args[0])) return NIL_VAL;
    ObjString* url = AS_STRING(args[0]);

    HttpOpts opts;
    http_opts_default(&opts);
    apply_http_globals(&opts);   /* inherit global config */

    /* Call form detection:
     *   (url)
     *   (url, json_options)
     *   (url, method, body, headers) -- legacy */
    if (argCount >= 2) {
        if (IS_JSON(args[1])) {
            ObjJson* j = AS_JSON(args[1]);
            if (j->jtype == JSON_T_OBJECT) parse_json_options(j, &opts);
        } else if (IS_STRING(args[1])) {
            ObjString* m = AS_STRING(args[1]);
            opts.method = m->chars;
            opts.method_len = m->length;
        }
    }
    if (argCount >= 3 && IS_STRING(args[2])) {
        ObjString* b = AS_STRING(args[2]);
        opts.body = b->chars; opts.body_len = b->length;
    }
    if (argCount >= 4 && IS_STRING(args[3])) {
        ObjString* h = AS_STRING(args[3]);
        opts.headers_raw = h->chars; opts.headers_raw_len = h->length;
    }

    ParsedURL u;
    if (parse_url(url->chars, url->length, &u) != 0) {
        http_opts_free(&opts);
        owl_emit_native_error(E_NATIVE_HTTP_INVALID_URL, "HTTPRequete",
                              "native", "HTTPRequete",
                              NULL);
        return NIL_VAL;
    }

    /* Build form/multipart body if requested (sets opts.body, opts.content_type) */
    if (finalize_body(&opts) != 0) {
        http_opts_free(&opts);
        owl_emit_native_error(E_NATIVE_HTTP_BUILD_BODY_FAILURE, "HTTPRequete",
                              "native", "HTTPRequete",
                              NULL);
        return NIL_VAL;
    }

    ByteBuf resp; bb_init(&resp);
    int rc = do_http_request_with_redirects(&u, &opts, &resp);
    http_opts_free(&opts);
    if (rc != 0) {
        bb_free(&resp);
        emet_erreur_requete("HTTPRequete");
        return NIL_VAL;
    }

    owl_clear_error();
    Value result = OBJ_VAL(obj_copy_string(resp.data ? resp.data : "", resp.len));
    bb_free(&resp);
    return result;
}

static Value fn_http_dernier_statut(int argCount, Value* args) {
    (void)argCount; (void)args;
    return INTEGER_VAL(g_last_status);
}

/* HTTPEnvoieSOAP(url, body, [action], [options])
 * Sends a SOAP-formatted POST: Content-Type is set to text/xml, SOAPAction
 * header added if action provided. Any extra options (auth, proxy, etc.)
 * from the 4th arg are merged; SOAP-specific fields win. */
static Value fn_http_envoie_soap(int argCount, Value* args) {
    g_last_status = 0;
    g_last_headers_len = 0;
    g_last_headers_brut_len = 0;

    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) return NIL_VAL;
    ObjString* url  = AS_STRING(args[0]);
    ObjString* body = AS_STRING(args[1]);

    const char* action = NULL;
    int action_len = 0;
    if (argCount >= 3 && IS_STRING(args[2])) {
        ObjString* a = AS_STRING(args[2]);
        action = a->chars; action_len = a->length;
    }

    HttpOpts opts;
    http_opts_default(&opts);
    apply_http_globals(&opts);

    if (argCount >= 4 && IS_JSON(args[3])) {
        ObjJson* j = AS_JSON(args[3]);
        if (j->jtype == JSON_T_OBJECT) parse_json_options(j, &opts);
    }

    /* SOAP always forces these. */
    opts.method = "POST"; opts.method_len = 4;
    opts.body = body->chars; opts.body_len = body->length;
    opts.content_type = "text/xml; charset=utf-8"; opts.content_type_len = 23;

    /* Append SOAPAction to headers_raw (combine with any user-provided raw). */
    if (action_len > 0) {
        int extra_cap = action_len + 32;
        int base_len  = opts.headers_raw_len;
        char* buf = malloc((size_t)(base_len + extra_cap + 1));
        if (!buf) { http_opts_free(&opts); return NIL_VAL; }
        int p = 0;
        if (base_len > 0) { memcpy(buf, opts.headers_raw, base_len); p = base_len; }
        p += snprintf(buf + p, extra_cap,
                      "SOAPAction: \"%.*s\"\r\n", action_len, action);
        opts.owned_headers_raw = buf;
        opts.headers_raw = buf;
        opts.headers_raw_len = p;
    }

    ParsedURL u;
    if (parse_url(url->chars, url->length, &u) != 0) { http_opts_free(&opts); return NIL_VAL; }
    if (finalize_body(&opts) != 0) { http_opts_free(&opts); return NIL_VAL; }

    ByteBuf resp; bb_init(&resp);
    int rc = do_http_request_with_redirects(&u, &opts, &resp);
    http_opts_free(&opts);
    if (rc != 0) { bb_free(&resp); return NIL_VAL; }

    Value result = OBJ_VAL(obj_copy_string(resp.data ? resp.data : "", resp.len));
    bb_free(&resp);
    return result;
}

/* ================================================================
 * HTTPTransfereFichier(url_or_req, path) — streaming download
 *
 * Equivalent of calling HTTPEnvoie with save_to set. Body is streamed
 * directly to the destination path with constant memory usage.
 * Returns the httpReponse (Contenu is empty since we saved to disk).
 * ================================================================ */

/* Forward declarations — implementations are in the HTTPEnvoie section below. */
static Value inst_field_or(ObjInstance* inst, const char* name, Value def);
static const char* method_field_to_string(Value vM, int* out_len);
static void envoie_build_headers(HttpOpts* o, ObjAssocArray* entete);
static void envoie_fill_response_headers(ObjAssocArray* dest);
static ObjInstance* clone_template(const char* tname);
static ObjInstance* clone_reponse(const char* native);
static void inst_set_string(ObjInstance* inst, const char* name, const char* val, int vlen);
/*  : inst_set_int forward decl supprimee — utilise canon
 * inst_field_set_int de object.h (deja inclus via object.h transitif). */
static const char* status_text(int code);

static Value fn_http_transfere_fichier(int argCount, Value* args) {
    if (argCount != 2 || !IS_STRING(args[1])) return NIL_VAL;
    /* Build a new argument vector [req_or_url] and call fn_http_envoie with
     * save_to pre-set via opts override. We shortcut by constructing opts
     * directly here. */
    g_last_status = 0;
    g_last_headers_len = 0;
    g_last_headers_brut_len = 0;

    HttpOpts opts; http_opts_default(&opts);
    apply_http_globals(&opts);
    ObjString* url_str = NULL;

    if (IS_STRING(args[0])) {
        url_str = AS_STRING(args[0]);
    } else if (IS_INSTANCE(args[0])) {
        ObjInstance* req = AS_INSTANCE(args[0]);
        if (!req->structName ||
            strcasecmp(req->structName->chars, "httpRequete") != 0) {
            http_opts_free(&opts); return NIL_VAL;
        }
        Value vURL = inst_field_or(req, "URL", NIL_VAL);
        if (!IS_STRING(vURL)) { http_opts_free(&opts); return NIL_VAL; }
        url_str = AS_STRING(vURL);

        /* Reuse the same field extraction as fn_http_envoie */
        Value vM = inst_field_or(req, "Methode", NIL_VAL);
        int mlen = 0; opts.method = method_field_to_string(vM, &mlen); opts.method_len = mlen;

        const unsigned char* body_bytes = NULL; int body_len = 0;
        if (value_as_bytes(inst_field_or(req, "Contenu", NIL_VAL), &body_bytes, &body_len) && body_len > 0) {
            opts.body = (const char*)body_bytes; opts.body_len = body_len;
        }

        Value vCT = inst_field_or(req, "ContentType", NIL_VAL);
        if (IS_STRING(vCT) && AS_STRING(vCT)->length > 0) {
            opts.content_type = AS_STRING(vCT)->chars;
            opts.content_type_len = AS_STRING(vCT)->length;
        }
        Value vUser = inst_field_or(req, "Utilisateur", NIL_VAL);
        Value vPwd  = inst_field_or(req, "MotDePasse", NIL_VAL);
        if (IS_STRING(vUser) && AS_STRING(vUser)->length > 0) {
            opts.auth_basic_user = AS_STRING(vUser)->chars;
            opts.auth_basic_user_len = AS_STRING(vUser)->length;
            if (IS_STRING(vPwd)) {
                opts.auth_basic_pass = AS_STRING(vPwd)->chars;
                opts.auth_basic_pass_len = AS_STRING(vPwd)->length;
            }
        }
        Value vEn = inst_field_or(req, "Entete", NIL_VAL);
        if (IS_ASSOC_ARRAY(vEn)) envoie_build_headers(&opts, AS_ASSOC_ARRAY(vEn));

        Value vCC = inst_field_or(req, "CertificatClient", NIL_VAL);
        if (IS_STRING(vCC) && AS_STRING(vCC)->length > 0) {
            opts.client_cert_path = AS_STRING(vCC)->chars;
            opts.client_cert_path_len = AS_STRING(vCC)->length;
        }
    } else {
        return NIL_VAL;
    }

    /* Override save_to from arg[1] */
    ObjString* dest = AS_STRING(args[1]);
    opts.save_to = dest->chars;
    opts.save_to_len = dest->length;

    ParsedURL u;
    if (parse_url(url_str->chars, url_str->length, &u) != 0) {
        http_opts_free(&opts);
        owl_emit_native_error(E_NATIVE_HTTP_INVALID_URL, "HTTPTransfereFichier",
                              "native", "HTTPTransfereFichier",
                              NULL);
        ObjInstance* rep = clone_reponse("HTTPTransfereFichier");
        if (rep) {
            inst_field_set_int(rep, "CodeEtat", 0);
            inst_set_string(rep, "DescriptionCodeEtat", "Invalid URL", 11);
            return OBJ_VAL(rep);
        }
        return NIL_VAL;
    }
    if (finalize_body(&opts) != 0) {
        http_opts_free(&opts);
        owl_emit_native_error(E_NATIVE_HTTP_BUILD_BODY_FAILURE, "HTTPTransfereFichier",
                              "native", "HTTPTransfereFichier",
                              NULL);
        return NIL_VAL;
    }

    ByteBuf body; bb_init(&body);
    int rc = do_http_request_with_redirects(&u, &opts, &body);
    http_opts_free(&opts);
    if (rc != 0) {
        emet_erreur_requete("HTTPTransfereFichier");
    } else if (g_last_status >= 400) {
        char status_buf[16];
        snprintf(status_buf, sizeof(status_buf), "%d", g_last_status);
        owl_emit_native_error(E_NATIVE_HTTP_STATUS_ERROR, "HTTPTransfereFichier",
                              "native", "HTTPTransfereFichier",
                              "status", status_buf,
                              NULL);
    } else {
        owl_clear_error();
    }

    ObjInstance* rep = clone_reponse("HTTPTransfereFichier");
    if (rep == NULL) { bb_free(&body); return NIL_VAL; }

    /* Root rep pendant la population : envoie_fill_response_headers alloue et
     * peut declencher un GC alors que rep n'est qu'un C local (UAF sous poison). */
    vm_push(OBJ_VAL(rep));

    inst_field_set_int(rep, "CodeEtat", g_last_status);
    inst_set_string(rep, "DescriptionCodeEtat", status_text(g_last_status),
                    (int)strlen(status_text(g_last_status)));
    /* Contenu stays empty — body was streamed to file.
     *  : delegue au canon. */
    {
        ObjBuffer* b = obj_buffer_empty(0);
        vm_push(OBJ_VAL(b));
        inst_field_set_value(rep, "Contenu", OBJ_VAL(b));
        vm_pop();
    }
    int ct_len = 0;
    const char* ct = find_header(g_last_headers, g_last_headers_len, "Content-Type", &ct_len);
    inst_set_string(rep, "ContentType", ct ? ct : "", ct_len);
    inst_set_string(rep, "EnteteBrut", g_last_headers_brut, g_last_headers_brut_len);

    char url_final[2560];
    int ul = build_url_finale(&u, url_final, sizeof(url_final));
    if (ul > 0) inst_set_string(rep, "URL", url_final,
                                 ul < (int)sizeof(url_final) ? ul : (int)sizeof(url_final) - 1);

    Value vE = inst_field_or(rep, "Entete", NIL_VAL);
    if (IS_ASSOC_ARRAY(vE)) {
        envoie_fill_response_headers(AS_ASSOC_ARRAY(vE));
    }

    bb_free(&body);
    (void)rc;
    vm_pop(); /* rep */
    return OBJ_VAL(rep);
}

/* ================================================================
 * HTTPParametre(nomParam, valeur) — global HTTP config
 * ================================================================ */

/* httpParam* constant values : sentinelles centralisees dans sentinels.h */
#include "sentinels.h"
#define HTTP_PARAM_REINITIALISE     OWL_HTTP_PARAM_REINITIALISE
#define HTTP_PARAM_PROXY            OWL_HTTP_PARAM_PROXY
#define HTTP_PARAM_NO_PROXY         OWL_HTTP_PARAM_NO_PROXY
#define HTTP_PARAM_TIMEOUT          OWL_HTTP_PARAM_TIMEOUT
#define HTTP_PARAM_USER_AGENT       OWL_HTTP_PARAM_USER_AGENT
#define HTTP_PARAM_REDIRECTION      OWL_HTTP_PARAM_REDIRECTION
#define HTTP_PARAM_MAX_REDIRECTION  OWL_HTTP_PARAM_MAX_REDIRECTION
#define HTTP_PARAM_COMPRESSION      OWL_HTTP_PARAM_COMPRESSION
#define HTTP_PARAM_VERIF_CERTIFICAT OWL_HTTP_PARAM_VERIF_CERTIFICAT
#define HTTP_PARAM_GARDE_CONNEXION  OWL_HTTP_PARAM_GARDE_CONNEXION

static void copy_to_fixed(char* dst, int dst_cap, int* dst_len, const char* src, int src_len) {
    int n = src_len < dst_cap - 1 ? src_len : dst_cap - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
    *dst_len = n;
}

static Value fn_http_parametre(int argCount, Value* args) {
    (void)argCount;
    if (!IS_INTEGER(args[0])) return BOOL_VAL(false);
    int param = (int)AS_INTEGER(args[0]);

    /* Reset all */
    if (param == HTTP_PARAM_REINITIALISE) {
        memset(&g_http_params, 0, sizeof(g_http_params));
        g_http_params.timeout_sec      = DEFAULT_TIMEOUT_SEC;
        g_http_params.follow_redirects = 1;
        g_http_params.max_redirects    = DEFAULT_MAX_REDIRECTS;
        g_http_params.accept_gzip      = 1;
        g_http_params.verify_tls       = 1;
        g_http_params.keep_alive       = 0;
        /* Verrouillage d'isolation : le decalage d'horloge des self-tests
         * ne survit pas a une remise a zero des parametres. */
        g_horloge_decalage_test        = 0;
        return BOOL_VAL(true);
    }

    /* Extract string or int value depending on param */
    const char* sval = NULL;
    int slen = 0;
    int64_t ival = 0;
    bool bval = false;
    if (IS_STRING(args[1])) { sval = AS_STRING(args[1])->chars; slen = AS_STRING(args[1])->length; }
    else if (IS_INTEGER(args[1])) ival = AS_INTEGER(args[1]);
    else if (IS_BOOL(args[1]))    bval = AS_BOOL(args[1]);
    else if (IS_REAL(args[1]))    ival = (int64_t)AS_REAL(args[1]);

    switch (param) {
        case HTTP_PARAM_PROXY:
            if (sval) copy_to_fixed(g_http_params.proxy, sizeof(g_http_params.proxy),
                                     &g_http_params.proxy_len, sval, slen);
            break;
        case HTTP_PARAM_NO_PROXY:
            if (sval) copy_to_fixed(g_http_params.no_proxy, sizeof(g_http_params.no_proxy),
                                     &g_http_params.no_proxy_len, sval, slen);
            break;
        case HTTP_PARAM_TIMEOUT:
            /* User may pass milliseconds or seconds; heuristic: if > 300, assume ms */
            if (ival > 300) ival /= 1000;
            g_http_params.timeout_sec = (int)(ival > 0 ? ival : DEFAULT_TIMEOUT_SEC);
            break;
        case HTTP_PARAM_USER_AGENT:
            if (sval) copy_to_fixed(g_http_params.user_agent, sizeof(g_http_params.user_agent),
                                     &g_http_params.user_agent_len, sval, slen);
            break;
        case HTTP_PARAM_REDIRECTION:
            g_http_params.follow_redirects = IS_BOOL(args[1]) ? (bval ? 1 : 0) : (ival != 0 ? 1 : 0);
            break;
        case HTTP_PARAM_MAX_REDIRECTION:
            g_http_params.max_redirects = (int)(ival > 0 ? ival : DEFAULT_MAX_REDIRECTS);
            break;
        case HTTP_PARAM_COMPRESSION:
            g_http_params.accept_gzip = IS_BOOL(args[1]) ? (bval ? 1 : 0) : (ival != 0 ? 1 : 0);
            break;
        case HTTP_PARAM_VERIF_CERTIFICAT:
            g_http_params.verify_tls = IS_BOOL(args[1]) ? (bval ? 1 : 0) : (ival != 0 ? 1 : 0);
            break;
        case HTTP_PARAM_GARDE_CONNEXION:
            g_http_params.keep_alive = IS_BOOL(args[1]) ? (bval ? 1 : 0) : (ival != 0 ? 1 : 0);
            break;
        default:
            return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

/* ================================================================
 * Debug helpers + misc
 * ================================================================ */

static Value fn_http_info(int argCount, Value* args) {
    (void)argCount; (void)args;
    /* Return a JSON object describing the HTTP client build.
     * Root `o` on the VM stack so the cascading obj_copy_string /
     * obj_new_json_string allocations (each can trigger GC via the
     * string intern table) don't collect it. */
    ObjJson* o = obj_new_json(JSON_T_OBJECT);
    vm_push(OBJ_VAL(o));
    ObjString* (*mk)(const char*, int) = obj_copy_string;
    obj_json_object_set(o, mk("owl",        3), obj_new_json_string(mk("1.0.0", 5)));
    obj_json_object_set(o, mk("http",       4), obj_new_json_string(mk("1.1",   3)));
    obj_json_object_set(o, mk("tls",        3), obj_new_json_string(mk("1.2",   3)));
    obj_json_object_set(o, mk("mbedtls",    7), obj_new_json_string(mk("3.6.2", 5)));
    obj_json_object_set(o, mk("pcre2",      5), obj_new_json_string(mk("10.44", 5)));
    obj_json_object_set(o, mk("miniz",      5), obj_new_json_string(mk("3.0.2", 5)));
    obj_json_object_set(o, mk("ca_bundle",  9), obj_new_json_string(mk("mozilla-embedded", 16)));
    vm_pop();
    return OBJ_VAL(o);
}

static Value fn_http_donne_chaine_requete(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_copy_string(g_last_request_raw, g_last_request_raw_len));
}

static Value fn_http_donne_chaine_reponse(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_copy_string(g_last_response_raw, g_last_response_raw_len));
}

/* Returns HTTP version from last response status line ("1.0", "1.1"...). */
static Value fn_http_code_http(int argCount, Value* args) {
    (void)argCount; (void)args;
    /* status line starts "HTTP/X.Y " */
    if (g_last_headers_len >= 8 && memcmp(g_last_headers, "HTTP/", 5) == 0) {
        /* find space after version */
        int i = 5;
        while (i < g_last_headers_len && g_last_headers[i] != ' ' &&
               g_last_headers[i] != '\r' && g_last_headers[i] != '\n') i++;
        int vlen = i - 5;
        if (vlen > 0 && vlen < 16) {
            return OBJ_VAL(obj_copy_string(g_last_headers + 5, vlen));
        }
    }
    return OBJ_VAL(obj_copy_string("", 0));
}

/* HTTPDonneResultat(rep, type) — sugar for struct field access.
 * type constants: httpResultContenu=1, httpResultCodeEtat=2, httpResultEntete=3,
 *                 httpResultContentType=4, httpResultEnteteBrut=5, httpResultURL=6,
 *                 httpResultDescriptionCodeEtat=7 */
static Value fn_http_donne_resultat(int argCount, Value* args) {
    if (argCount < 2 || !IS_INSTANCE(args[0]) || !IS_INTEGER(args[1])) return NIL_VAL;
    ObjInstance* rep = AS_INSTANCE(args[0]);
    int type = (int)AS_INTEGER(args[1]);
    const char* field = NULL;
    switch (type) {
        case OWL_HTTP_RESULT_CONTENU:        field = "Contenu"; break;
        case OWL_HTTP_RESULT_CODE_ETAT:      field = "CodeEtat"; break;
        case OWL_HTTP_RESULT_ENTETE:         field = "Entete"; break;
        case OWL_HTTP_RESULT_CONTENT_TYPE:   field = "ContentType"; break;
        case OWL_HTTP_RESULT_ENTETE_BRUT:    field = "EnteteBrut"; break;
        case OWL_HTTP_RESULT_URL:            field = "URL"; break;
        case OWL_HTTP_RESULT_DESC_CODE_ETAT: field = "DescriptionCodeEtat"; break;
        default: return NIL_VAL;
    }
    ObjString* k = obj_copy_string(field, (int)strlen(field));
    Value v;
    if (table_get(&rep->fields, k, &v)) return v;
    return NIL_VAL;
}

/* HTTPCookie(nom) — reads a cookie's value.
 * HTTPCookie(nom, valeur, [domaine], [path]) — writes. */
static Value fn_http_cookie(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return NIL_VAL;
    ObjString* name = AS_STRING(args[0]);

    if (argCount == 1) {
        /* Read: find first match by name. Un cookie expire n'est plus
         * lisible — sinon la revocation ne serait vraie que sur le fil. */
        cookie_jar_evince_expires();
        for (int i = 0; i < MAX_COOKIES; i++) {
            if (!g_jar[i].is_set) continue;
            if (strcmp(g_jar[i].name, name->chars) == 0) {
                return OBJ_VAL(obj_copy_string(g_jar[i].value,
                                                (int)strlen(g_jar[i].value)));
            }
        }
        return OBJ_VAL(obj_copy_string("", 0));
    }

    /* Write: name, value, [domain], [path] */
    if (!IS_STRING(args[1])) return BOOL_VAL(false);
    ObjString* val = AS_STRING(args[1]);
    const char* domain = "";
    const char* path   = "/";
    if (argCount >= 3 && IS_STRING(args[2])) domain = AS_STRING(args[2])->chars;
    if (argCount >= 4 && IS_STRING(args[3])) path   = AS_STRING(args[3])->chars;

    /* Reuse cookie_jar_set by building a fake Set-Cookie string */
    char buf[1600];
    int n = snprintf(buf, sizeof(buf), "%s=%s; Domain=%s; Path=%s",
                     name->chars, val->chars,
                     domain[0] ? domain : "localhost",
                     path);
    if (n < 0 || n >= (int)sizeof(buf)) return BOOL_VAL(false);
    cookie_jar_set(domain[0] ? domain : "localhost", buf, n);
    return BOOL_VAL(true);
}

/* HTTPCookieGere([actif]) — global toggle for auto cookie jar.
 * No args: returns current state. With bool arg: sets. */
static Value fn_http_cookie_gere(int argCount, Value* args) {
    if (argCount == 0) return BOOL_VAL(g_cookies_auto != 0);
    if (IS_BOOL(args[0]))    g_cookies_auto = AS_BOOL(args[0]) ? 1 : 0;
    else if (IS_INTEGER(args[0])) g_cookies_auto = AS_INTEGER(args[0]) != 0 ? 1 : 0;
    return BOOL_VAL(g_cookies_auto != 0);
}

static Value fn_http_vide_cookies(int argCount, Value* args) {
    (void)argCount; (void)args;
    cookie_jar_clear();
    return NIL_VAL;
}

static Value fn_http_ferme_session(int argCount, Value* args) {
    (void)argCount; (void)args;
    pool_invalidate();
    return NIL_VAL;
}

/* ================================================================
 * HTTPEnvoie(req_or_url) — struct-based request API
 * ================================================================ */

/* Case-insensitive + accent-agnostic field fetch. Returns the value, or
 * `def` when the field is absent. */
static Value inst_field_or(ObjInstance* inst, const char* name, Value def) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    Value v;
    if (table_get(&inst->fields, k, &v)) return v;
    return def;
}

/* Resout la valeur du champ Methode d'un httpRequête vers son verbe HTTP.
 * Depuis  les constantes httpGet/… sont des chaines : le champ porte
 * toujours "GET"/"POST"/… ; un champ vide ou absent retombe sur "GET". */
static const char* method_field_to_string(Value vM, int* out_len) {
    if (IS_STRING(vM) && AS_STRING(vM)->length > 0) {
        *out_len = AS_STRING(vM)->length;
        return AS_STRING(vM)->chars;
    }
    *out_len = 3;
    return "GET";
}

/* Build HttpOpts.headers_raw buffer from the Entete assoc array. */
static void envoie_build_headers(HttpOpts* o, ObjAssocArray* entete) {
    if (entete == NULL || entete->count <= 0) return;
    /* Estimate capacity: each entry ~ 64 bytes + key.length + value.length */
    int cap = 64;
    for (int i = 0; i < entete->count; i++) {
        cap += entete->keys[i]->length + 4;
        if (IS_STRING(entete->values[i])) cap += AS_STRING(entete->values[i])->length;
        else cap += 16;
    }
    char* buf = malloc((size_t)cap);
    if (!buf) return;
    int pos = 0;
    for (int i = 0; i < entete->count; i++) {
        const char* kc = entete->keys[i]->chars;
        int kl = entete->keys[i]->length;
        const char* vc = ""; int vl = 0;
        Value v = entete->values[i];
        char tmp[64];
        if (IS_STRING(v)) { vc = AS_STRING(v)->chars; vl = AS_STRING(v)->length; }
        else if (IS_INTEGER(v)) { vl = snprintf(tmp, sizeof(tmp), "%lld", (long long)AS_INTEGER(v)); vc = tmp; }
        else if (IS_BOOL(v))    { if (AS_BOOL(v)) { vc = "true"; vl = 4; } else { vc = "false"; vl = 5; } }
        int need = kl + 2 + vl + 2 + 1;
        if (pos + need >= cap) {
            cap = (pos + need) * 2;
            char* nb = realloc(buf, (size_t)cap);
            if (!nb) { free(buf); return; }
            buf = nb;
        }
        memcpy(buf + pos, kc, kl); pos += kl;
        buf[pos++] = ':'; buf[pos++] = ' ';
        memcpy(buf + pos, vc, vl); pos += vl;
        buf[pos++] = '\r'; buf[pos++] = '\n';
    }
    o->owned_headers_raw = buf;
    o->headers_raw = buf;
    o->headers_raw_len = pos;
}

/* Populate headers_assoc in response instance from g_last_headers. */
static void envoie_fill_response_headers(ObjAssocArray* dest) {
    int i = 0;
    /* Skip status line */
    while (i < g_last_headers_len && g_last_headers[i] != '\n') i++;
    i++;
    while (i < g_last_headers_len) {
        if (g_last_headers[i] == '\r' || g_last_headers[i] == '\n') break;
        int ks = i;
        while (i < g_last_headers_len && g_last_headers[i] != ':' &&
               g_last_headers[i] != '\r' && g_last_headers[i] != '\n') i++;
        int kl = i - ks;
        if (i < g_last_headers_len && g_last_headers[i] == ':') {
            i++;
            while (i < g_last_headers_len && g_last_headers[i] == ' ') i++;
            int vs = i;
            while (i < g_last_headers_len && g_last_headers[i] != '\r' &&
                   g_last_headers[i] != '\n') i++;
            int vl = i - vs;
            if (kl > 0) {
                /*  lot 6 : k non rootee pendant alloc de v -> push.
                 * obj_assoc_set utilise libc realloc (pas GC) donc seul ce
                 * push est critique. dest est root par appelant. */
                ObjString* k = obj_copy_string(g_last_headers + ks, kl);
                vm_push(OBJ_VAL(k));
                ObjString* v = obj_copy_string(g_last_headers + vs, vl);
                obj_assoc_set(dest, k, OBJ_VAL(v));
                vm_pop(); /* k */
            }
        }
        while (i < g_last_headers_len && g_last_headers[i] != '\n') i++;
        i++;
    }
}

static const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "";
    }
}

/* Clone du modele de type nomme `tname` (doit etre une ObjInstance).
 *
 * Les modeles de types builtin vivent dans vm.types, pas dans vm.globals :
 * ce lookup ciblait vm.globals depuis le commit initial, anterieur a la
 * separation des deux namespaces, et echouait donc systematiquement. */
static ObjInstance* clone_template(const char* tname) {
    ObjString* k = obj_copy_string(tname, (int)strlen(tname));
    Value v;
    if (!table_get(&vm.types, k, &v)) return NULL;
    if (!IS_INSTANCE(v)) return NULL;
    ObjInstance* tpl = AS_INSTANCE(v);
    ObjInstance* out = obj_new_instance(tpl->structName);
    /*  : root out pendant les iterations. */
    vm_push(OBJ_VAL(out));
    for (int i = 0; i < tpl->fields.capacity; i++) {
        Entry* e = &tpl->fields.entries[i];
        if (e->key == NULL) continue;
        Value fv = e->value;
        if (IS_ASSOC_ARRAY(fv)) fv = OBJ_VAL(obj_assoc_clone(AS_ASSOC_ARRAY(fv)));
        obj_inst_set_kv(out, e->key, fv);
    }
    vm_pop(); /* out */
    return out;
}

/* Clone du modele httpReponse, bruyant en cas d'echec : une native qui ne
 * peut pas construire sa valeur de retour doit le signaler, pas rendre Null
 * en silence — c'est ce silence qui a masque le mauvais namespace ci-dessus
 * pendant des mois. */
static ObjInstance* clone_reponse(const char* native) {
    ObjInstance* rep = clone_template("httpReponse");
    if (rep == NULL) {
        owl_emit_native_error(E_NATIVE_HTTP_RESPONSE_TEMPLATE_MISSING, native,
                              "native", native,
                              "type", "httpReponse",
                              NULL);
    }
    return rep;
}

/*  : helpers inst_set_string/int/value supprimes — duplicatas
 * locaux GC-unsafe (k/v non root avant table_set qui peut grow → UAF
 * latent dans la construction des reponses HTTP, non couvert par les
 * sentinels stress-GC car parsing HTTP hors panel). Migration vers le
 * canon inst_field_set_str/int/value de object.c. Wrapper local conserve
 * pour la signature inst_set_string(name, val, vlen) — le canon prend
 * strlen au lieu d'un vlen explicite. */
static void inst_set_string(ObjInstance* inst, const char* name, const char* val, int vlen) {
    /* Cas particulier : val peut etre un buffer non null-terminated (vlen
     * passe explicitement par le caller HTTP). On construit la string
     * proprement puis on delegue. */
    vm_push(OBJ_VAL(inst));
    ObjString* v = obj_copy_string(val, vlen);
    vm_push(OBJ_VAL(v));
    inst_field_set_value(inst, name, OBJ_VAL(v));
    vm_pop();
    vm_pop();
}

/* HTTPEnvoie(req) ou HTTPEnvoie(url) */
static Value fn_http_envoie(int argCount, Value* args) {
    if (argCount != 1) return NIL_VAL;
    g_last_status = 0;
    g_last_headers_len = 0;
    g_last_headers_brut_len = 0;

    HttpOpts opts; http_opts_default(&opts);
    apply_http_globals(&opts);   /* inherit global config */
    ObjString* url_str = NULL;

    if (IS_STRING(args[0])) {
        url_str = AS_STRING(args[0]);
    } else if (IS_INSTANCE(args[0])) {
        ObjInstance* req = AS_INSTANCE(args[0]);
        if (!req->structName ||
            strcasecmp(req->structName->chars, "httpRequete") != 0) {
            http_opts_free(&opts); return NIL_VAL;
        }

        Value vURL = inst_field_or(req, "URL", NIL_VAL);
        if (!IS_STRING(vURL)) { http_opts_free(&opts); return NIL_VAL; }
        url_str = AS_STRING(vURL);

        /* Method */
        Value vM = inst_field_or(req, "Methode", NIL_VAL);
        int mlen = 0;
        const char* mstr = method_field_to_string(vM, &mlen);
        opts.method = mstr; opts.method_len = mlen;

        /* Body — accept both chaine and buffer. */
        Value vB = inst_field_or(req, "Contenu", NIL_VAL);
        const unsigned char* body_bytes = NULL;
        int body_len = 0;
        if (value_as_bytes(vB, &body_bytes, &body_len) && body_len > 0) {
            opts.body = (const char*)body_bytes;
            opts.body_len = body_len;
        }

        /* Content-Type */
        Value vCT = inst_field_or(req, "ContentType", NIL_VAL);
        if (IS_STRING(vCT) && AS_STRING(vCT)->length > 0) {
            opts.content_type = AS_STRING(vCT)->chars;
            opts.content_type_len = AS_STRING(vCT)->length;
        }

        /* Basic auth */
        Value vUser = inst_field_or(req, "Utilisateur", NIL_VAL);
        Value vPwd  = inst_field_or(req, "MotDePasse", NIL_VAL);
        if (IS_STRING(vUser) && AS_STRING(vUser)->length > 0) {
            opts.auth_basic_user = AS_STRING(vUser)->chars;
            opts.auth_basic_user_len = AS_STRING(vUser)->length;
            if (IS_STRING(vPwd)) {
                opts.auth_basic_pass = AS_STRING(vPwd)->chars;
                opts.auth_basic_pass_len = AS_STRING(vPwd)->length;
            }
        }

        /* Entete (assoc array) -> headers_raw string */
        Value vEn = inst_field_or(req, "Entete", NIL_VAL);
        if (IS_ASSOC_ARRAY(vEn)) {
            envoie_build_headers(&opts, AS_ASSOC_ARRAY(vEn));
        }

        /* Timeout override (ms). 0 = inherit global. */
        Value vTC = inst_field_or(req, "TimeoutConnexion", INTEGER_VAL(0));
        if (IS_INTEGER(vTC) && AS_INTEGER(vTC) > 0) {
            opts.timeout_sec = (int)(AS_INTEGER(vTC) / 1000);
        }
        Value vDR = inst_field_or(req, "DureeNonReponse", INTEGER_VAL(0));
        if (IS_INTEGER(vDR) && AS_INTEGER(vDR) > 0) {
            int secs = (int)(AS_INTEGER(vDR) / 1000);
            if (secs > opts.timeout_sec) opts.timeout_sec = secs;
        }

        /* mTLS */
        Value vCC = inst_field_or(req, "CertificatClient", NIL_VAL);
        if (IS_STRING(vCC) && AS_STRING(vCC)->length > 0) {
            opts.client_cert_path = AS_STRING(vCC)->chars;
            opts.client_cert_path_len = AS_STRING(vCC)->length;
        }
        /* Canon : .p12 contient cert+clé, le mot de passe sert à déchiffrer. */
        Value vCKP = inst_field_or(req, "MotDePasseCertificatClient", NIL_VAL);
        if (IS_STRING(vCKP) && AS_STRING(vCKP)->length > 0) {
            opts.client_key_password = AS_STRING(vCKP)->chars;
            opts.client_key_password_len = AS_STRING(vCKP)->length;
        }

        /* Palier 1 — trivial fields. */
        Value vUA = inst_field_or(req, "AgentUtilisateur", NIL_VAL);
        if (IS_STRING(vUA) && AS_STRING(vUA)->length > 0) {
            opts.user_agent = AS_STRING(vUA)->chars;
            opts.user_agent_len = AS_STRING(vUA)->length;
        }
        Value vDest = inst_field_or(req, "Destination", NIL_VAL);
        if (IS_STRING(vDest) && AS_STRING(vDest)->length > 0) {
            opts.save_to = AS_STRING(vDest)->chars;
            opts.save_to_len = AS_STRING(vDest)->length;
        }

        /* Palier 2 — enum-constrained bitmasks. */
        Value vVSSL = inst_field_or(req, "VersionSSL", INTEGER_VAL(0));
        if (IS_INTEGER(vVSSL)) opts.tls_version_bits = (int)AS_INTEGER(vVSSL);
        Value vIGE = inst_field_or(req, "IgnoreErreur", INTEGER_VAL(0));
        if (IS_INTEGER(vIGE)) {
            int bits = (int)AS_INTEGER(vIGE);
            opts.ignore_errors = bits;
            /* bits 1|2|4|64 = all cert-check related → turn off TLS verify. */
            if (bits & (1 | 2 | 4 | 64)) opts.verify_tls = 0;
            /* bit 8 = ignore redirections (don't follow). */
            if (bits & 8) opts.follow_redirects = 0;
            /* bits 16|32 = block cross-scheme redirect downgrade/upgrade. */
            if (bits & 16) opts.block_downgrade_http  = 1;
            if (bits & 32) opts.block_downgrade_https = 1;
        }
    } else {
        return NIL_VAL;
    }

    ParsedURL u;
    if (parse_url(url_str->chars, url_str->length, &u) != 0) {
        http_opts_free(&opts);
        owl_emit_native_error(E_NATIVE_HTTP_INVALID_URL, "HTTPEnvoie",
                              "native", "HTTPEnvoie",
                              NULL);
        ObjInstance* rep = clone_reponse("HTTPEnvoie");
        if (rep) {
            inst_field_set_int(rep, "CodeEtat", 0);
            inst_set_string(rep, "DescriptionCodeEtat", "Invalid URL", 11);
            return OBJ_VAL(rep);
        }
        return NIL_VAL;
    }

    if (finalize_body(&opts) != 0) {
        http_opts_free(&opts);
        owl_emit_native_error(E_NATIVE_HTTP_BUILD_BODY_FAILURE, "HTTPEnvoie",
                              "native", "HTTPEnvoie",
                              NULL);
        return NIL_VAL;
    }

    /* Reset handshake snapshots before sending. */
    g_last_cipher_name[0] = '\0';
    g_last_tls_version_bits = 0;

    ByteBuf body; bb_init(&body);
    int rc = do_http_request_with_redirects(&u, &opts, &body);
    http_opts_free(&opts);

    /* Palier 1 — writeback read-only post-handshake fields into the
     * httpRequête instance if the caller gave us one. */
    if (IS_INSTANCE(args[0])) {
        ObjInstance* req = AS_INSTANCE(args[0]);
        if (g_last_cipher_name[0] != '\0') {
            inst_set_string(req, "CipherChoisi",
                            g_last_cipher_name, (int)strlen(g_last_cipher_name));
        }
        if (g_last_tls_version_bits != 0) {
            inst_field_set_int(req, "VersionSSLChoisie", g_last_tls_version_bits);
        }
    }
    if (rc != 0) {
        emet_erreur_requete("HTTPEnvoie");
    } else if (g_last_status >= 400) {
        char status_buf[16];
        snprintf(status_buf, sizeof(status_buf), "%d", g_last_status);
        owl_emit_native_error(E_NATIVE_HTTP_STATUS_ERROR, "HTTPEnvoie",
                              "native", "HTTPEnvoie",
                              "status", status_buf,
                              NULL);
    } else {
        owl_clear_error();
    }

    /* Clone the httpReponse template and populate it. */
    ObjInstance* rep = clone_reponse("HTTPEnvoie");
    if (rep == NULL) { bb_free(&body); return NIL_VAL; }

    /* Root rep pendant TOUTE la population. Les inst_field_set_* s'auto-rootent
     * le temps de leur propre appel, mais envoie_fill_response_headers recoit
     * l'assoc Entete brut (contrat "dest root par appelant") et alloue des
     * ObjString qui peuvent declencher un GC : sans rep sur la pile VM, rep et
     * son champ Entete (le dest) sont collectes -> UAF sous stress-GC/poison. */
    vm_push(OBJ_VAL(rep));

    inst_field_set_int(rep, "CodeEtat", g_last_status);
    inst_set_string(rep, "DescriptionCodeEtat", status_text(g_last_status),
                    (int)strlen(status_text(g_last_status)));
    /* Contenu as buffer (canon-consistent).
     *  : delegue au canon. */
    {
        ObjBuffer* cb = obj_new_buffer(rc == 0 && body.data ? body.data : "",
                                        rc == 0 ? body.len : 0);
        vm_push(OBJ_VAL(cb));
        inst_field_set_value(rep, "Contenu", OBJ_VAL(cb));
        vm_pop();
    }

    /* ContentType from response headers */
    int ct_len = 0;
    const char* ct = find_header(g_last_headers, g_last_headers_len, "Content-Type", &ct_len);
    inst_set_string(rep, "ContentType", ct ? ct : "", ct_len);

    /* EnteteBrut */
    inst_set_string(rep, "EnteteBrut", g_last_headers_brut, g_last_headers_brut_len);

    /* URL finale : schema + hote (+ port si ecrit) + chemin, apres
     * redirections eventuelles. */
    char url_final[2560];
    int ul = build_url_finale(&u, url_final, sizeof(url_final));
    if (ul > 0) inst_set_string(rep, "URL", url_final, ul < (int)sizeof(url_final) ? ul : (int)sizeof(url_final) - 1);

    /* Entete (assoc array): parse g_last_headers into the response's Entete */
    Value vE = inst_field_or(rep, "Entete", NIL_VAL);
    if (IS_ASSOC_ARRAY(vE)) {
        envoie_fill_response_headers(AS_ASSOC_ARRAY(vE));
    } else {
        /* Template cloning missed it — make a fresh one. */
        ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
        vm_push(OBJ_VAL(a));   /* root : envoie_fill_response_headers alloue avant l'attache a rep */
        envoie_fill_response_headers(a);
        inst_field_set_value(rep, "Entete", OBJ_VAL(a));
        vm_pop(); /* a */
    }

    bb_free(&body);
    vm_pop(); /* rep */
    return OBJ_VAL(rep);
}

static Value fn_http_dernier_entete(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* name = AS_STRING(args[0]);

    char zname[128];
    if (name->length >= (int)sizeof(zname)) return OBJ_VAL(obj_copy_string("", 0));
    memcpy(zname, name->chars, name->length);
    zname[name->length] = '\0';

    int vlen = 0;
    const char* v = find_header(g_last_headers, g_last_headers_len, zname, &vlen);
    if (v == NULL) return OBJ_VAL(obj_copy_string("", 0));
    return OBJ_VAL(obj_copy_string(v, vlen));
}

/* ================================================================
 * INIT
 * ================================================================ */

/* ================================================================
 * Registry declarations — HTTP family.
 * Canonical names use accents: HTTPRequête, HTTPParamètre,
 * HTTPDernierEntête, HTTPDonneRésultat, HTTPTransfèreFichier, etc.
 * canonicalAscii fallback for runtime compat.
 * ================================================================ */

/* ---- Enums ---- exposed to stdlib_builtin_types.c via extern. */
static const char* const E_httpMethodes_values[] = {
    "httpGet", "httpPost", "httpPut", "httpDelete",
    "httpHead", "httpPatch", "httpCopy", NULL
};
const OwlEnum E_httpMethodes = {
    .name="httpM\xc3\xa9thodes", .nameAscii="httpMethodes", .nameEN="httpMethods", .category="HTTP",
    .descFR="Méthodes HTTP (verbes de requête).",
    .descEN="HTTP methods (request verbs).",
    .values=E_httpMethodes_values, .combinable=false
};

static const char* const E_httpParametres_values[] = {
    "httpParamReinitialise", "httpParamProxy", "httpParamNoProxy",
    "httpParamTimeoutConnexion",
    "httpParamUserAgent", "httpParamRedirection", "httpParamMaxRedirection",
    "httpParamCompression", "httpParamVerifCertificat", "httpParamGardeConnexion",
    NULL
};
const OwlEnum E_httpParametres = {
    .name="httpParam\xc3\xa8tres", .nameAscii="httpParametres", .nameEN="httpParameters", .category="HTTP",
    .descFR="Paramètres globaux de HTTPParamètre.",
    .descEN="Global parameters for HTTPParametre.",
    .values=E_httpParametres_values, .combinable=false
};

static const char* const E_httpResultats_values[] = {
    "httpResultContenu", "httpResultCodeEtat", "httpResultEntete",
    "httpResultContentType", "httpResultEnteteBrut", "httpResultURL",
    "httpResultDescriptionCodeEtat", NULL
};
const OwlEnum E_httpResultats = {
    .name="httpR\xc3\xa9sultats", .nameAscii="httpResultats", .nameEN="httpResults", .category="HTTP",
    .descFR="Composants extractibles d'une httpRéponse via HTTPDonneRésultat.",
    .descEN="Components extractible from a httpResponse via HTTPGetResult.",
    .values=E_httpResultats_values, .combinable=false
};

/* Versions TLS — bitmask combinable, utilisable pour VersionSSL (min/max
 * négociation) ET VersionSSLChoisie (un seul bit après handshake). */
static const char* const E_protocoleVersions_values[] = {
    "ProtocoleD\xc3\xa9" "faut",      /* ProtocoleDéfaut */
    "ProtocoleTLS1",
    "ProtocoleTLS1_1",
    "ProtocoleTLS1_2",
    "ProtocoleTLS1_3",
    NULL
};
const OwlEnum E_protocoleVersions = {
    .name="protocoleVersions", .nameAscii="protocoleVersions", .nameEN="protocolVersions",
    .category="HTTP",
    .descFR="Versions TLS (bitflags combinables : min+max de négociation).",
    .descEN="TLS versions (combinable bitflags: negotiation min+max).",
    .values=E_protocoleVersions_values, .combinable=true
};

/* Erreurs HTTP/TLS ignorables — bitmask combinable.
 * 9 valeurs canon doc httpRequête.IgnoreErreur. */
static const char* const E_httpIgnoreErreur_values[] = {
    "httpIgnoreCertificatInvalide",
    "httpIgnoreNomCertificatInvalide",
    "httpIgnoreCertificatExpir\xc3\xa9",  /* httpIgnoreCertificatExpiré */
    "httpIgnoreRedirection",
    "httpIgnoreRenvoiHTTP",
    "httpIgnoreRenvoiHTTPS",
    "httpIgnoreR\xc3\xa9vocation",         /* httpIgnoreRévocation */
    NULL
};
/* Enum de noms canon 2024 pour Content-Type. Les valeurs sont les
 * noms des constantes typeMime* (chacune est elle-même une chaîne
 * MIME). Liée à FIELDS_httpRequete.ContentType via allowedBase pour
 * l'autocomplete LSP. La saisie d'une chaîne libre reste valide
 * (cf cahier mime — `req..ContentType = "text/csv"` accepté). */
static const char* const E_typeMime_values[] = {
    /* Formulaires HTTP */
    "typeMimeFormulaireMultiPartie", "typeMimeFormulaireSimple",
    /* Médias */
    "typeMimeGIF", "typeMimeJPEG", "typeMimePNG", "typeMimeSVG",
    /* Documents */
    "typeMimePDF", "typeMimeHTML", "typeMimeTexte", "typeMimeTexteRiche",
    "typeMimeTexteXML", "typeMimeJSON", "typeMimeSOAP", "typeMimeTSV",
    /* Office */
    "typeMimeXLS", "typeMimeXLSX", "typeMimePPTX", "typeMimeZIP",
    /* OpenDocument */
    "typeMimeODP", "typeMimeODS", "typeMimeODT",
    /* Google Apps */
    "typeMimeGglAppsScript", "typeMimeGglDocs", "typeMimeGglDrawings",
    "typeMimeGglSheets",     "typeMimeGglSlides",
    NULL
};
const OwlEnum E_typeMime = {
    .name="typeMime", .nameAscii="typeMime", .nameEN="mimeType", .category="HTTP",
    .descFR="Types MIME courants pour httpRequête..ContentType. "
            "Liste indicative — toute chaîne MIME valide reste acceptée.",
    .descEN="Common MIME types for httpRequest..ContentType. "
            "Hint list — any valid MIME string is also accepted.",
    .values=E_typeMime_values, .combinable=false
};

const OwlEnum E_httpIgnoreErreur = {
    .name="httpIgnoreErreur", .nameAscii="httpIgnoreErreur", .nameEN="httpIgnoreError",
    .category="HTTP",
    .descFR="Erreurs HTTP/TLS à ignorer (bitflags combinables).",
    .descEN="HTTP/TLS errors to ignore (combinable bitflags).",
    .values=E_httpIgnoreErreur_values, .combinable=true
};

/* Codes de statut HTTP + schemas d'authentification d'un service HTTP
 * declaratif (blocs ServiceREST / OperationHTTP). Cables comme props DSL
 * (ui_catalog.c : PROPS_OPERATION_HTTP.CodeEtat / PROPS_SERVICE_REST.
 * Authentification) pour l'autocomplete LSP et la validation compile-time.
 *
 * Codes de statut : les valeurs entieres SONT les codes reels du protocole
 * (200, 404...), porteurs de sens runtime : force_small_int=true (opt-out de
 * la zone sentinel, cf self-test regle 20). */
static const char* const E_httpCodeEtat_values[] = {
    "httpOK", "httpCr\xc3\xa9\xc3\xa9",               /* httpCréé */
    "httpAccept\xc3\xa9",                              /* httpAccepté */
    "httpSansContenu",
    "httpD\xc3\xa9plac\xc3\xa9""D\xc3\xa9""finitivement", /* httpDéplacéDéfinitivement */
    "httpTrouv\xc3\xa9",                               /* httpTrouvé */
    "httpNonModifi\xc3\xa9",                           /* httpNonModifié */
    "httpRedirectionPermanente",
    "httpRequ\xc3\xaateIncorrecte",                    /* httpRequêteIncorrecte */
    "httpNonAutoris\xc3\xa9",                          /* httpNonAutorisé */
    "httpInterdit",
    "httpNonTrouv\xc3\xa9",                            /* httpNonTrouvé */
    "httpM\xc3\xa9thodeNonAutoris\xc3\xa9""e",         /* httpMéthodeNonAutorisée */
    "httpConflit",
    "httpContenuTropGrand",
    "httpTypeM\xc3\xa9""diaNonSupport\xc3\xa9",         /* httpTypeMédiaNonSupporté */
    "httpTropDeRequ\xc3\xaates",                       /* httpTropDeRequêtes */
    "httpErreurServeur",
    "httpNonImpl\xc3\xa9ment\xc3\xa9",                 /* httpNonImplémenté */
    "httpServiceIndisponible",
    NULL
};
const OwlEnum E_httpCodeEtat = {
    .name="httpCode\xc3\x89tat", .nameAscii="httpCodeEtat", .nameEN="httpStatusCode",
    .category="HTTP",
    .descFR="Codes de statut HTTP (200 OK, 404 Non trouvé...).",
    .descEN="HTTP status codes (200 OK, 404 Not Found...).",
    .values=E_httpCodeEtat_values, .combinable=false, .force_small_int=true
};

/* Schémas d'authentification d'un service HTTP. Sentinel-zone. */
static const char* const E_httpAuthentification_values[] = {
    "httpAuthAucune", "httpAuthBearer", "httpAuthBasique",
    "httpAuthCl\xc3\xa9""API",                         /* httpAuthCléAPI */
    "httpAuthJWT",
    NULL
};
const OwlEnum E_httpAuthentification = {
    .name="httpAuthentification", .nameAscii="httpAuthentification",
    .nameEN="httpAuthentication", .category="HTTP",
    .descFR="Schémas d'authentification d'un service HTTP.",
    .descEN="Authentication schemes for an HTTP service.",
    .values=E_httpAuthentification_values, .combinable=false
};

/* Mode de framing d'un flux de service HTTP declaratif (OperationHTTP..Flux,
 * ). Sentinel-zone. */
static const char* const E_httpFlux_values[] = {
    "fluxSSE", "fluxOctets",
    NULL
};
const OwlEnum E_httpFlux = {
    .name="httpFlux", .nameAscii="httpFlux", .nameEN="httpStream", .category="HTTP",
    .descFR="Mode de diffusion progressive d'une opération de service (framing SSE ou octets bruts).",
    .descEN="Progressive delivery mode of a service operation (SSE framing or raw bytes).",
    .values=E_httpFlux_values, .combinable=false
};

static const OwlEnum* const HTTP_ENUMS[] = {
    &E_httpMethodes, &E_httpParametres, &E_httpResultats,
    &E_protocoleVersions, &E_httpIgnoreErreur, &E_typeMime,
    &E_httpCodeEtat, &E_httpAuthentification, &E_httpFlux
};

/* ---- HTTP constants (were in stdlib_builtin_types.c CONSTS_INT[]).
 * Moved here so they live next to their enums — single source. */
/* en = nameEN (alias EN canon runtime). NULL si la const passe par une
 * propriété en canon (familles httpParam / httpResult) ou sans forme EN connue. */
#define C_HTTP(nm, val, dfr, den, en) {nm, "HTTP", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef HTTP_CONSTS[] = {
    /* Méthodes HTTP — enum string-valued (httpGet = "GET"…), enregistrees
     * runtime dans stdlib_http_init comme les typeMime* : le champ
     * httpRequête..Méthode et MoiMême..Requête..Méthode portent la chaine
     * de la methode, donc `..Méthode = httpDelete` doit comparer "DELETE"
     * a "DELETE". Voir HTTP_METHODS[] plus bas. */
    /* Paramètres HTTPParamètre — sentinelles centralisees. Forme EN canon
     * posee en nameEN (alias runtime auto, invariant bilingue n°3). */
    C_HTTP("httpParamReinitialise",     OWL_HTTP_PARAM_REINITIALISE,    "réinitialise les paramètres", "reset parameters", "httpParamReset"),
    C_HTTP("httpParamProxy",            OWL_HTTP_PARAM_PROXY,           "adresse du proxy", "proxy address", "httpParamProxy"),
    C_HTTP("httpParamNoProxy",          OWL_HTTP_PARAM_NO_PROXY,        "exclusions proxy", "proxy exclusions", "httpParamNoProxy"),
    C_HTTP("httpParamTimeoutConnexion", OWL_HTTP_PARAM_TIMEOUT,         "timeout connexion", "connect timeout", "httpParamConnectionTimeout"),
    C_HTTP("httpParamUserAgent",        OWL_HTTP_PARAM_USER_AGENT,      "User-Agent", "User-Agent", "httpParamUserAgent"),
    C_HTTP("httpParamRedirection",      OWL_HTTP_PARAM_REDIRECTION,     "suivre les redirects", "follow redirects", "httpParamRedirect"),
    C_HTTP("httpParamMaxRedirection",   OWL_HTTP_PARAM_MAX_REDIRECTION, "nb max de redirects", "max redirects", "httpParamMaxRedirect"),
    C_HTTP("httpParamCompression",      OWL_HTTP_PARAM_COMPRESSION,     "accepter compression", "accept compression", "httpParamCompression"),
    C_HTTP("httpParamVerifCertificat",  OWL_HTTP_PARAM_VERIF_CERTIFICAT,"vérifier cert TLS", "verify TLS cert", "httpParamVerifyCertificate"),
    C_HTTP("httpParamGardeConnexion",   OWL_HTTP_PARAM_GARDE_CONNEXION, "keep-alive", "keep-alive", "httpParamKeepConnection"),
    /* Résultats HTTPDonneRésultat — sentinelles centralisees. Forme EN canon. */
    C_HTTP("httpResultContenu",             OWL_HTTP_RESULT_CONTENU,         "corps", "body", "httpResultContent"),
    C_HTTP("httpResultCodeEtat",            OWL_HTTP_RESULT_CODE_ETAT,       "code HTTP", "HTTP code", "httpResultStatusCode"),
    C_HTTP("httpResultEntete",              OWL_HTTP_RESULT_ENTETE,          "entêtes", "headers", "httpResultHeader"),
    C_HTTP("httpResultContentType",         OWL_HTTP_RESULT_CONTENT_TYPE,    "Content-Type", "Content-Type", "httpResultContentType"),
    C_HTTP("httpResultEnteteBrut",          OWL_HTTP_RESULT_ENTETE_BRUT,     "entêtes bruts", "raw headers", "httpResultRawHeader"),
    C_HTTP("httpResultURL",                 OWL_HTTP_RESULT_URL,             "URL finale", "final URL", "httpResultURL"),
    C_HTTP("httpResultDescriptionCodeEtat", OWL_HTTP_RESULT_DESC_CODE_ETAT,  "texte du code", "status text", "httpResultStatusDescription"),
    /* Versions TLS (bitflags combinables). Noms ASCII — vm.globals stores
     * accent-stripped keys; the E_protocoleVersions enum's values[]
     * array above carries the accented form for LSP display. */
    C_HTTP("ProtocoleDefaut",    0, "valeur par défaut (laisse mbedtls décider)", "default (mbedtls defaults)", "ProtocolDefault"),
    C_HTTP("ProtocoleTLS1",      1, "TLS 1.0", "TLS 1.0", "ProtocolTLS1"),
    C_HTTP("ProtocoleTLS1_1",    2, "TLS 1.1", "TLS 1.1", "ProtocolTLS1_1"),
    C_HTTP("ProtocoleTLS1_2",    4, "TLS 1.2", "TLS 1.2", "ProtocolTLS1_2"),
    C_HTTP("ProtocoleTLS1_3",    8, "TLS 1.3", "TLS 1.3", "ProtocolTLS1_3"),
    /* Erreurs HTTP/TLS ignorables (bitflags combinables). ASCII keys.
     * 7 valeurs ; alias accentués générés auto par register_consts.
     * Ordre des bits OWL stable (non spécifié par la doc) — préservé pour
     * compat avec les checks runtime (cf fn_http_envoie). */
    C_HTTP("httpIgnoreCertificatInvalide",      1,   "ignore cert invalide", "skip invalid cert", "httpIgnoreInvalidCertificate"),
    C_HTTP("httpIgnoreNomCertificatInvalide",   2,   "ignore nom CN invalide", "skip invalid CN", "httpIgnoreInvalidCertificateName"),
    C_HTTP("httpIgnoreCertificatExpire",        4,   "ignore cert expiré", "skip expired cert", "httpIgnoreExpiredCertificate"),
    C_HTTP("httpIgnoreRedirection",             8,   "ignore redirections", "ignore redirects", "httpIgnoreRedirection"),
    C_HTTP("httpIgnoreRenvoiHTTP",              16,  "bloque downgrade HTTPS→HTTP", "block HTTPS→HTTP downgrade", "httpIgnoreRedirectToHTTP"),
    C_HTTP("httpIgnoreRenvoiHTTPS",             32,  "bloque upgrade HTTP→HTTPS", "block HTTP→HTTPS upgrade", "httpIgnoreRedirectToHTTPS"),
    C_HTTP("httpIgnoreRevocation",              64,  "ignore révocation cert", "skip cert revocation check", "httpIgnoreRevocation"),
    /* Codes de statut HTTP — valeurs entieres = codes reels du protocole.
     * Alias ASCII (httpCree, httpNonTrouve...) generes auto par register_consts. */
    C_HTTP("httpOK",                          200, "200 OK", "200 OK", "httpOk"),
    C_HTTP("httpCr\xc3\xa9\xc3\xa9",          201, "201 Créé", "201 Created", "httpCreated"),
    C_HTTP("httpAccept\xc3\xa9",              202, "202 Accepté", "202 Accepted", "httpAccepted"),
    C_HTTP("httpSansContenu",                 204, "204 Sans contenu", "204 No Content", "httpNoContent"),
    C_HTTP("httpD\xc3\xa9plac\xc3\xa9""D\xc3\xa9""finitivement", 301, "301 Déplacé définitivement", "301 Moved Permanently", "httpMovedPermanently"),
    C_HTTP("httpTrouv\xc3\xa9",               302, "302 Trouvé", "302 Found", "httpFound"),
    C_HTTP("httpNonModifi\xc3\xa9",           304, "304 Non modifié", "304 Not Modified", "httpNotModified"),
    C_HTTP("httpRedirectionPermanente",       308, "308 Redirection permanente", "308 Permanent Redirect", "httpPermanentRedirect"),
    C_HTTP("httpRequ\xc3\xaateIncorrecte",    400, "400 Requête incorrecte", "400 Bad Request", "httpBadRequest"),
    C_HTTP("httpNonAutoris\xc3\xa9",          401, "401 Non autorisé", "401 Unauthorized", "httpUnauthorized"),
    C_HTTP("httpInterdit",                    403, "403 Interdit", "403 Forbidden", "httpForbidden"),
    C_HTTP("httpNonTrouv\xc3\xa9",            404, "404 Non trouvé", "404 Not Found", "httpNotFound"),
    C_HTTP("httpM\xc3\xa9thodeNonAutoris\xc3\xa9""e", 405, "405 Méthode non autorisée", "405 Method Not Allowed", "httpMethodNotAllowed"),
    C_HTTP("httpConflit",                     409, "409 Conflit", "409 Conflict", "httpConflict"),
    C_HTTP("httpContenuTropGrand",            413, "413 Contenu trop grand", "413 Content Too Large", "httpContentTooLarge"),
    C_HTTP("httpTypeM\xc3\xa9""diaNonSupport\xc3\xa9", 415, "415 Type de média non supporté", "415 Unsupported Media Type", "httpUnsupportedMediaType"),
    C_HTTP("httpTropDeRequ\xc3\xaates",       429, "429 Trop de requêtes", "429 Too Many Requests", "httpTooManyRequests"),
    C_HTTP("httpErreurServeur",               500, "500 Erreur serveur", "500 Internal Server Error", "httpServerError"),
    C_HTTP("httpNonImpl\xc3\xa9ment\xc3\xa9", 501, "501 Non implémenté", "501 Not Implemented", "httpNotImplemented"),
    C_HTTP("httpServiceIndisponible",         503, "503 Service indisponible", "503 Service Unavailable", "httpServiceUnavailable"),
    /* Schémas d'authentification d'un service HTTP — sentinel-zone. */
    C_HTTP("httpAuthAucune",         OWL_HTTP_AUTH_AUCUNE,        "aucune authentification", "no authentication", "httpAuthNone"),
    C_HTTP("httpAuthBearer",         OWL_HTTP_AUTH_BEARER,        "jeton porteur (Bearer)", "bearer token", "httpAuthBearer"),
    C_HTTP("httpAuthBasique",        OWL_HTTP_AUTH_BASIQUE,       "authentification basique (Basic)", "basic authentication", "httpAuthBasic"),
    C_HTTP("httpAuthCl\xc3\xa9""API", OWL_HTTP_AUTH_CLE_API,      "clé d'API", "API key", "httpAuthApiKey"),
    C_HTTP("httpAuthJWT",            OWL_HTTP_AUTH_JWT,           "jeton JWT validé de façon déclarative (signature RS256, expiration, émetteur)", "declaratively validated JWT token (RS256 signature, expiry, issuer)", "httpAuthJWT"),
    /* Mode de framing d'un flux de service HTTP — sentinel-zone. */
    C_HTTP("fluxSSE",                OWL_HTTP_FLUX_SSE,           "diffusion en flux d'événements (framing SSE « data: »)", "server-sent events stream (SSE « data: » framing)", "streamSSE"),
    C_HTTP("fluxOctets",             OWL_HTTP_FLUX_OCTETS,        "diffusion en flux d'octets bruts (chunked)", "raw bytes stream (chunked)", "streamBytes"),
};
#undef C_HTTP

/* ----------------------------------------------------------------
 * Constantes de type MIME canon (typeMime*).
 *
 * Ces constantes sont des STRINGS, donc ne peuvent pas tenir dans le
 * tableau HTTP_CONSTS au-dessus (qui utilise INTEGER_VAL en static
 * initializer). On les déclare ici en spec, puis on les enregistre
 * runtime dans stdlib_http_init via owl_register_global_string +
 * owl_registry_register_consts (pour LSP autocomplete + doc).
 *
 * Convention canon : la propriété httpRequête..ContentType accepte
 * une chaîne libre. Ces constantes sont un raccourci ergonomique +
 * autocomplete-friendly — toute saisie de chaîne reste valide.
 * ---------------------------------------------------------------- */
typedef struct {
    const char* name;
    const char* value;       /* MIME type string */
    const char* descFR;
    const char* descEN;
    const char* nameEN;      /* alias EN canon (NULL si pas de forme EN connue) */
} MimeSpec;

static const MimeSpec MIME_TYPES[] = {
    /* Formulaires HTTP */
    {"typeMimeFormulaireMultiPartie", "multipart/form-data",
     "type MIME : multipart/form-data (upload de fichiers)",
     "MIME: multipart/form-data (file upload)", "mimeTypeMultipartForm"},
    {"typeMimeFormulaireSimple",      "application/x-www-form-urlencoded",
     "type MIME : formulaire URL-encodé (POST simple)",
     "MIME: URL-encoded form (simple POST)", "mimeTypeForm"},
    /* Contenus standards */
    {"typeMimeGIF",       "image/gif",
     "type MIME : image GIF (*.gif)", "MIME: GIF image (*.gif)", "mimeTypeGIF"},
    {"typeMimeHTML",      "text/html",
     "type MIME : page HTML (*.htm, *.html)", "MIME: HTML page (*.htm, *.html)", "mimeTypeHTML"},
    {"typeMimeJPEG",      "image/jpeg",
     "type MIME : image JPEG (*.jpg, *.jpeg)", "MIME: JPEG image", "mimeTypeJPEG"},
    {"typeMimeJSON",      "application/json",
     "type MIME : contenu JSON", "MIME: JSON content", "mimeTypeJSON"},
    {"typeMimePDF",       "application/pdf",
     "type MIME : document PDF (*.pdf)", "MIME: PDF document", "mimeTypePDF"},
    {"typeMimePNG",       "image/png",
     "type MIME : image PNG (*.png)", "MIME: PNG image", "mimeTypePNG"},
    {"typeMimeSOAP",      "application/soap+xml",
     "type MIME : enveloppe SOAP XML", "MIME: SOAP XML envelope", "mimeTypeSOAP"},
    {"typeMimeTexte",     "text/plain",
     "type MIME : texte brut (*.txt)", "MIME: plain text", "mimeTypeText"},
    {"typeMimeTexteXML",  "text/xml",
     "type MIME : contenu texte XML", "MIME: XML text", "mimeTypeXMLText"},
    {"typeMimeXLS",       "application/vnd.ms-excel",
     "type MIME : Excel binaire (*.xls)", "MIME: legacy Excel (*.xls)", "mimeTypeXLS"},
    {"typeMimeXLSX",      "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
     "type MIME : Excel moderne (*.xlsx)", "MIME: modern Excel (*.xlsx)", "mimeTypeXLSX"},
    {"typeMimeZIP",       "application/zip",
     "type MIME : archive ZIP (*.zip)", "MIME: ZIP archive", "mimeTypeZIP"},
    /* Nouveautés récentes */
    {"typeMimeODP",       "application/vnd.oasis.opendocument.presentation",
     "type MIME : présentation OpenDocument (*.odp)", "MIME: OpenDocument presentation", "mimeTypeODP"},
    {"typeMimeODS",       "application/vnd.oasis.opendocument.spreadsheet",
     "type MIME : tableur OpenDocument (*.ods)", "MIME: OpenDocument spreadsheet", "mimeTypeODS"},
    {"typeMimeODT",       "application/vnd.oasis.opendocument.text",
     "type MIME : document OpenDocument (*.odt)", "MIME: OpenDocument text", "mimeTypeODT"},
    {"typeMimePPTX",      "application/vnd.openxmlformats-officedocument.presentationml.presentation",
     "type MIME : présentation PowerPoint (*.pptx)", "MIME: PowerPoint presentation", "mimeTypePPTX"},
    {"typeMimeSVG",       "image/svg+xml",
     "type MIME : Scalable Vector Graphics (*.svg)", "MIME: SVG", "mimeTypeSVG"},
    {"typeMimeTexteRiche","application/rtf",
     "type MIME : texte enrichi (*.rtf)", "MIME: Rich Text Format", "mimeTypeRichText"},
    {"typeMimeTSV",       "text/tab-separated-values",
     "type MIME : Tab-Separated Values (*.tsv)", "MIME: TSV", "mimeTypeTSV"},
    /* Google Apps */
    {"typeMimeGglAppsScript", "application/vnd.google-apps.script",
     "type MIME : Google Apps Script", "MIME: Google Apps Script", "mimeTypeGglAppsScript"},
    {"typeMimeGglDocs",       "application/vnd.google-apps.document",
     "type MIME : Google Docs", "MIME: Google Docs", "mimeTypeGglDocs"},
    {"typeMimeGglDrawings",   "application/vnd.google-apps.drawing",
     "type MIME : Google Drawings", "MIME: Google Drawings", "mimeTypeGglDrawings"},
    {"typeMimeGglSheets",     "application/vnd.google-apps.spreadsheet",
     "type MIME : Google Sheets", "MIME: Google Sheets", "mimeTypeGglSheets"},
    {"typeMimeGglSlides",     "application/vnd.google-apps.presentation",
     "type MIME : Google Slides", "MIME: Google Slides", "mimeTypeGglSlides"},
    {NULL, NULL, NULL, NULL, NULL},
};

/* ----------------------------------------------------------------
 * Constantes de methode HTTP — enum httpMéthodes STRING-VALUED.
 *
 * Meme mecanique que MIME_TYPES : ces constantes sont des chaines
 * (httpGet = "GET"), donc ne peuvent pas tenir dans HTTP_CONSTS
 * (INTEGER_VAL en static initializer). Enregistrees runtime dans
 * stdlib_http_init via owl_register_global_string +
 * owl_registry_register_consts.
 *
 * La chaine = le verbe HTTP canon (majuscules). Ainsi
 * httpRequête..Méthode et MoiMême..Requête..Méthode, qui portent
 * la chaine de methode, sont comparables aux constantes httpGet/…
 * (elles-memes string-valued depuis ) ET aux litteraux
 * ("GET").
 * ---------------------------------------------------------------- */
static const MimeSpec HTTP_METHODS[] = {
    {"httpGet",    "GET",    "méthode GET",    "GET method",    "httpGet"},
    {"httpPost",   "POST",   "méthode POST",   "POST method",   "httpPost"},
    {"httpPut",    "PUT",    "méthode PUT",    "PUT method",    "httpPut"},
    {"httpDelete", "DELETE", "méthode DELETE", "DELETE method", "httpDelete"},
    {"httpHead",   "HEAD",   "méthode HEAD",   "HEAD method",   "httpHead"},
    {"httpPatch",  "PATCH",  "méthode PATCH",  "PATCH method",  "httpPatch"},
    {"httpCopy",   "COPY",   "méthode COPY",   "COPY method",   "httpCopy"},
    {NULL, NULL, NULL, NULL, NULL},
};


static const OwlParam p_url_opts[] = {
    {"url",     "chaîne",  "URL cible"},
    {"options", "chaîne|httpRequête", "options (optionnel)"},
    {0}
};
static const OwlParam p_req[]     = { {"req",  "httpRequête", "requête à envoyer"}, {0} };
static const OwlParam p_soap[]    = { {"url", "chaîne", "endpoint"}, {"enveloppe", "chaîne", "XML"}, {0} };
static const OwlParam p_hname[]   = { {"nom", "chaîne", "nom d'entête"}, {0} };
static const OwlParam p_param[]   = {
    {"clé",    "entier",  "paramètre à configurer", .allowedBase=&E_httpParametres},
    {"valeur", "chaîne|entier|booléen", "valeur"}, {0}
};
static const OwlParam p_xfer[]    = { {"url", "chaîne", "URL"}, {"chemin", "chaîne", "fichier local"}, {0} };
static const OwlParam p_rep_t[]   = {
    {"réponse", "httpRéponse", "réponse HTTP"},
    {"type",    "entier",      "composant à extraire", .allowedBase=&E_httpResultats},
    {0}
};
/* HTTPCookie : nom requis, valeur/domaine/path optionnels (lecture si seul le nom). */
static const OwlParam p_cookie[]  = {
    {"nom",     "chaîne", "nom du cookie"},
    {"valeur",  "chaîne", "valeur à écrire (optionnel ; lecture si absent)"},
    {"domaine", "chaîne", "domaine (optionnel)"},
    {"chemin",  "chaîne", "chemin (optionnel)"},
    {0}
};
/* HTTPCookieGère : forme positionnelle (requête/domaine optionnels, activation).
 * L'impl exploite le 1er argument comme activation ; les formes étendues
 * (domaine, requête) sont acceptées pour compatibilité. */
static const OwlParam p_cookie_gere[] = {
    {"actif",   "booléen|entier", "active/désactive la gestion (optionnel)"},
    {"arg2",    "chaîne|booléen", "domaine ou activation (optionnel)"},
    {"arg3",    "booléen",        "activation (optionnel)"},
    {0}
};

/* HTTPRequête : 1 à 7 args.
 * Convention moderne OWL : (url [, options JSON | méthode [, corps [, entêtes]]]).
 * Forme positionnelle étendue acceptée pour compat (url + agent, entête,
 * message, type, utilisateur, mot de passe). L'impl exploite positionnellement
 * les 4 premiers ; les arguments 5 à 7 sont acceptés mais non interprétés. */
static const OwlSignature sig_req_variadic[] = {
    {1, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {2, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {3, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {4, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {5, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {6, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {7, p_url_opts, "httpRéponse|chaîne", "réponse"},
    {0}
};
static const OwlSignature sig_envoie[]       = { {1,  p_req,      "httpRéponse", "réponse structurée"}, {0} };
static const OwlSignature sig_no_arg_int[]   = { {0,  NULL,       "entier", "statut/version"}, {0} };
static const OwlSignature sig_no_arg_str[]   = { {0,  NULL,       "chaîne", "info"}, {0} };
static const OwlSignature sig_no_arg_rien[]  = { {0,  NULL,       "rien", "action"}, {0} };
static const OwlSignature sig_hname[]        = { {1,  p_hname,    "chaîne", "valeur de l'entête"}, {0} };
/* HTTPEnvoieSOAP(<URL>, <Enveloppe> [, <SOAPAction> [, <Options>]]) — 2 à 4 args. */
static const OwlSignature sig_soap[]         = {
    {2, p_soap, "chaîne", "réponse SOAP"},
    {3, p_soap, "chaîne", "réponse SOAP"},
    {4, p_soap, "chaîne", "réponse SOAP"},
    {0}
};
static const OwlSignature sig_param[]        = { {2,  p_param,    "rien", "paramètre modifié"}, {0} };
static const OwlSignature sig_xfer[]         = { {2,  p_xfer,     "booléen", "succès"}, {0} };
static const OwlSignature sig_rep_t[]        = { {2,  p_rep_t,    "chaîne|entier", "composant demandé (selon hrtXxx : chaîne pour Contenu/Entête, entier pour CodeEtat...)"}, {0} };
/* HTTPCookie(<Nom> [, <Valeur> [, <Domaine> [, <Chemin>]]]) — 1 à 4 args.
 * 1 arg = lecture ; 2+ = écriture. */
static const OwlSignature sig_cookie[]       = {
    {1, p_cookie, "chaîne", "valeur du cookie (lecture)"},
    {2, p_cookie, "booléen", "succès de l'écriture"},
    {3, p_cookie, "booléen", "succès de l'écriture"},
    {4, p_cookie, "booléen", "succès de l'écriture"},
    {0}
};
/* HTTPCookieGère([[<Requête>,] [<Domaine>,] <Activation>]) — 0 à 3 args.
 * Sans argument : retourne l'état courant. Le 1er argument pilote l'activation
 * globale ; les arguments de domaine/requête sont acceptés pour compatibilité. */
static const OwlSignature sig_cookie_gere[]  = {
    {0, NULL,          "booléen", "état courant de la gestion automatique"},
    {1, p_cookie_gere, "booléen", "nouvel état de la gestion automatique"},
    {2, p_cookie_gere, "booléen", "nouvel état de la gestion automatique"},
    {3, p_cookie_gere, "booléen", "nouvel état de la gestion automatique"},
    {0}
};

#define H_DEF(fr, fr_ascii, en, df, de, sig, fn) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), .category = "HTTP", \
      .descFR = (df), .descEN = (de), .signatures = (sig), .impl = (fn) }

static const OwlNativeDef DEFS_HTTP[] = {
    H_DEF("HTTPRequ\xc3\xaa" "te", "HTTPRequete", "HTTPRequest",
        "Effectue une requête HTTP/HTTPS (GET/POST…) avec options.",
        "Perform an HTTP/HTTPS request (GET/POST…) with options.",
        sig_req_variadic, fn_http_requete),
    H_DEF("HTTPEnvoie", NULL, "HTTPSend",
        "Envoie une requête construite via httpRequête.",
        "Send a request built via httpRequest.",
        sig_envoie, fn_http_envoie),
    H_DEF("HTTPDernierStatut", NULL, "HTTPLastStatus",
        "Code de statut HTTP de la dernière requête (200, 404…).",
        "HTTP status code of the last request (200, 404…).",
        sig_no_arg_int, fn_http_dernier_statut),
    H_DEF("HTTPDernierEnt\xc3\xaa" "te", "HTTPDernierEntete", "HTTPLastHeader",
        "Valeur d'un entête de la dernière réponse.",
        "Value of a header from the last response.",
        sig_hname, fn_http_dernier_entete),
    H_DEF("HTTPVideCookies", NULL, "HTTPClearCookies",
        "Vide le cookie jar.", "Clear the cookie jar.",
        sig_no_arg_rien, fn_http_vide_cookies),
    H_DEF("HTTPFermeSession", NULL, "HTTPCloseSession",
        "Ferme la session HTTP keep-alive.", "Close the keep-alive HTTP session.",
        sig_no_arg_rien, fn_http_ferme_session),
    H_DEF("HTTPEnvoieSOAP", NULL, "HTTPSendSOAP",
        "Envoie une requête SOAP.", "Send a SOAP request.",
        sig_soap, fn_http_envoie_soap),
    H_DEF("HTTPParam\xc3\xa8" "tre", "HTTPParametre", "HTTPConfigure",
        "Paramètre HTTP global (proxy, user-agent…).",
        "Global HTTP parameter (proxy, user-agent…).",
        sig_param, fn_http_parametre),
    H_DEF("HTTPTransf\xc3\xa8" "reFichier", "HTTPTransfereFichier", "HTTPTransferFile",
        "Téléverse un fichier vers une URL.",
        "Upload a file to an URL.",
        sig_xfer, fn_http_transfere_fichier),

    /* Debug / introspection */
    H_DEF("HTTPInfo", NULL, "HTTPInfo",
        "Informations sur l'implémentation HTTP.", "Information about the HTTP implementation.",
        sig_no_arg_str, fn_http_info),
    H_DEF("HTTPVersion", NULL, "HTTPVersion",
        "Alias de HTTPInfo.", "Alias of HTTPInfo.",
        sig_no_arg_str, fn_http_info),
    H_DEF("HTTPDonneCha\xc3\xae" "neRequete", "HTTPDonneChaineRequete", "HTTPLastRequestString",
        "Retourne la dernière requête brute envoyée.", "Return the last raw request sent.",
        sig_no_arg_str, fn_http_donne_chaine_requete),
    H_DEF("HTTPDonneCha\xc3\xae" "neReponse", "HTTPDonneChaineReponse", "HTTPLastResponseString",
        "Retourne la dernière réponse brute reçue.", "Return the last raw response received.",
        sig_no_arg_str, fn_http_donne_chaine_reponse),
    H_DEF("HTTPCodeHTTP", NULL, "HTTPHTTPVersion",
        "Version HTTP négociée.", "Negotiated HTTP version.",
        sig_no_arg_int, fn_http_code_http),
    H_DEF("HTTPDonneR\xc3\xa9" "sultat", "HTTPDonneResultat", "HTTPGetResult",
        "Extrait un composant d'une réponse (Contenu, CodeEtat…).",
        "Extract a component of a response (Content, Status…).",
        sig_rep_t, fn_http_donne_resultat),

    /* Cookies */
    H_DEF("HTTPCookie", NULL, "HTTPCookie",
        "Manipule un cookie individuel (lecture/écriture).",
        "Manipulate a single cookie (read/write).",
        sig_cookie, fn_http_cookie),
    H_DEF("HTTPCookieG\xc3\xa8" "re", "HTTPCookieGere", "HTTPCookieManage",
        "Active/désactive la gestion automatique des cookies.",
        "Enable/disable automatic cookie management.",
        sig_cookie_gere, fn_http_cookie_gere),
};

#undef H_DEF

void stdlib_http_init(void) {
    owl_registry_register_module(DEFS_HTTP, (int)(sizeof(DEFS_HTTP)/sizeof(DEFS_HTTP[0])));
    owl_registry_register_enums (HTTP_ENUMS, (int)(sizeof(HTTP_ENUMS)/sizeof(HTTP_ENUMS[0])));
    /* register_consts auto-enregistre dans vm.globals. */
    owl_registry_register_consts(HTTP_CONSTS,(int)(sizeof(HTTP_CONSTS)/sizeof(HTTP_CONSTS[0])));

    /* Constantes typeMime* (strings, canon 2024). Double enregistrement :
     *   1) owl_register_global_string → accessibles depuis le code OWL via
     *      `req..ContentType = typeMimeJSON`
     *   2) owl_registry_register_consts → exposées au LSP autocomplete +
     *      doc + --vscode-data (génération grammaire TextMate)
     * Static buffer pour les OwlConstDef car Value(string) ne peut pas
     * vivre dans un static initializer. */
    {
        static OwlConstDef mimeConsts[40];
        int n = 0;
        for (int i = 0; MIME_TYPES[i].name != NULL && n < 39; i++) {
            int valLen = (int)strlen(MIME_TYPES[i].value);
            owl_register_global_string(MIME_TYPES[i].name, MIME_TYPES[i].value, valLen);
            /* Récupère le ObjString créé dans vm.globals pour partager
             * la même allocation côté registry. */
            ObjString* k = obj_copy_string(MIME_TYPES[i].name,
                                           (int)strlen(MIME_TYPES[i].name));
            Value stored;
            Value v = NIL_VAL;
            if (table_get(&vm.globals, k, &stored)) v = stored;
            mimeConsts[n].name     = MIME_TYPES[i].name;
            mimeConsts[n].category = "HTTP";
            mimeConsts[n].value    = v;
            mimeConsts[n].descFR   = MIME_TYPES[i].descFR;
            mimeConsts[n].descEN   = MIME_TYPES[i].descEN;
            mimeConsts[n].nameEN   = MIME_TYPES[i].nameEN;
            n++;
        }
        owl_registry_register_consts(mimeConsts, n);
    }

    /* Constantes httpMéthodes (strings, meme mecanique que typeMime*). */
    {
        static OwlConstDef methodConsts[8];
        int n = 0;
        for (int i = 0; HTTP_METHODS[i].name != NULL && n < 7; i++) {
            int valLen = (int)strlen(HTTP_METHODS[i].value);
            owl_register_global_string(HTTP_METHODS[i].name, HTTP_METHODS[i].value, valLen);
            ObjString* k = obj_copy_string(HTTP_METHODS[i].name,
                                           (int)strlen(HTTP_METHODS[i].name));
            Value stored;
            Value v = NIL_VAL;
            if (table_get(&vm.globals, k, &stored)) v = stored;
            methodConsts[n].name     = HTTP_METHODS[i].name;
            methodConsts[n].category = "HTTP";
            methodConsts[n].value    = v;
            methodConsts[n].descFR   = HTTP_METHODS[i].descFR;
            methodConsts[n].descEN   = HTTP_METHODS[i].descEN;
            methodConsts[n].nameEN   = HTTP_METHODS[i].nameEN;
            n++;
        }
        owl_registry_register_consts(methodConsts, n);
    }
}
