// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ui_decl.c - Implementation des structures IR du DSL fenetre v1.
 *
 * Voir ui_decl.h.
 */

#include "ui_decl.h"
#include "memory.h"
#include "scanner.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* Comparaison case-insensitive stricte (sans strip d'accents).
 * Utilisee pour matcher les noms de proprietes/evenements/kinds V1, qui
 * sont en ASCII pur cote API (Largeur, Hauteur, Table, etc.). Les rares
 * formes accentuees (Selecteur/Selecteur) sont gerees explicitement par
 * double appel cote control_kind_of(). */
static bool name_eq_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Comparaison case-insensitive ET accent-insensitive, pour les noms de
 * controles definis par l'utilisateur (TAB_Donnees == TAB_Donnees).
 * Aligne l'invariant 2 du langage : tout identifiant utilisateur est
 * resolu sur sa forme strippee, comme via scanner_strip_accents(). */
static bool ctrl_name_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    char sa[256], sb[256];
    int la = scanner_strip_accents(a, (int)strlen(a), sa, sizeof(sa));
    int lb = scanner_strip_accents(b, (int)strlen(b), sb, sizeof(sb));
    if (la != lb) return false;
    return strncasecmp(sa, sb, (size_t)la) == 0;
}

/* Resize d'un tableau dynamique de bytes (pattern utilise dans tout le code). */
#define RESIZE_ARRAY(type, ptr, count, capacity) do {                  \
    if ((count) >= (capacity)) {                                        \
        int new_cap = (capacity) < 4 ? 4 : (capacity) * 2;             \
        (ptr) = (type*)realloc((ptr), sizeof(type) * (size_t)new_cap); \
        (capacity) = new_cap;                                           \
    }                                                                   \
} while (0)

/* ============================================================
 * OwlPropertyInit
 * ============================================================ */

void owl_property_init_free(OwlPropertyInit* p) {
    if (!p) return;
    free(p->name);
    p->name = NULL;
    /* eval (ObjFunction*) est gere par le GC, pas free explicite. */
    /* const_value : si OBJ, GC s'en occupe. */
}

/* ============================================================
 * OwlEventBlock
 * ============================================================ */

void owl_event_block_free(OwlEventBlock* e) {
    if (!e) return;
    free(e->name);
    e->name = NULL;
    /* impl gere par GC */
}

/* ============================================================
 * OwlWindowVariable
 * ============================================================ */

void owl_window_variable_free(OwlWindowVariable* v) {
    if (!v) return;
    free(v->name);
    free(v->type_name);
    v->name = NULL;
    v->type_name = NULL;
}

/* ============================================================
 * OwlControlDecl
 * ============================================================ */

/*  — sentinelle « non posé » sur une brique de style de volet
 * (calloc les mettrait a 0 = noir/false, ce qui n'est pas « non posé »). */
static void onglet_style_brick_init_unset(OwlOngletStyleBrick* b) {
    b->couleur         = -1;
    b->couleur_fond    = -1;
    b->police_nom      = NULL;
    b->police_taille   = -1;
    b->police_gras     = -1;
    b->police_italique = -1;
    b->police_souligne = -1;
    b->police_barre    = -1;
    b->arrondi         = -1;   /*  : arrondi des coins de la languette */
}

OwlControlDecl* owl_control_decl_new(OwlControlKind kind, const char* name, int line) {
    OwlControlDecl* c = (OwlControlDecl*)calloc(1, sizeof(OwlControlDecl));
    c->kind = kind;
    c->name = dup_cstr(name);
    c->line = line;
    /*  — slots de style Onglet a « non posé » (repli au défaut). */
    c->ong_cadre_ext.couleur_fond    = -1;
    c->ong_cadre_ext.cadre_couleur   = -1;
    c->ong_cadre_ext.cadre_epaisseur = -1;
    c->ong_cadre_ext.cadre_arrondi   = -1;
    onglet_style_brick_init_unset(&c->ong_actif_base);
    onglet_style_brick_init_unset(&c->ong_actif_survol);
    onglet_style_brick_init_unset(&c->ong_inactif_base);
    onglet_style_brick_init_unset(&c->ong_inactif_survol);
    return c;
}

/*  — une affectation de la propriete Style ENTIERE (reference de style)
 * annule les slots plats deja poses par les formes littérale/statement. Canon
 *  : `Style..sous = Y` puis `Style = X` -> X gagne (reset). Les
 * ecritures `Style..` qui SUIVENT repartent donc d'une ardoise vierge et sont
 * appliquees au runtime en surcharge de l'instance resolue. */
void owl_control_decl_reset_onglet_style(OwlControlDecl* c) {
    if (!c) return;
    free(c->ong_actif_base.police_nom);
    free(c->ong_actif_survol.police_nom);
    free(c->ong_inactif_base.police_nom);
    free(c->ong_inactif_survol.police_nom);
    c->ong_has_style = 0;
    c->ong_cadre_ext.couleur_fond    = -1;
    c->ong_cadre_ext.cadre_couleur   = -1;
    c->ong_cadre_ext.cadre_epaisseur = -1;
    c->ong_cadre_ext.cadre_arrondi   = -1;
    onglet_style_brick_init_unset(&c->ong_actif_base);
    onglet_style_brick_init_unset(&c->ong_actif_survol);
    onglet_style_brick_init_unset(&c->ong_inactif_base);
    onglet_style_brick_init_unset(&c->ong_inactif_survol);
}

void owl_control_decl_free(OwlControlDecl* c) {
    if (!c) return;
    for (int i = 0; i < c->property_count; i++) owl_property_init_free(&c->properties[i]);
    for (int i = 0; i < c->event_count; i++)    owl_event_block_free(&c->events[i]);
    for (int i = 0; i < c->user_proc_count; i++) owl_event_block_free(&c->user_procs[i]);
    for (int i = 0; i < c->column_count; i++) {
        OwlColumnDecl* col = &c->columns[i];
        for (int j = 0; j < col->property_count; j++) owl_property_init_free(&col->properties[j]);
        free(col->properties);
        free(col->name);
    }
    free(c->properties);
    free(c->events);
    free(c->user_procs);
    free(c->columns);
    free(c->name);
    free(c->parent_name);
    free(c->context_menu_name);
    free(c->magnifier_menu_name);
    /*  — noms de police posés sur les briques de style Onglet. */
    free(c->ong_actif_base.police_nom);
    free(c->ong_actif_survol.police_nom);
    free(c->ong_inactif_base.police_nom);
    free(c->ong_inactif_survol.police_nom);
    free(c);
}

void owl_control_decl_set_parent_name(OwlControlDecl* c, const char* parent_name) {
    if (!c) return;
    free(c->parent_name);
    c->parent_name = (parent_name && *parent_name) ? dup_cstr(parent_name) : NULL;
}

void owl_control_decl_add_property(OwlControlDecl* c, const char* name,
                                    Value const_value, ObjFunction* eval,
                                    bool is_constant, int line) {
    RESIZE_ARRAY(OwlPropertyInit, c->properties, c->property_count, c->property_capacity);
    OwlPropertyInit* p = &c->properties[c->property_count++];
    p->name        = dup_cstr(name);
    p->const_value = const_value;
    p->eval        = eval;
    p->is_constant = is_constant;
    p->line        = line;
}

void owl_control_decl_add_event(OwlControlDecl* c, const char* name,
                                  ObjFunction* impl, int line) {
    RESIZE_ARRAY(OwlEventBlock, c->events, c->event_count, c->event_capacity);
    OwlEventBlock* e = &c->events[c->event_count++];
    e->name = dup_cstr(name);
    e->impl = impl;
    e->line = line;
}

