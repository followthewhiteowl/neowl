// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_packet.h"

#include <string.h>
#include <stdio.h>

/* ================================================================
 * Header pack / parse
 * ================================================================ */

void owl_tds_packet_write_header(uint8_t* dst,
                                  uint8_t type,
                                  uint8_t status,
                                  uint16_t length,
                                  uint8_t packet_id) {
    dst[0] = type;
    dst[1] = status;
    /* Length : u16 BIG-ENDIAN (quirk TDS) */
    dst[2] = (uint8_t)((length >> 8) & 0xFF);
    dst[3] = (uint8_t)(length & 0xFF);
    /* SPID : u16 BIG-ENDIAN, client emet 0 (le serveur l'envoie au client
     * comme identifiant de session, mais le client peut l'ignorer). */
    dst[4] = 0;
    dst[5] = 0;
    dst[6] = packet_id;
    dst[7] = 0;            /* Window, toujours 0 */
}

bool owl_tds_packet_read_header(const uint8_t* src, size_t src_len,
                                 OwlTdsPacketHeader* out) {
    if (src_len < TDS_HEADER_SIZE) return false;
    out->type      = src[0];
    out->status    = src[1];
    out->length    = (uint16_t)((src[2] << 8) | src[3]);   /* BE */
    out->spid      = (uint16_t)((src[4] << 8) | src[5]);   /* BE */
    out->packet_id = src[6];
    out->window    = src[7];

    /* Sanity : length doit englober au moins le header lui-meme et ne pas
     * exceder le max TDS_MAX_PKT_SIZE + header. */
    if (out->length < TDS_HEADER_SIZE) return false;
    if (out->length > TDS_MAX_PKT_SIZE + TDS_HEADER_SIZE) return false;
    return true;
}

/* ================================================================
 * Sender
 * ================================================================ */

void owl_tds_packet_sender_init(OwlTdsPacketSender* s,
                                  uint8_t type,
                                  uint16_t max_payload_size) {
    s->type             = type;
    s->max_payload_size = max_payload_size > 0 ? max_payload_size
                                               : (TDS_DEFAULT_PKT_SIZE - TDS_HEADER_SIZE);
    s->next_packet_id   = 0;
}

size_t owl_tds_packet_sender_emit(OwlTdsPacketSender* s,
                                    uint8_t* dst, size_t dst_cap,
                                    const uint8_t* chunk, size_t chunk_len,
                                    bool is_last) {
    if (chunk_len > s->max_payload_size) return 0;
    size_t total = TDS_HEADER_SIZE + chunk_len;
    if (total > dst_cap) return 0;

    uint8_t status = is_last ? TDS_STATUS_EOM : TDS_STATUS_NORMAL;
    owl_tds_packet_write_header(dst, s->type, status,
                                 (uint16_t)total, s->next_packet_id);
    if (chunk_len > 0) {
        memcpy(dst + TDS_HEADER_SIZE, chunk, chunk_len);
    }
    s->next_packet_id = (uint8_t)(s->next_packet_id + 1);  /* wrap 256 */
    return total;
}

size_t owl_tds_packet_sender_emit_full(OwlTdsPacketSender* s,
                                         uint8_t* dst, size_t dst_cap,
                                         const uint8_t* message, size_t msg_len) {
    size_t emitted = 0;
    size_t remaining = msg_len;
    const uint8_t* cursor = message;

    /* Cas message vide : on emet quand meme un paquet vide avec EOM. C'est
     * ce que fait SQL Server pour les Attention packets, par exemple. */
    if (msg_len == 0) {
        size_t n = owl_tds_packet_sender_emit(s, dst, dst_cap, NULL, 0, true);
        return n;
    }

    while (remaining > 0) {
        size_t chunk = remaining > s->max_payload_size ? s->max_payload_size
                                                        : remaining;
        bool   last  = (chunk == remaining);
        size_t n = owl_tds_packet_sender_emit(s,
                                                dst + emitted,
                                                dst_cap - emitted,
                                                cursor, chunk, last);
        if (n == 0) return 0;
        emitted += n;
        cursor  += chunk;
        remaining -= chunk;
    }
    return emitted;
}

/* ================================================================
 * Reassembler
 * ================================================================ */

void owl_tds_packet_reassembler_init(OwlTdsPacketReassembler* r) {
    owl_le_buffer_init(&r->payload, 4096);
    r->msg_type = 0;
    r->complete = false;
    r->started  = false;
    r->error[0] = '\0';
}

void owl_tds_packet_reassembler_free(OwlTdsPacketReassembler* r) {
    owl_le_buffer_free(&r->payload);
    r->msg_type = 0;
    r->complete = false;
    r->started  = false;
    r->error[0] = '\0';
}

void owl_tds_packet_reassembler_reset(OwlTdsPacketReassembler* r) {
    owl_le_buffer_reset(&r->payload);
    r->msg_type = 0;
    r->complete = false;
    r->started  = false;
    r->error[0] = '\0';
}

bool owl_tds_packet_reassembler_push(OwlTdsPacketReassembler* r,
                                       const uint8_t* packet, size_t packet_len) {
    OwlTdsPacketHeader h;
    if (!owl_tds_packet_read_header(packet, packet_len, &h)) {
        snprintf(r->error, sizeof(r->error),
                 "header invalide (packet_len=%zu)", packet_len);
        return false;
    }
    if (h.length > packet_len) {
        snprintf(r->error, sizeof(r->error),
                 "length declare %u > recu %zu", h.length, packet_len);
        return false;
    }

    /* Premier paquet : on fige le type attendu. */
    if (!r->started) {
        r->msg_type = h.type;
        r->started  = true;
    } else if (h.type != r->msg_type) {
        snprintf(r->error, sizeof(r->error),
                 "type change en cours de message (%u vs %u)",
                 h.type, r->msg_type);
        return false;
    }

    /* Accumule le payload (h.length - TDS_HEADER_SIZE octets). */
    size_t payload_len = h.length - TDS_HEADER_SIZE;
    if (payload_len > 0) {
        owl_le_buffer_write_bytes(&r->payload,
                                    packet + TDS_HEADER_SIZE,
                                    payload_len);
    }

    if (h.status & TDS_STATUS_EOM) {
        r->complete = true;
    }
    return true;
}
