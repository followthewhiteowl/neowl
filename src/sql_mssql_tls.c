// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_tls.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Wire de tracing sur la couche BIO — desactive en prod. Mettre a 1 pour
 * imprimer chaque send/recv (utile pour diagnostiquer un handshake qui
 * casse). */
#define OWL_TDS_TLS_BIO_DEBUG 0
#if OWL_TDS_TLS_BIO_DEBUG
#  define BIO_DBG(...) fprintf(stderr, "[bio] " __VA_ARGS__)
#else
#  define BIO_DBG(...) ((void)0)
#endif

/* Forward declaration : flush utilise par set_wrap qui vient avant. */
static bool flush_send_buffer(OwlTdsTlsBio* bio);

/* ================================================================
 * Init / free
 * ================================================================ */

void owl_tds_tls_bio_init(OwlTdsTlsBio* bio,
                            OwlTdsTcpSendFn send_fn,
                            OwlTdsTcpRecvFn recv_fn,
                            void* transport_ctx,
                            size_t recv_buf_cap) {
    bio->send_fn       = send_fn;
    bio->recv_fn       = recv_fn;
    bio->transport_ctx = transport_ctx;
    bio->wrap          = true;
    bio->max_payload   = TDS_DEFAULT_PKT_SIZE - TDS_HEADER_SIZE;
    bio->recv_buf      = (uint8_t*)malloc(recv_buf_cap);
    bio->recv_cap      = recv_buf_cap;
    bio->recv_len      = 0;
    bio->recv_off      = 0;
    bio->send_buf      = (uint8_t*)malloc(recv_buf_cap);   /* meme taille */
    bio->send_cap      = recv_buf_cap;
    bio->send_len      = 0;
    bio->error[0]      = '\0';
    owl_tds_packet_sender_init(&bio->sender, TDS_PKT_PRELOGIN, bio->max_payload);
}

void owl_tds_tls_bio_free(OwlTdsTlsBio* bio) {
    free(bio->recv_buf);
    free(bio->send_buf);
    bio->recv_buf = NULL;
    bio->send_buf = NULL;
    bio->recv_cap = 0;
    bio->recv_len = 0;
    bio->recv_off = 0;
    bio->send_cap = 0;
    bio->send_len = 0;
}

void owl_tds_tls_bio_set_wrap(OwlTdsTlsBio* bio, bool wrap) {
    /* Si on bascule de wrap a non-wrap : flush ce qui reste en attente
     * pour ne pas perdre le dernier paquet handshake. */
    if (bio->wrap && !wrap && bio->send_len > 0) {
        flush_send_buffer(bio);
    }
    bio->wrap = wrap;
    /* On conserve le buffer recv pour permettre a TLS de finir de drainer
     * une trame partiellement lue. */
}

/* ================================================================
 * Send : ecrit via transport, en wrap = TDS packet, en raw = direct
 *
 * Pour V1 on choisit la strategie "1 appel send mbedTLS = 1 paquet TDS"
 * (avec EOM). Si l'appel depasse max_payload, on emet plusieurs paquets
 * dont seul le dernier porte EOM. Cette strategie marche pour SQL Server :
 * les TLS records lui arrivent en "fragments" successifs, ce qui reste
 * valide TLS-cote-serveur.
 *
 * Retour : len traite (positif), ou -1 sur erreur transport.
 * ================================================================ */

