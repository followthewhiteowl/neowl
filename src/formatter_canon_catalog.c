// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ================================================================
 * FORMATTER CANON — backend catalogue genere (owl-lsp.exe)
 * ================================================================
 * Implemente OwlFmtCanon en EXPOSANT LES DONNEES des tables statiques
 * generees (owl_catalog.h / lsp_catalog_gen.c), projection du registre
 * vivant produite par `owl --dump-lsp-catalog`. Aucune dependance
 * runtime/VM : c'est ce qui permet a owl-lsp.exe d'heberger le format
 * sans embarquer le moteur.
 *
 * Le matching (casse/accents/aliases) vit dans formatter.c, partage avec
 * le backend registry. Ce fichier ne fait que fournir les memes donnees
 * dans le meme ordre -> canonisation byte-identique a owl.exe (parite
 * verifiee par fmt-stability sur les deux backends, gate make).
 * ================================================================ */

#include "formatter_canon.h"
#include "owl_catalog.h"

#include <stddef.h> /* NULL */

/* ---- Natives ------------------------------------------------------------- */
static int cc_native_count(void) {
    return OWL_NATIVE_CATALOG_COUNT;
}

static const char* cc_native_canonical(int i) {
    return (i >= 0 && i < OWL_NATIVE_CATALOG_COUNT)
        ? OWL_NATIVE_CATALOG[i].name : NULL;
}

static const char* cc_native_ascii(int i) {
    return (i >= 0 && i < OWL_NATIVE_CATALOG_COUNT)
        ? OWL_NATIVE_CATALOG[i].nameAscii : NULL;
}

static const char* cc_native_en(int i) {
    return (i >= 0 && i < OWL_NATIVE_CATALOG_COUNT)
        ? OWL_NATIVE_CATALOG[i].nameEN : NULL;
}

static int cc_native_alias_count(int i) {
    if (i < 0 || i >= OWL_NATIVE_CATALOG_COUNT) return 0;
    const char* const* a = OWL_NATIVE_CATALOG[i].aliases;
    if (!a) return 0;
    int n = 0;
    while (a[n]) n++;
    return n;
}

static const char* cc_native_alias(int i, int aliasIndex) {
    if (i < 0 || i >= OWL_NATIVE_CATALOG_COUNT) return NULL;
    const char* const* a = OWL_NATIVE_CATALOG[i].aliases;
    if (!a) return NULL;
    for (int n = 0; a[n]; n++)
        if (n == aliasIndex) return a[n];
    return NULL;
}

/* ---- Constantes ---------------------------------------------------------- */
static int cc_const_count(void) {
    return OWL_CONST_CATALOG_COUNT;
}

static const char* cc_const_name(int i) {
    return (i >= 0 && i < OWL_CONST_CATALOG_COUNT)
        ? OWL_CONST_CATALOG[i].name : NULL;
}

/* ---- Proprietes builtin (date/heure/tableau/JSON/duree) ------------------ */
static int cc_builtin_prop_count(void) {
    return OWL_BUILTIN_PROP_CATALOG_COUNT;
}

static const char* cc_builtin_prop_fr(int i) {
    return (i >= 0 && i < OWL_BUILTIN_PROP_CATALOG_COUNT)
        ? OWL_BUILTIN_PROP_CATALOG[i].nameFR : NULL;
}

static const char* cc_builtin_prop_en(int i) {
    return (i >= 0 && i < OWL_BUILTIN_PROP_CATALOG_COUNT)
        ? OWL_BUILTIN_PROP_CATALOG[i].nameEN : NULL;
}

/* ---- Types builtin (struct-like) ----------------------------------------- */
static int cc_type_count(void) {
    return OWL_TYPE_CATALOG_COUNT;
}

static const char* cc_type_canonical(int i) {
    return (i >= 0 && i < OWL_TYPE_CATALOG_COUNT)
        ? OWL_TYPE_CATALOG[i].name : NULL;
}

static const char* cc_type_ascii(int i) {
    return (i >= 0 && i < OWL_TYPE_CATALOG_COUNT)
        ? OWL_TYPE_CATALOG[i].nameAscii : NULL;
}

/*  : le catalogue ne porte pas le genre (donnee translate-only,
 * consommee uniquement dans owl.exe via le backend registry). Renvoie NONE. */
static int cc_type_genre(int i) {
    (void)i;
    return 0;
}

static int cc_type_field_count(int ti) {
    return (ti >= 0 && ti < OWL_TYPE_CATALOG_COUNT)
        ? OWL_TYPE_CATALOG[ti].fieldCount : 0;
}

static const char* cc_type_field_key(int ti, int fi) {
    if (ti < 0 || ti >= OWL_TYPE_CATALOG_COUNT) return NULL;
    const OwlTypeInfo* t = &OWL_TYPE_CATALOG[ti];
    if (!t->fields || fi < 0 || fi >= t->fieldCount) return NULL;
    return t->fields[fi].nameAscii;
}

static const char* cc_type_field_display(int ti, int fi) {
    if (ti < 0 || ti >= OWL_TYPE_CATALOG_COUNT) return NULL;
    const OwlTypeInfo* t = &OWL_TYPE_CATALOG[ti];
    if (!t->fields || fi < 0 || fi >= t->fieldCount) return NULL;
    return t->fields[fi].name;
}

