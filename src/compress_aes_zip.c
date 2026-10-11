// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* compress_aes_zip.c — ZIP archive writer/reader with WinZip AES-2
 *
 * Miniz doesn't emit (or read) entries with compression method 99 +
 * extra field 0x9901, which is the WinZip AES-2 layout. To produce
 * archives compatible with WinZip / 7-Zip / Windows Explorer when a
 * password is set, we implement a thin ZIP writer and a companion
 * reader that live alongside miniz. Unencrypted archives keep going
 * through miniz as before — this module is activated only when
 * `ObjZipArchive.password != NULL`.
 *
 * Layout produced:
 *   Local file header (30 bytes + name_len + 11 bytes AES extra)
 *     ├── method field = 99 (0x63)
 *     ├── CRC32 = 0 (per AE-2 spec, actual CRC lives elsewhere)
 *     ├── compressed size = salt(16) + verifier(2) + deflate_stream + hmac(10)
 *     ├── uncompressed size = plaintext bytes
 *     └── extra field 0x9901 (7 bytes payload): AE version=2, "AE",
 *         strength=3 (AES-256), actual_method=8 (deflate)
 *   Encrypted payload: salt | verifier | AES-256-CTR(deflate(plain)) | hmac
 *   ... (repeat per entry)
 *   Central Directory: one record per entry (same metadata + local hdr offset)
 *   End-of-Central-Directory (EOCD)
 *
 * Interop verified against 7-Zip 23.x and Windows 11 Explorer.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "miniz.h"

/* --- Forward decl for the AE-2 primitives (src/compress_ae2.c). --- */
int owl_ae2_encrypt(const uint8_t* pt, size_t pt_len,
                     const char* password, size_t pw_len,
                     uint8_t** out, size_t* out_len);
int owl_ae2_decrypt(const uint8_t* in, size_t in_len,
                     const char* password, size_t pw_len,
                     uint8_t** out_pt, size_t* out_len);

/* --- Entry metadata tracked between add and finalize. --- */
typedef struct AesZipEntry {
    char*    name;            /* strdup */
    uint32_t crc32;           /* CRC of plaintext (informational; 0 in AE-2 local hdr) */
    uint32_t uncomp_size;     /* original plaintext bytes */
    uint32_t comp_size;       /* salt(16) + verifier(2) + deflate + hmac(10) */
    uint32_t local_offset;    /* where the local file header was written */
    uint16_t dos_time, dos_date;
    struct AesZipEntry* next;
} AesZipEntry;

typedef struct {
    FILE*         f;
    AesZipEntry*  head;
    AesZipEntry*  tail;
    int           count;
    int           compression_level;
} AesZipWriter;

/* --- Little-endian write helpers. --- */
static void w16(FILE* f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}
static void w32(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    fwrite(b, 1, 4, f);
}

/* Convert epoch time to DOS 2-second time + date. */
static void to_dos_time(time_t t, uint16_t* dos_time, uint16_t* dos_date) {
    struct tm tm_;
#ifdef _WIN32
    localtime_s(&tm_, &t);
#else
    localtime_r(&t, &tm_);
#endif
    /* DOS time: hour (5) | minute (6) | second/2 (5) */
    *dos_time = (uint16_t)((tm_.tm_hour << 11) | (tm_.tm_min << 5) | (tm_.tm_sec >> 1));
    /* DOS date: (year-1980) (7) | month (4) | day (5) */
    int y = tm_.tm_year + 1900 - 1980;
    if (y < 0) y = 0;
    *dos_date = (uint16_t)((y << 9) | ((tm_.tm_mon + 1) << 5) | tm_.tm_mday);
}

/* Build the 11-byte AES-2 extra field (tag 0x9901). */
static void make_aes_extra(uint8_t out[11], uint16_t actual_method) {
    out[0] = 0x01; out[1] = 0x99;              /* header ID 0x9901 (LE) */
    out[2] = 7;    out[3] = 0;                 /* data size = 7 */
    out[4] = 2;    out[5] = 0;                 /* AE version = 2 (AE-2) */
    out[6] = 'A';  out[7] = 'E';               /* vendor ID */
    out[8] = 3;                                /* strength: 3 = AES-256 */
    out[9] = (uint8_t)actual_method;           /* actual compression method */
    out[10] = (uint8_t)(actual_method >> 8);
}

AesZipWriter* aes_zip_writer_create(const char* path) {
    AesZipWriter* w = (AesZipWriter*)calloc(1, sizeof(AesZipWriter));
    if (!w) return NULL;
    w->f = fopen(path, "wb");
    if (!w->f) { free(w); return NULL; }
    w->compression_level = 6;
    return w;
}

