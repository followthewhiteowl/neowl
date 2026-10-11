// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mysql_wire_h
#define owl_sql_mysql_wire_h

/* ================================================================
 * MySQL/MariaDB wire protocol v10 — encoder/decoder
 *
 * Implemente le cote client du protocole binaire MySQL :
 * https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html
 *
 * Anatomie generale d'un paquet :
 *
 *   [length: u24 LE] [seq_num: u8] [payload: lenN]
 *
 *   length max = 16 MB - 1. Au-dela, split en chunks.
 *   seq_num : 0 a chaque nouvelle requete, incremente a chaque paquet.
 *
 * Cette couche est PURE encoder/decoder — pas de socket, pas de state
 * machine. Pour les tests unitaires + factorisation propre.
 *
 * Sources d'inspiration : sql_postgres_wire (pattern OwlBeBuffer pour
 * les paquets), libmariadb (auth plugins, capability flags).
 * ================================================================ */

#include "common.h"
#include "net_le_buffer.h"

/* ================================================================
 * Constantes protocole
 * ================================================================ */

#define MYSQL_PROTOCOL_VERSION 10
#define MYSQL_MAX_PACKET_PAYLOAD 0xFFFFFF   /* 16 MB - 1 */
#define MYSQL_HEADER_SIZE 4

/* Capability flags (bitmask 32 bits envoye dans Handshake / HandshakeResponse).
 * Subset utile cote client moderne. */
typedef enum {
    MYSQL_CAP_LONG_PASSWORD            = 0x00000001,
    MYSQL_CAP_FOUND_ROWS               = 0x00000002,
    MYSQL_CAP_LONG_FLAG                = 0x00000004,
    MYSQL_CAP_CONNECT_WITH_DB          = 0x00000008,
    MYSQL_CAP_NO_SCHEMA                = 0x00000010,
    MYSQL_CAP_COMPRESS                 = 0x00000020,
    MYSQL_CAP_LOCAL_FILES              = 0x00000080,
    MYSQL_CAP_PROTOCOL_41              = 0x00000200,  /* obligatoire */
    MYSQL_CAP_INTERACTIVE              = 0x00000400,
    MYSQL_CAP_SSL                      = 0x00000800,
    MYSQL_CAP_TRANSACTIONS             = 0x00002000,
    MYSQL_CAP_SECURE_CONNECTION        = 0x00008000,
    MYSQL_CAP_MULTI_STATEMENTS         = 0x00010000,
    MYSQL_CAP_MULTI_RESULTS            = 0x00020000,
    MYSQL_CAP_PS_MULTI_RESULTS         = 0x00040000,
    MYSQL_CAP_PLUGIN_AUTH              = 0x00080000,
    MYSQL_CAP_CONNECT_ATTRS            = 0x00100000,
    MYSQL_CAP_PLUGIN_AUTH_LENENC_DATA  = 0x00200000,
    MYSQL_CAP_DEPRECATE_EOF            = 0x01000000,
} MysqlCapability;

/* Type de paquet (premier octet du payload). Discrimine via le contexte
 * (etat du protocole) car certains codes sont ambigus selon la phase. */
typedef enum {
    MYSQL_PKT_OK            = 0x00,
    MYSQL_PKT_AUTH_MORE     = 0x01,  /* AuthMoreData (caching_sha2 step) */
    MYSQL_PKT_NULL_IN_ROW   = 0xFB,  /* SQL NULL dans une DataRow text mode */
    MYSQL_PKT_EOF_OR_AUTH_SWITCH = 0xFE, /* EOF (legacy), OK_eof, ou AuthSwitch (selon contexte) */
    MYSQL_PKT_ERROR         = 0xFF,
} MysqlPktKind;

/* Commandes client -> serveur (premier octet du payload de la requete). */
typedef enum {
    MYSQL_COM_QUIT          = 0x01,
    MYSQL_COM_INIT_DB       = 0x02,
    MYSQL_COM_QUERY         = 0x03,
    MYSQL_COM_PING          = 0x0E,
    MYSQL_COM_STMT_PREPARE  = 0x16,
    MYSQL_COM_STMT_EXECUTE  = 0x17,
    MYSQL_COM_STMT_CLOSE    = 0x19,
    MYSQL_COM_STMT_RESET    = 0x1A,
} MysqlCommand;

/* ================================================================
 * Packet framing
 * ================================================================ */

/* Commence un paquet : ecrit le header (u24 length placeholder + u8 seq).
 * Retourne l'offset du header pour back-patch ulterieur via mysql_pkt_end. */
size_t mysql_pkt_begin(OwlLeBuffer* buf, uint8_t seq_num);

/* Finalise un paquet ouvert par mysql_pkt_begin : calcule la longueur
 * du payload (depuis hdr_offset+4 jusqu'a buf.length) et la back-patch
 * dans le u24 length. */
void mysql_pkt_end(OwlLeBuffer* buf, size_t hdr_offset);

/* Lit le header d'un paquet (4 octets) depuis `in`. Retourne false si pas
 * assez d'octets. *out_len = taille du payload, *out_seq = seq_num. */
