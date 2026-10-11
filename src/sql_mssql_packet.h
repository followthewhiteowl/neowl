// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_packet_h
#define owl_sql_mssql_packet_h

/* ================================================================
 * MSSQL TDS 7.4 — packet framing
 *
 * Implemente la couche basse du protocole Tabular Data Stream :
 * encapsulation des messages logiques (Login7, SQLBatch, etc.) dans
 * des paquets TDS de taille fixe (negociee, defaut 4096 octets).
 *
 * Anatomie d'un paquet TDS :
 *
 *   Offset  Size  Champ        Endianness
 *   0       1     Type         -          (PreLogin, Login7, SQLBatch...)
 *   1       1     Status       -          (EOM, Ignore, ResetConn flags)
 *   2       2     Length       BE         (taille totale = header + payload)
 *   4       2     SPID         BE         (server process ID, ignore client)
 *   6       1     PacketID     -          (sequence, wrap 256)
 *   7       1     Window       -          (toujours 0)
 *
 * Particularite : Length et SPID sont en BIG-ENDIAN dans le header,
 * mais tout le payload TDS est en LITTLE-ENDIAN. Quirk historique a
 * cause d'origines Sybase/IBM. C'est la seule partie BE du protocole.
 *
 * Un message logique > packet size est splitte en plusieurs paquets,
 * tous avec le meme Type, sequence PacketID incrementee, et le dernier
 * porte le flag EOM=1 dans Status.
 *
 * Specification reference :
 *   MS-TDS section 2.2.3 (Packet Header)
 *   MS-TDS section 2.2.5 (Data Stream Headers and Tokens)
 *
 * Cette couche est PURE encoder/decoder — pas de socket, pas de state
 * machine. Pour les tests unitaires + factorisation propre.
 * ================================================================ */

#include "common.h"
#include "net_le_buffer.h"

/* ================================================================
 * Constantes protocole
 * ================================================================ */

#define TDS_HEADER_SIZE       8
#define TDS_DEFAULT_PKT_SIZE  4096    /* Taille payload negociable de 512 a 32767 */
#define TDS_MAX_PKT_SIZE      32767

/* Types de paquet (octet 0 du header). MS-TDS 2.2.3.1.1 */
typedef enum {
    TDS_PKT_SQL_BATCH       = 0x01,
    TDS_PKT_PRE_TDS7_LOGIN  = 0x02,   /* obsolete, jamais utilise par OWL */
    TDS_PKT_RPC             = 0x03,
    TDS_PKT_TABULAR_RESULT  = 0x04,   /* reponse serveur */
    TDS_PKT_ATTENTION       = 0x06,
    TDS_PKT_BULK_LOAD       = 0x07,
    TDS_PKT_FEDERATED_AUTH  = 0x08,
    TDS_PKT_TRANSACTION_MGR = 0x0E,
    TDS_PKT_LOGIN7          = 0x10,
    TDS_PKT_SSPI            = 0x11,
    TDS_PKT_PRELOGIN        = 0x12,
} OwlTdsPacketType;

/* Flags de Status (octet 1 du header). MS-TDS 2.2.3.1.2 */
#define TDS_STATUS_NORMAL          0x00
#define TDS_STATUS_EOM             0x01   /* end of message (dernier paquet) */
#define TDS_STATUS_IGNORE          0x02
#define TDS_STATUS_RESET_CONN      0x08
#define TDS_STATUS_RESET_CONN_SKIP 0x10

/* ================================================================
 * Header parsing / packing
 * ================================================================ */

typedef struct {
    uint8_t  type;
    uint8_t  status;
    uint16_t length;       /* taille totale du paquet, header inclus */
    uint16_t spid;
    uint8_t  packet_id;
    uint8_t  window;
} OwlTdsPacketHeader;

/* Ecrit les 8 octets du header dans dst. dst doit avoir TDS_HEADER_SIZE
 * octets disponibles. length doit etre le total (header + payload). */
void owl_tds_packet_write_header(uint8_t* dst,
                                  uint8_t type,
                                  uint8_t status,
                                  uint16_t length,
                                  uint8_t packet_id);

/* Parse les 8 octets du header depuis src. Renvoie false si src trop court
 * ou length aberrante (< 8 ou > TDS_MAX_PKT_SIZE+TDS_HEADER_SIZE). */
bool owl_tds_packet_read_header(const uint8_t* src, size_t src_len,
                                 OwlTdsPacketHeader* out);

/* ================================================================
 * Multi-packet sender : decoupe un message logique en paquets
 * ================================================================ */

typedef struct {
    uint8_t  type;
    uint16_t max_payload_size;   /* taille payload max (sans header) */
    uint8_t  next_packet_id;
} OwlTdsPacketSender;

/* Initialise un sender pour emettre un message logique de type donne.
 * max_payload_size = TDS_DEFAULT_PKT_SIZE - TDS_HEADER_SIZE par defaut. */
void owl_tds_packet_sender_init(OwlTdsPacketSender* s,
                                  uint8_t type,
                                  uint16_t max_payload_size);

/* Emet UN paquet contenant chunk_len octets de payload depuis chunk.
 * is_last = true positionne le flag EOM. Ecrit header + payload dans dst.
 * Renvoie le nombre d'octets ecrits (header + chunk), ou 0 en cas d'erreur
 * (chunk trop long ou dst trop court).
 *
 * Pour decouper un message > max_payload_size, le caller fait plusieurs
 * appels avec is_last=true seulement sur le dernier. */
size_t owl_tds_packet_sender_emit(OwlTdsPacketSender* s,
                                    uint8_t* dst, size_t dst_cap,
                                    const uint8_t* chunk, size_t chunk_len,
                                    bool is_last);

/* Helper : emet un message logique complet (gere le chunking).
 * Ecrit dans dst tout ce qu'il faut. Renvoie le total d'octets ecrits,
 * ou 0 si dst trop court (calcul preventif : ceil(msg_len/max)*8 + msg_len). */
size_t owl_tds_packet_sender_emit_full(OwlTdsPacketSender* s,
                                         uint8_t* dst, size_t dst_cap,
                                         const uint8_t* message, size_t msg_len);

/* ================================================================
 * Multi-packet reassembler : accumule les paquets entrants
 * ================================================================ */

typedef struct {
    OwlLeBuffer payload;        /* payload assemble (sans headers) */
    uint8_t     msg_type;       /* type du premier paquet (tous doivent matcher) */
    bool        complete;       /* true quand EOM vu */
    bool        started;        /* true des le premier paquet pousse */
    char        error[128];     /* message d'erreur si push retourne false */
} OwlTdsPacketReassembler;

void owl_tds_packet_reassembler_init(OwlTdsPacketReassembler* r);
void owl_tds_packet_reassembler_free(OwlTdsPacketReassembler* r);
void owl_tds_packet_reassembler_reset(OwlTdsPacketReassembler* r);

/* Pousse un paquet complet (header + payload, tel que lu sur le socket).
 * Verifie length coherente et type stable. Concatene le payload dans
 * r->payload. Si Status&EOM, marque r->complete = true.
 * Renvoie false sur incoherence (avec r->error rempli). */
bool owl_tds_packet_reassembler_push(OwlTdsPacketReassembler* r,
                                       const uint8_t* packet, size_t packet_len);

#endif /* owl_sql_mssql_packet_h */