void owl_control_decl_add_user_proc(OwlControlDecl* c, const char* name,
                                      ObjFunction* impl, int line) {
    RESIZE_ARRAY(OwlEventBlock, c->user_procs, c->user_proc_count, c->user_proc_capacity);
    OwlEventBlock* e = &c->user_procs[c->user_proc_count++];
    e->name = dup_cstr(name);
    e->impl = impl;
    e->line = line;
}

/*  — une colonne deja declaree dans cette Table ? Meme comparaison
 * que les noms de champs (invariants 1 & 2), et surtout meme comparaison que
 * la resolution runtime d'une cellule, qui interroge la colonne sur sa forme
 * strippee : une declaration plus permissive que la lecture laissait deux
 * colonnes coexister dans le modele alors qu'une seule etait atteignable par
 * son nom. La premiere devenait une colonne fantome — elle decalait les
 * valeurs positionnelles de TableAjouteLigne et n'etait jamais relisible. */
const OwlColumnDecl* owl_control_decl_find_column(const OwlControlDecl* c,
                                                  const char* name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->column_count; i++) {
        if (ctrl_name_eq(c->columns[i].name, name)) return &c->columns[i];
    }
    return NULL;
}

OwlColumnDecl* owl_control_decl_add_column(OwlControlDecl* c, const char* name, int line) {
    RESIZE_ARRAY(OwlColumnDecl, c->columns, c->column_count, c->column_capacity);
    OwlColumnDecl* col = &c->columns[c->column_count++];
    col->name = dup_cstr(name);
    col->properties = NULL;
    col->property_count = 0;
    col->property_capacity = 0;
    col->line = line;
    return col;
}

void owl_column_decl_add_property(OwlColumnDecl* col, const char* name,
                                    Value const_value, ObjFunction* eval,
                                    bool is_constant, int line) {
    RESIZE_ARRAY(OwlPropertyInit, col->properties, col->property_count, col->property_capacity);
    OwlPropertyInit* p = &col->properties[col->property_count++];
    p->name        = dup_cstr(name);
    p->const_value = const_value;
    p->eval        = eval;
    p->is_constant = is_constant;
    p->line        = line;
}

/* ============================================================
 * OwlWindowDecl
 * ============================================================ */

static void menu_decl_free(OwlMenuDecl* m);   /* fwd */

OwlWindowDecl* owl_window_decl_new(const char* name, int line) {
    OwlWindowDecl* w = (OwlWindowDecl*)calloc(1, sizeof(OwlWindowDecl));
    /* obj.type sera initialise quand on integrera au GC (V1 phase E). */
    w->name = dup_cstr(name);
    w->line = line;
    return w;
}

void owl_window_decl_free(OwlWindowDecl* w) {
    if (!w) return;
    for (int i = 0; i < w->property_count; i++) owl_property_init_free(&w->properties[i]);
    for (int i = 0; i < w->variable_count; i++) owl_window_variable_free(&w->variables[i]);
    /* controls est un tableau de OwlControlDecl par valeur : on libere les
     * contenus en place (pas owl_control_decl_free qui suppose un pointeur). */
    for (int i = 0; i < w->control_count; i++) {
        OwlControlDecl* c = &w->controls[i];
        for (int j = 0; j < c->property_count; j++) owl_property_init_free(&c->properties[j]);
        for (int j = 0; j < c->event_count; j++)    owl_event_block_free(&c->events[j]);
        for (int j = 0; j < c->user_proc_count; j++) owl_event_block_free(&c->user_procs[j]);
        for (int j = 0; j < c->column_count; j++) {
            OwlColumnDecl* col = &c->columns[j];
            for (int k = 0; k < col->property_count; k++) owl_property_init_free(&col->properties[k]);
            free(col->properties);
            free(col->name);
        }
        free(c->properties);
        free(c->events);
        free(c->user_procs);
        free(c->columns);
        free(c->name);
        free(c->context_menu_name);
        free(c->magnifier_menu_name);
    }
    for (int i = 0; i < w->event_count; i++)    owl_event_block_free(&w->events[i]);
    for (int i = 0; i < w->user_proc_count; i++) owl_event_block_free(&w->user_procs[i]);
    for (int i = 0; i < w->param_count; i++) {
        free(w->params[i].name);
        free(w->params[i].type_name);
    }
    for (int i = 0; i < w->menu_count; i++) menu_decl_free(w->menus[i]);
    free(w->menus);
    free(w->main_menu_name);
    free(w->context_menu_name);
    free(w->params);
    free(w->properties);
    free(w->variables);
    free(w->controls);
    free(w->events);
    free(w->user_procs);
    free(w->name);
    free(w);
}

void owl_window_decl_add_property(OwlWindowDecl* w, const char* name,
                                    Value const_value, ObjFunction* eval,
                                    bool is_constant, int line) {
    RESIZE_ARRAY(OwlPropertyInit, w->properties, w->property_count, w->property_capacity);
    OwlPropertyInit* p = &w->properties[w->property_count++];
    p->name        = dup_cstr(name);
    p->const_value = const_value;
    p->eval        = eval;
    p->is_constant = is_constant;
    p->line        = line;
}

void owl_window_decl_add_control(OwlWindowDecl* w, OwlControlDecl* c) {
    if (!c) return;
    RESIZE_ARRAY(OwlControlDecl, w->controls, w->control_count, w->control_capacity);
    /* Copie shallow : on prend ownership des champs alloues de c. */
    w->controls[w->control_count++] = *c;
    /* Le caller libere l'enveloppe vide (c) via free(c) sans liberer les contenus. */
    free(c);
}

void owl_window_decl_add_event(OwlWindowDecl* w, const char* name,
                                  ObjFunction* impl, int line) {
    RESIZE_ARRAY(OwlEventBlock, w->events, w->event_count, w->event_capacity);
    OwlEventBlock* e = &w->events[w->event_count++];
    e->name = dup_cstr(name);
    e->impl = impl;
    e->line = line;
}

void owl_window_decl_add_user_proc(OwlWindowDecl* w, const char* name,
                                     ObjFunction* impl, int line) {
    RESIZE_ARRAY(OwlEventBlock, w->user_procs, w->user_proc_count, w->user_proc_capacity);
    OwlEventBlock* e = &w->user_procs[w->user_proc_count++];
    e->name = dup_cstr(name);
    e->impl = impl;
    e->line = line;
}

/*  — peuplement de variables[] pour les Groupes top-level fenetre.
 * Premiere mise en service de cette infrastructure dormante (declaree mais
 * jamais peuplee jusqu'a ). Utilisee par la prop `champ..Groupe`
 * qui parcourt vm.windows pour collecter les Groupes contenant un controle. */
void owl_window_decl_add_variable(OwlWindowDecl* w, const char* name,
                                   const char* type_name, int line) {
    if (!w || !name) return;
    RESIZE_ARRAY(OwlWindowVariable, w->variables, w->variable_count, w->variable_capacity);
    OwlWindowVariable* v = &w->variables[w->variable_count++];
    v->name      = dup_cstr(name);
    v->type_name = dup_cstr(type_name);
    v->init      = NULL;
    v->line      = line;
}

void owl_window_decl_add_param(OwlWindowDecl* w, const char* name,
                                const char* type_name,
                                Value default_value, bool has_default, int line) {
    if (!w || !name) return;
    RESIZE_ARRAY(OwlWindowParam, w->params, w->param_count, w->param_capacity);
    OwlWindowParam* p = &w->params[w->param_count++];
    p->name          = dup_cstr(name);
    p->type_name     = type_name ? dup_cstr(type_name) : NULL;
    p->default_value = has_default ? default_value : NIL_VAL;
    p->has_default   = has_default;
    p->line          = line;
}

/* ============================================================
 * Menu classique
 * ============================================================ */

