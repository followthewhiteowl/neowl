// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_name_normalize.h"

#include <ctype.h>
#include <string.h>

void owl_normalize_name(const char* src, char* out, int outCap) {
    int j = 0;
    for (int i = 0; src[i] && j + 1 < outCap; ) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) {
            out[j++] = (char)tolower(c);
            i++;
        } else if ((c & 0xE0) == 0xC0 && src[i+1]) {
            /* 2-byte UTF-8: strip accent to its ASCII base. */
            unsigned int cp = ((c & 0x1Fu) << 6) | ((unsigned char)src[i+1] & 0x3Fu);
            char base;
            if      (cp >= 0xC0 && cp <= 0xC6) base = 'a';
            else if (cp >= 0xE0 && cp <= 0xE6) base = 'a';
            else if (cp == 0xC7 || cp == 0xE7) base = 'c';
            else if (cp >= 0xC8 && cp <= 0xCB) base = 'e';
            else if (cp >= 0xE8 && cp <= 0xEB) base = 'e';
            else if (cp >= 0xCC && cp <= 0xCF) base = 'i';
            else if (cp >= 0xEC && cp <= 0xEF) base = 'i';
            else if (cp >= 0xD2 && cp <= 0xD6) base = 'o';
            else if (cp >= 0xF2 && cp <= 0xF6) base = 'o';
            else if (cp >= 0xD9 && cp <= 0xDC) base = 'u';
            else if (cp >= 0xF9 && cp <= 0xFC) base = 'u';
            else                                base = '?';
            out[j++] = base;
            i += 2;
        } else {
            /* 3+ byte UTF-8 sequences we don't expect in identifiers. */
            out[j++] = '?';
            i++;
        }
    }
    out[j] = '\0';
}

bool owl_name_matches(const char* candidate, const char* normalizedQuery) {
    char buf[256];
    owl_normalize_name(candidate, buf, sizeof buf);
    return strcmp(buf, normalizedQuery) == 0;
}

/* Canonicalisation de la casse d'un tag BCP 47 (RFC 5646).
 * Les tags de langue sont case-insensibles ; OWL les stocke et les compare
 * toujours en forme canonique pour que la casse saisie ne change pas la
 * langue designee (ex "en-gb" == "EN-GB" == "en-GB" == langueAnglaisRoyaumeUni).
 *
 * Regle de casse (couvre les constantes officielles et les tags custom) :
 *   - sous-tag primaire (langue)         -> minuscule ("EN" -> "en")
 *   - 2e sous-tag de 2 lettres (region)  -> MAJUSCULE ("gb" -> "GB")
 *   - tout le reste                       -> minuscule
 * Mappe donc en-gb / EN-GB / en-GB -> "en-GB" et fr-fr -> "fr-FR".
 * Le script Titlecase (4 lettres) n'est pas traite : aucune constante
 * officielle n'en porte, et le cadrage l'exclut (nice-to-have).
 *
 * out recoit la forme canonique, tronquee a cap-1 et NUL-terminee. Un tag
 * NULL ou vide laisse out vide (le rejet est gere en amont). Robuste : jamais
 * d'overflow, cap toujours respecte. */
void owl_bcp47_canon(const char* tag, char* out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (tag == NULL) return;

    size_t o = 0;
    int subtagIndex = 0;   /* 0 = sous-tag primaire (langue) */
    const char* p = tag;
    while (*p != '\0' && o + 1 < cap) {
        const char* start = p;
        while (*p != '\0' && *p != '-') p++;
        size_t len = (size_t)(p - start);

        /* Region : 2e sous-tag (index 1) de 2 lettres -> MAJUSCULE. */
        int upper = 0;
        if (subtagIndex == 1 && len == 2 &&
            isalpha((unsigned char)start[0]) && isalpha((unsigned char)start[1])) {
            upper = 1;
        }
        for (size_t i = 0; i < len && o + 1 < cap; i++) {
            unsigned char c = (unsigned char)start[i];
            out[o++] = upper ? (char)toupper(c) : (char)tolower(c);
        }
        if (*p == '-' && o + 1 < cap) {
            out[o++] = '-';
            p++;
        }
        subtagIndex++;
    }
    out[o] = '\0';
}