bool mysql_read_header(OwlLeBuffer* in, uint32_t* out_payload_len, uint8_t* out_seq);

/* ================================================================
 * Initial Handshake (server -> client) — parsing
 * ================================================================ */

typedef struct {
    uint8_t     protocol_version;     /* 10 attendu */
    const char* server_version;       /* zero-copy ptr dans le payload */
    uint32_t    thread_id;
    uint32_t    capabilities;         /* low + high merged */
    uint8_t     charset;
    uint16_t    status_flags;
    /* Salt : concat de auth_plugin_data_1 (8 octets) + auth_plugin_data_2.
     * Longueur typique = 20 octets pour mysql_native_password,
     *                    32 octets pour caching_sha2_password.
     * Pointe dans le payload (zero-copy). */
    const uint8_t* auth_data;
    size_t      auth_data_len;
    const char* auth_plugin_name;     /* "caching_sha2_password" ou "mysql_native_password" */
} MysqlHandshake;

/* Parse un Initial Handshake. payload = le payload du paquet (sans header).
 * Le pointeurs dans MysqlHandshake (server_version, auth_data, auth_plugin_name)
 * pointent dans payload — payload doit rester vivant pendant l'usage. */
bool mysql_parse_handshake(const uint8_t* payload, size_t payload_len,
                            MysqlHandshake* out);

/* Helper : extrait le salt complet de 20 octets (8 part1 + 12 part2)
 * a partir du payload du handshake. out_salt doit faire >= 20 octets. */
bool mysql_handshake_extract_salt(const uint8_t* payload, size_t payload_len,
                                   uint8_t out_salt[20]);

/* ================================================================
 * HandshakeResponse41 (client -> server)
 * ================================================================ */

typedef struct {
    uint32_t    client_capabilities;
    uint32_t    max_packet_size;       /* typ. 16 MB - 1 */
    uint8_t     charset;
    const char* user;
    const uint8_t* auth_response;
    size_t      auth_response_len;
    const char* database;              /* NULL si pas CONNECT_WITH_DB */
    const char* auth_plugin_name;      /* ex. "caching_sha2_password" */
} MysqlHandshakeResponse;

/* Ecrit un HandshakeResponse41 dans buf, framed (header u24+seq + payload). */
void mysql_write_handshake_response(OwlLeBuffer* buf, uint8_t seq,
                                     const MysqlHandshakeResponse* hr);

/* ================================================================
 * SSL Request (avant la promotion TLS si CLIENT_SSL set)
 * ================================================================ */

void mysql_write_ssl_request(OwlLeBuffer* buf, uint8_t seq,
                              uint32_t client_capabilities,
                              uint32_t max_packet_size,
                              uint8_t charset);

/* ================================================================
 * Commands (client -> server)
 * ================================================================ */

/* COM_QUERY : 0x03 + cstring SQL (sans \0 terminal, longueur = packet_len - 1). */
void mysql_write_com_query(OwlLeBuffer* buf, uint8_t seq, const char* sql);

/* COM_QUIT : 0x01, vide. */
void mysql_write_com_quit(OwlLeBuffer* buf, uint8_t seq);

/* COM_PING : 0x0E, vide. */
void mysql_write_com_ping(OwlLeBuffer* buf, uint8_t seq);

/* ================================================================
 * OK / Error / EOF packet parsing
 * ================================================================ */

typedef struct {
    uint64_t affected_rows;
    uint64_t last_insert_id;
    uint16_t status_flags;
    uint16_t warnings;
    /* info string (zero-copy) — ex. "Records: 3  Duplicates: 0..." */
    const char* info;
    size_t      info_len;
} MysqlOkPacket;

/* Parse un OK packet (header 0x00 ou 0xFE-OK-eof si DEPRECATE_EOF).
 * Le caller a deja consomme le header byte avant d'appeler ; payload
 * pointe directement sur les affected_rows. */
bool mysql_parse_ok(const uint8_t* payload, size_t payload_len,
                     uint32_t client_capabilities, MysqlOkPacket* out);

typedef struct {
    uint16_t    error_code;
    const char* sql_state;     /* 5 chars zero-copy, NULL si protocole < 4.1 */
    const char* message;       /* zero-copy */
    size_t      message_len;
} MysqlErrorPacket;

/* Parse un Error packet (header 0xFF deja consomme par le caller). */
bool mysql_parse_error(const uint8_t* payload, size_t payload_len,
                        MysqlErrorPacket* out);

/* ================================================================
 * AuthSwitchRequest (server peut demander de changer de plugin)
 * 0xFE + plugin_name<NUL> + plugin_data (rest of packet)
 * ================================================================ */

typedef struct {
    const char*    plugin_name;
    const uint8_t* plugin_data;
    size_t         plugin_data_len;
} MysqlAuthSwitch;

/* Parse un AuthSwitchRequest (header 0xFE deja consomme). */
bool mysql_parse_auth_switch(const uint8_t* payload, size_t payload_len,
                              MysqlAuthSwitch* out);

#endif
