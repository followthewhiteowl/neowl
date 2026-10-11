// SPDX-License-Identifier: AGPL-3.0-only
/* payload_extract.c — Detection + extraction du payload single-file
 * appende a la fin de owl-build (PyInstaller-style).
 *
 * Layout du megafile :
 *   [owl-build.exe normal]      bytes 0..N
 *   [zip payload]               bytes N..M
 *   [footer 24 bytes]           bytes M..end
 *      offset (uint64 LE)       8 bytes
 *      size   (uint64 LE)       8 bytes
 *      magic  "OWLBUNDL"        8 bytes
 *
 * Au runtime, owl-build :
 *   1. ouvre argv[0]
 *   2. lit footer (-24 bytes)
 *   3. si magic OK : extract le zip dans <TEMP>/owl-build-<size>/ (cache)
 *   4. owlRoot = ce dossier extrait
 */

#include "payload_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define PATH_SEP '\\'
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define PATH_SEP '/'
#endif

#include "miniz.h"

#define OWL_PAYLOAD_MAGIC "OWLBUNDL"
#define OWL_PAYLOAD_FOOTER_SIZE 24

int owl_get_self_path(char* out, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) return -1;
    return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(out, &size);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
#endif
}

int owl_payload_check(const char* self_path,
                      uint64_t* out_offset, uint64_t* out_size) {
    FILE* f = fopen(self_path, "rb");
    if (!f) return -1;
    if (fseek(f, -OWL_PAYLOAD_FOOTER_SIZE, SEEK_END) != 0) {
        fclose(f); return -1;
    }
    uint8_t buf[OWL_PAYLOAD_FOOTER_SIZE];
    if (fread(buf, 1, OWL_PAYLOAD_FOOTER_SIZE, f) != OWL_PAYLOAD_FOOTER_SIZE) {
        fclose(f); return -1;
    }
    fclose(f);
    if (memcmp(buf + 16, OWL_PAYLOAD_MAGIC, 8) != 0) return -1;
    uint64_t off = 0, sz = 0;
    for (int i = 0; i < 8; i++) {
        off |= ((uint64_t)buf[i]) << (i * 8);
        sz  |= ((uint64_t)buf[8 + i]) << (i * 8);
    }
    *out_offset = off;
    *out_size = sz;
    return 0;
}

static int mkdirs(const char* path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    /* Iterate / create chaque parent */
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i]; buf[i] = '\0';
            MKDIR(buf);
            buf[i] = c;
        }
    }
    MKDIR(buf);
    return 0;
}

static int file_exists_local(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int owl_payload_extract(const char* self_path,
                         uint64_t offset, uint64_t size,
                         const char* dest_dir) {
    FILE* f = fopen(self_path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return -1; }
    void* buf = malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) { free(buf); return -1; }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, buf, (size_t)size, 0)) {
        fprintf(stderr, "owl-build: invalid zip payload\n");
        free(buf);
        return -1;
    }

    int n = (int)mz_zip_reader_get_num_files(&zip);
    int errors = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { errors++; continue; }
        char outpath[1024];
        snprintf(outpath, sizeof(outpath), "%s/%s", dest_dir, st.m_filename);
        /* Normalise separateurs */
#ifdef _WIN32
        for (char* p = outpath; *p; p++) if (*p == '/') *p = '\\';
#endif
        if (st.m_is_directory) {
            mkdirs(outpath);
        } else {
            /* mkdir du parent */
            char parent[1024];
            snprintf(parent, sizeof(parent), "%s", outpath);
            char* slash = strrchr(parent, '/');
            char* bslash = strrchr(parent, '\\');
            if (bslash && (!slash || bslash > slash)) slash = bslash;
            if (slash) { *slash = '\0'; mkdirs(parent); }
            if (!mz_zip_reader_extract_to_file(&zip, (mz_uint)i, outpath, 0)) {
                fprintf(stderr, "owl-build: extract failed for %s\n", st.m_filename);
                errors++;
            }
#ifndef _WIN32
            /* Restore exec bit pour tout fichier extrait sur POSIX.
             * zip ne preserve pas les modes par defaut, et les binaires
             * (zig, owl-build interne, etc.) doivent etre executables.
             * Pas grave que les .a / .h soient aussi 0755, c'est sans
             * impact runtime. */
            chmod(outpath, 0755);
#endif
        }
    }
    mz_zip_reader_end(&zip);
    free(buf);
    return errors == 0 ? 0 : -1;
}

int owl_payload_setup(char* out_root, size_t cap) {
    char self_path[1024];
    if (owl_get_self_path(self_path, sizeof(self_path)) != 0) return -1;
    uint64_t offset, size;
    if (owl_payload_check(self_path, &offset, &size) != 0) return -1;

    /* Compute temp dir avec hash simple = size en hex (suffisant comme
     * cache key : un payload regenere change de taille au moindre commit). */
    const char* tempVar = NULL;
#ifdef _WIN32
    tempVar = getenv("TEMP");
    if (!tempVar) tempVar = getenv("TMP");
    if (!tempVar) tempVar = "C:\\Windows\\Temp";
#else
    tempVar = getenv("TMPDIR");
    if (!tempVar) tempVar = "/tmp";
#endif
    char temp_dir[1024];
    snprintf(temp_dir, sizeof(temp_dir), "%s%cowl-build-%llx",
             tempVar, PATH_SEP, (unsigned long long)size);
    char marker[1024];
    snprintf(marker, sizeof(marker), "%s%c.complete", temp_dir, PATH_SEP);
    if (!file_exists_local(marker)) {
        printf("owl-build: extracting payload (%.1f MB) to %s ...\n",
               (double)size / (1024.0 * 1024.0), temp_dir);
        mkdirs(temp_dir);
        if (owl_payload_extract(self_path, offset, size, temp_dir) != 0) {
            return -1;
        }
        FILE* m = fopen(marker, "w");
        if (m) { fputs("ok\n", m); fclose(m); }
    }
    snprintf(out_root, cap, "%s", temp_dir);
    return 0;
}
