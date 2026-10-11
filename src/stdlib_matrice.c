// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_matrice.c — Type `Matrice` / `Matrix` (, noyau).
 *
 * Pendant 2D du Vecteur (cf. stdlib_vecteur.c, ). Une Matrice est
 * une grille rectangulaire de reels IEEE-754 stockes dans un tableau C
 * contigu row-major (ObjMatrix, cf. object.h). Un element se comporte
 * octet-pour-octet comme un scalaire `reel` : aucune semantique de nombre
 * supplementaire. Invariant : toute Matrice est >= 1 ligne x 1 colonne.
 *
 * Ce module pose le NOYAU du type :
 *   - enregistrement du type Matrice/Matrix (bilingue, coercible) ;
 *   - construction par litteral imbrique `m est une Matrice =
 *     [[1.0, 2.0], [3.0, 4.0]]` via le coerce `tableau imbrique -> Matrice` ;
 *   - conversions explicites TableauVersMatrice / MatriceVersTableau ;
 *   - extraction ligne/colonne vers Vecteur (MatriceLigne / MatriceColonne).
 *
 * L'indexation 1-based `m[i][j]` (lecture/ecriture), les proprietes
 * ..Lignes/..Colonnes et le tag TypeVar vivent cote VM (vm.c /
 * stdlib_reflect.c) — le storage est un objet natif, pas une ObjInstance.
 *
 * Hors-scope de ce module (tickets suivants du chantier Matrice) :
 * arithmetique, algebre lineaire (produit matriciel, transposee...),
 * produit matrice-vecteur.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "owl_parallel.h"
#include "vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Toute la contraction flottante mul+add -> FMA est INTERDITE dans ce module :
 * un FMA fond les deux operations en un seul arrondi et divergerait du chemin
 * scalaire de la VM (deux arrondis), cassant la parite bit-a-bit du produit
 * matriciel avec l'oracle OWL. On la coupe des la source (double ceinture avec
 * -ffp-contract=off pose sur ce fichier cote CMake). */
#pragma STDC FP_CONTRACT OFF

/* Intrinseques SIMD gardees par macro de cible (meme patron que llhttp :
 * #ifdef sur les macros predeclarees par le compilateur, jamais de -march
 * force ici). x86_64 -> SSE2 baseline (128 bits = 2 doubles) + variante AVX2
 * (256 bits = 4 doubles) selectionnee au RUNTIME via cpuid ; -mcpu=x86_64_v2
 * garantit SSE4.2, et SSE2 est de toute facon la baseline de tout x86_64.
 * immintrin.h expose SSE2 ET AVX2. aarch64 -> NEON AdvSIMD (float64x2_t).
 * Toute autre cible retombe sur le chemin scalaire garanti. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
  #include <immintrin.h>
  #include <cpuid.h>
  #define OWL_MATMUL_SSE2 1
#elif defined(__aarch64__)
  #include <arm_neon.h>
  #define OWL_MATMUL_NEON 1
#endif

/* Pont d'exception rattrapable (arme throwPending, consomme par le dispatch
 * OP_CALL englobant). Meme mecanique que le Vecteur. */
extern void vm_throw_from_erreur(const char* msg, int code);

/* Leve une exception rattrapable avec un message bilingue direct. Choisi
 * plutot qu'un code registry : aucun code existant ne decrit « litteral de
 * Matrice invalide » et le chantier n'ouvre pas owl_errors.def. */
static void matrice_lever(const char* fr, const char* en) {
    const char* msg = owl_error_render_lang_is_en() ? en : fr;
    vm_throw_from_erreur(msg, 0);
}

/* Variante par code registry (FR/EN selon la langue courante), meme pont
 * rattrapable. Utilisee par l'algebre lineaire pour reutiliser les codes de
 * l'arithmetique Matrice (dimensions incompatibles, operande tableau). */
static void matrice_lever_code(OwlErrorCode code) {
    const char* msg = owl_error_render_lang_is_en()
        ? owl_error_render_en(code, NULL)
        : owl_error_render_fr(code, NULL);
    vm_throw_from_erreur(msg, 0);
}

/* ================================================================
 * Helpers de construction
 * ================================================================ */

/* Construit une Matrice a partir d'un ObjArray imbrique (une ligne = un
 * ObjArray de nombres). Valide au runtime : au moins 1x1, rectangulaire,
 * tous les elements numeriques. Chaque element est coerce en reel (double),
 * coherent avec le mecanisme entier->reel. Une violation leve une exception
 * rattrapable et rend NIL.
 *
 * `outer` est suppose rooted par le caller (argument sur la pile VM).
 * La validation ne fait aucune allocation ; obj_new_matrix alloue ensuite
 * mais le remplissage relit `outer` (rooted) sans plus allouer -> GC-sur. */
static Value matrice_from_nested_array(ObjArray* outer, const char* nativeName) {
    (void)nativeName;
    int rows = outer->elements.count;
    if (rows < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }

    /* La premiere ligne fixe la largeur attendue. */
    Value firstRow = outer->elements.values[0];
    if (!IS_ARRAY(firstRow)) {
        matrice_lever(
            "Litteral de Matrice invalide : chaque ligne doit etre un tableau de reels.",
            "Invalid Matrix literal: each row must be an array of reals.");
        return NIL_VAL;
    }
    int cols = AS_ARRAY(firstRow)->elements.count;
    if (cols < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }

    /* Passe de validation : rectangulaire + numerique, sans allouer. */
    for (int r = 0; r < rows; r++) {
        Value rowVal = outer->elements.values[r];
        if (!IS_ARRAY(rowVal)) {
            matrice_lever(
                "Litteral de Matrice invalide : chaque ligne doit etre un tableau de reels.",
                "Invalid Matrix literal: each row must be an array of reals.");
            return NIL_VAL;
        }
        ObjArray* row = AS_ARRAY(rowVal);
        if (row->elements.count != cols) {
            matrice_lever(
                "Litteral de Matrice non rectangulaire : toutes les lignes "
                "doivent avoir la meme longueur.",
                "Non-rectangular Matrix literal: all rows must have the same length.");
            return NIL_VAL;
        }
        for (int c = 0; c < cols; c++) {
            if (!IS_NUMBER(row->elements.values[c])) {
                matrice_lever(
                    "Element de Matrice non numerique : chaque cellule doit etre un reel.",
                    "Non-numeric Matrix element: each cell must be a real.");
                return NIL_VAL;
            }
        }
    }

    /* Remplissage row-major. `outer` reste roote pendant l'alloc. */
    ObjMatrix* mat = obj_new_matrix(rows, cols);
    for (int r = 0; r < rows; r++) {
        ObjArray* row = AS_ARRAY(outer->elements.values[r]);
        for (int c = 0; c < cols; c++) {
            mat->data[r * cols + c] = AS_NUMBER(row->elements.values[c]);
        }
    }
    return OBJ_VAL(mat);
}

/* ================================================================
 * Coerce `tableau imbrique -> Matrice` (declaration-pilote)
 *
 * Emis par le compilateur sur `m est une Matrice = <expr>` via
 * __InstanceCoerce. Pass-through si <expr> est deja une Matrice ;
 * conversion depuis un tableau imbrique ; exception sinon.
 * ================================================================ */
static Value parse_to_matrice(Value v) {
    if (IS_MATRIX(v)) {
        owl_clear_error();
        return v;
    }
    if (IS_ARRAY(v)) {
        owl_clear_error();
        return matrice_from_nested_array(AS_ARRAY(v), "Matrice");
    }
    /* Source soft-errored (NIL + erreur posee) : propager le soft-error au lieu
     * de lever une exception generique qui le masque. Cf. . */
    if (IS_NIL(v) && owl_err_is_detected()) {
        return NIL_VAL;
    }
    matrice_lever(
        "Une Matrice se construit depuis un tableau imbrique de reels.",
        "A Matrix is built from a nested array of reals.");
    return NIL_VAL;
}

/* ================================================================
 * Conversions explicites
 * ================================================================ */

/* TableauVersMatrice(tableau imbrique) -> Matrice */
static Value fn_tableau_vers_matrice(int argCount, Value* args) {
    OWL_REQUIRE_ARG("TableauVersMatrice", "tableau", 0, IS_ARRAY(args[0]),
                    "tableau imbrique de reels", NIL_VAL);
    (void)argCount;
    return matrice_from_nested_array(AS_ARRAY(args[0]), "TableauVersMatrice");
}

