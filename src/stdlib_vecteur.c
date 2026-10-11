// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_vecteur.c — Type `Vecteur` / `Vector` (, noyau).
 *
 * Premiere brique d'un socle numerique. Un Vecteur est une collection
 * homogene de reels IEEE-754 stockes dans un tableau C contigu
 * (double[], cf. ObjVector dans object.h). Un element se comporte
 * octet-pour-octet comme un scalaire `reel` : aucune semantique de
 * nombre supplementaire.
 *
 * Ce module pose le NOYAU du type :
 *   - enregistrement du type Vecteur/Vector (bilingue, coercible) ;
 *   - construction par litteral `v est un Vecteur = [1.0, 2.0, 3.0]`
 *     via le coerce `tableau -> Vecteur` (elements coerces en reel) ;
 *   - conversions explicites TableauVersVecteur / VecteurVersTableau.
 *
 * L'indexation 1-based (lecture/ecriture), la propriete ..Taille/..Length
 * et le tag TypeVar vivent cote VM (vm.c / stdlib_reflect.c) — le storage
 * est un objet natif, pas une ObjInstance.
 *
 * Hors-scope de ce module (tickets suivants du chantier Vecteur) :
 * arithmetique, operations mathematiques et reductions.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "vm.h"

#include <math.h>
#include <string.h>

/* ================================================================
 * Helpers de construction
 * ================================================================ */

/* Construit un Vecteur a partir d'un ObjArray dont chaque cellule est un
 * nombre (entier/reel/monetaire/numerique). Chaque element est coerce en
 * reel (double), coherent avec le mecanisme entier->reel existant. Une
 * cellule non-numerique leve une erreur recuperable et rend NIL.
 *
 * `arr` est suppose rooted par le caller (argument sur la pile VM).
 * obj_new_vector peut declencher un GC mais n'alloue rien d'autre ; le
 * remplissage lit `arr` (rooted) et n'alloue plus. */
static Value vecteur_from_array(ObjArray* arr, const char* nativeName) {
    int n = arr->elements.count;
    ObjVector* vec = obj_new_vector(n);
    for (int i = 0; i < n; i++) {
        Value el = arr->elements.values[i];
        if (!IS_NUMBER(el)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                  "arg", "element",
                                  "expected", "reel",
                                  "actual", "type non pris en charge",
                                  NULL);
            return NIL_VAL;
        }
        vec->data[i] = AS_NUMBER(el);
    }
    return OBJ_VAL(vec);
}

/* ================================================================
 * Coerce `tableau -> Vecteur` (declaration-pilote)
 *
 * Emis par le compilateur sur `v est un Vecteur = <expr>` via
 * __InstanceCoerce. Pass-through si <expr> est deja un Vecteur ;
 * conversion depuis un tableau ; erreur sinon (pas de coercion
 * implicite depuis un scalaire ou un autre type).
 * ================================================================ */
static Value parse_to_vecteur(Value v) {
    if (IS_VECTOR(v)) {
        owl_clear_error();
        return v;
    }
    if (IS_ARRAY(v)) {
        owl_clear_error();
        return vecteur_from_array(AS_ARRAY(v), "Vecteur");
    }
    /* Source soft-errored (NIL + erreur posee) : propager, ne pas ecraser par
     * le generique. Cf. . */
    if (IS_NIL(v) && owl_err_is_detected()) {
        return NIL_VAL;
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Vecteur",
                          "arg", "source",
                          "expected", "tableau de reels",
                          "actual", "type non pris en charge",
                          NULL);
    return NIL_VAL;
}

/* ================================================================
 * Conversions explicites
 * ================================================================ */

/* TableauVersVecteur(tableau) -> Vecteur */
static Value fn_tableau_vers_vecteur(int argCount, Value* args) {
    OWL_REQUIRE_ARG("TableauVersVecteur", "tableau", 0, IS_ARRAY(args[0]),
                    "tableau de reels", NIL_VAL);
    (void)argCount;
    return vecteur_from_array(AS_ARRAY(args[0]), "TableauVersVecteur");
}

