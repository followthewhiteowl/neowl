// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_query.h"
#include "sql_mssql_login.h"   /* OwlTdsToken enum + obfuscate (pour test) */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * UTF-8 -> UCS-2 LE (idem login.c, helper privé local)
 * ================================================================ */

static void utf8_to_ucs2le_write(OwlLeBuffer* b, const char* utf8) {
    if (!utf8) return;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (uint32_t)(*p++ & 0x1F) << 6;
            cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (uint32_t)(*p++ & 0x0F) << 12;
            cp |= (uint32_t)(*p++ & 0x3F) << 6;
            cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF8) == 0xF0) {
            p += 4;
            cp = '?';
        } else {
            p++;
            cp = '?';
        }
        if (cp > 0xFFFF) cp = '?';
        owl_le_buffer_write_u16(b, (uint16_t)cp);
    }
}

static void ucs2le_to_utf8_str(const uint8_t* src, size_t n_units,
                                 char* out, size_t out_cap) {
    if (out_cap == 0) return;
    size_t o = 0;
    for (size_t i = 0; i < n_units && o + 4 < out_cap; ++i) {
        uint16_t cp = (uint16_t)(src[i*2] | (src[i*2+1] << 8));
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o < out_cap ? o : out_cap - 1] = '\0';
}

/* ================================================================
 * SQLBatch pack
 * ================================================================ */

bool owl_mssql_query_pack(OwlLeBuffer* out, const char* sql_utf8,
                            uint64_t txn_descriptor) {
    if (!out || !sql_utf8) return false;
    if (out->length != 0) return false;

    /* ALL_HEADERS :
     *   TotalLength u32 LE = 22 (4 self + 18 TransactionDesc header)
     *   Header 1 (Transaction Descriptor) :
     *     Length u32 LE = 18 (= self total inc. these 4 + 2 + 8 + 4)
     *     Type   u16 LE = 0x0002
     *     Data : u64 txn_descriptor + u32 outstanding_request_count
     */
    owl_le_buffer_write_u32(out, 22);
    owl_le_buffer_write_u32(out, 18);
    owl_le_buffer_write_u16(out, 0x0002);
    owl_le_buffer_write_u64(out, txn_descriptor);
    owl_le_buffer_write_u32(out, 1);   /* outstanding request count */

    /* SQL text en UCS-2 LE (sans NUL final). */
    utf8_to_ucs2le_write(out, sql_utf8);
    return true;
}

/* ================================================================
 * RPC sp_executesql — pack
 *
 * Format MS-TDS 2.2.6.6 :
 *
 *   ALL_HEADERS (22 octets, idem SQLBatch)
 *
 *   ProcID :
 *     soit u16 LE NameLen + Name UCS-2 LE
 *     soit u16 LE 0xFFFF + u16 LE procID (12 = sp_executesql)
 *   On utilise la forme courte (0xFFFF + 12).
 *
 *   OptionFlags u16 LE = 0
 *
 *   Pour chaque parametre :
 *     ParamNameLen u8 (chars, incluant '@')
 *     ParamName UCS-2 LE
 *     StatusFlags u8 = 0
 *     TypeInfo (selon type — voir owl_mssql_query.h commentaires)
 *     ValueData (length prefix + raw bytes)
 *
 * On emet toujours :
 *   @stmt   = NVARCHAR(MAX-ish) avec le SQL                       (param 1)
 *   @params = NVARCHAR(MAX-ish) avec les declarations            (param 2)
 *   @P1, @P2, ...                                                 (params 3+)
 *
 * NVARCHAR(MAX) utilise PARTLEN qui n'est pas trivial. Pour V1 on
 * limite stmt + params a 4000 chars chacun (NVARCHAR(4000) = max
 * pour USHORTLEN). Au-dela on echoue avec un message clair.
 * ================================================================ */

#define SP_EXECUTESQL_PROCID 10   /* MS-TDS Table de procs : 10 = sp_executesql */

static void rpc_write_param_name(OwlLeBuffer* b, const char* name) {
    /* "@" + name en UCS-2 LE. */
    size_t name_chars = 1 + strlen(name);   /* '@' + name */
    owl_le_buffer_write_u8(b, (uint8_t)name_chars);
    owl_le_buffer_write_u8(b, '@'); owl_le_buffer_write_u8(b, 0);
    for (const char* p = name; *p; ++p) {
        owl_le_buffer_write_u8(b, (uint8_t)*p);
        owl_le_buffer_write_u8(b, 0);
    }
}

/* Encode un parametre NVARCHAR (UCS-2 LE) avec max_len max chars.
 * Si value_chars = 0 ET is_null = true on emet NULL (USHORTLEN 0xFFFF). */
