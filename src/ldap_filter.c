// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ldap_filter.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Modele : un AST borne, construit en un passage de gauche a droite,
 * puis encode en BER.
 *
 * Pourquoi un AST (et pas un encodage direct au fil du parsing) :
 *   - le writer BER ecrit A REBOURS (le dernier octet ecrit atterrit en
 *     tete du buffer) ; pour obtenir les enfants d'un and/or dans
 *     l'ordre source, il faut les encoder en ordre inverse, ce qui se
 *     fait trivialement en iterant l'AST a l'envers a l'encodage ;
 *   - un parse->AST complet avant tout encodage garantit qu'une entree
 *     malformee laisse le writer VIDE (rejet atomique), pas a moitie
 *     rempli ;
 *   - les freres (enfants d'un meme and/or) sont une BOUCLE, seule
 *     l'imbrication est recursive et bornee par OWL_LDAP_FILTER_MAX_DEPTH.
 * ================================================================ */

typedef enum {
    LF_AND,        /* [0] SET OF Filter (>=1) */
    LF_OR,         /* [1] SET OF Filter (>=1) */
    LF_NOT,        /* [2] un seul Filter */
    LF_EQUALITY,   /* [3] attr = val */
    LF_GE,         /* [5] attr >= val */
    LF_LE,         /* [6] attr <= val */
    LF_APPROX,     /* [8] attr ~= val */
    LF_PRESENT,    /* [7] attr = *  (presence) */
    LF_SUBSTR      /* [4] attr = a*b*c (sous-chaines) */
} FilterKind;

typedef struct FilterNode {
    FilterKind kind;

    /* Items (equality/ge/le/approx/present/substrings) : description
     * d'attribut en octets bruts (les chars d'echappement sont interdits
     * dans un AttributeDescription, donc copie telle quelle). */
    unsigned char* attr;
    size_t         attr_len;

    /* Valeur d'assertion DECODEE (echappements \XX resolus) :
     *   - equality/ge/le/approx : la valeur complete ;
     *   - substrings : les segments concatenes, les '*' non echappes
     *     n'etant PAS stockes (ce sont des separateurs). `stars[i]` donne
     *     l'offset dans `val` ou se trouve le i-eme '*' : les segments
     *     sont val[0..stars[0]), val[stars[0]..stars[1]), ..., val[last..val_len). */
    unsigned char* val;
    size_t         val_len;
    size_t*        stars;
    size_t         star_count;

    /* and/or : liste d'enfants ; not : un seul enfant en [0]. */
    struct FilterNode** kids;
    size_t              kid_count;
    size_t              kid_cap;
} FilterNode;

typedef struct {
    const char* s;    /* curseur courant */
    const char* end;  /* un apres la fin (sur le NUL terminal) */
    int         depth;
} FilterParser;

/* ================================================================
 * Gestion memoire de l'AST
 * ================================================================ */

static FilterNode* fn_new(FilterKind k) {
    FilterNode* n = (FilterNode*)calloc(1, sizeof(FilterNode));
    if (n != NULL) n->kind = k;
    return n;
}

static void fn_free(FilterNode* n) {
    if (n == NULL) return;
    for (size_t i = 0; i < n->kid_count; i++) fn_free(n->kids[i]);
    free(n->kids);
    free(n->attr);
    free(n->val);
    free(n->stars);
    free(n);
}

static bool fn_add_kid(FilterNode* p, FilterNode* kid) {
    if (p->kid_count == p->kid_cap) {
        size_t nc = p->kid_cap ? p->kid_cap * 2 : 4;
        FilterNode** nk = (FilterNode**)realloc(p->kids, nc * sizeof(FilterNode*));
        if (nk == NULL) return false;
        p->kids = nk;
        p->kid_cap = nc;
    }
    p->kids[p->kid_count++] = kid;
    return true;
}

/* ================================================================
 * Parsing
 * ================================================================ */

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;  /* appele uniquement apres is_hex */
}

static FilterNode* parse_filter(FilterParser* ps);