static OwlMenuNode* menu_node_new(OwlMenuNodeKind kind, const char* name, int line) {
    OwlMenuNode* n = (OwlMenuNode*)calloc(1, sizeof(OwlMenuNode));
    n->kind = kind;
    n->name = dup_cstr(name);
    n->line = line;
    return n;
}

static void menu_node_free(OwlMenuNode* n) {
    if (!n) return;
    for (int i = 0; i < n->property_count; i++) owl_property_init_free(&n->properties[i]);
    for (int i = 0; i < n->event_count; i++)    owl_event_block_free(&n->events[i]);
    for (int i = 0; i < n->child_count; i++)    menu_node_free(n->children[i]);
    free(n->properties);
    free(n->events);
    free(n->children);
    free(n->name);
    free(n);
}

/*  — sentinelle « non pose » sur toutes les feuilles d'une brique
 * de style (calloc les mettrait a 0 = noir/false, ce qui n'est pas « non pose »). */
static void menu_style_brick_init_unset(OwlMenuStyleBrick* b) {
    b->couleur         = -1;
    b->couleur_fond    = -1;
    b->police_nom      = NULL;
    b->police_taille   = -1;
    b->police_gras     = -1;
    b->police_italique = -1;
    b->police_souligne = -1;
    b->police_barre    = -1;
}

/*  — une affectation de la propriete Style ENTIERE (reference de style)
 * annule les slots plats deja poses par les formes littérale/statement. Canon
 *  : `Style..sous = Y` puis `Style = X` -> X gagne (reset). Les
 * ecritures `Style..` qui SUIVENT repartent donc d'une ardoise vierge et sont
 * appliquees au runtime en surcharge de l'instance resolue. Pendant exact de
 * owl_control_decl_reset_onglet_style. */
void owl_menu_decl_reset_style(OwlMenuDecl* m) {
    if (!m) return;
    free(m->barre_base.police_nom);
    free(m->barre_survol.police_nom);
    free(m->option_base.police_nom);
    free(m->option_survol.police_nom);
    m->has_style = 0;
    menu_style_brick_init_unset(&m->barre_base);
    menu_style_brick_init_unset(&m->barre_survol);
    menu_style_brick_init_unset(&m->option_base);
    menu_style_brick_init_unset(&m->option_survol);
    m->sousmenu_couleur_fond       = -1;
    m->sousmenu_couleur_separateur = -1;
    m->sousmenu_arrondi            = -1;
    m->sousmenu_epaisseur          = -1;
    m->sousmenu_couleur_bordure    = -1;
}

/*  — enregistre l'expression d'un `Style = <reference>`. Reutilise
 * OwlPropertyInit pour heriter de la machinerie de resolution differee
 * (resolve_dynamic_props / invalidate_dynamic_props_cache) et du marquage GC
 * (mark_property_inits). Une seconde affectation ecrase la premiere. */
void owl_menu_decl_set_style_ref(OwlMenuDecl* m, ObjFunction* eval, int line) {
    if (!m) return;
    owl_property_init_free(&m->style_ref);
    m->style_ref.name        = dup_cstr("Style");
    m->style_ref.const_value = NIL_VAL;
    m->style_ref.eval        = eval;
    m->style_ref.is_constant = false;
    m->style_ref.line        = line;
    m->has_style_ref         = 1;
}

static void menu_decl_free(OwlMenuDecl* m) {
    if (!m) return;
    for (int i = 0; i < m->child_count; i++) menu_node_free(m->children[i]);
    free(m->children);
    free(m->name);
    free(m->barre_base.police_nom);
    free(m->barre_survol.police_nom);
    free(m->option_base.police_nom);
    free(m->option_survol.police_nom);
    owl_property_init_free(&m->style_ref);
    free(m);
}

OwlMenuDecl* owl_window_decl_add_menu(OwlWindowDecl* w, const char* name, int line) {
    if (!w) return NULL;
    RESIZE_ARRAY(OwlMenuDecl*, w->menus, w->menu_count, w->menu_capacity);
    OwlMenuDecl* m = (OwlMenuDecl*)calloc(1, sizeof(OwlMenuDecl));
    menu_style_brick_init_unset(&m->barre_base);
    menu_style_brick_init_unset(&m->barre_survol);
    menu_style_brick_init_unset(&m->option_base);
    menu_style_brick_init_unset(&m->option_survol);
    m->sousmenu_couleur_fond       = -1;
    m->sousmenu_couleur_separateur = -1;
    m->sousmenu_arrondi            = -1;
    m->sousmenu_epaisseur          = -1;
    m->sousmenu_couleur_bordure    = -1;
    m->name = dup_cstr(name);
    m->line = line;
    w->menus[w->menu_count++] = m;
    return m;
}

void owl_window_decl_set_main_menu(OwlWindowDecl* w, const char* name, int line) {
    if (!w) return;
    free(w->main_menu_name);
    w->main_menu_name = dup_cstr(name);
    w->main_menu_line = line;
}

void owl_control_decl_set_context_menu(OwlControlDecl* c, const char* name, int line) {
    if (!c) return;
    free(c->context_menu_name);
    c->context_menu_name = dup_cstr(name);
    c->context_menu_line = line;
}

void owl_window_decl_set_context_menu(OwlWindowDecl* w, const char* name, int line) {
    if (!w) return;
    free(w->context_menu_name);
    w->context_menu_name = dup_cstr(name);
    w->context_menu_line = line;
}

void owl_control_decl_set_magnifier_menu(OwlControlDecl* c, const char* name, int line) {
    if (!c) return;
    free(c->magnifier_menu_name);
    c->magnifier_menu_name = dup_cstr(name);
    c->magnifier_menu_line = line;
}

OwlMenuNode* owl_menu_decl_add_child(OwlMenuDecl* m, OwlMenuNodeKind kind,
                                      const char* name, int line) {
    if (!m) return NULL;
    RESIZE_ARRAY(OwlMenuNode*, m->children, m->child_count, m->child_capacity);
    OwlMenuNode* n = menu_node_new(kind, name, line);
    m->children[m->child_count++] = n;
    return n;
}

OwlMenuNode* owl_menu_node_add_child(OwlMenuNode* parent, OwlMenuNodeKind kind,
                                      const char* name, int line) {
    if (!parent) return NULL;
    RESIZE_ARRAY(OwlMenuNode*, parent->children, parent->child_count,
                 parent->child_capacity);
    OwlMenuNode* n = menu_node_new(kind, name, line);
    parent->children[parent->child_count++] = n;
    return n;
}

/*  — un enfant de ce meme parent porte-t-il deja ce nom ? Les deux
 * noeuds etaient montes dans le menu, mais la resolution par nom
 * (TestSimuleMenu, attache de menu contextuel, ...) n'atteignait que le
 * premier : le second etait cliquable sans jamais etre adressable. */
const OwlMenuNode* owl_menu_children_find(OwlMenuNode* const* children,
                                          int count, const char* name) {
    if (!children || !name) return NULL;
    for (int i = 0; i < count; i++) {
        if (children[i] && ctrl_name_eq(children[i]->name, name))
            return children[i];
    }
    return NULL;
}

/*  — ce noeud porte-t-il deja un bloc d'evenement de ce nom ? Le
 * cablage runtime n'en retient qu'un (le premier) ; sans ce refus, le second
 * bloc etait du code mort silencieux. Le caller compare des noms deja ramenes
 * a leur forme canon par le catalogue d'evenements, ce qui absorbe la paire
 * FR/EN ; ctrl_name_eq absorbe le reste (casse et accents). */
bool owl_menu_node_has_event(const OwlMenuNode* n, const char* name) {
    if (!n || !name) return false;
    for (int i = 0; i < n->event_count; i++) {
        if (ctrl_name_eq(n->events[i].name, name)) return true;
    }
    return false;
}