/* VecteurVersTableau(Vecteur) -> tableau de reels */
static Value fn_vecteur_vers_tableau(int argCount, Value* args) {
    OWL_REQUIRE_ARG("VecteurVersTableau", "vecteur", 0, IS_VECTOR(args[0]),
                    "Vecteur", NIL_VAL);
    (void)argCount;
    ObjVector* vec = AS_VECTOR(args[0]);
    ObjArray* arr = obj_new_array();
    /* Root l'array fraiche pendant les writes : obj_array_push (canon) peut
     * grow -> GC. La source `vec` est args[0], rooted par le dispatch. */
    vm_push(OBJ_VAL(arr));
    for (int i = 0; i < vec->count; i++) {
        obj_array_push(arr, REAL_VAL(vec->data[i]));
    }
    /* fix : marque le tableau comme `tableau de reels` (cellules par defaut
     * = 0.0 sur extension ulterieure), coherent avec la source homogene. */
    arr->elementPrimKind = 3; /* reel */
    vm_pop();
    return OBJ_VAL(arr);
}

/* ================================================================
 * Methodes fluentes arithmetiques
 *
 * `v.Ajoute(w)` / `.Soustrait` / `.Multiplie` / `.Divise` (formes EN
 * Add/Subtract/Multiply/Divide) sont strictement equivalentes aux
 * operateurs + - * / : meme coeur vm_vector_arith, meme contrat de bord
 * (mur division par zero, tailles incompatibles), operandes intacts.
 *
 * Le receveur (args[0]) est toujours un Vecteur — le dispatch sur
 * METHOD_TYPE_VECTOR le garantit ; l'operande args[1] peut etre un Vecteur
 * (element-par-element) ou un scalaire (broadcast). La coexistence avec le
 * `.Ajoute` MUTANT des tableaux (append) ne pose pas de collision : les deux
 * vivent sous des MethodReceiverType distincts (VECTOR vs ARRAY).
 *
 * En cas d'erreur, la promotion en exception rattrapable passe par
 * throwPending (vm_throw_from_erreur), consommee par le dispatch OP_INVOKE.
 * ================================================================ */
extern void vm_throw_from_erreur(const char* msg, int code);

static Value vecteur_fluent(Value* args, char op) {
    Value out;
    VecArithStatus st = vm_vector_arith(args[0], args[1], op, &out);
    if (st == VEC_ARITH_OK) return out;
    /* Meme mapping que l'opcode VM (message dedie par cas : operande tableau
     * refuse, tailles incompatibles). */
    OwlErrorCode code = (OwlErrorCode)owl_vector_arith_error_code(st);
    const char* msg = owl_error_render_lang_is_en()
        ? owl_error_render_en(code, NULL)
        : owl_error_render_fr(code, NULL);
    vm_throw_from_erreur(msg, 0);
    return NIL_VAL;
}

static Value fn_vecteur_ajoute(int argCount, Value* args)    { (void)argCount; return vecteur_fluent(args, '+'); }
static Value fn_vecteur_soustrait(int argCount, Value* args) { (void)argCount; return vecteur_fluent(args, '-'); }
static Value fn_vecteur_multiplie(int argCount, Value* args) { (void)argCount; return vecteur_fluent(args, '*'); }
static Value fn_vecteur_divise(int argCount, Value* args)    { (void)argCount; return vecteur_fluent(args, '/'); }

/* ================================================================
 * Operations mathematiques + reductions
 *
 * Chaque operation existe sous deux surfaces : native prefixee
 * `Vecteur<Op>` (discoverable, sans collision de namespace) et methode
 * fluente nue `.<Op>()`. Une seule impl C sert les deux : le dispatch
 * natif et le dispatch methode (OP_INVOKE) posent tous deux le Vecteur
 * receveur en args[0], donc la meme fonction lit args[0] (et args[1]
 * pour les ops binaires) sans distinction.
 *
 * Contrat de bord (replique du scalaire reel) :
 *   - operande tableau (litteral/variable) refuse (E_RT_VECTOR_ARRAY_OPERAND) ;
 *   - tailles incompatibles -> E_RT_VECTOR_SIZE_MISMATCH ;
 *   - magnitude nulle en denominateur (Normalise / SimilariteCosinus)
 *     -> mur division par zero (E_RT_DIV_BY_ZERO), jamais NaN.
 * Les deux premiers cas reutilisent l'enum VecArithStatus et le helper de
 * mapping owl_vector_arith_error_code de  (aucun code nouveau).
 * ================================================================ */

/* Rend le message d'un code registry (FR/EN selon la langue courante) et
 * arme l'exception rattrapable. Meme pont que vecteur_fluent. */
