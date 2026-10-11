// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "net_be_buffer.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Initial capacity / growth strategy
 *
 * Démarre à 64 octets par défaut, double au besoin. Capable de monter
 * jusqu'à ~SIZE_MAX/2 sans overflow. Pas de plafond explicite — le
 * caller doit valider les longueurs en amont s'il vient d'une source
 * non-fiable (network buffer overflow protection).
 * ================================================================ */

#define DEFAULT_CAPACITY 64

/* ---------- Construction / teardown ---------- */

void owl_be_buffer_init(OwlBeBuffer* buf, size_t initial_capacity) {
    if (initial_capacity == 0) initial_capacity = DEFAULT_CAPACITY;
    buf->data     = (uint8_t*)malloc(initial_capacity);
    buf->length   = 0;
    buf->capacity = (buf->data != NULL) ? initial_capacity : 0;
    buf->pos      = 0;
}

void owl_be_buffer_free(OwlBeBuffer* buf) {
    /* On free uniquement si on est "owner" (capacity > 0 + data != NULL).
     * Un buffer attaché via attach_read n'a pas alloué — on ne touche pas. */
    if (buf->capacity > 0 && buf->data != NULL) {
        free(buf->data);
    }
    buf->data     = NULL;
    buf->length   = 0;
    buf->capacity = 0;
    buf->pos      = 0;
}

void owl_be_buffer_reset(OwlBeBuffer* buf) {
    buf->length = 0;
    buf->pos    = 0;
}

void owl_be_buffer_attach_read(OwlBeBuffer* buf, const uint8_t* data, size_t length) {
    /* attach_read : data est read-only et non-owné. On signale ça en
     * mettant capacity = 0 pour que _free ne libère pas. */
    buf->data     = (uint8_t*)data;   /* cast away const : on ne mute jamais en mode read */
    buf->length   = length;
    buf->capacity = 0;                /* 0 = non-owné */
    buf->pos      = 0;
}

/* ---------- Internal grow ---------- */

static void ensure_capacity(OwlBeBuffer* buf, size_t needed) {
    if (buf->capacity >= needed) return;
    size_t newCap = buf->capacity == 0 ? DEFAULT_CAPACITY : buf->capacity;
    while (newCap < needed) {
        /* Double-growth, capped pour éviter overflow. */
        if (newCap >= (SIZE_MAX / 2)) { newCap = needed; break; }
        newCap *= 2;
    }
    uint8_t* nd = (uint8_t*)realloc(buf->data, newCap);
    /* OOM : on garde l'ancien data, on échoue silencieusement (la prochaine
     * write_* écrira dans la capacité dispo seulement). Caller détecte via
     * buf->length post-write. En pratique OWL ne tient pas un wrapper OOM
     * formel — on suit la convention C : malloc/realloc échoué = crash plus
     * tard si on déréfère. Ici on minimise l'impact. */
    if (nd == NULL) return;
    buf->data     = nd;
    buf->capacity = newCap;
}

/* ---------- Write ---------- */

void owl_be_buffer_write_u8(OwlBeBuffer* buf, uint8_t v) {
    ensure_capacity(buf, buf->length + 1);
    if (buf->length + 1 > buf->capacity) return;  /* OOM silent */
    buf->data[buf->length++] = v;
}

void owl_be_buffer_write_u16(OwlBeBuffer* buf, uint16_t v) {
    ensure_capacity(buf, buf->length + 2);
    if (buf->length + 2 > buf->capacity) return;
    buf->data[buf->length++] = (uint8_t)(v >> 8);
    buf->data[buf->length++] = (uint8_t)(v & 0xFF);
}

void owl_be_buffer_write_u32(OwlBeBuffer* buf, uint32_t v) {
    ensure_capacity(buf, buf->length + 4);
    if (buf->length + 4 > buf->capacity) return;
    buf->data[buf->length++] = (uint8_t)((v >> 24) & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >> 16) & 0xFF);
    buf->data[buf->length++] = (uint8_t)((v >>  8) & 0xFF);
    buf->data[buf->length++] = (uint8_t)( v        & 0xFF);
}

