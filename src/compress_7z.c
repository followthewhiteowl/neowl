// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* compress_7z.c — 7z archive read-only backend via LZMA SDK 23.01
 *
 * Wraps the LZMA SDK's CSzArEx API to give OWL's stdlib_compression a
 * minimal but complete 7z reader: open, list, extract by index or by
 * name, to disk or to memory. Write path is Phase 3 (needs Lzma2Enc
 * + CSzArEx writer, ~2x the linked size).
 *
 * Solid-block cache: SzArEx_Extract reuses `outBuffer` across calls
 * within the same solid block. We keep the cache on the state struct
 * and let each extract call pass the previous values; this cuts
 * per-file decode time on solid archives from O(nb_files²) to O(n).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "CpuArch.h"

/* Opaque state handed back to OWL. */
typedef struct {
    CFileInStream archiveStream;
    CLookToRead2  lookStream;
    CSzArEx       db;
    ISzAlloc      alloc;
    ISzAlloc      allocTemp;
    int           initialized;      /* 1 after SzArEx_Open succeeded */
    int           crc_inited;       /* CrcGenerateTable() called globally */
    Byte*         block_buf;        /* solid-block cache */
    size_t        block_buf_size;
    UInt32        last_block_index;
    /* Cached UTF-8 filenames (computed lazily on first listing request). */
    char**        names;            /* owned, strdup'd */
    int           name_count;
} SevenZState;

/* Global CRC table init (SDK needs it once per process). Idempotent via
 * sentinel — multiple concurrent openings are fine. */
static int g_crc_table_ready = 0;
static void ensure_crc_table(void) {
    if (!g_crc_table_ready) {
        CrcGenerateTable();
        g_crc_table_ready = 1;
    }
}

/* UTF-16LE → UTF-8 conversion. The SDK stores names as UTF-16; the
 * rest of OWL (and disk paths) speak UTF-8. Handles BMP + surrogate
 * pairs. Returns malloc'd buffer (caller frees). */