void owl_menu_node_add_property(OwlMenuNode* n, const char* name,
                                 Value const_value, ObjFunction* eval,
                                 bool is_constant, int line) {
    if (!n) return;
    RESIZE_ARRAY(OwlPropertyInit, n->properties, n->property_count, n->property_capacity);
    OwlPropertyInit* p = &n->properties[n->property_count++];
    p->name        = dup_cstr(name);
    p->const_value = const_value;
    p->eval        = eval;
    p->is_constant = is_constant;
    p->line        = line;
}

void owl_menu_node_add_event(OwlMenuNode* n, const char* name,
                              ObjFunction* impl, int line) {
    if (!n) return;
    RESIZE_ARRAY(OwlEventBlock, n->events, n->event_count, n->event_capacity);
    OwlEventBlock* e = &n->events[n->event_count++];
    e->name = dup_cstr(name);
    e->impl = impl;
    e->line = line;
}

bool owl_menu_node_has_property(const OwlMenuNode* n, const char* name) {
    if (!n || !name) return false;
    for (int i = 0; i < n->property_count; i++) {
        if (ctrl_name_eq(n->properties[i].name, name)) return true;
    }
    return false;
}

const char* owl_menu_node_kind_name_fr(OwlMenuNodeKind kind) {
    switch (kind) {
        case OWL_MENU_NODE_SUBMENU:   return "SousMenu";
        case OWL_MENU_NODE_OPTION:    return "OptionMenu";
        case OWL_MENU_NODE_SEPARATOR: return "MenuS\xc3\xa9parateur";
    }
    return "?";
}

const OwlMenuDecl* owl_window_decl_find_menu(const OwlWindowDecl* w, const char* name) {
    if (!w || !name) return NULL;
    for (int i = 0; i < w->menu_count; i++) {
        if (ctrl_name_eq(w->menus[i]->name, name)) return w->menus[i];
    }
    return NULL;
}

const OwlControlDecl* owl_window_decl_find_control(const OwlWindowDecl* w, const char* name) {
    if (!w || !name) return NULL;
    for (int i = 0; i < w->control_count; i++) {
        if (ctrl_name_eq(w->controls[i].name, name)) return &w->controls[i];
    }
    return NULL;
}

/* ============================================================
 * Mappings kind <-> nom
 * ============================================================ */

const char* owl_control_kind_name(OwlControlKind kind) {
    switch (kind) {
        case OWL_CONTROL_LABEL:     return "Libelle";
        case OWL_CONTROL_BUTTON:    return "Bouton";
        case OWL_CONTROL_INPUT:     return "Saisie";
        case OWL_CONTROL_TABLE:     return "Table";
        case OWL_CONTROL_CONTAINER: return "Conteneur";
        case OWL_CONTROL_IMAGE:     return "Image";
        case OWL_CONTROL_LISTE:     return "Liste";
        case OWL_CONTROL_COMBO:     return "Combo";
        case OWL_CONTROL_SELECTEUR: return "Selecteur";
        case OWL_CONTROL_INTERRUPTEUR: return "Interrupteur";
        case OWL_CONTROL_ZONE_REPETEE: return "ZoneRepetee";
        case OWL_CONTROL_JAUGE:     return "Jauge";
        case OWL_CONTROL_CODEBARRE: return "CodeBarres";
        case OWL_CONTROL_ONGLET:    return "Onglet";
        case OWL_CONTROL_VOLET:     return "Volet";
        case OWL_CONTROL_WINDOW:    return "Fenetre";
    }
    return "?";
}

const char* owl_control_kind_name_fr(OwlControlKind kind) {
    /* Forme canon accentuee (nameFR de CONTROL_KINDS) pour les messages
     * user-facing. owl_control_kind_name retourne la forme ASCII (slots
     * runtime) ; les diagnostics doivent afficher « Libellé », « Sélecteur »,
     * « ZoneRépétée », identiques a la compile (info->nameFR). */
    const char* ascii = owl_control_kind_name(kind);
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        const char* canonAscii = k->nameAscii ? k->nameAscii : k->nameFR;
        if (strcmp(canonAscii, ascii) == 0) return k->nameFR;
    }
    return ascii;
}

