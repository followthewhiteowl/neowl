// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_postgres_mock_server.h"

#include "sql_postgres_wire.h"
#include "net_be_buffer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

struct PostgresMockServer {
    PostgresMockAuthMode auth_mode;
    char           expected_password[64];
    int            port;
    int            ready;
    mock_thread_t  thread;
    char           last_query[1024];
    char           last_password[256];
    char           last_parse_sql[1024];
    char           last_bind_param0[1024];
};

/* ================================================================
 * Helpers : send buffer / recv exact n bytes sur socket OS direct.
 * On n'utilise PAS notre couche net_tcp ici pour rester independant
 * du client cote infra (le mock teste le client, pas l'inverse).
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

/* Envoie le contenu d'un OwlBeBuffer (write-mode) puis le free. */
static bool send_buffer_free(mock_socket_t s, OwlBeBuffer* buf) {
    bool ok = sock_send_all(s, buf->data, buf->length);
    owl_be_buffer_free(buf);
    return ok;
}

/* ================================================================
 * Script du serveur mock
 * ================================================================ */

static void mock_send_auth_ok(mock_socket_t s) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 16);
    size_t off = postgres_msg_begin(&out, 'R');
    owl_be_buffer_write_u32(&out, POSTGRES_AUTH_OK);  /* sub-code 0 */
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_auth_cleartext(mock_socket_t s) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 16);
    size_t off = postgres_msg_begin(&out, 'R');
    owl_be_buffer_write_u32(&out, POSTGRES_AUTH_CLEARTEXT_PWD);
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_error(mock_socket_t s, const char* msg) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 64);
    size_t off = postgres_msg_begin(&out, 'E');
    /* Format ErrorResponse : [code:1 byte][value:cstring]...[0] */
    owl_be_buffer_write_u8(&out, 'S');
    owl_be_buffer_write_cstring(&out, "ERROR");
    owl_be_buffer_write_u8(&out, 'M');
    owl_be_buffer_write_cstring(&out, msg ? msg : "mock error");
    owl_be_buffer_write_u8(&out, 0);  /* terminator */
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_parameter_status(mock_socket_t s, const char* key, const char* val) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 64);
    size_t off = postgres_msg_begin(&out, 'S');
    owl_be_buffer_write_cstring(&out, key);
    owl_be_buffer_write_cstring(&out, val);
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_backend_key(mock_socket_t s, int32_t pid, int32_t key) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 16);
    size_t off = postgres_msg_begin(&out, 'K');
    owl_be_buffer_write_i32(&out, pid);
    owl_be_buffer_write_i32(&out, key);
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_ready_for_query(mock_socket_t s) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 8);
    size_t off = postgres_msg_begin(&out, 'Z');
    owl_be_buffer_write_u8(&out, 'I');  /* status 'I' = idle, hors transaction */
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

/* RowDescription pour 1 colonne "n" int4. */
static void mock_send_row_description_int4(mock_socket_t s) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 64);
    size_t off = postgres_msg_begin(&out, 'T');
    owl_be_buffer_write_u16(&out, 1);  /* nFields = 1 */
    owl_be_buffer_write_cstring(&out, "n");
    owl_be_buffer_write_i32(&out, 0);          /* tableOid */
    owl_be_buffer_write_i16(&out, 0);          /* columnAttrNum */
    owl_be_buffer_write_i32(&out, 23);         /* dataTypeOid = INT4 */
    owl_be_buffer_write_i16(&out, 4);          /* typeLen = 4 */
    owl_be_buffer_write_i32(&out, -1);         /* typeModifier */
    owl_be_buffer_write_i16(&out, 0);          /* formatCode = 0 (text) */
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_data_row_int(mock_socket_t s, int value) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 32);
    size_t off = postgres_msg_begin(&out, 'D');
    owl_be_buffer_write_u16(&out, 1);  /* nValues = 1 */
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", value);
    owl_be_buffer_write_i32(&out, n);  /* length du value bytes */
    owl_be_buffer_write_bytes(&out, buf, (size_t)n);
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

static void mock_send_command_complete(mock_socket_t s, const char* tag) {
    OwlBeBuffer out; owl_be_buffer_init(&out, 32);
    size_t off = postgres_msg_begin(&out, 'C');
    owl_be_buffer_write_cstring(&out, tag);
    postgres_msg_end(&out, off);
    send_buffer_free(s, &out);
}