void owl_be_buffer_write_u64(OwlBeBuffer* buf, uint64_t v) {
    ensure_capacity(buf, buf->length + 8);
    if (buf->length + 8 > buf->capacity) return;
    for (int i = 7; i >= 0; i--) {
        buf->data[buf->length++] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
}

void owl_be_buffer_write_i16(OwlBeBuffer* buf, int16_t v) {
    owl_be_buffer_write_u16(buf, (uint16_t)v);
}
void owl_be_buffer_write_i32(OwlBeBuffer* buf, int32_t v) {
    owl_be_buffer_write_u32(buf, (uint32_t)v);
}
void owl_be_buffer_write_i64(OwlBeBuffer* buf, int64_t v) {
    owl_be_buffer_write_u64(buf, (uint64_t)v);
}

void owl_be_buffer_write_bytes(OwlBeBuffer* buf, const void* src, size_t n) {
    if (n == 0) return;
    ensure_capacity(buf, buf->length + n);
    if (buf->length + n > buf->capacity) return;
    memcpy(buf->data + buf->length, src, n);
    buf->length += n;
}

void owl_be_buffer_write_cstring(OwlBeBuffer* buf, const char* s) {
    size_t len = (s != NULL) ? strlen(s) : 0;
    /* +1 pour le '\0' terminal */
    ensure_capacity(buf, buf->length + len + 1);
    if (buf->length + len + 1 > buf->capacity) return;
    if (len > 0) memcpy(buf->data + buf->length, s, len);
    buf->length += len;
    buf->data[buf->length++] = 0;
}

void owl_be_buffer_patch_u32(OwlBeBuffer* buf, size_t offset, uint32_t v) {
    if (offset + 4 > buf->length) return;  /* hors limite : ignore */
    buf->data[offset    ] = (uint8_t)((v >> 24) & 0xFF);
    buf->data[offset + 1] = (uint8_t)((v >> 16) & 0xFF);
    buf->data[offset + 2] = (uint8_t)((v >>  8) & 0xFF);
    buf->data[offset + 3] = (uint8_t)( v        & 0xFF);
}

/* ---------- Read ---------- */

bool owl_be_buffer_read_u8(OwlBeBuffer* buf, uint8_t* out) {
    if (buf->pos + 1 > buf->length) return false;
    *out = buf->data[buf->pos++];
    return true;
}

bool owl_be_buffer_read_u16(OwlBeBuffer* buf, uint16_t* out) {
    if (buf->pos + 2 > buf->length) return false;
    *out = ((uint16_t)buf->data[buf->pos    ] << 8)
         | ((uint16_t)buf->data[buf->pos + 1]);
    buf->pos += 2;
    return true;
}

bool owl_be_buffer_read_u32(OwlBeBuffer* buf, uint32_t* out) {
    if (buf->pos + 4 > buf->length) return false;
    *out = ((uint32_t)buf->data[buf->pos    ] << 24)
         | ((uint32_t)buf->data[buf->pos + 1] << 16)
         | ((uint32_t)buf->data[buf->pos + 2] <<  8)
         | ((uint32_t)buf->data[buf->pos + 3]);
    buf->pos += 4;
    return true;
}

bool owl_be_buffer_read_u64(OwlBeBuffer* buf, uint64_t* out) {
    if (buf->pos + 8 > buf->length) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t)buf->data[buf->pos + i];
    }
    *out = v;
    buf->pos += 8;
    return true;
}

bool owl_be_buffer_read_i16(OwlBeBuffer* buf, int16_t* out) {
    uint16_t u; if (!owl_be_buffer_read_u16(buf, &u)) return false;
    *out = (int16_t)u; return true;
}
bool owl_be_buffer_read_i32(OwlBeBuffer* buf, int32_t* out) {
    uint32_t u; if (!owl_be_buffer_read_u32(buf, &u)) return false;
    *out = (int32_t)u; return true;
}
bool owl_be_buffer_read_i64(OwlBeBuffer* buf, int64_t* out) {
    uint64_t u; if (!owl_be_buffer_read_u64(buf, &u)) return false;
    *out = (int64_t)u; return true;
}

bool owl_be_buffer_read_bytes(OwlBeBuffer* buf, void* dst, size_t n) {
    if (buf->pos + n > buf->length) return false;
    if (n > 0) memcpy(dst, buf->data + buf->pos, n);
    buf->pos += n;
    return true;
}

const char* owl_be_buffer_read_cstring(OwlBeBuffer* buf) {
    /* Cherche '\0' dans les octets restants. */
    size_t start = buf->pos;
    while (buf->pos < buf->length) {
        if (buf->data[buf->pos] == 0) {
            const char* result = (const char*)(buf->data + start);
            buf->pos++;  /* avance après le '\0' */
            return result;
        }
        buf->pos++;
    }
    /* Pas de '\0' trouvé : rewind pos, retourne NULL. */
    buf->pos = start;
    return NULL;
}

/* ---------- Introspection ---------- */

size_t owl_be_buffer_remaining(const OwlBeBuffer* buf) {
    return (buf->pos > buf->length) ? 0 : (buf->length - buf->pos);
}

bool owl_be_buffer_eof(const OwlBeBuffer* buf) {
    return buf->pos >= buf->length;
}