/* ---- translate type-aware ------------------------------------- */
static const char* cc_type_en(int ti) {
    return (ti >= 0 && ti < OWL_TYPE_CATALOG_COUNT)
        ? OWL_TYPE_CATALOG[ti].nameEN : NULL;
}

static const char* cc_type_field_en(int ti, int fi) {
    if (ti < 0 || ti >= OWL_TYPE_CATALOG_COUNT) return NULL;
    const OwlTypeInfo* t = &OWL_TYPE_CATALOG[ti];
    if (!t->fields || fi < 0 || fi >= t->fieldCount) return NULL;
    return t->fields[fi].nameEN;
}

static const char* cc_type_field_type(int ti, int fi) {
    if (ti < 0 || ti >= OWL_TYPE_CATALOG_COUNT) return NULL;
    const OwlTypeInfo* t = &OWL_TYPE_CATALOG[ti];
    if (!t->fields || fi < 0 || fi >= t->fieldCount) return NULL;
    return t->fields[fi].type;
}

static const char* cc_const_en(int i) {
    return (i >= 0 && i < OWL_CONST_CATALOG_COUNT)
        ? OWL_CONST_CATALOG[i].nameEN : NULL;
}

/* ---- Methodes d'extension ------------------------------------------------- */
static int cc_method_count(void) {
    return OWL_METHOD_CATALOG_COUNT;
}

static const char* cc_method_name(int i) {
    return (i >= 0 && i < OWL_METHOD_CATALOG_COUNT)
        ? OWL_METHOD_CATALOG[i].name : NULL;
}

/*  — le catalogue statique ne porte pas les pointeurs d'impl : pas de
 * pairing par fonction possible. `owl translate` ne tourne de toute facon que
 * dans le backend registre (owl.exe), jamais dans owl-lsp. NULL -> aucun
 * partenaire. */
static const char* cc_method_partner(const char* name, int len) {
    (void)name; (void)len;
    return NULL;
}

/* ---- Proprietes de blocs DSL --------------------------------------------- */
static int cc_dsl_prop_count(void) {
    return OWL_DSL_PROP_CATALOG_COUNT;
}

static const char* cc_dsl_prop_fr(int i) {
    return (i >= 0 && i < OWL_DSL_PROP_CATALOG_COUNT)
        ? OWL_DSL_PROP_CATALOG[i].name : NULL;
}

static const char* cc_dsl_prop_ascii(int i) {
    return (i >= 0 && i < OWL_DSL_PROP_CATALOG_COUNT)
        ? OWL_DSL_PROP_CATALOG[i].nameAscii : NULL;
}

static const char* cc_dsl_prop_en(int i) {
    return (i >= 0 && i < OWL_DSL_PROP_CATALOG_COUNT)
        ? OWL_DSL_PROP_CATALOG[i].nameEN : NULL;
}

static const char* cc_dsl_prop_kind(int i) {
    return (i >= 0 && i < OWL_DSL_PROP_CATALOG_COUNT)
        ? OWL_DSL_PROP_CATALOG[i].kind : NULL;
}

static const char* cc_dsl_prop_object_type(int i) {
    return (i >= 0 && i < OWL_DSL_PROP_CATALOG_COUNT)
        ? OWL_DSL_PROP_CATALOG[i].objectType : NULL;
}

static const OwlFmtCanon CATALOG_CANON = {
    .native_count          = cc_native_count,
    .native_canonical      = cc_native_canonical,
    .native_ascii          = cc_native_ascii,
    .native_en             = cc_native_en,
    .native_alias_count    = cc_native_alias_count,
    .native_alias          = cc_native_alias,
    .const_count           = cc_const_count,
    .const_name            = cc_const_name,
    .builtin_prop_count    = cc_builtin_prop_count,
    .builtin_prop_fr       = cc_builtin_prop_fr,
    .builtin_prop_en       = cc_builtin_prop_en,
    .type_count            = cc_type_count,
    .type_canonical        = cc_type_canonical,
    .type_ascii            = cc_type_ascii,
    .type_genre            = cc_type_genre,
    .type_field_count      = cc_type_field_count,
    .type_field_key        = cc_type_field_key,
    .type_field_display    = cc_type_field_display,
    .type_en               = cc_type_en,
    .type_field_en         = cc_type_field_en,
    .type_field_type       = cc_type_field_type,
    .const_en              = cc_const_en,
    .method_count          = cc_method_count,
    .method_name           = cc_method_name,
    .method_partner        = cc_method_partner,
    .dsl_prop_count        = cc_dsl_prop_count,
    .dsl_prop_fr           = cc_dsl_prop_fr,
    .dsl_prop_ascii        = cc_dsl_prop_ascii,
    .dsl_prop_en           = cc_dsl_prop_en,
    .dsl_prop_kind         = cc_dsl_prop_kind,
    .dsl_prop_object_type  = cc_dsl_prop_object_type,
};

const OwlFmtCanon* owl_fmt_canon_catalog(void) {
    return &CATALOG_CANON;
}