void aes_zip_writer_set_level(AesZipWriter* w, int level) {
    if (!w) return;
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    w->compression_level = level;
}

/* Add an entry from memory with full encryption pipeline:
 *   1. Deflate plaintext (raw, no zlib header).
 *   2. Encrypt deflate output with WinZip AE-2 (AES-256-CTR + HMAC-SHA1).
 *   3. Write local file header + AES extra field + encrypted payload.
 *   4. Record entry metadata for the central directory. */
int aes_zip_writer_add(AesZipWriter* w, const char* name,
                        const uint8_t* data, size_t data_len,
                        const char* password) {
    if (!w || !name || !password) return -1;

    /* Step 1: deflate (raw, no zlib header via negative window_bits). */
    size_t defl_len = 0;
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(w->compression_level, -15, 0);
    void* defl = tdefl_compress_mem_to_heap(data, data_len, &defl_len, flags);
    if (!defl) return -1;

    /* Step 2: encrypt the deflate stream. */
    uint8_t* enc = NULL;
    size_t enc_len = 0;
    if (owl_ae2_encrypt((uint8_t*)defl, defl_len,
                         password, strlen(password),
                         &enc, &enc_len) < 0) {
        free(defl);
        return -1;
    }
    free(defl);

    /* Step 3: write the local file header. */
    size_t name_len = strlen(name);
    uint8_t extra[11];
    make_aes_extra(extra, 8 /* deflate */);

    long local_offset = ftell(w->f);

    uint16_t dt, dd;
    to_dos_time(time(NULL), &dt, &dd);

    w32(w->f, 0x04034b50);            /* local file header signature */
    w16(w->f, 51);                     /* version needed (5.1 for AE-2) */
    w16(w->f, 0x0001);                 /* gp flag: bit 0 = encrypted */
    w16(w->f, 99);                     /* method = AES */
    w16(w->f, dt);
    w16(w->f, dd);
    w32(w->f, 0);                       /* CRC32 = 0 per AE-2 spec */
    w32(w->f, (uint32_t)enc_len);       /* compressed size */
    w32(w->f, (uint32_t)data_len);      /* uncompressed size */
    w16(w->f, (uint16_t)name_len);
    w16(w->f, 11);                      /* extra field length */
    fwrite(name, 1, name_len, w->f);
    fwrite(extra, 1, 11, w->f);

    /* Step 4: write the encrypted payload. */
    fwrite(enc, 1, enc_len, w->f);

    /* Record metadata for the central directory pass. */
    uint32_t plain_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data, data_len);

    AesZipEntry* e = (AesZipEntry*)calloc(1, sizeof(AesZipEntry));
    e->name = (char*)malloc(name_len + 1);
    memcpy(e->name, name, name_len + 1);
    e->crc32       = plain_crc;
    e->uncomp_size = (uint32_t)data_len;
    e->comp_size   = (uint32_t)enc_len;
    e->local_offset = (uint32_t)local_offset;
    e->dos_time    = dt;
    e->dos_date    = dd;
    if (w->tail) { w->tail->next = e; w->tail = e; }
    else         { w->head = e; w->tail = e; }
    w->count++;

    free(enc);
    return 0;
}

/* Finalize: write the central directory + EOCD, then close the file. */
int aes_zip_writer_finalize(AesZipWriter* w) {
    if (!w) return -1;
    long cd_start = ftell(w->f);
    uint8_t extra[11];
    make_aes_extra(extra, 8);

    for (AesZipEntry* e = w->head; e; e = e->next) {
        size_t nl = strlen(e->name);
        w32(w->f, 0x02014b50);           /* central dir signature */
        w16(w->f, (10 << 8) | 51);        /* version made by (10=NTFS, 5.1) */
        w16(w->f, 51);                    /* version needed */
        w16(w->f, 0x0001);                /* gp flag */
        w16(w->f, 99);                    /* method */
        w16(w->f, e->dos_time);
        w16(w->f, e->dos_date);
        w32(w->f, 0);                      /* CRC32 in AE-2 = 0 */
        w32(w->f, e->comp_size);
        w32(w->f, e->uncomp_size);
        w16(w->f, (uint16_t)nl);
        w16(w->f, 11);                     /* extra len */
        w16(w->f, 0);                      /* comment len */
        w16(w->f, 0);                      /* disk start */
        w16(w->f, 0);                      /* internal attr */
        w32(w->f, 0);                      /* external attr */
        w32(w->f, e->local_offset);
        fwrite(e->name, 1, nl, w->f);
        fwrite(extra, 1, 11, w->f);
    }
    long cd_end = ftell(w->f);
    uint32_t cd_size = (uint32_t)(cd_end - cd_start);

    /* EOCD record. */
    w32(w->f, 0x06054b50);
    w16(w->f, 0);
    w16(w->f, 0);
    w16(w->f, (uint16_t)w->count);
    w16(w->f, (uint16_t)w->count);
    w32(w->f, cd_size);
    w32(w->f, (uint32_t)cd_start);
    w16(w->f, 0);                          /* comment len */

    fflush(w->f);
    fclose(w->f);
    w->f = NULL;
    return 0;
}

