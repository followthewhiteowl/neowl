// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_login.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * UTF-8 -> UCS-2 LE
 *
 * MSSQL Login7 attend les strings en UCS-2 little-endian (chaque char
 * = 2 octets). UCS-2 = subset UTF-16 sans surrogates. Pour V1 on
 * accepte tout code point < 0x10000 ; les autres (emojis hors BMP)
 * sont remplaces par '?'.
 *
 * Retourne le nombre d'unites UCS-2 ecrites dans out (donc len*2 octets).
 * out_cap : capacite en unites UCS-2 (pas en octets). out peut etre NULL
 * pour ne calculer que la longueur (mode dry-run).
 * ================================================================ */
static size_t utf8_to_ucs2le(const char* utf8, uint16_t* out, size_t out_cap) {
    if (!utf8) return 0;
    size_t written = 0;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (*p++ & 0x1F) << 6;
            if ((*p & 0xC0) != 0x80) { cp = '?'; continue; }
            cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (*p++ & 0x0F) << 12;
            if ((*p & 0xC0) != 0x80) { cp = '?'; continue; }
            cp |= (*p++ & 0x3F) << 6;
            if ((*p & 0xC0) != 0x80) { cp = '?'; continue; }
            cp |= (*p++ & 0x3F);
        } else if ((*p & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 -> code point > 0xFFFF, hors UCS-2 BMP. */
            p++;
            if ((*p & 0xC0) == 0x80) p++;
            if ((*p & 0xC0) == 0x80) p++;
            if ((*p & 0xC0) == 0x80) p++;
            cp = '?';
        } else {
            /* Invalide. */
            p++;
            cp = '?';
        }
        if (cp > 0xFFFF) cp = '?';
        if (out && written < out_cap) {
            out[written] = (uint16_t)cp;   /* memcpy en LE car CPU x86 LE */
        }
        written++;
    }
    return written;
}

/* Ecrit la string UTF-8 en UCS-2 LE dans le buffer (sans terminator).
 * Renvoie le nombre d'unites UCS-2 ecrites (1 unite = 2 octets). */
static size_t append_ucs2le(OwlLeBuffer* b, const char* utf8) {
    if (!utf8 || !*utf8) return 0;
    /* On encode caractere par caractere. Pour la simplicite on alloue un
     * petit buffer scratch dont on peut faire grandir si necessaire. */
    size_t cap = strlen(utf8) + 1;   /* upper bound : 1 unite UCS-2 par octet UTF-8 */
    uint16_t* tmp = (uint16_t*)malloc(cap * sizeof(uint16_t));
    if (!tmp) return 0;
    size_t n = utf8_to_ucs2le(utf8, tmp, cap);
    /* Ecrit n*2 octets en LE (l'hote x86 est deja LE, donc memcpy direct). */
    for (size_t i = 0; i < n; ++i) {
        owl_le_buffer_write_u16(b, tmp[i]);
    }
    free(tmp);
    return n;
}

/* ================================================================
 * Password obfuscation
 * ================================================================ */

void owl_mssql_login_obfuscate(uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        data[i] = (uint8_t)((((b & 0x0F) << 4) | ((b & 0xF0) >> 4)) ^ 0xA5);
    }
}

/* ================================================================
 * Login7 pack
 *
 * Layout (MS-TDS 2.2.6.4) :
 *
 *   Offset  Size  Field
 *   0       4     Length (u32 LE, total payload Login7)
 *   4       4     TDSVersion (u32 LE) — 0x74000004
 *   8       4     PacketSize (u32 LE)
 *   12      4     ClientProgVer (u32 LE)
 *   16      4     ClientPID (u32 LE)
 *   20      4     ConnectionID (u32 LE) — 0
 *   24      1     OptionFlags1
 *   25      1     OptionFlags2
 *   26      1     TypeFlags
 *   27      1     OptionFlags3
 *   28      4     ClientTimeZone (i32 LE) — minutes from GMT
 *   32      4     ClientLCID (u32 LE) — 0x409 = en-US
 *   ---- Variable section offset/length pairs (u16 each LE) ----
 *   36      4     HostName    (offset, length-in-chars)
 *   40      4     UserName
 *   44      4     Password
 *   48      4     AppName
 *   52      4     ServerName
 *   56      4     Extension/Unused
 *   60      4     CltIntName
 *   64      4     Language
 *   68      4     Database
 *   72      6     ClientID (MAC, raw 6 bytes)
 *   78      4     SSPI offset/length
 *   82      4     AtchDBFile offset/length
 *   86      4     ChangePassword offset/length
 *   90      4     cbSSPILong (u32 LE)
 *   94+     ...   Data area (UCS-2 LE strings)
 *
 * Total fixed = 94 octets. Le password est obfusque dans la data area.
 * ================================================================ */

