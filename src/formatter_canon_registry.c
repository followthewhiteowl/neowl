// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ================================================================
 * FORMATTER CANON — backend registre vivant (owl.exe)
 * ================================================================
 * Implemente OwlFmtCanon en EXPOSANT LES DONNEES du registre runtime
 * (peuple par stdlib_init via vm_init). Le matching (casse/accents/aliases)
 * vit cote formatter.c, partage avec le backend catalogue (owl-lsp) : une
 * seule logique, deux flux de donnees -> divergence impossible par
 * construction.
 *
 * Concentre les includes lourds (owl_registry, compiler, owl_methods,
 * ui_catalog) pour que formatter.c reste decouple du runtime.
 * ================================================================ */

#include "formatter_canon.h"
#include "owl_registry.h"
#include "owl_methods.h"
#include "owl_name_normalize.h"
#include "compiler.h"
#include "ui_catalog.h"

#include <stdlib.h>
#include <string.h>

/* ---- Natives : ensemble filtre identique a dump_lsp_catalog --------------
 * On expose le MEME ensemble que le catalogue (skip OWL_NAT_INTERNAL + natives
 * sans signature) pour que les deux backends iterent strictement les memes
 * natives. Cache construit une fois ; les OwlNativeDef sont static const
 * (pointeurs stables), rebuild seulement si le total registre change. */
static const OwlNativeDef** g_fmt_nat = NULL;
static int g_fmt_nat_count = 0;
static int g_fmt_nat_total = -1;

static bool nat_is_user_facing(const OwlNativeDef* d) {
    if (!d) return false;
    if (d->flags & OWL_NAT_INTERNAL) return false;
    if (!d->signatures) return false;
    int sc = 0;
    for (const OwlSignature* s = d->signatures; s->returnType; s++) sc++;
    return sc > 0;
}

static void ensure_fmt_nat(void) {
    int total = owl_registry_count();
    if (g_fmt_nat_total == total) return;  /* deja construit pour ce registre */
    free(g_fmt_nat);
    g_fmt_nat = (const OwlNativeDef**)malloc((size_t)(total > 0 ? total : 1)
                                             * sizeof(*g_fmt_nat));
    int k = 0;
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (nat_is_user_facing(d)) g_fmt_nat[k++] = d;
    }
    g_fmt_nat_count = k;
    g_fmt_nat_total = total;
}

static int rc_native_count(void) {
    ensure_fmt_nat();
    return g_fmt_nat_count;
}

static const char* rc_native_canonical(int i) {
    ensure_fmt_nat();
    return (i >= 0 && i < g_fmt_nat_count) ? g_fmt_nat[i]->canonical : NULL;
}

static const char* rc_native_ascii(int i) {
    ensure_fmt_nat();
    if (i < 0 || i >= g_fmt_nat_count) return NULL;
    const OwlNativeDef* d = g_fmt_nat[i];
    return d->canonicalAscii ? d->canonicalAscii : d->canonical;
}

static const char* rc_native_en(int i) {
    ensure_fmt_nat();
    return (i >= 0 && i < g_fmt_nat_count) ? g_fmt_nat[i]->nameEN : NULL;
}

static int rc_native_alias_count(int i) {
    ensure_fmt_nat();
    if (i < 0 || i >= g_fmt_nat_count) return 0;
    const char* const* a = g_fmt_nat[i]->aliases;
    if (!a) return 0;
    int n = 0;
    while (a[n]) n++;
    return n;
}

static const char* rc_native_alias(int i, int aliasIndex) {
    ensure_fmt_nat();
    if (i < 0 || i >= g_fmt_nat_count) return NULL;
    const char* const* a = g_fmt_nat[i]->aliases;
    if (!a) return NULL;
    for (int n = 0; a[n]; n++)
        if (n == aliasIndex) return a[n];
    return NULL;
}

/* ---- Constantes ---------------------------------------------------------- */
static int rc_const_count(void) {
    return owl_registry_consts_count();
}

static const char* rc_const_name(int i) {
    const OwlConstDef* c = owl_registry_const_at(i);
    return c ? c->name : NULL;
}