/* MatriceVersTableau(Matrice) -> tableau imbrique de reels */
static Value fn_matrice_vers_tableau(int argCount, Value* args) {
    OWL_REQUIRE_ARG("MatriceVersTableau", "matrice", 0, IS_MATRIX(args[0]),
                    "Matrice", NIL_VAL);
    (void)argCount;
    ObjMatrix* mat = AS_MATRIX(args[0]);
    /* Root le tableau externe en construction pendant les writes (obj_new_array
     * + obj_array_push peuvent grow -> GC). La source `mat` est args[0], rootee
     * par le dispatch. Chaque ligne est aussi rootee tant qu'on la remplit. */
    ObjArray* outer = obj_new_array();
    vm_push(OBJ_VAL(outer));
    for (int r = 0; r < mat->rows; r++) {
        ObjArray* row = obj_new_array();
        vm_push(OBJ_VAL(row));
        for (int c = 0; c < mat->cols; c++) {
            obj_array_push(row, REAL_VAL(mat->data[r * mat->cols + c]));
        }
        row->elementPrimKind = 3; /* reel */
        obj_array_push(outer, OBJ_VAL(row));
        vm_pop(); /* row */
    }
    vm_pop(); /* outer */
    return OBJ_VAL(outer);
}

/* MatriceLigne(Matrice, i) -> Vecteur (la ligne i, taille = nb colonnes) */
static Value fn_matrice_ligne(int argCount, Value* args) {
    OWL_REQUIRE_ARG("MatriceLigne", "matrice", 0, IS_MATRIX(args[0]),
                    "Matrice", NIL_VAL);
    OWL_REQUIRE_ARG("MatriceLigne", "ligne", 1, IS_NUMBER(args[1]),
                    "entier", NIL_VAL);
    (void)argCount;
    ObjMatrix* mat = AS_MATRIX(args[0]);
    int64_t i = (int64_t)AS_NUMBER(args[1]);
    if (i < 1 || i > mat->rows) {
        matrice_lever(
            "Indice de ligne hors limites pour la Matrice.",
            "Row index out of bounds for the Matrix.");
        return NIL_VAL;
    }
    /* `mat` reste roote via args[0] pendant obj_new_vector (peut GC). */
    ObjVector* vec = obj_new_vector(mat->cols);
    for (int c = 0; c < mat->cols; c++) {
        vec->data[c] = mat->data[(i - 1) * mat->cols + c];
    }
    return OBJ_VAL(vec);
}

/* MatriceColonne(Matrice, j) -> Vecteur (la colonne j, taille = nb lignes) */
static Value fn_matrice_colonne(int argCount, Value* args) {
    OWL_REQUIRE_ARG("MatriceColonne", "matrice", 0, IS_MATRIX(args[0]),
                    "Matrice", NIL_VAL);
    OWL_REQUIRE_ARG("MatriceColonne", "colonne", 1, IS_NUMBER(args[1]),
                    "entier", NIL_VAL);
    (void)argCount;
    ObjMatrix* mat = AS_MATRIX(args[0]);
    int64_t j = (int64_t)AS_NUMBER(args[1]);
    if (j < 1 || j > mat->cols) {
        matrice_lever(
            "Indice de colonne hors limites pour la Matrice.",
            "Column index out of bounds for the Matrix.");
        return NIL_VAL;
    }
    ObjVector* vec = obj_new_vector(mat->rows);
    for (int r = 0; r < mat->rows; r++) {
        vec->data[r] = mat->data[r * mat->cols + (j - 1)];
    }
    return OBJ_VAL(vec);
}

/* MatriceSousLignes(Matrice, indices) -> Matrice (les lignes aux indices
 * donnes, 1-based, dans l'ordre exact du tableau, repetition autorisee).
 * Miroir plurial de MatriceLigne : reste dans le monde Matrice. Coeur du
 * split train/test materialise depuis une permutation d'indices. */
static Value fn_matrice_sous_lignes(int argCount, Value* args) {
    OWL_REQUIRE_ARG("MatriceSousLignes", "matrice", 0, IS_MATRIX(args[0]),
                    "Matrice", NIL_VAL);
    OWL_REQUIRE_ARG("MatriceSousLignes", "indices", 1, IS_ARRAY(args[1]),
                    "tableau d'entiers", NIL_VAL);
    (void)argCount;
    ObjMatrix* mat = AS_MATRIX(args[0]);
    ObjArray* indices = AS_ARRAY(args[1]);
    int count = indices->elements.count;
    if (count < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }

    /* Passe de validation : chaque indice entier et dans les bornes, sans
     * allouer -> une exception rattrapable part avec la pile VM propre. */
    for (int k = 0; k < count; k++) {
        Value iv = indices->elements.values[k];
        if (!IS_NUMBER(iv)) {
            matrice_lever(
                "Indice de ligne non entier : chaque indice doit etre un entier.",
                "Non-integer row index: each index must be an integer.");
            return NIL_VAL;
        }
        int64_t i = (int64_t)AS_NUMBER(iv);
        if (i < 1 || i > mat->rows) {
            matrice_lever(
                "Indice de ligne hors limites pour la Matrice.",
                "Row index out of bounds for the Matrix.");
            return NIL_VAL;
        }
    }

    /* Remplissage row-major. `mat` (args[0]) et `indices` (args[1]) restent
     * rootes via la pile de dispatch pendant l'alloc ; aucune autre allocation
     * OWL ensuite -> GC-sur. */
    ObjMatrix* res = obj_new_matrix(count, mat->cols);
    for (int k = 0; k < count; k++) {
        int64_t i = (int64_t)AS_NUMBER(indices->elements.values[k]);
        for (int c = 0; c < mat->cols; c++) {
            res->data[k * mat->cols + c] = mat->data[(i - 1) * mat->cols + c];
        }
    }
    return OBJ_VAL(res);
}

/* TableauVersMatriceColonne(tableau plat de reels) -> Matrice colonne N×1.
 * Pont explicite tableau->Matrice (aucune coercion implicite), miroir colonne
 * de TableauVersMatrice. Materialise une cible d'entrainement. */
static Value fn_tableau_vers_matrice_colonne(int argCount, Value* args) {
    OWL_REQUIRE_ARG("TableauVersMatriceColonne", "valeurs", 0, IS_ARRAY(args[0]),
                    "tableau de reels", NIL_VAL);
    (void)argCount;
    ObjArray* valeurs = AS_ARRAY(args[0]);
    int n = valeurs->elements.count;
    if (n < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }

    /* Passe de validation : tout numerique, sans allouer. */
    for (int i = 0; i < n; i++) {
        if (!IS_NUMBER(valeurs->elements.values[i])) {
            matrice_lever(
                "Element de Matrice non numerique : chaque cellule doit etre un reel.",
                "Non-numeric Matrix element: each cell must be a real.");
            return NIL_VAL;
        }
    }

    /* `valeurs` (args[0]) reste roote pendant l'alloc ; remplissage sans autre
     * allocation OWL -> GC-sur. */
    ObjMatrix* res = obj_new_matrix(n, 1);
    for (int i = 0; i < n; i++) {
        res->data[i] = AS_NUMBER(valeurs->elements.values[i]);
    }
    return OBJ_VAL(res);
}

/* ================================================================
 * Methodes fluentes arithmetiques
 *
 * `m.Ajoute(n)` / `.Soustrait` / `.Multiplie` / `.Divise` (formes EN
 * Add/Subtract/Multiply/Divide) sont strictement equivalentes aux operateurs
 * + - * / : meme coeur vm_matrix_arith, meme contrat de bord (mur division
 * par zero, dimensions incompatibles, refus d'operande tableau), operandes
 * intacts. `.Multiplie` est le produit element-par-element (Hadamard), PAS le
 * produit matriciel. Le receveur (args[0]) est toujours une Matrice (dispatch
 * METHOD_TYPE_MATRIX) ; l'operande args[1] est une Matrice (element-par-
 * element) ou un scalaire (broadcast). En cas d'erreur, la promotion en
 * exception rattrapable passe par throwPending, consommee par OP_INVOKE. */
static Value matrice_fluent(Value* args, char op) {
    Value out;
    MatArithStatus st = vm_matrix_arith(args[0], args[1], op, &out);
    if (st == MAT_ARITH_OK) return out;
    OwlErrorCode code = (OwlErrorCode)owl_matrix_arith_error_code(st);
    const char* msg = owl_error_render_lang_is_en()
        ? owl_error_render_en(code, NULL)
        : owl_error_render_fr(code, NULL);
    vm_throw_from_erreur(msg, 0);
    return NIL_VAL;
}

static Value fn_matrice_ajoute(int argCount, Value* args)    { (void)argCount; return matrice_fluent(args, '+'); }
static Value fn_matrice_soustrait(int argCount, Value* args) { (void)argCount; return matrice_fluent(args, '-'); }
static Value fn_matrice_multiplie(int argCount, Value* args) { (void)argCount; return matrice_fluent(args, '*'); }
static Value fn_matrice_divise(int argCount, Value* args)    { (void)argCount; return matrice_fluent(args, '/'); }