static void rpc_write_nvarchar_param(OwlLeBuffer* b,
                                       const char* name,
                                       const char* utf8_value, size_t utf8_len,
                                       bool is_null,
                                       size_t max_chars) {
    rpc_write_param_name(b, name);
    owl_le_buffer_write_u8(b, 0);            /* StatusFlags */
    owl_le_buffer_write_u8(b, 0xE7);          /* NVARCHARTYPE */
    /* max_len en OCTETS (UCS-2 = 2 octets / char). Max 8000 octets pour
     * NVARCHAR sans MAX. */
    uint16_t max_bytes = (uint16_t)(max_chars * 2);
    if (max_bytes > 8000) max_bytes = 8000;
    owl_le_buffer_write_u16(b, max_bytes);
    /* Collation : 5 octets, 0 = serveur default. */
    for (int i = 0; i < 5; ++i) owl_le_buffer_write_u8(b, 0);
    /* Value : u16 LE length en octets, ou 0xFFFF si NULL. */
    if (is_null) {
        owl_le_buffer_write_u16(b, 0xFFFF);
    } else {
        /* Pre-calcule la longueur UCS-2 en chars puis emet bytes. */
        size_t cap = utf8_len + 1;
        uint16_t* tmp = (uint16_t*)malloc(cap * sizeof(uint16_t));
        size_t n_units = 0;
        if (tmp) {
            const unsigned char* p = (const unsigned char*)utf8_value;
            size_t consumed = 0;
            while (consumed < utf8_len && n_units < cap) {
                uint32_t cp;
                if (p[consumed] < 0x80) {
                    cp = p[consumed]; consumed += 1;
                } else if ((p[consumed] & 0xE0) == 0xC0 && consumed + 1 < utf8_len) {
                    cp = (uint32_t)(p[consumed] & 0x1F) << 6;
                    cp |= (p[consumed+1] & 0x3F);
                    consumed += 2;
                } else if ((p[consumed] & 0xF0) == 0xE0 && consumed + 2 < utf8_len) {
                    cp = (uint32_t)(p[consumed] & 0x0F) << 12;
                    cp |= (uint32_t)(p[consumed+1] & 0x3F) << 6;
                    cp |= (p[consumed+2] & 0x3F);
                    consumed += 3;
                } else if ((p[consumed] & 0xF8) == 0xF0 && consumed + 3 < utf8_len) {
                    consumed += 4;
                    cp = '?';
                } else {
                    consumed += 1;
                    cp = '?';
                }
                if (cp > 0xFFFF) cp = '?';
                tmp[n_units++] = (uint16_t)cp;
            }
        }
        uint16_t value_bytes = (uint16_t)(n_units * 2);
        owl_le_buffer_write_u16(b, value_bytes);
        for (size_t i = 0; i < n_units; ++i) {
            owl_le_buffer_write_u16(b, tmp[i]);
        }
        free(tmp);
    }
}

/* Encode un INT param : INTNTYPE avec max_len=8 (BIGINT). */
static void rpc_write_int_param(OwlLeBuffer* b, const char* name,
                                  int64_t value, bool is_null) {
    rpc_write_param_name(b, name);
    owl_le_buffer_write_u8(b, 0);            /* StatusFlags */
    owl_le_buffer_write_u8(b, 0x26);          /* INTNTYPE */
    owl_le_buffer_write_u8(b, 0x08);          /* max_len = 8 */
    if (is_null) {
        owl_le_buffer_write_u8(b, 0);         /* actual len = 0 -> NULL */
    } else {
        owl_le_buffer_write_u8(b, 0x08);
        owl_le_buffer_write_i64(b, value);
    }
}

/* Encode un REAL (FLOAT 8 octets) : FLTNTYPE max_len=8. */
static void rpc_write_real_param(OwlLeBuffer* b, const char* name,
                                   double value, bool is_null) {
    rpc_write_param_name(b, name);
    owl_le_buffer_write_u8(b, 0);
    owl_le_buffer_write_u8(b, 0x6D);          /* FLTN */
    owl_le_buffer_write_u8(b, 0x08);
    if (is_null) {
        owl_le_buffer_write_u8(b, 0);
    } else {
        owl_le_buffer_write_u8(b, 0x08);
        uint64_t bits;
        memcpy(&bits, &value, 8);
        owl_le_buffer_write_u64(b, bits);
    }
}

/* Encode un BOOL : BITNTYPE max_len=1. */
static void rpc_write_bool_param(OwlLeBuffer* b, const char* name,
                                   bool value, bool is_null) {
    rpc_write_param_name(b, name);
    owl_le_buffer_write_u8(b, 0);
    owl_le_buffer_write_u8(b, 0x68);          /* BITN */
    owl_le_buffer_write_u8(b, 0x01);
    if (is_null) {
        owl_le_buffer_write_u8(b, 0);
    } else {
        owl_le_buffer_write_u8(b, 0x01);
        owl_le_buffer_write_u8(b, value ? 1 : 0);
    }
}