/* ---- Proprietes builtin (date/heure/tableau/JSON/duree) ------------------ */
static int rc_builtin_prop_count(void) {
    return compiler_builtin_prop_count();
}

static const char* rc_builtin_prop_fr(int i) {
    const char* fr = NULL;
    compiler_builtin_prop_at(i, &fr, NULL);
    return fr;
}

static const char* rc_builtin_prop_en(int i) {
    const char* en = NULL;
    compiler_builtin_prop_at(i, NULL, &en);
    return en;
}

/* ---- Types builtin (struct-like) ----------------------------------------- */
static int rc_type_count(void) {
    return owl_registry_types_count();
}

static const char* rc_type_canonical(int i) {
    const OwlTypeDef* t = owl_registry_type_at(i);
    return t ? t->canonical : NULL;
}

static const char* rc_type_ascii(int i) {
    const OwlTypeDef* t = owl_registry_type_at(i);
    if (!t) return NULL;
    return t->canonicalAscii ? t->canonicalAscii : t->canonical;
}

static int rc_type_genre(int i) {
    const OwlTypeDef* t = owl_registry_type_at(i);
    return t ? (int)t->genre : 0;
}

static int rc_type_field_count(int ti) {
    const OwlTypeDef* t = owl_registry_type_at(ti);
    if (!t || !t->fields) return 0;
    int n = 0;
    for (const OwlField* f = t->fields; f->name != NULL; f++) n++;
    return n;
}

static const char* rc_type_field_key(int ti, int fi) {
    const OwlTypeDef* t = owl_registry_type_at(ti);
    if (!t || !t->fields) return NULL;
    int n = 0;
    for (const OwlField* f = t->fields; f->name != NULL; f++, n++)
        if (n == fi) return f->name;
    return NULL;
}

static const char* rc_type_field_display(int ti, int fi) {
    const OwlTypeDef* t = owl_registry_type_at(ti);
    if (!t || !t->fields) return NULL;
    int n = 0;
    for (const OwlField* f = t->fields; f->name != NULL; f++, n++)
        if (n == fi) return f->nameAccented ? f->nameAccented : f->name;
    return NULL;
}

/* ---- translate type-aware ------------------------------------- */
static const char* rc_type_en(int ti) {
    const OwlTypeDef* t = owl_registry_type_at(ti);
    return t ? t->nameEN : NULL;
}

static const char* rc_type_field_en(int ti, int fi) {
    const OwlTypeDef* t = owl_registry_type_at(ti);
    if (!t || !t->fields) return NULL;
    int n = 0;
    for (const OwlField* f = t->fields; f->name != NULL; f++, n++)
        if (n == fi) return f->nameEN;
    return NULL;
}

static const char* rc_type_field_type(int ti, int fi) {
    const OwlTypeDef* t = owl_registry_type_at(ti);
    if (!t || !t->fields) return NULL;
    int n = 0;
    for (const OwlField* f = t->fields; f->name != NULL; f++, n++)
        if (n == fi) return f->type;
    return NULL;
}

static const char* rc_const_en(int i) {
    const OwlConstDef* c = owl_registry_const_at(i);
    return c ? c->nameEN : NULL;
}

/* ---- Methodes d'extension ------------------------------------------------- */
static int rc_method_count(void) {
    return method_catalog_count();
}

static const char* rc_method_name(int i) {
    const char* typeName = NULL;
    const MethodEntry* e = method_catalog_at(i, &typeName);
    return e ? e->name : NULL;
}

/*  — graphie soeur (autre langue) d'une methode fluente. Une paire
 * bilingue est enregistree deux fois avec le MEME receveur et le MEME pointeur
 * d'impl (`Rétropropage`/`Backpropagate`). On resout `name` (accent/casse-
 * insensible), puis on cherche les autres entrees partageant (type, impl).
 * Exactement une soeur -> pairing sur ; zero ou plusieurs (alias ASCII, formes
 * prefixees comme les 5 graphies de fn_dateheure_vers_reel) -> NULL, le caller
 * retombe sur le catalogue natives. */
