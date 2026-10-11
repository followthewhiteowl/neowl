// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_tenseur.c — Type `Tenseur` / `Tensor` (, moteur autograd).
 *
 * Un Tenseur enveloppe une donnee (un Vecteur) et lui associe un gradient
 * (un Vecteur de meme forme, initialise a zero) ainsi qu'un noeud de graphe
 * de calcul : l'operation qui l'a produit (`+`, `*`, ou feuille) et ses
 * Tenseurs parents. C'est la brique de base de la differentiation
 * automatique en mode inverse (reverse-mode autograd) qui rend
 * l'entrainement d'un modele possible.
 *
 * Ce module (phase 1) pose le MOTEUR :
 *   - enregistrement du type Tenseur/Tensor (bilingue, coercible) ;
 *   - constructeur natif `Tenseur([...])` (feuille du graphe) ;
 *   - Retropropage()/Backpropagate() : tri topologique du graphe, remise a
 *     zero de tous les gradients, amorce a 1, puis parcours en ordre topo
 *     inverse appliquant la regle de la chaine en ACCUMULANT dans les
 *     parents.
 *
 * Les operateurs `+` et `*` differentiables entre Tenseurs vivent cote VM
 * (vm.c, OP_ADD / OP_MULTIPLY) et construisent le noeud via obj_tensor_binop.
 * Les proprietes ..Valeur/..Value et ..Gradient vivent aussi cote VM
 * (OP_STRUCT_GET) — le storage est un objet natif, pas une ObjInstance.
 *
 * Ce module ajoute aussi les OPS DE COUCHE differentiables par-dessus le meme
 * moteur : produit matriciel (deux cas — vecteur.vecteur -> scalaire ; matrice
 * m*n par vecteur n -> vecteur m) et activations (ReLU / Sigmoide / Tanh).
 * Chaque op ajoute un forward (arithmetique directe) et un noeud de graphe
 * portant sa regle backward (nouvelle entree du switch de tensor_backward).
 * Enregistrees sur les deux surfaces : native prefixee `Tenseur<Op>` et
 * methode fluente `.<Op>()`.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "memory.h"
#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* Parite bit-a-bit avec l'oracle OWL (deux arrondis par mul+add de la VM) :
 * on interdit la contraction mul+add -> FMA dans tout ce module (convolution
 * comprise), sinon un seul arrondi divergerait. Double ceinture avec
 * -ffp-contract=off pose cote CMake sur ce fichier. */
#pragma STDC FP_CONTRACT OFF

/* ================================================================
 * Helpers de forme N-D
 *
 * Le stockage d'un Tenseur est TOUJOURS un buffer plat contigu row-major ;
 * ndim + shape sur l'enveloppe decrivent la vue N-D. Les strides row-major se
 * deduisent de la forme (dernier axe le plus rapide) et sont recalcules a la
 * volee ici : pas d'etat redondant a maintenir, la donnee restant contigue.
 * ================================================================ */

/* Strides row-major contigus (en elements) pour `shape` de rang `ndim`. */
static void tensor_contig_strides(int ndim, const int* shape, int* strides) {
    int acc = 1;
    for (int i = ndim - 1; i >= 0; i--) {
        strides[i] = acc;
        acc *= shape[i];
    }
}

/* Lit un tableau OWL de dimensions (>= 1 chacune) dans `shape`. Rend le rang
 * (1..OWL_TENSOR_MAX_RANK) ou -1 sur erreur (deja emise). */
static int tensor_read_shape(Value v, int* shape, const char* nativeName) {
    if (!IS_ARRAY(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "dimensions",
                              "expected", "tableau de dimensions",
                              "actual", "type non pris en charge", NULL);
        return -1;
    }
    ObjArray* arr = AS_ARRAY(v);
    int nd = arr->elements.count;
    if (nd < 1 || nd > OWL_TENSOR_MAX_RANK) {
        char expbuf[96];
        snprintf(expbuf, sizeof expbuf,
                 "entre 1 et %d dimensions", OWL_TENSOR_MAX_RANK);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "dimensions",
                              "expected", expbuf,
                              "actual", "nombre de dimensions hors bornes", NULL);
        return -1;
    }
    for (int i = 0; i < nd; i++) {
        Value e = arr->elements.values[i];
        if (!IS_NUMBER(e)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                  "arg", "dimensions",
                                  "expected", "dimensions numeriques",
                                  "actual", "dimension non numerique", NULL);
            return -1;
        }
        int d = (int)AS_NUMBER(e);
        if (d < 1) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                  "arg", "dimensions",
                                  "expected", "dimensions strictement positives",
                                  "actual", "dimension <= 0", NULL);
            return -1;
        }
        shape[i] = d;
    }
    return nd;
}

/* ================================================================
 * Construction (feuille du graphe)
 * ================================================================ */

/* Construit un Tenseur feuille a partir d'un Vecteur ou d'un tableau de reels.
 * `src` est suppose rooted par le caller (argument sur la pile VM ou valeur de
 * coercion) : obj_new_tensor y alloue donnee + gradient. Une cellule de
 * tableau non-numerique leve une erreur recuperable et rend NIL. */
static Value tenseur_leaf_from_value(Value src, const char* nativeName) {
    if (IS_VECTOR(src)) {
        ObjVector* v = AS_VECTOR(src);
        ObjTensor* t = obj_new_tensor(v->count);
        double* td = tensor_cells(t->data);
        for (int i = 0; i < v->count; i++) td[i] = v->data[i];
        return OBJ_VAL(t);
    }
    /* Matrice -> Tenseur 2D feuille : ..Valeur/..Gradient rendront une Matrice
     * de meme forme. Le stockage interne reutilise ObjMatrix (row-major). */
    if (IS_MATRIX(src)) {
        ObjMatrix* m = AS_MATRIX(src);
        ObjTensor* t = obj_new_tensor_matrix(m->rows, m->cols);
        double* td = tensor_cells(t->data);
        int nel = m->rows * m->cols;
        for (int i = 0; i < nel; i++) td[i] = m->data[i];
        return OBJ_VAL(t);
    }
    if (IS_ARRAY(src)) {
        ObjArray* arr = AS_ARRAY(src);
        int n = arr->elements.count;
        /* Tableau imbrique (tableau-de-tableaux de reels) -> Tenseur 2D feuille,
         * aligne sur le chemin Tenseur(Matrice) : meme forme [lignes, colonnes],
         * memes valeurs, ..Valeur/..Gradient rendent une Matrice. Detection sur
         * le premier element ; regularite validee avant toute allocation. */
        if (n > 0 && IS_ARRAY(arr->elements.values[0])) {
            int rows = n;
            int cols = AS_ARRAY(arr->elements.values[0])->elements.count;
            for (int r = 0; r < rows; r++) {
                Value rowv = arr->elements.values[r];
                if (!IS_ARRAY(rowv)) {
                    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                          "arg", "ligne",
                                          "expected", "tableau imbrique regulier (chaque ligne est un tableau de reels)",
                                          "actual", "ligne non tableau (imbrication mixte)",
                                          NULL);
                    return NIL_VAL;
                }
                ObjArray* row = AS_ARRAY(rowv);
                if (row->elements.count != cols) {
                    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                          "arg", "lignes",
                                          "expected", "lignes de meme longueur",
                                          "actual", "lignes de longueurs differentes",
                                          NULL);
                    return NIL_VAL;
                }
                for (int c = 0; c < cols; c++) {
                    if (!IS_NUMBER(row->elements.values[c])) {
                        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                              "arg", "element",
                                              "expected", "reel (tableau imbrique 2D)",
                                              "actual", "type non pris en charge (imbrication au-dela de 2 dimensions ?)",
                                              NULL);
                        return NIL_VAL;
                    }
                }
            }
            /* src reste roote par le caller : arr survit a l'alloc/GC ci-dessous. */
            ObjTensor* t = obj_new_tensor_matrix(rows, cols);
            double* td = tensor_cells(t->data);
            for (int r = 0; r < rows; r++) {
                ObjArray* row = AS_ARRAY(arr->elements.values[r]);
                for (int c = 0; c < cols; c++) {
                    td[(size_t)r * cols + c] = AS_NUMBER(row->elements.values[c]);
                }
            }
            return OBJ_VAL(t);
        }
        for (int i = 0; i < n; i++) {
            if (!IS_NUMBER(arr->elements.values[i])) {
                owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                      "arg", "element",
                                      "expected", "reel",
                                      "actual", "type non pris en charge",
                                      NULL);
                return NIL_VAL;
            }
        }
        ObjTensor* t = obj_new_tensor(n);
        double* td = tensor_cells(t->data);
        for (int i = 0; i < n; i++) {
            td[i] = AS_NUMBER(arr->elements.values[i]);
        }
        return OBJ_VAL(t);
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                          "arg", "source",
                          "expected", "tableau de reels, Vecteur ou Matrice",
                          "actual", "type non pris en charge",
                          NULL);
    return NIL_VAL;
}

/* Coerce `tableau/Vecteur -> Tenseur` (declaration-pilote). Emis par le
 * compilateur sur `t est un Tenseur = <expr>`. Pass-through si <expr> est deja
 * un Tenseur ; construction d'une feuille depuis un tableau ou un Vecteur ;
 * erreur sinon. */
static Value parse_to_tenseur(Value v) {
    if (IS_TENSOR(v)) {
        owl_clear_error();
        return v;
    }
    /* L'expression source a soft-errored (rendu NIL apres avoir pose une
     * erreur) : PROPAGER cette erreur plutot que l'ecraser par le generique.
     * Le generique ne vaut que pour un vrai type non-coercible (aucune erreur
     * prealable). Cf. . */
    if (IS_NIL(v) && owl_err_is_detected()) {
        return NIL_VAL;
    }
    owl_clear_error();
    return tenseur_leaf_from_value(v, "Tenseur");
}

/* Constructeur natif : `Tenseur([3.0])` / `Tensor([5.0])`. */
static Value fn_tenseur(int argCount, Value* args) {
    (void)argCount;
    return tenseur_leaf_from_value(args[0], "Tenseur");
}

/* ================================================================
 * Moteur de retropropagation (reverse-mode autograd)
 * ================================================================ */

/* Ajoute `t` a la liste topo apres avoir visite ses parents (post-ordre DFS) :
 * un noeud apparait toujours APRES ses parents. Le parcours en ordre inverse
 * traite donc la sortie d'abord, les feuilles en dernier. Le marqueur
 * `visited` evite de re-visiter un noeud partage (ex `a*a` : le parent `a` est
 * atteint deux fois, mais ajoute une seule fois). */
static void tensor_build_topo(ObjTensor* t, ObjTensor*** list,
                              int* count, int* cap) {
    if (t == NULL || t->visited) return;
    t->visited = true;
    for (int i = 0; i < t->nParents; i++) {
        tensor_build_topo(t->parents[i], list, count, cap);
    }
    if (*count >= *cap) {
        *cap = (*cap == 0) ? 8 : (*cap * 2);
        *list = (ObjTensor**)realloc(*list, (size_t)(*cap) * sizeof(ObjTensor*));
    }
    (*list)[(*count)++] = t;
}

/* Transpose src (rows×cols, row-major) dans dst (cols×rows, row-major). */
static void tensor_transpose_into(const double* restrict src, double* restrict dst,
                                  int rows, int cols) {
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            dst[(size_t)j * rows + i] = src[(size_t)i * cols + j];
}

/* Retropropagation depuis `root`, dans cet ordre exact :
 *   1. tri topologique du graphe (feuilles comprises) ;
 *   2. RESET : tous les gradients du graphe a zero (garde-fou pour que deux
 *      appels successifs ne s'accumulent PAS entre eux) ;
 *   3. amorce : gradient de root = 1 ;
 *   4. parcours en ordre topo INVERSE, chaque noeud accumulant (`+=`) dans le
 *      gradient de ses parents selon sa regle backward.
 * L'accumulation est cruciale : un noeud utilise plusieurs fois (`a*a`) somme
 * ses contributions -> a.grad = 2*a.valeur.
 * Aucune allocation OWL ici (arithmetique sur des Vecteur existants) : pas de
 * cycle GC, `root` reste roote par le dispatch de la methode. */
/* ================================================================
 * Convolution 2D via im2col + owl_matmul
 *
 * On deplie la convolution en produit matriciel pour reutiliser le kernel
 * owl_matmul (threade, AVX2 non-FMA, cache-bloque, deterministe cross-thread).
 * Layout : entree id[B,Cin,H,W] contigu ; poids wd[Cout,Cin,kH,kW] contigu =
 * DEJA la matrice W_mat[Cout, KK] (KK = Cin*kH*kW, colonne k = (ci*kH+kh)*kW+kw,
 * aucune copie). Sortie od[B,Cout,Hout,Wout] : la tranche image b est
 * out_b[Cout, HW] contigue (HW = Hout*Wout, colonne s = i*Wout+j).
 *
 * Parite : owl_matmul reduit sa dimension interne k en UNE somme courante,
 * indices croissants -> memes arrondis (mul+add non-FMA) que la boucle directe.
 * Les positions de remplissage valent 0.0 dans la colonne : W*0 ajoute = neutre
 * IEEE, equivalent exact au `continue` de la boucle directe. Le forward et dW
 * restent bit-identiques a la boucle directe ; dInput passe par le regime
 * im2col (co reduit en premier via dCol) = re-baseline assume. */

/* Deplie l'image b en colonne col[KKg, HW] (layout [k, s], k=(cl,kh,kw)).
 * Positions hors bornes (remplissage) = 0.0. Sert le forward (col_b) et
 * chaque image du dInput.
 * Convolution groupee : ne deplie que les `CinG` canaux d'entree
 * du groupe, a partir du canal absolu `ciBase`. Le canal absolu ci=ciBase+cl
 * sert a indexer l'entree (stride canal = Cin ENTIER), mais l'index de ligne k
 * est LOCAL au groupe (cl in [0,CinG[). group=1 -> ciBase=0, CinG=Cin =
 * comportement d'origine bit-identique. */
static void conv_im2col_khw(const double* id, double* col, int b,
                            int Cin, int ciBase, int CinG,
                            int H, int Wd, int kH, int kW,
                            int Hout, int Wout, int stride, int pad) {
    int HW = Hout * Wout;
    for (int cl = 0; cl < CinG; cl++) {
        int ci = ciBase + cl;
        for (int kh = 0; kh < kH; kh++)
        for (int kw = 0; kw < kW; kw++) {
            int k = (cl * kH + kh) * kW + kw;
            double* crow = col + (size_t)k * HW;
            for (int i = 0; i < Hout; i++)
            for (int j = 0; j < Wout; j++) {
                int ih = i * stride - pad + kh;
                int iw = j * stride - pad + kw;
                crow[i * Wout + j] =
                    (ih >= 0 && ih < H && iw >= 0 && iw < Wd)
                    ? id[((b * Cin + ci) * H + ih) * Wd + iw] : 0.0;
            }
        }
    }
}

/* Deplie TOUT le lot en colonnes transposees COLt[B*HW, KK] (layout
 * [t = b*HW+s, k]) : c'est colᵀ, le second operande de dW = dOut @ colᵀ.
 * La dimension interne du matmul balaie alors t = (b,i,j) croissant en UNE
 * somme courante -> bit-identique a la boucle directe (pas de sous-totaux
 * par image, qui reordonneraient l'accumulation). */
static void conv_im2col_tk(const double* id, double* colt, int B,
                           int Cin, int ciBase, int CinG,
                           int H, int Wd, int kH, int kW,
                           int Hout, int Wout, int stride, int pad, int KK) {
    int HW = Hout * Wout;
    for (int b = 0; b < B; b++)
    for (int cl = 0; cl < CinG; cl++) {
        int ci = ciBase + cl;
        for (int kh = 0; kh < kH; kh++)
        for (int kw = 0; kw < kW; kw++) {
            int k = (cl * kH + kh) * kW + kw;
            for (int i = 0; i < Hout; i++)
            for (int j = 0; j < Wout; j++) {
                int ih = i * stride - pad + kh;
                int iw = j * stride - pad + kw;
                int t = b * HW + i * Wout + j;
                colt[(size_t)t * KK + k] =
                    (ih >= 0 && ih < H && iw >= 0 && iw < Wd)
                    ? id[((b * Cin + ci) * H + ih) * Wd + iw] : 0.0;
            }
        }
    }
}

/* Replie dCol_b[KK, HW] (layout [k, s]) dans le gradient d'entree ig de l'image
 * b : chaque (k=(ci,kh,kw), s=(i,j)) dont (ih,iw) tombe dans les bornes ajoute
 * sa contribution a ig[b,ci,ih,iw]. Balayage s = (i,j) croissants EXTERNE, k
 * interne -> pour une cible (ci,ih,iw) donnee, accumulation en (i,j) croissants
 * (col2im du regime im2col re-baseline). */
static void conv_col2im_add(const double* dcol, double* ig, int b,
                            int Cin, int ciBase, int CinG,
                            int H, int Wd, int kH, int kW,
                            int Hout, int Wout, int stride, int pad) {
    int HW = Hout * Wout;
    for (int i = 0; i < Hout; i++)
    for (int j = 0; j < Wout; j++) {
        int s = i * Wout + j;
        for (int cl = 0; cl < CinG; cl++) {
            int ci = ciBase + cl;
            for (int kh = 0; kh < kH; kh++)
            for (int kw = 0; kw < kW; kw++) {
                int ih = i * stride - pad + kh;
                int iw = j * stride - pad + kw;
                if (ih < 0 || ih >= H || iw < 0 || iw >= Wd) continue;
                int k = (cl * kH + kh) * kW + kw;
                ig[((b * Cin + ci) * H + ih) * Wd + iw] += dcol[(size_t)k * HW + s];
            }
        }
    }
}