/* Encode un VARBINARY : BIGVARBINTYPE max_len jusqu'a 8000. */
static void rpc_write_binary_param(OwlLeBuffer* b, const char* name,
                                     const uint8_t* data, size_t len,
                                     bool is_null) {
    rpc_write_param_name(b, name);
    owl_le_buffer_write_u8(b, 0);
    owl_le_buffer_write_u8(b, 0xA5);          /* BIGVARBINTYPE */
    uint16_t max_bytes = (len > 8000) ? 8000 : (uint16_t)len;
    if (max_bytes < 1) max_bytes = 1;
    owl_le_buffer_write_u16(b, max_bytes);
    if (is_null) {
        owl_le_buffer_write_u16(b, 0xFFFF);
    } else {
        uint16_t l = (len > 8000) ? 8000 : (uint16_t)len;
        owl_le_buffer_write_u16(b, l);
        owl_le_buffer_write_bytes(b, data, l);
    }
}

/* Construit la chaine "@P1 BIGINT, @P2 NVARCHAR(50), @P3 BIT" comme
 * declaration de types pour sp_executesql. */
static char* build_params_decl(const OwlMssqlRpcParam* params, size_t n) {
    /* Heuristique : chaque param prend max ~64 chars dans la decl. */
    size_t cap = n * 64 + 16;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < n; ++i) {
        if (off + 80 > cap) {
            cap *= 2;
            char* tmp = (char*)realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        const char* sep = (i > 0) ? ", " : "";
        const char* type_str;
        char tmpbuf[40];
        switch (params[i].type) {
            case OWL_MSSQL_PARAM_NULL:
                /* NULL par defaut -> declare comme NVARCHAR(1). */
                type_str = "NVARCHAR(1)";
                break;
            case OWL_MSSQL_PARAM_INT:
                type_str = "BIGINT";
                break;
            case OWL_MSSQL_PARAM_REAL:
                type_str = "FLOAT";
                break;
            case OWL_MSSQL_PARAM_BOOL:
                type_str = "BIT";
                break;
            case OWL_MSSQL_PARAM_STRING_UTF8: {
                /* On dimensionne la decl a (chars * 2) octets avec un
                 * minimum de 1. NVARCHAR(N) accepte N en chars cote SQL. */
                size_t chars = params[i].bytes_len;  /* approximation : 1 byte / char */
                if (chars < 1) chars = 1;
                if (chars > 4000) chars = 4000;
                snprintf(tmpbuf, sizeof(tmpbuf), "NVARCHAR(%zu)", chars);
                type_str = tmpbuf;
                break;
            }
            case OWL_MSSQL_PARAM_BUFFER: {
                size_t l = params[i].bytes_len;
                if (l < 1) l = 1;
                if (l > 8000) l = 8000;
                snprintf(tmpbuf, sizeof(tmpbuf), "VARBINARY(%zu)", l);
                type_str = tmpbuf;
                break;
            }
            default:
                type_str = "NVARCHAR(1)";
        }
        off += (size_t)snprintf(out + off, cap - off,
                                  "%s@%s %s", sep, params[i].name, type_str);
    }
    return out;
}

bool owl_mssql_query_pack_rpc(OwlLeBuffer* out,
                                const char* sql_utf8,
                                const OwlMssqlRpcParam* params, size_t n_params,
                                uint64_t txn_descriptor) {
    if (!out || !sql_utf8) return false;
    if (out->length != 0) return false;

    /* ALL_HEADERS (idem SQLBatch). */
    owl_le_buffer_write_u32(out, 22);
    owl_le_buffer_write_u32(out, 18);
    owl_le_buffer_write_u16(out, 0x0002);
    owl_le_buffer_write_u64(out, txn_descriptor);
    owl_le_buffer_write_u32(out, 1);

    /* ProcID : forme courte (0xFFFF + procID). */
    owl_le_buffer_write_u16(out, 0xFFFF);
    owl_le_buffer_write_u16(out, SP_EXECUTESQL_PROCID);
    owl_le_buffer_write_u16(out, 0);  /* OptionFlags */

    /* @stmt : NVARCHAR (UTF-8 -> UCS-2 LE), max 4000 chars V1. */
    size_t sql_len = strlen(sql_utf8);
    rpc_write_nvarchar_param(out, "stmt", sql_utf8, sql_len, false, 4000);

    /* @params : NVARCHAR avec declarations "@P1 BIGINT, ...". Si pas de
     * params, on emet @params = NULL. */
    if (n_params == 0) {
        rpc_write_nvarchar_param(out, "params", NULL, 0, true, 1);
    } else {
        char* decl = build_params_decl(params, n_params);
        if (!decl) return false;
        rpc_write_nvarchar_param(out, "params", decl, strlen(decl), false, 4000);
        free(decl);
    }

    /* Values @P1, @P2, ... */
    for (size_t i = 0; i < n_params; ++i) {
        const OwlMssqlRpcParam* p = &params[i];
        switch (p->type) {
            case OWL_MSSQL_PARAM_NULL:
                rpc_write_nvarchar_param(out, p->name, NULL, 0, true, 1);
                break;
            case OWL_MSSQL_PARAM_INT:
                rpc_write_int_param(out, p->name, p->i_value, false);
                break;
            case OWL_MSSQL_PARAM_REAL:
                rpc_write_real_param(out, p->name, p->r_value, false);
                break;
            case OWL_MSSQL_PARAM_BOOL:
                rpc_write_bool_param(out, p->name, p->b_value, false);
                break;
            case OWL_MSSQL_PARAM_STRING_UTF8:
                rpc_write_nvarchar_param(out, p->name,
                                           (const char*)p->bytes, p->bytes_len,
                                           false, 4000);
                break;
            case OWL_MSSQL_PARAM_BUFFER:
                rpc_write_binary_param(out, p->name, p->bytes, p->bytes_len, false);
                break;
        }
    }
    return true;
}