/* ================================================================
 * Algebre lineaire
 *
 * `ProduitMatriciel` / `MatrixProduct` (fluent `.ProduitMatriciel` /
 * `.MatrixProduct`) : produit A·B, surchargee Matrice×Matrice -> Matrice ET
 * Matrice×Vecteur -> Vecteur (le vecteur est traite comme une colonne). Une
 * seule impl C sert les deux surfaces (native prefixee et methode fluente) et
 * les deux surcharges : le dispatch pose le premier operande en args[0], le
 * second en args[1], et l'impl discrimine sur le type de args[1] au runtime.
 * Contrainte colonnes(A) == lignes(B) (resp. colonnes(m) == taille(v)), sinon
 * exception rattrapable (dimensions incompatibles). Operande tableau refuse,
 * comme l'arithmetique. Resultat alloue neuf ; operandes intacts.
 *
 * `MatriceTranspose` / `MatrixTranspose` (fluent `.Transpose`) : transposee,
 * resultat colonnes×lignes. `MatriceIdentité` / `MatrixIdentity` : identite
 * n×n (1 sur la diagonale, 0 ailleurs), n <= 0 refuse.
 * ================================================================ */

/* ----------------------------------------------------------------
 * Kernel du produit matriciel — refonte perf, resultat
 * bit-a-bit identique a l'ancienne triple boucle naive.
 *
 * Agencement `ikj` : pour chaque (i, k) fixes, on diffuse A[i][k] et on
 * accumule sur toute la ligne j de C la contribution A[i][k]*B[k][j].
 * B est alors lu en LIGNE (contigu) et C ecrit en ligne (contigu) — le
 * pire pattern cache de l'ancien kernel (B lu en stride colonne) disparait.
 *
 * Ordre d'accumulation preserve : pour un C[i][j] donne, les termes sont
 * ajoutes dans l'ordre k = 0, 1, ..., K-1 (boucle k croissante, tuiles
 * parcourues en ordre croissant). C'est exactement l'ordre de l'ancienne
 * boucle ijk -> resultat flottant IDENTIQUE bit-a-bit (l'invariant
 * associativite et les egalites exactes des tests tiennent).
 *
 * Cache blocking : tuiles (BI, BK, BJ) pour garder le bloc de B et la bande
 * de C au chaud en cache pendant qu'on balaie k. La SIMD vectorise la boucle
 * interne sur j (2 doubles) ; les restes non-multiples de 2 retombent sur le
 * scalaire. `restrict` sur les trois pointeurs : A, B, C ne se recouvrent
 * jamais (C est alloue neuf). Chargements NON alignes (`loadu`) obligatoires :
 * &B[k*N+jj] depend de N arbitraire, aucun alignement 16 garanti a l'interne
 * d'une ligne. */

#define OWL_MATMUL_BI 64
#define OWL_MATMUL_BK 256
#define OWL_MATMUL_BJ 256

#if defined(OWL_MATMUL_SSE2)
/* Detection AVX2 memorisee (premier dispatch CPU runtime du codebase). -1 =
 * non teste, 0/1 = resultat cpuid fige. Ecrit une seule fois par le thread VM
 * (owl_matmul, point serie) AVANT toute region parallele ; les workers ne font
 * que le LIRE -> aucune course. */
static int g_matmul_avx2 = -1;

/* Detection AVX2 par cpuid brut (pas __builtin_cpu_supports, qui tire
 * __cpu_model de libgcc — absent en link statique zig). On verifie AUSSI que
 * l'OS sauvegarde l'etat YMM (OSXSAVE + XCR0 bits SSE/AVX via XGETBV) : sans
 * ca, executer une instruction AVX sur un CPU capable mais un OS non configure
 * fauterait. leaf 7 EBX bit 5 = AVX2. */
static int owl_cpu_has_avx2(void) {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
    int osxsave = (ecx & (1u << 27)) != 0;   /* OS a active XSAVE/XGETBV */
    int avx     = (ecx & (1u << 28)) != 0;   /* CPU supporte AVX */
    if (!osxsave || !avx) return 0;
    unsigned xcr0_lo, xcr0_hi;
    __asm__ __volatile__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    (void)xcr0_hi;
    if ((xcr0_lo & 0x6u) != 0x6u) return 0;   /* XMM (bit1) + YMM (bit2) sauves */
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return 0;
    return (ebx & (1u << 5)) != 0;            /* AVX2 */
}

static void owl_matmul_detect_simd(void) {
    if (g_matmul_avx2 < 0)
        g_matmul_avx2 = owl_cpu_has_avx2();
}

/* Variante AVX2 (4 doubles) — compilee avec le jeu AVX2 via target attribute,
 * appelee UNIQUEMENT si le cpuid runtime confirme le support. mul + add
 * SEPARES, JAMAIS _mm256_fmadd_pd : le FMA fondrait les deux en un arrondi et
 * casserait la parite bit avec le chemin scalaire/SSE2. AXPY est element-par-
 * element (pas de reduction sur j) -> la largeur du vecteur n'affecte pas
 * l'ordre : AVX2 non-FMA = bit-identique a SSE2. */
__attribute__((target("avx2")))
static void owl_matmul_axpy_avx2(double* restrict c, const double* restrict b,
                                 double a, int len) {
    int j = 0;
    __m256d va = _mm256_set1_pd(a);
    for (; j + 4 <= len; j += 4) {
        __m256d vb = _mm256_loadu_pd(b + j);
        __m256d vc = _mm256_loadu_pd(c + j);
        vc = _mm256_add_pd(vc, _mm256_mul_pd(va, vb));
        _mm256_storeu_pd(c + j, vc);
    }
    for (; j < len; j++) c[j] += a * b[j];
}
#endif

/* c[0..len) += a * b[0..len). Coeur AXPY vectorise (AVX2 si dispo, sinon
 * SSE2/NEON) + reste scalaire, ou tout scalaire sur cible generique. */
static inline void owl_matmul_axpy(double* restrict c, const double* restrict b,
                                   double a, int len) {
    int j = 0;
#if defined(OWL_MATMUL_SSE2)
    if (g_matmul_avx2 == 1) { owl_matmul_axpy_avx2(c, b, a, len); return; }
    __m128d va = _mm_set1_pd(a);
    for (; j + 2 <= len; j += 2) {
        __m128d vb = _mm_loadu_pd(b + j);
        __m128d vc = _mm_loadu_pd(c + j);
        vc = _mm_add_pd(vc, _mm_mul_pd(va, vb));
        _mm_storeu_pd(c + j, vc);
    }
#elif defined(OWL_MATMUL_NEON)
    float64x2_t va = vdupq_n_f64(a);
    for (; j + 2 <= len; j += 2) {
        float64x2_t vb = vld1q_f64(b + j);
        float64x2_t vc = vld1q_f64(c + j);
        vc = vfmaq_f64(vc, va, vb);
        vst1q_f64(c + j, vc);
    }
#endif
    for (; j < len; j++) c[j] += a * b[j];
}

/* Contexte partage du kernel threade. Aucun objet OWL, que des double* bruts :
 * les workers du pool compute n'operent que la-dessus (isolation VM/GC). */
typedef struct {
    const double* A;
    const double* B;
    double*       C;
    int K, N;
} OwlMatmulCtx;

/* Kernel sur une BANDE de lignes [i0, i1). Chaque C[i][*] appartient a un seul
 * thread : partition disjointe de M, aucune ecriture partagee. L'ordre
 * d'accumulation sur k (tuiles croissantes, k interieur croissant) est
 * strictement celui du kernel serie -> C[i][j] bit-a-bit identique quel que
 * soit OWL_THREADS. Le split se fait sur i UNIQUEMENT ; jamais sur k (ce qui
 * reordonnerait l'accumulation). */
static void owl_matmul_rows(int i0, int i1, void* vctx) {
    const OwlMatmulCtx* mc = (const OwlMatmulCtx*)vctx;
    const double* restrict A = mc->A;
    const double* restrict B = mc->B;
    double* restrict C = mc->C;
    int K = mc->K, N = mc->N;

    memset(C + (size_t)i0 * N, 0, (size_t)(i1 - i0) * (size_t)N * sizeof(double));
    for (int ii = i0; ii < i1; ii += OWL_MATMUL_BI) {
        int iMax = ii + OWL_MATMUL_BI < i1 ? ii + OWL_MATMUL_BI : i1;
        for (int kk = 0; kk < K; kk += OWL_MATMUL_BK) {
            int kMax = kk + OWL_MATMUL_BK < K ? kk + OWL_MATMUL_BK : K;
            for (int jj = 0; jj < N; jj += OWL_MATMUL_BJ) {
                int jMax = jj + OWL_MATMUL_BJ < N ? jj + OWL_MATMUL_BJ : N;
                int len = jMax - jj;
                for (int i = ii; i < iMax; i++) {
                    double* restrict crow = C + (size_t)i * N + jj;
                    for (int k = kk; k < kMax; k++) {
                        double aik = A[(size_t)i * K + k];
                        const double* restrict brow = B + (size_t)k * N + jj;
                        owl_matmul_axpy(crow, brow, aik, len);
                    }
                }
            }
        }
    }
}