#define LOGIN7_FIXED_SIZE 94

bool owl_mssql_login_pack(OwlLeBuffer* out, const OwlMssqlLoginRequest* req) {
    if (!out || !req) return false;

    /* On suppose out vide (.length == 0). On ecrit d'abord toute la zone fixe
     * avec des placeholders, puis on patche les offsets/length apres ecriture
     * de la zone data. */
    if (out->length != 0) return false;

    /* Pre-calcule les longueurs UCS-2 (en unites = chars) de chaque champ. */
    const char* appname    = (req->appname && req->appname[0])    ? req->appname    : "OWL";
    const char* clt_int    = (req->clt_int_name && req->clt_int_name[0]) ? req->clt_int_name : "owl-mssql";

    size_t len_host = utf8_to_ucs2le(req->hostname,  NULL, 0);
    size_t len_user = utf8_to_ucs2le(req->username,  NULL, 0);
    size_t len_pass = utf8_to_ucs2le(req->password,  NULL, 0);
    size_t len_app  = utf8_to_ucs2le(appname,        NULL, 0);
    size_t len_srv  = utf8_to_ucs2le(req->servername,NULL, 0);
    size_t len_clt  = utf8_to_ucs2le(clt_int,        NULL, 0);
    size_t len_lang = utf8_to_ucs2le(req->language,  NULL, 0);
    size_t len_db   = utf8_to_ucs2le(req->database,  NULL, 0);

    /* Offsets dans le payload : data area apres la zone fixe. */
    uint16_t off_data    = (uint16_t)LOGIN7_FIXED_SIZE;
    uint16_t off_host    = off_data;
    uint16_t off_user    = (uint16_t)(off_host + len_host * 2);
    uint16_t off_pass    = (uint16_t)(off_user + len_user * 2);
    uint16_t off_app     = (uint16_t)(off_pass + len_pass * 2);
    uint16_t off_srv     = (uint16_t)(off_app  + len_app  * 2);
    uint16_t off_clt     = (uint16_t)(off_srv  + len_srv  * 2);
    uint16_t off_lang    = (uint16_t)(off_clt  + len_clt  * 2);
    uint16_t off_db      = (uint16_t)(off_lang + len_lang * 2);

    /* Taille totale prevue. */
    uint32_t total = (uint32_t)(LOGIN7_FIXED_SIZE
                                + (len_host + len_user + len_pass + len_app
                                + len_srv + len_clt + len_lang + len_db) * 2);

    /* --- Zone fixe (94 octets) --- */
    owl_le_buffer_write_u32(out, total);                /* offset 0 : Length */
    owl_le_buffer_write_u32(out, TDS_VERSION_7_4);      /* offset 4 : TDSVersion */
    owl_le_buffer_write_u32(out, req->packet_size ? req->packet_size : 4096);
    owl_le_buffer_write_u32(out, 0);                    /* ClientProgVer */
    owl_le_buffer_write_u32(out, req->client_pid);
    owl_le_buffer_write_u32(out, 0);                    /* ConnectionID */

    /* OptionFlags1 : USE_DB_FATAL_ON_FAIL = 0x40 (si echec USE database, on
     * echoue le login plutot que d'en utiliser une autre). Le reste a 0 :
     * byte order BIG_ENDIAN=0 (LE), CHAR_SET_ASCII=0, FLOAT_TYPE_IEEE_754=0,
     * DUMPLOAD_ON=0. */
    owl_le_buffer_write_u8(out, 0x00);

    /* OptionFlags2 : INIT_LANG_FATAL=0x01 (echec set language => fail).
     * ODBC=0x02 (on est compatible ODBC behaviors). */
    owl_le_buffer_write_u8(out, 0x03);

    /* TypeFlags : SQL_TYPE_DFLT=0 (SQL Server default). */
    owl_le_buffer_write_u8(out, 0x00);

    /* OptionFlags3 : UNKNOWN_COLLATION_HANDLING=0x01, EXTENSION=0x00. */
    owl_le_buffer_write_u8(out, 0x00);

    owl_le_buffer_write_i32(out, 0);                    /* ClientTimeZone : 0 = UTC */
    owl_le_buffer_write_u32(out, 0x00000409);           /* ClientLCID : en-US */

    /* --- Descripteurs variable section (58 octets) --- */
    owl_le_buffer_write_u16(out, off_host);  owl_le_buffer_write_u16(out, (uint16_t)len_host);
    owl_le_buffer_write_u16(out, off_user);  owl_le_buffer_write_u16(out, (uint16_t)len_user);
    owl_le_buffer_write_u16(out, off_pass);  owl_le_buffer_write_u16(out, (uint16_t)len_pass);
    owl_le_buffer_write_u16(out, off_app );  owl_le_buffer_write_u16(out, (uint16_t)len_app);
    owl_le_buffer_write_u16(out, off_srv );  owl_le_buffer_write_u16(out, (uint16_t)len_srv);
    /* Extension : offset/length a 0 pour V1 (pas d'extension envoyee). */
    owl_le_buffer_write_u16(out, 0);         owl_le_buffer_write_u16(out, 0);
    owl_le_buffer_write_u16(out, off_clt );  owl_le_buffer_write_u16(out, (uint16_t)len_clt);
    owl_le_buffer_write_u16(out, off_lang);  owl_le_buffer_write_u16(out, (uint16_t)len_lang);
    owl_le_buffer_write_u16(out, off_db  );  owl_le_buffer_write_u16(out, (uint16_t)len_db);

    /* ClientID : 6 octets bruts (MAC). */
    owl_le_buffer_write_bytes(out, req->client_id, 6);

    /* SSPI / AtchDBFile / ChangePassword / cbSSPILong : tous a 0 en V1. */
    owl_le_buffer_write_u16(out, 0); owl_le_buffer_write_u16(out, 0);   /* SSPI */
    owl_le_buffer_write_u16(out, 0); owl_le_buffer_write_u16(out, 0);   /* AtchDBFile */
    owl_le_buffer_write_u16(out, 0); owl_le_buffer_write_u16(out, 0);   /* ChangePassword */
    owl_le_buffer_write_u32(out, 0);                                     /* cbSSPILong */

    /* --- Zone data : UCS-2 LE des champs --- */
    size_t mark_host = out->length;
    append_ucs2le(out, req->hostname);
    size_t mark_user = out->length;
    append_ucs2le(out, req->username);
    size_t mark_pass = out->length;
    append_ucs2le(out, req->password);
    size_t mark_pass_end = out->length;
    append_ucs2le(out, appname);
    append_ucs2le(out, req->servername);
    append_ucs2le(out, clt_int);
    append_ucs2le(out, req->language);
    append_ucs2le(out, req->database);

    /* Obfusque le password en place (dans la zone data juste ecrite). */
    if (mark_pass_end > mark_pass) {
        owl_mssql_login_obfuscate(out->data + mark_pass, mark_pass_end - mark_pass);
    }

    /* Mark unused (just for code readability — the variables document the offsets). */
    (void)mark_host; (void)mark_user;
    return true;
}