static int send_raw(OwlTdsTlsBio* bio, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = bio->send_fn(bio->transport_ctx, data + sent, len - sent);
        if (n <= 0) {
            snprintf(bio->error, sizeof(bio->error),
                     "transport send echec apres %zu/%zu octets (n=%d)",
                     sent, len, n);
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

/* Flush le buffer send accumule comme UN SEUL paquet TDS 0x12 avec EOM.
 * SQL Server attend que tous les TLS records d'une flight handshake
 * soient bundled. Renvoie true si OK. */
static bool flush_send_buffer(OwlTdsTlsBio* bio) {
    if (bio->send_len == 0) return true;
    BIO_DBG("FLUSH send_buf %zu bytes -> 1 TDS packet\n", bio->send_len);

    size_t pkt_cap   = TDS_HEADER_SIZE + bio->max_payload;
    uint8_t* scratch = (uint8_t*)malloc(pkt_cap);
    if (!scratch) {
        snprintf(bio->error, sizeof(bio->error), "OOM scratch flush");
        return false;
    }

    size_t cursor = 0;
    bool ok = true;
    while (cursor < bio->send_len) {
        size_t chunk = (bio->send_len - cursor) > bio->max_payload
                       ? bio->max_payload : (bio->send_len - cursor);
        bool is_last = (cursor + chunk == bio->send_len);
        size_t emitted = owl_tds_packet_sender_emit(&bio->sender,
                                                      scratch, pkt_cap,
                                                      bio->send_buf + cursor,
                                                      chunk, is_last);
        if (emitted == 0) {
            snprintf(bio->error, sizeof(bio->error), "emit packet flush echec");
            ok = false; break;
        }
        if (send_raw(bio, scratch, emitted) < 0) {
            ok = false; break;
        }
        cursor += chunk;
    }
    free(scratch);
    bio->send_len = 0;
    return ok;
}

int owl_tds_tls_bio_send(void* ctx, const uint8_t* buf, size_t len) {
    OwlTdsTlsBio* bio = (OwlTdsTlsBio*)ctx;

    BIO_DBG("SEND wrap=%d len=%zu first4=[%02x %02x %02x %02x]\n",
            (int)bio->wrap, len,
            len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0,
            len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0);

    if (!bio->wrap) {
        /* Passthrough TCP direct (apres handshake en ENCRYPT_ON). */
        return send_raw(bio, buf, len);
    }

    /* Wrap mode : accumule dans send_buf. Le flush effectif (= emission
     * du paquet TDS 0x12) intervient :
     *  - au prochain appel a recv() (= tour du serveur)
     *  - au passage en mode non-wrap (= fin handshake)
     * Cela permet a SQL Server de recevoir CKE+CCS+Finished comme UN
     * SEUL paquet TDS, ce qu'il exige (l'envoi en 3 paquets separes
     * provoque une fermeture TCP cote serveur). */
    if (bio->send_len + len > bio->send_cap) {
        /* Grow exponentiel. */
        size_t new_cap = bio->send_cap ? bio->send_cap * 2 : 4096;
        while (new_cap < bio->send_len + len) new_cap *= 2;
        uint8_t* nb = (uint8_t*)realloc(bio->send_buf, new_cap);
        if (!nb) {
            snprintf(bio->error, sizeof(bio->error), "OOM grow send_buf");
            return -1;
        }
        bio->send_buf = nb;
        bio->send_cap = new_cap;
    }
    memcpy(bio->send_buf + bio->send_len, buf, len);
    bio->send_len += len;
    return (int)len;
}

/* ================================================================
 * Recv : sert depuis recv_buf, rerempli depuis transport au besoin
 *
 * En wrap mode : on lit un paquet TDS complet (header + payload) du
 * transport, et on copie le payload dans recv_buf. mbedTLS draine
 * recv_buf morceau par morceau.
 *
 * En raw mode : on copie directement les octets du transport (l'appelant
 * voit du TCP brut).
 *
 * Retour : nombre d'octets remplis dans buf (1..len), ou -1 sur erreur.
 *          jamais 0 sauf cas degenere (len=0).
 * ================================================================ */

/* Recoit exactement n octets depuis le transport (boucle anti court-recv). */
static int recv_exact(OwlTdsTlsBio* bio, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = bio->recv_fn(bio->transport_ctx, dst + got, n - got);
        if (r <= 0) {
            snprintf(bio->error, sizeof(bio->error),
                     "transport recv echec apres %zu/%zu (r=%d)",
                     got, n, r);
            return -1;
        }
        got += (size_t)r;
    }
    return (int)got;
}

/* Lit un paquet TDS complet depuis le transport et stocke son payload dans
 * recv_buf. Renvoie true si OK. */
static bool pull_one_packet(OwlTdsTlsBio* bio) {
    uint8_t hdr[TDS_HEADER_SIZE];
    if (recv_exact(bio, hdr, TDS_HEADER_SIZE) < 0) return false;

    OwlTdsPacketHeader h;
    if (!owl_tds_packet_read_header(hdr, TDS_HEADER_SIZE, &h)) {
        snprintf(bio->error, sizeof(bio->error), "header TDS recu invalide");
        return false;
    }

    size_t payload_len = (size_t)h.length - TDS_HEADER_SIZE;
    if (payload_len > bio->recv_cap) {
        snprintf(bio->error, sizeof(bio->error),
                 "payload %zu > recv_cap %zu", payload_len, bio->recv_cap);
        return false;
    }

    bio->recv_len = 0;
    bio->recv_off = 0;
    if (payload_len > 0) {
        if (recv_exact(bio, bio->recv_buf, payload_len) < 0) return false;
        bio->recv_len = payload_len;
    }
    /* h.status & TDS_STATUS_EOM : non utilise ici. mbedTLS va re-lire jusqu'a
     * obtenir un record TLS complet ; si plusieurs paquets sont necessaires,
     * pull_one_packet sera rappele. */
    return true;
}

int owl_tds_tls_bio_recv(void* ctx, uint8_t* buf, size_t len) {
    OwlTdsTlsBio* bio = (OwlTdsTlsBio*)ctx;
    if (len == 0) return 0;

    if (!bio->wrap) {
        int n = bio->recv_fn(bio->transport_ctx, buf, len);
        BIO_DBG("RECV passthrough len_req=%zu got=%d\n", len, n);
        return n;
    }

    /* Recv = tour du serveur. Flush ce qu'on a accumule cote send d'abord
     * pour que SQL Server recoive notre flight handshake en UN paquet TDS. */
    if (bio->send_len > 0) {
        if (!flush_send_buffer(bio)) return -1;
    }

    /* Si buffer recv vide -> on tire un nouveau paquet TDS. */
    if (bio->recv_off >= bio->recv_len) {
        BIO_DBG("RECV wrap pulling new packet (req=%zu)\n", len);
        if (!pull_one_packet(bio)) {
            BIO_DBG("RECV wrap pull failed: %s\n", bio->error);
            return -1;
        }
        BIO_DBG("RECV wrap pulled payload_len=%zu first4=[%02x %02x %02x %02x]\n",
                bio->recv_len,
                bio->recv_len > 0 ? bio->recv_buf[0] : 0,
                bio->recv_len > 1 ? bio->recv_buf[1] : 0,
                bio->recv_len > 2 ? bio->recv_buf[2] : 0,
                bio->recv_len > 3 ? bio->recv_buf[3] : 0);
        if (bio->recv_len == 0) {
            if (!pull_one_packet(bio)) return -1;
            if (bio->recv_len == 0) {
                snprintf(bio->error, sizeof(bio->error),
                         "2 paquets vides consecutifs en wrap recv");
                return -1;
            }
        }
    }

    /* Copie ce qu'on peut depuis recv_buf vers buf. */
    size_t avail = bio->recv_len - bio->recv_off;
    size_t n     = (avail < len) ? avail : len;
    memcpy(buf, bio->recv_buf + bio->recv_off, n);
    bio->recv_off += n;
    BIO_DBG("RECV wrap served %zu/%zu (off=%zu/%zu)\n",
            n, len, bio->recv_off, bio->recv_len);
    return (int)n;
}