static void vecteur_lever(OwlErrorCode code) {
    const char* msg = owl_error_render_lang_is_en()
        ? owl_error_render_en(code, NULL)
        : owl_error_render_fr(code, NULL);
    vm_throw_from_erreur(msg, 0);
}

/* Norme euclidienne : sqrt(somme des carres). Un Vecteur vide a une somme
 * de carres nulle -> magnitude 0.0 (sans exception). */
static double vecteur_magnitude_val(const ObjVector* v) {
    double acc = 0.0;
    for (int i = 0; i < v->count; i++) acc += v->data[i] * v->data[i];
    return sqrt(acc);
}

/* Validation stricte d'une op binaire sans allouer : les deux
 * operandes doivent etre des Vecteur natifs de meme taille. Renvoie un
 * VecArithStatus, mappe par le caller via owl_vector_arith_error_code. */
static VecArithStatus vecteur_binaire_valide(Value a, Value b, int* n) {
    if (IS_ARRAY(a) || IS_ARRAY(b)) return VEC_ARITH_ARRAY_OPERAND;
    if (!IS_VECTOR(a) || !IS_VECTOR(b)) return VEC_ARITH_TYPE;
    int ca = AS_VECTOR(a)->count;
    int cb = AS_VECTOR(b)->count;
    if (ca != cb) return VEC_ARITH_SIZE_MISMATCH;
    *n = ca;
    return VEC_ARITH_OK;
}

/* --- Produit scalaire : Sigma vi*wi -> reel --- */
static Value fn_vecteur_produit_scalaire(int argCount, Value* args) {
    (void)argCount;
    int n;
    VecArithStatus st = vecteur_binaire_valide(args[0], args[1], &n);
    if (st != VEC_ARITH_OK) {
        vecteur_lever((OwlErrorCode)owl_vector_arith_error_code(st));
        return NIL_VAL;
    }
    const ObjVector* v = AS_VECTOR(args[0]);
    const ObjVector* w = AS_VECTOR(args[1]);
    double acc = 0.0;
    for (int i = 0; i < n; i++) acc += v->data[i] * w->data[i];
    return REAL_VAL(acc);
}

/* --- Magnitude : sqrt(Sigma vi^2) -> reel --- */
static Value fn_vecteur_magnitude(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("VecteurMagnitude", "vecteur", 0, IS_VECTOR(args[0]),
                    "Vecteur", NIL_VAL);
    return REAL_VAL(vecteur_magnitude_val(AS_VECTOR(args[0])));
}

/* --- Normalise : v / Magnitude(v) -> nouveau Vecteur ; nul -> exception --- */
static Value fn_vecteur_normalise(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("VecteurNormalise", "vecteur", 0, IS_VECTOR(args[0]),
                    "Vecteur", NIL_VAL);
    const ObjVector* v = AS_VECTOR(args[0]);
    double mag = vecteur_magnitude_val(v);
    if (mag == 0.0) {              /* vecteur nul : mur div-zero, jamais NaN */
        vecteur_lever(E_RT_DIV_BY_ZERO);
        return NIL_VAL;
    }
    int n = v->count;
    /* Root la source pendant l'alloc : obj_new_vector peut declencher un GC.
     * args[0] est deja sur la pile VM (rooted par le dispatch), le push est
     * une garantie explicite ; apres l'alloc plus aucune allocation OWL. */
    vm_push(args[0]);
    ObjVector* res = obj_new_vector(n);
    vm_pop();
    for (int i = 0; i < n; i++) res->data[i] = v->data[i] / mag;
    return OBJ_VAL(res);
}

/* --- Distance : Magnitude(v - w) -> reel --- */
static Value fn_vecteur_distance(int argCount, Value* args) {
    (void)argCount;
    /* Reutilise le coeur de soustraction vectorielle : il applique deja le
     * refus d'operande tableau et le controle de tailles. */
    Value diff;
    VecArithStatus st = vm_vector_arith(args[0], args[1], '-', &diff);
    if (st != VEC_ARITH_OK) {
        vecteur_lever((OwlErrorCode)owl_vector_arith_error_code(st));
        return NIL_VAL;
    }
    /* `diff` est un Vecteur frais non-roote, mais aucune allocation OWL ne
     * suit avant sa lecture : le calcul de magnitude est pur. */
    return REAL_VAL(vecteur_magnitude_val(AS_VECTOR(diff)));
}

