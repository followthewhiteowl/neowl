// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* etat_decl.c - Implementation des structures IR du DSL Etat (V2).
 *
 * Voir etat_decl.h.
 */

#include "etat_decl.h"

#include "scanner.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * Helpers internes
 * ============================================================ */

static char* dup_cstr(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

/* Deux noms UTILISATEUR (blocs et events nommes d'un Etat) designent-ils le
 * meme identifiant ? Invariant 2 : casse ET accents ignores, comme partout
 * ailleurs dans le langage. `sep` et `sép` sont deux graphies du meme nom,
 * donc un doublon a la declaration et la meme cible a la recherche.
 * Declaration (compiler.c, etat_parse_subblock) et recherche
 * (owl_etat_decl_find_*) passent toutes deux par ici : elles ne peuvent pas
 * diverger. Buffers dimensionnes au-dessus du nameBuf[128] du parser pour que
 * la troncature de scanner_strip_accents ne fabrique pas de faux doublon. */
static bool name_eq_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    char sa[256], sb[256];
    int na = scanner_strip_accents(a, (int)strlen(a), sa, sizeof(sa));
    int nb = scanner_strip_accents(b, (int)strlen(b), sb, sizeof(sb));
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (tolower((unsigned char)sa[i]) != tolower((unsigned char)sb[i])) return false;
    }
    return true;
}

/* Meme regle de comparaison, autre question : ce mot est-il le nom CANON d'un
 * genre de bloc ? Le canon FR porte l'accent (EnTêteDocument, Itération) et la
 * source peut l'ecrire sans. Volontairement distincte de name_eq_ci malgre la
 * ressemblance : l'une classe un mot dans le vocabulaire du DSL, l'autre
 * compare deux identifiants utilisateur. Les confondre rendrait le jour ou
 * l'une des deux regles bouge indetectable. */
static bool canon_eq_ci_ai(const char* a, const char* b) {
    if (!a || !b) return false;
    char sa[64], sb[64];
    int na = scanner_strip_accents(a, (int)strlen(a), sa, sizeof(sa));
    int nb = scanner_strip_accents(b, (int)strlen(b), sb, sizeof(sb));
    if (na <= 0 || nb <= 0 || na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (tolower((unsigned char)sa[i]) != tolower((unsigned char)sb[i])) return false;
    }
    return true;
}

#define RESIZE_ARRAY(type, ptr, count, capacity) do {                  \
    if ((count) >= (capacity)) {                                        \
        int new_cap = (capacity) < 4 ? 4 : (capacity) * 2;             \
        (ptr) = (type*)realloc((ptr), sizeof(type) * (size_t)new_cap); \
        (capacity) = new_cap;                                           \
    }                                                                   \
} while (0)

/* ============================================================
 * OwlEtatBlockKind <-> nom
 * ============================================================ */

int owl_etat_block_kind_from_name(const char* name) {
    if (!name) return -1;
    /* Formes FR + EN canon (cf. CONTROL_KINDS dans ui_decl.c). Compare en
     * accent-insensible : les canons FR EnTêteDocument / Itération s'ecrivent
     * aussi sans accent. */
    if (canon_eq_ci_ai(name, "EnTeteDocument") ||
        canon_eq_ci_ai(name, "DocumentHeader")) return OWL_ETAT_BLOCK_ENTETEDOC;
    if (canon_eq_ci_ai(name, "HautDePage") ||
        canon_eq_ci_ai(name, "PageHeader"))     return OWL_ETAT_BLOCK_HAUTPAGE;
    if (canon_eq_ci_ai(name, "Corps") ||
        canon_eq_ci_ai(name, "Body"))           return OWL_ETAT_BLOCK_CORPS;
    if (canon_eq_ci_ai(name, "BasDePage") ||
        canon_eq_ci_ai(name, "PageFooter"))     return OWL_ETAT_BLOCK_BASPAGE;
    if (canon_eq_ci_ai(name, "FinDeDocument") ||
        canon_eq_ci_ai(name, "DocumentFooter")) return OWL_ETAT_BLOCK_FINDOC;
    if (canon_eq_ci_ai(name, "Iteration"))      return OWL_ETAT_BLOCK_ITERATION;
    return -1;
}

/* ============================================================
 * OwlEtatBlockDecl
 * ============================================================ */

void owl_etat_block_decl_add_property(OwlEtatBlockDecl* b, const char* name,
                                       Value const_value, ObjFunction* eval,
                                       bool is_constant, int line) {
    if (!b || !name) return;
    RESIZE_ARRAY(OwlPropertyInit, b->properties, b->property_count, b->property_capacity);
    OwlPropertyInit* p = &b->properties[b->property_count++];
    p->name        = dup_cstr(name);
    p->const_value = const_value;
    p->eval        = eval;
    p->is_constant = is_constant;
    p->line        = line;
}

static void block_decl_free(OwlEtatBlockDecl* b) {
    if (!b) return;
    free(b->name);
    for (int i = 0; i < b->property_count; i++) {
        owl_property_init_free(&b->properties[i]);
    }
    free(b->properties);
    for (int i = 0; i < b->control_count; i++) {
        owl_control_decl_free(b->controls[i]);
    }
    free(b->controls);
}

