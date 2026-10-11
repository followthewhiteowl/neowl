// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sb_h
#define owl_sb_h

/* ================================================================
 * SB — tampon d'octets croissant (C-heap), partage (, decision C).
 *
 * Brique commune pour composer une frame / un corps / une GBNF sans se soucier
 * de la capacite. Extraite de serve_llm.c pour etre reutilisee par le
 * convertisseur schema->GBNF (stdlib_llm.c) : les deux composent des octets C
 * purs, aucune dependance VM/GC. Fonctions `static inline` pour rester
 * header-only (aucune unite de compilation dediee, pas de double definition).
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char*  p;
    size_t len;
    size_t cap;
} SB;

static inline void sb_init(SB* b) { b->p = NULL; b->len = 0; b->cap = 0; }
static inline void sb_free(SB* b) { free(b->p); b->p = NULL; b->len = 0; b->cap = 0; }

static inline int sb_reserve(SB* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t nc = b->cap == 0 ? 256 : b->cap;
    while (b->len + extra + 1 > nc) nc *= 2;
    char* np = (char*)realloc(b->p, nc);
    if (np == NULL) return -1;
    b->p   = np;
    b->cap = nc;
    return 0;
}

static inline void sb_putn(SB* b, const char* s, size_t n) {
    if (n == 0) return;
    if (sb_reserve(b, n) != 0) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static inline void sb_puts(SB* b, const char* s) { sb_putn(b, s, strlen(s)); }

static inline void sb_puti(SB* b, long long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", v);
    if (n > 0) sb_putn(b, tmp, (size_t)n);
}

#endif /* owl_sb_h */
