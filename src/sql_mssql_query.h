// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mssql_query_h
#define owl_sql_mssql_query_h

/* ================================================================
 * MSSQL TDS 7.4 — SQLBatch packet + response stream parser
 *
 * SQLBatch (type TDS 0x01) envoie une requete SQL au serveur. Format :
 *
 *   [ALL_HEADERS]
 *   [SQL Text en UCS-2 LE, sans NUL final]
 *
 * ALL_HEADERS (TDS 7.2+) :
 *   u32 LE TotalLength (inclus lui-meme)
 *   [Header 1] [Header 2] ...
 *
 * Le seul header requis V1 est le Transaction Descriptor Header :
 *   u32 LE Length (= 18)
 *   u16 LE Type   (= 0x0002 = TransactionDescriptor)
 *   u64 LE TransactionDescriptor (= 0 si pas de txn active)
 *   u32 LE OutstandingRequestCount (= 1)
 *
 * Reponse serveur : TabularResult (type 0x04) avec stream de tokens :
 *   COLMETADATA (0x81) : descripteurs colonnes
 *   ROW         (0xD1) : ligne de donnees
 *   NBCROW      (0xD2) : ligne avec null-bitmap compresse
 *   DONE        (0xFD) : fin requete (status, rowcount)
 *   ERROR/INFO  (0xAA/0xAB) : message
 *   ORDER       (0xA9) : ORDER BY hint (skippe)
 *   RETURNSTATUS (0x79) : retour de stored proc (skippe V1)
 *
 * Cette couche est pure encoder/decoder — pas de socket, pas de state
 * machine. Le state d'execution (envoi requete, lecture chunks) sera
 * gere au niveau conn (Lot 6).
 *
 * Reference : MS-TDS 2.2.6.7 (SQLBatch), 2.2.7 (Tokens)
 * ================================================================ */

#include "common.h"
#include "net_le_buffer.h"

/* ================================================================
 * SQLBatch packet construction
 * ================================================================ */

/* Pack une requete SQL en payload SQLBatch (ALL_HEADERS + UCS-2 LE text).
 * sql_utf8 : requete en UTF-8, sera convertie en UCS-2 LE.
 * txn_descriptor : id de transaction active, 0 sinon.
 * out : buffer destination (suppose vide). */
bool owl_mssql_query_pack(OwlLeBuffer* out, const char* sql_utf8,
                            uint64_t txn_descriptor);

/* ================================================================
 * RPC (sp_executesql) pour requetes parametrees
 *
 * Au lieu d'un SQLBatch, on emet un paquet RPC (type TDS 0x03) qui
 * appelle la stored proc systeme sp_executesql avec 3+ parametres :
 *   @stmt   NVARCHAR : le SQL avec @P1, @P2, ... placeholders
 *   @params NVARCHAR : declaration des types, "@P1 INT, @P2 NVARCHAR(50)"
 *   @P1, @P2, ...    : les valeurs effectives
 *
 * Avantages :
 *   - Anti-injection garanti (les valeurs ne sont jamais concatenees au SQL)
 *   - Le serveur peut cacher le plan d'execution (perf si requete repetee)
 *
 * Format MS-TDS 2.2.6.6 (RPCRequest).
 * ================================================================ */

typedef enum {
    OWL_MSSQL_PARAM_NULL,
    OWL_MSSQL_PARAM_INT,           /* i64 -> BIGINT (INTN size 8) */
    OWL_MSSQL_PARAM_REAL,          /* double -> FLOAT (FLTN size 8) */
    OWL_MSSQL_PARAM_BOOL,          /* bool -> BIT (BITN size 1) */
    OWL_MSSQL_PARAM_STRING_UTF8,   /* UTF-8 -> NVARCHAR (UCS-2 LE) */
    OWL_MSSQL_PARAM_BUFFER,        /* raw bytes -> VARBINARY */
} OwlMssqlParamType;

typedef struct {
    const char*       name;        /* sans le @, ex "P1" ou "id" */
    OwlMssqlParamType type;
    int64_t           i_value;
    double            r_value;
    bool              b_value;
    const uint8_t*    bytes;       /* STRING_UTF8: UTF-8 bytes; BUFFER: raw */
    size_t            bytes_len;
} OwlMssqlRpcParam;

/* Pack RPC sp_executesql dans le payload (sans header TDS).
 * sql_utf8 : la requete avec placeholders @P1, @P2... (deja transformes
 *            depuis {nom} cote stdlib_sql).
 * params   : tableau de n_params valeurs, dans l'ordre @P1..@PN.
 *            params[i].name = "P1", "P2", etc. (sans @).
 */
bool owl_mssql_query_pack_rpc(OwlLeBuffer* out,
                                const char* sql_utf8,
                                const OwlMssqlRpcParam* params, size_t n_params,
                                uint64_t txn_descriptor);

