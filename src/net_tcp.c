// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "net_tcp.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <ws2tcpip.h>      /* getaddrinfo */
  /* ws2_32 lie via CMakeLists target_link_libraries — pas besoin de
   * #pragma comment (MSVC-only, casse GCC -Werror=unknown-pragmas). */
  #define OWL_CLOSESOCKET closesocket
  #define OWL_SOCK_ERRNO  WSAGetLastError()
  #define OWL_INPROGRESS  WSAEWOULDBLOCK
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>         /* getaddrinfo */
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #define OWL_CLOSESOCKET close
  #define OWL_SOCK_ERRNO  errno
  #define OWL_INPROGRESS  EINPROGRESS
#endif

/* Bascule un fd en non-blocking. */
static bool set_nonblocking(owl_socket_fd_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/* Restaure un fd en blocking. */
static bool set_blocking(owl_socket_fd_t fd) {
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

/* ================================================================
 * Global init (winsock startup only — POSIX is a no-op).
 * ================================================================ */

static int g_tcp_init_count = 0;

bool owl_tcp_global_init(void) {
    if (g_tcp_init_count++ > 0) return true;
#ifdef _WIN32
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        fprintf(stderr, "[owl-tcp] WSAStartup failed: %d\n", rc);
        g_tcp_init_count = 0;
        return false;
    }
#endif
    return true;
}

void owl_tcp_global_shutdown(void) {
    if (--g_tcp_init_count > 0) return;
    if (g_tcp_init_count < 0) g_tcp_init_count = 0;
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ================================================================
 * Helpers
 * ================================================================ */

static void set_error_from_errno(OwlTcpSocket* s, const char* prefix) {
    int err = OWL_SOCK_ERRNO;
#ifdef _WIN32
    /* WinSock : utiliser FormatMessage. Pour simplifier on imprime juste
     * le code numérique — suffisant pour debug, on traduira plus tard si
     * nécessaire. */
    snprintf(s->last_error, sizeof(s->last_error),
             "%s: WSA error %d", prefix, err);
#else
    snprintf(s->last_error, sizeof(s->last_error),
             "%s: %s (errno %d)", prefix, strerror(err), err);
#endif
}

static void set_error_fmt(OwlTcpSocket* s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * Instance API
 * ================================================================ */

void owl_tcp_init(OwlTcpSocket* s) {
    s->fd = OWL_INVALID_SOCKET;
    s->connected = false;
    s->last_error[0] = '\0';
}

bool owl_tcp_connect(OwlTcpSocket* s, const char* host, int port, int timeout_ms) {
    if (host == NULL || port <= 0 || port > 65535) {
        set_error_fmt(s, "connect: host ou port invalide (port=%d)", port);
        return false;
    }

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;        /* IPv4 ou IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host, portStr, &hints, &res);
    if (rc != 0 || res == NULL) {
#ifdef _WIN32
        set_error_fmt(s, "getaddrinfo(%s:%d) failed: WSA %d", host, port, rc);
#else
        set_error_fmt(s, "getaddrinfo(%s:%d) failed: %s", host, port, gai_strerror(rc));
#endif
        if (res) freeaddrinfo(res);
        return false;
    }

    /* Essaie chaque résultat (typiquement IPv4 + IPv6) jusqu'à ce qu'un
     * connect réussisse.
     *
     * Stratégie pour honorer le timeout_ms (anciennement ignoré) :
     *   1. socket() en mode non-blocking
     *   2. connect() retourne 0 (succès immédiat — loopback ou cache)
     *      ou -1/EINPROGRESS (négociation TCP en cours)
     *   3. select(writable, timeout_ms) attend la complétion
     *   4. getsockopt(SO_ERROR) lève le verdict final (RST vs connecté)
     *   5. Restore blocking pour les recv/send ultérieurs
     *
     * timeout_ms <= 0 → comportement legacy (connect blocking, timeout OS). */
    owl_socket_fd_t fd = OWL_INVALID_SOCKET;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == OWL_INVALID_SOCKET) continue;

        bool ok = false;
        if (timeout_ms <= 0) {
            if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) ok = true;
        } else {
            if (!set_nonblocking(fd)) {
                OWL_CLOSESOCKET(fd);
                fd = OWL_INVALID_SOCKET;
                continue;
            }
            int crc = connect(fd, p->ai_addr, (int)p->ai_addrlen);
            if (crc == 0) {
                ok = true;  /* succès immédiat */
            } else {
                int err = OWL_SOCK_ERRNO;
                if (err == OWL_INPROGRESS
#ifndef _WIN32
                    || err == EAGAIN
#endif
                    ) {
                    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
                    /* On surveille aussi les erreurs (exception sur Win32). */
                    fd_set efds; FD_ZERO(&efds); FD_SET(fd, &efds);
                    struct timeval tv = {
                        timeout_ms / 1000,
                        (timeout_ms % 1000) * 1000
                    };
                    int sel = select((int)fd + 1, NULL, &wfds, &efds, &tv);
                    if (sel > 0) {
                        /* Vérifie SO_ERROR pour distinguer succès vrai
                         * d'un RST silencieux qui a juste « débloqué » select. */
                        int soerr = 0;
                        socklen_t slen = sizeof(soerr);
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                       (char*)&soerr, &slen) == 0 &&
                            soerr == 0 && FD_ISSET(fd, &wfds)) {
                            ok = true;
                        }
                    }
                    /* sel == 0 → timeout, sel < 0 → erreur — ok=false */
                }
            }
            if (ok) {
                /* Restore blocking pour les I/O suivantes. */
                set_blocking(fd);
            }
        }

        if (ok) break;
        OWL_CLOSESOCKET(fd);
        fd = OWL_INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (fd == OWL_INVALID_SOCKET) {
        if (timeout_ms > 0) {
            set_error_fmt(s, "connect: %s:%d injoignable (timeout %dms)",
                          host, port, timeout_ms);
        } else {
            set_error_from_errno(s, "connect");
        }
        return false;
    }

    s->fd = fd;
    s->connected = true;
    s->last_error[0] = '\0';
    return true;
}

