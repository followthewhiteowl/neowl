// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_le_buffer_h
#define owl_net_le_buffer_h

/* ================================================================
 * LITTLE-ENDIAN BINARY BUFFER
 *
 * Read/write primitives pour parser les protocoles binaires en
 * little-endian — typiquement MySQL/MariaDB wire (qui n'a pas suivi
 * la convention "network byte order" big-endian).
 *
 * API symetrique a OwlBeBuffer (cf net_be_buffer.h). Seule difference :
 * l'ordre des octets dans les write_u16/u32/u64 et les read_*.
 *
 * MySQL ajoute en plus des integers de taille VARIABLE (length-encoded :
 * 1/3/4/9 octets selon valeur) — exposes ici via owl_le_buffer_*_lenenc.
 *
 * Convention erreur identique a BeBuffer :
 *   - write_* : pas d'erreur (grow auto sauf OOM silencieux).
 *   - read_*  : retourne false si truncated, n'avance pas pos.
 * ================================================================ */

#include "common.h"

typedef struct {
    uint8_t* data;
    size_t   length;
    size_t   capacity;
    size_t   pos;
} OwlLeBuffer;

/* ---------- Construction / teardown ---------- */

void owl_le_buffer_init       (OwlLeBuffer* buf, size_t initial_capacity);
void owl_le_buffer_free       (OwlLeBuffer* buf);
void owl_le_buffer_reset      (OwlLeBuffer* buf);
void owl_le_buffer_attach_read(OwlLeBuffer* buf, const uint8_t* data, size_t length);

/* ---------- Write (mode append) ---------- */

void owl_le_buffer_write_u8 (OwlLeBuffer* buf, uint8_t  v);
void owl_le_buffer_write_u16(OwlLeBuffer* buf, uint16_t v);  /* LE */
void owl_le_buffer_write_u24(OwlLeBuffer* buf, uint32_t v);  /* LE — 3 octets (MySQL packet length) */
void owl_le_buffer_write_u32(OwlLeBuffer* buf, uint32_t v);  /* LE */
void owl_le_buffer_write_u64(OwlLeBuffer* buf, uint64_t v);  /* LE */
void owl_le_buffer_write_i16(OwlLeBuffer* buf, int16_t  v);
void owl_le_buffer_write_i32(OwlLeBuffer* buf, int32_t  v);
void owl_le_buffer_write_i64(OwlLeBuffer* buf, int64_t  v);

void owl_le_buffer_write_bytes  (OwlLeBuffer* buf, const void* src, size_t n);

/* C-string null-terminée (MySQL : "string<NUL>"). */
void owl_le_buffer_write_cstring(OwlLeBuffer* buf, const char* s);

/* MySQL length-encoded integer (lenenc int) :
 *   < 251        : 1 octet (valeur directe)
 *   < 2^16       : 0xFC + 2 octets LE
 *   < 2^24       : 0xFD + 3 octets LE
 *   sinon        : 0xFE + 8 octets LE
 * Les valeurs 251, 252, 253, 254 du premier octet sont des SENTINELLES
 * (FB/FC/FD/FE), elles ne peuvent pas etre une valeur directe < 251 — donc
 * sans collision. */
void owl_le_buffer_write_lenenc_int(OwlLeBuffer* buf, uint64_t v);

/* MySQL length-encoded string : lenenc int (longueur) + bytes. */
void owl_le_buffer_write_lenenc_str(OwlLeBuffer* buf, const char* s, size_t len);

/* Patch ulterieur d'un u24 LE (pour back-patcher la longueur d'un paquet
 * MySQL une fois la taille connue). */
void owl_le_buffer_patch_u24(OwlLeBuffer* buf, size_t offset, uint32_t v);

/* ---------- Read (mode consume) ---------- */

bool owl_le_buffer_read_u8 (OwlLeBuffer* buf, uint8_t*  out);
bool owl_le_buffer_read_u16(OwlLeBuffer* buf, uint16_t* out);
bool owl_le_buffer_read_u24(OwlLeBuffer* buf, uint32_t* out);  /* 3 octets LE */
bool owl_le_buffer_read_u32(OwlLeBuffer* buf, uint32_t* out);
bool owl_le_buffer_read_u64(OwlLeBuffer* buf, uint64_t* out);
bool owl_le_buffer_read_i16(OwlLeBuffer* buf, int16_t*  out);
bool owl_le_buffer_read_i32(OwlLeBuffer* buf, int32_t*  out);
bool owl_le_buffer_read_i64(OwlLeBuffer* buf, int64_t*  out);

bool owl_le_buffer_read_bytes(OwlLeBuffer* buf, void* dst, size_t n);
bool owl_le_buffer_read_skip (OwlLeBuffer* buf, size_t n);

/* C-string : zero-copy pointer interne. NULL si pas de '\0' trouve. */
const char* owl_le_buffer_read_cstring(OwlLeBuffer* buf);

/* MySQL lenenc int. Retourne false si truncated. */
bool owl_le_buffer_read_lenenc_int(OwlLeBuffer* buf, uint64_t* out);

/* MySQL lenenc string : lit la longueur, puis pointe (zero-copy) vers les
 * octets dans le buffer. *out_len recoit la longueur. Avance pos.
 * Retourne NULL si truncated (et n'avance pas pos). */
const uint8_t* owl_le_buffer_read_lenenc_str(OwlLeBuffer* buf, size_t* out_len);

/* ---------- Introspection ---------- */

size_t owl_le_buffer_remaining(const OwlLeBuffer* buf);
bool   owl_le_buffer_eof      (const OwlLeBuffer* buf);

#endif