OwlControlKind owl_control_kind_from_name(const char* name) {
    if (name_eq_ci(name, "Libelle"))   return OWL_CONTROL_LABEL;
    if (name_eq_ci(name, "Bouton"))    return OWL_CONTROL_BUTTON;
    if (name_eq_ci(name, "Saisie"))    return OWL_CONTROL_INPUT;
    if (name_eq_ci(name, "Table"))     return OWL_CONTROL_TABLE;
    if (name_eq_ci(name, "Conteneur")) return OWL_CONTROL_CONTAINER;
    if (name_eq_ci(name, "Image"))     return OWL_CONTROL_IMAGE;
    if (name_eq_ci(name, "Liste"))     return OWL_CONTROL_LISTE;
    if (name_eq_ci(name, "Combo"))     return OWL_CONTROL_COMBO;
    if (name_eq_ci(name, "Selecteur") || name_eq_ci(name, "Sélecteur"))
                                       return OWL_CONTROL_SELECTEUR;
    if (name_eq_ci(name, "Interrupteur")) return OWL_CONTROL_INTERRUPTEUR;
    if (name_eq_ci(name, "ZoneRepetee") || name_eq_ci(name, "ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "e") ||
        name_eq_ci(name, "Looper"))
                                       return OWL_CONTROL_ZONE_REPETEE;
    if (name_eq_ci(name, "Jauge"))     return OWL_CONTROL_JAUGE;
    if (name_eq_ci(name, "CodeBarres") || name_eq_ci(name, "BarCodes"))
                                       return OWL_CONTROL_CODEBARRE;
    if (name_eq_ci(name, "Onglet") || name_eq_ci(name, "Tab"))
                                       return OWL_CONTROL_ONGLET;
    if (name_eq_ci(name, "Volet") || name_eq_ci(name, "Pane"))
                                       return OWL_CONTROL_VOLET;
    return OWL_CONTROL_LABEL; /* default fallback, ne devrait pas arriver */
}

/* Lot F — table FR/ASCII/EN unique pour la generation auto de la grammaire
 * TextMate (cf src/main.c::dump_vscode_data + generate_vscode.py). Verifiee
 * par le self-test rule 5c qui exige une entree pour chaque OwlControlKind. */
static const OwlControlKindInfo CONTROL_KINDS[] = {
    /* Champs par entree (cf ui_decl.h) :
     *   nameFR, nameAscii, nameEN, is_widget, defaultProperty, canonicalPrefix,
     *   descFR, descEN, genre
     * .canonicalPrefix — prefixe de nommage canon du type ("BTN_",
     * "SAI_", "COL_"...), souligne inclus. NULL pour les sortes sans convention
     * (Fenetre, Onglet, CodeBarres, Volet) et les sous-blocs structurels.
     * .defaultProperty — convention canon "Valeur" pour les widgets
     * scalaires. NULL pour Table (lignes TAB-sep), Interrupteur (indexe par
     * option [idx]=val), ZoneRepetee (API specifique). Fenetre NULL :
     * `FEN_X = "titre"` est valide dans le canon mais ambigu en OWL top-level —
     * on prefere l'explicite `FEN_X..Titre = "..."`.
     * .descFR/.descEN — user-facing, regle 7 : pas de mention
     * de produit tiers, texte reformule. Affichees par owl --dump-doc-html.
     * .genre — genre grammatical du nom FR, 8e colonne. Lu par
     * `owl translate` pour accorder l'article de la declaration (`a Pane` ->
     * `un Volet`). Vivait avant dans fmt_type_genre[] (formatter.c), loin de
     * la paire de noms qu'il accorde : une sorte ajoutee ici partait sans
     * genre et rendait `est a <Sorte>` en silence. Colonne OBLIGATOIRE,
     * exigee par la regle 57 du self-test registre. */
    {"Fen\xc3\xaatre",      "Fenetre",   "Window",           true,  NULL, NULL,
     "Conteneur principal d'une interface : porte les champs et pilote le cycle de vie de l'affichage.",
     "Top-level container of an interface: holds the controls and drives the display lifecycle.",
     OWL_KIND_GENRE_F},   /* une fenêtre */
    /*  — defaultProperty="Libelle" (pas "Valeur"). Le contenu d'un
     * Libelle est son texte (prop "Libelle"/"Caption" dans PROPS_LABEL) ;
     * "Valeur" n'a pas de slot runtime cable sur le texte du QLabel (read
     * tombe sur un slot headless fantome -> chaine vide ; write ne touche pas
     * le texte). Symetrie avec le widget Image : defaultProperty
     * doit cibler la vraie prop de contenu. `LIB = X`, `x = LIB`, `LIB..Valeur`
     * passent ainsi par set_text/get_text. (Saisie garde "Valeur" : c'est sa
     * vraie prop de contenu, cablee lineedit.) Bouton : meme bug, meme fix —
     * sa caption est la prop "Libelle"/"Caption" (PROPS_BUTTON) ; "Valeur"
     * n'y existe pas et n'a aucun slot runtime cable sur le texte. */
    {"Libell\xc3\xa9",      "Libelle",   "Label",            true,  "Libelle", "LIB_",
     "Texte fixe affiché à l'écran, non éditable.",
     "Static text displayed on screen, not editable.",
     OWL_KIND_GENRE_M},   /* un libellé */
    {"Bouton",              NULL,        "Button",           true,  "Libelle", "BTN_",
     "Bouton cliquable déclenchant une action.",
     "Clickable button that triggers an action.",
     OWL_KIND_GENRE_M},   /* un bouton */
    {"Saisie",              NULL,        "Input",            true,  "Valeur", "SAI_",
     "Champ de saisie de texte éditable par l'utilisateur.",
     "Text input field editable by the user.",
     OWL_KIND_GENRE_F},   /* une saisie */
    {"Table",               NULL,        "Table",            true,  NULL, "TABLE_",
     "Grille de données en lignes et colonnes.",
     "Data grid laid out in rows and columns.",
     OWL_KIND_GENRE_F},   /* une table */
    {"Conteneur",           NULL,        "Container",        true,  NULL, "CONT_",
     "Regroupe et positionne d'autres champs dans une zone.",
     "Groups and positions other controls within an area.",
     OWL_KIND_GENRE_M},   /* un conteneur */
    /*  — defaultProperty="Image" (pas "Valeur"). Le widget Image
     * n'expose pas de prop "Valeur" dans PROPS_IMAGE_FIELD et n'a pas de
     * handler runtime "Valeur" dans stdlib_ui::owl_ui_control_set_prop ;
     * `IMG_X = "/chemin"` etait traduit en `IMG_X..Valeur = "/chemin"` qui
     * populait un champ struct fantome sans toucher au QLabel. La voie
     * canonique est "Image" qui existe + a un handler runtime complet
     * (chemin, image memoire, icone). Cf regle 25b registry_self_test.c. */
    {"Image",               NULL,        "Image",            true,  "Image", "IMG_",
     "Affiche une image depuis un fichier, la mémoire ou une icône.",
     "Displays an image from a file, memory or an icon.",
     /* Le type reifie Image du registre porte deja genre=F ; la sorte porte le
      * sien pour que la table source soit dense (le verrou est mecanique). */
     OWL_KIND_GENRE_F},   /* une image */
    {"Liste",               NULL,        "List",             true,  "Valeur", "LISTE_",
     "Liste d'éléments sélectionnables, un par ligne.",
     "List of selectable items, one per line.",
     OWL_KIND_GENRE_F},   /* une liste */
    {"Combo",               NULL,        "ComboBox",         true,  "Valeur", "COMBO_",
     "Liste déroulante : un choix parmi plusieurs options.",
     "Drop-down list: one choice among several options.",
     /* Sortes ecrites uniquement avec le prefixe `Champ` : l'article se lit sur
      * `Champ`/`Control` (M), leur genre propre n'etait donc jamais consulte et
      * fmt_type_genre[] ne l'avait jamais eu. Il est pose ici quand meme. */
     OWL_KIND_GENRE_M},   /* un combo */
    {"S\xc3\xa9lecteur",    "Selecteur", "RadioGroup",       true,  "Valeur", "SEL_",
     "Groupe d'options exclusives : un seul choix actif à la fois.",
     "Group of mutually exclusive options: a single active choice at a time.",
     OWL_KIND_GENRE_M},   /* un sélecteur */
    {"Interrupteur",        NULL,        "CheckBox",         true,  NULL, "INT_",
     "Case(s) à cocher : état activé/désactivé.",
     "Checkbox(es): on/off state.",
     OWL_KIND_GENRE_M},   /* un interrupteur */
    {"ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "e", "ZoneRepetee", "Looper", true, NULL, "ZR_",
     "Zone répétant une mise en page sur une collection de données.",
     "Area repeating a layout over a collection of data.",
     OWL_KIND_GENRE_F},   /* une zone répétée */
    /* Champ Jauge — barre de progression output-only. defaultProperty="Valeur"
     * (scalaire entier borne). Variantes lineaire horizontale/verticale +
     * symetrique (progression depuis le centre). */
    {"Jauge",               NULL,        "ProgressBar",      true,  "Valeur", "JAU_",
     "Barre de progression affichant une valeur entre deux bornes.",
     "Progress bar showing a value between two bounds.",
     OWL_KIND_GENRE_F},   /* une jauge */
    /* Champ CodeBarres — affichage output-only d'un code-barres genere.
     * defaultProperty="CodeBarres" (calque du widget Image, defaultProperty=
     * "Image") : `CBA = maVariableCodeBarres` affecte le descripteur complet
     * du code-barres, prop VARIANT dediee qui accepte l'instance et regenere
     * l'image affichee. */
    {"CodeBarres",          NULL,        "BarCodes",         true,  "CodeBarres", NULL,
     "Affiche un code-barres généré à partir de son type, son contenu et son niveau de correction d'erreur.",
     "Displays a barcode generated from its type, content and error-correction level.",
     OWL_KIND_GENRE_M},   /* un code-barres */
    /* Champ Onglet — organise le contenu en pages ("volets"), une seule
     * visible a la fois. Widget positionne (is_widget=true). nameEN "Tab" :
     * resolution contextuelle vs la constante TAB (le kind ne gagne qu'apres
     * `est un Champ`). defaultProperty="Valeur" (volet actif, entier 1-based). */
    {"Onglet",              NULL,        "Tab",              true,  "Valeur", NULL,
     "Organise le contenu en pages (volets), une seule visible à la fois ; l'utilisateur navigue en cliquant sur les onglets.",
     "Organizes content into pages (panes), one visible at a time; the user navigates by clicking the tabs.",
     OWL_KIND_GENRE_M},   /* un onglet */
    /* Colonne : sous-element de Table, pas un widget runtime independant.
     * is_widget=false car owl_control_kind_of_info ne resout pas
     * "Colonne" en OwlControlKind (il n'y a pas d'enum dedie). */
    {"Colonne",             NULL,        "Column",           false, NULL, "COL_",
     "Colonne d'une Table : décrit l'affichage d'un champ de données.",
     "Column of a Table: describes how a data field is displayed.",
     OWL_KIND_GENRE_F},   /* une colonne */
    /* Volet : sous-bloc de rôle d'un Champ Onglet (une page). Structurel
     * (is_widget=false, comme Colonne) mais point de recursion enfants comme
     * un Conteneur : porte son Libellé, sa Disposition et des champs enfants. */
    {"Volet",               NULL,        "Pane",             false, NULL, NULL,
     "Page d'un Champ Onglet : porte un libellé et regroupe ses propres champs enfants.",
     "Page of an Onglet field: carries a caption and groups its own child controls.",
     OWL_KIND_GENRE_M},   /* un volet */
    /* DSL Etat V2 — Etat top-level + sous-blocs (HautDePage, Corps...).
     * is_widget=false : ce sont des blocs structurels, pas des widgets
     * affectables. defaultProperty=NULL coherent. */
    {"\xc3\x89tat",         "Etat",      "Report",           false, NULL, NULL,
     "Document imprimable structuré en blocs (en-tête, corps, pied).",
     "Printable document structured in blocks (header, body, footer).",
     OWL_KIND_GENRE_M},   /* un état */
    {"EnT\xc3\xaateDocument", "EnTeteDocument", "DocumentHeader", false, NULL, NULL,
     "Bloc imprimé une fois au tout début du document.",
     "Block printed once at the very start of the document.",
     OWL_KIND_GENRE_M},   /* un en-tête de document */
    {"HautDePage",          NULL,        "PageHeader",       false, NULL, NULL,
     "Bloc répété en haut de chaque page.",
     "Block repeated at the top of every page.",
     OWL_KIND_GENRE_M},   /* un haut de page */
    {"Corps",               NULL,        "Body",             false, NULL, NULL,
     "Bloc répété pour chaque enregistrement parcouru.",
     "Block repeated for each record iterated.",
     OWL_KIND_GENRE_M},   /* un corps */
    {"BasDePage",           NULL,        "PageFooter",       false, NULL, NULL,
     "Bloc répété en bas de chaque page.",
     "Block repeated at the bottom of every page.",
     OWL_KIND_GENRE_M},   /* un bas de page */
    {"FinDeDocument",       NULL,        "DocumentFooter",   false, NULL, NULL,
     "Bloc imprimé une fois à la toute fin du document.",
     "Block printed once at the very end of the document.",
     OWL_KIND_GENRE_F},   /* une fin de document */
    /* Menu classique de fenetre — blocs structurels, pas des
     * widgets posables : ni geometrie, ni prefixe `Champ`, ni materialisation
     * ObjUiControl. is_widget=false, comme les sous-blocs d'Etat. */
    {"MenuPrincipal",       NULL,        "MainMenu",         false, NULL, NULL,
     "Barre de menus d'une fenêtre : arbre nommé de menus déroulants et d'options.",
     "Menu bar of a window: named tree of drop-down menus and options.",
     OWL_KIND_GENRE_M},   /* un menu principal */
    {"MenuContextuel",      NULL,        "ContextMenu",      false, NULL, NULL,
     "Menu affiché au clic droit sur un champ ou une fenêtre : arbre nommé d'options contextuelles.",
     "Menu shown on right-click over a control or a window: named tree of contextual options.",
     OWL_KIND_GENRE_M},   /* un menu contextuel */
    {"SousMenu",            NULL,        "Submenu",          false, NULL, NULL,
     "Menu déroulant d'une barre de menus : porte un libellé et contient d'autres éléments de menu.",
     "Drop-down menu of a menu bar: carries a caption and holds further menu elements.",
     OWL_KIND_GENRE_M},   /* un sous-menu */
    {"OptionMenu",          NULL,        "MenuOption",       false, NULL, NULL,
     "Élément terminal d'un menu : déclenche une action à la sélection.",
     "Terminal menu element: triggers an action when selected.",
     OWL_KIND_GENRE_F},   /* une option de menu */
    {"MenuS\xc3\xa9parateur", "MenuSeparateur", "MenuSeparator", false, NULL, NULL,
     "Trait de séparation entre deux groupes d'éléments dans un menu.",
     "Separator line between two groups of elements inside a menu.",
     OWL_KIND_GENRE_M},   /* un séparateur */
    {"It\xc3\xa9ration",    "Iteration", "Iteration",        false, NULL, NULL,
     "Bloc d'itération sur une sous-collection à l'intérieur du corps.",
     "Iteration block over a sub-collection inside the body.",
     OWL_KIND_GENRE_F},   /* une itération */
    {NULL, NULL, NULL, false, NULL, NULL, NULL, NULL, OWL_KIND_GENRE_NONE},
};
const OwlControlKindInfo* owl_control_kinds_table(void) {
    return CONTROL_KINDS;
}

/* Resout un nom canon (un des nameFR/nameAscii/nameEN d'une entree
 * CONTROL_KINDS) en OwlControlKind. Mapping local au .c pour avoir la
 * connaissance des deux : la table CONTROL_KINDS (data) et l'enum
 * OwlControlKind (semantique). */
static bool kind_from_canon(const char* canon, OwlControlKind* out) {
    if (!canon) return false;
    /* Compare au canonical FR de chaque case du switch (idem
     * owl_control_kind_name + EN/Ascii fallback de from_name). */
    OwlControlKind candidates[] = {
        OWL_CONTROL_LABEL, OWL_CONTROL_BUTTON, OWL_CONTROL_INPUT,
        OWL_CONTROL_TABLE, OWL_CONTROL_CONTAINER, OWL_CONTROL_IMAGE,
        OWL_CONTROL_LISTE, OWL_CONTROL_COMBO, OWL_CONTROL_SELECTEUR,
        OWL_CONTROL_INTERRUPTEUR, OWL_CONTROL_ZONE_REPETEE,
        OWL_CONTROL_JAUGE, OWL_CONTROL_CODEBARRE,
        OWL_CONTROL_ONGLET, OWL_CONTROL_VOLET, OWL_CONTROL_WINDOW,
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        OwlControlKind k = candidates[i];
        const char* nm = owl_control_kind_name(k);
        if (name_eq_ci(canon, nm)) {
            if (out) *out = k;
            return true;
        }
    }
    /* Fallback via from_name (qui couvre les variantes accentuees). */
    OwlControlKind k = owl_control_kind_from_name(canon);
    /* from_name fallback retourne LABEL si rien trouve — distinguer le
     * vrai LABEL d'un fallback : on a deja teste LABEL au-dessus avec
     * name_eq_ci, donc si on est ici c'est un faux match. */
    (void)k;
    return false;
}

bool owl_control_kind_of_info(const OwlControlKindInfo* info,
                               OwlControlKind* out_kind) {
    if (!info || !info->nameFR) return false;
    /* Sous-blocs DSL Etat (Iteration, Corps, ...) : pas un widget runtime.
     * On ne mappe pas vers OwlControlKind ; le caller doit filtrer
     * is_widget=false en amont. */
    if (!info->is_widget) return false;
    /* owl_control_kind_name retourne la forme ASCII (cf switch dans ce
     * fichier). On compare prioritairement contre nameAscii pour eviter
     * les faux negatifs sur les noms accentues (Libellé / ZoneRépétée). */
    const char* canonAscii = info->nameAscii ? info->nameAscii : info->nameFR;
    if (kind_from_canon(canonAscii, out_kind)) return true;
    /* Fallback EN. */
    if (info->nameEN) return kind_from_canon(info->nameEN, out_kind);
    return false;
}

/*  — entree CONTROL_KINDS pour un OwlControlKind (widgets seulement).
 * Scan de la table : une entree is_widget=true dont owl_control_kind_of_info
 * resout vers `kind`. NULL pour les kinds non mappes (sous-blocs). */
const OwlControlKindInfo* owl_control_kind_info(OwlControlKind kind) {
    for (const OwlControlKindInfo* k = CONTROL_KINDS; k->nameFR; k++) {
        OwlControlKind kk;
        if (owl_control_kind_of_info(k, &kk) && kk == kind) return k;
    }
    return NULL;
}

/* Comparaison case-insensitive accent-insensitive sur deux UTF-8 strings.
 * Strip accents + lowercase de chaque cote puis memcmp. Buffers stack 64. */
static bool ci_ai_eq(const char* a, int alen, const char* b) {
    if (!a || !b) return false;
    int blen = (int)strlen(b);
    /* On strippe les deux cotes pour comparer en ASCII. Hack : on utilise
     * le helper exporté par scanner.c. */
    extern int scanner_strip_accents(const char*, int, char*, int);
    char ba[64], bb[64];
    int na = scanner_strip_accents(a, alen, ba, sizeof(ba));
    int nb = scanner_strip_accents(b, blen, bb, sizeof(bb));
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (tolower((unsigned char)ba[i]) != tolower((unsigned char)bb[i])) return false;
    }
    return true;
}

