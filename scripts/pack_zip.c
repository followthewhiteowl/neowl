// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* pack_zip.c — Mini packageur zip recursif via miniz.
 *
 * Pourquoi pas la cli `zip` : pas dispo par defaut sur MSYS2 / Windows.
 * miniz (~120 KB) est deja vendore dans owl pour le runtime, on l'utilise
 * aussi au build. Zero dep externe.
 *
 * Usage : pack_zip <source_dir> <output.zip>
 * Compresse en DEFLATE (niveau 6) : le payload est embarque dans
 * owl-build.exe, et un payload compresse divise la taille de distribution
 * de l'exe (download + scan initial de l'exe par l'antivirus). L'extraction
 * decompresse en transparence via miniz ; le cout CPU de la decompression
 * DEFLATE est negligeable devant l'ecriture disque et le scan antivirus de
 * l'arbre extrait.
 *
 * Build :
 *   gcc -O2 scripts/pack_zip.c vendor/miniz/miniz.c \
 *       -Ivendor/miniz -o scripts/pack_zip
 */

#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define PATH_SEP '\\'
#else
#  include <unistd.h>
#  define PATH_SEP '/'
#endif

#include "miniz.h"

static mz_zip_archive g_zip;
static const char* g_root = NULL;
static int g_added = 0;
static int g_errors = 0;

/* Recursion : ajoute tous les fichiers du dir absolu `abs_dir` au zip,
 * en utilisant `rel_prefix` comme prefixe d'entree zip (chemins relatifs
 * au root). */
static void walk_dir(const char* abs_dir, const char* rel_prefix) {
    DIR* d = opendir(abs_dir);
    if (!d) {
        fprintf(stderr, "pack_zip: cannot open '%s'\n", abs_dir);
        g_errors++;
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char abs_path[2048];
        char rel_path[2048];
        snprintf(abs_path, sizeof(abs_path), "%s%c%s", abs_dir, PATH_SEP, e->d_name);
        if (rel_prefix && *rel_prefix) {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, e->d_name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", e->d_name);
        }
        struct stat st;
        if (stat(abs_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_dir(abs_path, rel_path);
        } else if (S_ISREG(st.st_mode)) {
            /* DEFLATE niveau 6 : compresse le payload embarque dans
             * owl-build.exe pour reduire la taille de distribution. miniz
             * decompresse en transparence a l'extraction (cout CPU
             * negligeable vs ecriture disque / scan antivirus). */
            if (!mz_zip_writer_add_file(&g_zip, rel_path, abs_path,
                                        NULL, 0, MZ_DEFAULT_LEVEL)) {
                fprintf(stderr, "pack_zip: failed to add '%s'\n", rel_path);
                g_errors++;
            } else {
                g_added++;
            }
        }
    }
    closedir(d);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source_dir> <output.zip>\n", argv[0]);
        return 2;
    }
    g_root = argv[1];

    memset(&g_zip, 0, sizeof(g_zip));
    if (!mz_zip_writer_init_file(&g_zip, argv[2], 0)) {
        fprintf(stderr, "%s: cannot create '%s'\n", argv[0], argv[2]);
        return 1;
    }

    walk_dir(g_root, "");

    if (!mz_zip_writer_finalize_archive(&g_zip) ||
        !mz_zip_writer_end(&g_zip)) {
        fprintf(stderr, "%s: finalize failed\n", argv[0]);
        return 1;
    }
    fprintf(stderr, "pack_zip: %d files added, %d errors\n", g_added, g_errors);
    return g_errors == 0 ? 0 : 1;
}
