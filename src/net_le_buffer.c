// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "net_le_buffer.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_CAPACITY 64

/* ---------- Construction / teardown ---------- */

void owl_le_buffer_init(OwlLeBuffer* buf, size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = DEFAULT_CAPACITY;
    buf->data     = (uint8_t*)malloc(initial_capacity);
    buf->length   = 0;
    buf->capacity = (buf->data != NULL) ? initial_capacity : 0;
    buf->pos      = 0;
}

void owl_le_buffer_free(OwlLeBuffer* buf) {
    if (buf->capacity > 0 && buf->data != NULL) {
        free(buf->data);
    }
    buf->data     = NULL;
    buf->length   = 0;
    buf->capacity = 0;
    buf->pos      = 0;
}

void owl_le_buffer_reset(OwlLeBuffer* buf) {
    buf->length = 0;
    buf->pos    = 0;
}

void owl_le_buffer_attach_read(OwlLeBuffer* buf, const uint8_t* data, size_t length) {
    buf->data     = (uint8_t*)data;
    buf->length   = length;
    buf->capacity = 0;   /* 0 = non-owne */
    buf->pos      = 0;
}

/* ---------- Internal grow ---------- */

static void ensure_capacity(OwlLeBuffer* buf, size_t needed) {
    if (buf->capacity >= needed) return;
    size_t newCap = buf->capacity == 0 ? DEFAULT_CAPACITY : buf->capacity;
    while (newCap < needed) {
        if (newCap >= (SIZE_MAX / 2)) { newCap = needed; break; }
        newCap *= 2;
    }
    uint8_t* nd = (uint8_t*)realloc(buf->data, newCap);
    if (nd == NULL) return;  /* OOM silent */
    buf->data     = nd;
    buf->capacity = newCap;
}

/* ---------- Write ----------
 * LE = octet de poids faible d'abord (lecture inverse de BE). */

void owl_le_buffer_write_u8(OwlLeBuffer* buf, uint8_t v) {
    ensure_capacity(buf, buf->length + 1);
    if (buf->length + 1 > buf->capacity) return;
    buf->data[buf->length++] = v;
}

void owl_le_buffer_write_u16(OwlLeBuffer* buf, uint16_t v) {
    ensure_capacity(buf, buf->length + 2);
    if (buf->length + 2 > buf->capacity) return;
    buf->data[buf->length++] = (uint8_t)( v        & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 8 ) & 0xFF);
}

void owl_le_buffer_write_u24(OwlLeBuffer* buf, uint32_t v) {
    ensure_capacity(buf, buf->length + 3);
    if (buf->length + 3 > buf->capacity) return;
    buf->data[buf->length++] = (uint8_t)( v        & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 8 ) & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 16) & 0xFF);
}

void owl_le_buffer_write_u32(OwlLeBuffer* buf, uint32_t v) {
    ensure_capacity(buf, buf->length + 4);
    if (buf->length + 4 > buf->capacity) return;
    buf->data[buf->length++] = (uint8_t)( v        & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 8 ) & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 16) & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 24) & 0xFF);
}

