// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_globalvar_catalog.c — implémentation du catalogue des variables
 * globales prédéfinies. Voir owl_globalvar_catalog.h. */

#include "owl_globalvar_catalog.h"
#include "scanner.h"  /* scanner_strip_accents */

#include <string.h>
#include <ctype.h>

#define OWL_GLOBALVAR_MAX 32

static const OwlGlobalVarDef* g_defs[OWL_GLOBALVAR_MAX];
static int g_count = 0;

void owl_globalvar_catalog_register(const OwlGlobalVarDef* def) {
    if (def == NULL) return;
    if (g_count >= OWL_GLOBALVAR_MAX) return;
    /* Évite les doublons par identité de pointeur (idempotent au cas où
     * un init est appelé deux fois). */
    for (int i = 0; i < g_count; i++) {
        if (g_defs[i] == def) return;
    }
    g_defs[g_count++] = def;
}

/* strip accents + lower in place. Buffer de 64 octets suffit pour les
 * noms attendus (ErreurDétectée = 18 bytes UTF-8). */
static void normalize(const char* src, char* dst, int dstSize) {
    char tmp[128];
    int srcLen = (int)strlen(src);
    scanner_strip_accents(src, srcLen, tmp, (int)sizeof tmp);
    int o = 0;
    for (int i = 0; tmp[i] && o + 1 < dstSize; i++) {
        dst[o++] = (char)tolower((unsigned char)tmp[i]);
    }
    dst[o] = '\0';
}

static bool name_eq_loose(const char* a, const char* b) {
    if (!a || !b) return false;
    char na[128], nb[128];
    normalize(a, na, sizeof na);
    normalize(b, nb, sizeof nb);
    return strcmp(na, nb) == 0;
}

const OwlGlobalVarDef* owl_globalvar_catalog_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_count; i++) {
        const OwlGlobalVarDef* d = g_defs[i];
        if (name_eq_loose(d->canonical, name))             return d;
        if (d->nameAscii && name_eq_loose(d->nameAscii, name)) return d;
        if (d->nameEN    && name_eq_loose(d->nameEN, name))    return d;
    }
    return NULL;
}

bool owl_globalvar_catalog_is_global_name(const char* name) {
    if (!name) return false;
    /* Match strict (les 3 formes sont posées dans vm.globals). */
    for (int i = 0; i < g_count; i++) {
        const OwlGlobalVarDef* d = g_defs[i];
        if (strcmp(d->canonical, name) == 0) return true;
        if (d->nameAscii && strcmp(d->nameAscii, name) == 0) return true;
        if (d->nameEN    && strcmp(d->nameEN, name) == 0)    return true;
    }
    return false;
}

int owl_globalvar_catalog_count(void) {
    return g_count;
}

const OwlGlobalVarDef* owl_globalvar_catalog_at(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return g_defs[index];
}