/* ================================================================
 * Catalogue des types TDS V1
 *
 * Mapping (type_code, len_category, fixed_size). On supporte la
 * majorite des types courants ; les autres provoquent une erreur de
 * parse propre.
 *
 * Pour DECIMAL/NUMERIC (BYTELEN avec precision+scale), TIME/DATETIME2/
 * DATETIMEOFFSET (BYTELEN avec scale), DECIMALN (BYTELEN avec
 * precision+scale), on lit les metadonnees supplementaires dans
 * decode_typeinfo.
 *
 * Pour BIGCHAR/BIGVARCHAR/NCHAR/NVARCHAR (USHORTLEN), on lit en plus
 * une "collation" de 5 octets.
 * ================================================================ */

typedef struct {
    uint8_t code;
    OwlTdsLenCategory cat;
    uint32_t fixed_size;
    bool needs_prec_scale;     /* DECIMAL/NUMERIC */
    bool needs_scale_only;     /* TIME, DATETIME2, DATETIMEOFFSET */
    bool needs_collation;      /* texte avec collation */
} TdsTypeSpec;

static const TdsTypeSpec TYPE_TABLE[] = {
    /* Fixed-length */
    { 0x30, TDS_LEN_FIXED, 1, false, false, false },  /* INT1 (TINYINT) */
    { 0x32, TDS_LEN_FIXED, 1, false, false, false },  /* BIT */
    { 0x34, TDS_LEN_FIXED, 2, false, false, false },  /* INT2 (SMALLINT) */
    { 0x38, TDS_LEN_FIXED, 4, false, false, false },  /* INT4 (INT) */
    { 0x7F, TDS_LEN_FIXED, 8, false, false, false },  /* INT8 (BIGINT) */
    { 0x3A, TDS_LEN_FIXED, 4, false, false, false },  /* DATETIM4 (SMALLDATETIME) */
    { 0x3B, TDS_LEN_FIXED, 4, false, false, false },  /* FLT4 (REAL) */
    { 0x3D, TDS_LEN_FIXED, 8, false, false, false },  /* DATETIME8 */
    { 0x3E, TDS_LEN_FIXED, 8, false, false, false },  /* FLT8 (FLOAT) */
    { 0x7A, TDS_LEN_FIXED, 4, false, false, false },  /* MONEY4 (SMALLMONEY) */
    { 0x3C, TDS_LEN_FIXED, 8, false, false, false },  /* MONEY */

    /* BYTELEN (length u8) */
    { 0x24, TDS_LEN_BYTE, 0, false, false, false },   /* GUID (UNIQUEIDENTIFIER) */
    { 0x26, TDS_LEN_BYTE, 0, false, false, false },   /* INTN — taille declaree dynamiquement */
    { 0x68, TDS_LEN_BYTE, 0, false, false, false },   /* BITN */
    { 0x6A, TDS_LEN_BYTE, 0, true,  false, false },   /* DECIMALN (legacy) */
    { 0x6C, TDS_LEN_BYTE, 0, true,  false, false },   /* NUMERICN */
    { 0x6D, TDS_LEN_BYTE, 0, false, false, false },   /* FLTN */
    { 0x6E, TDS_LEN_BYTE, 0, false, false, false },   /* MONEYN */
    { 0x6F, TDS_LEN_BYTE, 0, false, false, false },   /* DATETIMN */
    { 0x28, TDS_LEN_BYTE, 0, false, false, false },   /* DATEN (TDS 7.3+) */
    { 0x29, TDS_LEN_BYTE, 0, false, true,  false },   /* TIMEN (scale) */
    { 0x2A, TDS_LEN_BYTE, 0, false, true,  false },   /* DATETIME2N (scale) */
    { 0x2B, TDS_LEN_BYTE, 0, false, true,  false },   /* DATETIMEOFFSETN (scale) */

    /* USHORTLEN (length u16, max 8000) */
    { 0xA5, TDS_LEN_USHORT, 0, false, false, false }, /* BIGVARBINARY */
    { 0xAD, TDS_LEN_USHORT, 0, false, false, false }, /* BIGBINARY */
    { 0xA7, TDS_LEN_USHORT, 0, false, false, true  }, /* BIGVARCHAR */
    { 0xAF, TDS_LEN_USHORT, 0, false, false, true  }, /* BIGCHAR */
    { 0xE7, TDS_LEN_USHORT, 0, false, false, true  }, /* NVARCHAR */
    { 0xEF, TDS_LEN_USHORT, 0, false, false, true  }, /* NCHAR */

    /* LONGLEN (length u32) — V2 (TEXT/NTEXT/IMAGE — streamed). On expose
     * la categorie mais on echoue si on tombe dessus en parse de ROW. */
    { 0x22, TDS_LEN_LONG, 0, false, false, false },   /* IMAGE */
    { 0x23, TDS_LEN_LONG, 0, false, false, true  },   /* TEXT */
    { 0x63, TDS_LEN_LONG, 0, false, false, true  },   /* NTEXT */
};
#define TYPE_TABLE_SIZE (sizeof(TYPE_TABLE) / sizeof(TYPE_TABLE[0]))

