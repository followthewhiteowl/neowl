// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_mssql_prelogin.h"

#include <string.h>
#include <stdio.h>

/* ================================================================
 * Helpers : ecriture BE 16 bits dans OwlLeBuffer
 *
 * net_le_buffer ecrit en little-endian, mais les TokenPosition et
 * TokenLength du PreLogin sont specifies en BIG-ENDIAN dans MS-TDS.
 * On les ecrit manuellement.
 * ================================================================ */

static void write_u16_be(OwlLeBuffer* b, uint16_t v) {
    owl_le_buffer_write_u8(b, (uint8_t)((v >> 8) & 0xFF));
    owl_le_buffer_write_u8(b, (uint8_t)(v & 0xFF));
}

static uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* ================================================================
 * Pack
 *
 * Strategie en 2 passes :
 *   1) On enumere les tokens a inclure et on calcule leurs tailles
 *      pour determiner les offsets dans la zone data.
 *   2) On ecrit la zone descripteurs (token, position, length) puis
 *      la zone data dans l'ordre.
 * ================================================================ */

bool owl_tds_prelogin_pack(OwlLeBuffer* out, const OwlTdsPreloginRequest* req) {
    if (!out || !req) return false;

    /* On a toujours VERSION + ENCRYPTION + THREADID + MARS + TERMINATOR.
     * INSTOPT est conditionnel (si instance != NULL && != ""). */
    bool send_instance = req->instance && req->instance[0] != '\0';
    size_t instance_len = send_instance ? strlen(req->instance) + 1 : 0; /* +1 = \0 final */

    /* Nombre de descripteurs : 4 fixes + 1 si instance. */
    size_t n_desc = 4 + (send_instance ? 1 : 0);

    /* Taille zone descripteurs = n_desc * 5 + 1 (terminator 0xFF). */
    size_t desc_size = n_desc * 5 + 1;

    /* Tailles donnees :
     *   VERSION    = 6 octets (4 version + 2 subbuild)
     *   ENCRYPTION = 1 octet
     *   INSTOPT    = instance_len (0-terminated)
     *   THREADID   = 4 octets (u32, BE par convention TDS)
     *   MARS       = 1 octet
     */
    size_t data_size = 6 + 1 + instance_len + 4 + 1;

    /* Offsets dans le payload : la zone donnees commence apres la zone
     * descripteurs (donc offset == desc_size). */
    uint16_t off_version    = (uint16_t)desc_size;
    uint16_t off_encryption = (uint16_t)(off_version + 6);
    uint16_t off_instance   = (uint16_t)(off_encryption + 1);
    uint16_t off_threadid   = (uint16_t)(off_instance + instance_len);
    uint16_t off_mars       = (uint16_t)(off_threadid + 4);

    /* --- Zone descripteurs --- */
    /* VERSION */
    owl_le_buffer_write_u8(out, TDS_PRELOGIN_VERSION);
    write_u16_be(out, off_version);
    write_u16_be(out, 6);

    /* ENCRYPTION */
    owl_le_buffer_write_u8(out, TDS_PRELOGIN_ENCRYPTION);
    write_u16_be(out, off_encryption);
    write_u16_be(out, 1);

    /* INSTOPT (optionnel) */
    if (send_instance) {
        owl_le_buffer_write_u8(out, TDS_PRELOGIN_INSTOPT);
        write_u16_be(out, off_instance);
        write_u16_be(out, (uint16_t)instance_len);
    }

    /* THREADID */
    owl_le_buffer_write_u8(out, TDS_PRELOGIN_THREADID);
    write_u16_be(out, off_threadid);
    write_u16_be(out, 4);

    /* MARS */
    owl_le_buffer_write_u8(out, TDS_PRELOGIN_MARS);
    write_u16_be(out, off_mars);
    write_u16_be(out, 1);

    /* TERMINATOR */
    owl_le_buffer_write_u8(out, TDS_PRELOGIN_TERMINATOR);

    /* --- Zone donnees --- */
    /* VERSION : major.minor.build (build u16 BE), subbuild u16 LE. */
    owl_le_buffer_write_u8(out, req->version_major);
    owl_le_buffer_write_u8(out, req->version_minor);
    write_u16_be(out, req->version_build);     /* doc dit BE */
    owl_le_buffer_write_u16(out, req->version_subbuild); /* LE par convention */

    /* ENCRYPTION */
    owl_le_buffer_write_u8(out, (uint8_t)req->encryption);

    /* INSTOPT */
    if (send_instance) {
        owl_le_buffer_write_bytes(out, req->instance, instance_len);
    }

    /* THREADID : u32 BE par convention TDS */
    {
        uint32_t tid = req->thread_id;
        owl_le_buffer_write_u8(out, (uint8_t)((tid >> 24) & 0xFF));
        owl_le_buffer_write_u8(out, (uint8_t)((tid >> 16) & 0xFF));
        owl_le_buffer_write_u8(out, (uint8_t)((tid >> 8) & 0xFF));
        owl_le_buffer_write_u8(out, (uint8_t)(tid & 0xFF));
    }

    /* MARS */
    owl_le_buffer_write_u8(out, req->mars ? 1 : 0);

    /* Sanity check : taille totale = desc_size + data_size */
    (void)data_size;  /* utilise seulement pour le calcul d'offsets */
    return true;
}