void aes_zip_writer_destroy(AesZipWriter* w) {
    if (!w) return;
    if (w->f) fclose(w->f);
    AesZipEntry* e = w->head;
    while (e) {
        AesZipEntry* n = e->next;
        free(e->name);
        free(e);
        e = n;
    }
    free(w);
}

/* ================================================================
 * READER — extract an AES-2 encrypted entry that was located via
 * miniz's mz_zip_reader (which DOES parse the central directory
 * correctly even when method=99; it only refuses the decompression
 * step). We re-open the file, seek to the local header, parse it,
 * read the encrypted payload, decrypt, and inflate.
 * ================================================================ */

/* Read a little-endian u16 / u32 from a file. */
static uint16_t rd_u16(FILE* f) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return 0;
    return (uint16_t)(b[0] | (b[1] << 8));
}
static uint32_t rd_u32(FILE* f) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

/* Extract an AES-2 entry at the given local header offset. Returns 0 on
 * success with `*out` / `*out_len` set (caller frees). On bad password
 * returns -1, on HMAC mismatch -2, on other errors -3. */
int aes_zip_extract_at_offset(const char* archive_path, uint32_t local_offset,
                                const char* password,
                                uint8_t** out, size_t* out_len) {
    if (!archive_path || !password || !out || !out_len) return -3;
    *out = NULL; *out_len = 0;

    FILE* f = fopen(archive_path, "rb");
    if (!f) return -3;
    if (fseek(f, (long)local_offset, SEEK_SET) != 0) { fclose(f); return -3; }

    if (rd_u32(f) != 0x04034b50) { fclose(f); return -3; }
    (void)rd_u16(f); /* version needed */
    (void)rd_u16(f); /* gp flag */
    uint16_t method = rd_u16(f);
    (void)rd_u16(f); /* dos time */
    (void)rd_u16(f); /* dos date */
    (void)rd_u32(f); /* CRC */
    uint32_t comp_size   = rd_u32(f);
    uint32_t uncomp_size = rd_u32(f);
    uint16_t name_len    = rd_u16(f);
    uint16_t extra_len   = rd_u16(f);

    if (method != 99) { fclose(f); return -3; }

    /* Skip filename, parse extra to find the AES tag for `actual_method`. */
    fseek(f, name_len, SEEK_CUR);

    uint16_t actual_method = 8; /* default deflate if extra malformed */
    long extra_start = ftell(f);
    long pos = 0;
    while (pos + 4 <= extra_len) {
        uint16_t tag = rd_u16(f);
        uint16_t sz  = rd_u16(f);
        if (tag == 0x9901 && sz >= 7) {
            (void)rd_u16(f); /* version */
            (void)rd_u16(f); /* "AE" */
            (void)fgetc(f);  /* strength */
            actual_method = rd_u16(f);
            break;
        }
        fseek(f, sz, SEEK_CUR);
        pos += 4 + sz;
    }
    /* Land at end of extra field regardless of what we parsed. */
    fseek(f, extra_start + extra_len, SEEK_SET);

    /* Read encrypted payload. */
    uint8_t* enc = (uint8_t*)malloc(comp_size);
    if (!enc) { fclose(f); return -3; }
    if (fread(enc, 1, comp_size, f) != comp_size) {
        free(enc); fclose(f); return -3;
    }
    fclose(f);

    /* Decrypt. */
    uint8_t* defl = NULL;
    size_t   defl_len = 0;
    int rc = owl_ae2_decrypt(enc, comp_size, password, strlen(password),
                              &defl, &defl_len);
    free(enc);
    if (rc != 0) return rc;

    /* Inflate (actual_method=8) or passthrough (actual_method=0). */
    if (actual_method == 0) {
        *out = defl;
        *out_len = defl_len;
        return 0;
    }
    if (actual_method != 8) {
        free(defl);
        return -3;
    }

    size_t plain_len = 0;
    void* plain = tinfl_decompress_mem_to_heap(defl, defl_len, &plain_len, 0);
    free(defl);
    if (!plain) return -3;
    if (plain_len != uncomp_size) {
        /* Size mismatch indicates corruption or a lie; reject. */
        free(plain);
        return -2;
    }
    *out = (uint8_t*)plain;
    *out_len = plain_len;
    return 0;
}
