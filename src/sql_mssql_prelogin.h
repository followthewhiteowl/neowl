// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_prelogin_h
#define owl_sql_mssql_prelogin_h

/* ================================================================
 * MSSQL TDS 7.4 — PreLogin packet
 *
 * Premier echange client<->serveur, hors TLS, qui sert a :
 *   - Annoncer la version client (VERSION token)
 *   - Negocier le mode chiffrement (ENCRYPTION token)
 *   - Identifier l'instance (INSTOPT, ex: "MSSQLSERVER")
 *   - Transmettre divers metadata (THREADID, MARS, TRACEID...)
 *
 * Format payload (apres l'octet de type TDS 0x12) :
 *
 *   [token_desc 1] ... [token_desc N] [terminator 0xFF]
 *   [token_data 1] ... [token_data N]
 *
 * Chaque token_desc fait 5 octets :
 *   TokenType    u8       (0x00=VERSION, 0x01=ENCRYPTION, ...)
 *   TokenPosition u16 BE  (offset depuis le debut du payload)
 *   TokenLength   u16 BE  (taille de la donnee)
 *
 * Les offsets sont relatifs au DEBUT DU PAYLOAD PreLogin (apres header
 * TDS, donc apres 8 octets de wrapper). On les calcule a la fin une
 * fois qu'on connait la longueur de la zone descripteurs.
 *
 * Reference : MS-TDS section 2.2.6.5
 * ================================================================ */

#include "common.h"
#include "net_le_buffer.h"

/* ================================================================
 * Tokens PreLogin (MS-TDS 2.2.6.5)
 * ================================================================ */

typedef enum {
    TDS_PRELOGIN_VERSION         = 0x00,
    TDS_PRELOGIN_ENCRYPTION      = 0x01,
    TDS_PRELOGIN_INSTOPT         = 0x02,   /* nom de l'instance, ex "MSSQLSERVER" */
    TDS_PRELOGIN_THREADID        = 0x03,
    TDS_PRELOGIN_MARS            = 0x04,   /* Multiple Active Result Sets */
    TDS_PRELOGIN_TRACEID         = 0x05,
    TDS_PRELOGIN_FEDAUTHREQUIRED = 0x06,
    TDS_PRELOGIN_NONCEOPT        = 0x07,
    TDS_PRELOGIN_TERMINATOR      = 0xFF,
} OwlTdsPreloginToken;

/* Modes de chiffrement (1 octet dans le token ENCRYPTION). MS-TDS 2.2.6.5 */
typedef enum {
    TDS_ENCRYPT_OFF      = 0x00,  /* TLS pendant login seul, plain ensuite */
    TDS_ENCRYPT_ON       = 0x01,  /* TLS pour toute la session (recommande) */
    TDS_ENCRYPT_NOT_SUP  = 0x02,  /* pas supporte */
    TDS_ENCRYPT_REQ      = 0x03,  /* serveur exige TLS */
    TDS_ENCRYPT_CLI_CERT = 0x04,  /* TDS 8.0+ : cert client requis */
} OwlTdsEncryptionMode;

/* ================================================================
 * Construction d'un PreLogin client
 * ================================================================ */

/* Parametres minimaux pour construire un PreLogin client.
 *
 * version_* : annonces au serveur. On met une version coherente OWL
 *             (visible cote serveur dans sys.dm_exec_connections).
 * encryption: mode demande. ENCRYPT_ON = full TLS (recommande).
 * instance  : nom d'instance ("MSSQLSERVER", "SQLEXPRESS", ou "" pour default).
 * thread_id : identifiant client (info, peut etre 0).
 * mars      : true = active MARS (V1 OWL = false). */
typedef struct {
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t version_build;
    uint16_t version_subbuild;
    OwlTdsEncryptionMode encryption;
    const char* instance;   /* NULL ou "" = pas de INSTOPT envoye */
    uint32_t thread_id;
    bool     mars;
} OwlTdsPreloginRequest;

/* Construit le payload PreLogin dans out (vide avant). Renvoie true si OK.
 * Le payload obtenu doit ensuite etre encapsule dans un paquet TDS 0x12. */
bool owl_tds_prelogin_pack(OwlLeBuffer* out, const OwlTdsPreloginRequest* req);

/* ================================================================
 * Parsing du PreLogin serveur
 * ================================================================ */

/* Champs typiquement renvoyes par le serveur (subset suffisant pour V1).
 * Si un champ optionnel est absent dans la reponse, son flag _present est
 * a false. */
typedef struct {
    /* VERSION (toujours present cote serveur). */
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t version_build;
    uint16_t version_subbuild;
    bool     version_present;

    /* ENCRYPTION : mode negocie par le serveur. */
    OwlTdsEncryptionMode encryption;
    bool                 encryption_present;

    /* INSTOPT : echo du nom d'instance (V1 on ignore le contenu). */
    bool instance_present;

    /* MARS : MARS active ou non par le serveur. */
    bool mars_value;
    bool mars_present;

    /* FEDAUTHREQUIRED : auth federee requise (V1 = on echoue si true). */
    bool fedauth_required;
    bool fedauth_present;
} OwlTdsPreloginResponse;

/* Parse le payload PreLogin recu (sans l'enveloppe TDS, donc directement
 * la zone option-list + data). Remplit resp. Renvoie true si la structure
 * est coherente (descripteurs, offsets, terminator).
 *
 * out_err : message d'erreur (>=128 octets) en cas d'echec. */
bool owl_tds_prelogin_parse(const uint8_t* payload, size_t payload_len,
                              OwlTdsPreloginResponse* resp,
                              char* out_err);

#endif /* owl_sql_mssql_prelogin_h */