/* Lit le StartupMessage (pas de type byte, juste length+content). */
static bool mock_recv_startup(mock_socket_t s) {
    uint8_t lenBuf[4];
    if (!sock_recv_exact(s, lenBuf, 4)) return false;
    uint32_t total = ((uint32_t)lenBuf[0] << 24) | ((uint32_t)lenBuf[1] << 16)
                   | ((uint32_t)lenBuf[2] <<  8) |  (uint32_t)lenBuf[3];
    if (total < 8) return false;
    uint32_t payload_len = total - 4;
    uint8_t* payload = (uint8_t*)malloc(payload_len);
    if (!payload) return false;
    bool ok = sock_recv_exact(s, payload, payload_len);
    free(payload);
    return ok;
}

/* Lit un message PG standard (type + length + body). out_payload est
 * alloue, caller doit free. */
static bool mock_recv_msg(mock_socket_t s, char* out_type,
                           uint8_t** out_payload, uint32_t* out_payload_len) {
    uint8_t hdr[5];
    if (!sock_recv_exact(s, hdr, 5)) return false;
    char type = (char)hdr[0];
    uint32_t total = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16)
                   | ((uint32_t)hdr[3] <<  8) |  (uint32_t)hdr[4];
    if (total < 4) return false;
    uint32_t plen = total - 4;
    uint8_t* p = NULL;
    if (plen > 0) {
        p = (uint8_t*)malloc(plen);
        if (!p) return false;
        if (!sock_recv_exact(s, p, plen)) { free(p); return false; }
    }
    *out_type = type;
    *out_payload = p;
    *out_payload_len = plen;
    return true;
}

/* ================================================================
 * Thread principal du mock
 * ================================================================ */

static void mock_serve_client(PostgresMockServer* m, mock_socket_t cli) {
    /* 1. StartupMessage attendu. */
    if (!mock_recv_startup(cli)) return;

    /* 2. Auth selon mode. */
    switch (m->auth_mode) {
        case POSTGRES_MOCK_AUTH_FAIL:
            mock_send_error(cli, "mock: auth failed");
            return;

        case POSTGRES_MOCK_AUTH_CLEARTEXT: {
            mock_send_auth_cleartext(cli);
            char type;
            uint8_t* payload = NULL; uint32_t plen = 0;
            if (!mock_recv_msg(cli, &type, &payload, &plen)) return;
            if (type != 'p') { free(payload); return; }
            /* payload = cstring password */
            size_t cap = sizeof(m->last_password) - 1;
            size_t cp = plen > 0 ? (plen - 1) : 0;  /* sans \0 final */
            if (cp > cap) cp = cap;
            memcpy(m->last_password, payload, cp);
            m->last_password[cp] = '\0';
            free(payload);
            if (strcmp(m->last_password, m->expected_password) != 0) {
                mock_send_error(cli, "mock: bad password");
                return;
            }
            /* Auth OK. */
            mock_send_auth_ok(cli);
            break;
        }

        case POSTGRES_MOCK_AUTH_OK:
        default:
            mock_send_auth_ok(cli);
            break;
    }

    /* 3. ParameterStatus + BackendKeyData + ReadyForQuery. */
    mock_send_parameter_status(cli, "server_version", "16.0 (mock)");
    mock_send_parameter_status(cli, "client_encoding", "UTF8");
    mock_send_backend_key(cli, 12345, 67890);
    mock_send_ready_for_query(cli);

    /* 4. Boucle query/terminate. */
    while (1) {
        char type;
        uint8_t* payload = NULL; uint32_t plen = 0;
        if (!mock_recv_msg(cli, &type, &payload, &plen)) return;

        if (type == 'Q') {
            /* Query : payload = cstring SQL. */
            if (plen > 0) {
                size_t cap = sizeof(m->last_query) - 1;
                size_t cp = plen - 1;  /* sans \0 final */
                if (cp > cap) cp = cap;
                memcpy(m->last_query, payload, cp);
                m->last_query[cp] = '\0';
            }
            free(payload);
            /* Script canon : 1 ligne avec valeur "42". */
            mock_send_row_description_int4(cli);
            mock_send_data_row_int(cli, 42);
            mock_send_command_complete(cli, "SELECT 1");
            mock_send_ready_for_query(cli);
            continue;
        }

        if (type == 'X') {
            /* Terminate : client veut fermer. */
            free(payload);
            return;
        }

        /* Extended query : Parse 'P' | Bind 'B' | Describe 'D' | Execute 'E'
         * | Sync 'S'. On capture le SQL du Parse et le 1er param du Bind,
         * puis on repond uniquement sur Sync avec le script canon
         * (ParseComplete + BindComplete + meme RowDescription/DataRow). */
        if (type == 'P') {
            OwlBeBuffer in; owl_be_buffer_attach_read(&in, payload, plen);
            const char* stmt = owl_be_buffer_read_cstring(&in); (void)stmt;
            const char* sql  = owl_be_buffer_read_cstring(&in);
            if (sql) {
                snprintf(m->last_parse_sql, sizeof(m->last_parse_sql), "%s", sql);
            }
            free(payload);
            continue;
        }
        if (type == 'B') {
            /* Bind : portal_cstring stmt_cstring i16 nFmt [fmts] i16 nParams
             * [length:i32 + bytes] x N + i16 nResFmt [resFmts] */
            OwlBeBuffer in; owl_be_buffer_attach_read(&in, payload, plen);
            const char* portal = owl_be_buffer_read_cstring(&in); (void)portal;
            const char* stmt   = owl_be_buffer_read_cstring(&in); (void)stmt;
            uint16_t nFmt;     owl_be_buffer_read_u16(&in, &nFmt);
            for (uint16_t i = 0; i < nFmt; i++) {
                int16_t f; owl_be_buffer_read_i16(&in, &f);
            }
            uint16_t nParams;  owl_be_buffer_read_u16(&in, &nParams);
            if (nParams > 0) {
                int32_t len;
                if (owl_be_buffer_read_i32(&in, &len) && len > 0 &&
                    in.pos + (size_t)len <= in.length) {
                    size_t cp = (size_t)len;
                    if (cp >= sizeof(m->last_bind_param0)) cp = sizeof(m->last_bind_param0) - 1;
                    memcpy(m->last_bind_param0, payload + in.pos, cp);
                    m->last_bind_param0[cp] = '\0';
                }
            }
            free(payload);
            continue;
        }
        if (type == 'D' || type == 'E') {
            /* Describe / Execute : ignores, on attend Sync. */
            free(payload);
            continue;
        }
        if (type == 'S') {
            /* Sync : envoie la sequence canon. */
            free(payload);
            /* ParseComplete '1' + BindComplete '2'. */
            { OwlBeBuffer out; owl_be_buffer_init(&out, 8);
              size_t off = postgres_msg_begin(&out, '1');
              postgres_msg_end(&out, off);
              send_buffer_free(cli, &out); }
            { OwlBeBuffer out; owl_be_buffer_init(&out, 8);
              size_t off = postgres_msg_begin(&out, '2');
              postgres_msg_end(&out, off);
              send_buffer_free(cli, &out); }
            mock_send_row_description_int4(cli);
            mock_send_data_row_int(cli, 42);
            mock_send_command_complete(cli, "SELECT 1");
            mock_send_ready_for_query(cli);
            continue;
        }

        /* Type non geres : on log et on quitte. */
        free(payload);
        return;
    }
}