/*  — entree CONTROL_KINDS par nom canon FR/Ascii/EN, y compris les
 * sous-blocs is_widget=false (ex "Colonne"). Comparaison casse/accent-
 * insensible. NULL si aucun match. */
const OwlControlKindInfo* owl_control_kind_info_by_name(const char* name) {
    if (!name) return NULL;
    int nlen = (int)strlen(name);
    for (const OwlControlKindInfo* k = CONTROL_KINDS; k->nameFR; k++) {
        if (ci_ai_eq(name, nlen, k->nameFR)) return k;
        if (k->nameAscii && ci_ai_eq(name, nlen, k->nameAscii)) return k;
        if (k->nameEN && ci_ai_eq(name, nlen, k->nameEN)) return k;
    }
    return NULL;
}

bool owl_control_kind_from_token(const char* token, int token_len,
                                  OwlControlKind* out_kind,
                                  const OwlControlKindInfo** out_info) {
    if (!token || token_len <= 0) return false;
    for (const OwlControlKindInfo* k = CONTROL_KINDS; k->nameFR; k++) {
        if (!k->is_widget) continue;
        if (ci_ai_eq(token, token_len, k->nameFR) ||
            (k->nameAscii && ci_ai_eq(token, token_len, k->nameAscii)) ||
            (k->nameEN    && ci_ai_eq(token, token_len, k->nameEN))) {
            if (out_info) *out_info = k;
            return owl_control_kind_of_info(k, out_kind);
        }
    }
    return false;
}