/* ================================================================
 * Login response parser
 *
 * Stream de tokens decode jusqu'a DONE ou ERROR final. On ne lit que
 * les tokens utiles au login : ERROR / INFO / LOGINACK / ENVCHANGE / DONE.
 * Les autres tokens sont skip via leur length declare ou structure connue.
 *
 * Format des tokens utiles :
 *
 *   ERROR (0xAA) :
 *     u16 Length (longueur du contenu qui suit)
 *     u32 Number, u8 State, u8 Class, u16 MsgLen, MsgText (UCS-2 LE),
 *     u8 ServerNameLen, ServerName (UCS-2), u8 ProcNameLen, ProcName,
 *     u32 LineNumber
 *
 *   INFO (0xAB) : meme structure qu'ERROR.
 *
 *   LOGINACK (0xAD) :
 *     u16 Length
 *     u8 Interface, 4 bytes TDSVersion (BE),
 *     u8 ProgNameLen, ProgName (UCS-2),
 *     4 bytes ProgVersion (u8 major, u8 minor, u16 BE build)
 *
 *   ENVCHANGE (0xE3) :
 *     u16 Length
 *     u8 Type, then type-specific :
 *       PACKET_SIZE : u8 newLen, newValue (UCS-2), u8 oldLen, oldValue (UCS-2)
 *       DATABASE   : u8 newLen, newDB (UCS-2), u8 oldLen, oldDB (UCS-2)
 *
 *   DONE (0xFD) :
 *     u16 Status, u16 CurCmd, u64 RowCount
 *
 * Si Length n'est pas declare (tokens fixes), on skip la quantite fixe.
 * Tokens inconnus en Lot 3 -> erreur (Lot 4 ajoutera COLMETADATA/ROW).
 * ================================================================ */