static void tensor_backward(ObjTensor* root) {
    ObjTensor** topo = NULL;
    int count = 0, cap = 0;
    tensor_build_topo(root, &topo, &count, &cap);

    /* Reset de tous les gradients + nettoyage du marqueur transitoire. Le
     * gradient est plat (Vecteur 1D ou Matrice 2D) : tensor_numel/tensor_cells
     * unifient l'acces sans distinguer la dimension. */
    for (int i = 0; i < count; i++) {
        ObjTensor* t = topo[i];
        double* g = tensor_cells(t->grad);
        int ng = tensor_numel(t->grad);
        for (int k = 0; k < ng; k++) g[k] = 0.0;
        t->visited = false;
    }

    /* Amorce : dL/dL = 1 sur chaque element de la sortie. */
    {
        double* rg = tensor_cells(root->grad);
        int nr = tensor_numel(root->grad);
        for (int k = 0; k < nr; k++) rg[k] = 1.0;
    }

    /* Ordre topo inverse : sortie d'abord. */
    for (int i = count - 1; i >= 0; i--) {
        ObjTensor* t = topo[i];
        int n = tensor_numel(t->grad);
        double* tg = tensor_cells(t->grad);
        double* tdat = tensor_cells(t->data);
        switch (t->op) {
            case TENSOR_OP_ADD: {
                ObjTensor* a = t->parents[0];
                ObjTensor* b = t->parents[1];
                double* ag = tensor_cells(a->grad);
                double* bg = tensor_cells(b->grad);
                if (tensor_shapes_equal(a, b)) {
                    /* Addition element-par-element : gradient transmis tel quel
                     * aux deux operandes. */
                    for (int k = 0; k < n; k++) {
                        ag[k] += tg[k];
                        bg[k] += tg[k];
                    }
                } else {
                    /*  — un operande a ete diffuse : son gradient est la
                     * reduce-sum du gradient de sortie sur les axes etires/absents.
                     * Realisee implicitement par les strides broadcast (0 sur un
                     * axe diffuse -> plusieurs index de sortie retombent sur le
                     * meme index d'operande et s'y additionnent via `+=`). Le cas
                     * biais 2D+1D est desormais ce chemin general. */
                    int bsA[OWL_TENSOR_MAX_RANK], bsB[OWL_TENSOR_MAX_RANK];
                    tensor_broadcast_strides(a->ndim, a->shape, t->ndim, bsA);
                    tensor_broadcast_strides(b->ndim, b->shape, t->ndim, bsB);
                    for (int fo = 0; fo < n; fo++) {
                        int rem = fo, ia = 0, ib = 0;
                        for (int d = t->ndim - 1; d >= 0; d--) {
                            int idx = rem % t->shape[d];
                            rem /= t->shape[d];
                            ia += idx * bsA[d];
                            ib += idx * bsB[d];
                        }
                        ag[ia] += tg[fo];
                        bg[ib] += tg[fo];
                    }
                }
                break;
            }
            case TENSOR_OP_MATMUL2D: {
                /* Produit matriciel dense C(p×r) = A(p×q) @ B(q×r). tg = gradient
                 * de C (taille p*r). Regles backward :
                 *   dA(p×q) = dSortie(p×r) @ Bᵀ(r×q)
                 *   dB(q×r) = Aᵀ(q×p) @ dSortie(p×r)
                 * Ce sont deux produits de matrices -> memes kernel rapide que le
                 * forward (`owl_matmul`, tuile cache + AXPY SSE2). `owl_matmul`
                 * ECRASE sa sortie -> on calcule dans des buffers scratch C-heap
                 * puis on ACCUMULE (`+=`) dans le gradient existant (preserve le
                 * partage d'un meme parent). Scratch = pur double*, aucun objet
                 * OWL -> pas de rooting GC. L'accumulation bloquee du kernel change
                 * l'ordre des additions flottantes vs l'ancien scalaire (ecart au
                 * dernier ULP, non associatif) : attendu, comme le forward. */
                ObjTensor* A = t->parents[0];
                ObjTensor* B = t->parents[1];
                ObjMatrix* Am = (ObjMatrix*)A->data;
                ObjMatrix* Bm = (ObjMatrix*)B->data;
                int p = Am->rows, q = Am->cols, r = Bm->cols;
                double* Ad = Am->data;
                double* Bd = Bm->data;
                double* Ag = tensor_cells(A->grad);
                double* Bg = tensor_cells(B->grad);
                size_t nBt = (size_t)r * q;   /* Bᵀ  : r×q */
                size_t nAt = (size_t)q * p;   /* Aᵀ  : q×p */
                size_t ndA = (size_t)p * q;   /* dA  : p×q */
                size_t ndB = (size_t)q * r;   /* dB  : q×r */
                double* scratch = (double*)malloc((nBt + nAt + ndA + ndB) * sizeof(double));
                double* Bt  = scratch;
                double* At  = Bt + nBt;
                double* dAt = At + nAt;
                double* dBt = dAt + ndA;
                tensor_transpose_into(Bd, Bt, q, r);       /* B(q×r) -> Bᵀ(r×q) */
                tensor_transpose_into(Ad, At, p, q);       /* A(p×q) -> Aᵀ(q×p) */
                owl_matmul(tg, Bt, dAt, p, r, q);          /* dA = dSortie @ Bᵀ */
                owl_matmul(At, tg, dBt, q, p, r);          /* dB = Aᵀ @ dSortie */
                for (size_t k = 0; k < ndA; k++) Ag[k] += dAt[k];
                for (size_t k = 0; k < ndB; k++) Bg[k] += dBt[k];
                free(scratch);
                break;
            }
            case TENSOR_OP_MUL: {
                ObjTensor* a = t->parents[0];
                ObjTensor* b = t->parents[1];
                double* ag = tensor_cells(a->grad);
                double* bg = tensor_cells(b->grad);
                double* ad = tensor_cells(a->data);
                double* bd = tensor_cells(b->data);
                if (tensor_shapes_equal(a, b)) {
                    for (int k = 0; k < n; k++) {
                        ag[k] += tg[k] * bd[k];
                        bg[k] += tg[k] * ad[k];
                    }
                } else {
                    /*  — produit diffuse : grad d'un operande =
                     * grad_out * autre_operande (chacun lu a son index broadcast),
                     * PUIS reduce-sum sur les axes diffuses de cet operande (les
                     * strides 0 font retomber les contributions sur le meme index
                     * -> accumulation via `+=`). */
                    int bsA[OWL_TENSOR_MAX_RANK], bsB[OWL_TENSOR_MAX_RANK];
                    tensor_broadcast_strides(a->ndim, a->shape, t->ndim, bsA);
                    tensor_broadcast_strides(b->ndim, b->shape, t->ndim, bsB);
                    for (int fo = 0; fo < n; fo++) {
                        int rem = fo, ia = 0, ib = 0;
                        for (int d = t->ndim - 1; d >= 0; d--) {
                            int idx = rem % t->shape[d];
                            rem /= t->shape[d];
                            ia += idx * bsA[d];
                            ib += idx * bsB[d];
                        }
                        ag[ia] += tg[fo] * bd[ib];
                        bg[ib] += tg[fo] * ad[ia];
                    }
                }
                break;
            }
            case TENSOR_OP_MATMUL: {
                /* Produit scalaire : sortie scalaire (t->grad = 1 element), mais
                 * gradients des parents de la forme des vecteurs d'entree.
                 * dC/da[k] = grad_out * b[k] ; dC/db[k] = grad_out * a[k]. */
                ObjTensor* a = t->parents[0];
                ObjTensor* b = t->parents[1];
                double g = tg[0];
                double* ag = tensor_cells(a->grad);
                double* bg = tensor_cells(b->grad);
                double* ad = tensor_cells(a->data);
                double* bd = tensor_cells(b->data);
                int m = tensor_numel(a->data);
                for (int k = 0; k < m; k++) {
                    ag[k] += g * bd[k];
                    bg[k] += g * ad[k];
                }
                break;
            }
            case TENSOR_OP_MATVEC: {
                /* Produit matrice-vecteur : W (2D m×n, parents[0]) @ x (1D n,
                 * parents[1]) -> y (1D m). gradSortie = grad de y (taille m).
                 *   gradW[i][j] += gradSortie[i] * x[j]  (produit externe)
                 *   gradX[j]    += Σ_i W[i][j] * gradSortie[i]  (Wᵀ @ gradSortie)
                 * L'accumulation (`+=`) preserve le partage d'un meme parent. */
                ObjTensor* W = t->parents[0];
                ObjTensor* x = t->parents[1];
                ObjMatrix* Wd = (ObjMatrix*)W->data;
                double* Wg = tensor_cells(W->grad);   /* meme forme que Wd */
                double* xd = tensor_cells(x->data);
                double* xg = tensor_cells(x->grad);
                int rows = Wd->rows, cols = Wd->cols;
                for (int r = 0; r < rows; r++) {
                    double gi = tg[r];
                    int base = r * cols;
                    for (int c = 0; c < cols; c++) {
                        Wg[base + c] += gi * xd[c];
                        xg[c]        += Wd->data[base + c] * gi;
                    }
                }
                break;
            }
            case TENSOR_OP_RELU: {
                /* Le gradient ne passe que la ou l'entree etait > 0. */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                double* ad = tensor_cells(a->data);
                for (int k = 0; k < n; k++) {
                    if (ad[k] > 0.0) ag[k] += tg[k];
                }
                break;
            }
            case TENSOR_OP_SIGMOID: {
                /* dSigmoide/dx = y*(1-y), y = sortie (deja calculee dans data). */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                for (int k = 0; k < n; k++) {
                    double y = tdat[k];
                    ag[k] += y * (1.0 - y) * tg[k];
                }
                break;
            }
            case TENSOR_OP_TANH: {
                /* dTanh/dx = 1 - y^2, y = sortie (deja calculee dans data). */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                for (int k = 0; k < n; k++) {
                    double y = tdat[k];
                    ag[k] += (1.0 - y * y) * tg[k];
                }
                break;
            }
            case TENSOR_OP_MSE: {
                /* Sortie scalaire ; gradient remonte uniquement vers la
                 * prediction (parents[0]). dL/dpred[k] = grad_out * 2*(pred[k]
                 * - cible[k]) / m. La cible (parents[1]) ne recoit rien. */
                ObjTensor* pred   = t->parents[0];
                ObjTensor* cible  = t->parents[1];
                double g = tg[0];
                double* pg = tensor_cells(pred->grad);
                double* pd = tensor_cells(pred->data);
                double* cd = tensor_cells(cible->data);
                int m = tensor_numel(pred->data);
                for (int k = 0; k < m; k++) {
                    double diff = pd[k] - cd[k];
                    pg[k] += g * 2.0 * diff / (double)m;
                }
                break;
            }
            case TENSOR_OP_CROSS_ENTROPY: {
                /* Softmax fusionnee. Gradient exact de L = -sum(cible*log(p))
                 * vers les logits (parents[0]) : dL/dx_j = p_j*(sum cible) -
                 * cible_j. Se reduit a (p - cible) quand la cible somme a 1
                 * (one-hot ou distribution), et donne 0 pour une ligne-cible
                 * nulle (sum=0, cellule de fond) -- coherent avec le forward
                 * qui vaut alors 0. Softmax recalculee ici (stable : soustraction
                 * du max), sans allocation OWL. La cible (parents[1]) ne recoit
                 * rien. */
                ObjTensor* pred  = t->parents[0];
                ObjTensor* cible = t->parents[1];
                double g = tg[0];
                double* pg = tensor_cells(pred->grad);
                double* pd = tensor_cells(pred->data);
                double* cd = tensor_cells(cible->data);
                if (tensor_is_2d(pred)) {
                    /* Batch : softmax par ligne, gradient
                     * (softmax * rowSum - cible) / B par element, B = nombre de
                     * lignes, rowSum = somme de la ligne cible. */
                    ObjMatrix* pm = (ObjMatrix*)pred->data;
                    int B = pm->rows, K = pm->cols;
                    for (int rr = 0; rr < B; rr++) {
                        int base = rr * K;
                        double mx = pd[base];
                        for (int c = 1; c < K; c++) {
                            if (pd[base + c] > mx) mx = pd[base + c];
                        }
                        double sum = 0.0;
                        for (int c = 0; c < K; c++) sum += exp(pd[base + c] - mx);
                        double rowSum = 0.0;
                        for (int c = 0; c < K; c++) rowSum += cd[base + c];
                        for (int c = 0; c < K; c++) {
                            double pk = exp(pd[base + c] - mx) / sum;
                            pg[base + c] += g * (pk * rowSum - cd[base + c]) / (double)B;
                        }
                    }
                    break;
                }
                int m = tensor_numel(pred->data);
                double mx = pd[0];
                for (int k = 1; k < m; k++) {
                    if (pd[k] > mx) mx = pd[k];
                }
                double sum = 0.0;
                for (int k = 0; k < m; k++) sum += exp(pd[k] - mx);
                double totalSum = 0.0;
                for (int k = 0; k < m; k++) totalSum += cd[k];
                for (int k = 0; k < m; k++) {
                    double pk = exp(pd[k] - mx) / sum;
                    pg[k] += g * (pk * totalSum - cd[k]);
                }
                break;
            }
            case TENSOR_OP_BCE_LOGITS: {
                /* Sortie scalaire ; gradient remonte uniquement vers les logits
                 * (parents[0]). Sigmoide fusionnee : dL/dx[k] = grad_out *
                 * (sigmoide(x[k]) - cible[k]) / m. La cible (parents[1]) ne recoit
                 * rien. */
                ObjTensor* pred  = t->parents[0];
                ObjTensor* cible = t->parents[1];
                double g = tg[0];
                double* pg = tensor_cells(pred->grad);
                double* pd = tensor_cells(pred->data);
                double* cd = tensor_cells(cible->data);
                int m = tensor_numel(pred->data);
                for (int k = 0; k < m; k++) {
                    double sig = 1.0 / (1.0 + exp(-pd[k]));
                    pg[k] += g * (sig - cd[k]) / (double)m;
                }
                break;
            }
            case TENSOR_OP_L1: {
                /* Sortie scalaire ; gradient remonte uniquement vers la
                 * prediction (parents[0]). dL/dpred[k] = grad_out *
                 * signe(pred[k] - cible[k]) / m ; signe(0) = 0. La cible
                 * (parents[1]) ne recoit rien. */
                ObjTensor* pred  = t->parents[0];
                ObjTensor* cible = t->parents[1];
                double g = tg[0];
                double* pg = tensor_cells(pred->grad);
                double* pd = tensor_cells(pred->data);
                double* cd = tensor_cells(cible->data);
                int m = tensor_numel(pred->data);
                for (int k = 0; k < m; k++) {
                    double diff = pd[k] - cd[k];
                    double sgn = (diff > 0.0) ? 1.0 : ((diff < 0.0) ? -1.0 : 0.0);
                    pg[k] += g * sgn / (double)m;
                }
                break;
            }
            case TENSOR_OP_RESHAPE: {
                /*  — reindexation identite (ordre plat preserve) :
                 * le gradient recu revient tel quel a la meme position plate du
                 * parent (memes numel). */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                for (int k = 0; k < n; k++) ag[k] += tg[k];
                break;
            }
            case TENSOR_OP_CONCAT_CH: {
                /*  — concatenation canal : le gradient de sortie se
                 * SPLIT par tranche de canaux. Les Ca premiers canaux reviennent
                 * a `a`, les Cb suivants a `b` — chaque branche ne recoit QUE sa
                 * propre tranche (aucun melange). Meme decoupage en blocs
                 * contigus qu'au forward (outer=N, inner=spatial). */
                ObjTensor* a = t->parents[0];
                ObjTensor* b = t->parents[1];
                double* ag = tensor_cells(a->grad);
                double* bg = tensor_cells(b->grad);
                int Ca = a->shape[1], Cb = b->shape[1], Cout = t->shape[1];
                int outer = t->shape[0];
                int inner = 1;
                for (int d = 2; d < t->ndim; d++) inner *= t->shape[d];
                for (int nn = 0; nn < outer; nn++) {
                    for (int c = 0; c < Ca; c++)
                        for (int k = 0; k < inner; k++)
                            ag[(nn * Ca + c) * inner + k] += tg[(nn * Cout + c) * inner + k];
                    for (int c = 0; c < Cb; c++)
                        for (int k = 0; k < inner; k++)
                            bg[(nn * Cb + c) * inner + k] += tg[(nn * Cout + Ca + c) * inner + k];
                }
                break;
            }
            case TENSOR_OP_SLICE_CH: {
                /*  — tranche de canaux : le gradient de sortie revient
                 * DANS la tranche source [cstart, cstart+Cout) du parent, 0
                 * ailleurs (inverse du CONCAT_CH). Meme decoupage en blocs
                 * contigus (outer=N, inner=spatial). */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                int cstart = t->op_axis;
                int Cin = a->shape[1], Cout = t->shape[1];
                int outer = t->shape[0];
                int inner = 1;
                for (int d = 2; d < t->ndim; d++) inner *= t->shape[d];
                for (int nn = 0; nn < outer; nn++) {
                    for (int c = 0; c < Cout; c++)
                        for (int k = 0; k < inner; k++)
                            ag[(nn * Cin + cstart + c) * inner + k] +=
                                tg[(nn * Cout + c) * inner + k];
                }
                break;
            }
            case TENSOR_OP_SELECT: {
                /*  — sous-Tenseur mat[i] (selection axe 0) : le gradient
                 * de sortie (une valeur par element de la tranche) revient dans la
                 * tranche `i` du parent (bloc contigu [i*inner, (i+1)*inner)), 0
                 * ailleurs (motif SLICE_CH). op_axis = index 0-based ; n = numel du
                 * sous-Tenseur = inner. */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                int base = t->op_axis * n;   /* n = taille de la tranche */
                for (int k = 0; k < n; k++) ag[base + k] += tg[k];
                break;
            }
            case TENSOR_OP_PERMUTE: {
                /*  — reordonnancement bijectif : chaque gradient de
                 * sortie revient a la position source d'ou il provenait. On
                 * refait le meme mapping qu'au forward (out -> in via op_perm)
                 * et on accumule dans le parent. */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                int inStr[OWL_TENSOR_MAX_RANK], outStr[OWL_TENSOR_MAX_RANK];
                tensor_contig_strides(a->ndim, a->shape, inStr);
                tensor_contig_strides(t->ndim, t->shape, outStr);
                for (int outFlat = 0; outFlat < n; outFlat++) {
                    int rem = outFlat, inFlat = 0;
                    for (int m = 0; m < t->ndim; m++) {
                        int im = rem / outStr[m];
                        rem %= outStr[m];
                        inFlat += im * inStr[t->op_perm[m]];
                    }
                    ag[inFlat] += tg[outFlat];
                }
                break;
            }
            case TENSOR_OP_SUM_AXIS:
            case TENSOR_OP_MEAN_AXIS: {
                /*  — reduction d'un axe : le gradient recu (une valeur
                 * par position de sortie) se rediffuse sur chaque element de
                 * l'axe reduit qui y a contribue. Moyenne = divise par la taille
                 * de l'axe (chaque contributeur pese 1/taille). */
                ObjTensor* a = t->parents[0];
                double* ag = tensor_cells(a->grad);
                int axis = t->op_axis;
                int axisSize = a->shape[axis];
                double scale = (t->op == TENSOR_OP_MEAN_AXIS)
                             ? 1.0 / (double)axisSize : 1.0;
                int inStr[OWL_TENSOR_MAX_RANK], outStr[OWL_TENSOR_MAX_RANK];
                tensor_contig_strides(a->ndim, a->shape, inStr);
                tensor_contig_strides(t->ndim, t->shape, outStr);
                int numelIn = tensor_numel(a->grad);
                for (int inFlat = 0; inFlat < numelIn; inFlat++) {
                    int rem = inFlat, outFlat = 0, oo = 0;
                    for (int m = 0; m < a->ndim; m++) {
                        int im = rem / inStr[m];
                        rem %= inStr[m];
                        if (m == axis) continue;
                        outFlat += im * outStr[oo];
                        oo++;
                    }
                    ag[inFlat] += tg[outFlat] * scale;
                }
                break;
            }
            case TENSOR_OP_CONV2D: {
                /*  — backward via im2col + owl_matmul. Deux gradients :
                 *   dW   = dOut @ colᵀ   : reduction sur le spatial de TOUT le
                 *          lot en UNE somme courante (t = (b,i,j) croissant) ->
                 *          bit-identique a la boucle directe.
                 *   dCol = Wᵀ @ dOut, par image, puis col2im : co collapse en
                 *          premier -> regime im2col (ig re-baseline).
                 * owl_matmul ECRASE sa sortie -> scratch C-heap + `+=` (patron
                 * MATMUL2D). Buffers = double* bruts, aucune allocation OWL dans
                 * le coeur (isolation VM/GC, ) ; liberes en fin de passe. */
                ObjTensor* in = t->parents[0];
                ObjTensor* w  = t->parents[1];
                int stride = t->conv_stride, pad = t->conv_pad;
                int group  = t->op_axis > 0 ? t->op_axis : 1;  /*  */
                int B   = in->shape[0], Cin = in->shape[1];
                int H   = in->shape[2], Wd  = in->shape[3];
                int Cout = w->shape[0], kH = w->shape[2], kW = w->shape[3];
                int Hout = t->shape[2], Wout = t->shape[3];
                int CinG  = Cin / group;
                int CoutG = Cout / group;
                int KKg = CinG * kH * kW;   /* = w->shape[1]*kH*kW */
                int HW = Hout * Wout;
                int T  = B * HW;
                double* id = tensor_cells(in->data);
                double* wd = tensor_cells(w->data);   /* = W_mat[Cout, KKg] */
                double* ig = tensor_cells(in->grad);
                double* wg = tensor_cells(w->grad);

                /* Groupe par groupe (group=1 -> une iteration, ciBase=0,
                 * CinG=Cin : chemin d'origine bit-identique). Les buffers sont
                 * dimensionnes au groupe et reutilises d'un groupe a l'autre. */
                double* colt = (double*)malloc((size_t)T * KKg * sizeof(double));
                double* dmat = (double*)malloc((size_t)CoutG * T * sizeof(double));
                double* dws  = (double*)malloc((size_t)CoutG * KKg * sizeof(double));
                double* wt   = (double*)malloc((size_t)KKg * CoutG * sizeof(double));
                double* dcol = (double*)malloc((size_t)KKg * HW * sizeof(double));
                for (int g = 0; g < group; g++) {
                    int coBase = g * CoutG;
                    /* --- dW_g[CoutG,KKg] = Dmat[CoutG,T] @ COLt_g[T,KKg] ---
                     * COLt_g deplie les canaux d'entree du groupe ; Dmat regroupe
                     * tg[B,CoutG,HW] du groupe en [co_local, b*HW+s]. */
                    conv_im2col_tk(id, colt, B, Cin, g * CinG, CinG,
                                   H, Wd, kH, kW, Hout, Wout, stride, pad, KKg);
                    for (int b = 0; b < B; b++)
                    for (int co = 0; co < CoutG; co++) {
                        const double* src = tg + ((size_t)b * Cout + coBase + co) * HW;
                        double* dst = dmat + (size_t)co * T + (size_t)b * HW;
                        for (int s = 0; s < HW; s++) dst[s] = src[s];
                    }
                    owl_matmul(dmat, colt, dws, CoutG, T, KKg);
                    double* wg_g = wg + (size_t)coBase * KKg;
                    for (size_t k = 0; k < (size_t)CoutG * KKg; k++) wg_g[k] += dws[k];

                    /* --- dInput_g : Wt_g[KKg,CoutG] = W_g^T ; par image
                     * dCol[KKg,HW] = Wt_g @ dOut_bg, replie dans les canaux
                     * d'entree du groupe par col2im. --- */
                    double* wd_g = wd + (size_t)coBase * KKg;
                    for (int co = 0; co < CoutG; co++)
                        for (int k = 0; k < KKg; k++)
                            wt[(size_t)k * CoutG + co] = wd_g[(size_t)co * KKg + k];
                    for (int b = 0; b < B; b++) {
                        const double* dout_bg = tg + ((size_t)b * Cout + coBase) * HW;
                        owl_matmul(wt, dout_bg, dcol, KKg, CoutG, HW);
                        conv_col2im_add(dcol, ig, b, Cin, g * CinG, CinG,
                                        H, Wd, kH, kW, Hout, Wout, stride, pad);
                    }
                }
                free(colt); free(dmat); free(dws); free(wt); free(dcol);
                break;
            }
            case TENSOR_OP_MAXPOOL: {
                /*  +  — max-pooling : le gradient de chaque
                 * fenetre va UNIQUEMENT a la cellule gagnante (argmax). La position
                 * gagnante est RECALCULEE depuis la donnee du parent (fixe apres le
                 * forward), premier maximum en cas d'egalite (meme regle qu'au
                 * forward). Fenetre de cote `f`, pas `s` : si s < f les fenetres se
                 * chevauchent et une meme cellule peut etre l'argmax de PLUSIEURS
                 * fenetres -> l'accumulation `+=` somme naturellement les gradients
                 * (ne pas ecraser). */
                ObjTensor* in = t->parents[0];
                int f = t->conv_stride;   /* fenetre */
                int s = t->conv_pad;      /* pas */
                int p = t->op_axis;       /* remplissage */
                int B = in->shape[0], C = in->shape[1];
                int H = in->shape[2], Wd = in->shape[3];
                int Hout = t->shape[2], Wout = t->shape[3];
                double* id = tensor_cells(in->data);
                double* ig = tensor_cells(in->grad);
                for (int b = 0; b < B; b++)
                for (int c = 0; c < C; c++)
                for (int i = 0; i < Hout; i++)
                for (int j = 0; j < Wout; j++) {
                    /* argmax sur les seules cellules REELLES (le remplissage vaut
                     * -infini, il ne gagne jamais). bestIh/bestIw restent < 0 si,
                     * par impossible, la fenetre etait entierement virtuelle. */
                    int bestIh = -1, bestIw = -1;
                    double mx = -INFINITY;
                    for (int kh = 0; kh < f; kh++)
                    for (int kw = 0; kw < f; kw++) {
                        int ih = i * s - p + kh, iw = j * s - p + kw;
                        if (ih < 0 || ih >= H || iw < 0 || iw >= Wd) continue;
                        double v = id[((b * C + c) * H + ih) * Wd + iw];
                        if (v > mx) { mx = v; bestIh = ih; bestIw = iw; }
                    }
                    if (bestIh < 0) continue;
                    double go = tg[((b * C + c) * Hout + i) * Wout + j];
                    ig[((b * C + c) * H + bestIh) * Wd + bestIw] += go;
                }
                break;
            }
            case TENSOR_OP_AVGPOOL: {
                /*  +  — average-pooling : le gradient de chaque
                 * fenetre est reparti UNIFORMEMENT (÷ fenetre²) sur toutes ses
                 * cellules. Fenetre de cote `f`, pas `s` : si s < f, les cellules
                 * partagees entre fenetres chevauchantes accumulent (`+=`) leurs
                 * parts. */
                ObjTensor* in = t->parents[0];
                int f = t->conv_stride;   /* fenetre */
                int s = t->conv_pad;      /* pas */
                int p = t->op_axis;       /* remplissage */
                int B = in->shape[0], C = in->shape[1];
                int H = in->shape[2], Wd = in->shape[3];
                int Hout = t->shape[2], Wout = t->shape[3];
                double* ig = tensor_cells(in->grad);
                for (int b = 0; b < B; b++)
                for (int c = 0; c < C; c++)
                for (int i = 0; i < Hout; i++)
                for (int j = 0; j < Wout; j++) {
                    /* count_include_pad=0 : la part se divise par le nombre de
                     * cellules REELLES de la fenetre (meme diviseur qu'au
                     * forward). p=0 -> count = f*f -> bit-identique . */
                    int count = 0;
                    for (int kh = 0; kh < f; kh++)
                    for (int kw = 0; kw < f; kw++) {
                        int ih = i * s - p + kh, iw = j * s - p + kw;
                        if (ih < 0 || ih >= H || iw < 0 || iw >= Wd) continue;
                        count++;
                    }
                    double share = tg[((b * C + c) * Hout + i) * Wout + j]
                                   * (1.0 / (double)count);
                    for (int kh = 0; kh < f; kh++)
                    for (int kw = 0; kw < f; kw++) {
                        int ih = i * s - p + kh, iw = j * s - p + kw;
                        if (ih < 0 || ih >= H || iw < 0 || iw >= Wd) continue;
                        ig[((b * C + c) * H + ih) * Wd + iw] += share;
                    }
                }
                break;
            }
            case TENSOR_OP_BATCHNORM: {
                /*  — retour de la normalisation par canal (partie
                 * x -> xhat = (x - moyenne)/racine(var+eps), le scale/shift
                 * gamma/beta etant porte par des ops MUL/ADD separees). Formule BN
                 * standard, par canal, avec N = B·H·W elements :
                 *   dx_i = rstd * (dxhat_i - moy(dxhat) - xhat_i * moy(dxhat·xhat))
                 * moyenne et variance RECALCULEES depuis la donnee du parent (fixe
                 * apres le forward). Aucune allocation OWL. */
                ObjTensor* x = t->parents[0];
                double eps = t->bn_eps;
                int B = x->shape[0], C = x->shape[1];
                int H = x->shape[2], Wd = x->shape[3];
                int N = B * H * Wd;
                double* xd = tensor_cells(x->data);
                double* xg = tensor_cells(x->grad);
                for (int c = 0; c < C; c++) {
                    double mean = 0.0;
                    for (int b = 0; b < B; b++)
                    for (int i = 0; i < H; i++)
                    for (int j = 0; j < Wd; j++)
                        mean += xd[((b * C + c) * H + i) * Wd + j];
                    mean /= (double)N;
                    double var = 0.0;
                    for (int b = 0; b < B; b++)
                    for (int i = 0; i < H; i++)
                    for (int j = 0; j < Wd; j++) {
                        double d = xd[((b * C + c) * H + i) * Wd + j] - mean;
                        var += d * d;
                    }
                    var /= (double)N;
                    double rstd = 1.0 / sqrt(var + eps);
                    double sumG = 0.0, sumGx = 0.0;
                    for (int b = 0; b < B; b++)
                    for (int i = 0; i < H; i++)
                    for (int j = 0; j < Wd; j++) {
                        int idx = ((b * C + c) * H + i) * Wd + j;
                        double dxh  = tg[idx];
                        double xhat = (xd[idx] - mean) * rstd;
                        sumG  += dxh;
                        sumGx += dxh * xhat;
                    }
                    double mG = sumG / (double)N, mGx = sumGx / (double)N;
                    for (int b = 0; b < B; b++)
                    for (int i = 0; i < H; i++)
                    for (int j = 0; j < Wd; j++) {
                        int idx = ((b * C + c) * H + i) * Wd + j;
                        double dxh  = tg[idx];
                        double xhat = (xd[idx] - mean) * rstd;
                        xg[idx] += rstd * (dxh - mG - xhat * mGx);
                    }
                }
                break;
            }
            case TENSOR_OP_LEAF:
                break;   /* feuille : rien a propager */
        }
    }

    free(topo);
}