void owl_le_buffer_write_u64(OwlLeBuffer* buf, uint64_t v) {
    ensure_capacity(buf, buf->length + 8);
    if (buf->length + 8 > buf->capacity) return;
    for (int i = 0; i < 8; i++) {
        buf->data[buf->length++] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
}

void owl_le_buffer_write_i16(OwlLeBuffer* buf, int16_t v) {
    owl_le_buffer_write_u16(buf, (uint16_t)v);
}
void owl_le_buffer_write_i32(OwlLeBuffer* buf, int32_t v) {
    owl_le_buffer_write_u32(buf, (uint32_t)v);
}
void owl_le_buffer_write_i64(OwlLeBuffer* buf, int64_t v) {
    owl_le_buffer_write_u64(buf, (uint64_t)v);
}

void owl_le_buffer_write_bytes(OwlLeBuffer* buf, const void* src, size_t n) {
    if (n == 0) return;
    ensure_capacity(buf, buf->length + n);
    if (buf->length + n > buf->capacity) return;
    memcpy(buf->data + buf->length, src, n);
    buf->length += n;
}

void owl_le_buffer_write_cstring(OwlLeBuffer* buf, const char* s) {
    size_t len = (s != NULL) ? strlen(s) : 0;
    ensure_capacity(buf, buf->length + len + 1);
    if (buf->length + len + 1 > buf->capacity) return;
    if (len > 0) memcpy(buf->data + buf->length, s, len);
    buf->length += len;
    buf->data[buf->length++] = 0;
}

void owl_le_buffer_write_lenenc_int(OwlLeBuffer* buf, uint64_t v) {
    if (v < 251ULL) {
        owl_le_buffer_write_u8(buf, (uint8_t)v);
    } else if (v < (1ULL << 16)) {
        owl_le_buffer_write_u8 (buf, 0xFC);
        owl_le_buffer_write_u16(buf, (uint16_t)v);
    } else if (v < (1ULL << 24)) {
        owl_le_buffer_write_u8 (buf, 0xFD);
        owl_le_buffer_write_u24(buf, (uint32_t)v);
    } else {
        owl_le_buffer_write_u8 (buf, 0xFE);
        owl_le_buffer_write_u64(buf, v);
    }
}

void owl_le_buffer_write_lenenc_str(OwlLeBuffer* buf, const char* s, size_t len) {
    owl_le_buffer_write_lenenc_int(buf, (uint64_t)len);
    if (len > 0) owl_le_buffer_write_bytes(buf, s, len);
}

void owl_le_buffer_patch_u24(OwlLeBuffer* buf, size_t offset, uint32_t v) {
    if (offset + 3 > buf->length) return;
    buf->data[offset    ] = (uint8_t)( v        & 0xFF);
    buf->data[offset + 1] = (uint8_t)((v >> 8 ) & 0xFF);
    buf->data[offset + 2] = (uint8_t)((v >> 16) & 0xFF);
}

/* ---------- Read ---------- */

bool owl_le_buffer_read_u8(OwlLeBuffer* buf, uint8_t* out) {
    if (buf->pos + 1 > buf->length) return false;
    *out = buf->data[buf->pos++];
    return true;
}

bool owl_le_buffer_read_u16(OwlLeBuffer* buf, uint16_t* out) {
    if (buf->pos + 2 > buf->length) return false;
    *out = ((uint16_t)buf->data[buf->pos    ])
         | ((uint16_t)buf->data[buf->pos + 1] << 8);
    buf->pos += 2;
    return true;
}

bool owl_le_buffer_read_u24(OwlLeBuffer* buf, uint32_t* out) {
    if (buf->pos + 3 > buf->length) return false;
    *out = ((uint32_t)buf->data[buf->pos    ])
         | ((uint32_t)buf->data[buf->pos + 1] <<  8)
         | ((uint32_t)buf->data[buf->pos + 2] << 16);
    buf->pos += 3;
    return true;
}

bool owl_le_buffer_read_u32(OwlLeBuffer* buf, uint32_t* out) {
    if (buf->pos + 4 > buf->length) return false;
    *out = ((uint32_t)buf->data[buf->pos    ])
         | ((uint32_t)buf->data[buf->pos + 1] <<  8)
         | ((uint32_t)buf->data[buf->pos + 2] << 16)
         | ((uint32_t)buf->data[buf->pos + 3] << 24);
    buf->pos += 4;
    return true;
}

bool owl_le_buffer_read_u64(OwlLeBuffer* buf, uint64_t* out) {
    if (buf->pos + 8 > buf->length) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)buf->data[buf->pos + i]) << (i * 8);
    }
    *out = v;
    buf->pos += 8;
    return true;
}