/* ================================================================
 * Parse
 *
 * On parcourt les descripteurs (5 octets chacun) jusqu'au TERMINATOR,
 * puis on lit chaque donnee a son offset+length declares. On verifie
 * que les ranges ne debordent pas du payload.
 * ================================================================ */

bool owl_tds_prelogin_parse(const uint8_t* payload, size_t payload_len,
                              OwlTdsPreloginResponse* resp,
                              char* out_err) {
    memset(resp, 0, sizeof(*resp));
    if (out_err) out_err[0] = '\0';

    if (payload_len < 1) {
        if (out_err) snprintf(out_err, 128, "payload vide");
        return false;
    }

    /* Stocke les (token, offset, length) trouves pour les traiter ensuite. */
    typedef struct { uint8_t type; uint16_t off; uint16_t len; } Desc;
    Desc descs[16];
    size_t n_desc = 0;

    size_t cursor = 0;
    while (cursor < payload_len) {
        uint8_t t = payload[cursor];
        if (t == TDS_PRELOGIN_TERMINATOR) {
            cursor += 1;
            break;
        }
        if (cursor + 5 > payload_len) {
            if (out_err) snprintf(out_err, 128,
                "descripteur tronque (cursor=%zu, payload_len=%zu)",
                cursor, payload_len);
            return false;
        }
        if (n_desc >= 16) {
            if (out_err) snprintf(out_err, 128, "trop de descripteurs (>16)");
            return false;
        }
        descs[n_desc].type = t;
        descs[n_desc].off  = read_u16_be(payload + cursor + 1);
        descs[n_desc].len  = read_u16_be(payload + cursor + 3);
        n_desc++;
        cursor += 5;
    }

    /* On verifie que chaque (off, len) reste dans payload_len. */
    for (size_t i = 0; i < n_desc; ++i) {
        size_t end = (size_t)descs[i].off + (size_t)descs[i].len;
        if (end > payload_len) {
            if (out_err) snprintf(out_err, 128,
                "token 0x%02X out-of-bounds (off=%u len=%u, payload=%zu)",
                descs[i].type, descs[i].off, descs[i].len, payload_len);
            return false;
        }
    }

    /* Decode chaque token connu. */
    for (size_t i = 0; i < n_desc; ++i) {
        const uint8_t* d = payload + descs[i].off;
        uint16_t       n = descs[i].len;

        switch (descs[i].type) {
            case TDS_PRELOGIN_VERSION:
                if (n < 6) {
                    if (out_err) snprintf(out_err, 128,
                        "VERSION trop court (%u)", n);
                    return false;
                }
                resp->version_major    = d[0];
                resp->version_minor    = d[1];
                resp->version_build    = read_u16_be(d + 2);
                resp->version_subbuild = (uint16_t)(d[4] | (d[5] << 8));  /* LE */
                resp->version_present  = true;
                break;
            case TDS_PRELOGIN_ENCRYPTION:
                if (n < 1) {
                    if (out_err) snprintf(out_err, 128,
                        "ENCRYPTION sans valeur");
                    return false;
                }
                resp->encryption         = (OwlTdsEncryptionMode)d[0];
                resp->encryption_present = true;
                break;
            case TDS_PRELOGIN_INSTOPT:
                resp->instance_present = true;
                break;
            case TDS_PRELOGIN_MARS:
                if (n < 1) {
                    if (out_err) snprintf(out_err, 128, "MARS sans valeur");
                    return false;
                }
                resp->mars_value   = (d[0] != 0);
                resp->mars_present = true;
                break;
            case TDS_PRELOGIN_FEDAUTHREQUIRED:
                if (n < 1) {
                    if (out_err) snprintf(out_err, 128,
                        "FEDAUTHREQUIRED sans valeur");
                    return false;
                }
                resp->fedauth_required = (d[0] != 0);
                resp->fedauth_present  = true;
                break;
            case TDS_PRELOGIN_THREADID:
            case TDS_PRELOGIN_TRACEID:
            case TDS_PRELOGIN_NONCEOPT:
                /* tokens connus mais ignores en V1 */
                break;
            default:
                /* token inconnu : on ignore (forward compat). */
                break;
        }
    }

    return true;
}