/* --- Similarite cosinus : dot / (|v| |w|) ; l'un nul -> exception --- */
static Value fn_vecteur_similarite_cosinus(int argCount, Value* args) {
    (void)argCount;
    int n;
    VecArithStatus st = vecteur_binaire_valide(args[0], args[1], &n);
    if (st != VEC_ARITH_OK) {
        vecteur_lever((OwlErrorCode)owl_vector_arith_error_code(st));
        return NIL_VAL;
    }
    const ObjVector* v = AS_VECTOR(args[0]);
    const ObjVector* w = AS_VECTOR(args[1]);
    double dot = 0.0, sv = 0.0, sw = 0.0;
    for (int i = 0; i < n; i++) {
        dot += v->data[i] * w->data[i];
        sv  += v->data[i] * v->data[i];
        sw  += w->data[i] * w->data[i];
    }
    double mv = sqrt(sv), mw = sqrt(sw);
    if (mv == 0.0 || mw == 0.0) {  /* vecteur nul : mur div-zero, jamais NaN */
        vecteur_lever(E_RT_DIV_BY_ZERO);
        return NIL_VAL;
    }
    return REAL_VAL(dot / (mv * mw));
}

/* ================================================================
 * Reductions (fluentes uniquement)
 *
 * Vecteur vide : Somme/Moyenne -> 0.0 (aligne sur le canon tableau
 * Somme([])/Moyenne([]) = 0) ; Min/Max/ArgMax -> exception rattrapable
 * (le resultat est indefini, pas de valeur arbitraire silencieuse).
 * ================================================================ */

/* Vecteur vide sans reduction possible (Min/Max/ArgMax) : code registry
 * dedie, exception rattrapable. */
static void vecteur_lever_reduction_vide(void) {
    vecteur_lever(E_RT_VECTOR_EMPTY_REDUCTION);
}

static Value fn_vecteur_somme(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    double acc = 0.0;
    for (int i = 0; i < v->count; i++) acc += v->data[i];
    return REAL_VAL(acc);
}

static Value fn_vecteur_moyenne(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    if (v->count == 0) return REAL_VAL(0.0);   /* canon tableau : Moyenne([]) = 0 */
    double acc = 0.0;
    for (int i = 0; i < v->count; i++) acc += v->data[i];
    return REAL_VAL(acc / v->count);
}

static Value fn_vecteur_min(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    if (v->count == 0) { vecteur_lever_reduction_vide(); return NIL_VAL; }
    double m = v->data[0];
    for (int i = 1; i < v->count; i++) if (v->data[i] < m) m = v->data[i];
    return REAL_VAL(m);
}

static Value fn_vecteur_max(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    if (v->count == 0) { vecteur_lever_reduction_vide(); return NIL_VAL; }
    double m = v->data[0];
    for (int i = 1; i < v->count; i++) if (v->data[i] > m) m = v->data[i];
    return REAL_VAL(m);
}

static Value fn_vecteur_argmax(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    if (v->count == 0) { vecteur_lever_reduction_vide(); return NIL_VAL; }
    int best = 0;   /* `>` strict : la premiere occurrence gagne en cas d'ex-aequo */
    for (int i = 1; i < v->count; i++) if (v->data[i] > v->data[best]) best = i;
    return INTEGER_VAL(best + 1);   /* indice base 1 */
}

/* Variance de POPULATION : moyenne des carres des ecarts a la moyenne
 * (division par n, pas n-1) — convention usuelle en apprentissage automatique,
 * ou l'on decrit l'echantillon observe plutot que d'estimer une population plus
 * large. Vecteur vide -> 0.0 (aligne sur Moyenne([]) = 0 : rien a disperser,
 * pas d'exception). */
static double vecteur_variance_val(const ObjVector* v) {
    if (v->count == 0) return 0.0;
    double mean = 0.0;
    for (int i = 0; i < v->count; i++) mean += v->data[i];
    mean /= v->count;
    double acc = 0.0;
    for (int i = 0; i < v->count; i++) {
        double d = v->data[i] - mean;
        acc += d * d;
    }
    return acc / v->count;
}

static Value fn_vecteur_variance(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    return REAL_VAL(vecteur_variance_val(v));
}

static Value fn_vecteur_ecart_type(int argCount, Value* args) {
    (void)argCount;
    const ObjVector* v = AS_VECTOR(args[0]);
    return REAL_VAL(sqrt(vecteur_variance_val(v)));   /* racine de la variance */
}