static const char* rc_method_partner(const char* name, int len) {
    if (name == NULL || len <= 0 || len >= 200) return NULL;
    char buf[256];
    memcpy(buf, name, (size_t)len);
    buf[len] = '\0';
    char q[256];
    owl_normalize_name(buf, q, sizeof q);

    int total = method_catalog_count();
    /* Localise l'entree source. */
    NativeFn srcFn = NULL;
    const char* srcType = NULL;
    for (int i = 0; i < total; i++) {
        const char* typeName = NULL;
        const MethodEntry* e = method_catalog_at(i, &typeName);
        if (e && e->name && owl_name_matches(e->name, q)) {
            srcFn = e->function; srcType = typeName; break;
        }
    }
    if (srcFn == NULL) return NULL;

    /* Cherche les soeurs : meme (type, impl), graphie differente. */
    const char* partner = NULL;
    for (int i = 0; i < total; i++) {
        const char* typeName = NULL;
        const MethodEntry* e = method_catalog_at(i, &typeName);
        if (!e || e->function != srcFn) continue;
        if (typeName != srcType && (typeName == NULL || srcType == NULL
                                    || strcmp(typeName, srcType) != 0))
            continue;
        if (e->name && owl_name_matches(e->name, q)) continue;  /* la source */
        if (partner != NULL) return NULL;   /* >= 2 soeurs -> ambigu */
        partner = e->name;
    }
    return partner;
}

/* ---- Proprietes de blocs DSL --------------------------------------------- */
static const OwlControlPropDef* rc_dsl_def(int i) {
    const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
    return (e && e->def) ? e->def : NULL;
}

static int rc_dsl_prop_count(void) {
    return owl_dsl_props_count();
}

static const char* rc_dsl_prop_fr(int i) {
    const OwlControlPropDef* p = rc_dsl_def(i);
    return p ? p->nameFR : NULL;
}

static const char* rc_dsl_prop_ascii(int i) {
    const OwlControlPropDef* p = rc_dsl_def(i);
    return p ? p->nameAscii : NULL;
}

static const char* rc_dsl_prop_en(int i) {
    const OwlControlPropDef* p = rc_dsl_def(i);
    return p ? p->nameEN : NULL;
}

static const char* rc_dsl_prop_kind(int i) {
    const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
    return e ? e->kind : NULL;
}

static const char* rc_dsl_prop_object_type(int i) {
    const OwlControlPropDef* p = rc_dsl_def(i);
    return p ? p->object_type : NULL;
}

static const OwlFmtCanon REGISTRY_CANON = {
    .native_count          = rc_native_count,
    .native_canonical      = rc_native_canonical,
    .native_ascii          = rc_native_ascii,
    .native_en             = rc_native_en,
    .native_alias_count    = rc_native_alias_count,
    .native_alias          = rc_native_alias,
    .const_count           = rc_const_count,
    .const_name            = rc_const_name,
    .builtin_prop_count    = rc_builtin_prop_count,
    .builtin_prop_fr       = rc_builtin_prop_fr,
    .builtin_prop_en       = rc_builtin_prop_en,
    .type_count            = rc_type_count,
    .type_canonical        = rc_type_canonical,
    .type_ascii            = rc_type_ascii,
    .type_genre            = rc_type_genre,
    .type_field_count      = rc_type_field_count,
    .type_field_key        = rc_type_field_key,
    .type_field_display    = rc_type_field_display,
    .type_en               = rc_type_en,
    .type_field_en         = rc_type_field_en,
    .type_field_type       = rc_type_field_type,
    .const_en              = rc_const_en,
    .method_count          = rc_method_count,
    .method_name           = rc_method_name,
    .method_partner        = rc_method_partner,
    .dsl_prop_count        = rc_dsl_prop_count,
    .dsl_prop_fr           = rc_dsl_prop_fr,
    .dsl_prop_ascii        = rc_dsl_prop_ascii,
    .dsl_prop_en           = rc_dsl_prop_en,
    .dsl_prop_kind         = rc_dsl_prop_kind,
    .dsl_prop_object_type  = rc_dsl_prop_object_type,
};

const OwlFmtCanon* owl_fmt_canon_registry(void) {
    return &REGISTRY_CANON;
}