/* Un item : attributeDescription op valeur. op = '=' | '~=' | '>=' | '<='.
 * ps->s pointe juste apres le '(' (sur le premier char de l'attribut) ;
 * laisse ps->s sur le ')' fermant (consomme par parse_filter). */
static FilterNode* parse_item(FilterParser* ps) {
    const char* attr_start = ps->s;
    while (ps->s < ps->end) {
        char c = *ps->s;
        if (c == '=' || c == '<' || c == '>' || c == '~') break;
        /* Chars structurels ou a echapper : interdits dans un attribut.
         * Les rencontrer ici = operateur manquant / filtre malforme. */
        if (c == '(' || c == ')' || c == '*' || c == '\\') return NULL;
        ps->s++;
    }
    size_t attr_len = (size_t)(ps->s - attr_start);
    if (attr_len == 0) return NULL;      /* attribut vide (ex. "()" ) */
    if (ps->s >= ps->end) return NULL;   /* operateur manquant */

    FilterKind kind;
    char op = *ps->s;
    if (op == '=') {
        ps->s++;
        kind = LF_EQUALITY;  /* affine plus bas en present/substrings */
    } else {
        ps->s++;
        if (ps->s >= ps->end || *ps->s != '=') return NULL;
        ps->s++;
        kind = (op == '~') ? LF_APPROX : (op == '>') ? LF_GE : LF_LE;
    }

    /* Passe 1 : mesure la valeur (longueur decodee + nombre de '*' non
     * echappes) en validant les echappements, jusqu'au ')' non echappe. */
    const char* vp = ps->s;
    size_t dlen = 0, scount = 0;
    const char* q = vp;
    while (q < ps->end) {
        char c = *q;
        if (c == ')') break;
        if (c == '(') return NULL;       /* parenthese non echappee dans la valeur */
        if (c == '*') { scount++; q++; continue; }
        if (c == '\\') {
            if (q + 2 >= ps->end) return NULL;        /* echappement tronque */
            if (!is_hex(q[1]) || !is_hex(q[2])) return NULL;
            dlen++; q += 3; continue;
        }
        dlen++; q++;
    }
    if (q >= ps->end) return NULL;       /* valeur non terminee (pas de ')') */
    const char* vend = q;                /* sur le ')' */

    /* Discrimination present / substrings / egalite. Seul '=' autorise le
     * caractere joker ; pour ~= >= <= un '*' non echappe est malforme. */
    if (kind == LF_EQUALITY) {
        if (scount == 1 && dlen == 0) kind = LF_PRESENT;   /* (attr=*) */
        else if (scount > 0)          kind = LF_SUBSTR;    /* (attr=a*b) */
        /* sinon : egalite (scount == 0) */
    } else if (scount != 0) {
        return NULL;
    }
    /* Un SubstringFilter doit avoir >= 1 composant ; sans aucun octet
     * decode (ex. "(attr=**)"), tous les segments sont vides. Rejet. */
    if (kind == LF_SUBSTR && dlen == 0) return NULL;

    FilterNode* node = fn_new(kind);
    if (node == NULL) return NULL;

    node->attr = (unsigned char*)malloc(attr_len);
    if (node->attr == NULL) { fn_free(node); return NULL; }
    memcpy(node->attr, attr_start, attr_len);
    node->attr_len = attr_len;

    /* Passe 2 : materialise la valeur decodee (present n'en a pas). */
    if (kind != LF_PRESENT) {
        node->val = (unsigned char*)malloc(dlen > 0 ? dlen : 1);
        if (node->val == NULL) { fn_free(node); return NULL; }
        if (scount > 0) {
            node->stars = (size_t*)malloc(scount * sizeof(size_t));
            if (node->stars == NULL) { fn_free(node); return NULL; }
        }
        size_t vi = 0, si = 0;
        for (const char* r = vp; r < vend; ) {
            char c = *r;
            if (c == '*') { node->stars[si++] = vi; r++; }
            else if (c == '\\') {
                node->val[vi++] = (unsigned char)((hex_val(r[1]) << 4) | hex_val(r[2]));
                r += 3;
            } else {
                node->val[vi++] = (unsigned char)c;
                r++;
            }
        }
        node->val_len = vi;
        node->star_count = si;
    }

    ps->s = vend;   /* parse_filter consommera le ')' */
    return node;
}