/* ================================================================
 * Fonctions d'activation
 *
 * Applications element-par-element rendant un NOUVEAU Vecteur (immuable) :
 *   - ReLU     : max(0, x) ;
 *   - Sigmoide : 1 / (1 + e^-x), dans l'intervalle ouvert (0, 1) ;
 *   - Tanh     : tangente hyperbolique, dans l'intervalle ouvert (-1, 1).
 * Fonctions pures, sans differentiabilite : juste le calcul forward. Chaque
 * op existe en native prefixee `Vecteur<Op>` et en methode fluente `.<Op>()`.
 * ================================================================ */
typedef double (*VecActivation)(double);

static double act_relu(double x)    { return x > 0.0 ? x : 0.0; }
static double act_sigmoide(double x){ return 1.0 / (1.0 + exp(-x)); }
static double act_tanh(double x)    { return tanh(x); }

static Value vecteur_activation(int argCount, Value* args, const char* name, VecActivation f) {
    OWL_REQUIRE_ARG(name, "vecteur", 0, IS_VECTOR(args[0]), "Vecteur", NIL_VAL);
    const ObjVector* v = AS_VECTOR(args[0]);
    int n = v->count;
    /* Root la source pendant l'alloc : obj_new_vector peut declencher un GC.
     * Apres l'alloc, plus aucune allocation OWL (calcul pur). */
    vm_push(args[0]);
    ObjVector* res = obj_new_vector(n);
    vm_pop();
    for (int i = 0; i < n; i++) res->data[i] = f(v->data[i]);
    return OBJ_VAL(res);
}

static Value fn_vecteur_relu(int argCount, Value* args) {
    return vecteur_activation(argCount, args, "VecteurReLU", act_relu);
}
static Value fn_vecteur_sigmoide(int argCount, Value* args) {
    return vecteur_activation(argCount, args, "VecteurSigmo\xc3\xaf" "de", act_sigmoide);
}
static Value fn_vecteur_tanh(int argCount, Value* args) {
    return vecteur_activation(argCount, args, "VecteurTanh", act_tanh);
}

/* ================================================================
 * Registry
 * ================================================================ */

/* Forme EN dans les aliases (resolution compilo + coerce) EN PLUS de nameEN
 * (formatter / LSP), meme convention que `langue`/`language`. */
static const char* const ALIAS_vecteur[] = { "Vector", NULL };

static const OwlTypeDef VECTEUR_TYPES[] = {
    { .canonical      = "Vecteur",
      .canonicalAscii = "Vecteur",
      .aliases        = ALIAS_vecteur,
      .category       = "Vecteurs",
      .descFR         = "Collection homogene de reels, indexee a partir de 1. "
                        "Construite par un litteral entre crochets "
                        "(`[1.0, 2.0, 3.0]`) ou convertie depuis un tableau de "
                        "reels. La taille est fixee a la construction ; chaque "
                        "element se lit et s'ecrit comme un reel.",
      .descEN         = "Homogeneous collection of reals, indexed from 1. Built "
                        "from a bracketed literal (`[1.0, 2.0, 3.0]`) or "
                        "converted from an array of reals. The size is fixed at "
                        "construction; each element reads and writes as a real.",
      .fields         = NULL,
      /* Coerce `tableau -> Vecteur` enregistre ci-dessous : marque le type
       * comme documente "se laisse coercer depuis un tableau" pour --doc et
       * autorise le litteral `= [...]` a la declaration. */
      .coercible      = true,
      /* Alias scalaire registry sans template : declaration sans init -> Null
       * (le vecteur n'est pas encore construit). Coherent avec langue /
       * zipArchive / MockNative. */
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "Vector",
      .genre          = OWL_GENRE_M },   /* un Vecteur */
};