/* Methode fluente Retropropage()/Backpropagate() — receveur seul (arite 1). */
static Value fn_tenseur_retropropage(int argCount, Value* args) {
    (void)argCount;
    tensor_backward(AS_TENSOR(args[0]));
    return args[0];   /* rend le receveur (chainage possible) */
}

/* ================================================================
 * Ops de couche differentiables (phase 2)
 *
 * Chaque op = forward (arithmetique directe) + noeud de graphe portant l'op et
 * ses parents. Rooting GC : les parents sont les arguments de la native/methode,
 * rootes sur la pile VM par le dispatch ; obj_new_tensor se root lui-meme
 * pendant l'alloc de sa donnee et de son gradient. Une fois obj_new_tensor
 * revenu, plus aucune allocation OWL (ecriture de doubles + pointeurs parents) :
 * le noeud reste sain jusqu'au retour.
 * ================================================================ */

/* Construit un noeud d'activation (1 parent) en appliquant `op` element par
 * element sur la donnee du parent `p` (suppose rooted par le caller). */
static ObjTensor* tensor_make_unary(TensorOp op, ObjTensor* p) {
    int n = tensor_numel(p->data);
    /* Forme N-D preservee : la sortie a exactement le rang et la forme de
     * l'entree (2D -> Matrice, 4D -> 4D pour un reseau convolutif, ).
     * Le forward et le backward operent sur le buffer plat, dimension-agnostiques. */
    ObjTensor* t = obj_new_tensor_nd(p->ndim, p->shape);
    double* pd = tensor_cells(p->data);
    double* td = tensor_cells(t->data);
    for (int i = 0; i < n; i++) {
        double x = pd[i];
        double y;
        switch (op) {
            case TENSOR_OP_RELU:    y = x > 0.0 ? x : 0.0;      break;
            case TENSOR_OP_SIGMOID: y = 1.0 / (1.0 + exp(-x));  break;
            case TENSOR_OP_TANH:    y = tanh(x);                break;
            default:                y = x;                      break;
        }
        td[i] = y;
    }
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = op;
        t->parents[0] = p;
        t->nParents = 1;
    }
    return t;
}

/* Emet une erreur recuperable « argument pas un Tenseur » et rend NIL. */
static Value tensor_expect_error(const char* nativeName) {
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                          "arg", "operande",
                          "expected", "Tenseur",
                          "actual", "type non pris en charge",
                          NULL);
    return NIL_VAL;
}

/* Verifie que les deux arguments d'une perte sont des Tenseurs de meme forme.
 * Type non-Tenseur ET formes differentes -> soft-error `ErreurDetectee` (idiome
 * standard des natives OWL, comme les autres validations de ce fichier). Rend
 * true si tout est bon, false apres avoir signale (le caller rend NIL_VAL). */
/* Produit matriciel differentiable. En phase 2 le Tenseur reste a donnee plate
 * (Vecteur) : vecteur . vecteur -> Tenseur scalaire (produit scalaire
 * Sum(a[i]*b[i])). Sert la native prefixee `TenseurProduitMatriciel(a, b)` et
 * la methode fluente `.ProduitMatriciel(b)` (receveur = args[0]). */
static Value fn_tenseur_produit_matriciel(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0]) || !IS_TENSOR(args[1])) {
        return tensor_expect_error("TenseurProduitMatriciel");
    }
    ObjTensor* a = AS_TENSOR(args[0]);
    ObjTensor* b = AS_TENSOR(args[1]);

    /* Cas 2D × 1D : poids (Matrice m×n) @ entree (Vecteur n) -> sortie
     * (Vecteur m). Vrai produit matrice-vecteur, differentiable (backward
     * TENSOR_OP_MATVEC). */
    if (tensor_is_2d(a) && !tensor_is_2d(b)) {
        ObjMatrix* Wm = (ObjMatrix*)a->data;
        ObjVector* xv = (ObjVector*)b->data;
        if (xv->count != Wm->cols) {
            char expbuf[160], actbuf[96];
            int en = owl_error_render_lang_is_en();
            snprintf(expbuf, sizeof expbuf, en
                     ? "a vector whose size equals the matrix column count (%d)"
                     : "un vecteur de taille egale au nombre de colonnes de la matrice (%d)",
                     Wm->cols);
            snprintf(actbuf, sizeof actbuf, en
                     ? "a vector of size %d"
                     : "un vecteur de taille %d",
                     xv->count);
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurProduitMatriciel",
                                  "arg", "operande",
                                  "expected", expbuf,
                                  "actual", actbuf,
                                  NULL);
            return NIL_VAL;
        }
        int rows = Wm->rows, cols = Wm->cols;
        ObjTensor* t = obj_new_tensor(rows);   /* sortie 1D */
        double* out = tensor_cells(t->data);
        for (int r = 0; r < rows; r++) {
            double s = 0.0;
            int base = r * cols;
            for (int c = 0; c < cols; c++) s += Wm->data[base + c] * xv->data[c];
            out[r] = s;
        }
        if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
            t->op = TENSOR_OP_MATVEC;
            t->parents[0] = a;
            t->parents[1] = b;
            t->nParents = 2;
        }
        return OBJ_VAL(t);
    }

    /* Cas 2D × 2D : produit matriciel dense A(p×q) @ B(q×r) -> C(p×r).
     * Forward par le kernel rapide `owl_matmul` (tuile + AXPY vectorise).
     * Backward (TENSOR_OP_MATMUL2D) : dA = dSortie @ Bᵀ, dB = Aᵀ @ dSortie. */
    if (tensor_is_2d(a) && tensor_is_2d(b)) {
        ObjMatrix* Am = (ObjMatrix*)a->data;
        ObjMatrix* Bm = (ObjMatrix*)b->data;
        if (Am->cols != Bm->rows) {
            char expbuf[160], actbuf[96];
            int en = owl_error_render_lang_is_en();
            snprintf(expbuf, sizeof expbuf, en
                     ? "B row count equal to A column count (%d)"
                     : "un nombre de lignes de B egal au nombre de colonnes de A (%d)",
                     Am->cols);
            snprintf(actbuf, sizeof actbuf, en
                     ? "matrix B with %d rows"
                     : "une matrice B a %d lignes",
                     Bm->rows);
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurProduitMatriciel",
                                  "arg", "operande",
                                  "expected", expbuf,
                                  "actual", actbuf,
                                  NULL);
            return NIL_VAL;
        }
        int p = Am->rows, q = Am->cols, r = Bm->cols;
        ObjTensor* t = obj_new_tensor_matrix(p, r);   /* sortie 2D */
        /* Plus aucune allocation OWL a partir d'ici. */
        owl_matmul(Am->data, Bm->data, ((ObjMatrix*)t->data)->data, p, q, r);
        if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
            t->op = TENSOR_OP_MATMUL2D;
            t->parents[0] = a;
            t->parents[1] = b;
            t->nParents = 2;
        }
        return OBJ_VAL(t);
    }

    /* Cas 1D × 1D : produit scalaire -> Tenseur scalaire (comportement
     * historique). Une seule operande 2D (l'autre 1D, hors cas Matrice@Vecteur
     * traite plus haut) reste une combinaison non prise en charge. */
    if (tensor_is_2d(a) || tensor_is_2d(b)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurProduitMatriciel",
                              "arg", "operande",
                              "expected", "Matrice @ Matrice, Matrice @ Vecteur ou Vecteur . Vecteur",
                              "actual", "combinaison de formes non prise en charge",
                              NULL);
        return NIL_VAL;
    }
    if (tensor_numel(a->data) != tensor_numel(b->data)) {
        char expbuf[96], actbuf[96];
        int en = owl_error_render_lang_is_en();
        snprintf(expbuf, sizeof expbuf, en
                 ? "two vectors of the same size (%d)"
                 : "deux vecteurs de meme taille (%d)",
                 tensor_numel(a->data));
        snprintf(actbuf, sizeof actbuf, en
                 ? "a second vector of size %d"
                 : "un second vecteur de taille %d",
                 tensor_numel(b->data));
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurProduitMatriciel",
                              "arg", "operande",
                              "expected", expbuf,
                              "actual", actbuf,
                              NULL);
        return NIL_VAL;
    }
    int n = tensor_numel(a->data);
    ObjTensor* t = obj_new_tensor(1);   /* sortie scalaire */
    double* ad = tensor_cells(a->data);
    double* bd = tensor_cells(b->data);
    double s = 0.0;
    for (int i = 0; i < n; i++) s += ad[i] * bd[i];
    tensor_cells(t->data)[0] = s;
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = TENSOR_OP_MATMUL;
        t->parents[0] = a;
        t->parents[1] = b;
        t->nParents = 2;
    }
    return OBJ_VAL(t);
}

/* Activations : native prefixee `Tenseur<Op>(t)` et methode fluente `.<Op>()`
 * (receveur = args[0], arite 1). */
static Value fn_tenseur_relu(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurReLU");
    return OBJ_VAL(tensor_make_unary(TENSOR_OP_RELU, AS_TENSOR(args[0])));
}

static Value fn_tenseur_sigmoide(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurSigmo\xc3\xaf" "de");
    return OBJ_VAL(tensor_make_unary(TENSOR_OP_SIGMOID, AS_TENSOR(args[0])));
}

static Value fn_tenseur_tanh(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurTanh");
    return OBJ_VAL(tensor_make_unary(TENSOR_OP_TANH, AS_TENSOR(args[0])));
}

/* ================================================================
 * Pertes differentiables (phase 3)
 *
 * Deux ops (prediction, cible) -> Tenseur scalaire (1 element), la perte, d'ou
 * part `Retropropage()`. Le noeud a 2 parents mais le backward n'accumule que
 * dans la prediction (parents[0]) : la cible est une verite fixe, son gradient
 * reste a zero. Rooting GC : prediction et cible sont les arguments de la
 * native/methode, rootes sur la pile par le dispatch ; obj_new_tensor se root
 * pendant l'alloc de sa donnee/gradient. Ensuite, plus aucune allocation OWL.
 * ================================================================ */

/* Construit un noeud de perte scalaire (2 parents) portant l'op `op`, avec la
 * valeur scalaire `loss` deja calculee par le forward. */
static ObjTensor* tensor_make_loss(TensorOp op, ObjTensor* pred,
                                   ObjTensor* cible, double loss) {
    ObjTensor* t = obj_new_tensor(1);   /* sortie scalaire */
    tensor_cells(t->data)[0] = loss;
    if (!g_no_grad) {   /*  : feuille sous no-grad (les pertes ne servent
                         * qu'a l'entrainement, jamais arme en no-grad) */
        t->op = op;
        t->parents[0] = pred;
        t->parents[1] = cible;
        t->nParents = 2;
    }
    return t;
}

/* Verifie que les deux arguments sont des Tenseurs de meme taille, emet une
 * erreur recuperable sinon. Rend true si tout est bon. */
static bool tensor_loss_check(Value* args, const char* nativeName) {
    if (!IS_TENSOR(args[0]) || !IS_TENSOR(args[1])) {
        tensor_expect_error(nativeName);
        return false;
    }
    if (tensor_numel(AS_TENSOR(args[0])->data) != tensor_numel(AS_TENSOR(args[1])->data)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "operande",
                              "expected", "Tenseurs de meme taille",
                              "actual", "tailles differentes",
                              NULL);
        return false;
    }
    return true;
}

/* Erreur quadratique moyenne (MSE) : perte = moyenne((pred - cible)^2), un
 * Tenseur scalaire. Native prefixee `TenseurErreurQuadratique(pred, cible)` et
 * methode fluente `.ErreurQuadratique(cible)` (receveur = args[0]). */
static Value fn_tenseur_erreur_quadratique(int argCount, Value* args) {
    (void)argCount;
    if (!tensor_loss_check(args, "TenseurErreurQuadratique")) return NIL_VAL;
    ObjTensor* pred  = AS_TENSOR(args[0]);
    ObjTensor* cible = AS_TENSOR(args[1]);
    int n = tensor_numel(pred->data);
    double* pd = tensor_cells(pred->data);
    double* cd = tensor_cells(cible->data);
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = pd[i] - cd[i];
        s += diff * diff;
    }
    double loss = (n > 0) ? s / (double)n : 0.0;
    return OBJ_VAL(tensor_make_loss(TENSOR_OP_MSE, pred, cible, loss));
}

/* softplus(z) = log(1 + e^z), calcule de facon stable (pas d'overflow, pas de
 * log(0)) : max(z, 0) + log1p(e^-|z|). */
static double tensor_softplus(double z) {
    double az = (z < 0.0) ? -z : z;
    double base = (z > 0.0) ? z : 0.0;
    return base + log1p(exp(-az));
}

/* Entropie croisee binaire avec sigmoide fusionnee : la prediction porte des
 * scores bruts (logits), la cible des valeurs 0/1. Perte = moyenne de
 * cible*softplus(-logit) + (1 - cible)*softplus(logit), un Tenseur scalaire.
 * La fusion evite tout log(0). Native prefixee
 * `TenseurEntropieCroiseeBinaire(pred, cible)` et methode fluente
 * `.EntropieCroiseeBinaire(cible)` (receveur = args[0]). */
static Value fn_tenseur_entropie_croisee_binaire(int argCount, Value* args) {
    (void)argCount;
    if (!tensor_loss_check(args, "TenseurEntropieCrois\xc3\xa9" "eBinaire")) return NIL_VAL;
    ObjTensor* pred  = AS_TENSOR(args[0]);
    ObjTensor* cible = AS_TENSOR(args[1]);
    int n = tensor_numel(pred->data);
    double* pd = tensor_cells(pred->data);
    double* cd = tensor_cells(cible->data);
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        double x = pd[i], y = cd[i];
        s += y * tensor_softplus(-x) + (1.0 - y) * tensor_softplus(x);
    }
    double loss = (n > 0) ? s / (double)n : 0.0;
    return OBJ_VAL(tensor_make_loss(TENSOR_OP_BCE_LOGITS, pred, cible, loss));
}

/* Perte L1 (erreur absolue moyenne) : perte = moyenne(|pred - cible|), un
 * Tenseur scalaire. Native prefixee `TenseurL1(pred, cible)` et methode fluente
 * `.L1(cible)` (receveur = args[0]). */
static Value fn_tenseur_l1(int argCount, Value* args) {
    (void)argCount;
    if (!tensor_loss_check(args, "TenseurL1")) return NIL_VAL;
    ObjTensor* pred  = AS_TENSOR(args[0]);
    ObjTensor* cible = AS_TENSOR(args[1]);
    int n = tensor_numel(pred->data);
    double* pd = tensor_cells(pred->data);
    double* cd = tensor_cells(cible->data);
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = pd[i] - cd[i];
        s += (diff < 0.0) ? -diff : diff;
    }
    double loss = (n > 0) ? s / (double)n : 0.0;
    return OBJ_VAL(tensor_make_loss(TENSOR_OP_L1, pred, cible, loss));
}

/* Entropie croisee avec softmax fusionnee : la prediction porte des scores
 * bruts (logits), softmaxes en interne (stable, soustraction du max) en
 * probabilites, puis perte = -Somme(cible * log(proba)), un Tenseur scalaire.
 * Native prefixee `TenseurEntropieCroisee(pred, cible)` et methode fluente
 * `.EntropieCroisee(cible)` (receveur = args[0]). */
/* Coeur du calcul entropie croisee softmax : lit les logits `pred` et une cible
 * one-hot `cible` (memes numel), calcule la perte scalaire (softmax stable par
 * soustraction du max) et construit le noeud de perte. Partage par la voie
 * one-hot (cible fournie par l'utilisateur) et la voie indices (cible one-hot
 * construite en interne) : un seul chemin -> parite bit-a-bit garantie entre
 * les deux surfaces. `pred` et `cible` doivent etre rootes par le caller
 * (tensor_make_loss alloue un Tenseur scalaire). */
static Value tensor_cross_entropy_core(ObjTensor* pred, ObjTensor* cible) {
    double* pd = tensor_cells(pred->data);
    double* cd = tensor_cells(cible->data);

    /* Cas batch : logits 2D (B lignes × classes). Softmax + entropie croisee
     * par ligne, puis moyenne des B pertes -> un Tenseur scalaire. */
    if (tensor_is_2d(pred)) {
        ObjMatrix* pm = (ObjMatrix*)pred->data;
        int B = pm->rows, K = pm->cols;
        double loss = 0.0;
        for (int r = 0; r < B; r++) {
            int base = r * K;
            double mx = pd[base];
            for (int c = 1; c < K; c++) {
                if (pd[base + c] > mx) mx = pd[base + c];
            }
            double sum = 0.0;
            for (int c = 0; c < K; c++) sum += exp(pd[base + c] - mx);
            for (int c = 0; c < K; c++) {
                double proba = exp(pd[base + c] - mx) / sum;
                loss -= cd[base + c] * log(proba);
            }
        }
        loss = (B > 0) ? loss / (double)B : 0.0;
        return OBJ_VAL(tensor_make_loss(TENSOR_OP_CROSS_ENTROPY, pred, cible, loss));
    }

    int n = tensor_numel(pred->data);
    double mx = (n > 0) ? pd[0] : 0.0;
    for (int i = 1; i < n; i++) {
        if (pd[i] > mx) mx = pd[i];
    }
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += exp(pd[i] - mx);
    double loss = 0.0;
    for (int i = 0; i < n; i++) {
        double proba = exp(pd[i] - mx) / sum;
        loss -= cd[i] * log(proba);
    }
    return OBJ_VAL(tensor_make_loss(TENSOR_OP_CROSS_ENTROPY, pred, cible, loss));
}

static Value fn_tenseur_entropie_croisee(int argCount, Value* args) {
    (void)argCount;
    if (!tensor_loss_check(args, "TenseurEntropieCrois\xc3\xa9" "e")) return NIL_VAL;
    return tensor_cross_entropy_core(AS_TENSOR(args[0]), AS_TENSOR(args[1]));
}

/*  — construit une matrice one-hot 2D B×nbClasses (Tenseur feuille)
 * a partir d'un Tenseur 1D de `B` indices de classe 0-based : la ligne i porte
 * 1.0 a la colonne indices[i], 0 ailleurs. NON differentiable (feuille sans
 * parent) : c'est une cible fixe. Rend NULL sur erreur recuperable deja emise
 * (arg non-Tenseur, rang != 1, nbClasses < 1, indice hors de [0, nbClasses[).
 * Les bornes sont validees AVANT toute allocation. */
static ObjTensor* tensor_onehot_from_indices(Value idxVal, int nbClasses,
                                             const char* nativeName) {
    if (!IS_TENSOR(idxVal)) {
        tensor_expect_error(nativeName);
        return NULL;
    }
    ObjTensor* idx = AS_TENSOR(idxVal);
    if (idx->ndim != 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "indices",
                              "expected", "un Tenseur 1D d'indices de classe",
                              "actual", "rang different de 1", NULL);
        return NULL;
    }
    if (nbClasses < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                              "arg", "nbClasses",
                              "expected", "un nombre de classes strictement positif",
                              "actual", "nombre de classes <= 0", NULL);
        return NULL;
    }
    int B = idx->shape[0];
    double* id = tensor_cells(idx->data);
    for (int i = 0; i < B; i++) {
        long c = lround(id[i]);
        if (c < 0 || c >= nbClasses) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nativeName,
                                  "arg", "indices",
                                  "expected", "des indices de classe dans [0, nbClasses[",
                                  "actual", "indice de classe hors bornes", NULL);
            return NULL;
        }
    }
    ObjTensor* oh = obj_new_tensor_matrix(B, nbClasses);
    double* od = tensor_cells(oh->data);   /* zero-init par obj_new_tensor_matrix */
    for (int i = 0; i < B; i++) {
        long c = lround(id[i]);
        od[(size_t)i * nbClasses + c] = 1.0;
    }
    return oh;
}