int owl_tcp_send(OwlTcpSocket* s, const void* buf, size_t len) {
    if (!s->connected) {
        set_error_fmt(s, "send: socket non connectee");
        return -1;
    }
    const char* p = (const char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = send(s->fd, p + total, (int)(len - total), 0);
        if (n <= 0) {
            set_error_from_errno(s, "send");
            return -1;
        }
        total += (size_t)n;
    }
    return (int)total;
}

int owl_tcp_recv(OwlTcpSocket* s, void* buf, size_t len) {
    if (!s->connected) {
        set_error_fmt(s, "recv: socket non connectee");
        return -1;
    }
    int n = recv(s->fd, (char*)buf, (int)len, 0);
    if (n < 0) {
        set_error_from_errno(s, "recv");
        return -1;
    }
    return n;  /* 0 = peer closed, > 0 = bytes received */
}

bool owl_tcp_recv_exact(OwlTcpSocket* s, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = owl_tcp_recv(s, p + total, len - total);
        if (n <= 0) {
            /* 0 = fermeture proprement, < 0 = erreur. Dans les deux cas
             * on n'a pas la longueur demandée. */
            if (n == 0) {
                set_error_fmt(s, "recv_exact: peer ferme (%zu/%zu octets recus)",
                              total, len);
            }
            return false;
        }
        total += (size_t)n;
    }
    return true;
}

void owl_tcp_close(OwlTcpSocket* s) {
    if (s->fd != OWL_INVALID_SOCKET) {
#ifdef _WIN32
        shutdown(s->fd, SD_BOTH);
#else
        shutdown(s->fd, SHUT_RDWR);
#endif
        OWL_CLOSESOCKET(s->fd);
        s->fd = OWL_INVALID_SOCKET;
    }
    s->connected = false;
}

const char* owl_tcp_last_error(const OwlTcpSocket* s) {
    return s->last_error;
}