/* Un filtre complet : '(' ( '&'|'|'|'!' ... | item ) ')'. */
static FilterNode* parse_filter_inner(FilterParser* ps) {
    if (ps->s >= ps->end || *ps->s != '(') return NULL;
    ps->s++;  /* '(' */
    if (ps->s >= ps->end) return NULL;

    FilterNode* node;
    char c = *ps->s;
    if (c == '&' || c == '|') {
        ps->s++;
        node = fn_new(c == '&' ? LF_AND : LF_OR);
        if (node == NULL) return NULL;
        /* SET OF Filter : boucle sur les freres (pas de recursion ici). */
        while (ps->s < ps->end && *ps->s == '(') {
            FilterNode* kid = parse_filter(ps);
            if (kid == NULL) { fn_free(node); return NULL; }
            if (!fn_add_kid(node, kid)) { fn_free(kid); fn_free(node); return NULL; }
        }
        if (node->kid_count == 0) { fn_free(node); return NULL; }  /* >=1 exige */
    } else if (c == '!') {
        ps->s++;
        node = fn_new(LF_NOT);
        if (node == NULL) return NULL;
        FilterNode* kid = parse_filter(ps);
        if (kid == NULL) { fn_free(node); return NULL; }
        if (!fn_add_kid(node, kid)) { fn_free(kid); fn_free(node); return NULL; }
        /* Exactement un operande : tout ce qui n'est pas ')' ci-dessous
         * (ex. un second filtre) fait echouer. */
    } else {
        node = parse_item(ps);
        if (node == NULL) return NULL;
    }

    if (ps->s >= ps->end || *ps->s != ')') { fn_free(node); return NULL; }
    ps->s++;  /* ')' */
    return node;
}

/* Enveloppe bornant la profondeur d'imbrication. Chaque filtre emboite
 * incremente depth ; les freres d'un and/or n'accumulent pas (depth est
 * decremente au retour), seule l'imbrication reelle compte. */
static FilterNode* parse_filter(FilterParser* ps) {
    if (ps->depth >= OWL_LDAP_FILTER_MAX_DEPTH) return NULL;
    ps->depth++;
    FilterNode* r = parse_filter_inner(ps);
    ps->depth--;
    return r;
}

/* ================================================================
 * Encodage BER (a rebours)
 * ================================================================ */

/* OCTET STRING a tag IMPLICIT : ecrit l'OCTET STRING universel (0x04)
 * puis remplace en place l'octet de tag en tete par le tag CONTEXT voulu.
 * C'est exactement la semantique du tagging IMPLICIT (le tag universel
 * est remplace, pas enrobe) et ca evite d'ajouter une primitive a la
 * couche ldap_ber. L'octet de tag est le premier octet ecrit (w->p apres
 * l'ecriture backward) ; patch inoffensif sur memoire deja allouee, fait
 * uniquement apres une ecriture reussie. */
static bool write_implicit_octet_string(OwlBerWriter* w, unsigned char tag,
                                        const unsigned char* data, size_t len) {
    if (!owl_ber_write_octet_string(w, data, len)) return false;
    w->p[0] = tag;
    return true;
}