/* Encodage one-hot : native prefixee `TenseurUnParmiN(indices, nbClasses)` /
 * `TensorOneHot` et methode fluente `.UnParmiN(nbClasses)` / `.OneHot`
 * (receveur = args[0] = indices, arite 2). */
static Value fn_tenseur_unparmin(int argCount, Value* args) {
    (void)argCount;
    if (!IS_NUMBER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurUnParmiN",
                              "arg", "nbClasses",
                              "expected", "un nombre de classes",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjTensor* oh = tensor_onehot_from_indices(args[0], (int)AS_NUMBER(args[1]),
                                               "TenseurUnParmiN");
    if (oh == NULL) return NIL_VAL;
    return OBJ_VAL(oh);
}

/* Entropie croisee softmax sur des indices de classe (sparse) : logits en
 * args[0] (2D lot×classes, ou 1D classes pour un exemple), Tenseur 1D d'indices
 * cible 0-based en args[1]. La cible one-hot est construite en interne puis le
 * calcul rejoint exactement `tensor_cross_entropy_core` -> parite bit-a-bit
 * (forward ET backward) avec la voie one-hot. Native prefixee
 * `TenseurEntropieCroiseeIndices` / `TensorCrossEntropyIndices` et methode
 * fluente `.EntropieCroiseeIndices(indices)` / `.CrossEntropyIndices`. */
static Value fn_tenseur_entropie_croisee_indices(int argCount, Value* args) {
    (void)argCount;
    const char* nm = "TenseurEntropieCrois\xc3\xa9" "eIndices";
    if (!IS_TENSOR(args[0])) return tensor_expect_error(nm);
    ObjTensor* pred = AS_TENSOR(args[0]);
    /* nbClasses = largeur des logits : nb de colonnes si 2D, nb d'elements
     * si 1D (un seul exemple). */
    int nbClasses = tensor_is_2d(pred) ? pred->shape[1]
                                       : tensor_numel(pred->data);
    ObjTensor* oh = tensor_onehot_from_indices(args[1], nbClasses, nm);
    if (oh == NULL) return NIL_VAL;
    /* Verifie l'accord logits <-> indices : autant d'indices que de lignes de
     * logits (le one-hot a alors le meme numel que les logits). */
    if (tensor_numel(pred->data) != tensor_numel(oh->data)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "indices",
                              "expected", "autant d'indices que d'exemples (lignes) de logits",
                              "actual", "nombre d'indices incompatible avec les logits", NULL);
        return NIL_VAL;
    }
    vm_push(OBJ_VAL(oh));   /* root pendant l'alloc du Tenseur de perte */
    Value r = tensor_cross_entropy_core(pred, oh);
    vm_pop();
    return r;
}

/* ================================================================
 * Descente de gradient (mise a jour de parametres)
 *
 * Rend une FEUILLE FRAICHE (op LEAF, aucun parent) de meme forme que
 * l'operande : nouvelle_valeur = valeur - taux * gradient. Son propre gradient
 * repart a zero, sans lien avec le graphe qui a produit le gradient d'origine
 * (pas d'accumulation entre deux pas). Marche sur un Tenseur 2D (poids) comme
 * sur un Tenseur 1D (biais). Rooting GC : l'operande (args[0]) est roote par le
 * dispatch ; obj_new_tensor* se root pendant l'alloc de sa donnee/gradient.
 * Ensuite plus aucune allocation OWL (lecture/ecriture de doubles).
 * ================================================================ */
static Value fn_tenseur_descente_gradient(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurDescenteGradient");
    if (!IS_NUMBER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurDescenteGradient",
                              "arg", "taux",
                              "expected", "reel",
                              "actual", "type non pris en charge",
                              NULL);
        return NIL_VAL;
    }
    ObjTensor* src  = AS_TENSOR(args[0]);
    double     taux = AS_NUMBER(args[1]);

    /* Feuille de MEME forme N-D que l'operande (poids 2D, biais 1D, mais aussi
     * noyau conv 4D depuis ) : obj_new_tensor_nd preserve le rang. */
    ObjTensor* t = obj_new_tensor_nd(src->ndim, src->shape);
    /* A partir d'ici, plus aucune allocation OWL. */
    int    nel  = tensor_numel(src->data);
    double* out = tensor_cells(t->data);
    double* sd  = tensor_cells(src->data);
    double* sg  = tensor_cells(src->grad);
    for (int i = 0; i < nel; i++) out[i] = sd[i] - taux * sg[i];
    return OBJ_VAL(t);
}

/* ================================================================
 * Ops de forme N-D — reshape / permute / transpose / reductions
 *
 * Chaque op = forward (reindexation ou reduction sur le buffer plat, guidee par
 * shape/strides) + noeud de graphe portant sa regle backward (nouvelle entree
 * du switch de tensor_backward). Rooting GC : le(s) parent(s) sont les
 * arguments, rootes par le dispatch ; obj_new_tensor_nd se root pendant l'alloc
 * de sa donnee/gradient. Ensuite, plus aucune allocation OWL.
 * ================================================================ */

/* Redimensionne : meme donnee, meme ordre plat, nouvelle forme (produit des
 * dimensions inchange). Native `TenseurRedimensionne(t, [dims])` et methode
 * fluente `.Redimensionne([dims])` / `.Reshape([dims])`. */
static Value fn_tenseur_redimensionne(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurRedimensionne");
    ObjTensor* src = AS_TENSOR(args[0]);
    int shape[OWL_TENSOR_MAX_RANK];
    int nd = tensor_read_shape(args[1], shape, "TenseurRedimensionne");
    if (nd < 0) return NIL_VAL;
    int numelNew = 1;
    for (int i = 0; i < nd; i++) numelNew *= shape[i];
    int numelSrc = tensor_numel(src->data);
    if (numelNew != numelSrc) {
        char expbuf[128], actbuf[64];
        snprintf(expbuf, sizeof expbuf,
                 "un produit des dimensions egal au nombre d'elements (%d)",
                 numelSrc);
        snprintf(actbuf, sizeof actbuf, "un produit de %d", numelNew);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurRedimensionne",
                              "arg", "dimensions",
                              "expected", expbuf,
                              "actual", actbuf, NULL);
        return NIL_VAL;
    }
    ObjTensor* t = obj_new_tensor_nd(nd, shape);   /* src roote par le caller */
    /* Plus aucune allocation OWL a partir d'ici. */
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    for (int i = 0; i < numelSrc; i++) td[i] = sd[i];
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = TENSOR_OP_RESHAPE;
        t->parents[0] = src;
        t->nParents = 1;
    }
    return OBJ_VAL(t);
}

/* Construit une permutation d'axes (perm 0-based, deja validee). Sert Permute
 * (ordre complet) et Transpose (echange de 2 axes). */
static Value tensor_permute_build(ObjTensor* src, int nd, const int* perm) {
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int i = 0; i < nd; i++) outShape[i] = src->shape[perm[i]];
    ObjTensor* t = obj_new_tensor_nd(nd, outShape);   /* src roote par le caller */
    /* Plus aucune allocation OWL a partir d'ici. */
    int inStr[OWL_TENSOR_MAX_RANK], outStr[OWL_TENSOR_MAX_RANK];
    tensor_contig_strides(src->ndim, src->shape, inStr);
    tensor_contig_strides(nd, outShape, outStr);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    int numel = tensor_numel(src->data);
    /* Pour chaque position de sortie, retrouver la position source : l'axe de
     * sortie m vient de l'axe source perm[m]. */
    for (int outFlat = 0; outFlat < numel; outFlat++) {
        int rem = outFlat, inFlat = 0;
        for (int m = 0; m < nd; m++) {
            int im = rem / outStr[m];
            rem %= outStr[m];
            inFlat += im * inStr[perm[m]];
        }
        td[outFlat] = sd[inFlat];
    }
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = TENSOR_OP_PERMUTE;
        for (int i = 0; i < nd; i++) t->op_perm[i] = perm[i];
        t->parents[0] = src;
        t->nParents = 1;
    }
    return OBJ_VAL(t);
}

/* Permute : reordonne TOUS les axes selon l'ordre 1-based fourni (une
 * permutation complete). Methode fluente `.Permute([ordre])`. */
static Value fn_tenseur_permute(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("Permute");
    ObjTensor* src = AS_TENSOR(args[0]);
    if (!IS_ARRAY(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Permute",
                              "arg", "ordre",
                              "expected", "tableau ordonnant tous les axes",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjArray* arr = AS_ARRAY(args[1]);
    int nd = arr->elements.count;
    if (nd != src->ndim) {
        char expbuf[96];
        snprintf(expbuf, sizeof expbuf,
                 "un ordre couvrant les %d axes", src->ndim);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Permute",
                              "arg", "ordre",
                              "expected", expbuf,
                              "actual", "nombre d'axes different", NULL);
        return NIL_VAL;
    }
    int perm[OWL_TENSOR_MAX_RANK];
    bool seen[OWL_TENSOR_MAX_RANK] = { false };
    for (int i = 0; i < nd; i++) {
        Value e = arr->elements.values[i];
        if (!IS_NUMBER(e)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Permute",
                                  "arg", "ordre",
                                  "expected", "axes numeriques",
                                  "actual", "axe non numerique", NULL);
            return NIL_VAL;
        }
        int ax = (int)AS_NUMBER(e);
        if (ax < 1 || ax > nd || seen[ax - 1]) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Permute",
                                  "arg", "ordre",
                                  "expected", "une permutation valide des axes (1-based, sans repetition)",
                                  "actual", "permutation invalide", NULL);
            return NIL_VAL;
        }
        seen[ax - 1] = true;
        perm[i] = ax - 1;
    }
    return tensor_permute_build(src, nd, perm);
}

/* Transpose : echange deux axes precis (1-based), generalisation N-D du
 * `.Transpose()` sans argument de la Matrice. Methode fluente
 * `.Transpose(axe1, axe2)`. */
static Value fn_tenseur_transpose(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("Transpose");
    ObjTensor* src = AS_TENSOR(args[0]);
    int nd = src->ndim;
    if (nd < 2) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Transpose",
                              "arg", "operande",
                              "expected", "un Tenseur d'au moins 2 axes",
                              "actual", "Tenseur de rang 1", NULL);
        return NIL_VAL;
    }
    if (!IS_NUMBER(args[1]) || !IS_NUMBER(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Transpose",
                              "arg", "axes",
                              "expected", "deux axes numeriques (1-based)",
                              "actual", "axe non numerique", NULL);
        return NIL_VAL;
    }
    int a1 = (int)AS_NUMBER(args[1]);
    int a2 = (int)AS_NUMBER(args[2]);
    if (a1 < 1 || a1 > nd || a2 < 1 || a2 > nd) {
        char expbuf[96];
        snprintf(expbuf, sizeof expbuf, "deux axes entre 1 et %d", nd);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Transpose",
                              "arg", "axes",
                              "expected", expbuf,
                              "actual", "axe hors bornes", NULL);
        return NIL_VAL;
    }
    int perm[OWL_TENSOR_MAX_RANK];
    for (int i = 0; i < nd; i++) perm[i] = i;
    perm[a1 - 1] = a2 - 1;
    perm[a2 - 1] = a1 - 1;
    return tensor_permute_build(src, nd, perm);
}

/*  — tranche de canaux differentiable : extrait les canaux
 * [debut, debut+longueur) sur l'axe canal (2e axe, 1-based en surface) d'un
 * Tenseur de rang >= 2. Enrobe obj_tensor_slice_ch (, backward =
 * scatter). Indices 1-based comme partout dans l'indexation Tenseur
 * (..Valeur[1], ..Forme[1]) : cstart_0based = debut - 1. Le receveur args[0]
 * est roote par le dispatch (sur la pile VM) pendant l'unique alloc de
 * obj_tensor_slice_ch. */
static Value fn_tenseur_tranchecanaux(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurTrancheCanaux");
    ObjTensor* src = AS_TENSOR(args[0]);
    if (src->ndim < 2) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurTrancheCanaux",
                              "arg", "operande",
                              "expected", "un Tenseur d'au moins 2 axes (avec axe canal)",
                              "actual", "Tenseur de rang 1", NULL);
        return NIL_VAL;
    }
    if (!IS_NUMBER(args[1]) || !IS_NUMBER(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurTrancheCanaux",
                              "arg", "debut/longueur",
                              "expected", "un debut et une longueur numeriques (1-based)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int debut    = (int)AS_NUMBER(args[1]);
    int longueur = (int)AS_NUMBER(args[2]);
    int Cin = src->shape[1];
    if (debut < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurTrancheCanaux",
                              "arg", "debut",
                              "expected", "un debut >= 1 (1-based)",
                              "actual", "debut hors plage", NULL);
        return NIL_VAL;
    }
    if (longueur < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurTrancheCanaux",
                              "arg", "longueur",
                              "expected", "une longueur >= 1",
                              "actual", "longueur nulle ou negative", NULL);
        return NIL_VAL;
    }
    if ((debut - 1) + longueur > Cin) {
        char expbuf[96];
        snprintf(expbuf, sizeof expbuf,
                 "une plage tenant dans les %d canaux (debut + longueur - 1 <= %d)",
                 Cin, Cin);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurTrancheCanaux",
                              "arg", "debut/longueur",
                              "expected", expbuf,
                              "actual", "plage hors des canaux", NULL);
        return NIL_VAL;
    }
    ObjTensor* t = obj_tensor_slice_ch(src, debut - 1, longueur);
    if (t == NULL) {   /* backstop : bornes deja validees ci-dessus */
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurTrancheCanaux",
                              "arg", "debut/longueur",
                              "expected", "une plage de canaux valide",
                              "actual", "plage invalide", NULL);
        return NIL_VAL;
    }
    return OBJ_VAL(t);
}

/* Reduction d'un axe (somme ou moyenne), abaissant le rang de 1. */
static Value tensor_reduce_build(ObjTensor* src, int axis0, bool mean) {
    int nd = src->ndim;
    int outShape[OWL_TENSOR_MAX_RANK];
    int outNdim = 0;
    for (int m = 0; m < nd; m++) {
        if (m == axis0) continue;
        outShape[outNdim++] = src->shape[m];
    }
    /* Reduire un rang 1 donnerait un rang 0 : on rend un scalaire (rang 1,
     * taille 1) — coherent avec le reste de la surface (pas de rang 0). */
    if (outNdim < 1) { outNdim = 1; outShape[0] = 1; }

    int axisSize = src->shape[axis0];
    ObjTensor* t = obj_new_tensor_nd(outNdim, outShape);   /* zero-init */
    /* Plus aucune allocation OWL a partir d'ici. */
    int inStr[OWL_TENSOR_MAX_RANK], outStr[OWL_TENSOR_MAX_RANK];
    tensor_contig_strides(nd, src->shape, inStr);
    tensor_contig_strides(outNdim, outShape, outStr);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    int numelIn = tensor_numel(src->data);
    for (int inFlat = 0; inFlat < numelIn; inFlat++) {
        int rem = inFlat, outFlat = 0, oo = 0;
        for (int m = 0; m < nd; m++) {
            int im = rem / inStr[m];
            rem %= inStr[m];
            if (m == axis0) continue;
            outFlat += im * outStr[oo];
            oo++;
        }
        td[outFlat] += sd[inFlat];
    }
    if (mean) {
        int numelOut = tensor_numel(t->data);
        for (int k = 0; k < numelOut; k++) td[k] /= (double)axisSize;
    }
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = mean ? TENSOR_OP_MEAN_AXIS : TENSOR_OP_SUM_AXIS;
        t->op_axis = axis0;
        t->parents[0] = src;
        t->nParents = 1;
    }
    return OBJ_VAL(t);
}

/* Verifie/normalise un axe 1-based en 0-based, emet une erreur sinon. Rend -1
 * sur erreur. */
static int tensor_check_axis(ObjTensor* src, Value axeVal, const char* nm) {
    if (!IS_NUMBER(axeVal)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "axe",
                              "expected", "un axe numerique (1-based)",
                              "actual", "type non pris en charge", NULL);
        return -1;
    }
    int axe = (int)AS_NUMBER(axeVal);
    if (axe < 1 || axe > src->ndim) {
        char expbuf[96];
        snprintf(expbuf, sizeof expbuf, "un axe entre 1 et %d", src->ndim);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "axe",
                              "expected", expbuf,
                              "actual", "axe hors bornes", NULL);
        return -1;
    }
    return axe - 1;
}

/* Somme par axe : native `TenseurSomme(t, axe)` et methode `.Somme(axe)`/`.Sum(axis)`. */
static Value fn_tenseur_somme(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurSomme");
    ObjTensor* src = AS_TENSOR(args[0]);
    int axis0 = tensor_check_axis(src, args[1], "TenseurSomme");
    if (axis0 < 0) return NIL_VAL;
    return tensor_reduce_build(src, axis0, false);
}

/* Moyenne par axe : native `TenseurMoyenne(t, axe)` et methode
 * `.Moyenne(axe)`/`.Mean(axis)`. */
static Value fn_tenseur_moyenne(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurMoyenne");
    ObjTensor* src = AS_TENSOR(args[0]);
    int axis0 = tensor_check_axis(src, args[1], "TenseurMoyenne");
    if (axis0 < 0) return NIL_VAL;
    return tensor_reduce_build(src, axis0, true);
}

/* ================================================================
 * Ops de convolution — Conv2D / Pooling / BatchNorm
 *
 * Toutes operent sur des Tenseurs 4D B×C×H×W (lot × canaux × hauteur ×
 * largeur). Le forward de Conv2D passe par im2col + owl_matmul (cf. la section
 * im2col plus haut) ; pooling et BatchNorm restent en boucles directes.
 * Rooting GC : parents = arguments rootes par le dispatch ;
 * obj_new_tensor_nd se root pendant l'alloc de sa donnee/gradient ; au-dela,
 * plus aucune allocation OWL (sauf la BatchNorm, qui chaine plusieurs noeuds
 * et roote chaque intermediaire).
 * ================================================================ */

/* Verifie qu'un argument est un Tenseur 4D, emet une erreur sinon. */
static bool tensor_expect_4d(ObjTensor* t, const char* nm, const char* which) {
    if (t->ndim != 4) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", which,
                              "expected", "un Tenseur 4D (lot x canaux x hauteur x largeur)",
                              "actual", "rang different de 4", NULL);
        return false;
    }
    return true;
}

/* Convolution 2D (correlation croisee) : entree B×Cin×H×W, poids
 * Cout×(Cin/group)×kH×kW -> sortie B×Cout×Hout×Wout, avec pas, remplissage et
 * un nombre de groupes optionnel (, defaut 1). Native
 * `TenseurConvolution2D(entree, poids, pas, remplissage[, group])` et methode
 * fluente `.Convolution2D(poids, pas, remplissage[, group])` (receveur =
 * args[0]).
 *
 * Convolution groupee : les canaux d'entree ET de sortie sont partitionnes en
 * `group` groupes independants. Le groupe g ne connecte les canaux de sortie
 * [g*Cout/group, (g+1)*Cout/group[ qu'aux canaux d'entree
 * [g*Cin/group, (g+1)*Cin/group[. Chaque filtre porte Cin/group canaux
 * d'entree (2e dim des poids). Depthwise = group == Cin (un filtre par canal).
 * group=1 -> une seule iteration, ciBase=0, CinG=Cin : chemin bit-identique a
 * l'origine (aucune regression sur la convolution non groupee). */
static Value fn_tenseur_convolution2d(int argCount, Value* args) {
    if (!IS_TENSOR(args[0]) || !IS_TENSOR(args[1]))
        return tensor_expect_error("TenseurConvolution2D");
    if (!IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurConvolution2D",
                              "arg", "pas/remplissage",
                              "expected", "pas et remplissage entiers",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjTensor* in = AS_TENSOR(args[0]);
    ObjTensor* w  = AS_TENSOR(args[1]);
    int stride = (int)AS_NUMBER(args[2]);
    int pad    = (int)AS_NUMBER(args[3]);
    int group  = 1;
    if (argCount >= 5) {
        if (!IS_NUMBER(args[4])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurConvolution2D",
                                  "arg", "group",
                                  "expected", "un nombre de groupes entier",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        group = (int)AS_NUMBER(args[4]);
    }
    if (!tensor_expect_4d(in, "TenseurConvolution2D", "entree")) return NIL_VAL;
    if (!tensor_expect_4d(w,  "TenseurConvolution2D", "poids"))  return NIL_VAL;
    if (stride < 1 || pad < 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurConvolution2D",
                              "arg", "pas/remplissage",
                              "expected", "pas >= 1 et remplissage >= 0",
                              "actual", "valeur hors bornes", NULL);
        return NIL_VAL;
    }
    int B = in->shape[0], Cin = in->shape[1], H = in->shape[2], Wd = in->shape[3];
    int Cout = w->shape[0], wCin = w->shape[1], kH = w->shape[2], kW = w->shape[3];
    if (group < 1 || Cin % group != 0 || Cout % group != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurConvolution2D",
                              "arg", "group",
                              "expected", "un nombre de groupes >= 1 divisant les canaux d'entree ET de sortie",
                              "actual", "group ne divise pas les canaux", NULL);
        return NIL_VAL;
    }
    int CinG  = Cin / group;    /* canaux d'entree par groupe (= 2e dim des poids) */
    int CoutG = Cout / group;   /* canaux de sortie par groupe */
    if (wCin != CinG) {
        char expbuf[160];
        snprintf(expbuf, sizeof expbuf,
                 "des poids dont le nombre de canaux d'entree egale celui de l'entree divise par group (%d)",
                 CinG);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurConvolution2D",
                              "arg", "poids",
                              "expected", expbuf,
                              "actual", "nombre de canaux d'entree different", NULL);
        return NIL_VAL;
    }
    int Hout = (H + 2 * pad - kH) / stride + 1;
    int Wout = (Wd + 2 * pad - kW) / stride + 1;
    if (Hout < 1 || Wout < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurConvolution2D",
                              "arg", "poids",
                              "expected", "un noyau (avec remplissage) qui tient dans l'entree",
                              "actual", "taille de sortie nulle ou negative", NULL);
        return NIL_VAL;
    }
    int outShape[4] = { B, Cout, Hout, Wout };
    ObjTensor* t = obj_new_tensor_nd(4, outShape);   /* in/w rootes par le caller */
    /* Plus aucune allocation OWL a partir d'ici (buffer col = double* brut). */
    double* id = tensor_cells(in->data);
    double* wd = tensor_cells(w->data);   /* = W_mat[Cout, KKg], KKg = CinG*kH*kW */
    double* od = tensor_cells(t->data);
    int KKg = CinG * kH * kW;
    int HW = Hout * Wout;
    /* Par (image, groupe) : out_bg[CoutG,HW] = W_g[CoutG,KKg] @ col_bg[KKg,HW].
     * Les filtres du groupe g (lignes [g*CoutG,(g+1)*CoutG[ de W) ne lisent que
     * les canaux d'entree [g*CinG,(g+1)*CinG[. col reutilise d'une iteration a
     * l'autre (une seule alloc). owl_matmul (threade, non-FMA, somme courante en
     * k croissant) ecrit directement la tranche du groupe. */
    double* col = (double*)malloc((size_t)KKg * HW * sizeof(double));
    for (int b = 0; b < B; b++) {
        for (int g = 0; g < group; g++) {
            conv_im2col_khw(id, col, b, Cin, g * CinG, CinG,
                            H, Wd, kH, kW, Hout, Wout, stride, pad);
            owl_matmul(wd + (size_t)(g * CoutG) * KKg, col,
                       od + (size_t)b * Cout * HW + (size_t)(g * CoutG) * HW,
                       CoutG, KKg, HW);
        }
    }
    free(col);
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = TENSOR_OP_CONV2D;
        t->parents[0] = in;
        t->parents[1] = w;
        t->nParents = 2;
        t->conv_stride = stride;
        t->conv_pad = pad;
        t->op_axis = group;   /*  : group porte par op_axis (libre pour CONV2D) */
    }
    return OBJ_VAL(t);
}

