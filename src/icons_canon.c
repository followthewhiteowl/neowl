// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icons_canon.c — . Lookup helper + normalisation FR. */

#include "icons_canon.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 * Normalisation FR : lowercase ASCII + strip accents UTF-8.
 *
 * Couvre les diacritiques latin-1 / latin extended A courants en FR :
 *   à â ä ã  -> a
 *   é è ê ë  -> e
 *   î ï      -> i
 *   ô ö õ    -> o
 *   ù û ü    -> u
 *   ç        -> c
 *   ÿ        -> y
 *   œ        -> oe (expand)
 *   æ        -> ae (expand)
 *
 * Pas de normalisation NFC/NFD complete : suffit pour les termes FR
 * courants du mapping. Les utilisateurs qui tapent du texte exotique
 * tomberont sur le fallback EN direct (comportement degrade acceptable).
 * ------------------------------------------------------------------ */

bool owl_icones_canon_normalize_fr(const char* input,
                                   char* out_buf, int out_size) {
    if (!input || !out_buf || out_size <= 0) return false;
    int oi = 0;
    int trunc = 0;
    for (int i = 0; input[i] != '\0' && !trunc; i++) {
        unsigned char c = (unsigned char)input[i];

        /* ASCII : direct lowercase. */
        if (c < 0x80) {
            if (oi >= out_size - 1) { trunc = 1; break; }
            out_buf[oi++] = (char)tolower(c);
            continue;
        }

        /* UTF-8 2-byte sequence. Diacritiques courants vivent dans
         * U+00C0..U+017F (Latin-1 Supplement + Latin Extended-A). */
        if ((c & 0xE0) == 0xC0 && (unsigned char)input[i + 1] != '\0') {
            unsigned int cp = ((c & 0x1F) << 6) |
                              ((unsigned char)input[i + 1] & 0x3F);
            i++;  /* consume 2nd byte */
            const char* repl = NULL;
            switch (cp) {
                /* a */
                case 0xC0: case 0xC1: case 0xC2: case 0xC3:
                case 0xC4: case 0xC5:
                case 0xE0: case 0xE1: case 0xE2: case 0xE3:
                case 0xE4: case 0xE5: repl = "a"; break;
                /* c */
                case 0xC7: case 0xE7: repl = "c"; break;
                /* e */
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                case 0xE8: case 0xE9: case 0xEA: case 0xEB: repl = "e"; break;
                /* i */
                case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                case 0xEC: case 0xED: case 0xEE: case 0xEF: repl = "i"; break;
                /* n */
                case 0xD1: case 0xF1: repl = "n"; break;
                /* o */
                case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
                case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
                    repl = "o"; break;
                /* u */
                case 0xD9: case 0xDA: case 0xDB: case 0xDC:
                case 0xF9: case 0xFA: case 0xFB: case 0xFC: repl = "u"; break;
                /* y */
                case 0xDD: case 0xFD: case 0xFF: repl = "y"; break;
                /* ae / oe ligatures */
                case 0xC6: case 0xE6: repl = "ae"; break;
                case 0x152: case 0x153: repl = "oe"; break;
                default: repl = NULL; break;
            }
            if (repl) {
                for (int k = 0; repl[k] != '\0'; k++) {
                    if (oi >= out_size - 1) { trunc = 1; break; }
                    out_buf[oi++] = repl[k];
                }
            } else {
                /* Caractere multibyte inconnu : on le skip silencieusement
                 * (mode degrade — le mapping ne contient pas de tel char). */
            }
            continue;
        }

        /* UTF-8 3-byte / 4-byte : on skip les bytes additionnels et on
         * ignore le caractere (pas de mapping dans la table FR). */
        if ((c & 0xF0) == 0xE0) { i += 2; continue; }
        if ((c & 0xF8) == 0xF0) { i += 3; continue; }
        /* Autre cas : byte isole bizarre, skip. */
    }
    out_buf[oi] = '\0';
    return !trunc;
}

/* ------------------------------------------------------------------
 * Lookup bsearch sur la table triee FR ASC.
 * ------------------------------------------------------------------ */

static int bsearch_cmp_fr(const void* key, const void* item) {
    const char* needle = (const char*)key;
    const OwlIconCanonEntry* e = (const OwlIconCanonEntry*)item;
    return strcmp(needle, e->fr);
}

bool owl_icones_fr_to_en(const char* fr_lowercase,
                         const char* const** out_en_list,
                         int* out_count) {
    if (!fr_lowercase || OWL_ICONS_FR_EN_COUNT <= 0) return false;
    const OwlIconCanonEntry* hit = (const OwlIconCanonEntry*)bsearch(
        fr_lowercase, OWL_ICONS_FR_EN_MAPPING,
        (size_t)OWL_ICONS_FR_EN_COUNT, sizeof(OwlIconCanonEntry),
        bsearch_cmp_fr);
    if (!hit) return false;
    if (out_en_list) *out_en_list = hit->en;
    if (out_count)   *out_count   = hit->en_count;
    return true;
}
