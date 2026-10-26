// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_common_h
#define owl_common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Debug flags - uncomment to enable */
/* #define DEBUG_PRINT_CODE */
/* #define DEBUG_TRACE_EXECUTION */
/* #define DEBUG_STRESS_GC */
/* #define DEBUG_LOG_GC */

#define OWL_VERSION "1.6.0"
#define OWL_NEXT_VERSION "1.7.0"
#define OWL_VERSION_MAJOR 1
#define OWL_VERSION_MINOR 6

/* Maximum number of local variables in a scope */
#define UINT8_COUNT (UINT8_MAX + 1)

/* Strip d'un BOM UTF-8 (EF BB BF) en tete de chaine, in-place via memmove.
 * Renvoie true si un BOM a ete retire, false sinon (no-op safe sur chaine
 * vide ou < 3 octets). Partage par le chemin fichier (project.c) et le
 * chemin stdin/REPL (main.c) — meme convention : seul le tout debut du
 * flux est traite, un BOM apparaissant au milieu reste intact. */
static inline bool owl_strip_utf8_bom(char* s) {
    if (s == NULL) return false;
    size_t len = strlen(s);
    if (len >= 3 &&
        (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        memmove(s, s + 3, len - 3 + 1); /* +1 pour le \0 final */
        return true;
    }
    return false;
}

/* Ouvre en creation un fichier SOURCE genere par OWL (`owl new projet`,
 * export de squelette, ...) avec la fin de ligne NATIVE a la plateforme :
 * CRLF sous Windows, LF ailleurs. Point de decision UNIQUE — un nouveau
 * generateur passe par ici et herite du bon comportement, pas de #ifdef
 * recopie sur chaque site.
 *
 * Le mode texte ("w") suffit : le CRT Windows traduit chaque '\n' emis en
 * "\r\n", les libc POSIX ne touchent a rien. C'est exactement ce que fait
 * deja stdout (mode texte par defaut), donc `owl new <type> > f.owl` et
 * l'ecriture directe livrent les memes octets sur une meme plateforme.
 *
 * NE PAS utiliser pour les ecritures de DONNEES (fSauveTexte, fSauveBuffer,
 * exports, snapshots, archives) : celles-la restent binaires, octet pour
 * octet (cf. ). */
static inline FILE* owl_fopen_generated_source(const char* path) {
    return fopen(path, "w");
}

#endif
