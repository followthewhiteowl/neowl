// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_event_catalog.c — implémentation du catalogue d'events. */

#include "owl_event_catalog.h"
#include "scanner.h"   /* scanner_strip_accents */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Stockage par kind. Chaque kind peut accumuler plusieurs CHUNKS (tables
 * statiques de OwlEventDef) — utile pour la mutualisation des familles
 * d'events communes (focus, mouse, layout, ...). Chaque appel a
 * owl_event_catalog_register AJOUTE un chunk plutot que de remplacer. */
#define OWL_EVENT_MAX_CHUNKS 8

typedef struct {
    const OwlEventDef* defs[OWL_EVENT_MAX_CHUNKS];
    int                counts[OWL_EVENT_MAX_CHUNKS];
    int                chunkCount;
    int                totalCount;  /* somme cache des counts[] */
} KindEntry;

/* OwlControlKind est un enum a valeurs <10 ; on dimensionne large
 * (32) pour absorber les futurs widgets sans repackager. */
#define OWL_EVENT_MAX_KINDS 32

static KindEntry g_byKind[OWL_EVENT_MAX_KINDS];

/* Reset un KindEntry. Helper pour mutualiser le reset des trois
 * catalogues parallèles (g_etat / g_etat_sub / g_window_lifecycle)
 * en sus de g_byKind.  : sans ce reset, un re-vm_init
 * (cas --vscode-data) re-empile les chunks et le dump dedoublonne. */
static void kind_entry_clear(KindEntry* e) {
    for (int c = 0; c < OWL_EVENT_MAX_CHUNKS; c++) {
        e->defs[c]   = NULL;
        e->counts[c] = 0;
    }
    e->chunkCount = 0;
    e->totalCount = 0;
}

/* Forward : impl en fin de fichier (apres declaration des `static` cibles). */
static void reset_parallel_catalogs(void);

void owl_event_catalog_reset(void) {
    for (int i = 0; i < OWL_EVENT_MAX_KINDS; i++) {
        kind_entry_clear(&g_byKind[i]);
    }
    /*  : reset les catalogues parallèles (Etat / sub-Etat /
     * lifecycle Fenetre) en plus du catalogue widget indexe par kind.
     * Sans ca, un re-vm_init (cas --vscode-data : 2x vm_init dans le
     * meme process) accumule les chunks et le dump publie en double. */
    reset_parallel_catalogs();
}

void owl_event_catalog_register(OwlControlKind kind,
                                const OwlEventDef* defs, int count) {
    if ((int)kind < 0 || (int)kind >= OWL_EVENT_MAX_KINDS) return;
    if (defs == NULL || count <= 0) return;
    KindEntry* e = &g_byKind[kind];
    if (e->chunkCount >= OWL_EVENT_MAX_CHUNKS) return; /* capacite sature */
    e->defs[e->chunkCount]   = defs;
    e->counts[e->chunkCount] = count;
    e->chunkCount++;
    e->totalCount += count;
}

/* Match case-insensitive ET accent-insensitive : la forme tapée par le dev
 * peut etre "surEntréeSurvol" ou "surentreesurvol" ou "SurEntreeSurvol". */
static bool name_eq_loose(const char* a, const char* b) {
    if (a == NULL || b == NULL) return false;
    char na[128], nb[128];
    int la = scanner_strip_accents(a, (int)strlen(a), na, (int)sizeof(na));
    int lb = scanner_strip_accents(b, (int)strlen(b), nb, (int)sizeof(nb));
    if (la <= 0 || lb <= 0 || la != lb) return false;
    for (int i = 0; i < la; i++) {
        if (tolower((unsigned char)na[i]) != tolower((unsigned char)nb[i]))
            return false;
    }
    return true;
}