#ifdef _WIN32
static DWORD WINAPI mock_thread_fn(LPVOID arg) {
#else
static void* mock_thread_fn(void* arg) {
#endif
    PostgresMockServer* m = (PostgresMockServer*)arg;
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

/* ================================================================
 * API publique
 * ================================================================ */

PostgresMockServer* postgres_mock_server_start(PostgresMockAuthMode mode,
                                    const char* expected_password) {
    PostgresMockServer* m = (PostgresMockServer*)calloc(1, sizeof(PostgresMockServer));
    if (!m) return NULL;
    m->auth_mode = mode;
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
    /* Attend la disponibilité du listen (max 2s). */
    int waited = 0;
    while (!m->ready && waited < 2000) {
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);
#endif
        waited += 10;
    }
    return m;
}

int  postgres_mock_server_port(const PostgresMockServer* m) { return m->port; }
int  postgres_mock_server_is_ready(const PostgresMockServer* m) { return m->ready; }

void postgres_mock_server_join(PostgresMockServer* m) {
#ifdef _WIN32
    WaitForSingleObject(m->thread, INFINITE);
    CloseHandle(m->thread);
#else
    pthread_join(m->thread, NULL);
#endif
}

void postgres_mock_server_free(PostgresMockServer* m) {
    free(m);
}

const char* postgres_mock_server_last_query(const PostgresMockServer* m) {
    return m->last_query;
}

const char* postgres_mock_server_last_password(const PostgresMockServer* m) {
    return m->last_password;
}

const char* postgres_mock_server_last_parse_sql(const PostgresMockServer* m) {
    return m->last_parse_sql;
}

const char* postgres_mock_server_last_bind_param0(const PostgresMockServer* m) {
    return m->last_bind_param0;
}