/* Fabrique commune du pooling (max ou moyenne) : fenetre carree de cote `f`,
 * pas `s` optionnel (defaut = f -> fenetres non chevauchantes), remplissage `p`
 * optionnel (defaut 0). Un pas < f produit des fenetres chevauchantes ; un
 * remplissage > 0 ceinture l'entree de p cellules VIRTUELLES sur chaque bord
 * (jamais materialisees). Entree 4D -> sortie
 * B×C×(floor((H+2p-f)/s)+1)×(floor((W+2p-f)/s)+1).
 *   - Max : les cellules de remplissage valent -infini (elles ne gagnent jamais
 *     le max) ; une fenetre entierement dans le remplissage n'existe pas (la
 *     taille de sortie garantit au moins une cellule reelle par fenetre).
 *   - Moyenne : convention ONNX count_include_pad=0 -> la moyenne divise par le
 *     nombre de cellules REELLES de la fenetre (pas par f² quand elle deborde).
 * Le remplissage est stocke dans t->op_axis (champ libre pour les ops pooling ;
 * conv_stride porte la fenetre, conv_pad porte le pas — cf. object.h). */
static Value tensor_pooling(Value* args, int argCount, bool isMax,
                            const char* nm, TensorOp op) {
    if (!IS_TENSOR(args[0])) return tensor_expect_error(nm);
    if (!IS_NUMBER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "taille",
                              "expected", "taille de fenetre entiere",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjTensor* in = AS_TENSOR(args[0]);
    int f = (int)AS_NUMBER(args[1]);
    if (!tensor_expect_4d(in, nm, "entree")) return NIL_VAL;
    if (f < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "taille",
                              "expected", "une taille de fenetre >= 1",
                              "actual", "taille nulle ou negative", NULL);
        return NIL_VAL;
    }
    int s = f;   /* pas par defaut = fenetre (comportement non chevauchant) */
    if (argCount >= 3) {
        if (!IS_NUMBER(args[2])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "pas",
                                  "expected", "un pas entier",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        s = (int)AS_NUMBER(args[2]);
        if (s < 1) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "pas",
                                  "expected", "un pas >= 1",
                                  "actual", "pas nul ou negatif", NULL);
            return NIL_VAL;
        }
    }
    int p = 0;   /* remplissage par defaut = 0 (non chevauchant, sans padding) */
    if (argCount >= 4) {
        if (!IS_NUMBER(args[3])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "remplissage",
                                  "expected", "un remplissage entier",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        p = (int)AS_NUMBER(args[3]);
        if (p < 0) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "remplissage",
                                  "expected", "un remplissage >= 0",
                                  "actual", "remplissage negatif", NULL);
            return NIL_VAL;
        }
    }
    int B = in->shape[0], C = in->shape[1], H = in->shape[2], Wd = in->shape[3];
    int Hout = (H + 2 * p - f) / s + 1, Wout = (Wd + 2 * p - f) / s + 1;
    if (Hout < 1 || Wout < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "taille",
                              "expected", "une fenetre (avec remplissage) qui tient dans l'entree",
                              "actual", "taille de sortie nulle", NULL);
        return NIL_VAL;
    }
    /* Un remplissage >= fenetre ferait qu'une fenetre de bord serait entierement
     * virtuelle (aucune cellule reelle) : interdit (ONNX l'interdit aussi). */
    if (p >= f) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "remplissage",
                              "expected", "un remplissage strictement inferieur a la fenetre",
                              "actual", "remplissage >= fenetre", NULL);
        return NIL_VAL;
    }
    int outShape[4] = { B, C, Hout, Wout };
    ObjTensor* t = obj_new_tensor_nd(4, outShape);   /* in roote par le caller */
    /* Plus aucune allocation OWL a partir d'ici. */
    double* id = tensor_cells(in->data);
    double* od = tensor_cells(t->data);
    for (int b = 0; b < B; b++)
    for (int c = 0; c < C; c++)
    for (int i = 0; i < Hout; i++)
    for (int j = 0; j < Wout; j++) {
        double acc;
        if (isMax) {
            /* Remplissage = -infini : seules les cellules reelles concourent.
             * p=0 -> toutes les cellules sont reelles, la premiere fixe acc
             * (max exact, bit-identique a ). */
            acc = -INFINITY;
            for (int kh = 0; kh < f; kh++)
            for (int kw = 0; kw < f; kw++) {
                int ih = i * s - p + kh, iw = j * s - p + kw;
                if (ih < 0 || ih >= H || iw < 0 || iw >= Wd) continue;
                double v = id[((b * C + c) * H + ih) * Wd + iw];
                if (v > acc) acc = v;
            }
        } else {
            /* Moyenne count_include_pad=0 : somme des cellules reelles / leur
             * nombre. p=0 -> count = f*f, 1.0/count == 1.0/(f*f) (invWin de
             * ) -> produit bit-identique. */
            double sum = 0.0;
            int count = 0;
            for (int kh = 0; kh < f; kh++)
            for (int kw = 0; kw < f; kw++) {
                int ih = i * s - p + kh, iw = j * s - p + kw;
                if (ih < 0 || ih >= H || iw < 0 || iw >= Wd) continue;
                sum += id[((b * C + c) * H + ih) * Wd + iw];
                count++;
            }
            acc = sum * (1.0 / (double)count);
        }
        od[((b * C + c) * Hout + i) * Wout + j] = acc;
    }
    if (!g_no_grad) {   /*  : feuille sous no-grad (inference) */
        t->op = op;
        t->parents[0] = in;
        t->nParents = 1;
        t->conv_stride = f;   /* fenetre */
        t->conv_pad    = s;   /* pas */
        t->op_axis     = p;   /* remplissage (champ libre pour le pooling) */
    }
    return OBJ_VAL(t);
}

/* Max-pooling : native `TenseurMaxPooling(entree, taille)` et methode
 * `.MaxPooling(taille)`. */
static Value fn_tenseur_maxpooling(int argCount, Value* args) {
    return tensor_pooling(args, argCount, true, "TenseurMaxPooling", TENSOR_OP_MAXPOOL);
}

/* Average-pooling : native `TenseurMoyennePooling(entree, taille)` et methode
 * `.MoyennePooling(taille)` / `.AveragePooling(taille)`. */
static Value fn_tenseur_moyennepooling(int argCount, Value* args) {
    return tensor_pooling(args, argCount, false, "TenseurMoyennePooling", TENSOR_OP_AVGPOOL);
}

/* Normalisation par lot : entree 4D B×C×H×W, gamma/beta de taille C, epsilon.
 * Chaque canal est normalise (moyenne/variance sur B,H,W) puis mis a l'echelle
 * (gamma) et decale (beta) :
 *   sortie = gamma * (x - moyenne)/racine(var+eps) + beta.
 *
 * Assemblage en GRAPHE plutot qu'en une op monolithique a 3 parents : un noeud
 * de normalisation (x -> xhat, 1 parent, backward = formule BN) puis, en
 * reutilisant les briques existantes, gamma/beta redimensionnes en [1,C,1,1]
 * (broadcast par canal correct quel que soit C) puis appliques par MUL et ADD.
 * Les gradients de gamma, beta ET entree tombent alors des regles backward deja
 * livrees (reduce-sum broadcast, reshape, batchnorm). Rooting : chaque
 * intermediaire est empile pendant la construction du suivant. */
ObjTensor* tensor_batchnorm_forward(ObjTensor* x, ObjTensor* gamma, ObjTensor* beta,
                                    double eps, ObjTensor* runMean, ObjTensor* runVar,
                                    double momentum, bool eval) {
    if (!tensor_expect_4d(x, "TenseurNormalisationLot", "entree")) return NULL;
    int B = x->shape[0], C = x->shape[1], H = x->shape[2], Wd = x->shape[3];
    if (tensor_numel(gamma->data) != C || tensor_numel(beta->data) != C) {
        char expbuf[128];
        snprintf(expbuf, sizeof expbuf,
                 "gamma et beta de taille egale au nombre de canaux (%d)", C);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurNormalisationLot",
                              "arg", "gamma/beta",
                              "expected", expbuf,
                              "actual", "taille differente du nombre de canaux", NULL);
        return NULL;
    }

    /* 1) Noeud de normalisation x -> xhat (par canal, sur B,H,W).
     * En mode eval avec stats mobiles : normalise avec runMean/runVar (constants
     * du lot), donc xhat est une FEUILLE (aucun backward — l'inference ne
     * retropropage pas). Sinon : stats du LOT + noeud TENSOR_OP_BATCHNORM
     * differentiable, et si runMean present (couche en entrainement) mise a jour
     * EMA des stats mobiles. */
    ObjTensor* xhat = obj_new_tensor_nd(4, x->shape);   /* x roote par le caller */
    vm_push(OBJ_VAL(xhat));
    {
        int N = B * H * Wd;
        double* xd  = tensor_cells(x->data);
        double* xhd = tensor_cells(xhat->data);
        double* rmd = (runMean != NULL) ? tensor_cells(runMean->data) : NULL;
        double* rvd = (runVar  != NULL) ? tensor_cells(runVar->data)  : NULL;
        for (int c = 0; c < C; c++) {
            double mean, var;
            if (eval && rmd != NULL) {
                /* Inference : stats mobiles figees (pas de reduction sur le lot). */
                mean = rmd[c];
                var  = rvd[c];
            } else {
                mean = 0.0;
                for (int b = 0; b < B; b++)
                for (int i = 0; i < H; i++)
                for (int j = 0; j < Wd; j++)
                    mean += xd[((b * C + c) * H + i) * Wd + j];
                mean /= (double)N;
                var = 0.0;
                for (int b = 0; b < B; b++)
                for (int i = 0; i < H; i++)
                for (int j = 0; j < Wd; j++) {
                    double d = xd[((b * C + c) * H + i) * Wd + j] - mean;
                    var += d * d;
                }
                var /= (double)N;
                /* EMA des stats mobiles : run = (1-m)*run + m*lot. Variance de
                 * population (÷ N), coherente avec la normalisation appliquee. */
                if (rmd != NULL) {
                    rmd[c] = (1.0 - momentum) * rmd[c] + momentum * mean;
                    rvd[c] = (1.0 - momentum) * rvd[c] + momentum * var;
                }
            }
            double rstd = 1.0 / sqrt(var + eps);
            for (int b = 0; b < B; b++)
            for (int i = 0; i < H; i++)
            for (int j = 0; j < Wd; j++) {
                int idx = ((b * C + c) * H + i) * Wd + j;
                xhd[idx] = (xd[idx] - mean) * rstd;
            }
        }
        if (!(eval && rmd != NULL) && !g_no_grad) {   /*  : feuille sous no-grad */
            xhat->op = TENSOR_OP_BATCHNORM;
            xhat->parents[0] = x;
            xhat->nParents = 1;
            xhat->bn_eps = eps;
        }
    }

    /* 2) gamma/beta -> [1,C,1,1] (broadcast par canal). */
    int rsh[4] = { 1, C, 1, 1 };
    ObjTensor* gr = obj_tensor_reshape(gamma, 4, rsh);
    vm_push(OBJ_VAL(gr));
    ObjTensor* br = obj_tensor_reshape(beta, 4, rsh);
    vm_push(OBJ_VAL(br));

    /* 3) sortie = xhat * gamma + beta (broadcast MUL puis ADD). */
    ObjTensor* scaled = obj_tensor_binop(TENSOR_OP_MUL, xhat, gr);
    if (scaled == NULL) { vm_pop(); vm_pop(); vm_pop(); return NULL; }
    vm_push(OBJ_VAL(scaled));
    ObjTensor* out = obj_tensor_binop(TENSOR_OP_ADD, scaled, br);
    vm_pop();   /* scaled */
    vm_pop();   /* br */
    vm_pop();   /* gr */
    vm_pop();   /* xhat */
    return out;
}

static Value fn_tenseur_normalisationlot(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TENSOR(args[0]) || !IS_TENSOR(args[1]) || !IS_TENSOR(args[2]))
        return tensor_expect_error("TenseurNormalisationLot");
    if (!IS_NUMBER(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurNormalisationLot",
                              "arg", "epsilon",
                              "expected", "epsilon reel",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    /* Native publique : stats du LOT, sans stats mobiles (comportement
     * historique). Le mode eval + running-stats est reserve au forward de la
     * couche BatchNorm d'un IAModèle. */
    ObjTensor* out = tensor_batchnorm_forward(AS_TENSOR(args[0]), AS_TENSOR(args[1]),
                                              AS_TENSOR(args[2]), AS_NUMBER(args[3]),
                                              NULL, NULL, 0.0, false);
    return (out == NULL) ? NIL_VAL : OBJ_VAL(out);
}

/* Standardisation z-score par canal : pretraitement d'un Tenseur
 * image 4D B×C×H×W. Chaque canal est centre-reduit sur les axes lot/hauteur/
 * largeur (B,H,W) : sortie = (x - moyenne_canal) / racine(variance_canal + eps),
 * variance de population (÷ N = B·H·W). L'epsilon (optionnel, defaut 1e-5) evite
 * la division par zero sur un canal constant.
 *
 * Contrairement a la NormalisationLot (couche entrainable, differentiable),
 * c'est un PRETRAITEMENT fige : la sortie est une FEUILLE du graphe (aucun
 * parent, aucun gradient ne remonte). Native `TenseurStandardise(entree,
 * [epsilon])` et methode fluente `.Standardise([epsilon])`. */
#define TENSOR_STANDARDISE_EPS_DEFAUT 1e-5
static Value fn_tenseur_standardise(int argCount, Value* args) {
    if (!IS_TENSOR(args[0])) return tensor_expect_error("TenseurStandardise");
    double eps = TENSOR_STANDARDISE_EPS_DEFAUT;
    if (argCount >= 2) {
        if (!IS_NUMBER(args[1])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TenseurStandardise",
                                  "arg", "epsilon",
                                  "expected", "epsilon reel",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        eps = AS_NUMBER(args[1]);
    }
    ObjTensor* x = AS_TENSOR(args[0]);
    if (!tensor_expect_4d(x, "TenseurStandardise", "entree")) return NIL_VAL;
    int B = x->shape[0], C = x->shape[1], H = x->shape[2], Wd = x->shape[3];

    ObjTensor* out = obj_new_tensor_nd(4, x->shape);   /* x roote par le caller */
    /* Plus aucune allocation OWL a partir d'ici : feuille (op LEAF, sans parent). */
    int N = B * H * Wd;
    double* xd = tensor_cells(x->data);
    double* od = tensor_cells(out->data);
    for (int c = 0; c < C; c++) {
        double mean = 0.0;
        for (int b = 0; b < B; b++)
        for (int i = 0; i < H; i++)
        for (int j = 0; j < Wd; j++)
            mean += xd[((b * C + c) * H + i) * Wd + j];
        mean /= (double)N;
        double var = 0.0;
        for (int b = 0; b < B; b++)
        for (int i = 0; i < H; i++)
        for (int j = 0; j < Wd; j++) {
            double d = xd[((b * C + c) * H + i) * Wd + j] - mean;
            var += d * d;
        }
        var /= (double)N;
        double rstd = 1.0 / sqrt(var + eps);
        for (int b = 0; b < B; b++)
        for (int i = 0; i < H; i++)
        for (int j = 0; j < Wd; j++) {
            int idx = ((b * C + c) * H + i) * Wd + j;
            od[idx] = (xd[idx] - mean) * rstd;
        }
    }
    return OBJ_VAL(out);   /* feuille : out->op reste TENSOR_OP_LEAF, nParents = 0 */
}

/* ================================================================
 * Metriques d'evaluation de classification
 *
 * Comptage pur, NON differentiable (pas de graphe, pas de gradient) : ce sont
 * des mesures de qualite calculees APRES l'entrainement, distinctes des pertes
 * (TenseurErreurQuadratique / TenseurEntropieCroisee). Entrees acceptees :
 * Tenseur (1D/2D), Matrice ou Vecteur, tous adosses a un buffer plat contigu.
 * Aucune allocation OWL cote comptage (buffers de classes en tas C, liberes
 * avant retour) ; la seule allocation OWL est la Matrice de confusion rendue.
 * ================================================================ */

/* Vue plate sur des donnees numeriques : rows = nombre d'exemples (B) ;
 * cols = 0 pour une entree 1D (indices de classe), sinon nombre de classes
 * (scores par ligne). */
typedef struct { double* cells; int rows; int cols; } MetricView;

static bool metric_read_view(Value v, MetricView* mv,
                             const char* nm, const char* which) {
    if (IS_TENSOR(v)) {
        ObjTensor* t = AS_TENSOR(v);
        if (t->ndim == 1) {
            mv->cells = tensor_cells(t->data); mv->rows = t->shape[0]; mv->cols = 0;
            return true;
        }
        if (t->ndim == 2) {
            mv->cells = tensor_cells(t->data);
            mv->rows = t->shape[0]; mv->cols = t->shape[1];
            return true;
        }
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", which,
                              "expected", "un Tenseur 1D (indices) ou 2D (scores par classe)",
                              "actual", "rang superieur a 2", NULL);
        return false;
    }
    if (IS_MATRIX(v)) {
        ObjMatrix* m = AS_MATRIX(v);
        mv->cells = m->data; mv->rows = m->rows; mv->cols = m->cols;
        return true;
    }
    if (IS_VECTOR(v)) {
        ObjVector* vec = AS_VECTOR(v);
        mv->cells = vec->data; mv->rows = vec->count; mv->cols = 0;
        return true;
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                          "arg", which,
                          "expected", "un Tenseur, une Matrice ou un Vecteur",
                          "actual", "type non pris en charge", NULL);
    return false;
}

/* Rend un tableau (tas C) des `mv->rows` classes 0-based : lecture directe
 * (1D) ou ArgMax par ligne (2D, premiere occurrence en cas d'ex-aequo, comme
 * `Vecteur..ArgMax`). NULL sur erreur (deja emise) ; le caller libere. */
static int* metric_classes(const MetricView* mv, const char* nm,
                           const char* which) {
    int B = mv->rows;
    int* cls = (int*)malloc(sizeof(int) * (B > 0 ? (size_t)B : 1));
    /* cols <= 1 : vue lue a plat via cells[i] (pour cols==1 le stride est 1,
     * donc cells[i] est bien la i-eme valeur). Une colonne N×1 d'indices EST
     * un vecteur 1D de N indices ; pas d'argmax sur une seule colonne. */
    if (mv->cols <= 1) {
        for (int i = 0; i < B; i++) {
            long c = lround(mv->cells[i]);
            if (c < 0) {
                free(cls);
                owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                      "arg", which,
                                      "expected", "des indices de classe positifs ou nuls",
                                      "actual", "indice de classe negatif", NULL);
                return NULL;
            }
            cls[i] = (int)c;
        }
    } else {
        int C = mv->cols;
        for (int i = 0; i < B; i++) {
            const double* row = mv->cells + (size_t)i * C;
            int best = 0;   /* `>` strict : la premiere occurrence gagne */
            for (int j = 1; j < C; j++) if (row[j] > row[best]) best = j;
            cls[i] = best;
        }
    }
    return cls;
}

/* Facteur commun : lit predictions + cibles, valide (cibles 1D, memes
 * effectifs, lot non vide) et rend les deux tableaux de classes. false sur
 * erreur (deja emise ; tout buffer alloue est libere). */
static bool metric_pair_classes(Value* args, const char* nm,
                                 int** predOut, int** trueOut, int* bOut) {
    MetricView pv, cv;
    if (!metric_read_view(args[0], &pv, nm, "predictions")) return false;
    if (!metric_read_view(args[1], &cv, nm, "cibles")) return false;
    if (cv.cols > 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "cibles",
                              "expected", "des indices de classe 1D (un par exemple)",
                              "actual", "cibles a deux dimensions", NULL);
        return false;
    }
    if (pv.rows != cv.rows) {
        char actbuf[96];
        snprintf(actbuf, sizeof actbuf,
                 "%d predictions face a %d cibles", pv.rows, cv.rows);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "cibles",
                              "expected", "autant de cibles que de predictions",
                              "actual", actbuf, NULL);
        return false;
    }
    if (pv.rows == 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "predictions",
                              "expected", "au moins un exemple",
                              "actual", "lot vide", NULL);
        return false;
    }
    int* pc = metric_classes(&pv, nm, "predictions");
    if (pc == NULL) return false;
    int* tc = metric_classes(&cv, nm, "cibles");
    if (tc == NULL) { free(pc); return false; }
    *predOut = pc; *trueOut = tc; *bOut = pv.rows;
    /* cols des predictions perdu ici : recalcule par le caller confusion via
     * une seconde lecture de la vue si besoin. */
    return true;
}

/* Exactitude(predictions, cibles) / Accuracy : fraction d'exemples bien
 * classes, reel dans [0, 1]. */
static Value fn_exactitude(int argCount, Value* args) {
    (void)argCount;
    int *pc, *tc, B;
    if (!metric_pair_classes(args, "Exactitude", &pc, &tc, &B)) return NIL_VAL;
    int ok = 0;
    for (int i = 0; i < B; i++) if (pc[i] == tc[i]) ok++;
    free(pc); free(tc);
    return REAL_VAL((double)ok / (double)B);
}

/* MatriceDeConfusion(predictions, cibles) / ConfusionMatrix : Matrice carree
 * classes x classes, cellule (classe reelle i, classe predite j) = nombre
 * d'exemples de classe reelle i predits classe j (indices 0-based -> position
 * 1-based dans la Matrice). Le nombre de classes est le nombre de colonnes des
 * scores 2D, elargi au besoin pour couvrir toute classe reelle observee. */
