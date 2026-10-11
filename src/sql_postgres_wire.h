// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_postgres_wire_h
#define owl_sql_postgres_wire_h

/* ================================================================
 * PostgreSQL wire protocol v3 — encoder/decoder
 *
 * Implémente le côté client du protocole binaire PostgreSQL v3.0
 * (utilisé depuis PG 7.4, stable depuis 25 ans, documenté sur
 * https://www.postgresql.org/docs/current/protocol.html).
 *
 * Format général d'un message :
 *
 *   Client -> Server (sauf StartupMessage / SSLRequest) :
 *     [type : 1 byte] [length : i32 BE] [payload]
 *     length = total - 1 (inclut length, pas type)
 *
 *   StartupMessage et SSLRequest n'ont PAS de type byte :
 *     [length : i32 BE] [content]
 *     length = total
 *
 *   Server -> Client : toujours
 *     [type : 1 byte] [length : i32 BE] [payload]
 *
 * Cette implem ne dépend que de net_be_buffer pour le binaire.
 * Aucun socket, aucune allocation de string : pur encoder/decoder
 * pour faciliter les tests unitaires.
 * ================================================================ */

#include "common.h"
#include "net_be_buffer.h"

/* Type bytes des messages serveur (1 char ASCII). */
typedef enum {
    POSTGRES_MSG_AUTH            = 'R',  /* AuthenticationXxx */
    POSTGRES_MSG_PARAM_STATUS    = 'S',  /* ParameterStatus / SSL response */
    POSTGRES_MSG_BACKEND_KEY     = 'K',  /* BackendKeyData (pour cancel) */
    POSTGRES_MSG_READY           = 'Z',  /* ReadyForQuery */
    POSTGRES_MSG_ROW_DESC        = 'T',  /* RowDescription */
    POSTGRES_MSG_DATA_ROW        = 'D',  /* DataRow */
    POSTGRES_MSG_CMD_COMPLETE    = 'C',  /* CommandComplete + CloseClient */
    POSTGRES_MSG_EMPTY_QUERY     = 'I',  /* EmptyQueryResponse */
    POSTGRES_MSG_ERROR           = 'E',  /* ErrorResponse + Execute (client) */
    POSTGRES_MSG_NOTICE          = 'N',  /* NoticeResponse (ou NegotiateProtocolVersion) */
    POSTGRES_MSG_PARSE_COMPLETE  = '1',
    POSTGRES_MSG_BIND_COMPLETE   = '2',
    POSTGRES_MSG_CLOSE_COMPLETE  = '3',
    POSTGRES_MSG_NO_DATA         = 'n',
    POSTGRES_MSG_PARAM_DESC      = 't',
    POSTGRES_MSG_PORTAL_SUSPEND  = 's',
} PostgresMsgType;

/* Sub-codes d'AuthenticationXxx (i32 dans le payload). */
typedef enum {
    POSTGRES_AUTH_OK             = 0,
    POSTGRES_AUTH_CLEARTEXT_PWD  = 3,
    POSTGRES_AUTH_MD5_PWD        = 5,
    POSTGRES_AUTH_SASL           = 10,   /* doit utiliser SCRAM-SHA-256 ou SCRAM-SHA-256-PLUS */
    POSTGRES_AUTH_SASL_CONTINUE  = 11,
    POSTGRES_AUTH_SASL_FINAL     = 12,
} PostgresAuthCode;

/* SSL request : code magique entier dans la spec PG. */
#define POSTGRES_SSL_REQUEST_CODE 80877103

/* Protocol version 3.0 (major << 16 | minor). */
#define POSTGRES_PROTOCOL_VERSION 196608   /* 3 << 16 | 0 */

/* ================================================================
 * Write helpers (client -> server)
 * ================================================================ */

/* Commence un message standard (avec type byte). Écrit le type +
 * 4 bytes placeholder pour la length. Retourne l'offset de la length
 * à back-patcher avec postgres_msg_end. */
size_t postgres_msg_begin(OwlBeBuffer* buf, char type);

/* Variante pour StartupMessage / SSLRequest qui n'ont pas de type byte :
 * écrit juste 4 bytes placeholder de length. */
size_t postgres_msg_begin_no_type(OwlBeBuffer* buf);

/* Termine un message commencé avec postgres_msg_begin* : calcule la length
 * réelle (depuis len_offset jusqu'à buf.length actuelle) et la back-patch. */
void postgres_msg_end(OwlBeBuffer* buf, size_t len_offset);

/* Constructeurs de haut niveau. Tous append au buffer existant —
 * permet de batch plusieurs messages dans un seul send (pipelining). */

void postgres_write_startup(OwlBeBuffer* buf, const char* user, const char* database);
void postgres_write_ssl_request(OwlBeBuffer* buf);
void postgres_write_password(OwlBeBuffer* buf, const char* password);  /* cleartext ou MD5 */
void postgres_write_sasl_initial_response(OwlBeBuffer* buf,
                                     const char* mechanism,
                                     const char* sasl_first_msg);
void postgres_write_sasl_response(OwlBeBuffer* buf, const char* sasl_resp);
void postgres_write_query(OwlBeBuffer* buf, const char* sql);
void postgres_write_terminate(OwlBeBuffer* buf);

/* Extended query (V2.1) — pour requetes parametrees ($1, $2, ...).
 * Sequence : Parse -> Bind -> Describe -> Execute -> Sync.
 * Reponses serveur : ParseComplete '1' + BindComplete '2' +
 * RowDescription 'T' + DataRow 'D' + CommandComplete 'C' + 'Z'. */
void postgres_write_parse(OwlBeBuffer* buf,
                           const char* stmt_name,
                           const char* sql,
                           int n_param_types,
                           const uint32_t* param_type_oids);
/* Bind en text mode (format 0 pour tous params + results).
 * param_values[i] = NULL -> SQL NULL (length -1). */
void postgres_write_bind_text(OwlBeBuffer* buf,
                               const char* portal_name,
                               const char* stmt_name,
                               int n_params,
                               const char* const* param_values);
void postgres_write_describe(OwlBeBuffer* buf, char kind, const char* name);
void postgres_write_execute(OwlBeBuffer* buf, const char* portal_name, int32_t max_rows);
void postgres_write_sync(OwlBeBuffer* buf);

/* ================================================================
 * Read helpers (server -> client)
 * ================================================================ */

/* Lit le header d'un message (1 byte type + 4 bytes length BE).
 * Le caller doit ensuite lire (length - 4) octets de payload depuis
 * la source (socket). Retourne false si pas assez d'octets pour le
 * header complet (5 bytes minimum). */
bool postgres_read_msg_header(OwlBeBuffer* in, char* out_type, uint32_t* out_payload_len);

/* Parse un AuthenticationXxx message dont le payload a déjà été lu.
 * payload pointe sur le body (length-4 octets). Retourne le sub-code
 * d'auth ; sasl_data (si non-NULL et code == SASL_CONTINUE/FINAL)
 * pointe vers les octets suivants dans le payload (zero-copy).
 * Retourne -1 si payload malformé. */
int postgres_parse_auth(const uint8_t* payload, uint32_t payload_len,
                   const uint8_t** out_sasl_data, uint32_t* out_sasl_len);

/* Parse un ErrorResponse / NoticeResponse — extrait le champ 'M'
 * (Message human-readable). Retourne pointer interne (zero-copy) ou
 * NULL si pas de 'M' trouvé. Le payload doit rester vivant pendant
 * l'usage du retour. */
const char* postgres_parse_error_message(const uint8_t* payload, uint32_t payload_len);

#endif