static const TdsTypeSpec* lookup_type(uint8_t code) {
    for (size_t i = 0; i < TYPE_TABLE_SIZE; ++i) {
        if (TYPE_TABLE[i].code == code) return &TYPE_TABLE[i];
    }
    return NULL;
}

/* ================================================================
 * COLMETADATA : decode type info pour une colonne
 *
 * Format dans le stream (apres token byte + count) :
 *
 *   Per col :
 *     UserType  u32 LE  (TDS 7.2+, sinon u16)
 *     Flags     u16 LE
 *     TypeInfo  : voir Type Info Rule MS-TDS 2.2.5.4
 *       Type byte
 *       BYTELEN/USHORTLEN/LONGLEN : taille max declaree (u8/u16/u32)
 *         FIXEDLEN : rien
 *       DECIMAL/NUMERIC : Precision u8, Scale u8
 *       TIME/DATETIME2/DATETIMEOFFSET : Scale u8 (avant la length)
 *       BIGCHAR/VARCHAR/NCHAR/NVARCHAR + TEXT/NTEXT : Collation 5 octets
 *     [TableName u8 + UCS-2] : seulement TEXT/NTEXT/IMAGE
 *     ColName u8 (chars) + UCS-2 LE
 * ================================================================ */

static bool parse_one_column(OwlLeBuffer* b, OwlMssqlColumn* col, char* err) {
    /* UserType u32 + Flags u16 (TDS 7.2+) */
    uint32_t user_type;
    uint16_t flags;
    if (!owl_le_buffer_read_u32(b, &user_type) ||
        !owl_le_buffer_read_u16(b, &flags)) {
        if (err) snprintf(err, 128, "colmetadata: usertype/flags tronques");
        return false;
    }
    col->nullable = (flags & 0x01) != 0;

    /* Type byte */
    uint8_t type_byte;
    if (!owl_le_buffer_read_u8(b, &type_byte)) {
        if (err) snprintf(err, 128, "colmetadata: type byte absent");
        return false;
    }
    col->type_code = type_byte;

    const TdsTypeSpec* spec = lookup_type(type_byte);
    if (!spec) {
        if (err) snprintf(err, 128, "type TDS 0x%02X non supporté", type_byte);
        return false;
    }
    col->len_cat = spec->cat;
    col->fixed_size = spec->fixed_size;

    /* Pour TIME/DATETIME2/DATETIMEOFFSET : Scale u8 AVANT la length max. */
    if (spec->needs_scale_only) {
        if (!owl_le_buffer_read_u8(b, &col->scale)) {
            if (err) snprintf(err, 128, "colmetadata: scale absente (type 0x%02X)", type_byte);
            return false;
        }
    }

    /* Length declaree selon categorie. */
    switch (spec->cat) {
        case TDS_LEN_FIXED:
            /* rien */
            break;
        case TDS_LEN_BYTE: {
            uint8_t max_len;
            if (!owl_le_buffer_read_u8(b, &max_len)) {
                if (err) snprintf(err, 128, "colmetadata: max_len u8 absent");
                return false;
            }
            col->fixed_size = max_len;   /* utile pour DATEN qui est BYTELEN avec taille 3 */
            break;
        }
        case TDS_LEN_USHORT: {
            uint16_t max_len;
            if (!owl_le_buffer_read_u16(b, &max_len)) {
                if (err) snprintf(err, 128, "colmetadata: max_len u16 absent");
                return false;
            }
            (void)max_len;
            break;
        }
        case TDS_LEN_LONG: {
            uint32_t max_len;
            if (!owl_le_buffer_read_u32(b, &max_len)) {
                if (err) snprintf(err, 128, "colmetadata: max_len u32 absent");
                return false;
            }
            (void)max_len;
            break;
        }
        case TDS_LEN_PART:
            if (err) snprintf(err, 128, "TDS_LEN_PART non supporté");
            return false;
    }

    /* DECIMAL/NUMERIC : precision + scale apres la length. */
    if (spec->needs_prec_scale) {
        if (!owl_le_buffer_read_u8(b, &col->precision) ||
            !owl_le_buffer_read_u8(b, &col->scale)) {
            if (err) snprintf(err, 128, "colmetadata: prec/scale absent");
            return false;
        }
    }

    /* Collation (5 octets) pour les texte avec collation. */
    if (spec->needs_collation) {
        if (!owl_le_buffer_read_bytes(b, col->collation, 5)) {
            if (err) snprintf(err, 128, "colmetadata: collation absente");
            return false;
        }
        col->has_collation = true;
    }

    /* TableName : seulement pour TEXT/NTEXT/IMAGE. Format : u16 LE count
     * + UCS-2 LE. On skip. */
    if (type_byte == 0x22 || type_byte == 0x23 || type_byte == 0x63) {
        uint16_t tn_parts;
        if (!owl_le_buffer_read_u16(b, &tn_parts)) {
            if (err) snprintf(err, 128, "colmetadata: tablename parts");
            return false;
        }
        for (uint16_t i = 0; i < tn_parts; ++i) {
            uint16_t part_len;
            if (!owl_le_buffer_read_u16(b, &part_len)) {
                if (err) snprintf(err, 128, "colmetadata: tablename part");
                return false;
            }
            if (!owl_le_buffer_read_skip(b, (size_t)part_len * 2)) {
                if (err) snprintf(err, 128, "colmetadata: tablename skip");
                return false;
            }
        }
    }

    /* ColName : u8 (chars) + UCS-2 LE. */
    uint8_t name_chars;
    if (!owl_le_buffer_read_u8(b, &name_chars)) {
        if (err) snprintf(err, 128, "colmetadata: name length absente");
        return false;
    }
    if (owl_le_buffer_remaining(b) < (size_t)name_chars * 2) {
        if (err) snprintf(err, 128, "colmetadata: name UCS-2 tronque");
        return false;
    }
    ucs2le_to_utf8_str(b->data + b->pos, name_chars, col->name, sizeof(col->name));
    owl_le_buffer_read_skip(b, (size_t)name_chars * 2);

    return true;
}