/* C(M×N) = A(M×K) · B(K×N), row-major contigu. C est ecrit entierement
 * (mise a zero puis accumulation). Sert aussi le produit Matrice×Vecteur
 * (N = 1) : la boucle interne degenere alors en scalaire, ordre d'accumu-
 * lation sur k preserve -> bit-a-bit identique la aussi.
 *
 * Threade sur les lignes i via le pool compute. La detection AVX2 est faite
 * ICI (thread VM, point serie) avant tout fork : les workers ne LISENT que
 * g_matmul_avx2. */
void owl_matmul(const double* restrict A, const double* restrict B,
                double* restrict C, int M, int K, int N) {
#if defined(OWL_MATMUL_SSE2)
    owl_matmul_detect_simd();
#endif
    OwlMatmulCtx mc = { A, B, C, K, N };
    owl_parallel_for(M, (long)M * (long)K * (long)N, owl_matmul_rows, &mc);
}

static Value fn_produit_matriciel(int argCount, Value* args) {
    (void)argCount;
    /* Refus operande tableau (pas d'inference), comme l'arithmetique Matrice. */
    if (IS_ARRAY(args[0]) || IS_ARRAY(args[1])) {
        matrice_lever_code(E_RT_MATRIX_ARRAY_OPERAND);
        return NIL_VAL;
    }
    if (!IS_MATRIX(args[0])) {
        matrice_lever(
            "Le premier operande du produit matriciel doit etre une Matrice.",
            "The first operand of a matrix product must be a Matrix.");
        return NIL_VAL;
    }
    ObjMatrix* a = AS_MATRIX(args[0]);

    /* Surcharge Matrice × Matrice -> Matrice. */
    if (IS_MATRIX(args[1])) {
        ObjMatrix* b = AS_MATRIX(args[1]);
        if (a->cols != b->rows) {
            matrice_lever_code(E_RT_MATRIX_SIZE_MISMATCH);
            return NIL_VAL;
        }
        /* a et b restent rootes via args[] pendant l'alloc du resultat ;
         * le remplissage ne fait plus d'allocation OWL -> GC-sur. */
        ObjMatrix* res = obj_new_matrix(a->rows, b->cols);
        owl_matmul(a->data, b->data, res->data, a->rows, a->cols, b->cols);
        return OBJ_VAL(res);
    }

    /* Surcharge Matrice × Vecteur -> Vecteur (vecteur colonne). */
    if (IS_VECTOR(args[1])) {
        ObjVector* v = AS_VECTOR(args[1]);
        if (a->cols != v->count) {
            matrice_lever_code(E_RT_MATRIX_SIZE_MISMATCH);
            return NIL_VAL;
        }
        ObjVector* res = obj_new_vector(a->rows);
        /* Vecteur colonne = Matrice N=1 : meme kernel, boucle j degeneree. */
        owl_matmul(a->data, v->data, res->data, a->rows, a->cols, 1);
        return OBJ_VAL(res);
    }

    matrice_lever(
        "Le second operande du produit matriciel doit etre une Matrice ou un Vecteur.",
        "The second operand of a matrix product must be a Matrix or a Vector.");
    return NIL_VAL;
}

static Value fn_matrice_transpose(int argCount, Value* args) {
    (void)argCount;
    if (IS_ARRAY(args[0])) {
        matrice_lever_code(E_RT_MATRIX_ARRAY_OPERAND);
        return NIL_VAL;
    }
    if (!IS_MATRIX(args[0])) {
        matrice_lever(
            "La transposee attend une Matrice.",
            "Transpose expects a Matrix.");
        return NIL_VAL;
    }
    ObjMatrix* m = AS_MATRIX(args[0]);
    /* `m` reste roote via args[0] pendant l'alloc ; le remplissage ne fait
     * plus d'allocation OWL. Resultat cols×rows, element (j,i) <- (i,j). */
    ObjMatrix* res = obj_new_matrix(m->cols, m->rows);
    for (int i = 0; i < m->rows; i++) {
        for (int j = 0; j < m->cols; j++) {
            res->data[j * m->rows + i] = m->data[i * m->cols + j];
        }
    }
    return OBJ_VAL(res);
}

static Value fn_matrice_identite(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("MatriceIdentit\xc3\xa9", "taille", 0, IS_NUMBER(args[0]),
                    "entier", NIL_VAL);
    int64_t n = (int64_t)AS_NUMBER(args[0]);
    if (n < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }
    ObjMatrix* res = obj_new_matrix((int)n, (int)n);
    for (int i = 0; i < (int)n; i++) {
        for (int j = 0; j < (int)n; j++) {
            res->data[i * (int)n + j] = (i == j) ? 1.0 : 0.0;
        }
    }
    return OBJ_VAL(res);
}

/* MatriceAléatoire(lignes, colonnes) : Matrice lignes×colonnes remplie de
 * reels uniformes dans [0, 1[. Reutilise le PRNG non-crypto de la libc
 * (`rand()`) — le meme que `Hasard()`, meme etat global, meme formule
 * [0,1[. Le seeding (paresseux au premier tirage, ou explicite via
 * InitHasard) est detenu par la famille Hasard ; on ne re-seed pas ici pour
 * ne pas diverger de cet etat partage. Non-graine -> sequence par defaut de
 * la libc si aucun tirage prealable, ce qui suffit a une init de poids.
 * Dimensions <= 0 refusees (meme mur que MatriceIdentité). */
static Value fn_matrice_aleatoire(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("MatriceAl\xc3\xa9""atoire", "lignes", 0, IS_NUMBER(args[0]),
                    "entier", NIL_VAL);
    OWL_REQUIRE_ARG("MatriceAl\xc3\xa9""atoire", "colonnes", 1, IS_NUMBER(args[1]),
                    "entier", NIL_VAL);
    int64_t rows = (int64_t)AS_NUMBER(args[0]);
    int64_t cols = (int64_t)AS_NUMBER(args[1]);
    if (rows < 1 || cols < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }
    ObjMatrix* res = obj_new_matrix((int)rows, (int)cols);
    int total = (int)rows * (int)cols;
    for (int i = 0; i < total; i++) {
        res->data[i] = (double)rand() / ((double)RAND_MAX + 1.0);
    }
    return OBJ_VAL(res);
}

/* MatriceCrée(lignes, colonnes) : Matrice lignes×colonnes entierement
 * remplie de zeros (0.0), a batir ensuite cellule par cellule via
 * m[ligne][colonne]. Point de depart pour charger des donnees au runtime.
 * Dimensions <= 0 refusees (meme mur que MatriceIdentité / MatriceAléatoire).
 * obj_new_matrix zero-initialise deja le storage, mais on l'ecrit
 * explicitement pour ne dependre d'aucun invariant d'allocation. */
static Value fn_matrice_cree(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("MatriceCr\xc3\xa9""e", "lignes", 0, IS_NUMBER(args[0]),
                    "entier", NIL_VAL);
    OWL_REQUIRE_ARG("MatriceCr\xc3\xa9""e", "colonnes", 1, IS_NUMBER(args[1]),
                    "entier", NIL_VAL);
    int64_t rows = (int64_t)AS_NUMBER(args[0]);
    int64_t cols = (int64_t)AS_NUMBER(args[1]);
    if (rows < 1 || cols < 1) {
        matrice_lever(
            "Une Matrice doit avoir au moins une ligne et une colonne.",
            "A Matrix must have at least one row and one column.");
        return NIL_VAL;
    }
    ObjMatrix* res = obj_new_matrix((int)rows, (int)cols);
    int total = (int)rows * (int)cols;
    for (int i = 0; i < total; i++) res->data[i] = 0.0;
    return OBJ_VAL(res);
}

/* ================================================================
 * Fonctions d'activation
 *
 * Applications element-par-element rendant une NOUVELLE Matrice (immuable),
 * dimensions preservees :
 *   - ReLU     : max(0, x) ;
 *   - Sigmoide : 1 / (1 + e^-x), dans l'intervalle ouvert (0, 1) ;
 *   - Tanh     : tangente hyperbolique, dans l'intervalle ouvert (-1, 1).
 * Fonctions pures, sans differentiabilite : juste le calcul forward. Chaque
 * op existe en native prefixee `Matrice<Op>` et en methode fluente `.<Op>()`.
 * ================================================================ */
typedef double (*MatActivation)(double);

static double act_relu(double x)    { return x > 0.0 ? x : 0.0; }
static double act_sigmoide(double x){ return 1.0 / (1.0 + exp(-x)); }
static double act_tanh(double x)    { return tanh(x); }

