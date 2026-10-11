// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_hfsql_install.h"
#include "owl_hfsql_driver.h"

#if defined(OWL_HAS_HFSQL_EMBED) && OWL_HAS_HFSQL_EMBED

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>     /* SHGetFolderPathA pour LOCALAPPDATA */
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#endif

/* LZMA decoder : on lit le pack au format .lzma (LZMA1, header 13 B
 * + flux brut). Le LZMA SDK vendore expose LzmaDecode() directement. */
#include "LzmaDec.h"
#include "Alloc.h"

/* Symbols generes par objcopy --input binary depuis hfsql_pack.tar.lzma.
 * objcopy produit toujours ces 3 symboles : start, end, size. */
extern const unsigned char _binary_hfsql_pack_tar_lzma_start[];
extern const unsigned char _binary_hfsql_pack_tar_lzma_end[];

/* Version du pack vendore. v29 (driver ODBC pre-rachat) sur les deux
 * plateformes. Bump = re-extraction (un dossier different sera utilise).
 * Le nom du fichier driver lui-meme vit dans owl_hfsql_driver.h (source
 * unique partagee avec le loader, cf OWL_HFSQL_DRIVER_DLL_NAME). */
#ifdef _WIN32
#define OWL_HFSQL_PACK_VERSION "v290"
#define OWL_PATH_SEP_C         '\\'
#define OWL_PATH_SEP_S         "\\"
#define OWL_MAX_PATH           MAX_PATH
#else
#define OWL_HFSQL_PACK_VERSION "v290"
#define OWL_PATH_SEP_C         '/'
#define OWL_PATH_SEP_S         "/"
#define OWL_MAX_PATH           4096
#endif

/* ================================================================
 * Helpers
 * ================================================================ */

static void hf_err(char* err_out, size_t err_cap, const char* fmt, ...) {
    if (!err_out || err_cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_out, err_cap, fmt, ap);
    va_end(ap);
}

/* Resout le path d'install :
 *  - Windows : %LOCALAPPDATA%/owl/hfsql_driver/<version>/
 *  - POSIX   : $XDG_DATA_HOME/owl/hfsql_driver/<version>/
 *              fallback $HOME/.local/share/owl/hfsql_driver/<version>/
 *
 * Remplit out_buf. Renvoie true si tout tient dans le buffer. */
bool owl_hfsql_get_install_dir(char* out_buf, size_t out_cap) {
    int n;
#ifdef _WIN32
    char base[MAX_PATH];
    HRESULT hr = SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL,
                                    SHGFP_TYPE_CURRENT, base);
    if (FAILED(hr)) return false;
    n = snprintf(out_buf, out_cap, "%s\\owl\\hfsql_driver\\%s",
                 base, OWL_HFSQL_PACK_VERSION);
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        n = snprintf(out_buf, out_cap, "%s/owl/hfsql_driver/%s",
                     xdg, OWL_HFSQL_PACK_VERSION);
    } else {
        const char* home = getenv("HOME");
        if (!home || !home[0]) return false;
        n = snprintf(out_buf, out_cap, "%s/.local/share/owl/hfsql_driver/%s",
                     home, OWL_HFSQL_PACK_VERSION);
    }
#endif
    return n > 0 && (size_t)n < out_cap;
}

/* Version interne qui alloue. */
static char* hfsql_target_dir(void) {
    char buf[OWL_MAX_PATH];
    if (!owl_hfsql_get_install_dir(buf, sizeof(buf))) return NULL;
    char* out = (char*)malloc(strlen(buf) + 1);
    if (out) strcpy(out, buf);
    return out;
}

