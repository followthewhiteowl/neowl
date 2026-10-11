// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_tcp_tls_h
#define owl_net_tcp_tls_h

/* ================================================================
 * TLS SOCKET (mbedTLS over OwlTcpSocket)
 *
 * Wrap mbedTLS au-dessus de notre OwlTcpSocket. Permet :
 *   - Connect direct en TLS (HTTPS pattern : TLS dès la connexion)
 *   - Negociate STARTTLS (PG/IMAP/SMTP pattern : connexion TCP en
 *     clair puis upgrade vers TLS apres un message protocol-specific)
 *
 * Utilise les trust roots Mozilla embarques (owl_ca_bundle, partage
 * avec stdlib_http.c) pour la verification serveur par defaut.
 *
 * Pas de thread-safety au niveau instance (un OwlTlsSocket = une
 * connexion utilisee par un seul thread). La couche d'init globale
 * (RNG + CA bundle) est thread-safe via lazy init protege par flag.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

/* Niveau de verification TLS — granularite alignee Postgres sslmode. */
typedef enum {
    OWL_TLS_VERIFY_NONE  = 0,  /* aucune verif : encrypt only (sslmode=require) */
    OWL_TLS_VERIFY_CHAIN = 1,  /* chaine CA verifiee, hostname ignore (sslmode=verify-ca) */
    OWL_TLS_VERIFY_FULL  = 2,  /* chaine + hostname (sslmode=verify-full, defaut) */
} OwlTlsVerifyLevel;

typedef struct {
    OwlTcpSocket          tcp;             /* TCP underlying transport */
    mbedtls_ssl_context   ssl;             /* TLS session state */
    mbedtls_ssl_config    conf;            /* TLS config (RNG, CA chain, etc.) */
    bool                  ssl_active;      /* true apres handshake reussi */
    int                   verify_level;    /* OwlTlsVerifyLevel */
    mbedtls_x509_crt      extra_ca;        /* CA custom (cnx..CertificatCA) */
    bool                  extra_ca_loaded;
    char                  last_error[256]; /* errno+mbedtls combined */
    /* Negotiated parameters (read-only apres handshake). */
    char                  cipher_chosen[64];
    char                  version_chosen[16];  /* "TLSv1.2", "TLSv1.3" */
} OwlTlsSocket;

/* ---------- Initialisation globale ---------- */

/* Lazy init du RNG + CA bundle (Mozilla). Idempotent. Retourne true
 * sur succes. Appele automatiquement par tls_connect, exposé ici
 * pour les tests. */
bool owl_tls_global_init(void);

/* Acces aux objets globaux mbedTLS (DRBG seed + CA chain Mozilla).
 * Utilise par d'autres modules TLS (sql_mssql_conn) qui veulent reutiliser
 * la meme entropie + chaine CA sans dupliquer 200 KB de certs.
 * Le pointeur est valide tant que owl_tls_global_init a ete appele.
 * Renvoie NULL si non initialise (caller doit gerer). */
void* owl_tls_get_drbg(void);
void* owl_tls_get_cacert(void);

/* ---------- Instance ---------- */

void owl_tls_init(OwlTlsSocket* s);

/* Connect TCP puis handshake TLS dans la foulee (HTTPS pattern).
 *   host          : hostname (SNI + cert verification selon verify_level)
 *   port          : port TCP
 *   verify_level  : OwlTlsVerifyLevel (NONE / CHAIN / FULL)
 *   timeout_ms    : pour la connexion TCP. mbedTLS handshake non timeout-e
 *                   pour V1 (a faire via mbedtls_ssl_set_timer_cb plus tard).
 *   extra_ca_path : NULL ou "" -> utilise le bundle Mozilla embedded.
 *                   Sinon : chemin vers un PEM contenant une CA additionnelle
 *                   utilisee comme racine de confiance UNIQUE (pas mixe avec
 *                   Mozilla, le caller est explicite). */
bool owl_tls_connect(OwlTlsSocket* s, const char* host, int port,
                      int verify_level, int timeout_ms,
                      const char* extra_ca_path);

/* STARTTLS pattern (PostgreSQL, IMAP, SMTP) : promeut une connexion
 * TCP existante (deja connectee en clair, premier message
 * protocol-specific deja echange) en TLS. La socket TCP doit
 * deja contenir un fd connecte.
 *   extra_ca_path : meme semantique que owl_tls_connect. */
bool owl_tls_start_tls(OwlTlsSocket* s, const char* host, int verify_level,
                        const char* extra_ca_path);

/* Send/recv au-dessus de TLS. Meme semantique que owl_tcp_send/recv :
 *   send : retourne bytes envoyes (= len si OK), -1 sinon
 *   recv : retourne bytes recus (> 0), 0 si peer ferme, -1 si erreur
 *   recv_exact : true si exactement len octets recus */
int  owl_tls_send(OwlTlsSocket* s, const void* buf, size_t len);
int  owl_tls_recv(OwlTlsSocket* s, void* buf, size_t len);
bool owl_tls_recv_exact(OwlTlsSocket* s, void* buf, size_t len);

/* Ferme proprement : close_notify TLS puis shutdown TCP. */
void owl_tls_close(OwlTlsSocket* s);

const char* owl_tls_last_error(const OwlTlsSocket* s);

#endif