static Value fn_matrice_confusion(int argCount, Value* args) {
    (void)argCount;
    MetricView pv;
    if (!metric_read_view(args[0], &pv, "MatriceDeConfusion", "predictions"))
        return NIL_VAL;
    int predCols = pv.cols;   /* nb de classes deduit des scores (0 si 1D) */
    int *pc, *tc, B;
    if (!metric_pair_classes(args, "MatriceDeConfusion", &pc, &tc, &B))
        return NIL_VAL;
    int nbClasses = predCols;
    for (int i = 0; i < B; i++) {
        if (pc[i] + 1 > nbClasses) nbClasses = pc[i] + 1;
        if (tc[i] + 1 > nbClasses) nbClasses = tc[i] + 1;
    }
    /* args rootes par le dispatch ; obj_new_matrix est la seule alloc OWL. */
    ObjMatrix* mc = obj_new_matrix(nbClasses, nbClasses);
    double* md = mc->data;   /* zero-init par obj_new_matrix */
    for (int i = 0; i < B; i++)
        md[(size_t)tc[i] * nbClasses + pc[i]] += 1.0;
    free(pc); free(tc);
    return OBJ_VAL(mc);
}

typedef enum { PRF_PRECISION, PRF_RECALL, PRF_F1 } PrfKind;

/* Precision / Rappel / F1 par classe -> Vecteur de longueur nbClasses
 * (position 1-based dans le Vecteur = classe 0-based + 1). Formules par
 * classe : precision = VP/(VP+FP), rappel = VP/(VP+FN), F1 = 2PR/(P+R).
 * Conventions deterministes sans NaN : denominateur nul -> 0.0 (classe sans
 * prediction positive ou sans instance reelle), et pour F1, P+R nul -> 0.0.
 * Le nombre de classes suit le nombre de colonnes des scores, elargi au
 * besoin pour couvrir toute classe observee (memes regles que
 * MatriceDeConfusion). La moyenne macro (non ponderee) s'obtient par
 * Vecteur..Moyenne() sur le resultat. */
static Value metric_per_class(Value* args, const char* nm, PrfKind kind) {
    MetricView pv;
    if (!metric_read_view(args[0], &pv, nm, "predictions")) return NIL_VAL;
    int predCols = pv.cols;   /* nb de classes deduit des scores (0 si 1D) */
    int *pc, *tc, B;
    if (!metric_pair_classes(args, nm, &pc, &tc, &B)) return NIL_VAL;
    int nbClasses = predCols;
    for (int i = 0; i < B; i++) {
        if (pc[i] + 1 > nbClasses) nbClasses = pc[i] + 1;
        if (tc[i] + 1 > nbClasses) nbClasses = tc[i] + 1;
    }
    int* tp = (int*)calloc((size_t)nbClasses, sizeof(int));
    int* fp = (int*)calloc((size_t)nbClasses, sizeof(int));
    int* fn = (int*)calloc((size_t)nbClasses, sizeof(int));
    for (int i = 0; i < B; i++) {
        if (pc[i] == tc[i]) {
            tp[pc[i]]++;
        } else {
            fp[pc[i]]++;   /* predit c mais reel != c */
            fn[tc[i]]++;   /* reel c mais predit != c */
        }
    }
    free(pc); free(tc);
    /* args rootes par le dispatch ; obj_new_vector est la seule alloc OWL et
     * rien n'alloue ensuite -> vec ne peut etre collecte avant le retour. */
    ObjVector* vec = obj_new_vector(nbClasses);
    for (int c = 0; c < nbClasses; c++) {
        double prec = (tp[c] + fp[c] > 0)
                          ? (double)tp[c] / (double)(tp[c] + fp[c]) : 0.0;
        double rec  = (tp[c] + fn[c] > 0)
                          ? (double)tp[c] / (double)(tp[c] + fn[c]) : 0.0;
        double val;
        switch (kind) {
            case PRF_PRECISION: val = prec; break;
            case PRF_RECALL:    val = rec;  break;
            default: /* PRF_F1 */
                val = (prec + rec > 0.0) ? 2.0 * prec * rec / (prec + rec) : 0.0;
                break;
        }
        vec->data[c] = val;
    }
    free(tp); free(fp); free(fn);
    return OBJ_VAL(vec);
}

/* Precision(predictions, cibles) / Precision : precision par classe. */
static Value fn_precision(int argCount, Value* args) {
    (void)argCount;
    return metric_per_class(args, "Precision", PRF_PRECISION);
}

/* Rappel(predictions, cibles) / Recall : rappel par classe. */
static Value fn_rappel(int argCount, Value* args) {
    (void)argCount;
    return metric_per_class(args, "Rappel", PRF_RECALL);
}

/* F1(predictions, cibles) : score F1 par classe. */
static Value fn_f1(int argCount, Value* args) {
    (void)argCount;
    return metric_per_class(args, "F1", PRF_F1);
}

/* ================================================================
 * Metriques d'evaluation de regression
 *
 * Comptage pur, NON differentiable : mesures de qualite sur des sorties
 * continues, distinctes des pertes du meme nom (TenseurErreurQuadratique /
 * TenseurL1) qui portent, elles, un graphe et un gradient. Entrees acceptees :
 * deux vues plates de meme effectif total (predictions continues + cibles),
 * lues sans allocation OWL -> les cellules pointent dans les buffers rootes
 * par les arguments, valides jusqu'au retour.
 * ================================================================ */

/* Nombre total de cellules d'une vue plate (1D : rows ; 2D : rows x cols). */
static int metric_view_count(const MetricView* mv) {
    return mv->cols == 0 ? mv->rows : mv->rows * mv->cols;
}

/* Lit predictions + cibles comme deux vues plates de meme effectif total.
 * false sur erreur (deja emise) : forme non prise en charge, effectifs
 * differents, ou lot vide. */
static bool metric_pair_flat(Value* args, const char* nm,
                             const double** predOut, const double** cibOut,
                             int* nOut) {
    MetricView pv, cv;
    if (!metric_read_view(args[0], &pv, nm, "predictions")) return false;
    if (!metric_read_view(args[1], &cv, nm, "cibles")) return false;
    int np = metric_view_count(&pv);
    int nc = metric_view_count(&cv);
    if (np != nc) {
        char actbuf[96];
        snprintf(actbuf, sizeof actbuf,
                 "%d predictions face a %d cibles", np, nc);
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "cibles",
                              "expected", "autant de cibles que de predictions",
                              "actual", actbuf, NULL);
        return false;
    }
    if (np == 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "predictions",
                              "expected", "au moins une valeur",
                              "actual", "lot vide", NULL);
        return false;
    }
    *predOut = pv.cells; *cibOut = cv.cells; *nOut = np;
    return true;
}

/* ErreurQuadratiqueMoyenne(predictions, cibles) / MeanSquaredError : MSE
 * d'evaluation = moyenne des carres des ecarts (cible - prediction). */
static Value fn_erreur_quadratique_moyenne(int argCount, Value* args) {
    (void)argCount;
    const double *pr, *ci; int n;
    if (!metric_pair_flat(args, "ErreurQuadratiqueMoyenne", &pr, &ci, &n))
        return NIL_VAL;
    double somme = 0.0;
    for (int i = 0; i < n; i++) {
        double d = ci[i] - pr[i];
        somme += d * d;
    }
    return REAL_VAL(somme / (double)n);
}

/* ErreurAbsolueMoyenne(predictions, cibles) / MeanAbsoluteError : MAE
 * d'evaluation = moyenne des ecarts absolus |cible - prediction|. */
static Value fn_erreur_absolue_moyenne(int argCount, Value* args) {
    (void)argCount;
    const double *pr, *ci; int n;
    if (!metric_pair_flat(args, "ErreurAbsolueMoyenne", &pr, &ci, &n))
        return NIL_VAL;
    double somme = 0.0;
    for (int i = 0; i < n; i++) somme += fabs(ci[i] - pr[i]);
    return REAL_VAL(somme / (double)n);
}

/* CoefficientDetermination(predictions, cibles) / RSquared : R² = 1 -
 * SS_res/SS_tot, ou SS_res = Somme(cible - pred)² et SS_tot = Somme(cible -
 * moyenne_cibles)². Convention deterministe : cibles toutes egales (SS_tot = 0)
 * -> 0.0 (jamais NaN ni infini). Pas de clampage : R² peut etre negatif quand
 * les predictions sont pires que la simple moyenne. */