/* Cree un repertoire et ses parents si necessaire (mkdir -p). */
static bool hfsql_mkdir_p(const char* path) {
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return true;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    char buf[MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);
    char* slash = strrchr(buf, '\\');
    if (!slash) return false;
    *slash = '\0';
    if (!hfsql_mkdir_p(buf)) return false;
    return (CreateDirectoryA(path, NULL) ||
            GetLastError() == ERROR_ALREADY_EXISTS);
#else
    if (mkdir(path, 0755) == 0) return true;
    if (errno == EEXIST) return true;
    if (errno != ENOENT) return false;
    /* Parent manquant : creation recursive. */
    char buf[OWL_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);
    char* slash = strrchr(buf, '/');
    if (!slash || slash == buf) return false;
    *slash = '\0';
    if (!hfsql_mkdir_p(buf)) return false;
    return (mkdir(path, 0755) == 0 || errno == EEXIST);
#endif
}

/* Test si un fichier existe. */
static bool hfsql_file_exists(const char* path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
#endif
}

/* ================================================================
 * Decompression LZMA1 du blob embarque
 *
 * Format .lzma (xz --format=lzma) :
 *   bytes 0..4  : 5 bytes de proprietes LZMA (lc/lp/pb + dict size)
 *   bytes 5..12 : taille decompressee en uint64 little-endian
 *                  (0xFFFFFFFFFFFFFFFF si inconnu — pas notre cas)
 *   bytes 13..  : flux LZMA1 compresse
 * ================================================================ */
static bool hfsql_lzma_decompress_pack(uint8_t** out_buf, size_t* out_size,
                                         char* err_out, size_t err_cap) {
    const unsigned char* src = _binary_hfsql_pack_tar_lzma_start;
    size_t src_size = (size_t)(_binary_hfsql_pack_tar_lzma_end - src);
    if (src_size < 13) {
        hf_err(err_out, err_cap, "blob LZMA trop court (%zu B)", src_size);
        return false;
    }
    uint64_t un_size = 0;
    for (int i = 0; i < 8; ++i) un_size |= ((uint64_t)src[5 + i]) << (8 * i);
    if (un_size == UINT64_MAX) {
        un_size = 200ULL * 1024 * 1024;
    }
    if (un_size == 0 || un_size > 200ULL * 1024 * 1024) {
        hf_err(err_out, err_cap,
               "taille decompressee invalide (%llu)",
               (unsigned long long)un_size);
        return false;
    }
    uint8_t* dst = (uint8_t*)malloc((size_t)un_size);
    if (!dst) {
        hf_err(err_out, err_cap, "OOM alloc %llu B",
               (unsigned long long)un_size);
        return false;
    }
    SizeT dst_len = (SizeT)un_size;
    SizeT stream_len = (SizeT)(src_size - 13);
    ELzmaStatus status;
    SRes rc = LzmaDecode(dst, &dst_len,
                          src + 13, &stream_len,
                          src, 5,
                          LZMA_FINISH_END, &status,
                          &g_Alloc);
    if (rc != SZ_OK) {
        hf_err(err_out, err_cap, "LzmaDecode echec rc=%d status=%d",
               (int)rc, (int)status);
        free(dst);
        return false;
    }
    *out_buf = dst;
    *out_size = (size_t)dst_len;
    return true;
}

/* ================================================================
 * Parse tar POSIX ustar dans une zone memoire
 * ================================================================ */
static uint64_t tar_parse_octal(const char* s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == 0 || s[i] == ' ') break;
        if (s[i] < '0' || s[i] > '7') return 0;
        v = v * 8 + (uint64_t)(s[i] - '0');
    }
    return v;
}

static bool hfsql_write_file(const char* path, const uint8_t* data,
                              size_t size, char* err_out, size_t err_cap) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        hf_err(err_out, err_cap, "CreateFile %s echec (code %lu)",
               path, GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, NULL);
    CloseHandle(h);
    if (!ok || written != (DWORD)size) {
        hf_err(err_out, err_cap, "WriteFile %s echec (%lu/%zu)",
               path, (unsigned long)written, size);
        return false;
    }
    return true;