static Value matrice_activation(int argCount, Value* args, const char* name, MatActivation f) {
    if (IS_ARRAY(args[0])) {
        matrice_lever_code(E_RT_MATRIX_ARRAY_OPERAND);
        return NIL_VAL;
    }
    OWL_REQUIRE_ARG(name, "matrice", 0, IS_MATRIX(args[0]), "Matrice", NIL_VAL);
    const ObjMatrix* m = AS_MATRIX(args[0]);
    int rows = m->rows, cols = m->cols;
    /* Root la source pendant l'alloc : obj_new_matrix peut declencher un GC.
     * Apres l'alloc, plus aucune allocation OWL (calcul pur). */
    vm_push(args[0]);
    ObjMatrix* res = obj_new_matrix(rows, cols);
    vm_pop();
    int total = rows * cols;
    for (int i = 0; i < total; i++) res->data[i] = f(m->data[i]);
    return OBJ_VAL(res);
}

static Value fn_matrice_relu(int argCount, Value* args) {
    return matrice_activation(argCount, args, "MatriceReLU", act_relu);
}
static Value fn_matrice_sigmoide(int argCount, Value* args) {
    return matrice_activation(argCount, args, "MatriceSigmo\xc3\xaf" "de", act_sigmoide);
}
static Value fn_matrice_tanh(int argCount, Value* args) {
    return matrice_activation(argCount, args, "MatriceTanh", act_tanh);
}

/* ================================================================
 * Socle tabulaire — passerelle CSV -> Matrice et
 * pretraitement par colonne (standardisation, normalisation).
 *
 * Point d'entree du ML tabulaire : charger un CSV tout-numerique puis
 * ramener chaque colonne (feature) sur une echelle comparable avant
 * l'entrainement. L'I/O reste a la charge des fonctions Fichiers ; ces
 * natives ne prennent QUE des chaines / Matrices deja en memoire.
 * ================================================================ */

/* Parse une cellule CSV en reel. Espaces de bord tolerES (RFC-4180 ne les
 * trim pas, mais un CSV numerique « propre » les considere insignifiants).
 * La conversion doit consommer TOUTE la cellule : "1.0" ok, "abc" refuse,
 * "1.0x" refuse. Cellule vide -> refusee. *ok porte le verdict. */
static double matrice_parse_cell(const char* s, int len, bool* ok) {
    while (len > 0 && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len <= 0 || len >= 64) { *ok = false; return 0.0; }
    char buf[64];
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    char* endptr = NULL;
    double v = strtod(buf, &endptr);
    if (endptr != buf + len) { *ok = false; return 0.0; }
    *ok = true;
    return v;
}

/* Avance d'une ligne logique. Frontieres reconnues : CRLF, CR seul, LF seul,
 * ou fin de chaine. Rend faux quand il n'y a plus de ligne (un separateur de
 * fin de chaine ne produit pas de ligne vide fantome). */
static bool matrice_csv_next_line(const char** p, const char* end,
                                  const char** lineStart, int* lineLen) {
    if (*p >= end) return false;
    const char* s = *p;
    const char* q = s;
    while (q < end && *q != '\r' && *q != '\n') q++;
    *lineStart = s;
    *lineLen = (int)(q - s);
    if (q < end) {
        if (*q == '\r' && q + 1 < end && q[1] == '\n') q += 2;
        else q += 1;
    }
    *p = q;
    return true;
}

/* Decoupe une ligne en champs sur `sep` (multi-octets accepte) et parse
 * chaque champ en reel. Si `out` != NULL, ecrit les reels (au plus `outCap`).
 * Rend le nombre de champs ; positionne *bad si un champ n'est pas numerique.
 * Aucune allocation OWL : sûr vis-a-vis du GC. */
static int matrice_csv_parse_line(const char* line, int lineLen,
                                  const char* sep, int sepLen,
                                  double* out, int outCap, bool* bad) {
    int count = 0;
    const char* fieldStart = line;
    const char* p = line;
    const char* end = line + lineLen;
    for (;;) {
        bool atEnd = (p >= end);
        bool atSep = (!atEnd && sepLen > 0 && p + sepLen <= end &&
                      memcmp(p, sep, (size_t)sepLen) == 0);
        if (!atEnd && !atSep) { p++; continue; }
        bool ok = true;
        double v = matrice_parse_cell(fieldStart, (int)(p - fieldStart), &ok);
        if (!ok) *bad = true;
        if (out && count < outCap) out[count] = v;
        count++;
        if (atEnd) break;
        p += sepLen;
        fieldStart = p;
    }
    return count;
}

/* CSVVersMatrice(csv, sepColonne, ignoreEntete) -> Matrice.
 * Deux passes sur la chaine : la premiere valide (dimensions, rectangularite,
 * cellules numeriques) sans rien allouer -> une exception rattrapable part
 * avec la pile VM propre ; la seconde alloue la Matrice une fois et la remplit
 * (csv reste roote via args[0], aucune autre allocation OWL -> GC-sûr). */
static Value fn_csv_vers_matrice(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("CSVVersMatrice", "csv", 0, IS_STRING(args[0]),
                    "chaine", NIL_VAL);
    OWL_REQUIRE_ARG("CSVVersMatrice", "separateur", 1, IS_STRING(args[1]),
                    "chaine", NIL_VAL);
    OWL_REQUIRE_ARG("CSVVersMatrice", "ignoreEntete", 2, IS_BOOL(args[2]),
                    "booleen", NIL_VAL);
    ObjString* csv = AS_STRING(args[0]);
    ObjString* sep = AS_STRING(args[1]);
    bool skipHeader = AS_BOOL(args[2]);
    const char* sepChars = sep->chars;
    int sepLen = sep->length;

    const char* end = csv->chars + csv->length;
    const char* p;
    const char* ls;
    int ll;

    /* Passe 1 : validation, sans allocation. */
    int cols = -1;
    int rows = 0;
    bool headerSkipped = false;
    p = csv->chars;
    while (matrice_csv_next_line(&p, end, &ls, &ll)) {
        if (skipHeader && !headerSkipped) { headerSkipped = true; continue; }
        bool bad = false;
        int n = matrice_csv_parse_line(ls, ll, sepChars, sepLen, NULL, 0, &bad);
        if (bad) {
            matrice_lever(
                "Cellule non numerique dans le CSV : chaque cellule de donnees "
                "doit etre un reel.",
                "Non-numeric cell in the CSV: every data cell must be a real.");
            return NIL_VAL;
        }
        if (cols < 0) cols = n;
        else if (n != cols) {
            matrice_lever(
                "CSV non rectangulaire : toutes les lignes doivent avoir le "
                "meme nombre de colonnes.",
                "Non-rectangular CSV: all rows must have the same number of "
                "columns.");
            return NIL_VAL;
        }
        rows++;
    }
    if (rows < 1 || cols < 1) {
        matrice_lever(
            "CSV vide : au moins une ligne de donnees d'une colonne est requise.",
            "Empty CSV: at least one data row with one column is required.");
        return NIL_VAL;
    }

    /* Passe 2 : remplissage. */
    ObjMatrix* mat = obj_new_matrix(rows, cols);
    headerSkipped = false;
    int r = 0;
    p = csv->chars;
    while (matrice_csv_next_line(&p, end, &ls, &ll)) {
        if (skipHeader && !headerSkipped) { headerSkipped = true; continue; }
        bool bad = false;
        matrice_csv_parse_line(ls, ll, sepChars, sepLen,
                               &mat->data[(size_t)r * cols], cols, &bad);
        r++;
    }
    return OBJ_VAL(mat);
}

/* Coeur partage des pretraitements par colonne. `f` recoit, pour la colonne
 * courante, la valeur brute et les deux statistiques calculees (a, b) et rend
 * la valeur transformee. Une colonne « plate » (statistique de dispersion
 * nulle) sort a 0.0 par convention, jamais NaN. */
static Value matrice_preprocess_cols(int argCount, Value* args, const char* name,
                                     bool standardize) {
    (void)argCount;
    if (IS_ARRAY(args[0])) {
        matrice_lever_code(E_RT_MATRIX_ARRAY_OPERAND);
        return NIL_VAL;
    }
    OWL_REQUIRE_ARG(name, "matrice", 0, IS_MATRIX(args[0]), "Matrice", NIL_VAL);
    const ObjMatrix* m = AS_MATRIX(args[0]);
    int rows = m->rows, cols = m->cols;
    /* Root la source pendant l'alloc (obj_new_matrix peut GC). Ensuite, plus
     * aucune allocation OWL : calcul pur sur des double. */
    vm_push(args[0]);
    ObjMatrix* res = obj_new_matrix(rows, cols);
    vm_pop();

    for (int c = 0; c < cols; c++) {
        if (standardize) {
            /* z-score : (x - moyenne) / ecart-type de population (division n). */
            double sum = 0.0;
            for (int r = 0; r < rows; r++) sum += m->data[(size_t)r * cols + c];
            double mean = sum / rows;
            double var = 0.0;
            for (int r = 0; r < rows; r++) {
                double d = m->data[(size_t)r * cols + c] - mean;
                var += d * d;
            }
            var /= rows;
            double sd = sqrt(var);
            for (int r = 0; r < rows; r++) {
                double x = m->data[(size_t)r * cols + c];
                res->data[(size_t)r * cols + c] = (sd == 0.0) ? 0.0 : (x - mean) / sd;
            }
        } else {
            /* min-max : (x - min) / (max - min) -> [0, 1]. */
            double mn = m->data[c], mx = m->data[c];
            for (int r = 0; r < rows; r++) {
                double x = m->data[(size_t)r * cols + c];
                if (x < mn) mn = x;
                if (x > mx) mx = x;
            }
            double range = mx - mn;
            for (int r = 0; r < rows; r++) {
                double x = m->data[(size_t)r * cols + c];
                res->data[(size_t)r * cols + c] = (range == 0.0) ? 0.0 : (x - mn) / range;
            }
        }
    }
    return OBJ_VAL(res);
}