void owl_etat_block_decl_add_control(OwlEtatBlockDecl* b, OwlControlDecl* c) {
    if (!b || !c) return;
    RESIZE_ARRAY(OwlControlDecl*, b->controls, b->control_count, b->control_capacity);
    b->controls[b->control_count++] = c;
}

/* ============================================================
 * OwlEtatDecl
 * ============================================================ */

OwlEtatDecl* owl_etat_decl_new(const char* name, int line) {
    OwlEtatDecl* e = (OwlEtatDecl*)calloc(1, sizeof(OwlEtatDecl));
    if (!e) return NULL;
    e->name = dup_cstr(name);
    e->line = line;
    return e;
}

void owl_etat_decl_free(OwlEtatDecl* e) {
    if (!e) return;
    free(e->name);
    for (int i = 0; i < e->property_count; i++) {
        owl_property_init_free(&e->properties[i]);
    }
    free(e->properties);
    for (int i = 0; i < e->block_count; i++) {
        block_decl_free(&e->blocks[i]);
    }
    free(e->blocks);
    for (int i = 0; i < e->event_count; i++) {
        owl_event_block_free(&e->events[i]);
    }
    free(e->events);
    for (int i = 0; i < e->user_proc_count; i++) {
        owl_event_block_free(&e->user_procs[i]);
    }
    free(e->user_procs);
    for (int i = 0; i < e->param_count; i++) {
        free(e->params[i].name);
        free(e->params[i].type_name);
    }
    free(e->params);
    free(e);
}

void owl_etat_decl_add_param(OwlEtatDecl* e, const char* name,
                                const char* type_name,
                                Value default_value, bool has_default, int line) {
    if (!e || !name) return;
    RESIZE_ARRAY(OwlEtatParam, e->params, e->param_count, e->param_capacity);
    OwlEtatParam* p = &e->params[e->param_count++];
    p->name          = dup_cstr(name);
    p->type_name     = type_name ? dup_cstr(type_name) : NULL;
    p->default_value = has_default ? default_value : NIL_VAL;
    p->has_default   = has_default;
    p->line          = line;
}

void owl_etat_decl_add_property(OwlEtatDecl* e, const char* name,
                                  Value const_value, ObjFunction* eval,
                                  bool is_constant, int line) {
    if (!e || !name) return;
    RESIZE_ARRAY(OwlPropertyInit, e->properties, e->property_count, e->property_capacity);
    OwlPropertyInit* p = &e->properties[e->property_count++];
    p->name        = dup_cstr(name);
    p->const_value = const_value;
    p->eval        = eval;
    p->is_constant = is_constant;
    p->line        = line;
}

void owl_etat_decl_add_event(OwlEtatDecl* e, const char* name,
                                ObjFunction* impl, int line) {
    if (!e || !name) return;
    RESIZE_ARRAY(OwlEventBlock, e->events, e->event_count, e->event_capacity);
    OwlEventBlock* ev = &e->events[e->event_count++];
    ev->name = dup_cstr(name);
    ev->impl = impl;
    ev->line = line;
}

void owl_etat_decl_add_user_proc(OwlEtatDecl* e, const char* name,
                                    ObjFunction* impl, int line) {
    if (!e || !name) return;
    RESIZE_ARRAY(OwlEventBlock, e->user_procs, e->user_proc_count, e->user_proc_capacity);
    OwlEventBlock* ev = &e->user_procs[e->user_proc_count++];
    ev->name = dup_cstr(name);
    ev->impl = impl;
    ev->line = line;
}

OwlEtatBlockDecl* owl_etat_decl_add_block(OwlEtatDecl* e, OwlEtatBlockKind kind,
                                            const char* name, int line) {
    if (!e || !name) return NULL;
    RESIZE_ARRAY(OwlEtatBlockDecl, e->blocks, e->block_count, e->block_capacity);
    OwlEtatBlockDecl* b = &e->blocks[e->block_count++];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->name = dup_cstr(name);
    b->line = line;
    return b;
}

const OwlEventBlock* owl_etat_decl_find_event(const OwlEtatDecl* e, const char* name) {
    if (!e || !name) return NULL;
    for (int i = 0; i < e->event_count; i++) {
        if (name_eq_ci(e->events[i].name, name)) return &e->events[i];
    }
    return NULL;
}

const OwlEtatBlockDecl* owl_etat_decl_find_block_by_name(const OwlEtatDecl* e, const char* name) {
    if (!e || !name) return NULL;
    for (int i = 0; i < e->block_count; i++) {
        if (name_eq_ci(e->blocks[i].name, name)) return &e->blocks[i];
    }
    return NULL;
}

const OwlEtatBlockDecl* owl_etat_decl_find_canon_block(const OwlEtatDecl* e, OwlEtatBlockKind kind) {
    if (!e || kind == OWL_ETAT_BLOCK_ITERATION) return NULL;
    for (int i = 0; i < e->block_count; i++) {
        if (e->blocks[i].kind == kind) return &e->blocks[i];
    }
    return NULL;
}