/* ================================================================
 * ROW / NBCROW : extraction de cellules brutes
 *
 * Pour chaque colonne, on lit :
 *   FIXED  : fixed_size octets bruts (pas de NULL).
 *   BYTE   : u8 len ; len=0 => NULL ; sinon len octets.
 *            (Cas special : DATEN a longueur 3 ou 0)
 *   USHORT : u16 len ; len=0xFFFF => NULL ; sinon len octets.
 *   LONG   : u32 len ; len=0xFFFFFFFF => NULL ; sinon len octets (TEXT V2).
 * ================================================================ */

static bool parse_one_cell(OwlLeBuffer* b, const OwlMssqlColumn* col,
                            OwlMssqlCell* cell, char* err) {
    cell->data = NULL;
    cell->len = 0;
    cell->is_null = false;

    switch (col->len_cat) {
        case TDS_LEN_FIXED:
            if (owl_le_buffer_remaining(b) < col->fixed_size) {
                if (err) snprintf(err, 128, "row: fixed data tronque (col %s)", col->name);
                return false;
            }
            cell->data = b->data + b->pos;
            cell->len  = col->fixed_size;
            owl_le_buffer_read_skip(b, col->fixed_size);
            break;
        case TDS_LEN_BYTE: {
            uint8_t l;
            if (!owl_le_buffer_read_u8(b, &l)) {
                if (err) snprintf(err, 128, "row: bytelen absente (col %s)", col->name);
                return false;
            }
            if (l == 0) {
                cell->is_null = true;
            } else {
                if (owl_le_buffer_remaining(b) < l) {
                    if (err) snprintf(err, 128, "row: bytelen data tronque");
                    return false;
                }
                cell->data = b->data + b->pos;
                cell->len  = l;
                owl_le_buffer_read_skip(b, l);
            }
            break;
        }
        case TDS_LEN_USHORT: {
            uint16_t l;
            if (!owl_le_buffer_read_u16(b, &l)) {
                if (err) snprintf(err, 128, "row: ushortlen absente");
                return false;
            }
            if (l == 0xFFFF) {
                cell->is_null = true;
            } else {
                if (owl_le_buffer_remaining(b) < l) {
                    if (err) snprintf(err, 128, "row: ushortlen data tronque");
                    return false;
                }
                cell->data = b->data + b->pos;
                cell->len  = l;
                owl_le_buffer_read_skip(b, l);
            }
            break;
        }
        case TDS_LEN_LONG:
            if (err) snprintf(err, 128, "TEXT/NTEXT/IMAGE non supporté");
            return false;
        case TDS_LEN_PART:
            if (err) snprintf(err, 128, "PARTLEN non supporté");
            return false;
    }
    return true;
}

/* ================================================================
 * ERROR/INFO/DONE — parse simplifie similaire a login.c
 *
 * (Aurait pu etre factorise dans un fichier "sql_mssql_tokens". V2.)
 * ================================================================ */