static Value fn_matrice_standardise_colonnes(int argCount, Value* args) {
    return matrice_preprocess_cols(argCount, args,
                                   "MatriceStandardiseColonnes", true);
}

static Value fn_matrice_normalise_colonnes(int argCount, Value* args) {
    return matrice_preprocess_cols(argCount, args,
                                   "MatriceNormaliseColonnes", false);
}

/* ================================================================
 * Registry
 * ================================================================ */

/* Forme EN dans les aliases (resolution compilo + coerce) EN PLUS de nameEN
 * (formatter / LSP), meme convention que le Vecteur. */
static const char* const ALIAS_matrice[] = { "Matrix", NULL };

static const OwlTypeDef MATRICE_TYPES[] = {
    { .canonical      = "Matrice",
      .canonicalAscii = "Matrice",
      .aliases        = ALIAS_matrice,
      .category       = "Matrices",
      .descFR         = "Grille rectangulaire de reels indexee a partir de 1 "
                        "sur deux dimensions (ligne, colonne). Construite par un "
                        "litteral imbrique (`[[1.0, 2.0], [3.0, 4.0]]`) ou par un "
                        "pont explicite depuis un tableau : `TableauVersMatrice` "
                        "(tableau imbrique) ou `TableauVersMatriceColonne` (tableau "
                        "plat -> colonne). Aucune coercion implicite depuis un "
                        "tableau construit dynamiquement. Les dimensions sont fixees "
                        "a la construction ; chaque element se lit et s'ecrit comme "
                        "un reel via `m[ligne][colonne]`.",
      .descEN         = "Rectangular grid of reals indexed from 1 across two "
                        "dimensions (row, column). Built from a nested literal "
                        "(`[[1.0, 2.0], [3.0, 4.0]]`) or through an explicit bridge "
                        "from an array: `ArrayToMatrix` (nested array) or "
                        "`ArrayToColumnMatrix` (flat array -> column). No implicit "
                        "coercion from a dynamically built array. Dimensions are "
                        "fixed at construction; each element reads and writes as a "
                        "real through `m[row][column]`.",
      .fields         = NULL,
      /* Coerce `tableau imbrique -> Matrice` enregistre ci-dessous : marque le
       * type comme documente "se laisse coercer depuis un tableau" et autorise
       * le litteral `= [[...]]` a la declaration. */
      .coercible      = true,
      /* Declaration sans init -> Null (la matrice n'est pas encore construite),
       * comme le Vecteur. */
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "Matrix",
      .genre          = OWL_GENRE_F },   /* une Matrice */
};