/* Helper : decode une chaine UCS-2 LE de N unites (chars) en UTF-8 ASCII
 * compatible (pour les messages d'erreur). On tronque a out_cap-1.
 * Si caractere > 0x7F, on l'encode minimalement en UTF-8 a partir du code
 * point. Pour V1 c'est suffisant. */
static void ucs2le_to_utf8(const uint8_t* src, size_t n_units,
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

bool owl_mssql_login_parse_response(const uint8_t* payload, size_t len,
                                      OwlMssqlLoginResponse* resp,
                                      char* parse_err) {
    memset(resp, 0, sizeof(*resp));
    if (parse_err) parse_err[0] = '\0';

    OwlLeBuffer b;
    owl_le_buffer_attach_read(&b, payload, len);

    bool got_loginack = false;
    bool got_done     = false;

    while (owl_le_buffer_remaining(&b) > 0) {
        uint8_t tok;
        if (!owl_le_buffer_read_u8(&b, &tok)) {
            if (parse_err) snprintf(parse_err, 128, "stream tronque (token)");
            return false;
        }

        switch ((OwlTdsToken)tok) {
            case TDS_TOK_ERROR:
            case TDS_TOK_INFO: {
                uint16_t length;
                if (!owl_le_buffer_read_u16(&b, &length)) {
                    if (parse_err) snprintf(parse_err, 128, "ERROR/INFO len absent");
                    return false;
                }
                if (length > owl_le_buffer_remaining(&b)) {
                    if (parse_err) snprintf(parse_err, 128, "ERROR/INFO contenu tronque");
                    return false;
                }
                uint32_t num; uint8_t state; uint8_t class_;
                uint16_t msglen;
                if (!owl_le_buffer_read_u32(&b, &num)
                 || !owl_le_buffer_read_u8(&b, &state)
                 || !owl_le_buffer_read_u8(&b, &class_)
                 || !owl_le_buffer_read_u16(&b, &msglen)) {
                    if (parse_err) snprintf(parse_err, 128, "ERROR/INFO header lecture");
                    return false;
                }
                const uint8_t* msg = b.data + b.pos;
                if (owl_le_buffer_remaining(&b) < (size_t)msglen * 2) {
                    if (parse_err) snprintf(parse_err, 128, "ERROR/INFO msg tronque");
                    return false;
                }
                owl_le_buffer_read_skip(&b, (size_t)msglen * 2);

                uint8_t srv_len, proc_len;
                if (!owl_le_buffer_read_u8(&b, &srv_len)) return false;
                const uint8_t* srv = b.data + b.pos;
                if (owl_le_buffer_remaining(&b) < (size_t)srv_len * 2) return false;
                owl_le_buffer_read_skip(&b, (size_t)srv_len * 2);

                if (!owl_le_buffer_read_u8(&b, &proc_len)) return false;
                const uint8_t* proc = b.data + b.pos;
                if (owl_le_buffer_remaining(&b) < (size_t)proc_len * 2) return false;
                owl_le_buffer_read_skip(&b, (size_t)proc_len * 2);

                uint32_t line_no;
                if (!owl_le_buffer_read_u32(&b, &line_no)) return false;

                if (tok == TDS_TOK_ERROR) {
                    resp->error_number = num;
                    resp->error_state  = state;
                    resp->error_class  = class_;
                    resp->error_line   = line_no;
                    ucs2le_to_utf8(msg,  msglen,   resp->error_message, sizeof(resp->error_message));
                    ucs2le_to_utf8(srv,  srv_len,  resp->error_server,  sizeof(resp->error_server));
                    ucs2le_to_utf8(proc, proc_len, resp->error_proc,    sizeof(resp->error_proc));
                } else {
                    /* INFO : on garde seulement le premier message. */
                    if (!resp->info_present) {
                        ucs2le_to_utf8(msg, msglen,
                                        resp->info_message, sizeof(resp->info_message));
                        resp->info_present = true;
                    }
                }
                break;
            }

            case TDS_TOK_LOGINACK: {
                uint16_t length;
                if (!owl_le_buffer_read_u16(&b, &length)) return false;
                if (length > owl_le_buffer_remaining(&b)) {
                    if (parse_err) snprintf(parse_err, 128, "LOGINACK tronque");
                    return false;
                }
                uint8_t interf;
                if (!owl_le_buffer_read_u8(&b, &interf)) return false;
                /* TDSVersion : 4 bytes BE selon spec. */
                uint8_t tv[4];
                if (!owl_le_buffer_read_bytes(&b, tv, 4)) return false;
                resp->tds_version_negotiated = ((uint32_t)tv[0] << 24)
                                              | ((uint32_t)tv[1] << 16)
                                              | ((uint32_t)tv[2] << 8)
                                              | (uint32_t)tv[3];
                uint8_t prog_len;
                if (!owl_le_buffer_read_u8(&b, &prog_len)) return false;
                const uint8_t* prog = b.data + b.pos;
                if (owl_le_buffer_remaining(&b) < (size_t)prog_len * 2) return false;
                owl_le_buffer_read_skip(&b, (size_t)prog_len * 2);
                ucs2le_to_utf8(prog, prog_len,
                                resp->server_program, sizeof(resp->server_program));
                /* ProgVersion : 4 octets : u8 major, u8 minor, u16 BE build. */
                uint8_t pv[4];
                if (!owl_le_buffer_read_bytes(&b, pv, 4)) return false;
                resp->server_version_major = pv[0];
                resp->server_version_minor = pv[1];
                resp->server_version_build = (uint16_t)((pv[2] << 8) | pv[3]);
                got_loginack = true;
                break;
            }

            case TDS_TOK_ENVCHANGE: {
                uint16_t length;
                if (!owl_le_buffer_read_u16(&b, &length)) return false;
                size_t end_pos = b.pos + length;
                if (end_pos > b.length) {
                    if (parse_err) snprintf(parse_err, 128, "ENVCHANGE tronque");
                    return false;
                }
                uint8_t env_type;
                if (!owl_le_buffer_read_u8(&b, &env_type)) return false;
                if (env_type == TDS_ENVCHANGE_PACKET_SIZE) {
                    /* new : B_VARCHAR (u8 len + UCS-2). On le lit en char int. */
                    uint8_t newlen_chars;
                    if (!owl_le_buffer_read_u8(&b, &newlen_chars)) return false;
                    char numstr[16] = {0};
                    ucs2le_to_utf8(b.data + b.pos, newlen_chars,
                                    numstr, sizeof(numstr));
                    owl_le_buffer_read_skip(&b, (size_t)newlen_chars * 2);
                    /* old : on skip. */
                    uint8_t oldlen_chars;
                    if (!owl_le_buffer_read_u8(&b, &oldlen_chars)) return false;
                    owl_le_buffer_read_skip(&b, (size_t)oldlen_chars * 2);
                    resp->negotiated_packet_size = (uint32_t)atoi(numstr);
                    resp->packet_size_changed    = true;
                } else {
                    /* Autres types ENVCHANGE : on skip jusqu'a end_pos. */
                    if (end_pos > b.pos) {
                        owl_le_buffer_read_skip(&b, end_pos - b.pos);
                    }
                }
                break;
            }

            case TDS_TOK_DONE:
            case TDS_TOK_DONEPROC:
            case TDS_TOK_DONEINPROC: {
                /* Structure DONE : u16 Status, u16 CurCmd, u64 RowCount. */
                uint16_t status, cur_cmd;
                uint64_t row_count;
                if (!owl_le_buffer_read_u16(&b, &status)) return false;
                if (!owl_le_buffer_read_u16(&b, &cur_cmd)) return false;
                if (!owl_le_buffer_read_u64(&b, &row_count)) return false;
                (void)status; (void)cur_cmd; (void)row_count;
                if (tok == TDS_TOK_DONE) got_done = true;
                break;
            }

            default:
                if (parse_err) {
                    snprintf(parse_err, 128,
                             "token inconnu 0x%02X dans reponse Login7", tok);
                }
                return false;
        }
    }

    /* Resultat final : success si LOGINACK + DONE et pas d'ERROR. */
    if (resp->error_number != 0) {
        resp->success = false;
    } else if (got_loginack && got_done) {
        resp->success = true;
    } else {
        resp->success = false;
        if (parse_err && parse_err[0] == '\0') {
            snprintf(parse_err, 128,
                     "Login response incomplete (loginack=%d done=%d)",
                     got_loginack, got_done);
        }
    }
    return true;
}