/* ================================================================
 * Token codes (extension de OwlTdsToken de login.h)
 * ================================================================ */

/* Categorie de longueur des types TDS — determine comment skip dans ROW.
 * Cf MS-TDS 2.2.5.4 (Type Info Rule). */
typedef enum {
    TDS_LEN_FIXED       = 0,    /* longueur fixe (INT4=4, INT8=8, FLT8=8, BIT=1) */
    TDS_LEN_BYTE        = 1,    /* prefixe u8 (max 255), incluant NULL (=0) */
    TDS_LEN_USHORT      = 2,    /* prefixe u16 LE, NULL = 0xFFFF */
    TDS_LEN_LONG        = 3,    /* prefixe u32 LE, NULL = 0xFFFFFFFF */
    TDS_LEN_PART        = 4,    /* PARTLEN (NVARCHAR(MAX) etc.) — V2 */
} OwlTdsLenCategory;

/* Descripteur d'une colonne extrait du COLMETADATA. */
typedef struct {
    char     name[128];       /* UTF-8, decode depuis UCS-2 */
    uint8_t  type_code;       /* code TDS (0x38=INT4, 0xE7=NVARCHAR, ...) */
    OwlTdsLenCategory len_cat;
    uint32_t fixed_size;      /* utile si TDS_LEN_FIXED */
    uint8_t  precision;       /* DECIMAL/NUMERIC seulement */
    uint8_t  scale;           /* DECIMAL/NUMERIC et TIME/DATETIME2/DATETIMEOFFSET */
    uint8_t  collation[5];    /* texte (BIGCHAR/BIGVARCHAR/NCHAR/NVARCHAR) */
    bool     has_collation;
    bool     nullable;        /* depuis Flags */
} OwlMssqlColumn;

/* Donnee brute d'une cellule de ligne : pointe dans le buffer source.
 * Pour V1 on stocke juste (ptr, len, is_null). L'interpretation valeur
 * -> Value OWL viendra en Lot 5. */
typedef struct {
    const uint8_t* data;   /* pointe dans le buffer payload, NULL si NULL */
    size_t         len;    /* longueur en octets (0 si NULL) */
    bool           is_null;
} OwlMssqlCell;

/* ================================================================
 * Response stream parser
 *
 * On parse incrementalement : chaque appel decode un token (ou indique
 * EOF). Permet un mode streaming si on doit gerer des resultats trop
 * gros pour memoire (V2).
 *
 * Pour V1 on a aussi un mode "parse_full" qui materialise tout en
 * memoire (suffisant pour les usages typiques OWL).
 * ================================================================ */

typedef enum {
    TDS_RESULT_EMPTY      = 0,    /* requete OK, 0 result set (DDL, INSERT...) */
    TDS_RESULT_ROWS       = 1,    /* result set normal avec colonnes + lignes */
    TDS_RESULT_ERROR      = 2,    /* requete a echoue */
} OwlMssqlResultKind;

/* Resultat materialise d'une requete. Tableau de result sets — une
 * requete peut renvoyer plusieurs (e.g. "SELECT ...; SELECT ..."). V1
 * support : 1 set. Surplus = ignore.
 *
 * S'occupe lui-meme de l'allocation/free des tableaux et des chaines.
 */
typedef struct {
    OwlMssqlResultKind kind;

    /* Si ERROR */
    uint32_t error_number;
    uint8_t  error_state;
    uint8_t  error_class;
    char     error_message[512];
    uint32_t error_line;

    /* Si ROWS : colonnes (count + descripteurs) */
    OwlMssqlColumn* columns;
    size_t          column_count;

    /* Cellules : tableau de [row_count * column_count] OwlMssqlCell.
     * Les .data pointent dans `payload_keep` ci-dessous qu'on conserve
     * pour la duree de vie du resultat. */
    OwlMssqlCell* cells;
    size_t        row_count;

    /* Copie du payload depuis lequel cells.data pointent. Free dans _free. */
    uint8_t* payload_keep;
    size_t   payload_keep_len;

    /* DONE infos */
    uint64_t rows_affected;
    bool     done_received;
} OwlMssqlResult;

void owl_mssql_result_init(OwlMssqlResult* r);
void owl_mssql_result_free(OwlMssqlResult* r);

/* Parse un payload complet (deja reassemble depuis les paquets TDS) en
 * un OwlMssqlResult. payload est duplique en interne (r->payload_keep).
 *
 * Renvoie true si parse OK. r->kind indique le type de resultat.
 * parse_err (>=128) : erreur si false retourne. */
bool owl_mssql_query_parse_response(const uint8_t* payload, size_t len,
                                      OwlMssqlResult* r,
                                      char* parse_err);

#endif /* owl_sql_mssql_query_h */