static bool parse_error_info_skip(OwlLeBuffer* b, OwlMssqlResult* r,
                                    bool is_error, char* err) {
    uint16_t length;
    if (!owl_le_buffer_read_u16(b, &length)) {
        if (err) snprintf(err, 128, "%s length manquant", is_error ? "ERROR" : "INFO");
        return false;
    }
    if (length > owl_le_buffer_remaining(b)) {
        if (err) snprintf(err, 128, "%s tronque", is_error ? "ERROR" : "INFO");
        return false;
    }
    uint32_t num; uint8_t state; uint8_t class_;
    uint16_t msglen;
    if (!owl_le_buffer_read_u32(b, &num)
     || !owl_le_buffer_read_u8(b, &state)
     || !owl_le_buffer_read_u8(b, &class_)
     || !owl_le_buffer_read_u16(b, &msglen)) {
        if (err) snprintf(err, 128, "ERROR/INFO header lecture");
        return false;
    }
    const uint8_t* msg = b->data + b->pos;
    if (owl_le_buffer_remaining(b) < (size_t)msglen * 2) {
        if (err) snprintf(err, 128, "ERROR/INFO msg tronque");
        return false;
    }
    owl_le_buffer_read_skip(b, (size_t)msglen * 2);

    uint8_t srv_len, proc_len;
    if (!owl_le_buffer_read_u8(b, &srv_len)) return false;
    if (!owl_le_buffer_read_skip(b, (size_t)srv_len * 2)) return false;
    if (!owl_le_buffer_read_u8(b, &proc_len)) return false;
    if (!owl_le_buffer_read_skip(b, (size_t)proc_len * 2)) return false;
    uint32_t line_no;
    if (!owl_le_buffer_read_u32(b, &line_no)) return false;

    if (is_error) {
        r->error_number = num;
        r->error_state  = state;
        r->error_class  = class_;
        r->error_line   = line_no;
        ucs2le_to_utf8_str(msg, msglen, r->error_message, sizeof(r->error_message));
        r->kind = TDS_RESULT_ERROR;
    }
    return true;
}

/* Skip un token ENVCHANGE (length-prefixed). */
static bool skip_envchange(OwlLeBuffer* b, char* err) {
    uint16_t length;
    if (!owl_le_buffer_read_u16(b, &length)) {
        if (err) snprintf(err, 128, "ENVCHANGE length");
        return false;
    }
    if (length > owl_le_buffer_remaining(b)) {
        if (err) snprintf(err, 128, "ENVCHANGE tronque");
        return false;
    }
    return owl_le_buffer_read_skip(b, length);
}

/* Skip ORDER token (u16 length + bytes). */
static bool skip_order(OwlLeBuffer* b, char* err) {
    uint16_t length;
    if (!owl_le_buffer_read_u16(b, &length)) {
        if (err) snprintf(err, 128, "ORDER length");
        return false;
    }
    return owl_le_buffer_read_skip(b, length);
}

/* ================================================================
 * Init / free
 * ================================================================ */

void owl_mssql_result_init(OwlMssqlResult* r) {
    memset(r, 0, sizeof(*r));
    r->kind = TDS_RESULT_EMPTY;
}

void owl_mssql_result_free(OwlMssqlResult* r) {
    free(r->columns);
    free(r->cells);
    free(r->payload_keep);
    memset(r, 0, sizeof(*r));
}

/* ================================================================
 * Parse principal
 * ================================================================ */