bool owl_le_buffer_read_i16(OwlLeBuffer* buf, int16_t* out) {
    uint16_t u; if (!owl_le_buffer_read_u16(buf, &u)) return false;
    *out = (int16_t)u; return true;
}
bool owl_le_buffer_read_i32(OwlLeBuffer* buf, int32_t* out) {
    uint32_t u; if (!owl_le_buffer_read_u32(buf, &u)) return false;
    *out = (int32_t)u; return true;
}
bool owl_le_buffer_read_i64(OwlLeBuffer* buf, int64_t* out) {
    uint64_t u; if (!owl_le_buffer_read_u64(buf, &u)) return false;
    *out = (int64_t)u; return true;
}

bool owl_le_buffer_read_bytes(OwlLeBuffer* buf, void* dst, size_t n) {
    if (buf->pos + n > buf->length) return false;
    if (n > 0) memcpy(dst, buf->data + buf->pos, n);
    buf->pos += n;
    return true;
}

bool owl_le_buffer_read_skip(OwlLeBuffer* buf, size_t n) {
    if (buf->pos + n > buf->length) return false;
    buf->pos += n;
    return true;
}

const char* owl_le_buffer_read_cstring(OwlLeBuffer* buf) {
    size_t start = buf->pos;
    while (buf->pos < buf->length) {
        if (buf->data[buf->pos] == 0) {
            const char* result = (const char*)(buf->data + start);
            buf->pos++;
            return result;
        }
        buf->pos++;
    }
    buf->pos = start;
    return NULL;
}

bool owl_le_buffer_read_lenenc_int(OwlLeBuffer* buf, uint64_t* out) {
    /* Sauve pos pour rollback en cas de truncation. */
    size_t savedPos = buf->pos;
    uint8_t first;
    if (!owl_le_buffer_read_u8(buf, &first)) return false;

    if (first < 0xFB) {
        *out = (uint64_t)first;
        return true;
    }
    if (first == 0xFB) {
        /* NULL sentinel (utilise dans les resultats text mode pour SQL NULL).
         * On retourne UINT64_MAX comme convention "ce n'est pas un int". Le
         * caller doit gerer specifiquement le cas NULL avant. Pour les autres
         * usages c'est un format error. */
        *out = (uint64_t)-1;
        return true;
    }
    if (first == 0xFC) {
        uint16_t v;
        if (!owl_le_buffer_read_u16(buf, &v)) { buf->pos = savedPos; return false; }
        *out = (uint64_t)v;
        return true;
    }
    if (first == 0xFD) {
        uint32_t v;
        if (!owl_le_buffer_read_u24(buf, &v)) { buf->pos = savedPos; return false; }
        *out = (uint64_t)v;
        return true;
    }
    if (first == 0xFE) {
        uint64_t v;
        if (!owl_le_buffer_read_u64(buf, &v)) { buf->pos = savedPos; return false; }
        *out = v;
        return true;
    }
    /* 0xFF est reserve pour les ErrorPacket en MySQL, jamais un lenenc int. */
    buf->pos = savedPos;
    return false;
}

const uint8_t* owl_le_buffer_read_lenenc_str(OwlLeBuffer* buf, size_t* out_len) {
    size_t savedPos = buf->pos;
    uint64_t len;
    if (!owl_le_buffer_read_lenenc_int(buf, &len)) return NULL;
    if (len == (uint64_t)-1) {
        /* NULL : convention specifique au caller (resultset text mode). */
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (buf->pos + (size_t)len > buf->length) {
        buf->pos = savedPos;
        return NULL;
    }
    const uint8_t* p = buf->data + buf->pos;
    buf->pos += (size_t)len;
    if (out_len) *out_len = (size_t)len;
    return p;
}

/* ---------- Introspection ---------- */

size_t owl_le_buffer_remaining(const OwlLeBuffer* buf) {
    return (buf->pos > buf->length) ? 0 : (buf->length - buf->pos);
}

bool owl_le_buffer_eof(const OwlLeBuffer* buf) {
    return buf->pos >= buf->length;
}