void stdlib_vecteur_init(void) {
    static const OwlParam p_tableau[] = {
        { "tableau", "tableau de reels",
          "Tableau de reels a convertir en Vecteur.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_tableau_vers_vecteur[] = {
        { 1, p_tableau, "Vecteur",
          "Vecteur de meme taille, meme valeurs aux memes positions." },
        {0}
    };
    static const OwlParam p_vecteur[] = {
        { "vecteur", "Vecteur",
          "Vecteur a convertir en tableau de reels.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_vecteur_vers_tableau[] = {
        { 1, p_vecteur, "tableau de reels",
          "Tableau de reels de meme taille, meme valeurs aux memes positions." },
        {0}
    };

    /* --- Operations mathematiques --- */
    static const OwlParam p_deux_vecteurs[] = {
        { "vecteur1", "Vecteur", "Premier Vecteur.", NULL, NULL },
        { "vecteur2", "Vecteur", "Second Vecteur, de meme taille que le premier.",
          NULL, NULL },
        {0}
    };
    static const OwlParam p_un_vecteur[] = {
        { "vecteur", "Vecteur", "Vecteur a mesurer.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_produit_scalaire[] = {
        { 2, p_deux_vecteurs, "reel",
          "Somme des produits des elements de meme position." },
        {0}
    };
    static const OwlSignature sig_magnitude[] = {
        { 1, p_un_vecteur, "reel",
          "Norme euclidienne : racine carree de la somme des carres." },
        {0}
    };
    static const OwlSignature sig_normalise[] = {
        { 1, p_un_vecteur, "Vecteur",
          "Nouveau Vecteur de meme direction et de norme 1." },
        {0}
    };
    static const OwlSignature sig_distance[] = {
        { 2, p_deux_vecteurs, "reel",
          "Distance euclidienne : norme de la difference des deux Vecteur." },
        {0}
    };
    static const OwlSignature sig_similarite_cosinus[] = {
        { 2, p_deux_vecteurs, "reel",
          "Cosinus de l'angle entre les deux Vecteur, entre -1 et 1." },
        {0}
    };

    /* --- Fonctions d'activation --- */
    static const OwlParam p_vecteur_activation[] = {
        { "vecteur", "Vecteur",
          "Vecteur dont chaque element est transforme.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_activation[] = {
        { 1, p_vecteur_activation, "Vecteur",
          "Nouveau Vecteur de meme taille, la fonction etant appliquee a "
          "chaque element." },
        {0}
    };

    static const OwlNativeDef DEFS_VECTEUR[] = {
        { .canonical   = "TableauVersVecteur",
          .nameEN      = "ArrayToVector",
          .category    = "Vecteurs",
          .descFR      = "Convertit un tableau de reels en Vecteur de meme "
                         "taille, sans coercion implicite entre les deux types.",
          .descEN      = "Converts an array of reals into a Vector of the same "
                         "size, without implicit coercion between the two types.",
          .signatures  = sig_tableau_vers_vecteur,
          .impl        = fn_tableau_vers_vecteur,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurVersTableau",
          .nameEN      = "VectorToArray",
          .category    = "Vecteurs",
          .descFR      = "Convertit un Vecteur en tableau de reels de meme "
                         "taille, sans coercion implicite entre les deux types.",
          .descEN      = "Converts a Vector into an array of reals of the same "
                         "size, without implicit coercion between the two types.",
          .signatures  = sig_vecteur_vers_tableau,
          .impl        = fn_vecteur_vers_tableau,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurProduitScalaire",
          .nameEN      = "VectorDotProduct",
          .category    = "Vecteurs",
          .descFR      = "Produit scalaire de deux Vecteur de meme taille : la "
                         "somme des produits de leurs elements de meme position. "
                         "Un operande tableau est refuse ; conversion explicite "
                         "requise.",
          .descEN      = "Dot product of two Vectors of the same size: the sum of "
                         "the products of their elements at matching positions. An "
                         "array operand is rejected; explicit conversion required.",
          .signatures  = sig_produit_scalaire,
          .impl        = fn_vecteur_produit_scalaire,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurMagnitude",
          .nameEN      = "VectorMagnitude",
          .category    = "Vecteurs",
          .descFR      = "Norme euclidienne d'un Vecteur : la racine carree de la "
                         "somme des carres de ses elements. Un Vecteur vide a une "
                         "magnitude de 0.",
          .descEN      = "Euclidean norm of a Vector: the square root of the sum "
                         "of the squares of its elements. An empty Vector has a "
                         "magnitude of 0.",
          .signatures  = sig_magnitude,
          .impl        = fn_vecteur_magnitude,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurNormalise",
          .nameEN      = "VectorNormalize",
          .category    = "Vecteurs",
          .descFR      = "Renvoie un nouveau Vecteur de meme direction et de norme "
                         "1, obtenu en divisant chaque element par la magnitude. "
                         "Un Vecteur nul (magnitude 0) leve une exception.",
          .descEN      = "Returns a new Vector with the same direction and a norm "
                         "of 1, obtained by dividing each element by the "
                         "magnitude. A null Vector (magnitude 0) raises an "
                         "exception.",
          .signatures  = sig_normalise,
          .impl        = fn_vecteur_normalise,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurDistance",
          .nameEN      = "VectorDistance",
          .category    = "Vecteurs",
          .descFR      = "Distance euclidienne entre deux Vecteur de meme taille : "
                         "la magnitude de leur difference. Un operande tableau est "
                         "refuse ; conversion explicite requise.",
          .descEN      = "Euclidean distance between two Vectors of the same size: "
                         "the magnitude of their difference. An array operand is "
                         "rejected; explicit conversion required.",
          .signatures  = sig_distance,
          .impl        = fn_vecteur_distance,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical      = "VecteurSimilarit\xc3\xa9" "Cosinus",
          .canonicalAscii = "VecteurSimilariteCosinus",
          .nameEN         = "VectorCosineSimilarity",
          .category       = "Vecteurs",
          .descFR         = "Similarite cosinus de deux Vecteur de meme taille : le "
                            "cosinus de l'angle qui les separe, entre -1 et 1. Un "
                            "Vecteur nul leve une exception ; un operande tableau "
                            "est refuse.",
          .descEN         = "Cosine similarity of two Vectors of the same size: the "
                            "cosine of the angle between them, from -1 to 1. A null "
                            "Vector raises an exception; an array operand is "
                            "rejected.",
          .signatures     = sig_similarite_cosinus,
          .impl           = fn_vecteur_similarite_cosinus,
          .flags          = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurReLU",
          .nameEN      = "VectorReLU",
          .category    = "Vecteurs",
          .descFR      = "Applique la fonction ReLU (max(0, x)) a chaque element "
                         "d'un Vecteur : les valeurs negatives deviennent 0, les "
                         "positives et le zero restent inchangees. Rend un nouveau "
                         "Vecteur de meme taille.",
          .descEN      = "Applies the ReLU function (max(0, x)) to each element of "
                         "a Vector: negative values become 0, positive values and "
                         "zero are left unchanged. Returns a new Vector of the same "
                         "size.",
          .signatures  = sig_activation,
          .impl        = fn_vecteur_relu,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical      = "VecteurSigmo\xc3\xaf" "de",
          .canonicalAscii = "VecteurSigmoide",
          .nameEN         = "VectorSigmoid",
          .category       = "Vecteurs",
          .descFR         = "Applique la fonction sigmoide (1 / (1 + e^-x)) a chaque "
                            "element d'un Vecteur, ramenant toute valeur reelle dans "
                            "l'intervalle ouvert (0, 1). Rend un nouveau Vecteur de "
                            "meme taille.",
          .descEN         = "Applies the sigmoid function (1 / (1 + e^-x)) to each "
                            "element of a Vector, mapping any real value into the "
                            "open interval (0, 1). Returns a new Vector of the same "
                            "size.",
          .signatures     = sig_activation,
          .impl           = fn_vecteur_sigmoide,
          .flags          = OWL_NAT_PURE,
        },
        { .canonical   = "VecteurTanh",
          .nameEN      = "VectorTanh",
          .category    = "Vecteurs",
          .descFR      = "Applique la tangente hyperbolique a chaque element d'un "
                         "Vecteur, ramenant toute valeur reelle dans l'intervalle "
                         "ouvert (-1, 1). Rend un nouveau Vecteur de meme taille.",
          .descEN      = "Applies the hyperbolic tangent to each element of a "
                         "Vector, mapping any real value into the open interval "
                         "(-1, 1). Returns a new Vector of the same size.",
          .signatures  = sig_activation,
          .impl        = fn_vecteur_tanh,
          .flags       = OWL_NAT_PURE,
        },
    };

    owl_registry_register_types(VECTEUR_TYPES,
        (int)(sizeof(VECTEUR_TYPES) / sizeof(VECTEUR_TYPES[0])));
    owl_registry_register_module(DEFS_VECTEUR,
        (int)(sizeof(DEFS_VECTEUR) / sizeof(DEFS_VECTEUR[0])));
    owl_register_instance_coerce("Vecteur", parse_to_vecteur);

    /* Methodes fluentes arithmetiques (arity = receveur + 1 operande). */
    owl_register_method(METHOD_TYPE_VECTOR, "Ajoute",    fn_vecteur_ajoute,    2);
    owl_register_method(METHOD_TYPE_VECTOR, "Add",       fn_vecteur_ajoute,    2);
    owl_register_method(METHOD_TYPE_VECTOR, "Soustrait", fn_vecteur_soustrait, 2);
    owl_register_method(METHOD_TYPE_VECTOR, "Subtract",  fn_vecteur_soustrait, 2);
    owl_register_method(METHOD_TYPE_VECTOR, "Multiplie", fn_vecteur_multiplie, 2);
    owl_register_method(METHOD_TYPE_VECTOR, "Multiply",  fn_vecteur_multiplie, 2);
    owl_register_method(METHOD_TYPE_VECTOR, "Divise",    fn_vecteur_divise,    2);
    owl_register_method(METHOD_TYPE_VECTOR, "Divide",    fn_vecteur_divise,    2);

    /* Operations mathematiques fluentes. Le fluent drop le prefixe
     * `Vecteur` de la native. Binaires = receveur + 1 operande (arite 2) ;
     * Magnitude/Normalise = receveur seul (arite 1). */
    owl_register_method(METHOD_TYPE_VECTOR, "ProduitScalaire",  fn_vecteur_produit_scalaire,    2);
    owl_register_method(METHOD_TYPE_VECTOR, "DotProduct",       fn_vecteur_produit_scalaire,    2);
    owl_register_method(METHOD_TYPE_VECTOR, "Magnitude",        fn_vecteur_magnitude,           1);
    owl_register_method(METHOD_TYPE_VECTOR, "Normalise",        fn_vecteur_normalise,           1);
    owl_register_method(METHOD_TYPE_VECTOR, "Normalize",        fn_vecteur_normalise,           1);
    owl_register_method(METHOD_TYPE_VECTOR, "Distance",         fn_vecteur_distance,            2);
    owl_register_method(METHOD_TYPE_VECTOR, "Similarit\xc3\xa9" "Cosinus", fn_vecteur_similarite_cosinus, 2);
    owl_register_method(METHOD_TYPE_VECTOR, "CosineSimilarity", fn_vecteur_similarite_cosinus,  2);

    /* Reductions fluentes — receveur seul (arite 1). Somme/Moyenne
     * bilingues (Sum/Average) ; Min/Max/ArgMax identiques FR/EN. */
    owl_register_method(METHOD_TYPE_VECTOR, "Somme",   fn_vecteur_somme,   1);
    owl_register_method(METHOD_TYPE_VECTOR, "Sum",     fn_vecteur_somme,   1);
    owl_register_method(METHOD_TYPE_VECTOR, "Moyenne", fn_vecteur_moyenne, 1);
    owl_register_method(METHOD_TYPE_VECTOR, "Average", fn_vecteur_moyenne, 1);
    owl_register_method(METHOD_TYPE_VECTOR, "Min",     fn_vecteur_min,     1);
    owl_register_method(METHOD_TYPE_VECTOR, "Max",     fn_vecteur_max,     1);
    owl_register_method(METHOD_TYPE_VECTOR, "ArgMax",  fn_vecteur_argmax,  1);

    /* Statistiques descriptives fluentes — receveur seul (arite 1).
     * Variance de population (÷n). Variance identique FR/EN ; ÉcartType a une
     * forme EN distincte (StandardDeviation). Fluentes uniquement, comme les
     * autres reductions (Somme/Moyenne...). */
    owl_register_method(METHOD_TYPE_VECTOR, "Variance",          fn_vecteur_variance,   1);
    owl_register_method(METHOD_TYPE_VECTOR, "\xc3\x89" "cartType", fn_vecteur_ecart_type, 1);
    owl_register_method(METHOD_TYPE_VECTOR, "StandardDeviation", fn_vecteur_ecart_type, 1);

    /* Fonctions d'activation fluentes — receveur seul (arite 1).
     * ReLU/Tanh identiques FR/EN ; Sigmoide a une forme EN distincte (Sigmoid). */
    owl_register_method(METHOD_TYPE_VECTOR, "ReLU",             fn_vecteur_relu,     1);
    owl_register_method(METHOD_TYPE_VECTOR, "Sigmo\xc3\xaf" "de", fn_vecteur_sigmoide, 1);
    owl_register_method(METHOD_TYPE_VECTOR, "Sigmoid",          fn_vecteur_sigmoide, 1);
    owl_register_method(METHOD_TYPE_VECTOR, "Tanh",             fn_vecteur_tanh,     1);
}
