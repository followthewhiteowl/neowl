// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* write_footer.c — Helper minimaliste pour append le footer 24 bytes
 * du megafile owl-build (PyInstaller-style).
 *
 * Format ecrit (24 bytes) :
 *   offset (uint64 LE)   = ARGV[1]
 *   size   (uint64 LE)   = ARGV[2]
 *   magic  "OWLBUNDL"    = 8 bytes ASCII
 *
 * Usage : write_footer <offset> <size> <output_file>
 *
 * Pourquoi un binaire C dedie au lieu de python/perl one-liner :
 * la toolchain OWL doit rester C-pur (cf feedback_toolchain_no_scripting).
 *
 * Build :
 *   gcc -O2 -o write_footer scripts/write_footer.c
 *   (Le binaire est utilise par scripts/pack_megafile.sh)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <offset> <size> <output_file>\n", argv[0]);
        return 2;
    }
    uint64_t offset = strtoull(argv[1], NULL, 10);
    uint64_t size   = strtoull(argv[2], NULL, 10);
    const char* path = argv[3];

    FILE* f = fopen(path, "ab");
    if (!f) {
        fprintf(stderr, "%s: cannot open '%s'\n", argv[0], path);
        return 1;
    }

    /* Ecrit en little-endian explicite (independent du host endian). */
    uint8_t buf[24];
    for (int i = 0; i < 8; i++) {
        buf[i]     = (uint8_t)((offset >> (i * 8)) & 0xFF);
        buf[8 + i] = (uint8_t)((size   >> (i * 8)) & 0xFF);
    }
    memcpy(buf + 16, "OWLBUNDL", 8);

    if (fwrite(buf, 1, 24, f) != 24) {
        fprintf(stderr, "%s: write failed\n", argv[0]);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}