static bool encode_node(OwlBerWriter* w, const FilterNode* n) {
    switch (n->kind) {
        case LF_AND:
        case LF_OR: {
            size_t mark = owl_ber_writer_len(w);
            /* Ordre inverse : le writer ecrit a rebours, donc encoder le
             * dernier enfant d'abord le place en queue de buffer. */
            for (size_t i = n->kid_count; i-- > 0; ) {
                if (!encode_node(w, n->kids[i])) return false;
            }
            unsigned char tag = owl_ber_tag(OWL_BER_CLASS_CONTEXT, true,
                                            n->kind == LF_AND ? 0 : 1);
            return owl_ber_wrap(w, tag, owl_ber_writer_len(w) - mark);
        }
        case LF_NOT: {
            size_t mark = owl_ber_writer_len(w);
            if (!encode_node(w, n->kids[0])) return false;
            return owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, true, 2),
                                owl_ber_writer_len(w) - mark);
        }
        case LF_EQUALITY:
        case LF_GE:
        case LF_LE:
        case LF_APPROX: {
            unsigned char num = (n->kind == LF_EQUALITY) ? 3
                              : (n->kind == LF_GE)       ? 5
                              : (n->kind == LF_LE)       ? 6
                                                         : 8;
            size_t mark = owl_ber_writer_len(w);
            const unsigned char* v = n->val ? n->val : (const unsigned char*)"";
            /* AVA = { attributeDesc, assertionValue } ; backward -> valeur
             * d'abord (queue), attribut ensuite (tete). */
            if (!owl_ber_write_octet_string(w, v, n->val_len)) return false;
            if (!owl_ber_write_octet_string(w, n->attr, n->attr_len)) return false;
            return owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, true, num),
                                owl_ber_writer_len(w) - mark);
        }
        case LF_PRESENT:
            /* [7] primitif : octets bruts de l'attribut (pas d'OCTET STRING
             * interne). On ecrit un OCTET STRING puis on retagge en 0x87. */
            return write_implicit_octet_string(
                w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 7),
                n->attr, n->attr_len);
        case LF_SUBSTR: {
            size_t mark_outer = owl_ber_writer_len(w);
            size_t sc = n->star_count;

            /* substrings SEQUENCE OF CHOICE { initial [0], any [1], final [2] }.
             * Segments en ordre source : initial, any..., final. Backward ->
             * final d'abord, any en ordre inverse, initial en dernier.
             * Les segments vides ne produisent aucun composant. */
            size_t mark_inner = owl_ber_writer_len(w);

            /* final = dernier segment : val[stars[sc-1] .. val_len) */
            {
                size_t s = (sc == 0) ? 0 : n->stars[sc - 1];
                size_t e = n->val_len;
                if (e > s && !write_implicit_octet_string(
                        w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 2),
                        n->val + s, e - s)) {
                    return false;
                }
            }
            /* any = segments intermediaires seg[sc-1] .. seg[1], a l'envers. */
            for (size_t i = sc; i-- > 1; ) {
                size_t s = n->stars[i - 1];
                size_t e = n->stars[i];
                if (e > s && !write_implicit_octet_string(
                        w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 1),
                        n->val + s, e - s)) {
                    return false;
                }
            }
            /* initial = premier segment : val[0 .. stars[0]) */
            {
                size_t e = (sc == 0) ? n->val_len : n->stars[0];
                if (e > 0 && !write_implicit_octet_string(
                        w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 0),
                        n->val, e)) {
                    return false;
                }
            }
            if (!owl_ber_wrap_sequence(w, owl_ber_writer_len(w) - mark_inner)) {
                return false;
            }
            /* type OCTET STRING (en tete du SubstringFilter). */
            if (!owl_ber_write_octet_string(w, n->attr, n->attr_len)) return false;
            return owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, true, 4),
                                owl_ber_writer_len(w) - mark_outer);
        }
    }
    return false;  /* kind inconnu : defensif, inatteignable */
}

/* ================================================================
 * Entree publique
 * ================================================================ */

bool owl_ldap_filter_encode(OwlBerWriter* w, const char* filter_str) {
    if (w == NULL || filter_str == NULL) return false;

    FilterParser ps;
    ps.s = filter_str;
    ps.end = filter_str + strlen(filter_str);
    ps.depth = 0;

    FilterNode* root = parse_filter(&ps);
    if (root == NULL) return false;

    /* L'entiere chaine doit etre consommee : un reliquat (ex. "(cn=x)zz"
     * ou un ')' surnumeraire) est un filtre malforme. */
    if (ps.s != ps.end) { fn_free(root); return false; }

    bool ok = encode_node(w, root);
    fn_free(root);
    return ok;
}
