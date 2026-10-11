// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_tls_h
#define owl_sql_mssql_tls_h

/* ================================================================
 * MSSQL TDS 7.4 — TLS-in-TDS handshake bridge
 *
 * Particularite TDS : pendant le handshake TLS, les records TLS sont
 * encapsules dans des paquets TDS de type PRELOGIN (0x12). Apres la
 * fin du handshake :
 *   - mode ENCRYPT_ON  : les records TLS deviennent "raw" sur TCP
 *                        (le payload TLS contiendra les paquets TDS
 *                        Login7/SQLBatch/...). Plus de wrapping TDS.
 *   - mode ENCRYPT_OFF : TLS est ferme apres login, retour plain TCP.
 *
 * Ce module fournit :
 *   1. Une abstraction "transport" (send/recv via fct pointers) pour
 *      decoupler du vrai socket TCP.
 *   2. Deux callbacks compatibles mbedtls_ssl_set_bio :
 *        owl_tds_tls_bio_send  : ecrit, wrap dans un paquet TDS si actif
 *        owl_tds_tls_bio_recv  : lit, deballe le paquet TDS si actif
 *   3. Une bascule wrap on/off appelee apres handshake selon le mode
 *      negocie.
 *
 * Compatible mbedTLS : les callbacks renvoient :
 *   > 0  : nombre d'octets envoyes / recus
 *   == 0 : (rare) signifie "rien dispo, reessayer" -> non utilise
 *   < 0  : erreur (codes mbedtls compatibles MBEDTLS_ERR_NET_*)
 * ================================================================ */

#include "common.h"
#include "sql_mssql_packet.h"

/* Signatures transport. Retournent :
 *   > 0 : octets traites
 *   0   : EOF (recv) ou erreur (send) — caller doit traiter comme echec
 *   < 0 : code d'erreur opaque (-1 generique suffit pour V1) */
typedef int (*OwlTdsTcpSendFn)(void* ctx, const uint8_t* data, size_t len);
typedef int (*OwlTdsTcpRecvFn)(void* ctx, uint8_t* buf, size_t cap);

/* Etat du BIO TLS-in-TDS. */
typedef struct {
    /* Transport sous-jacent (TCP en prod, memoire en tests). */
    OwlTdsTcpSendFn send_fn;
    OwlTdsTcpRecvFn recv_fn;
    void*           transport_ctx;

    /* Mode wrap : true pendant handshake (TLS records dans paquets TDS 0x12),
     * false apres handshake en ENCRYPT_ON (TLS records raw sur TCP). */
    bool wrap;

    /* Sender pour la couche TDS (utilise quand wrap = true). Type fixe a
     * TDS_PKT_PRELOGIN. */
    OwlTdsPacketSender sender;

    /* Buffer recv : on accumule le payload du dernier paquet TDS lu pour
     * le servir par morceaux a mbedTLS au fil de ses appels. */
    uint8_t* recv_buf;
    size_t   recv_cap;
    size_t   recv_len;
    size_t   recv_off;

    /* Buffer send (wrap mode) : SQL Server attend que tous les TLS records
     * d'une flight handshake (ClientKeyExchange + ChangeCipherSpec +
     * Finished par ex.) soient bundled dans UN SEUL paquet TDS 0x12.
     * On accumule donc les writes mbedTLS dans send_buf et on flushe
     * comme un unique paquet TDS quand recv() est appele (= tour serveur)
     * ou quand wrap est desactive (= fin handshake). */
    uint8_t* send_buf;
    size_t   send_cap;
    size_t   send_len;

    /* Taille max payload TDS (defaut TDS_DEFAULT_PKT_SIZE - 8). */
    uint16_t max_payload;

    /* Message d'erreur si une fonction retourne < 0. */
    char error[128];
} OwlTdsTlsBio;

/* Initialise un BIO en mode wrap (handshake). recv_buf_cap recommande
 * = 8192 (taille raisonnable pour stocker un paquet TDS de payload). */
void owl_tds_tls_bio_init(OwlTdsTlsBio* bio,
                            OwlTdsTcpSendFn send_fn,
                            OwlTdsTcpRecvFn recv_fn,
                            void* transport_ctx,
                            size_t recv_buf_cap);

void owl_tds_tls_bio_free(OwlTdsTlsBio* bio);

/* Bascule le mode wrap. true (defaut a init) = wrap TLS records dans paquets
 * TDS 0x12. false = transport TCP direct (apres fin de handshake en ENCRYPT_ON). */
void owl_tds_tls_bio_set_wrap(OwlTdsTlsBio* bio, bool wrap);

/* Callbacks compatibles mbedtls_ssl_set_bio. ctx doit etre un OwlTdsTlsBio*. */
int owl_tds_tls_bio_send(void* ctx, const uint8_t* buf, size_t len);
int owl_tds_tls_bio_recv(void* ctx, uint8_t* buf, size_t len);

#endif /* owl_sql_mssql_tls_h */
