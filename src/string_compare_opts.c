// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* string_compare_opts.c — implementation factorisee, extraite de
 * fn_chaine_compare (stdlib_string.c) pour reutilisation par le
 * comparateur de tri tableau (stdlib_array.c). Semantique strictement
 * identique a l'historique. */

#include "string_compare_opts.h"
#include "scanner.h"  /* scanner_strip_accents */

#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define owl_strcasecmp _stricmp
#else
#include <strings.h>
#define owl_strcasecmp strcasecmp
#endif

int owl_string_compare_with_opts(const char* a, const char* b, int flags) {
    if (a == NULL) a = "";
    if (b == NULL) b = "";

    char buf1[4096], buf2[4096];
    const char* p1 = a;
    const char* p2 = b;

    int needBuf = (flags & (OWLCMP_SANS_ACCENT | OWLCMP_SANS_ESPACE |
                            OWLCMP_SANS_ESPACE_INT | OWLCMP_SANS_PONCTUATION |
                            OWLCMP_ORDRE_LEXICOGRAPHIQUE)) != 0;
    if (needBuf) {
        /* OrdreLexicographique : trier 'é' comme 'e' = strip accents pour
         * la cle de tri (cf. fn_chaine_compare). */
        if ((flags & OWLCMP_SANS_ACCENT) || (flags & OWLCMP_ORDRE_LEXICOGRAPHIQUE)) {
            scanner_strip_accents(a, (int)strlen(a), buf1, (int)sizeof(buf1));
            scanner_strip_accents(b, (int)strlen(b), buf2, (int)sizeof(buf2));
        } else {
            size_t la = strlen(a), lb = strlen(b);
            if (la >= sizeof(buf1)) la = sizeof(buf1) - 1;
            if (lb >= sizeof(buf2)) lb = sizeof(buf2) - 1;
            memcpy(buf1, a, la); buf1[la] = '\0';
            memcpy(buf2, b, lb); buf2[lb] = '\0';
        }
        if (flags & OWLCMP_SANS_ESPACE) {
            /* trim leading + trailing */
            char *t1 = buf1, *t2 = buf2;
            while (*t1 == ' ' || *t1 == '\t') t1++;
            int l1 = (int)strlen(t1);
            while (l1 > 0 && (t1[l1-1] == ' ' || t1[l1-1] == '\t')) l1--;
            memmove(buf1, t1, (size_t)l1); buf1[l1] = '\0';
            while (*t2 == ' ' || *t2 == '\t') t2++;
            int l2 = (int)strlen(t2);
            while (l2 > 0 && (t2[l2-1] == ' ' || t2[l2-1] == '\t')) l2--;
            memmove(buf2, t2, (size_t)l2); buf2[l2] = '\0';
        }
        if (flags & OWLCMP_SANS_ESPACE_INT) {
            char tmp1[4096], tmp2[4096]; int t;
            t = 0; for (const char* c = buf1; *c; c++) { if (*c != ' ' && *c != '\t') tmp1[t++] = *c; } tmp1[t] = '\0';
            memcpy(buf1, tmp1, (size_t)t + 1);
            t = 0; for (const char* c = buf2; *c; c++) { if (*c != ' ' && *c != '\t') tmp2[t++] = *c; } tmp2[t] = '\0';
            memcpy(buf2, tmp2, (size_t)t + 1);
        }
        if (flags & OWLCMP_SANS_PONCTUATION) {
            char tmp1[4096], tmp2[4096]; int t;
            t = 0; for (const char* c = buf1; *c; c++) { if (!ispunct((unsigned char)*c) && *c != ' ' && *c != '\t') tmp1[t++] = *c; } tmp1[t] = '\0';
            memcpy(buf1, tmp1, (size_t)t + 1);
            t = 0; for (const char* c = buf2; *c; c++) { if (!ispunct((unsigned char)*c) && *c != ' ' && *c != '\t') tmp2[t++] = *c; } tmp2[t] = '\0';
            memcpy(buf2, tmp2, (size_t)t + 1);
        }
        p1 = buf1; p2 = buf2;
    }

    if (flags & OWLCMP_RESPECTE_NUMERIQUE) {
        const char* x = p1;
        const char* y = p2;
        while (*x && *y) {
            if (isdigit((unsigned char)*x) && isdigit((unsigned char)*y)) {
                long long na = 0, nb = 0;
                while (isdigit((unsigned char)*x)) { na = na*10 + (*x - '0'); x++; }
                while (isdigit((unsigned char)*y)) { nb = nb*10 + (*y - '0'); y++; }
                if (na < nb) return -1;
                if (na > nb) return  1;
            } else {
                int ca = (flags & OWLCMP_SANS_CASSE) ? tolower((unsigned char)*x) : (unsigned char)*x;
                int cb = (flags & OWLCMP_SANS_CASSE) ? tolower((unsigned char)*y) : (unsigned char)*y;
                if (ca < cb) return -1;
                if (ca > cb) return  1;
                x++; y++;
            }
        }
        if (!*x && !*y) return 0;
        return *x ? 1 : -1;
    }

    int cmp = (flags & OWLCMP_SANS_CASSE) ? owl_strcasecmp(p1, p2) : strcmp(p1, p2);
    return cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
}

int owl_tcc_to_cmp_flags(int tcc_opts) {
    /* TCC_* vivent en high-byte (bits 8..14) avec lex<->num historiquement
     * inverses vs CC_*. Mapping explicite plutot que shift mecanique. */
    int f = 0;
    if (tcc_opts & 0x0100) f |= OWLCMP_SANS_CASSE;            /* TCC_SANS_CASSE */
    if (tcc_opts & 0x0200) f |= OWLCMP_SANS_ACCENT;           /* TCC_SANS_ACCENT */
    if (tcc_opts & 0x0400) f |= OWLCMP_SANS_ESPACE;           /* TCC_SANS_ESPACE */
    if (tcc_opts & 0x0800) f |= OWLCMP_SANS_ESPACE_INT;       /* TCC_SANS_ESPACE_INT */
    if (tcc_opts & 0x1000) f |= OWLCMP_SANS_PONCTUATION;      /* TCC_SANS_PONCTUATION */
    if (tcc_opts & 0x2000) f |= OWLCMP_RESPECTE_NUMERIQUE;    /* TCC_RESPECTE_NUMERIQUE */
    if (tcc_opts & 0x4000) f |= OWLCMP_ORDRE_LEXICOGRAPHIQUE; /* TCC_ORDRE_LEXICOGRAPHIQUE */
    return f;
}
