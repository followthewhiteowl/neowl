// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_tcp_h
#define owl_net_tcp_h

/* ================================================================
 * TCP SOCKET (cross-platform Win32 / POSIX)
 *
 * Couche minimale au-dessus de l'API socket système. API blocante,
 * synchrone, simple. Pas d'async ni de select — on n'en a pas besoin
 * pour le client SQL (un appel = une requête = un round-trip).
 *
 * Cross-platform via #ifdef _WIN32 :
 *   - Windows : WinSock2 (ws2_32 déjà link dans CMakeLists)
 *   - POSIX   : sys/socket.h + netinet/in.h
 *
 * Pas de thread-safety au niveau structure : un OwlTcpSocket = une
 * connexion utilisée par un seul thread à la fois. Plusieurs sockets
 * peuvent vivre en parallèle dans des threads différents.
 * ================================================================ */

#include "common.h"
#include <stddef.h>

#ifdef _WIN32
  #include <winsock2.h>
  typedef SOCKET owl_socket_fd_t;
  #define OWL_INVALID_SOCKET INVALID_SOCKET
#else
  typedef int owl_socket_fd_t;
  #define OWL_INVALID_SOCKET (-1)
#endif

typedef struct {
    owl_socket_fd_t fd;
    bool            connected;
    /* Buffer interne pour le dernier message d'erreur (errno→string,
     * WSAGetLastError→string). Non thread-safe — c'est l'état de
     * l'instance. */
    char            last_error[256];
} OwlTcpSocket;

/* ---------- Initialisation globale ---------- */

/* Appel global une seule fois au démarrage (WSAStartup sous Win32).
 * Idempotent. Retourne true si OK, false sur erreur fatale. */
bool owl_tcp_global_init(void);

/* Appel global au shutdown (WSACleanup). Idempotent. */
void owl_tcp_global_shutdown(void);

/* ---------- Instance ---------- */

/* Initialise une socket vide. À appeler avant tout autre op. */
void owl_tcp_init(OwlTcpSocket* s);

/* Connect blocant à host:port. host peut être un hostname (DNS) ou une
 * IP textuelle. Retourne true sur succès. Sur échec, last_error est
 * rempli avec un message lisible.
 *
 * timeout_ms : timeout total pour resolution DNS + connect TCP. 0 ou
 * négatif = pas de timeout (défaut OS). */
bool owl_tcp_connect(OwlTcpSocket* s, const char* host, int port, int timeout_ms);

/* Envoie len octets. Bloque jusqu'à ce que tout soit envoyé ou erreur.
 * Retourne le nombre d'octets envoyés (= len si OK), -1 sur erreur. */
int  owl_tcp_send(OwlTcpSocket* s, const void* buf, size_t len);

/* Reçoit jusqu'à len octets. Bloque jusqu'à au moins 1 octet ou
 * fermeture/timeout. Retourne :
 *   > 0 : octets effectivement reçus (peut être < len)
 *   = 0 : peer a fermé proprement (FIN reçu)
 *   < 0 : erreur (last_error rempli) */
int  owl_tcp_recv(OwlTcpSocket* s, void* buf, size_t len);

/* Reçoit EXACTEMENT len octets (boucle interne sur recv).
 * Retourne true sur succès complet, false sinon (timeout, fermeture
 * pendant lecture, erreur). En cas d'échec partiel, les octets déjà
 * reçus sont écrits dans buf jusqu'à la coupure. */
bool owl_tcp_recv_exact(OwlTcpSocket* s, void* buf, size_t len);

/* Ferme la connexion proprement (shutdown puis close). Idempotent. */
void owl_tcp_close(OwlTcpSocket* s);

/* Dernier message d'erreur sur cette socket. Pointe vers le buffer
 * interne — valable jusqu'à la prochaine op qui peut le réécrire. */
const char* owl_tcp_last_error(const OwlTcpSocket* s);

#endif
