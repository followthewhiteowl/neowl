// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_login_h
#define owl_sql_mssql_login_h

/* ================================================================
 * MSSQL TDS 7.4 — Login7 packet + response parser
 *
 * Login7 (type TDS 0x10) est le paquet d'authentification envoye juste
 * apres le handshake TLS. Il porte :
 *   - HostName, UserName, Password (obfusque), AppName, ServerName,
 *     CltIntName, Language, Database
 *   - ClientID (MAC, optionnel)
 *   - Options : byte order, char set, TDS version, packet size,
 *     client PID, etc.
 *
 * Le serveur repond avec un TabularResult (type 0x04) contenant une
 * sequence de tokens :
 *   - ENVCHANGE (0xE3) : changement d'env (packet size, db, language)
 *   - INFO      (0xAB) : message informatif
 *   - LOGINACK  (0xAD) : login accepte (porte la version serveur)
 *   - DONE      (0xFD) : fin de la reponse
 *   - ERROR     (0xAA) : login refuse (login invalide, db inexistante)
 *
 * Auth supportee V1 : SQL Server Authentication (user/password).
 * Auth Windows Integrated (NTLM/Kerberos via SSPI) deferree V2.
 *
 * References :
 *   MS-TDS 2.2.6.4 (LOGIN7)
 *   MS-TDS 2.2.7   (Data Stream Tokens)
 * ================================================================ */

#include "common.h"
#include "net_le_buffer.h"

/* ================================================================
 * Login7 — construction
 * ================================================================ */

/* TDS 7.4 = SQL Server 2012+. */
#define TDS_VERSION_7_4 0x74000004u

/* Parametres d'un Login7 client.
 * Champs string : UTF-8 nul-termines. NULL ou "" = champ omis (offset/length=0).
 * password est obfusque automatiquement par le pack (XOR 0xA5 + swap nibbles).
 *
 * Defaults pertinents :
 *   appname      = "OWL"
 *   clt_int_name = "owl-mssql"
 *   packet_size  = 4096
 *   client_pid   = 0
 *   client_id    = {0,0,0,0,0,0}   (MAC fictive)
 */
typedef struct {
    const char* hostname;
    const char* username;
    const char* password;
    const char* appname;
    const char* servername;
    const char* database;
    const char* language;
    const char* clt_int_name;
    uint32_t    packet_size;
    uint32_t    client_pid;
    uint8_t     client_id[6];
} OwlMssqlLoginRequest;

/* Construit le payload Login7 dans out (vide avant). Renvoie true si OK.
 * Le payload obtenu doit ensuite etre encapsule dans un paquet TDS 0x10. */
bool owl_mssql_login_pack(OwlLeBuffer* out, const OwlMssqlLoginRequest* req);

/* Obfuscation du mot de passe : swap_nibbles(b) XOR 0xA5 sur chaque
 * octet. Ce n'est PAS du chiffrement, juste un masquage trivial pour
 * eviter le clear pwd dans wireshark si TLS est off. Reversible cote
 * serveur en appliquant XOR puis swap.
 *
 * Expose pour les tests. */
void owl_mssql_login_obfuscate(uint8_t* data, size_t len);

/* ================================================================
 * Login response parsing
 * ================================================================ */

/* Tokens TDS rencontres dans la reponse de login. */
typedef enum {
    TDS_TOK_ERROR        = 0xAA,
    TDS_TOK_INFO         = 0xAB,
    TDS_TOK_LOGINACK     = 0xAD,
    TDS_TOK_ENVCHANGE    = 0xE3,
    TDS_TOK_DONE         = 0xFD,
    TDS_TOK_DONEPROC     = 0xFE,
    TDS_TOK_DONEINPROC   = 0xFF,
    TDS_TOK_RETURNSTATUS = 0x79,
    TDS_TOK_COLMETADATA  = 0x81,   /* utilise en Lot 4 */
    TDS_TOK_ROW          = 0xD1,   /* utilise en Lot 4 */
    TDS_TOK_NBCROW       = 0xD2,   /* utilise en Lot 4 */
    TDS_TOK_ORDER        = 0xA9,   /* utilise en Lot 4 */
} OwlTdsToken;

/* Sous-types du token ENVCHANGE. MS-TDS 2.2.7.9 */
typedef enum {
    TDS_ENVCHANGE_DATABASE        = 1,
    TDS_ENVCHANGE_LANGUAGE        = 2,
    TDS_ENVCHANGE_CHARSET         = 3,
    TDS_ENVCHANGE_PACKET_SIZE     = 4,
    TDS_ENVCHANGE_COLLATION       = 7,
    TDS_ENVCHANGE_BEGIN_TXN       = 8,
    TDS_ENVCHANGE_COMMIT_TXN      = 9,
    TDS_ENVCHANGE_ROLLBACK_TXN    = 10,
    TDS_ENVCHANGE_ENLIST_DTC      = 11,
    TDS_ENVCHANGE_DEFECT_DTC      = 12,
} OwlTdsEnvChangeType;

/* Resultat de l'analyse de la reponse Login7. */
typedef struct {
    /* Status final : true si LOGINACK + DONE.Status==0, false si ERROR. */
    bool        success;

    /* Si !success, info de l'ERROR token. */
    uint32_t    error_number;
    uint8_t     error_state;
    uint8_t     error_class;
    char        error_message[512];   /* UTF-8 nul-termine */
    char        error_server[128];
    char        error_proc[128];
    uint32_t    error_line;

    /* Si success, infos serveur du LOGINACK. */
    char        server_program[64];
    uint8_t     server_version_major;
    uint8_t     server_version_minor;
    uint16_t    server_version_build;
    uint32_t    tds_version_negotiated;

    /* Si packet size modifie via ENVCHANGE. */
    uint32_t    negotiated_packet_size;
    bool        packet_size_changed;

    /* Premier message INFO recu (pour eventuel diagnostic) — UTF-8. */
    char        info_message[256];
    bool        info_present;
} OwlMssqlLoginResponse;

/* Parse le payload de la reponse Login7 (la zone apres le header TDS
 * du paquet TabularResult). Remplit resp.
 * Renvoie true si la structure tokens est coherente. resp.success doit
 * etre teste pour savoir si le login a effectivement reussi.
 * parse_err : message d'erreur (>= 128 octets) sur false. */
bool owl_mssql_login_parse_response(const uint8_t* payload, size_t len,
                                      OwlMssqlLoginResponse* resp,
                                      char* parse_err);

#endif /* owl_sql_mssql_login_h */