static Value fn_coefficient_determination(int argCount, Value* args) {
    (void)argCount;
    const double *pr, *ci; int n;
    if (!metric_pair_flat(args, "CoefficientDetermination", &pr, &ci, &n))
        return NIL_VAL;
    double moyenne = 0.0;
    for (int i = 0; i < n; i++) moyenne += ci[i];
    moyenne /= (double)n;
    double ssRes = 0.0, ssTot = 0.0;
    for (int i = 0; i < n; i++) {
        double dr = ci[i] - pr[i];
        double dt = ci[i] - moyenne;
        ssRes += dr * dr;
        ssTot += dt * dt;
    }
    if (ssTot == 0.0) return REAL_VAL(0.0);
    return REAL_VAL(1.0 - ssRes / ssTot);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const char* const ALIAS_tenseur[] = { "Tensor", NULL };

static const OwlTypeDef TENSEUR_TYPES[] = {
    { .canonical      = "Tenseur",
      .canonicalAscii = "Tenseur",
      .aliases        = ALIAS_tenseur,
      .category       = "Tenseurs",
      .descFR         = "Enveloppe differentiable au-dessus d'une donnee "
                        "numerique de forme quelconque : scalaire ou 1D (un "
                        "Vecteur), 2D (une Matrice), ou N-D. Elle porte une "
                        "valeur (`..Valeur`) et un gradient (`..Gradient`) de "
                        "meme forme, initialise a zero, et memorise en coulisse "
                        "le graphe des operations qui l'ont produite. Construite "
                        "par `Tenseur([...])` ; pour une forme 2D ou N-D, partir "
                        "d'un tableau plat puis `Tenseur([...]).Redimensionne("
                        "[forme])`. Les operateurs `+` et `*` entre Tenseurs "
                        "enchainent le graphe ; `Retropropage()` remplit les "
                        "gradients par la regle de la chaine.",
      .descEN         = "Differentiable wrapper over numeric data of arbitrary "
                        "shape: scalar or 1D (a Vector), 2D (a Matrix), or N-D. "
                        "It carries a value (`..Value`) and a gradient "
                        "(`..Gradient`) of the same shape, initialized to zero, "
                        "and records behind the scenes the graph of operations "
                        "that produced it. Built with `Tensor([...])`; for a 2D "
                        "or N-D shape, start from a flat array then "
                        "`Tensor([...]).Reshape([shape])`. The `+` and `*` "
                        "operators between Tensors chain the graph; "
                        "`Backpropagate()` fills the gradients by the chain "
                        "rule.",
      .fields         = NULL,
      .coercible      = true,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "Tensor",
      .genre          = OWL_GENRE_M },   /* un Tenseur */
};

void stdlib_tenseur_init(void) {
    static const OwlParam p_donnee[] = {
        { "donnee", "tableau de reels",
          "Tableau de reels (ou Vecteur) dont le Tenseur enveloppe la valeur.",
          NULL, NULL },
        {0}
    };
    static const OwlSignature sig_tenseur[] = {
        { 1, p_donnee, "Tenseur",
          "Tenseur feuille enveloppant la donnee fournie, gradient a zero." },
        {0}
    };

    static const OwlParam p_matmul[] = {
        { "a", "Tenseur", "Premier operande : Tenseur 1D (donnees plates) ou "
          "Tenseur 2D m*n (poids).", NULL, NULL },
        { "b", "Tenseur", "Second operande : si `a` est 1D, Tenseur 1D de meme "
          "taille ; si `a` est 2D m*n, Tenseur 1D de taille n (matrice-vecteur) "
          "ou Tenseur 2D n*r (matrice-matrice).", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_matmul[] = {
        { 2, p_matmul, "Tenseur",
          "Tenseur scalaire (cas 1D.1D), Tenseur 1D de taille m (cas 2D m*n par "
          "1D) ou Tenseur 2D p*r (cas 2D p*q par 2D q*r), differentiable." },
        {0}
    };

    static const OwlParam p_activation[] = {
        { "t", "Tenseur", "Tenseur d'entree.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_activation[] = {
        { 1, p_activation, "Tenseur",
          "Nouveau Tenseur de meme forme, differentiable." },
        {0}
    };

    static const OwlParam p_perte[] = {
        { "prediction", "Tenseur", "Tenseur predit par le modele.", NULL, NULL },
        { "cible", "Tenseur",
          "Tenseur cible (verite) de meme taille ; ne recoit pas de gradient.",
          NULL, NULL },
        {0}
    };
    static const OwlSignature sig_perte[] = {
        { 2, p_perte, "Tenseur",
          "Tenseur scalaire (la perte), point de depart de `Retropropage()`." },
        {0}
    };

    static const OwlParam p_onehot[] = {
        { "indices", "Tenseur",
          "Tenseur 1D d'indices de classe 0-based, un par exemple.", NULL, NULL },
        { "nbClasses", "entier",
          "Nombre total de classes : largeur de la matrice one-hot rendue.",
          NULL, NULL },
        {0}
    };
    static const OwlSignature sig_onehot[] = {
        { 2, p_onehot, "Tenseur",
          "Tenseur 2D lot*nbClasses one-hot (1.0 a la colonne de l'indice, 0 "
          "ailleurs), feuille non differentiable." },
        {0}
    };

    static const OwlParam p_perte_indices[] = {
        { "prediction", "Tenseur",
          "Logits : Tenseur 2D lot*classes (ou 1D classes pour un seul exemple).",
          NULL, NULL },
        { "indices", "Tenseur",
          "Tenseur 1D d'indices de classe cible 0-based, un par exemple ; ne "
          "recoit pas de gradient.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_perte_indices[] = {
        { 2, p_perte_indices, "Tenseur",
          "Tenseur scalaire (la perte), point de depart de `Retropropage()`." },
        {0}
    };

    static const OwlParam p_descente[] = {
        { "parametre", "Tenseur",
          "Tenseur a mettre a jour (poids 2D ou biais 1D), dont le gradient a "
          "deja ete rempli par `Retropropage()`.", NULL, NULL },
        { "taux", "reel",
          "Taux d'apprentissage : facteur applique au gradient avant "
          "soustraction.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_descente[] = {
        { 2, p_descente, "Tenseur",
          "Nouvelle feuille de meme forme, valeur - taux*gradient, gradient a "
          "zero." },
        {0}
    };

    static const OwlParam p_redim[] = {
        { "t", "Tenseur", "Tenseur a redimensionner.", NULL, NULL },
        { "dimensions", "tableau d'entiers",
          "Nouvelle forme : tableau des tailles d'axes. Leur produit doit egaler "
          "le nombre d'elements du Tenseur d'origine.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_redim[] = {
        { 2, p_redim, "Tenseur",
          "Tenseur de la nouvelle forme, meme donnee dans le meme ordre de "
          "lecture, differentiable." },
        {0}
    };

    static const OwlParam p_reduction[] = {
        { "t", "Tenseur", "Tenseur a reduire.", NULL, NULL },
        { "axe", "entier",
          "Axe a reduire (1-based). Le rang du resultat est abaisse de 1.",
          NULL, NULL },
        {0}
    };
    static const OwlSignature sig_reduction[] = {
        { 2, p_reduction, "Tenseur",
          "Tenseur de rang abaisse de 1 (l'axe reduit disparait), "
          "differentiable." },
        {0}
    };

    static const OwlParam p_permute[] = {
        { "t", "Tenseur", "Tenseur a reordonner.", NULL, NULL },
        { "ordre", "tableau d'entiers",
          "Nouvel ordre des axes : une permutation complete des positions "
          "(1-based), chaque axe cite une seule fois.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_permute[] = {
        { 2, p_permute, "Tenseur",
          "Tenseur dont les axes suivent l'ordre demande, differentiable." },
        {0}
    };

    static const OwlParam p_transpose[] = {
        { "t", "Tenseur", "Tenseur d'au moins 2 axes a transposer.", NULL, NULL },
        { "axe1", "entier", "Premier axe a echanger (1-based).", NULL, NULL },
        { "axe2", "entier", "Second axe a echanger (1-based).", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_transpose[] = {
        { 3, p_transpose, "Tenseur",
          "Tenseur dont les deux axes indiques sont echanges, differentiable." },
        {0}
    };

    static const OwlParam p_tranche_ch[] = {
        { "t", "Tenseur",
          "Tenseur d'au moins 2 axes a trancher (forme N x canaux x ...spatial).",
          NULL, NULL },
        { "debut", "entier",
          "Premier canal de la plage extraite (1-based).", NULL, NULL },
        { "longueur", "entier",
          "Nombre de canaux extraits a partir de debut.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_tranche_ch[] = {
        { 3, p_tranche_ch, "Tenseur",
          "Tenseur de meme rang dont l'axe canal ne garde que la plage "
          "[debut, debut+longueur), les autres axes intacts, differentiable." },
        {0}
    };

    static const OwlParam p_conv[] = {
        { "entree", "Tenseur",
          "Tenseur 4D d'entree (lot x canaux x hauteur x largeur).", NULL, NULL },
        { "poids", "Tenseur",
          "Noyau 4D (canaux de sortie x canaux d'entree x hauteur x largeur).",
          NULL, NULL },
        { "pas", "entier",
          "Decalage entre deux fenetres consecutives (identique en hauteur et "
          "largeur).", NULL, NULL },
        { "remplissage", "entier",
          "Nombre de zeros ajoutes de chaque cote de l'entree avant le balayage.",
          NULL, NULL },
        { "group", "entier",
          "Nombre de groupes de canaux independants. Optionnel, defaut 1 "
          "(chaque canal de sortie voit tous les canaux d'entree). Doit diviser "
          "les canaux d'entree ET de sortie ; les poids ont alors la forme "
          "(canaux de sortie x canaux d'entree / group x hauteur x largeur). "
          "group egal aux canaux d'entree donne la convolution depthwise (un "
          "filtre par canal).", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_conv[] = {
        { 4, p_conv, "Tenseur",
          "Tenseur 4D de sortie (lot x canaux de sortie x hauteur' x largeur'), "
          "differentiable." },
        { 5, p_conv, "Tenseur",
          "Tenseur 4D de sortie avec convolution groupee (group groupes de "
          "canaux independants ; poids (canaux de sortie x canaux d'entree / "
          "group x hauteur x largeur)), differentiable." },
        {0}
    };

    static const OwlParam p_pool[] = {
        { "entree", "Tenseur",
          "Tenseur 4D d'entree (lot x canaux x hauteur x largeur).", NULL, NULL },
        { "taille", "entier",
          "Cote de la fenetre carree.", NULL, NULL },
        { "pas", "entier",
          "Decalage entre deux fenetres consecutives. Optionnel, defaut = taille "
          "(fenetres non chevauchantes) ; un pas inferieur a la taille produit "
          "des fenetres chevauchantes.", NULL, NULL },
        { "remplissage", "entier",
          "Nombre de cellules ajoutees symetriquement sur chaque bord avant le "
          "decoupage. Optionnel, defaut 0. En max, le remplissage vaut -infini "
          "(il ne gagne jamais) ; en moyenne, la division ne compte que les "
          "cellules reelles (count_include_pad=0).", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_pool[] = {
        { 2, p_pool, "Tenseur",
          "Tenseur 4D reduit (pas par defaut = taille : fenetres non "
          "chevauchantes), differentiable." },
        { 3, p_pool, "Tenseur",
          "Tenseur 4D reduit avec un pas explicite (hauteur' = "
          "plancher((hauteur - taille) / pas) + 1, idem largeur), differentiable." },
        { 4, p_pool, "Tenseur",
          "Tenseur 4D reduit avec pas et remplissage explicites (hauteur' = "
          "plancher((hauteur + 2*remplissage - taille) / pas) + 1, idem largeur), "
          "differentiable." },
        {0}
    };

    static const OwlParam p_batchnorm[] = {
        { "entree", "Tenseur",
          "Tenseur 4D d'entree (lot x canaux x hauteur x largeur).", NULL, NULL },
        { "gamma", "Tenseur",
          "Facteur d'echelle par canal (Tenseur de taille = nombre de canaux).",
          NULL, NULL },
        { "beta", "Tenseur",
          "Decalage par canal (Tenseur de taille = nombre de canaux).",
          NULL, NULL },
        { "epsilon", "r\xc3\xa9" "el",
          "Petit reel ajoute a la variance avant la racine (evite la division "
          "par zero).", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_batchnorm[] = {
        { 4, p_batchnorm, "Tenseur",
          "Tenseur 4D normalise par canal, mis a l'echelle puis decale, "
          "differentiable." },
        {0}
    };

    static const OwlParam p_standardise[] = {
        { "entree", "Tenseur",
          "Tenseur 4D d'entree (lot x canaux x hauteur x largeur).", NULL, NULL },
        { "epsilon", "r\xc3\xa9" "el",
          "Petit reel ajoute a la variance avant la racine (evite la division "
          "par zero). Optionnel, defaut 1e-5.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_standardise[] = {
        { 1, p_standardise, "Tenseur",
          "Tenseur 4D standardise par canal (epsilon par defaut 1e-5)." },
        { 2, p_standardise, "Tenseur",
          "Idem, avec un epsilon explicite." },
        {0}
    };

    static const OwlParam p_metrique[] = {
        { "predictions", "Tenseur",
          "Predictions du modele : soit un Tenseur 2D lot x classes (scores ou "
          "probabilites, la classe retenue par ligne etant celle du plus grand "
          "score), soit un Tenseur 1D d'indices de classe (0-based).",
          NULL, NULL },
        { "cibles", "Tenseur",
          "Verites terrain : Tenseur 1D d'indices de classe attendus (0-based), "
          "un par exemple.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_exactitude[] = {
        { 2, p_metrique, "r\xc3\xa9" "el",
          "Fraction d'exemples correctement classes, entre 0.0 et 1.0." },
        {0}
    };
    static const OwlSignature sig_confusion[] = {
        { 2, p_metrique, "Matrice",
          "Matrice carree classes x classes : la cellule (classe reelle, classe "
          "predite) compte les exemples correspondants." },
        {0}
    };
    static const OwlSignature sig_prf[] = {
        { 2, p_metrique, "Vecteur",
          "Valeur de la metrique par classe (position 1-based = classe 0-based "
          "+ 1). Moyenne macro (non ponderee) via ..Moyenne() sur le Vecteur." },
        {0}
    };

    static const OwlParam p_metrique_reg[] = {
        { "predictions", "Tenseur",
          "Predictions continues du modele : un Tenseur de valeurs reelles, de "
          "meme forme que les cibles.",
          NULL, NULL },
        { "cibles", "Tenseur",
          "Verites terrain continues : un Tenseur de valeurs reelles, de meme "
          "forme que les predictions.", NULL, NULL },
        {0}
    };
    static const OwlSignature sig_metrique_reg[] = {
        { 2, p_metrique_reg, "r\xc3\xa9" "el",
          "Valeur reelle de la metrique de regression, calculee sur l'ensemble "
          "des cellules." },
        {0}
    };

    static const OwlNativeDef DEFS_TENSEUR[] = {
        { .canonical   = "Tenseur",
          .nameEN      = "Tensor",
          .category    = "Tenseurs",
          .descFR      = "Construit un Tenseur feuille enveloppant un tableau "
                         "de reels (ou un Vecteur) : sa valeur est la donnee "
                         "fournie, son gradient demarre a zero. Le tableau "
                         "fourni est scalaire ou 1D ; pour une forme 2D "
                         "(Matrice) ou N-D, l'enchainer avec "
                         "`.Redimensionne([forme])`. Point de depart d'un graphe "
                         "de calcul differentiable.",
          .descEN      = "Builds a leaf Tensor wrapping an array of reals (or a "
                         "Vector): its value is the given data, its gradient "
                         "starts at zero. The given array is scalar or 1D; for a "
                         "2D (Matrix) or N-D shape, chain it with "
                         "`.Reshape([shape])`. The starting point of a "
                         "differentiable computation graph.",
          .signatures  = sig_tenseur,
          .impl        = fn_tenseur,
          .flags       = 0,
        },
        { .canonical   = "TenseurProduitMatriciel",
          .nameEN      = "TensorMatrixProduct",
          .category    = "Tenseurs",
          .descFR      = "Produit differentiable entre deux Tenseurs, sous trois "
                         "formes selon les operandes. Deux Tenseurs 1D (donnees "
                         "plates) : produit scalaire Somme(a[i]*b[i]), resultat "
                         "Tenseur scalaire. Un Tenseur 2D m*n (poids) et un "
                         "Tenseur 1D de taille n (entree) : produit "
                         "matrice-vecteur, resultat Tenseur 1D de taille m. Deux "
                         "Tenseurs 2D p*q et q*r : produit matrice-matrice, "
                         "resultat Tenseur 2D p*r (par exemple un paquet "
                         "d'entrees B*in par des poids in*out donne B*out). Dans "
                         "les trois cas, `Retropropage()` remonte le gradient "
                         "jusqu'a chaque operande.",
          .descEN      = "Differentiable product between two Tensors, in three "
                         "forms depending on the operands. Two 1D Tensors (flat "
                         "data): dot product Sum(a[i]*b[i]), result a scalar "
                         "Tensor. A 2D Tensor m*n (weights) and a 1D Tensor of "
                         "size n (input): matrix-vector product, result a 1D "
                         "Tensor of size m. Two 2D Tensors p*q and q*r: "
                         "matrix-matrix product, result a 2D Tensor p*r (for "
                         "example a batch of inputs B*in by weights in*out gives "
                         "B*out). In all three cases, `Backpropagate()` carries "
                         "the gradient back to each operand.",
          .signatures  = sig_matmul,
          .impl        = fn_tenseur_produit_matriciel,
          .flags       = 0,
          /* 3 formes d'operandes gerees par fn_tenseur_produit_matriciel :
           * Vecteur.Vecteur, Matrice@Vecteur, Matrice@Matrice. Bumper ce
           * compteur en meme temps que l'ajout d'une forme au dispatch force
           * la mise a jour de la desc (regle self-test 58). */
          .formCount   = 3,
        },
        { .canonical   = "TenseurReLU",
          .nameEN      = "TensorReLU",
          .category    = "Tenseurs",
          .descFR      = "Applique la fonction ReLU (max(0, x)) a chaque "
                         "composante d'un Tenseur, de facon differentiable : "
                         "`Retropropage()` ne laisse passer le gradient que la ou "
                         "la composante d'entree etait strictement positive.",
          .descEN      = "Applies the ReLU function (max(0, x)) to each component "
                         "of a Tensor, differentiably: `Backpropagate()` only "
                         "lets the gradient through where the input component was "
                         "strictly positive.",
          .signatures  = sig_activation,
          .impl        = fn_tenseur_relu,
          .flags       = 0,
        },
        { .canonical      = "TenseurSigmo\xc3\xaf" "de",
          .canonicalAscii = "TenseurSigmoide",
          .nameEN         = "TensorSigmoid",
          .category       = "Tenseurs",
          .descFR         = "Applique la sigmoide (1 / (1 + e^-x)) a chaque "
                            "composante d'un Tenseur, ramenant toute valeur dans "
                            "l'intervalle ouvert (0, 1), de facon differentiable "
                            "(gradient y*(1-y) issu de la sortie).",
          .descEN         = "Applies the sigmoid (1 / (1 + e^-x)) to each "
                            "component of a Tensor, mapping any value into the "
                            "open interval (0, 1), differentiably (gradient "
                            "y*(1-y) from the output).",
          .signatures     = sig_activation,
          .impl           = fn_tenseur_sigmoide,
          .flags          = 0,
        },
        { .canonical   = "TenseurTanh",
          .nameEN      = "TensorTanh",
          .category    = "Tenseurs",
          .descFR      = "Applique la tangente hyperbolique a chaque composante "
                         "d'un Tenseur, ramenant toute valeur dans l'intervalle "
                         "ouvert (-1, 1), de facon differentiable (gradient "
                         "1 - y^2 issu de la sortie).",
          .descEN      = "Applies the hyperbolic tangent to each component of a "
                         "Tensor, mapping any value into the open interval "
                         "(-1, 1), differentiably (gradient 1 - y^2 from the "
                         "output).",
          .signatures  = sig_activation,
          .impl        = fn_tenseur_tanh,
          .flags       = 0,
        },
        { .canonical   = "TenseurErreurQuadratique",
          .nameEN      = "TensorMeanSquaredError",
          .category    = "Tenseurs",
          .descFR      = "Perte erreur quadratique moyenne (MSE) entre une "
                         "prediction et une cible : moyenne des carres des "
                         "ecarts, rendue en un Tenseur scalaire. Differentiable : "
                         "`Retropropage()` remonte le gradient vers la prediction "
                         "(2*(prediction - cible) / n) ; la cible, verite fixe, "
                         "ne recoit aucun gradient.",
          .descEN      = "Mean squared error (MSE) loss between a prediction and "
                         "a target: the mean of the squared differences, returned "
                         "as a scalar Tensor. Differentiable: `Backpropagate()` "
                         "carries the gradient back to the prediction "
                         "(2*(prediction - target) / n); the target, a fixed "
                         "truth, receives no gradient.",
          .signatures  = sig_perte,
          .impl        = fn_tenseur_erreur_quadratique,
          .flags       = 0,
        },
        { .canonical      = "TenseurEntropieCrois\xc3\xa9" "e",
          .canonicalAscii = "TenseurEntropieCroisee",
          .nameEN         = "TensorCrossEntropy",
          .category       = "Tenseurs",
          .descFR         = "Perte entropie croisee avec softmax fusionnee : la "
                            "prediction porte des scores bruts (logits), "
                            "transformes en interne en probabilites par une "
                            "softmax stable, puis compares a une cible one-hot. "
                            "Accepte aussi bien une prediction 1D (un exemple) "
                            "qu'un lot de predictions 2D B*classes face a des "
                            "cibles 2D B*classes : softmax par ligne puis moyenne "
                            "des pertes sur le lot. "
                            "Rend un Tenseur scalaire. Differentiable : "
                            "`Retropropage()` remonte vers la prediction un "
                            "gradient fusionne (probabilites - cible) ; la cible "
                            "ne recoit aucun gradient.",
          .descEN         = "Cross-entropy loss with fused softmax: the "
                            "prediction carries raw scores (logits), turned "
                            "internally into probabilities by a stable softmax, "
                            "then compared to a one-hot target. Accepts either a "
                            "1D prediction (a single example) or a batch of 2D "
                            "predictions B*classes against 2D targets B*classes: "
                            "softmax per row then the mean of the per-row losses. "
                            "Returns a scalar "
                            "Tensor. Differentiable: `Backpropagate()` carries "
                            "back to the prediction a fused gradient "
                            "(probabilities - target); the target receives no "
                            "gradient.",
          .signatures     = sig_perte,
          .impl           = fn_tenseur_entropie_croisee,
          .flags          = 0,
        },
        { .canonical      = "TenseurEntropieCrois\xc3\xa9" "eIndices",
          .canonicalAscii = "TenseurEntropieCroiseeIndices",
          .nameEN         = "TensorCrossEntropyIndices",
          .category       = "Tenseurs",
          .descFR         = "Variante de l'entropie croisee acceptant directement "
                            "des indices de classe cible (Tenseur 1D 0-based, un "
                            "par exemple) plutot qu'une cible one-hot 2D. La "
                            "prediction porte des logits (Tenseur 2D lot*classes, "
                            "ou 1D classes pour un seul exemple) ; la cible one-hot "
                            "est construite en interne, puis le calcul rejoint "
                            "exactement `TenseurEntropieCroisee` : meme perte et "
                            "meme gradient. Rend un Tenseur scalaire. "
                            "Differentiable : `Retropropage()` remonte vers la "
                            "prediction le gradient fusionne (probabilites - "
                            "cible) ; les indices, verite fixe, ne recoivent aucun "
                            "gradient.",
          .descEN         = "Cross-entropy variant taking class target indices "
                            "directly (a 0-based 1D Tensor, one per example) rather "
                            "than a 2D one-hot target. The prediction carries "
                            "logits (a 2D Tensor batch*classes, or 1D classes for a "
                            "single example); the one-hot target is built "
                            "internally, then the computation matches "
                            "`TensorCrossEntropy` exactly: same loss and same "
                            "gradient. Returns a scalar Tensor. Differentiable: "
                            "`Backpropagate()` carries back to the prediction the "
                            "fused gradient (probabilities - target); the indices, "
                            "a fixed truth, receive no gradient.",
          .signatures     = sig_perte_indices,
          .impl           = fn_tenseur_entropie_croisee_indices,
          .flags          = 0,
        },
        { .canonical      = "TenseurEntropieCrois\xc3\xa9" "eBinaire",
          .canonicalAscii = "TenseurEntropieCroiseeBinaire",
          .nameEN         = "TensorBinaryCrossEntropy",
          .category       = "Tenseurs",
          .descFR         = "Perte entropie croisee binaire avec sigmoide "
                            "fusionnee : la prediction porte des scores bruts "
                            "(logits, avant sigmoide), la cible une valeur entre 0 "
                            "et 1. Perte = moyenne, sur les elements, de "
                            "cible*softplus(-logit) + (1 - cible)*softplus(logit) ; "
                            "la fusion evite tout debordement numerique. Rend un "
                            "Tenseur scalaire. Perte de choix pour un score binaire "
                            "de presence, mieux conditionnee que composer une "
                            "sigmoide puis une erreur quadratique. Differentiable : "
                            "`Retropropage()` remonte vers la prediction un gradient "
                            "fusionne (sigmoide(logit) - cible) / n ; la cible ne "
                            "recoit aucun gradient.",
          .descEN         = "Binary cross-entropy loss with fused sigmoid: the "
                            "prediction carries raw scores (logits, before the "
                            "sigmoid), the target a value between 0 and 1. Loss = the "
                            "mean, over the elements, of "
                            "target*softplus(-logit) + (1 - target)*softplus(logit); "
                            "the fusion avoids any numerical overflow. Returns a "
                            "scalar Tensor. The loss of choice for a binary presence "
                            "score, better conditioned than composing a sigmoid then "
                            "a squared error. Differentiable: `Backpropagate()` "
                            "carries back to the prediction a fused gradient "
                            "(sigmoid(logit) - target) / n; the target receives no "
                            "gradient.",
          .signatures     = sig_perte,
          .impl           = fn_tenseur_entropie_croisee_binaire,
          .flags          = 0,
        },
        { .canonical   = "TenseurL1",
          .nameEN      = "TensorL1",
          .category    = "Tenseurs",
          .descFR      = "Perte L1 (erreur absolue moyenne) entre une prediction et "
                         "une cible : moyenne des ecarts absolus, rendue en un "
                         "Tenseur scalaire. Moins sensible aux valeurs aberrantes "
                         "que l'erreur quadratique, adaptee a une regression de "
                         "boite englobante. Differentiable : `Retropropage()` "
                         "remonte vers la prediction un gradient fait du seul signe "
                         "de l'ecart (signe(prediction - cible) / n) ; la cible ne "
                         "recoit aucun gradient.",
          .descEN      = "L1 loss (mean absolute error) between a prediction and a "
                         "target: the mean of the absolute differences, returned as "
                         "a scalar Tensor. Less sensitive to outliers than the "
                         "squared error, suited to bounding-box regression. "
                         "Differentiable: `Backpropagate()` carries back to the "
                         "prediction a gradient made of the sign of the difference "
                         "only (sign(prediction - target) / n); the target receives "
                         "no gradient.",
          .signatures  = sig_perte,
          .impl        = fn_tenseur_l1,
          .flags       = 0,
        },
        { .canonical   = "TenseurUnParmiN",
          .nameEN      = "TensorOneHot",
          .category    = "Tenseurs",
          .descFR      = "Encode un Tenseur 1D d'indices de classe 0-based (un par "
                         "exemple) en une matrice one-hot 2D lot*nbClasses : chaque "
                         "ligne porte 1.0 a la colonne de son indice, 0 ailleurs. "
                         "Sert a fabriquer une cible one-hot a partir d'etiquettes "
                         "compactes. Feuille non differentiable (c'est une cible). "
                         "Un indice hors de [0, nbClasses[ signale une erreur.",
          .descEN      = "Encodes a 1D Tensor of 0-based class indices (one per "
                         "example) into a 2D one-hot matrix batch*nbClasses: each "
                         "row carries 1.0 at its index column, 0 elsewhere. Used to "
                         "build a one-hot target from compact labels. A "
                         "non-differentiable leaf (it is a target). An index "
                         "outside [0, nbClasses[ signals an error.",
          .signatures  = sig_onehot,
          .impl        = fn_tenseur_unparmin,
          .flags       = 0,
        },
        { .canonical   = "TenseurDescenteGradient",
          .nameEN      = "TensorGradientDescent",
          .category    = "Tenseurs",
          .descFR      = "Applique un pas de descente de gradient a un Tenseur "
                         "parametre, aussi bien une matrice de poids 2D qu'un "
                         "biais 1D : rend une nouvelle feuille de meme forme, "
                         "egale a valeur - taux*gradient. Le gradient doit avoir "
                         "ete rempli au prealable par `Retropropage()`. La feuille "
                         "rendue repart avec un gradient a zero, sans lien avec le "
                         "graphe precedent.",
          .descEN      = "Applies one gradient-descent step to a parameter Tensor, "
                         "whether a 2D weight matrix or a 1D bias: "
                         "returns a new leaf of the same shape, equal to "
                         "value - rate*gradient. The gradient must have been filled "
                         "beforehand by `Backpropagate()`. The returned leaf starts "
                         "with a zero gradient, unlinked from the previous graph.",
          .signatures  = sig_descente,
          .impl        = fn_tenseur_descente_gradient,
          .flags       = 0,
        },
        { .canonical   = "TenseurRedimensionne",
          .nameEN      = "TensorReshape",
          .category    = "Tenseurs",
          .descFR      = "Redimensionne un Tenseur vers une nouvelle forme sans "
                         "changer ni ses elements ni leur ordre de lecture (le "
                         "premier axe varie le plus lentement, le dernier le plus "
                         "vite). Le produit des nouvelles dimensions doit egaler "
                         "le nombre d'elements. Differentiable : `Retropropage()` "
                         "renvoie chaque gradient a sa position d'origine.",
          .descEN      = "Reshapes a Tensor to a new shape without changing its "
                         "elements or their reading order (the first axis varies "
                         "slowest, the last fastest). The product of the new "
                         "dimensions must equal the element count. Differentiable: "
                         "`Backpropagate()` sends each gradient back to its "
                         "original position.",
          .signatures  = sig_redim,
          .impl        = fn_tenseur_redimensionne,
          .flags       = 0,
        },
        { .canonical   = "TenseurSomme",
          .nameEN      = "TensorSum",
          .category    = "Tenseurs",
          .descFR      = "Somme les elements d'un Tenseur le long d'un axe donne "
                         "(1-based), abaissant le rang de 1 (l'axe reduit "
                         "disparait). Differentiable : `Retropropage()` rediffuse "
                         "le gradient recu sur chaque element de l'axe reduit.",
          .descEN      = "Sums a Tensor's elements along a given axis (1-based), "
                         "lowering the rank by 1 (the reduced axis disappears). "
                         "Differentiable: `Backpropagate()` broadcasts the "
                         "incoming gradient back onto each element of the reduced "
                         "axis.",
          .signatures  = sig_reduction,
          .impl        = fn_tenseur_somme,
          .flags       = 0,
        },
        { .canonical   = "TenseurMoyenne",
          .nameEN      = "TensorMean",
          .category    = "Tenseurs",
          .descFR      = "Moyenne des elements d'un Tenseur le long d'un axe "
                         "donne (1-based), abaissant le rang de 1. Differentiable : "
                         "`Retropropage()` rediffuse le gradient recu, divise par "
                         "la taille de l'axe reduit, sur chaque element.",
          .descEN      = "Averages a Tensor's elements along a given axis "
                         "(1-based), lowering the rank by 1. Differentiable: "
                         "`Backpropagate()` broadcasts the incoming gradient, "
                         "divided by the reduced axis size, onto each element.",
          .signatures  = sig_reduction,
          .impl        = fn_tenseur_moyenne,
          .flags       = 0,
        },
        { .canonical   = "TenseurPermute",
          .nameEN      = "TensorPermute",
          .category    = "Tenseurs",
          .descFR      = "Reordonne tous les axes d'un Tenseur selon un ordre "
                         "donne (1-based) : une permutation complete ou chaque "
                         "axe est cite une seule fois. Aucune donnee n'est perdue "
                         "ni dupliquee, seule leur disposition change. "
                         "Differentiable : `Retropropage()` renvoie chaque "
                         "gradient a sa position d'origine.",
          .descEN      = "Reorders all axes of a Tensor by a given order "
                         "(1-based): a full permutation where each axis appears "
                         "exactly once. No data is lost or duplicated, only its "
                         "layout changes. Differentiable: `Backpropagate()` sends "
                         "each gradient back to its original position.",
          .signatures  = sig_permute,
          .impl        = fn_tenseur_permute,
          .flags       = 0,
        },
        { .canonical   = "TenseurTranspose",
          .nameEN      = "TensorTranspose",
          .category    = "Tenseurs",
          .descFR      = "Echange deux axes precis d'un Tenseur d'au moins 2 axes "
                         "(1-based) : generalisation a N dimensions de la "
                         "transposition d'une matrice. Differentiable : "
                         "`Retropropage()` renvoie chaque gradient a sa position "
                         "d'origine.",
          .descEN      = "Swaps two given axes of a Tensor of at least 2 axes "
                         "(1-based): the N-dimensional generalization of a matrix "
                         "transpose. Differentiable: `Backpropagate()` sends each "
                         "gradient back to its original position.",
          .signatures  = sig_transpose,
          .impl        = fn_tenseur_transpose,
          .flags       = 0,
        },
        { .canonical   = "TenseurTrancheCanaux",
          .nameEN      = "TensorChannelSlice",
          .category    = "Tenseurs",
          .descFR      = "Extrait une plage de canaux contigus sur l'axe canal "
                         "(2e axe) d'un Tenseur d'au moins 2 axes : de la forme "
                         "N x canaux x ...spatial vers N x longueur x ...spatial, "
                         "les autres axes intacts. Le debut est 1-based (le premier "
                         "canal est 1). Utile pour separer une sortie multi-canaux "
                         "en groupes (par exemple une tete de detection en boite / "
                         "objectness / classes). Differentiable : `Retropropage()` "
                         "renvoie le gradient recu dans la plage source, et zero "
                         "pour les canaux laisses de cote.",
          .descEN      = "Extracts a contiguous range of channels along the channel "
                         "axis (2nd axis) of a Tensor of at least 2 axes: from shape "
                         "N x channels x ...spatial to N x length x ...spatial, the "
                         "other axes untouched. The start is 1-based (the first "
                         "channel is 1). Useful to split a multi-channel output into "
                         "groups (for example a detection head into box / objectness "
                         "/ classes). Differentiable: `Backpropagate()` sends the "
                         "incoming gradient back to the source range, and zero to "
                         "the channels left out.",
          .signatures  = sig_tranche_ch,
          .impl        = fn_tenseur_tranchecanaux,
          .flags       = 0,
        },
        { .canonical   = "TenseurConvolution2D",
          .nameEN      = "TensorConvolution2D",
          .category    = "Tenseurs",
          .descFR      = "Convolution 2D (correlation croisee) d'une entree 4D "
                         "(lot x canaux x hauteur x largeur) par un noyau 4D "
                         "(canaux de sortie x canaux d'entree / group x hauteur x "
                         "largeur), avec un pas, un remplissage de zeros et un "
                         "nombre de groupes optionnel (defaut 1). Le noyau glisse "
                         "sur l'entree, chaque cellule de sortie etant la somme "
                         "ponderee de la fenetre couverte, sommee sur les canaux "
                         "d'entree. Avec group > 1, les canaux d'entree et de "
                         "sortie sont partitionnes en group groupes independants "
                         "(group egal aux canaux d'entree = convolution "
                         "depthwise). La taille de sortie vaut "
                         "(taille + 2*remplissage - noyau) / pas + 1. "
                         "Differentiable : `Retropropage()` remonte le gradient "
                         "vers l'entree ET vers le noyau.",
          .descEN      = "2D convolution (cross-correlation) of a 4D input "
                         "(batch x channels x height x width) by a 4D kernel "
                         "(output channels x input channels / group x height x "
                         "width), with a stride, zero padding and an optional "
                         "number of groups (default 1). The kernel slides over "
                         "the input, each output cell being the weighted sum of "
                         "the covered window, summed over the input channels. "
                         "With group > 1, input and output channels are "
                         "partitioned into group independent groups (group equal "
                         "to the input channels = depthwise convolution). The "
                         "output size is (size + 2*padding - kernel) / stride + 1. "
                         "Differentiable: `Backpropagate()` carries the gradient "
                         "back to the input AND to the kernel.",
          .signatures  = sig_conv,
          .impl        = fn_tenseur_convolution2d,
          .flags       = 0,
        },
        { .canonical   = "TenseurMaxPooling",
          .nameEN      = "TensorMaxPooling",
          .category    = "Tenseurs",
          .descFR      = "Sous-echantillonnage par le maximum : decoupe une "
                         "entree 4D (lot x canaux x hauteur x largeur) en "
                         "fenetres carrees et rend le maximum de chaque fenetre. "
                         "Le pas est optionnel (defaut = taille, fenetres non "
                         "chevauchantes) ; un pas inferieur a la taille produit "
                         "des fenetres chevauchantes. Le remplissage est optionnel "
                         "(defaut 0) : les cellules ajoutees valent -infini et ne "
                         "gagnent jamais le maximum. Differentiable : "
                         "`Retropropage()` ne route le gradient que vers la "
                         "cellule gagnante de chaque fenetre (les gradients "
                         "s'accumulent sur une cellule gagnante de plusieurs "
                         "fenetres chevauchantes), les autres recevant un "
                         "gradient nul.",
          .descEN      = "Max downsampling: splits a 4D input (batch x channels x "
                         "height x width) into square windows and returns the "
                         "maximum of each window. The stride is optional (default "
                         "= size, non-overlapping windows); a stride smaller than "
                         "the size produces overlapping windows. Padding is "
                         "optional (default 0): the added cells are -infinity and "
                         "never win the maximum. Differentiable: "
                         "`Backpropagate()` only routes the gradient to the "
                         "winning cell of each window (gradients accumulate on a "
                         "cell that wins several overlapping windows), the others "
                         "receiving a zero gradient.",
          .signatures  = sig_pool,
          .impl        = fn_tenseur_maxpooling,
          .flags       = 0,
        },
        { .canonical   = "TenseurMoyennePooling",
          .nameEN      = "TensorAveragePooling",
          .category    = "Tenseurs",
          .descFR      = "Sous-echantillonnage par la moyenne : decoupe une "
                         "entree 4D (lot x canaux x hauteur x largeur) en "
                         "fenetres carrees et rend la moyenne de chaque fenetre. "
                         "Le pas est optionnel (defaut = taille, fenetres non "
                         "chevauchantes) ; un pas inferieur a la taille produit "
                         "des fenetres chevauchantes. Le remplissage est optionnel "
                         "(defaut 0) : la moyenne ne divise que par le nombre de "
                         "cellules reelles de la fenetre (les cellules de "
                         "remplissage ne sont pas comptees). Differentiable : "
                         "`Retropropage()` repartit le gradient de chaque fenetre "
                         "uniformement sur toutes ses cellules reelles (les parts "
                         "s'accumulent sur les cellules partagees par plusieurs "
                         "fenetres chevauchantes).",
          .descEN      = "Average downsampling: splits a 4D input (batch x "
                         "channels x height x width) into square windows and "
                         "returns the mean of each window. The stride is optional "
                         "(default = size, non-overlapping windows); a stride "
                         "smaller than the size produces overlapping windows. "
                         "Padding is optional (default 0): the mean divides only "
                         "by the number of real cells in the window (padding cells "
                         "are not counted). Differentiable: `Backpropagate()` "
                         "spreads each window's gradient uniformly over all its "
                         "real cells (shares accumulate on cells shared by several "
                         "overlapping windows).",
          .signatures  = sig_pool,
          .impl        = fn_tenseur_moyennepooling,
          .flags       = 0,
        },
        { .canonical   = "TenseurNormalisationLot",
          .nameEN      = "TensorBatchNorm",
          .category    = "Tenseurs",
          .descFR      = "Normalisation par lot d'une entree 4D (lot x canaux x "
                         "hauteur x largeur) : chaque canal est centre-reduit sur "
                         "les axes lot, hauteur et largeur (moyenne et variance de "
                         "ce canal), puis mis a l'echelle par gamma et decale par "
                         "beta, tous deux de taille egale au nombre de canaux. "
                         "L'epsilon est ajoute a la variance avant la racine. "
                         "Differentiable : `Retropropage()` remonte le gradient "
                         "vers l'entree, gamma et beta.",
          .descEN      = "Batch normalization of a 4D input (batch x channels x "
                         "height x width): each channel is standardized over the "
                         "batch, height and width axes (that channel's mean and "
                         "variance), then scaled by gamma and shifted by beta, "
                         "both of size equal to the channel count. Epsilon is "
                         "added to the variance before the square root. "
                         "Differentiable: `Backpropagate()` carries the gradient "
                         "back to the input, gamma and beta.",
          .signatures  = sig_batchnorm,
          .impl        = fn_tenseur_normalisationlot,
          .flags       = 0,
        },
        { .canonical   = "TenseurStandardise",
          .nameEN      = "TensorStandardize",
          .category    = "Tenseurs",
          .descFR      = "Standardise (z-score) par canal une entree 4D (lot x "
                         "canaux x hauteur x largeur) : chaque canal est "
                         "centre-reduit sur les axes lot, hauteur et largeur, soit "
                         "(entree - moyenne) / racine(variance + epsilon), moyenne "
                         "et variance de ce canal (variance de population, divisee "
                         "par le nombre d'elements). L'epsilon (optionnel, defaut "
                         "1e-5) evite la division par zero sur un canal constant. "
                         "Pretraitement fige, sans gradient : le resultat est une "
                         "feuille du graphe, contrairement a la normalisation par "
                         "lot entrainable.",
          .descEN      = "Standardizes (z-score) a 4D input (batch x channels x "
                         "height x width) per channel: each channel is centered "
                         "and scaled over the batch, height and width axes, i.e. "
                         "(input - mean) / sqrt(variance + epsilon), that channel's "
                         "mean and variance (population variance, divided by the "
                         "element count). Epsilon (optional, default 1e-5) avoids "
                         "division by zero on a constant channel. A fixed "
                         "preprocessing step, without gradient: the result is a "
                         "leaf of the graph, unlike the trainable batch "
                         "normalization.",
          .signatures  = sig_standardise,
          .impl        = fn_tenseur_standardise,
          .flags       = 0,
        },
        { .canonical   = "Exactitude",
          .nameEN      = "Accuracy",
          .category    = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation : fraction d'exemples correctement "
                         "classes (entre 0.0 et 1.0). Les predictions sont soit "
                         "un Tenseur 2D lot x classes (scores ou probabilites, la "
                         "classe retenue par ligne etant celle du plus grand "
                         "score), soit un Tenseur 1D d'indices de classe ; les "
                         "cibles sont un Tenseur 1D d'indices attendus (0-based). "
                         "Comptage pur, sans gradient : c'est une metrique "
                         "d'evaluation, pas une perte d'entrainement.",
          .descEN      = "Evaluation measure: the fraction of correctly classified "
                         "examples (between 0.0 and 1.0). Predictions are either a "
                         "2D Tensor batch x classes (scores or probabilities, the "
                         "class kept per row being the one with the highest score) "
                         "or a 1D Tensor of class indices; targets are a 1D Tensor "
                         "of expected indices (0-based). Pure counting, no "
                         "gradient: an evaluation metric, not a training loss.",
          .signatures  = sig_exactitude,
          .impl        = fn_exactitude,
          .flags       = 0,
        },
        { .canonical   = "MatriceDeConfusion",
          .nameEN      = "ConfusionMatrix",
          .category    = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation : Matrice carree classes x classes "
                         "dont la cellule a la classe reelle i et la classe "
                         "predite j compte les exemples de classe reelle i predits "
                         "en classe j (indices 0-based, lus en position 1-based "
                         "dans la Matrice). Les predictions sont soit un Tenseur 2D "
                         "lot x classes (la classe retenue par ligne etant celle "
                         "du plus grand score), soit un Tenseur 1D d'indices ; les "
                         "cibles sont un Tenseur 1D d'indices attendus. Le nombre "
                         "de classes suit le nombre de colonnes des scores, elargi "
                         "au besoin pour couvrir toute classe observee. Comptage "
                         "pur, sans gradient.",
          .descEN      = "Evaluation measure: a square classes x classes Matrix "
                         "whose cell at true class i and predicted class j counts "
                         "the examples of true class i predicted as class j "
                         "(0-based indices, read at 1-based positions in the "
                         "Matrix). Predictions are either a 2D Tensor batch x "
                         "classes (the class kept per row being the one with the "
                         "highest score) or a 1D Tensor of indices; targets are a "
                         "1D Tensor of expected indices. The class count follows "
                         "the score columns, widened as needed to cover every "
                         "observed class. Pure counting, no gradient.",
          .signatures  = sig_confusion,
          .impl        = fn_matrice_confusion,
          .flags       = 0,
        },
        { .canonical      = "Pr\xc3\xa9" "cision",
          .canonicalAscii = "Precision",
          .nameEN         = "Precision",
          .category       = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation : precision par classe, rendue dans "
                         "un Vecteur (position 1-based = classe 0-based + 1). Pour "
                         "chaque classe, precision = vrais positifs / (vrais "
                         "positifs + faux positifs), soit la proportion de "
                         "predictions de cette classe qui sont correctes. Une "
                         "classe sans aucune prediction positive vaut 0.0 (pas de "
                         "valeur indefinie). Les predictions sont soit un Tenseur "
                         "2D lot x classes (classe retenue par ligne = plus grand "
                         "score), soit un Tenseur 1D d'indices ; les cibles sont "
                         "un Tenseur 1D d'indices attendus (0-based). La moyenne "
                         "macro s'obtient avec ..Moyenne() sur le Vecteur. Comptage "
                         "pur, sans gradient.",
          .descEN      = "Evaluation measure: per-class precision, returned in a "
                         "Vector (1-based position = 0-based class + 1). For each "
                         "class, precision = true positives / (true positives + "
                         "false positives), the share of predictions of that class "
                         "that are correct. A class with no positive prediction is "
                         "0.0 (no undefined value). Predictions are either a 2D "
                         "Tensor batch x classes (class kept per row = highest "
                         "score) or a 1D Tensor of indices; targets are a 1D Tensor "
                         "of expected indices (0-based). The macro average is "
                         "obtained with ..Mean() on the Vector. Pure counting, no "
                         "gradient.",
          .signatures  = sig_prf,
          .impl        = fn_precision,
          .flags       = 0,
        },
        { .canonical   = "Rappel",
          .nameEN      = "Recall",
          .category    = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation : rappel par classe, rendu dans un "
                         "Vecteur (position 1-based = classe 0-based + 1). Pour "
                         "chaque classe, rappel = vrais positifs / (vrais positifs "
                         "+ faux negatifs), soit la proportion d'exemples reels de "
                         "cette classe qui sont retrouves. Une classe sans instance "
                         "reelle vaut 0.0 (pas de valeur indefinie). Les predictions "
                         "sont soit un Tenseur 2D lot x classes (classe retenue par "
                         "ligne = plus grand score), soit un Tenseur 1D d'indices ; "
                         "les cibles sont un Tenseur 1D d'indices attendus (0-based). "
                         "La moyenne macro s'obtient avec ..Moyenne() sur le Vecteur. "
                         "Comptage pur, sans gradient.",
          .descEN      = "Evaluation measure: per-class recall, returned in a Vector "
                         "(1-based position = 0-based class + 1). For each class, "
                         "recall = true positives / (true positives + false "
                         "negatives), the share of actual examples of that class "
                         "that are recovered. A class with no actual instance is 0.0 "
                         "(no undefined value). Predictions are either a 2D Tensor "
                         "batch x classes (class kept per row = highest score) or a "
                         "1D Tensor of indices; targets are a 1D Tensor of expected "
                         "indices (0-based). The macro average is obtained with "
                         "..Mean() on the Vector. Pure counting, no gradient.",
          .signatures  = sig_prf,
          .impl        = fn_rappel,
          .flags       = 0,
        },
        { .canonical   = "F1",
          .nameEN      = "F1",
          .category    = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation : score F1 par classe, rendu dans un "
                         "Vecteur (position 1-based = classe 0-based + 1). Pour "
                         "chaque classe, F1 = 2 x precision x rappel / (precision + "
                         "rappel), la moyenne harmonique des deux. Une classe dont "
                         "precision et rappel sont nuls vaut 0.0 (pas de valeur "
                         "indefinie). Les predictions sont soit un Tenseur 2D lot x "
                         "classes (classe retenue par ligne = plus grand score), "
                         "soit un Tenseur 1D d'indices ; les cibles sont un Tenseur "
                         "1D d'indices attendus (0-based). La moyenne macro "
                         "s'obtient avec ..Moyenne() sur le Vecteur. Comptage pur, "
                         "sans gradient.",
          .descEN      = "Evaluation measure: per-class F1 score, returned in a "
                         "Vector (1-based position = 0-based class + 1). For each "
                         "class, F1 = 2 x precision x recall / (precision + recall), "
                         "the harmonic mean of the two. A class whose precision and "
                         "recall are both zero is 0.0 (no undefined value). "
                         "Predictions are either a 2D Tensor batch x classes (class "
                         "kept per row = highest score) or a 1D Tensor of indices; "
                         "targets are a 1D Tensor of expected indices (0-based). The "
                         "macro average is obtained with ..Mean() on the Vector. Pure "
                         "counting, no gradient.",
          .signatures  = sig_prf,
          .impl        = fn_f1,
          .flags       = 0,
        },
        { .canonical   = "ErreurQuadratiqueMoyenne",
          .nameEN      = "MeanSquaredError",
          .category    = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation de regression : erreur quadratique "
                         "moyenne (MSE), soit la moyenne des carres des ecarts "
                         "entre cibles et predictions. Les deux Tenseurs sont "
                         "continus et de meme forme ; toutes leurs cellules sont "
                         "prises en compte. A distinguer de la perte "
                         "TenseurErreurQuadratique, differentiable et destinee a "
                         "l'entrainement : ici c'est une mesure pure, sans "
                         "gradient, calculee apres coup.",
          .descEN      = "Regression evaluation measure: mean squared error (MSE), "
                         "the average of the squared gaps between targets and "
                         "predictions. Both Tensors are continuous and share the "
                         "same shape; every cell is taken into account. Distinct "
                         "from the TensorMeanSquaredError loss, which is "
                         "differentiable and meant for training: this is a pure "
                         "measure, without gradient, computed afterwards.",
          .signatures  = sig_metrique_reg,
          .impl        = fn_erreur_quadratique_moyenne,
          .flags       = 0,
        },
        { .canonical   = "ErreurAbsolueMoyenne",
          .nameEN      = "MeanAbsoluteError",
          .category    = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation de regression : erreur absolue "
                         "moyenne (MAE), soit la moyenne des ecarts absolus entre "
                         "cibles et predictions. Les deux Tenseurs sont continus "
                         "et de meme forme ; toutes leurs cellules sont prises en "
                         "compte. Mesure pure, sans gradient, calculee apres "
                         "l'entrainement (a ne pas confondre avec une perte "
                         "differentiable).",
          .descEN      = "Regression evaluation measure: mean absolute error (MAE), "
                         "the average of the absolute gaps between targets and "
                         "predictions. Both Tensors are continuous and share the "
                         "same shape; every cell is taken into account. A pure "
                         "measure, without gradient, computed after training (not "
                         "to be confused with a differentiable loss).",
          .signatures  = sig_metrique_reg,
          .impl        = fn_erreur_absolue_moyenne,
          .flags       = 0,
        },
        { .canonical      = "CoefficientD\xc3\xa9" "termination",
          .canonicalAscii = "CoefficientDetermination",
          .nameEN         = "RSquared",
          .category       = "M\xc3\xa9triques d'\xc3\xa9valuation",
          .descFR      = "Mesure d'evaluation de regression : coefficient de "
                         "determination R² = 1 - SS_res / SS_tot, ou SS_res est la "
                         "somme des carres des residus (cible - prediction) et "
                         "SS_tot la somme des carres des ecarts a la moyenne des "
                         "cibles. Vaut 1.0 pour un ajustement parfait, 0.0 pour un "
                         "modele qui predit uniformement la moyenne, et peut "
                         "devenir negatif pour un modele pire que cette moyenne "
                         "(aucune borne basse). Convention deterministe : cibles "
                         "toutes egales (SS_tot = 0) -> 0.0, jamais de valeur "
                         "indefinie. Mesure pure, sans gradient.",
          .descEN      = "Regression evaluation measure: coefficient of "
                         "determination R² = 1 - SS_res / SS_tot, where SS_res is "
                         "the sum of squared residuals (target - prediction) and "
                         "SS_tot the sum of squared deviations from the target "
                         "mean. It is 1.0 for a perfect fit, 0.0 for a model that "
                         "predicts the mean everywhere, and can go negative for a "
                         "model worse than that mean (no lower bound). "
                         "Deterministic convention: all targets equal (SS_tot = 0) "
                         "-> 0.0, never an undefined value. A pure measure, without "
                         "gradient.",
          .signatures  = sig_metrique_reg,
          .impl        = fn_coefficient_determination,
          .flags       = 0,
        },
    };

    owl_registry_register_types(TENSEUR_TYPES,
        (int)(sizeof(TENSEUR_TYPES) / sizeof(TENSEUR_TYPES[0])));
    owl_registry_register_module(DEFS_TENSEUR,
        (int)(sizeof(DEFS_TENSEUR) / sizeof(DEFS_TENSEUR[0])));
    owl_register_instance_coerce("Tenseur", parse_to_tenseur);

    /* Retropropagation — receveur seul (arite 1). Bilingue Retropropage /
     * Backpropagate. */
    owl_register_method(METHOD_TYPE_TENSOR, "R\xc3\xa9tropropage",
                        fn_tenseur_retropropage, 1);
    owl_register_method(METHOD_TYPE_TENSOR, "Backpropagate",
                        fn_tenseur_retropropage, 1);

    /* Ops de couche fluentes (phase 2). Produit matriciel = receveur + 1
     * operande (arite 2) ; activations = receveur seul (arite 1). ReLU et Tanh
     * identiques FR/EN ; Sigmoide a une forme EN distincte (Sigmoid). */
    owl_register_method(METHOD_TYPE_TENSOR, "ProduitMatriciel",
                        fn_tenseur_produit_matriciel, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "MatrixProduct",
                        fn_tenseur_produit_matriciel, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "ReLU", fn_tenseur_relu, 1);
    owl_register_method(METHOD_TYPE_TENSOR, "Sigmo\xc3\xaf" "de",
                        fn_tenseur_sigmoide, 1);
    owl_register_method(METHOD_TYPE_TENSOR, "Sigmoid", fn_tenseur_sigmoide, 1);
    owl_register_method(METHOD_TYPE_TENSOR, "Tanh", fn_tenseur_tanh, 1);

    /* Pertes fluentes (phase 3) : receveur (prediction) + 1 operande (cible),
     * arite 2. MSE = ErreurQuadratique / MeanSquaredError ; entropie croisee =
     * EntropieCroisee / CrossEntropy. */
    owl_register_method(METHOD_TYPE_TENSOR, "ErreurQuadratique",
                        fn_tenseur_erreur_quadratique, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "MeanSquaredError",
                        fn_tenseur_erreur_quadratique, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "EntropieCrois\xc3\xa9" "e",
                        fn_tenseur_entropie_croisee, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "CrossEntropy",
                        fn_tenseur_entropie_croisee, 2);

    /*  — pertes de detection : entropie croisee binaire (sigmoide
     * fusionnee, logits bruts) = EntropieCroiseeBinaire / BinaryCrossEntropy ;
     * L1 (erreur absolue moyenne) = L1 (nom neutre, une seule entree). */
    owl_register_method(METHOD_TYPE_TENSOR, "EntropieCrois\xc3\xa9" "eBinaire",
                        fn_tenseur_entropie_croisee_binaire, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "BinaryCrossEntropy",
                        fn_tenseur_entropie_croisee_binaire, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "L1", fn_tenseur_l1, 2);

    /*  — entropie croisee sur indices (sparse) : receveur (logits) +
     * indices, arite 2. Encodage one-hot : receveur (indices) + nbClasses,
     * arite 2. Bilingue FR/EN. */
    owl_register_method(METHOD_TYPE_TENSOR, "EntropieCrois\xc3\xa9" "eIndices",
                        fn_tenseur_entropie_croisee_indices, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "CrossEntropyIndices",
                        fn_tenseur_entropie_croisee_indices, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "UnParmiN", fn_tenseur_unparmin, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "OneHot", fn_tenseur_unparmin, 2);

    /* Descente de gradient fluente : receveur (parametre) + taux, arite 2.
     * Bilingue DescenteGradient / GradientDescent. */
    owl_register_method(METHOD_TYPE_TENSOR, "DescenteGradient",
                        fn_tenseur_descente_gradient, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "GradientDescent",
                        fn_tenseur_descente_gradient, 2);

    /* Ops de forme N-D fluentes. Redimensionne/Somme/Moyenne =
     * receveur + 1 operande (arite 2) ; Permute = receveur + ordre (arite 2) ;
     * Transpose = receveur + 2 axes (arite 3). Bilingue FR/EN. */
    owl_register_method(METHOD_TYPE_TENSOR, "Redimensionne",
                        fn_tenseur_redimensionne, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "Reshape",
                        fn_tenseur_redimensionne, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "Permute", fn_tenseur_permute, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "Transpose", fn_tenseur_transpose, 3);
    owl_register_method(METHOD_TYPE_TENSOR, "Somme", fn_tenseur_somme, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "Sum", fn_tenseur_somme, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "Moyenne", fn_tenseur_moyenne, 2);
    owl_register_method(METHOD_TYPE_TENSOR, "Mean", fn_tenseur_moyenne, 2);

    /* Tranche de canaux differentiable : receveur + debut +
     * longueur (arite 3, 1-based). Bilingue TrancheCanaux / ChannelSlice. */
    owl_register_method(METHOD_TYPE_TENSOR, "TrancheCanaux",
                        fn_tenseur_tranchecanaux, 3);
    owl_register_method(METHOD_TYPE_TENSOR, "ChannelSlice",
                        fn_tenseur_tranchecanaux, 3);

    /* Ops de convolution fluentes. Convolution2D = receveur + poids
     * + pas + remplissage [+ group] (arite 4 ou 5, group optionnel ,
     * d'ou -1) ; pooling = receveur + taille [+ pas] (arite 2 ou 3, ) ;
     * NormalisationLot = receveur + gamma + beta + epsilon (arite 4). Bilingue :
     * Convolution2D et MaxPooling identiques FR/EN ; MoyennePooling/
     * AveragePooling et NormalisationLot/BatchNorm distincts. */
    owl_register_method(METHOD_TYPE_TENSOR, "Convolution2D",
                        fn_tenseur_convolution2d, -1);
    owl_register_method(METHOD_TYPE_TENSOR, "MaxPooling",
                        fn_tenseur_maxpooling, -1);
    owl_register_method(METHOD_TYPE_TENSOR, "MoyennePooling",
                        fn_tenseur_moyennepooling, -1);
    owl_register_method(METHOD_TYPE_TENSOR, "AveragePooling",
                        fn_tenseur_moyennepooling, -1);
    owl_register_method(METHOD_TYPE_TENSOR, "NormalisationLot",
                        fn_tenseur_normalisationlot, 4);
    owl_register_method(METHOD_TYPE_TENSOR, "BatchNorm",
                        fn_tenseur_normalisationlot, 4);

    /* Standardisation z-score par canal — receveur + epsilon
     * OPTIONNEL, d'ou l'arite variadic (-1) : `.Standardise()` (epsilon defaut)
     * ou `.Standardise(eps)`. Bilingue Standardise / Standardize. */
    owl_register_method(METHOD_TYPE_TENSOR, "Standardise",
                        fn_tenseur_standardise, -1);
    owl_register_method(METHOD_TYPE_TENSOR, "Standardize",
                        fn_tenseur_standardise, -1);
}