/* ============================================================
 * Phase E.X — Dispatch UI control (function pointer override)
 * ============================================================ */

static Value default_ui_get_prop(ObjUiControl* c, const char* prop) {
    (void)c; (void)prop;
    /* Mode CLI (owl.exe sans GUI) : aucun widget Qt — retourne Nul. */
    return NIL_VAL;
}

static OwlUiControlGetPropFn g_ui_get_prop_impl = default_ui_get_prop;

Value owl_ui_control_get_prop(ObjUiControl* c, const char* prop) {
    return g_ui_get_prop_impl(c, prop);
}

void owl_ui_register_get_prop(OwlUiControlGetPropFn fn) {
    g_ui_get_prop_impl = fn ? fn : default_ui_get_prop;
}

static bool default_ui_set_prop(ObjUiControl* c, const char* prop, Value v) {
    (void)c; (void)prop; (void)v;
    return false;
}

static OwlUiControlSetPropFn g_ui_set_prop_impl = default_ui_set_prop;

bool owl_ui_control_set_prop(ObjUiControl* c, const char* prop, Value v) {
    return g_ui_set_prop_impl(c, prop, v);
}

void owl_ui_register_set_prop(OwlUiControlSetPropFn fn) {
    g_ui_set_prop_impl = fn ? fn : default_ui_set_prop;
}

/* V1.5-D : hook on_instance_modified. Default no-op (mode CLI). */
static void default_ui_on_instance_modified(ObjInstance* inst) { (void)inst; }
static OwlUiInstanceModifiedFn g_ui_on_inst_modified_impl = default_ui_on_instance_modified;

void owl_ui_on_instance_modified(ObjInstance* inst) {
    g_ui_on_inst_modified_impl(inst);
}

void owl_ui_register_on_instance_modified(OwlUiInstanceModifiedFn fn) {
    g_ui_on_inst_modified_impl = fn ? fn : default_ui_on_instance_modified;
}

/*  lot 2 — apply scalaire au widget. Default no-op (CLI). */
static void default_apply_instance_scalar(struct ObjUiControlInstance* uici,
                                          const char* prop, Value v) {
    (void)uici; (void)prop; (void)v;
}
static OwlUiInstanceScalarAppliedFn g_ui_apply_scalar_impl =
    default_apply_instance_scalar;

void owl_ui_apply_instance_scalar(struct ObjUiControlInstance* uici,
                                  const char* prop, Value v) {
    g_ui_apply_scalar_impl(uici, prop, v);
}

void owl_ui_register_apply_instance_scalar(OwlUiInstanceScalarAppliedFn fn) {
    g_ui_apply_scalar_impl = fn ? fn : default_apply_instance_scalar;
}

/*  — logique metier Onglet/Volet apres set. Default no-op (CLI). */
static void default_control_logic_after_set(struct ObjUiControlInstance* uici,
                                            const char* prop,
                                            Value old, Value neuf) {
    (void)uici; (void)prop; (void)old; (void)neuf;
}
static OwlUiControlLogicAfterSetFn g_ui_control_logic_after_set_impl =
    default_control_logic_after_set;

void owl_ui_control_logic_after_set(struct ObjUiControlInstance* uici,
                                    const char* prop, Value old, Value neuf) {
    g_ui_control_logic_after_set_impl(uici, prop, old, neuf);
}

void owl_ui_register_control_logic_after_set(OwlUiControlLogicAfterSetFn fn) {
    g_ui_control_logic_after_set_impl =
        fn ? fn : default_control_logic_after_set;
}

/*  — coercion d'une cellule de rep ZR. Default identite (CLI). */
static Value default_zr_rep_coerce(ObjInstance* rep, const char* field, Value v) {
    (void)rep; (void)field;
    return v;
}
static OwlUiZrRepCoerceFn g_ui_zr_rep_coerce_impl = default_zr_rep_coerce;

Value owl_ui_zr_rep_coerce_cell(ObjInstance* rep, const char* field, Value v) {
    return g_ui_zr_rep_coerce_impl(rep, field, v);
}

void owl_ui_register_zr_rep_coerce(OwlUiZrRepCoerceFn fn) {
    g_ui_zr_rep_coerce_impl = fn ? fn : default_zr_rep_coerce;
}

/*  — materialisation lazy d'une cellule ZR. Default NIL (CLI). */
static Value default_zr_rep_cell_subcontrol(ObjInstance* rep,
                                            const char* field) {
    (void)rep; (void)field;
    return NIL_VAL;
}
static OwlUiZrRepCellSubcontrolFn g_ui_zr_rep_cell_subcontrol_impl =
    default_zr_rep_cell_subcontrol;

Value owl_ui_zr_rep_cell_subcontrol(ObjInstance* rep, const char* field) {
    return g_ui_zr_rep_cell_subcontrol_impl(rep, field);
}

void owl_ui_register_zr_rep_cell_subcontrol(OwlUiZrRepCellSubcontrolFn fn) {
    g_ui_zr_rep_cell_subcontrol_impl =
        fn ? fn : default_zr_rep_cell_subcontrol;
}

/*  — mise en forme d'une cellule / ligne de Table poussee au widget.
 * Default no-op (CLI, headless : le store porte deja la valeur). */