static char* utf16_to_utf8(const UInt16* src, size_t src_len) {
    /* Conservative upper bound: each UTF-16 unit → at most 3 bytes
     * UTF-8 (surrogates fold to 4 total which matches 2 UTF-16 → 4 UTF-8). */
    size_t cap = src_len * 3 + 1;
    char*  out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        UInt32 c = src[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < src_len) {
            UInt32 c2 = src[i + 1];
            if (c2 >= 0xDC00 && c2 < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 |  (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 |  (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 |  (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 |  (c & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

/* Resolve + cache all filenames in UTF-8. Called lazily from the
 * first zipListeFichier / zipChercheFichier request. */
static void sevenz_load_names(SevenZState* s) {
    if (s->names) return;
    s->name_count = (int)s->db.NumFiles;
    s->names = (char**)calloc((size_t)s->name_count, sizeof(char*));
    if (!s->names) return;

    UInt16* tmp = NULL;
    size_t  tmp_cap = 0;
    for (int i = 0; i < s->name_count; i++) {
        size_t len = SzArEx_GetFileNameUtf16(&s->db, (size_t)i, NULL);
        if (len > tmp_cap) {
            tmp_cap = len;
            tmp = (UInt16*)realloc(tmp, tmp_cap * sizeof(UInt16));
        }
        SzArEx_GetFileNameUtf16(&s->db, (size_t)i, tmp);
        /* tmp is NUL-terminated (len includes trailing 0). */
        s->names[i] = utf16_to_utf8(tmp, len > 0 ? len - 1 : 0);
    }
    free(tmp);
}

/* ------------------------------------------------------------------
 * PUBLIC API — called from stdlib_compression.c
 * ------------------------------------------------------------------ */

void* owl_7z_open_reader(const char* path) {
    ensure_crc_table();
    SevenZState* s = (SevenZState*)calloc(1, sizeof(SevenZState));
    if (!s) return NULL;

    s->alloc.Alloc     = SzAlloc;
    s->alloc.Free      = SzFree;
    s->allocTemp.Alloc = SzAllocTemp;
    s->allocTemp.Free  = SzFreeTemp;
    s->last_block_index = 0xFFFFFFFF; /* "no cached block yet" */

    if (InFile_Open(&s->archiveStream.file, path) != 0) { free(s); return NULL; }
    FileInStream_CreateVTable(&s->archiveStream);
    s->archiveStream.wres = 0;

    LookToRead2_CreateVTable(&s->lookStream, False);
    s->lookStream.buf = (Byte*)ISzAlloc_Alloc(&s->alloc, 1 << 18);
    if (!s->lookStream.buf) {
        File_Close(&s->archiveStream.file);
        free(s);
        return NULL;
    }
    s->lookStream.bufSize    = 1 << 18;
    s->lookStream.realStream = &s->archiveStream.vt;
    LookToRead2_INIT(&s->lookStream)

    SzArEx_Init(&s->db);
    SRes r = SzArEx_Open(&s->db, &s->lookStream.vt, &s->alloc, &s->allocTemp);
    if (r != SZ_OK) {
        SzArEx_Free(&s->db, &s->alloc);
        ISzAlloc_Free(&s->alloc, s->lookStream.buf);
        File_Close(&s->archiveStream.file);
        free(s);
        return NULL;
    }
    s->initialized = 1;
    return s;
}

int owl_7z_num_files(void* state) {
    if (!state) return 0;
    return (int)((SevenZState*)state)->db.NumFiles;
}

/* Returns a pointer to an internally-owned UTF-8 filename. Valid until
 * the archive is closed. Returns NULL if index out of range. */
const char* owl_7z_filename(void* state, int index) {
    SevenZState* s = (SevenZState*)state;
    if (!s || index < 0 || index >= (int)s->db.NumFiles) return NULL;
    sevenz_load_names(s);
    if (!s->names) return NULL;
    return s->names[index];
}

int owl_7z_is_dir(void* state, int index) {
    SevenZState* s = (SevenZState*)state;
    if (!s || index < 0 || index >= (int)s->db.NumFiles) return 0;
    return SzArEx_IsDir(&s->db, index) ? 1 : 0;
}

/* Search a filename by exact match. Returns 1-based index (canonical
 * convention) or 0 if not found. */
int owl_7z_find(void* state, const char* name) {
    SevenZState* s = (SevenZState*)state;
    if (!s || !name) return 0;
    sevenz_load_names(s);
    if (!s->names) return 0;
    for (int i = 0; i < s->name_count; i++) {
        if (s->names[i] && strcmp(s->names[i], name) == 0) return i + 1;
    }
    return 0;
}

/* Extract one file by index. On success allocates `*out` (caller
 * frees). Returns 0 on success, -1 on index out of range, -2 on
 * decoding error, -3 on memory error. */
int owl_7z_extract(void* state, int index, uint8_t** out, size_t* out_len) {
    SevenZState* s = (SevenZState*)state;
    if (!s || !out || !out_len) return -3;
    if (index < 0 || index >= (int)s->db.NumFiles) return -1;
    *out = NULL; *out_len = 0;

    size_t offset = 0, processed = 0;
    SRes r = SzArEx_Extract(&s->db, &s->lookStream.vt, (UInt32)index,
                             &s->last_block_index,
                             &s->block_buf, &s->block_buf_size,
                             &offset, &processed,
                             &s->alloc, &s->allocTemp);
    if (r != SZ_OK) return -2;

    uint8_t* data = (uint8_t*)malloc(processed > 0 ? processed : 1);
    if (!data) return -3;
    if (processed > 0) memcpy(data, s->block_buf + offset, processed);
    *out = data;
    *out_len = processed;
    return 0;
}

uint64_t owl_7z_file_size(void* state, int index) {
    SevenZState* s = (SevenZState*)state;
    if (!s || index < 0 || index >= (int)s->db.NumFiles) return 0;
    return (uint64_t)SzArEx_GetFileSize(&s->db, index);
}

void owl_7z_close(void* state) {
    SevenZState* s = (SevenZState*)state;
    if (!s) return;
    if (s->names) {
        for (int i = 0; i < s->name_count; i++) free(s->names[i]);
        free(s->names);
    }
    if (s->block_buf) ISzAlloc_Free(&s->alloc, s->block_buf);
    if (s->initialized) SzArEx_Free(&s->db, &s->alloc);
    if (s->lookStream.buf) ISzAlloc_Free(&s->alloc, s->lookStream.buf);
    File_Close(&s->archiveStream.file);
    free(s);
}