void stdlib_matrice_init(void) {
    static const OwlParam p_tableau[] = {
        { "tableau", "tableau imbrique de reels",
          "Tableau imbrique rectangulaire (une ligne = un tableau de reels) a "
          "convertir en Matrice.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_tableau_vers_matrice[] = {
        { 1, p_tableau, "Matrice",
          "Matrice de memes dimensions, memes valeurs aux memes positions." },
        {0}
    };
    static const OwlParam p_matrice[] = {
        { "matrice", "Matrice",
          "Matrice a convertir en tableau imbrique de reels.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_matrice_vers_tableau[] = {
        { 1, p_matrice, "tableau imbrique de reels",
          "Tableau imbrique de memes dimensions, memes valeurs aux memes positions." },
        {0}
    };
    static const OwlParam p_ligne[] = {
        { "matrice", "Matrice", "Matrice source.", NULL, NULL },
        { "ligne", "entier",
          "Indice de ligne, a partir de 1.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_matrice_ligne[] = {
        { 2, p_ligne, "Vecteur",
          "Vecteur reprenant la ligne demandee ; sa taille est le nombre de "
          "colonnes de la Matrice." },
        {0}
    };
    static const OwlParam p_colonne[] = {
        { "matrice", "Matrice", "Matrice source.", NULL, NULL },
        { "colonne", "entier",
          "Indice de colonne, a partir de 1.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_matrice_colonne[] = {
        { 2, p_colonne, "Vecteur",
          "Vecteur reprenant la colonne demandee ; sa taille est le nombre de "
          "lignes de la Matrice." },
        {0}
    };
    static const OwlParam p_sous_lignes[] = {
        { "matrice", "Matrice", "Matrice source.", NULL, NULL },
        { "indices", "tableau",
          "Tableau d'entiers 1-based : les lignes a reprendre, dans cet ordre "
          "(repetition autorisee).", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_sous_lignes[] = {
        { 2, p_sous_lignes, "Matrice",
          "Nouvelle Matrice dont les lignes sont celles de la source aux indices "
          "donnes, dans l'ordre du tableau ; meme nombre de colonnes que la "
          "source." },
        {0}
    };
    static const OwlParam p_vers_colonne[] = {
        { "valeurs", "tableau",
          "Tableau plat de reels a ranger en colonne, un element par ligne.",
          NULL, NULL },
        {0}
    };
    static const OwlSignature sig_vers_colonne[] = {
        { 1, p_vers_colonne, "Matrice",
          "Matrice colonne de taille (nombre d'elements) × 1, memes valeurs dans "
          "le meme ordre." },
        {0}
    };

    /* --- Algebre lineaire --- */
    static const OwlParam p_produit_mm[] = {
        { "a", "Matrice", "Matrice de gauche.", NULL, NULL },
        { "b", "Matrice",
          "Matrice de droite ; son nombre de lignes doit egaler le nombre de "
          "colonnes de la premiere.", NULL, NULL },
        {0}
    };
    static const OwlParam p_produit_mv[] = {
        { "matrice", "Matrice", "Matrice de gauche.", NULL, NULL },
        { "vecteur", "Vecteur",
          "Vecteur colonne ; sa taille doit egaler le nombre de colonnes de la "
          "Matrice.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_produit_matriciel[] = {
        { 2, p_produit_mm, "Matrice",
          "Matrice produit, de dimensions lignes(a) × colonnes(b)." },
        { 2, p_produit_mv, "Vecteur",
          "Vecteur produit, de taille egale au nombre de lignes de la Matrice." },
        {0}
    };
    static const OwlParam p_transpose[] = {
        { "matrice", "Matrice", "Matrice a transposer.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_transpose[] = {
        { 1, p_transpose, "Matrice",
          "Transposee : l'element (ligne, colonne) devient (colonne, ligne) ; "
          "dimensions colonnes × lignes de la source." },
        {0}
    };
    static const OwlParam p_identite[] = {
        { "taille", "entier",
          "Ordre de la matrice identite ; doit etre strictement positif.",
          NULL, NULL },
        {0}
    };
    static const OwlSignature sig_identite[] = {
        { 1, p_identite, "Matrice",
          "Matrice identite taille × taille : 1 sur la diagonale, 0 ailleurs." },
        {0}
    };
    static const OwlParam p_aleatoire[] = {
        { "lignes", "entier",
          "Nombre de lignes ; doit etre strictement positif.", NULL, NULL },
        { "colonnes", "entier",
          "Nombre de colonnes ; doit etre strictement positif.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_aleatoire[] = {
        { 2, p_aleatoire, "Matrice",
          "Matrice lignes × colonnes dont chaque element est un reel aleatoire "
          "dans [0, 1[." },
        {0}
    };
    static const OwlParam p_cree[] = {
        { "lignes", "entier",
          "Nombre de lignes ; doit etre strictement positif.", NULL, NULL },
        { "colonnes", "entier",
          "Nombre de colonnes ; doit etre strictement positif.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_cree[] = {
        { 2, p_cree, "Matrice",
          "Matrice lignes × colonnes dont chaque cellule vaut 0.0." },
        {0}
    };

    /* --- Fonctions d'activation --- */
    static const OwlParam p_matrice_activation[] = {
        { "matrice", "Matrice",
          "Matrice dont chaque element est transforme.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_activation[] = {
        { 1, p_matrice_activation, "Matrice",
          "Nouvelle Matrice de memes dimensions, la fonction etant appliquee "
          "a chaque element." },
        {0}
    };

    /* --- Socle tabulaire --- */
    static const OwlParam p_csv_vers_matrice[] = {
        { "csv", "chaine",
          "Contenu CSV tout-numerique a convertir (une chaine deja en memoire, "
          "pas un chemin de fichier).", NULL, NULL },
        { "separateur", "chaine",
          "Separateur de colonne, par exemple \";\" ou \",\".", NULL, NULL },
        { "ignoreEntete", "booleen",
          "Vrai pour sauter la premiere ligne (entete) : elle n'est jamais "
          "convertie en reel.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_csv_vers_matrice[] = {
        { 3, p_csv_vers_matrice, "Matrice",
          "Matrice dont chaque ligne CSV devient une ligne et chaque champ une "
          "colonne. Une cellule non numerique (hors entete ignoree) leve une "
          "exception." },
        {0}
    };
    static const OwlParam p_matrice_preprocess[] = {
        { "matrice", "Matrice",
          "Matrice dont chaque colonne est traitee independamment.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_standardise[] = {
        { 1, p_matrice_preprocess, "Matrice",
          "Nouvelle Matrice de memes dimensions, chaque colonne centree-reduite ; "
          "une colonne constante sort a 0.0." },
        {0}
    };
    static const OwlSignature sig_normalise[] = {
        { 1, p_matrice_preprocess, "Matrice",
          "Nouvelle Matrice de memes dimensions, chaque colonne ramenee sur "
          "[0, 1] ; une colonne constante sort a 0.0." },
        {0}
    };

    static const OwlNativeDef DEFS_MATRICE[] = {
        { .canonical   = "TableauVersMatrice",
          .nameEN      = "ArrayToMatrix",
          .category    = "Matrices",
          .descFR      = "Convertit un tableau imbrique rectangulaire de reels en "
                         "Matrice de memes dimensions, sans coercion implicite "
                         "entre les deux types. Un tableau non rectangulaire ou "
                         "vide est refuse.",
          .descEN      = "Converts a rectangular nested array of reals into a "
                         "Matrix of the same dimensions, without implicit coercion "
                         "between the two types. A non-rectangular or empty array "
                         "is rejected.",
          .signatures  = sig_tableau_vers_matrice,
          .impl        = fn_tableau_vers_matrice,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceVersTableau",
          .nameEN      = "MatrixToArray",
          .category    = "Matrices",
          .descFR      = "Convertit une Matrice en tableau imbrique de reels de "
                         "memes dimensions, sans coercion implicite entre les deux "
                         "types.",
          .descEN      = "Converts a Matrix into a nested array of reals of the "
                         "same dimensions, without implicit coercion between the "
                         "two types.",
          .signatures  = sig_matrice_vers_tableau,
          .impl        = fn_matrice_vers_tableau,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceLigne",
          .nameEN      = "MatrixRow",
          .category    = "Matrices",
          .descFR      = "Extrait la ligne demandee d'une Matrice sous la forme "
                         "d'un Vecteur, sans coercion implicite entre les deux "
                         "types. Un indice de ligne hors limites leve une exception.",
          .descEN      = "Extracts the requested row of a Matrix as a Vector, "
                         "without implicit coercion between the two types. An "
                         "out-of-bounds row index raises an exception.",
          .signatures  = sig_matrice_ligne,
          .impl        = fn_matrice_ligne,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceColonne",
          .nameEN      = "MatrixColumn",
          .category    = "Matrices",
          .descFR      = "Extrait la colonne demandee d'une Matrice sous la forme "
                         "d'un Vecteur, sans coercion implicite entre les deux "
                         "types. Un indice de colonne hors limites leve une exception.",
          .descEN      = "Extracts the requested column of a Matrix as a Vector, "
                         "without implicit coercion between the two types. An "
                         "out-of-bounds column index raises an exception.",
          .signatures  = sig_matrice_colonne,
          .impl        = fn_matrice_colonne,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceSousLignes",
          .nameEN      = "MatrixSubRows",
          .category    = "Matrices",
          .descFR      = "Selectionne des lignes d'une Matrice par leurs indices "
                         "(a partir de 1) et rend une nouvelle Matrice : une ligne "
                         "par indice, dans l'ordre du tableau, avec le meme nombre "
                         "de colonnes que la source. Un indice peut se repeter. Sert "
                         "a materialiser un sous-ensemble de lignes, par exemple une "
                         "decoupe train/test. Un tableau d'indices vide, ou un indice "
                         "hors des lignes de la source, leve une exception.",
          .descEN      = "Selects rows of a Matrix by their indices (from 1) and "
                         "returns a new Matrix: one row per index, in array order, "
                         "with the same number of columns as the source. An index may "
                         "repeat. Useful to materialize a subset of rows, such as a "
                         "train/test split. An empty index array, or an index outside "
                         "the source rows, raises an exception.",
          .signatures  = sig_sous_lignes,
          .impl        = fn_matrice_sous_lignes,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "TableauVersMatriceColonne",
          .nameEN      = "ArrayToColumnMatrix",
          .category    = "Matrices",
          .descFR      = "Convertit un tableau plat de reels en Matrice colonne : "
                         "chaque element devient une ligne d'une Matrice a une seule "
                         "colonne, sans coercion implicite entre les deux types. Sert "
                         "a materialiser une cible d'entrainement. Un tableau vide, ou "
                         "un element non reel, leve une exception.",
          .descEN      = "Converts a flat array of reals into a column Matrix: each "
                         "element becomes a row of a single-column Matrix, without "
                         "implicit coercion between the two types. Useful to "
                         "materialize a training target. An empty array, or a "
                         "non-real element, raises an exception.",
          .signatures  = sig_vers_colonne,
          .impl        = fn_tableau_vers_matrice_colonne,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "ProduitMatriciel",
          .nameEN      = "MatrixProduct",
          .category    = "Matrices",
          .descFR      = "Produit matriciel A·B. Combine chaque ligne du premier "
                         "operande avec chaque colonne du second : le nombre de "
                         "colonnes du premier doit egaler le nombre de lignes du "
                         "second, sinon une exception est levee. Surcharge pour un "
                         "second operande Vecteur (traite comme une colonne), qui "
                         "produit alors un Vecteur. Un operande tableau est refuse ; "
                         "conversion explicite requise.",
          .descEN      = "Matrix product A·B. Combines each row of the first "
                         "operand with each column of the second: the first "
                         "operand's column count must equal the second's row count, "
                         "otherwise an exception is raised. Overloaded for a Vector "
                         "second operand (treated as a column), which then yields a "
                         "Vector. An array operand is rejected; explicit conversion "
                         "required.",
          .signatures  = sig_produit_matriciel,
          .impl        = fn_produit_matriciel,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceTranspose",
          .nameEN      = "MatrixTranspose",
          .category    = "Matrices",
          .descFR      = "Transposee d'une Matrice : echange lignes et colonnes, "
                         "l'element en (ligne, colonne) se retrouve en (colonne, "
                         "ligne). Le resultat a pour dimensions le nombre de "
                         "colonnes puis le nombre de lignes de la source. Un "
                         "operande tableau est refuse ; conversion explicite "
                         "requise.",
          .descEN      = "Transpose of a Matrix: swaps rows and columns, the "
                         "element at (row, column) moves to (column, row). The "
                         "result's dimensions are the source's column count then "
                         "row count. An array operand is rejected; explicit "
                         "conversion required.",
          .signatures  = sig_transpose,
          .impl        = fn_matrice_transpose,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical      = "MatriceIdentit\xc3\xa9",
          .canonicalAscii = "MatriceIdentite",
          .nameEN         = "MatrixIdentity",
          .category       = "Matrices",
          .descFR         = "Construit une Matrice identite d'ordre donne : 1 sur "
                            "la diagonale, 0 partout ailleurs. C'est l'element "
                            "neutre du produit matriciel. Une taille non "
                            "strictement positive leve une exception.",
          .descEN         = "Builds an identity Matrix of the given order: 1 on the "
                            "diagonal, 0 everywhere else. It is the neutral element "
                            "of the matrix product. A non strictly positive size "
                            "raises an exception.",
          .signatures     = sig_identite,
          .impl           = fn_matrice_identite,
          .flags          = OWL_NAT_PURE,
        },
        { .canonical      = "MatriceAl\xc3\xa9""atoire",
          .canonicalAscii = "MatriceAleatoire",
          .nameEN         = "MatrixRandom",
          .category       = "Matrices",
          .descFR         = "Construit une Matrice aux dimensions donnees (lignes, "
                            "colonnes) dont chaque element est un reel aleatoire "
                            "uniforme dans l'intervalle [0, 1[. Utile pour initialiser "
                            "des poids ; l'echelle et le centrage se font ensuite via "
                            "l'arithmetique Matrice, par exemple (m - 0.5) * echelle. "
                            "Des dimensions non strictement positives levent une "
                            "exception. Generateur non cryptographique.",
          .descEN         = "Builds a Matrix of the given dimensions (rows, columns) "
                            "where each element is a uniform random real in the "
                            "interval [0, 1[. Useful to initialize weights; scaling and "
                            "centering are then done through Matrix arithmetic, for "
                            "example (m - 0.5) * scale. Non strictly positive "
                            "dimensions raise an exception. Non-cryptographic "
                            "generator.",
          .signatures     = sig_aleatoire,
          .impl           = fn_matrice_aleatoire,
        },
        { .canonical      = "MatriceCr\xc3\xa9""e",
          .canonicalAscii = "MatriceCree",
          .nameEN         = "MatrixCreate",
          .category       = "Matrices",
          .descFR         = "Construit une Matrice aux dimensions donnees (lignes, "
                            "colonnes) initialisee a zero : chaque cellule vaut 0.0. "
                            "Sert de point de depart, a remplir ensuite cellule par "
                            "cellule via m[ligne][colonne]. Des dimensions non "
                            "strictement positives levent une exception.",
          .descEN         = "Builds a Matrix of the given dimensions (rows, columns) "
                            "initialized to zero: each cell holds 0.0. Serves as a "
                            "starting point, to be filled afterwards cell by cell "
                            "through m[row][column]. Non strictly positive dimensions "
                            "raise an exception.",
          .signatures     = sig_cree,
          .impl           = fn_matrice_cree,
        },
        { .canonical   = "MatriceReLU",
          .nameEN      = "MatrixReLU",
          .category    = "Matrices",
          .descFR      = "Applique la fonction ReLU (max(0, x)) a chaque element "
                         "d'une Matrice : les valeurs negatives deviennent 0, les "
                         "positives et le zero restent inchangees. Rend une nouvelle "
                         "Matrice de memes dimensions.",
          .descEN      = "Applies the ReLU function (max(0, x)) to each element of "
                         "a Matrix: negative values become 0, positive values and "
                         "zero are left unchanged. Returns a new Matrix of the same "
                         "dimensions.",
          .signatures  = sig_activation,
          .impl        = fn_matrice_relu,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical      = "MatriceSigmo\xc3\xaf" "de",
          .canonicalAscii = "MatriceSigmoide",
          .nameEN         = "MatrixSigmoid",
          .category       = "Matrices",
          .descFR         = "Applique la fonction sigmoide (1 / (1 + e^-x)) a chaque "
                            "element d'une Matrice, ramenant toute valeur reelle dans "
                            "l'intervalle ouvert (0, 1). Rend une nouvelle Matrice de "
                            "memes dimensions.",
          .descEN         = "Applies the sigmoid function (1 / (1 + e^-x)) to each "
                            "element of a Matrix, mapping any real value into the "
                            "open interval (0, 1). Returns a new Matrix of the same "
                            "dimensions.",
          .signatures     = sig_activation,
          .impl           = fn_matrice_sigmoide,
          .flags          = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceTanh",
          .nameEN      = "MatrixTanh",
          .category    = "Matrices",
          .descFR      = "Applique la tangente hyperbolique a chaque element d'une "
                         "Matrice, ramenant toute valeur reelle dans l'intervalle "
                         "ouvert (-1, 1). Rend une nouvelle Matrice de memes "
                         "dimensions.",
          .descEN      = "Applies the hyperbolic tangent to each element of a "
                         "Matrix, mapping any real value into the open interval "
                         "(-1, 1). Returns a new Matrix of the same dimensions.",
          .signatures  = sig_activation,
          .impl        = fn_matrice_tanh,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "CSVVersMatrice",
          .nameEN      = "CSVToMatrix",
          .category    = "Matrices",
          .descFR      = "Convertit un contenu CSV tout-numerique en Matrice : "
                         "chaque ligne devient une ligne, chaque champ une colonne. "
                         "L'argument est une chaine deja en memoire, pas un chemin. "
                         "Le separateur de colonne est parametrable et l'entete peut "
                         "etre ignoree (la premiere ligne n'est alors jamais "
                         "convertie). Une cellule non numerique hors entete leve une "
                         "exception ; les lignes doivent avoir le meme nombre de "
                         "colonnes.",
          .descEN      = "Converts an all-numeric CSV content into a Matrix: each "
                         "line becomes a row, each field a column. The argument is a "
                         "string already in memory, not a path. The column separator "
                         "is configurable and the header can be ignored (the first "
                         "line is then never converted). A non-numeric cell outside "
                         "the header raises an exception; rows must have the same "
                         "number of columns.",
          .signatures  = sig_csv_vers_matrice,
          .impl        = fn_csv_vers_matrice,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceStandardiseColonnes",
          .nameEN      = "MatrixStandardizeColumns",
          .category    = "Matrices",
          .descFR      = "Centre-reduit (z-score) chaque colonne d'une Matrice "
                         "independamment : (x - moyenne) / ecart-type, avec un "
                         "ecart-type de population (division par le nombre de "
                         "lignes). Une colonne constante sort entierement a 0.0, "
                         "jamais NaN. Rend une nouvelle Matrice de memes dimensions ; "
                         "la source n'est pas modifiee.",
          .descEN      = "Centers and scales (z-score) each column of a Matrix "
                         "independently: (x - mean) / standard deviation, using a "
                         "population standard deviation (division by the number of "
                         "rows). A constant column comes out entirely at 0.0, never "
                         "NaN. Returns a new Matrix of the same dimensions; the "
                         "source is left unchanged.",
          .signatures  = sig_standardise,
          .impl        = fn_matrice_standardise_colonnes,
          .flags       = OWL_NAT_PURE,
        },
        { .canonical   = "MatriceNormaliseColonnes",
          .nameEN      = "MatrixNormalizeColumns",
          .category    = "Matrices",
          .descFR      = "Ramene chaque colonne d'une Matrice sur l'intervalle "
                         "[0, 1] par min-max : (x - min) / (max - min), colonne par "
                         "colonne. Une colonne constante sort entierement a 0.0, "
                         "jamais NaN. Rend une nouvelle Matrice de memes dimensions ; "
                         "la source n'est pas modifiee.",
          .descEN      = "Rescales each column of a Matrix onto the [0, 1] interval "
                         "with min-max: (x - min) / (max - min), column by column. A "
                         "constant column comes out entirely at 0.0, never NaN. "
                         "Returns a new Matrix of the same dimensions; the source is "
                         "left unchanged.",
          .signatures  = sig_normalise,
          .impl        = fn_matrice_normalise_colonnes,
          .flags       = OWL_NAT_PURE,
        },
    };

    owl_registry_register_types(MATRICE_TYPES,
        (int)(sizeof(MATRICE_TYPES) / sizeof(MATRICE_TYPES[0])));
    owl_registry_register_module(DEFS_MATRICE,
        (int)(sizeof(DEFS_MATRICE) / sizeof(DEFS_MATRICE[0])));
    owl_register_instance_coerce("Matrice", parse_to_matrice);

    /* Methodes fluentes arithmetiques (arity = receveur + 1 operande). */
    owl_register_method(METHOD_TYPE_MATRIX, "Ajoute",    fn_matrice_ajoute,    2);
    owl_register_method(METHOD_TYPE_MATRIX, "Add",       fn_matrice_ajoute,    2);
    owl_register_method(METHOD_TYPE_MATRIX, "Soustrait", fn_matrice_soustrait, 2);
    owl_register_method(METHOD_TYPE_MATRIX, "Subtract",  fn_matrice_soustrait, 2);
    owl_register_method(METHOD_TYPE_MATRIX, "Multiplie", fn_matrice_multiplie, 2);
    owl_register_method(METHOD_TYPE_MATRIX, "Multiply",  fn_matrice_multiplie, 2);
    owl_register_method(METHOD_TYPE_MATRIX, "Divise",    fn_matrice_divise,    2);
    owl_register_method(METHOD_TYPE_MATRIX, "Divide",    fn_matrice_divise,    2);

    /* Algebre lineaire fluente. Produit = receveur + 1 operande
     * (arite 2, l'operande est une Matrice ou un Vecteur) ; Transpose = receveur
     * seul (arite 1). */
    owl_register_method(METHOD_TYPE_MATRIX, "ProduitMatriciel", fn_produit_matriciel, 2);
    owl_register_method(METHOD_TYPE_MATRIX, "MatrixProduct",    fn_produit_matriciel, 2);
    owl_register_method(METHOD_TYPE_MATRIX, "Transpose",        fn_matrice_transpose, 1);

    /* Fonctions d'activation fluentes — receveur seul (arite 1).
     * ReLU/Tanh identiques FR/EN ; Sigmoide a une forme EN distincte (Sigmoid). */
    owl_register_method(METHOD_TYPE_MATRIX, "ReLU",             fn_matrice_relu,     1);
    owl_register_method(METHOD_TYPE_MATRIX, "Sigmo\xc3\xaf" "de", fn_matrice_sigmoide, 1);
    owl_register_method(METHOD_TYPE_MATRIX, "Sigmoid",          fn_matrice_sigmoide, 1);
    owl_register_method(METHOD_TYPE_MATRIX, "Tanh",             fn_matrice_tanh,     1);
}