static void default_table_fmt_applied(ObjInstance* rep, const char* col,
                                      const char* propCanon, Value v) {
    (void)rep; (void)col; (void)propCanon; (void)v;
}
static OwlUiTableFmtAppliedFn g_ui_table_fmt_applied_impl =
    default_table_fmt_applied;

void owl_ui_table_fmt_applied(ObjInstance* rep, const char* col,
                              const char* propCanon, Value v) {
    g_ui_table_fmt_applied_impl(rep, col, propCanon, v);
}

void owl_ui_register_table_fmt_applied(OwlUiTableFmtAppliedFn fn) {
    g_ui_table_fmt_applied_impl = fn ? fn : default_table_fmt_applied;
}

/*  — masque de colonne poussé au widget. Default no-op (CLI sans GUI,
 * headless : le store OWL-side porte la valeur). */
static void default_table_colmask_applied(void* widget, int col_1based,
                                          const char* wl_mask) {
    (void)widget; (void)col_1based; (void)wl_mask;
}
static OwlUiTableColmaskAppliedFn g_ui_table_colmask_applied_impl =
    default_table_colmask_applied;

void owl_ui_table_colmask_applied(void* widget, int col_1based,
                                  const char* wl_mask) {
    g_ui_table_colmask_applied_impl(widget, col_1based, wl_mask);
}

void owl_ui_register_table_colmask_applied(OwlUiTableColmaskAppliedFn fn) {
    g_ui_table_colmask_applied_impl = fn ? fn : default_table_colmask_applied;
}

/*  lot C — apply scalaire au QMainWindow. Default no-op (CLI). */
static void default_apply_window_scalar(struct ObjWindowInstance* wi,
                                        const char* prop, Value v) {
    (void)wi; (void)prop; (void)v;
}
static OwlUiWindowScalarAppliedFn g_ui_apply_window_scalar_impl =
    default_apply_window_scalar;

void owl_ui_apply_window_scalar(struct ObjWindowInstance* wi,
                                const char* prop, Value v) {
    g_ui_apply_window_scalar_impl(wi, prop, v);
}

void owl_ui_register_apply_window_scalar(OwlUiWindowScalarAppliedFn fn) {
    g_ui_apply_window_scalar_impl = fn ? fn : default_apply_window_scalar;
}

/*  — lecture live de la geometrie fenetre. Default false (CLI). */
static bool default_window_live_get(struct ObjWindowInstance* wi,
                                    const char* prop, Value* out) {
    (void)wi; (void)prop; (void)out;
    return false;
}
static OwlUiWindowLiveGetFn g_ui_window_live_get_impl = default_window_live_get;

bool owl_ui_window_live_get(struct ObjWindowInstance* wi,
                            const char* prop, Value* out) {
    return g_ui_window_live_get_impl(wi, prop, out);
}

void owl_ui_register_window_live_get(OwlUiWindowLiveGetFn fn) {
    g_ui_window_live_get_impl = fn ? fn : default_window_live_get;
}

/*  lot 2 — liaison instance composee -> widget. Default no-op (CLI). */
static void default_register_composite(struct ObjUiControlInstance* uici,
                                       ObjInstance* composite) {
    (void)uici; (void)composite;
}
static OwlUiRegisterCompositeFn g_ui_register_composite_impl =
    default_register_composite;

void owl_ui_register_instance_composite(struct ObjUiControlInstance* uici,
                                        ObjInstance* composite) {
    g_ui_register_composite_impl(uici, composite);
}

void owl_ui_register_register_composite(OwlUiRegisterCompositeFn fn) {
    g_ui_register_composite_impl = fn ? fn : default_register_composite;
}

/* S.4 — hook indexation widget[i]. Default = NIL_VAL (pas d'indexation). */
static Value default_ui_get_subitem(ObjUiControl* c, int idx) {
    (void)c; (void)idx;
    return NIL_VAL;
}
static OwlUiControlGetSubitemFn g_ui_get_subitem_impl = default_ui_get_subitem;

Value owl_ui_control_get_subitem(ObjUiControl* c, int idx) {
    return g_ui_get_subitem_impl(c, idx);
}
void owl_ui_register_get_subitem(OwlUiControlGetSubitemFn fn) {
    g_ui_get_subitem_impl = fn ? fn : default_ui_get_subitem;
}

/*  — hook indexation ZR[i] sur ObjUiControlInstance. Default =
 * NIL_VAL (pas d'indexation cote CLI/lsp). Override par stdlib_ui_init. */
static Value default_ui_instance_get_subitem(struct ObjUiControlInstance* uici,
                                             int idx) {
    (void)uici; (void)idx;
    return NIL_VAL;
}
static OwlUiControlInstanceGetSubitemFn g_ui_instance_get_subitem_impl =
    default_ui_instance_get_subitem;

Value owl_ui_control_instance_get_subitem(struct ObjUiControlInstance* uici,
                                          int idx) {
    return g_ui_instance_get_subitem_impl(uici, idx);
}
void owl_ui_register_instance_get_subitem(OwlUiControlInstanceGetSubitemFn fn) {
    g_ui_instance_get_subitem_impl = fn ? fn : default_ui_instance_get_subitem;
}

/*  — Count items widgets-a-items : 2 function pointers (widget Qt
 * + headless store), defaults = 0 (CLI / lsp). Overrides par stdlib_ui_init
 * sous OWL_HAS_GUI. Pourquoi pas en direct dans ui_decl.c via
 * obj_ui_control_*_headless_get : ui_decl.c est aussi compile dans owl-lsp.exe
 * qui ne linke PAS object.c (pas de runtime / pas de GC). Le pattern function
 * pointer permet a vm.c d'appeler ce helper sans linker stdlib_ui.c / object.c
 * cote lsp.
 *
 *  — generalises kind-agnostic (Liste/Combo + Sel/Int/ZR). */
static int default_widget_items_count(int k, void* w) { (void)k; (void)w; return 0; }
static OwlUiWidgetItemsCountFn g_widget_items_count_impl =
    default_widget_items_count;

int owl_ui_widget_items_count(int kind, void* widget_or_null) {
    if (!widget_or_null) return 0;
    return g_widget_items_count_impl(kind, widget_or_null);
}
void owl_ui_register_widget_items_count(OwlUiWidgetItemsCountFn fn) {
    g_widget_items_count_impl = fn ? fn : default_widget_items_count;
}

static int default_headless_widget_items_count_uic(ObjUiControl* c) { (void)c; return 0; }
static OwlUiHeadlessWidgetItemsCountUicFn g_headless_widget_items_count_uic_impl =
    default_headless_widget_items_count_uic;
int owl_ui_headless_widget_items_count_uic(ObjUiControl* uic) {
    return g_headless_widget_items_count_uic_impl(uic);
}
void owl_ui_register_headless_widget_items_count_uic(OwlUiHeadlessWidgetItemsCountUicFn fn) {
    g_headless_widget_items_count_uic_impl = fn ? fn : default_headless_widget_items_count_uic;
}

static int default_headless_widget_items_count_uici(struct ObjUiControlInstance* c) {
    (void)c; return 0;
}
static OwlUiHeadlessWidgetItemsCountUiciFn g_headless_widget_items_count_uici_impl =
    default_headless_widget_items_count_uici;
int owl_ui_headless_widget_items_count_uici(struct ObjUiControlInstance* uici) {
    return g_headless_widget_items_count_uici_impl(uici);
}
void owl_ui_register_headless_widget_items_count_uici(OwlUiHeadlessWidgetItemsCountUiciFn fn) {
    g_headless_widget_items_count_uici_impl = fn ? fn : default_headless_widget_items_count_uici;
}