const OwlEventDef* owl_event_catalog_find(OwlControlKind kind,
                                           const char* eventName) {
    if ((int)kind < 0 || (int)kind >= OWL_EVENT_MAX_KINDS) return NULL;
    if (eventName == NULL) return NULL;
    const KindEntry* e = &g_byKind[kind];
    for (int c = 0; c < e->chunkCount; c++) {
        for (int i = 0; i < e->counts[c]; i++) {
            const OwlEventDef* d = &e->defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_event_catalog_count(OwlControlKind kind) {
    if ((int)kind < 0 || (int)kind >= OWL_EVENT_MAX_KINDS) return 0;
    return g_byKind[kind].totalCount;
}

const OwlEventDef* owl_event_catalog_at(OwlControlKind kind, int index) {
    if ((int)kind < 0 || (int)kind >= OWL_EVENT_MAX_KINDS) return NULL;
    if (index < 0) return NULL;
    const KindEntry* e = &g_byKind[kind];
    int offset = 0;
    for (int c = 0; c < e->chunkCount; c++) {
        if (index < offset + e->counts[c])
            return &e->defs[c][index - offset];
        offset += e->counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue parallèle pour les events État (rapports d'impression).
 * Stockage identique à widgets, mais standalone (pas indexé par kind).
 * ================================================================ */
static KindEntry g_etat;

void owl_etat_event_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_etat.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_etat.defs[g_etat.chunkCount]   = defs;
    g_etat.counts[g_etat.chunkCount] = count;
    g_etat.chunkCount++;
    g_etat.totalCount += count;
}

const OwlEventDef* owl_etat_event_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_etat.chunkCount; c++) {
        for (int i = 0; i < g_etat.counts[c]; i++) {
            const OwlEventDef* d = &g_etat.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_etat_event_catalog_count(void) {
    return g_etat.totalCount;
}

const OwlEventDef* owl_etat_event_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_etat.chunkCount; c++) {
        if (index < offset + g_etat.counts[c])
            return &g_etat.defs[c][index - offset];
        offset += g_etat.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue des events de cycle de vie des sous-contrôles
 * d'un bloc État (Libellé, Image dans Corps/EnTeteDocument/...).
 * ================================================================ */
static KindEntry g_etat_sub;

void owl_etat_subcontrol_event_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_etat_sub.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_etat_sub.defs[g_etat_sub.chunkCount]   = defs;
    g_etat_sub.counts[g_etat_sub.chunkCount] = count;
    g_etat_sub.chunkCount++;
    g_etat_sub.totalCount += count;
}

const OwlEventDef* owl_etat_subcontrol_event_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_etat_sub.chunkCount; c++) {
        for (int i = 0; i < g_etat_sub.counts[c]; i++) {
            const OwlEventDef* d = &g_etat_sub.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_etat_subcontrol_event_catalog_count(void) {
    return g_etat_sub.totalCount;
}

const OwlEventDef* owl_etat_subcontrol_event_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_etat_sub.chunkCount; c++) {
        if (index < offset + g_etat_sub.counts[c])
            return &g_etat_sub.defs[c][index - offset];
        offset += g_etat_sub.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue lifecycle Fenetre (FinInitialisation/Fermeture/...).
 * Storage identique aux deux precedents, standalone.
 * ================================================================ */
static KindEntry g_window_lifecycle;

void owl_window_lifecycle_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_window_lifecycle.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_window_lifecycle.defs[g_window_lifecycle.chunkCount]   = defs;
    g_window_lifecycle.counts[g_window_lifecycle.chunkCount] = count;
    g_window_lifecycle.chunkCount++;
    g_window_lifecycle.totalCount += count;
}

const OwlEventDef* owl_window_lifecycle_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_window_lifecycle.chunkCount; c++) {
        for (int i = 0; i < g_window_lifecycle.counts[c]; i++) {
            const OwlEventDef* d = &g_window_lifecycle.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_window_lifecycle_catalog_count(void) {
    return g_window_lifecycle.totalCount;
}

const OwlEventDef* owl_window_lifecycle_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_window_lifecycle.chunkCount; c++) {
        if (index < offset + g_window_lifecycle.counts[c])
            return &g_window_lifecycle.defs[c][index - offset];
        offset += g_window_lifecycle.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue des events d'une OptionMenu.
 * Storage identique aux precedents, standalone.
 * ================================================================ */
static KindEntry g_menu_option;

void owl_menu_option_event_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_menu_option.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_menu_option.defs[g_menu_option.chunkCount]   = defs;
    g_menu_option.counts[g_menu_option.chunkCount] = count;
    g_menu_option.chunkCount++;
    g_menu_option.totalCount += count;
}

const OwlEventDef* owl_menu_option_event_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_menu_option.chunkCount; c++) {
        for (int i = 0; i < g_menu_option.counts[c]; i++) {
            const OwlEventDef* d = &g_menu_option.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_menu_option_event_catalog_count(void) {
    return g_menu_option.totalCount;
}

const OwlEventDef* owl_menu_option_event_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_menu_option.chunkCount; c++) {
        if (index < offset + g_menu_option.counts[c])
            return &g_menu_option.defs[c][index - offset];
        offset += g_menu_option.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue des events d'un bloc CollectionDeProcedures.
 * Storage identique aux precedents, standalone.
 * ================================================================ */
static KindEntry g_collection;

void owl_collection_event_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_collection.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_collection.defs[g_collection.chunkCount]   = defs;
    g_collection.counts[g_collection.chunkCount] = count;
    g_collection.chunkCount++;
    g_collection.totalCount += count;
}

const OwlEventDef* owl_collection_event_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_collection.chunkCount; c++) {
        for (int i = 0; i < g_collection.counts[c]; i++) {
            const OwlEventDef* d = &g_collection.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_collection_event_catalog_count(void) {
    return g_collection.totalCount;
}

const OwlEventDef* owl_collection_event_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_collection.chunkCount; c++) {
        if (index < offset + g_collection.counts[c])
            return &g_collection.defs[c][index - offset];
        offset += g_collection.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue des events de niveau ServiceREST.
 * Storage identique aux precedents, standalone.
 * ================================================================ */
static KindEntry g_service;

void owl_service_event_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_service.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_service.defs[g_service.chunkCount]   = defs;
    g_service.counts[g_service.chunkCount] = count;
    g_service.chunkCount++;
    g_service.totalCount += count;
}

const OwlEventDef* owl_service_event_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_service.chunkCount; c++) {
        for (int i = 0; i < g_service.counts[c]; i++) {
            const OwlEventDef* d = &g_service.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_service_event_catalog_count(void) {
    return g_service.totalCount;
}

const OwlEventDef* owl_service_event_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_service.chunkCount; c++) {
        if (index < offset + g_service.counts[c])
            return &g_service.defs[c][index - offset];
        offset += g_service.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Catalogue des events de niveau OpérationHTTP.
 * Storage identique aux precedents, standalone.
 * ================================================================ */
static KindEntry g_operation;

void owl_operation_event_catalog_register(const OwlEventDef* defs, int count) {
    if (defs == NULL || count <= 0) return;
    if (g_operation.chunkCount >= OWL_EVENT_MAX_CHUNKS) return;
    g_operation.defs[g_operation.chunkCount]   = defs;
    g_operation.counts[g_operation.chunkCount] = count;
    g_operation.chunkCount++;
    g_operation.totalCount += count;
}

const OwlEventDef* owl_operation_event_catalog_find(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int c = 0; c < g_operation.chunkCount; c++) {
        for (int i = 0; i < g_operation.counts[c]; i++) {
            const OwlEventDef* d = &g_operation.defs[c][i];
            if (name_eq_loose(d->canonical, eventName)) return d;
            if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
        }
    }
    return NULL;
}

int owl_operation_event_catalog_count(void) {
    return g_operation.totalCount;
}

const OwlEventDef* owl_operation_event_catalog_at(int index) {
    if (index < 0) return NULL;
    int offset = 0;
    for (int c = 0; c < g_operation.chunkCount; c++) {
        if (index < offset + g_operation.counts[c])
            return &g_operation.defs[c][index - offset];
        offset += g_operation.counts[c];
    }
    return NULL;
}

/* ================================================================
 * Lookup transverse tous catalogues confondus.
 * Defini ici (et pas cote appelant) pour rester la source unique :
 * un nouveau catalogue parallele n'a qu'un endroit a mettre a jour.
 * ================================================================ */
const OwlEventDef* owl_event_catalog_find_any(const char* eventName) {
    if (eventName == NULL) return NULL;
    for (int k = 0; k < OWL_EVENT_MAX_KINDS; k++) {
        const KindEntry* e = &g_byKind[k];
        for (int c = 0; c < e->chunkCount; c++) {
            for (int i = 0; i < e->counts[c]; i++) {
                const OwlEventDef* d = &e->defs[c][i];
                if (name_eq_loose(d->canonical, eventName)) return d;
                if (d->nameEN && name_eq_loose(d->nameEN, eventName)) return d;
            }
        }
    }
    const OwlEventDef* d;
    if ((d = owl_etat_event_catalog_find(eventName)) != NULL) return d;
    if ((d = owl_etat_subcontrol_event_catalog_find(eventName)) != NULL) return d;
    if ((d = owl_window_lifecycle_catalog_find(eventName)) != NULL) return d;
    if ((d = owl_menu_option_event_catalog_find(eventName)) != NULL) return d;
    if ((d = owl_collection_event_catalog_find(eventName)) != NULL) return d;
    if ((d = owl_service_event_catalog_find(eventName)) != NULL) return d;
    if ((d = owl_operation_event_catalog_find(eventName)) != NULL) return d;
    return NULL;
}

/*  : reset des catalogues parallèles. Defini en fin de fichier
 * pour acceder aux `static KindEntry` g_etat / g_etat_sub /
 * g_window_lifecycle / g_menu_option / g_collection. Appele depuis
 * owl_event_catalog_reset. */
static void reset_parallel_catalogs(void) {
    kind_entry_clear(&g_etat);
    kind_entry_clear(&g_etat_sub);
    kind_entry_clear(&g_window_lifecycle);
    kind_entry_clear(&g_menu_option);
    kind_entry_clear(&g_collection);
    kind_entry_clear(&g_service);
    kind_entry_clear(&g_operation);
}
