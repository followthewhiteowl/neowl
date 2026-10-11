// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mysql_mock_server.h"

#include "sql_mysql_wire.h"
#include "auth_mysql_native.h"
#include "auth_mysql_caching.h"
#include "net_le_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE mock_thread_t;
  typedef SOCKET mock_socket_t;
  #define MOCK_CLOSESOCKET closesocket
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef pthread_t mock_thread_t;
  typedef int mock_socket_t;
  #define MOCK_CLOSESOCKET close
#endif

/* Salt fixe utilise par le mock : 1..20. Le test peut donc pre-calculer
 * le token attendu et l'echo dans le mock pour comparaison. */
static const uint8_t MOCK_SALT[20] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20
};

struct MysqlMockServer {
    MysqlMockAuthMode auth_mode;
    MysqlMockPlugin   plugin;
    char           expected_password[128];
    int            port;
    int            ready;
    mock_thread_t  thread;
    char           last_user [128];
    char           last_db   [128];
    char           last_query[1024];
    bool           auth_token_matched;
};

/* ================================================================
 * Helpers transport bas niveau (socket OS direct)
 * ================================================================ */

static bool sock_send_all(mock_socket_t s, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = send(s, p + total, (int)(len - total), 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

static bool sock_recv_exact(mock_socket_t s, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = recv(s, p + total, (int)(len - total), 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

/* Recv un paquet MySQL complet (header + payload). Retourne true si OK. */
static bool mock_recv_pkt(mock_socket_t s, uint8_t** out_payload,
                            uint32_t* out_plen, uint8_t* out_seq) {
    uint8_t hdr[4];
    if (!sock_recv_exact(s, hdr, 4)) return false;
    *out_plen = ((uint32_t)hdr[0])
              | ((uint32_t)hdr[1] << 8)
              | ((uint32_t)hdr[2] << 16);
    *out_seq = hdr[3];
    if (*out_plen == 0) {
        *out_payload = NULL;
        return true;
    }
    *out_payload = (uint8_t*)malloc(*out_plen);
    if (!*out_payload) return false;
    if (!sock_recv_exact(s, *out_payload, *out_plen)) {
        free(*out_payload);
        *out_payload = NULL;
        return false;
    }
    return true;
}

/* Envoie le contenu d'un OwlLeBuffer puis le free. */
static bool send_buffer_free(mock_socket_t s, OwlLeBuffer* buf) {
    bool ok = sock_send_all(s, buf->data, buf->length);
    owl_le_buffer_free(buf);
    return ok;
}

/* ================================================================
 * Builders des reponses canon
 * ================================================================ */

static void mock_send_initial_handshake(mock_socket_t s, MysqlMockPlugin plugin) {
    OwlLeBuffer out; owl_le_buffer_init(&out, 128);
    size_t off = mysql_pkt_begin(&out, 0);  /* seq = 0 */

    owl_le_buffer_write_u8     (&out, 10);              /* protocol_version */
    owl_le_buffer_write_cstring(&out, "8.0.35-mock");   /* server_version */
    owl_le_buffer_write_u32    (&out, 42);              /* thread_id */
    owl_le_buffer_write_bytes  (&out, MOCK_SALT, 8);    /* salt part 1 (8 oct) */
    owl_le_buffer_write_u8     (&out, 0);               /* filler */

    /* Capabilities : on annonce ce qu'on supporte. */
    uint32_t caps = MYSQL_CAP_PROTOCOL_41 | MYSQL_CAP_SECURE_CONNECTION
                  | MYSQL_CAP_PLUGIN_AUTH | MYSQL_CAP_TRANSACTIONS
                  | MYSQL_CAP_CONNECT_WITH_DB | MYSQL_CAP_LONG_FLAG
                  | MYSQL_CAP_LONG_PASSWORD;
    owl_le_buffer_write_u16(&out, (uint16_t)( caps        & 0xFFFF));
    owl_le_buffer_write_u8 (&out, 0x2D);    /* charset utf8mb4 */
    owl_le_buffer_write_u16(&out, 0x0002);  /* status flags */
    owl_le_buffer_write_u16(&out, (uint16_t)((caps >> 16) & 0xFFFF));
    owl_le_buffer_write_u8 (&out, 21);      /* auth_data_len = 21 (20 utiles + NUL) */
    for (int i = 0; i < 10; i++) owl_le_buffer_write_u8(&out, 0);  /* reserved */
    owl_le_buffer_write_bytes  (&out, MOCK_SALT + 8, 12);  /* salt part 2 (12 utiles) */
    owl_le_buffer_write_u8     (&out, 0);                   /* salt NUL terminator */

    const char* plugin_name = (plugin == MYSQL_MOCK_PLUGIN_CACHING)
                               ? "caching_sha2_password"
                               : "mysql_native_password";
    owl_le_buffer_write_cstring(&out, plugin_name);

    mysql_pkt_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_ok(mock_socket_t s, uint8_t seq) {
    OwlLeBuffer out; owl_le_buffer_init(&out, 16);
    size_t off = mysql_pkt_begin(&out, seq);
    owl_le_buffer_write_u8         (&out, MYSQL_PKT_OK);
    owl_le_buffer_write_lenenc_int (&out, 0);  /* affected_rows */
    owl_le_buffer_write_lenenc_int (&out, 0);  /* last_insert_id */
    owl_le_buffer_write_u16        (&out, 0x0002);  /* status (autocommit on) */
    owl_le_buffer_write_u16        (&out, 0);  /* warnings */
    mysql_pkt_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_error(mock_socket_t s, uint8_t seq,
                              uint16_t code, const char* sql_state, const char* msg) {
    OwlLeBuffer out; owl_le_buffer_init(&out, 128);
    size_t off = mysql_pkt_begin(&out, seq);
    owl_le_buffer_write_u8 (&out, MYSQL_PKT_ERROR);
    owl_le_buffer_write_u16(&out, code);
    owl_le_buffer_write_u8 (&out, '#');
    owl_le_buffer_write_bytes(&out, sql_state, 5);
    owl_le_buffer_write_bytes(&out, msg, strlen(msg));
    mysql_pkt_end(&out, off);
    send_buffer_free(s, &out);
}

/* Caching_sha2 fast auth success : AuthMoreData 0x01 + 0x03. */
static void mock_send_caching_fast_ok(mock_socket_t s, uint8_t seq) {
    OwlLeBuffer out; owl_le_buffer_init(&out, 16);
    size_t off = mysql_pkt_begin(&out, seq);
    owl_le_buffer_write_u8(&out, MYSQL_PKT_AUTH_MORE);  /* 0x01 */
    owl_le_buffer_write_u8(&out, 0x03);                  /* fast auth success */
    mysql_pkt_end(&out, off);
    send_buffer_free(s, &out);
}

/* ResultSet : 1 col "n" INT4, 1 row "42", EOF/OK_eof.
 * Sequence post-Query :
 *   pkt seq=1 : column count = 1 (lenenc)
 *   pkt seq=2 : ColumnDefinition41 pour "n"
 *   pkt seq=3 : EOF (FE 00 00 status flags)
 *   pkt seq=4 : DataRow ("42")
 *   pkt seq=5 : EOF
 */
static void mock_send_resultset_42(mock_socket_t s) {
    /* seq 1 : column count */
    { OwlLeBuffer b; owl_le_buffer_init(&b, 16);
      size_t off = mysql_pkt_begin(&b, 1);
      owl_le_buffer_write_lenenc_int(&b, 1);
      mysql_pkt_end(&b, off);
      send_buffer_free(s, &b); }

    /* seq 2 : ColumnDefinition41 :
     *   lenenc "def"
     *   lenenc schema  ""
     *   lenenc table   ""
     *   lenenc org_table ""
     *   lenenc name     "n"
     *   lenenc org_name "n"
     *   lenenc 0x0c                                    (length of fixed-length fields = 12)
     *   u16 charset, u32 column_length, u8 type, u16 flags, u8 decimals, u16 filler
     */
    { OwlLeBuffer b; owl_le_buffer_init(&b, 64);
      size_t off = mysql_pkt_begin(&b, 2);
      owl_le_buffer_write_lenenc_str(&b, "def", 3);
      owl_le_buffer_write_lenenc_str(&b, "", 0);
      owl_le_buffer_write_lenenc_str(&b, "", 0);
      owl_le_buffer_write_lenenc_str(&b, "", 0);
      owl_le_buffer_write_lenenc_str(&b, "n", 1);
      owl_le_buffer_write_lenenc_str(&b, "n", 1);
      owl_le_buffer_write_lenenc_int(&b, 0x0c);
      owl_le_buffer_write_u16(&b, 0x003F);   /* charset */
      owl_le_buffer_write_u32(&b, 11);       /* column_length */
      owl_le_buffer_write_u8 (&b, 0x03);     /* type = MYSQL_TYPE_LONG (INT4) */
      owl_le_buffer_write_u16(&b, 0x0081);   /* flags : NOT_NULL | PRI_KEY */
      owl_le_buffer_write_u8 (&b, 0);        /* decimals */
      owl_le_buffer_write_u16(&b, 0);        /* filler */
      mysql_pkt_end(&b, off);
      send_buffer_free(s, &b); }

    /* seq 3 : EOF (legacy : 0xFE + warnings u16 + status u16) */
    { OwlLeBuffer b; owl_le_buffer_init(&b, 16);
      size_t off = mysql_pkt_begin(&b, 3);
      owl_le_buffer_write_u8 (&b, MYSQL_PKT_EOF_OR_AUTH_SWITCH);
      owl_le_buffer_write_u16(&b, 0);        /* warnings */
      owl_le_buffer_write_u16(&b, 0x0002);   /* status */
      mysql_pkt_end(&b, off);
      send_buffer_free(s, &b); }

    /* seq 4 : DataRow "42" text mode (lenenc str) */
    { OwlLeBuffer b; owl_le_buffer_init(&b, 16);
      size_t off = mysql_pkt_begin(&b, 4);
      owl_le_buffer_write_lenenc_str(&b, "42", 2);
      mysql_pkt_end(&b, off);
      send_buffer_free(s, &b); }

    /* seq 5 : EOF terminal */
    { OwlLeBuffer b; owl_le_buffer_init(&b, 16);
      size_t off = mysql_pkt_begin(&b, 5);
      owl_le_buffer_write_u8 (&b, MYSQL_PKT_EOF_OR_AUTH_SWITCH);
      owl_le_buffer_write_u16(&b, 0);
      owl_le_buffer_write_u16(&b, 0x0002);
      mysql_pkt_end(&b, off);
      send_buffer_free(s, &b); }
}

/* ================================================================
 * Validation auth token cote mock
 * ================================================================ */

static bool validate_auth_token(MysqlMockServer* m,
                                  const uint8_t* token, size_t token_len) {
    if (m->expected_password[0] == '\0') {
        /* Pas de password attendu : on accepte si token vide. */
        return token_len == 0;
    }
    if (m->plugin == MYSQL_MOCK_PLUGIN_NATIVE) {
        if (token_len != MYSQL_NATIVE_TOKEN_LEN) return false;
        uint8_t expected[MYSQL_NATIVE_TOKEN_LEN];
        owl_mysql_native_compute_token(m->expected_password,
                                        strlen(m->expected_password),
                                        MOCK_SALT, expected);
        return memcmp(token, expected, MYSQL_NATIVE_TOKEN_LEN) == 0;
    }
    if (m->plugin == MYSQL_MOCK_PLUGIN_CACHING) {
        if (token_len != MYSQL_CACHING_TOKEN_LEN) return false;
        uint8_t expected[MYSQL_CACHING_TOKEN_LEN];
        owl_mysql_caching_compute_token(m->expected_password,
                                         strlen(m->expected_password),
                                         MOCK_SALT, expected);
        return memcmp(token, expected, MYSQL_CACHING_TOKEN_LEN) == 0;
    }
    return false;
}

/* Parse HandshakeResponse41 et valide l'auth. Retourne true si le token
 * matche le password attendu. Remplit aussi last_user / last_db. */
static bool parse_and_validate_response(MysqlMockServer* m,
                                         const uint8_t* payload, uint32_t plen) {
    OwlLeBuffer in;
    owl_le_buffer_attach_read(&in, payload, plen);

    uint32_t client_caps;
    if (!owl_le_buffer_read_u32(&in, &client_caps)) return false;
    uint32_t max_pkt;
    if (!owl_le_buffer_read_u32(&in, &max_pkt))     return false;
    uint8_t charset;
    if (!owl_le_buffer_read_u8(&in, &charset))      return false;
    if (!owl_le_buffer_read_skip(&in, 23))          return false;  /* reserved */

    const char* user = owl_le_buffer_read_cstring(&in);
    if (!user) return false;
    snprintf(m->last_user, sizeof(m->last_user), "%s", user);

    /* auth_response length depend des capabilities. On supporte les 2 cas
     * courants (SECURE_CONNECTION ou PLUGIN_AUTH_LENENC_DATA). */
    size_t token_len = 0;
    const uint8_t* token = NULL;
    if (client_caps & MYSQL_CAP_PLUGIN_AUTH_LENENC_DATA) {
        uint64_t v;
        if (!owl_le_buffer_read_lenenc_int(&in, &v)) return false;
        token_len = (size_t)v;
        token = in.data + in.pos;
        if (!owl_le_buffer_read_skip(&in, token_len)) return false;
    } else if (client_caps & MYSQL_CAP_SECURE_CONNECTION) {
        uint8_t L;
        if (!owl_le_buffer_read_u8(&in, &L)) return false;
        token_len = L;
        token = in.data + in.pos;
        if (!owl_le_buffer_read_skip(&in, token_len)) return false;
    } else {
        /* legacy null-terminated, non supporte par le mock */
        return false;
    }

    if (client_caps & MYSQL_CAP_CONNECT_WITH_DB) {
        const char* db = owl_le_buffer_read_cstring(&in);
        if (db) snprintf(m->last_db, sizeof(m->last_db), "%s", db);
    }
    /* plugin_name suit, on l'ignore. */

    m->auth_token_matched = validate_auth_token(m, token, token_len);
    return m->auth_token_matched;
}

/* ================================================================
 * Main loop : serve one client
 * ================================================================ */

static void mock_serve_client(MysqlMockServer* m, mock_socket_t cli) {
    /* 1. Send Initial Handshake. */
    mock_send_initial_handshake(cli, m->plugin);

    /* 2. Recv HandshakeResponse. */
    uint8_t* payload = NULL; uint32_t plen = 0; uint8_t seq = 0;
    if (!mock_recv_pkt(cli, &payload, &plen, &seq)) return;
    bool auth_ok = parse_and_validate_response(m, payload, plen);
    free(payload);

    if (m->auth_mode == MYSQL_MOCK_AUTH_FAIL || !auth_ok) {
        mock_send_error(cli, (uint8_t)(seq + 1), 1045, "28000",
                         "Access denied for user");
        return;
    }

    /* 3. Si caching_sha2 : on envoie le fast auth success d'abord. */
    if (m->plugin == MYSQL_MOCK_PLUGIN_CACHING) {
        mock_send_caching_fast_ok(cli, (uint8_t)(seq + 1));
        mock_send_ok            (cli, (uint8_t)(seq + 2));
    } else {
        mock_send_ok(cli, (uint8_t)(seq + 1));
    }

    /* 4. Boucle commandes (COM_QUERY / COM_QUIT). */
    while (1) {
        if (!mock_recv_pkt(cli, &payload, &plen, &seq)) return;
        if (plen == 0) { free(payload); return; }
        uint8_t cmd = payload[0];
        if (cmd == MYSQL_COM_QUIT) {
            free(payload);
            return;
        }
        if (cmd == MYSQL_COM_QUERY) {
            /* Capture la query (plen-1 octets apres le cmd byte). */
            size_t qlen = plen - 1;
            if (qlen >= sizeof(m->last_query)) qlen = sizeof(m->last_query) - 1;
            memcpy(m->last_query, payload + 1, qlen);
            m->last_query[qlen] = '\0';
            free(payload);
            mock_send_resultset_42(cli);
            continue;
        }
        /* Cmd non supportee : Error generique. */
        free(payload);
        mock_send_error(cli, 1, 1064, "42000", "command not implemented in mock");
        return;
    }
}

/* ================================================================
 * Thread entry + API publique
 * ================================================================ */

#ifdef _WIN32
static DWORD WINAPI mock_thread_fn(LPVOID arg) {
#else
static void* mock_thread_fn(void* arg) {
#endif
    MysqlMockServer* m = (MysqlMockServer*)arg;
    mock_socket_t srv;
#ifdef _WIN32
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return 0;
#else
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
#endif
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(0);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0) goto cleanup;

    socklen_t alen = sizeof(addr);
    if (getsockname(srv, (struct sockaddr*)&addr, &alen) != 0) goto cleanup;
    m->port = ntohs(addr.sin_port);

    if (listen(srv, 1) != 0) goto cleanup;
    m->ready = 1;

    mock_socket_t cli;
#ifdef _WIN32
    cli = accept(srv, NULL, NULL);
    MOCK_CLOSESOCKET(srv);
    if (cli == INVALID_SOCKET) return 0;
#else
    cli = accept(srv, NULL, NULL);
    MOCK_CLOSESOCKET(srv);
    if (cli < 0) return NULL;
#endif

    mock_serve_client(m, cli);

#ifdef _WIN32
    shutdown(cli, SD_BOTH);
    MOCK_CLOSESOCKET(cli);
    return 0;
#else
    shutdown(cli, SHUT_RDWR);
    MOCK_CLOSESOCKET(cli);
    return NULL;
#endif

cleanup:
    MOCK_CLOSESOCKET(srv);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

MysqlMockServer* mysql_mock_server_start(MysqlMockAuthMode mode,
                                          MysqlMockPlugin   plugin,
                                          const char*       expected_password) {
    MysqlMockServer* m = (MysqlMockServer*)calloc(1, sizeof(MysqlMockServer));
    if (!m) return NULL;
    m->auth_mode = mode;
    m->plugin    = plugin;
    if (expected_password) {
        snprintf(m->expected_password, sizeof(m->expected_password),
                 "%s", expected_password);
    }
#ifdef _WIN32
    m->thread = CreateThread(NULL, 0, mock_thread_fn, m, 0, NULL);
    if (m->thread == NULL) { free(m); return NULL; }
#else
    if (pthread_create(&m->thread, NULL, mock_thread_fn, m) != 0) {
        free(m); return NULL;
    }
#endif
    int waited = 0;
    while (!m->ready && waited < 2000) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
        waited += 10;
    }
    return m;
}

int  mysql_mock_server_port    (const MysqlMockServer* m) { return m->port; }
int  mysql_mock_server_is_ready(const MysqlMockServer* m) { return m->ready; }

void mysql_mock_server_join(MysqlMockServer* m) {
    if (!m) return;
#ifdef _WIN32
    WaitForSingleObject(m->thread, INFINITE);
    CloseHandle(m->thread);
#else
    pthread_join(m->thread, NULL);
#endif
}

void mysql_mock_server_free(MysqlMockServer* m) {
    if (!m) return;
    free(m);
}

const char* mysql_mock_server_last_user (const MysqlMockServer* m) { return m->last_user;  }
const char* mysql_mock_server_last_db   (const MysqlMockServer* m) { return m->last_db;    }
const char* mysql_mock_server_last_query(const MysqlMockServer* m) { return m->last_query; }
bool        mysql_mock_server_auth_token_matched(const MysqlMockServer* m) {
    return m->auth_token_matched;
}
