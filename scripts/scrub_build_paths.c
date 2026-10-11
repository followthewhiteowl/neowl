// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* scrub_build_paths.c — anonymise un binaire release en effacant le chemin de
 * build absolu (machine-specific) qui peut etre baked dans les libs vendorees
 * (Qt6 : qt_prfxpath + __FILE__ de qabstractspinbox.cpp/qdatetimeedit.cpp).
 *
 * Remplace chaque occurrence de <from> par autant d'octets '/' (meme longueur
 * -> offsets preserves, binaire toujours valide). Le chemin vise n'existe de
 * toute facon pas sur la machine de l'utilisateur final : Qt tombe sur ses
 * plugins statiques au runtime, donc l'ecrasement est neutre a l'execution.
 *
 * Usage : scrub_build_paths <binaire> <from>
 * Compile a la volee par build_release.sh (toolchain C only, pas de scripting).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <binaire> <chemin-a-effacer>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const char* from = argv[2];
    size_t flen = strlen(from);
    if (flen == 0) { fprintf(stderr, "scrub: chaine vide\n"); return 2; }

    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "scrub: open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); fprintf(stderr, "scrub: taille nulle\n"); return 1; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); fprintf(stderr, "scrub: oom\n"); return 1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); fprintf(stderr, "scrub: read\n"); return 1;
    }
    fclose(f);

    size_t hits = 0;
    for (size_t i = 0; i + flen <= (size_t)sz; ) {
        if (memcmp(buf + i, from, flen) == 0) {
            memset(buf + i, '/', flen);
            hits++;
            i += flen;
        } else {
            i++;
        }
    }

    if (hits > 0) {
        f = fopen(path, "rb+");
        if (!f) { free(buf); fprintf(stderr, "scrub: reopen\n"); return 1; }
        if (fwrite(buf, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f); free(buf); fprintf(stderr, "scrub: write\n"); return 1;
        }
        fclose(f);
    }
    free(buf);
    printf("scrub %s : %zu occurrence(s) de '%s'\n", path, hits, from);
    return 0;
}