#else
    FILE* f = fopen(path, "wb");
    if (!f) {
        hf_err(err_out, err_cap, "fopen %s echec : %s",
               path, strerror(errno));
        return false;
    }
    size_t written = fwrite(data, 1, size, f);
    int close_rc = fclose(f);
    if (written != size || close_rc != 0) {
        hf_err(err_out, err_cap, "fwrite %s echec (%zu/%zu)",
               path, written, size);
        return false;
    }
    /* Permissions executables (libraries .so doivent etre lisibles +
     * executables sur certains montages, et le bit x est attendu pour
     * dlopen avec certains LSM/AppArmor). */
    chmod(path, 0755);
    return true;
#endif
}

static bool hfsql_tar_extract(const uint8_t* tar, size_t tar_size,
                               const char* target_dir,
                               char* err_out, size_t err_cap) {
    size_t off = 0;
    while (off + 512 <= tar_size) {
        const uint8_t* hdr = tar + off;
        bool is_zero = true;
        for (int i = 0; i < 512; ++i) if (hdr[i]) { is_zero = false; break; }
        if (is_zero) break;

        char name[101];
        memcpy(name, hdr, 100);
        name[100] = '\0';

        uint64_t size = tar_parse_octal((const char*)(hdr + 124), 12);
        char typeflag = (char)hdr[156];
        off += 512;

        if (off + size > tar_size) {
            hf_err(err_out, err_cap, "tar: fichier %s deborde", name);
            return false;
        }

        if (typeflag == '0' || typeflag == '\0') {
            char out_path[OWL_MAX_PATH];
            snprintf(out_path, sizeof(out_path), "%s%s%s",
                     target_dir, OWL_PATH_SEP_S, name);
            if (!hfsql_write_file(out_path, tar + off, (size_t)size,
                                    err_out, err_cap)) {
                return false;
            }
        }
        off += (size_t)size;
        if (size & 511) off += 512 - (size_t)(size & 511);
    }
    return true;
}

static bool hfsql_extract_pack(const char* target_dir,
                                 char* err_out, size_t err_cap) {
    uint8_t* tar_buf = NULL;
    size_t tar_size = 0;
    if (!hfsql_lzma_decompress_pack(&tar_buf, &tar_size, err_out, err_cap)) {
        return false;
    }
    bool ok = hfsql_tar_extract(tar_buf, tar_size, target_dir,
                                  err_out, err_cap);
    free(tar_buf);
    return ok;
}

/* ================================================================
 * Public API : ensure installed (idempotent)
 *
 * Idempotence basee uniquement sur la presence physique de la
 * bibliotheque dans le dossier de la version courante. Chargement
 * direct : pas d'enregistrement systeme requis.
 * ================================================================ */
bool owl_hfsql_ensure_installed(char* err_out, size_t err_cap) {
    if (err_out && err_cap > 0) err_out[0] = '\0';

    char* target_dir = hfsql_target_dir();
    if (!target_dir) {
        hf_err(err_out, err_cap, "resolve target_dir echec");
        return false;
    }

    /* Check : driver deja extrait pour cette version ? */
    char driver_path[OWL_MAX_PATH];
    snprintf(driver_path, sizeof(driver_path), "%s%s%s",
             target_dir, OWL_PATH_SEP_S, OWL_HFSQL_DRIVER_DLL_NAME);
    if (hfsql_file_exists(driver_path)) {
        free(target_dir);
        return true;
    }

    /* Sinon extraction fraiche. */
    if (!hfsql_mkdir_p(target_dir)) {
        hf_err(err_out, err_cap, "mkdir %s echec", target_dir);
        free(target_dir);
        return false;
    }
    if (!hfsql_extract_pack(target_dir, err_out, err_cap)) {
        free(target_dir);
        return false;
    }
    free(target_dir);

    if (!hfsql_file_exists(driver_path)) {
        hf_err(err_out, err_cap, "post-extract: %s manquant", driver_path);
        return false;
    }
    return true;
}

#endif /* OWL_HAS_HFSQL_EMBED */