bool owl_mssql_query_parse_response(const uint8_t* payload, size_t len,
                                      OwlMssqlResult* r,
                                      char* parse_err) {
    owl_mssql_result_init(r);
    if (parse_err) parse_err[0] = '\0';

    /* On dup le payload pour que cells.data reste valide apres retour. */
    r->payload_keep = (uint8_t*)malloc(len);
    if (!r->payload_keep) {
        if (parse_err) snprintf(parse_err, 128, "OOM payload_keep");
        return false;
    }
    memcpy(r->payload_keep, payload, len);
    r->payload_keep_len = len;

    OwlLeBuffer b;
    owl_le_buffer_attach_read(&b, r->payload_keep, len);

    /* Buffers temporaires pour les lignes : on alloue exponentiellement. */
    size_t row_cap = 0;
    /* row_count vit dans r */
    OwlMssqlCell* row_buf = NULL;
    bool got_done = false;

    while (owl_le_buffer_remaining(&b) > 0 && !got_done) {
        uint8_t tok;
        if (!owl_le_buffer_read_u8(&b, &tok)) {
            if (parse_err) snprintf(parse_err, 128, "stream tronque (token)");
            free(row_buf);
            return false;
        }

        switch ((OwlTdsToken)tok) {
            case TDS_TOK_COLMETADATA: {
                uint16_t count;
                if (!owl_le_buffer_read_u16(&b, &count)) {
                    if (parse_err) snprintf(parse_err, 128, "COLMETADATA count");
                    free(row_buf); return false;
                }
                if (count == 0xFFFF) {
                    /* "No metadata" — pas de result set, juste un DONE attendu. */
                    r->kind = TDS_RESULT_EMPTY;
                    break;
                }
                r->columns = (OwlMssqlColumn*)calloc(count, sizeof(OwlMssqlColumn));
                if (!r->columns) {
                    if (parse_err) snprintf(parse_err, 128, "OOM columns");
                    free(row_buf); return false;
                }
                r->column_count = count;
                for (size_t i = 0; i < count; ++i) {
                    if (!parse_one_column(&b, &r->columns[i], parse_err)) {
                        free(row_buf); return false;
                    }
                }
                r->kind = TDS_RESULT_ROWS;
                break;
            }

            case TDS_TOK_ROW: {
                if (r->column_count == 0) {
                    if (parse_err) snprintf(parse_err, 128, "ROW sans COLMETADATA");
                    free(row_buf); return false;
                }
                /* Grow row_buf si necessaire. */
                if (r->row_count == row_cap) {
                    size_t new_cap = row_cap ? row_cap * 2 : 16;
                    OwlMssqlCell* tmp = (OwlMssqlCell*)realloc(
                        row_buf, new_cap * r->column_count * sizeof(OwlMssqlCell));
                    if (!tmp) {
                        if (parse_err) snprintf(parse_err, 128, "OOM rows");
                        free(row_buf); return false;
                    }
                    row_buf = tmp;
                    row_cap = new_cap;
                }
                OwlMssqlCell* row = row_buf + r->row_count * r->column_count;
                for (size_t i = 0; i < r->column_count; ++i) {
                    if (!parse_one_cell(&b, &r->columns[i], &row[i], parse_err)) {
                        free(row_buf); return false;
                    }
                }
                r->row_count++;
                break;
            }

            case TDS_TOK_NBCROW: {
                if (r->column_count == 0) {
                    if (parse_err) snprintf(parse_err, 128, "NBCROW sans COLMETADATA");
                    free(row_buf); return false;
                }
                size_t nb_bytes = (r->column_count + 7) / 8;
                if (owl_le_buffer_remaining(&b) < nb_bytes) {
                    if (parse_err) snprintf(parse_err, 128, "NBCROW bitmap tronque");
                    free(row_buf); return false;
                }
                const uint8_t* bitmap = b.data + b.pos;
                owl_le_buffer_read_skip(&b, nb_bytes);

                if (r->row_count == row_cap) {
                    size_t new_cap = row_cap ? row_cap * 2 : 16;
                    OwlMssqlCell* tmp = (OwlMssqlCell*)realloc(
                        row_buf, new_cap * r->column_count * sizeof(OwlMssqlCell));
                    if (!tmp) {
                        if (parse_err) snprintf(parse_err, 128, "OOM rows");
                        free(row_buf); return false;
                    }
                    row_buf = tmp;
                    row_cap = new_cap;
                }
                OwlMssqlCell* row = row_buf + r->row_count * r->column_count;
                for (size_t i = 0; i < r->column_count; ++i) {
                    bool is_null_bit = (bitmap[i / 8] & (1 << (i % 8))) != 0;
                    if (is_null_bit) {
                        row[i].data = NULL;
                        row[i].len = 0;
                        row[i].is_null = true;
                    } else {
                        if (!parse_one_cell(&b, &r->columns[i], &row[i], parse_err)) {
                            free(row_buf); return false;
                        }
                    }
                }
                r->row_count++;
                break;
            }

            case TDS_TOK_DONE:
            case TDS_TOK_DONEPROC:
            case TDS_TOK_DONEINPROC: {
                uint16_t status, cur_cmd;
                uint64_t row_count;
                if (!owl_le_buffer_read_u16(&b, &status)
                 || !owl_le_buffer_read_u16(&b, &cur_cmd)
                 || !owl_le_buffer_read_u64(&b, &row_count)) {
                    if (parse_err) snprintf(parse_err, 128, "DONE tronque");
                    free(row_buf); return false;
                }
                r->rows_affected = row_count;
                if (tok == TDS_TOK_DONE) {
                    r->done_received = true;
                    got_done = true;
                }
                (void)status; (void)cur_cmd;
                break;
            }

            case TDS_TOK_ERROR:
                if (!parse_error_info_skip(&b, r, true, parse_err)) {
                    free(row_buf); return false;
                }
                break;
            case TDS_TOK_INFO:
                if (!parse_error_info_skip(&b, r, false, parse_err)) {
                    free(row_buf); return false;
                }
                break;
            case TDS_TOK_ENVCHANGE:
                if (!skip_envchange(&b, parse_err)) {
                    free(row_buf); return false;
                }
                break;
            case TDS_TOK_ORDER:
                if (!skip_order(&b, parse_err)) {
                    free(row_buf); return false;
                }
                break;
            case TDS_TOK_RETURNSTATUS: {
                /* RETURNSTATUS = u32 LE. Skip. */
                if (!owl_le_buffer_read_skip(&b, 4)) {
                    if (parse_err) snprintf(parse_err, 128, "RETURNSTATUS tronque");
                    free(row_buf); return false;
                }
                break;
            }

            default:
                if (parse_err) {
                    snprintf(parse_err, 128, "token 0x%02X non géré", tok);
                }
                free(row_buf);
                return false;
        }
    }

    r->cells = row_buf;
    return true;
}
