// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_mlarbre.c — Type `MLArbreDécision` / `MLDecisionTree`.
 *
 * Premier modèle de machine learning classique d'OWL et socle du patron
 * d'estimateur ML tabulaire (arbres -> forêts -> boosting). Un arbre de
 * décision CART hand-written en C (zéro dépendance externe) : split glouton
 * par réduction d'impureté, prédiction récursive. Un seul type couvre la
 * classification ET la régression via un objectif EXPLICITE (..Objectif),
 * jamais inféré du jeu de données.
 *
 * Modèle mental :
 *   - Le type porte les hyperparamètres (objectif, profondeur max, tailles
 *     minimales) réglés par double point, et l'arbre appris (structure C-heap
 *     pure, libérée au finaliseur GC — l'ObjMLArbre est une feuille GC).
 *   - `.Entraîne(X, y)` construit l'arbre : X = Matrice N×variables, y =
 *     Tenseur 1D de cibles (indices de classe 0-based en classification,
 *     valeurs continues en régression).
 *   - `.Prédit(X)` rend un Tenseur 1D, une prédiction par ligne.
 *
 * Algorithme CART (déterministe) :
 *   - Split glouton : pour chaque colonne, seuils candidats = milieux entre
 *     valeurs adjacentes triées ; on garde la réduction d'impureté maximale.
 *     Classification : Gini pondéré = (nG/n)·GiniG + (nD/n)·GiniD.
 *     Régression      : SSE pondéré = SSE_G + SSE_D.
 *   - Arrêt d'un nœud (-> feuille) : nœud pur, OU profondeur == ProfondeurMax
 *     (si > 0), OU n < MinÉchantillonsSéparation, OU aucun split ne respecte
 *     MinÉchantillonsFeuille sur les deux enfants.
 *   - Feuille : classe majoritaire (classification) ou moyenne (régression).
 *   - Profondeur : racine = 0 ; ProfondeurMax = N autorise N niveaux de split ;
 *     ProfondeurMax = 0 = illimité.
 *   - Tie-break déterministe : à réduction d'impureté égale, première colonne
 *     puis plus petit seuil (premier candidat rencontré).
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "object.h"
#include "sentinels.h"
#include "vm.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * PRNG local reproductible ( — forêt aléatoire)
 *
 * Générateur xorshift64 seedé par ..Graine, ENCAPSULÉ dans l'entraînement de
 * la forêt (jamais l'état global Hasard/InitHasard). C'est ce qui garantit la
 * reproductibilité stricte du ticket : deux forêts entraînées avec la même
 * ..Graine consomment la MÊME séquence, sans dépendre d'un état partagé
 * modifié entre les deux. ..Graine=0 = une graine fixe (jamais "pas de
 * graine") : le mélange splitmix64 mappe 0 sur un état non nul déterministe.
 * ================================================================ */

/* Dérive un état 64 bits non nul depuis la graine (splitmix64). Seed 0 ->
 * état fixe reproductible ; l'état résultant est toujours ≠ 0 (xorshift
 * l'exige). */
static uint64_t ml_rng_seed(int64_t seed) {
    uint64_t z = (uint64_t)seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return z | 1ULL;   /* jamais 0 */
}

/* Prochain entier pseudo-aléatoire (xorshift64, période 2^64-1). */
static uint64_t ml_rng_next(uint64_t* s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* ================================================================
 * Lecture des données d'entrée
 * ================================================================ */

/* Extrait d'une valeur X une matrice d'échantillons row-major : `data`
 * (rows×cols contigus), `rows`, `cols`. Accepte une Matrice ou un Tenseur 2D.
 * Rend false si le type n'est pas exploitable. */
static bool mlarbre_read_matrix(Value v, const double** data, int* rows, int* cols) {
    if (IS_MATRIX(v)) {
        ObjMatrix* m = AS_MATRIX(v);
        *data = m->data; *rows = m->rows; *cols = m->cols;
        return true;
    }
    if (IS_TENSOR(v)) {
        ObjTensor* t = AS_TENSOR(v);
        if (t->ndim != 2) return false;
        *data = tensor_cells(t->data);
        *rows = t->shape[0];
        *cols = t->shape[1];
        return true;
    }
    return false;
}

/* Extrait d'une valeur y un vecteur plat de cibles : `data` + `count`.
 * Accepte un Tenseur, un Vecteur ou une Matrice (aplatie row-major). */
static bool mlarbre_read_targets(Value v, const double** data, int* count) {
    if (IS_TENSOR(v)) {
        ObjTensor* t = AS_TENSOR(v);
        *data = tensor_cells(t->data);
        *count = tensor_numel(t->data);
        return true;
    }
    if (IS_VECTOR(v)) {
        ObjVector* vec = AS_VECTOR(v);
        *data = vec->data; *count = vec->count;
        return true;
    }
    if (IS_MATRIX(v)) {
        ObjMatrix* m = AS_MATRIX(v);
        *data = m->data; *count = m->rows * m->cols;
        return true;
    }
    return false;
}

/* ================================================================
 * Construction CART
 * ================================================================ */

typedef struct {
    const double* X;        /* row-major nrows×ncols */
    const double* y;        /* nrows cibles */
    int ncols;
    int objective;          /* OWL_ML_OBJ_CLASSIFICATION / _REGRESSION */
    int maxDepth;           /* 0 = illimité */
    int minSamplesSplit;
    int minSamplesLeaf;
    int nClasses;           /* classification : plus grand indice de classe + 1 */
    /*  — sous-échantillonnage de variables (forêt). INACTIF pour un
     * MLArbreDécision seul : rngState = NULL et featScratch en ordre identité,
     * ce qui reproduit EXACTEMENT le parcours de colonnes de l'arbre. */
    int maxFeatures;        /* variables candidates par split ; 0 ou >= ncols = toutes */
    uint64_t* rngState;     /* PRNG local ; NULL = pas de tirage (arbre seul) */
    int* featScratch;       /* permutation de [0..ncols), fournie par le caller */
} MLBuildCtx;

/* Indice de classe 0-based d'une cible (les cibles sont des réels entiers). */
static int mlarbre_class_of(double yv) {
    int c = (int)lround(yv);
    return c < 0 ? 0 : c;
}

/* Impureté de Gini d'un sous-ensemble de lignes (rows[0..count)). */
static double mlarbre_gini(const MLBuildCtx* ctx, const int* rows, int count) {
    if (count == 0) return 0.0;
    int* counts = (int*)calloc((size_t)ctx->nClasses, sizeof(int));
    for (int i = 0; i < count; i++) counts[mlarbre_class_of(ctx->y[rows[i]])]++;
    double sum = 0.0;
    for (int c = 0; c < ctx->nClasses; c++) {
        double p = (double)counts[c] / (double)count;
        sum += p * p;
    }
    free(counts);
    return 1.0 - sum;
}

/* Somme des carrés des écarts à la moyenne (SSE) d'un sous-ensemble. */
static double mlarbre_sse(const MLBuildCtx* ctx, const int* rows, int count) {
    if (count == 0) return 0.0;
    double mean = 0.0;
    for (int i = 0; i < count; i++) mean += ctx->y[rows[i]];
    mean /= (double)count;
    double sse = 0.0;
    for (int i = 0; i < count; i++) {
        double d = ctx->y[rows[i]] - mean;
        sse += d * d;
    }
    return sse;
}

/* Impureté d'un nœud selon l'objectif. */
static double mlarbre_impurity(const MLBuildCtx* ctx, const int* rows, int count) {
    return ctx->objective == OWL_ML_OBJ_CLASSIFICATION
         ? mlarbre_gini(ctx, rows, count)
         : mlarbre_sse(ctx, rows, count);
}

/* Valeur d'une feuille : classe majoritaire (classif, plus petit indice en cas
 * d'égalité) ou moyenne des cibles (régression). */
static double mlarbre_leaf_value(const MLBuildCtx* ctx, const int* rows, int count) {
    if (ctx->objective == OWL_ML_OBJ_CLASSIFICATION) {
        int* counts = (int*)calloc((size_t)ctx->nClasses, sizeof(int));
        for (int i = 0; i < count; i++) counts[mlarbre_class_of(ctx->y[rows[i]])]++;
        int best = 0;
        for (int c = 1; c < ctx->nClasses; c++)
            if (counts[c] > counts[best]) best = c;   /* > garde le plus petit indice sur égalité */
        free(counts);
        return (double)best;
    }
    double mean = 0.0;
    for (int i = 0; i < count; i++) mean += ctx->y[rows[i]];
    return mean / (double)count;
}

/* Tri par insertion des lignes `rows` (count) par la valeur de la colonne
 * `col`, dans `sortedRows` ; dépose les valeurs triées dans `sortedVals`. */
static void mlarbre_sort_by_feature(const MLBuildCtx* ctx, const int* rows,
                                    int count, int col,
                                    int* sortedRows, double* sortedVals) {
    for (int i = 0; i < count; i++) {
        int r = rows[i];
        double v = ctx->X[(size_t)r * ctx->ncols + col];
        int j = i - 1;
        while (j >= 0 && sortedVals[j] > v) {
            sortedVals[j + 1] = sortedVals[j];
            sortedRows[j + 1] = sortedRows[j];
            j--;
        }
        sortedVals[j + 1] = v;
        sortedRows[j + 1] = r;
    }
}

/* Impureté pondérée des deux enfants d'un split (gauche = sortedRows[0..nL),
 * droite = sortedRows[nL..nL+nR)). Classif : moyenne pondérée par la taille ;
 * régression : somme des SSE. La valeur à MINIMISER. */
static double mlarbre_split_score(const MLBuildCtx* ctx, const int* sortedRows,
                                  int nL, int nR) {
    const int* left  = sortedRows;
    const int* right = sortedRows + nL;
    if (ctx->objective == OWL_ML_OBJ_CLASSIFICATION) {
        int n = nL + nR;
        return ((double)nL / n) * mlarbre_gini(ctx, left, nL)
             + ((double)nR / n) * mlarbre_gini(ctx, right, nR);
    }
    return mlarbre_sse(ctx, left, nL) + mlarbre_sse(ctx, right, nR);
}

/* Construit récursivement le sous-arbre pour les lignes `rows` (count) à la
 * profondeur `depth`. `rows` appartient au caller (non libéré ici). Aucune
 * allocation OWL : uniquement du C-heap, donc aucun cycle GC déclenché. Rend
 * NULL si malloc échoue (propagé -> arbre partiel, jamais lu si NULL au sommet). */
static MLTreeNode* mlarbre_build(const MLBuildCtx* ctx, const int* rows,
                                 int count, int depth) {
    MLTreeNode* node = (MLTreeNode*)calloc(1, sizeof(MLTreeNode));
    if (node == NULL) return NULL;
    node->isLeaf = true;
    node->value = mlarbre_leaf_value(ctx, rows, count);

    /* Conditions d'arrêt -> feuille. */
    if (mlarbre_impurity(ctx, rows, count) <= 0.0) return node;   /* nœud pur */
    if (count < ctx->minSamplesSplit) return node;
    if (ctx->maxDepth > 0 && depth >= ctx->maxDepth) return node;

    /* Recherche du meilleur split (réduction d'impureté maximale = score
     * pondéré minimal). Tie-break : première colonne, plus petit seuil — obtenu
     * en n'écrasant le meilleur que sur amélioration STRICTE. */
    int*    sortedRows = (int*)malloc((size_t)count * sizeof(int));
    double* sortedVals = (double*)malloc((size_t)count * sizeof(double));
    int bestFeat = -1;
    double bestThr = 0.0;
    double bestScore = INFINITY;

    /*  — variables candidates de ce split. Sans contrainte (arbre
     * seul, ou maxFeatures auto == ncols), on parcourt les colonnes dans
     * l'ordre identité de featScratch [0,1,2,...] : tie-break inchangé
     * (première colonne, plus petit seuil). Avec contrainte active (forêt),
     * un Fisher-Yates partiel tire `kFeat` colonnes distinctes via le PRNG
     * local — featScratch reste une permutation de [0..ncols) entre appels. */
    int kFeat = ctx->ncols;
    if (ctx->rngState != NULL && ctx->maxFeatures > 0
        && ctx->maxFeatures < ctx->ncols) {
        kFeat = ctx->maxFeatures;
        for (int i = 0; i < kFeat; i++) {
            int span = ctx->ncols - i;
            int j = i + (int)(ml_rng_next(ctx->rngState) % (uint64_t)span);
            int tmp = ctx->featScratch[i];
            ctx->featScratch[i] = ctx->featScratch[j];
            ctx->featScratch[j] = tmp;
        }
    }

    for (int fi = 0; fi < kFeat; fi++) {
        int c = ctx->featScratch[fi];
        mlarbre_sort_by_feature(ctx, rows, count, c, sortedRows, sortedVals);
        for (int i = 0; i < count - 1; i++) {
            if (sortedVals[i] == sortedVals[i + 1]) continue;   /* pas de frontière */
            int nL = i + 1;
            int nR = count - nL;
            if (nL < ctx->minSamplesLeaf || nR < ctx->minSamplesLeaf) continue;
            double score = mlarbre_split_score(ctx, sortedRows, nL, nR);
            if (score < bestScore) {
                bestScore = score;
                bestFeat = c;
                bestThr = (sortedVals[i] + sortedVals[i + 1]) / 2.0;
            }
        }
    }
    free(sortedRows);
    free(sortedVals);

    if (bestFeat < 0) return node;   /* aucun split valide -> feuille */

    /* Partition des lignes selon le meilleur seuil, puis récursion. */
    int* leftRows  = (int*)malloc((size_t)count * sizeof(int));
    int* rightRows = (int*)malloc((size_t)count * sizeof(int));
    int nL = 0, nR = 0;
    for (int i = 0; i < count; i++) {
        int r = rows[i];
        if (ctx->X[(size_t)r * ctx->ncols + bestFeat] <= bestThr) leftRows[nL++] = r;
        else                                                       rightRows[nR++] = r;
    }
    node->isLeaf = false;
    node->feature = bestFeat;
    node->threshold = bestThr;
    node->left  = mlarbre_build(ctx, leftRows, nL, depth + 1);
    node->right = mlarbre_build(ctx, rightRows, nR, depth + 1);
    free(leftRows);
    free(rightRows);
    return node;
}

/* Prédiction d'une ligne : descend l'arbre jusqu'à une feuille. */
static double mlarbre_predict_one(const MLTreeNode* node, const double* xrow) {
    while (!node->isLeaf) {
        node = (xrow[node->feature] <= node->threshold) ? node->left : node->right;
    }
    return node->value;
}

/* ================================================================
 * Factory + méthodes
 * ================================================================ */

/* Factory interne émise par le compilateur pour `arbre est un MLArbreDécision`
 * (instance fraîche à chaque déclaration). */
static Value fn_mlarbre_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_mlarbre());
}

/* .Entraîne(X, y) / .Train(X, y) — construit l'arbre CART. Objectif obligatoire
 * (soft-error sinon), tailles X/y cohérentes (soft-error sinon). Rend le
 * receveur (chaînage fluide). */
static Value fn_mlarbre_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjMLArbre* a = AS_MLARBRE(args[0]);

    if (a->objective != OWL_ML_OBJ_CLASSIFICATION
        && a->objective != OWL_ML_OBJ_REGRESSION) {
        owl_emit_native_error(E_NATIVE_ML_OBJECTIF_NON_REGLE, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    const double* X; int nrows, ncols;
    if (!mlarbre_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    const double* y; int ny;
    if (!mlarbre_read_targets(args[2], &y, &ny)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "y",
                              "expected", "Tenseur 1D de cibles",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    if (nrows != ny) {
        owl_emit_native_error(E_NATIVE_ML_TAILLE_XY, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    /* Réentraînement : libère l'arbre précédent. */
    obj_mlarbre_free_tree(a->root);
    a->root = NULL;

    MLBuildCtx ctx;
    ctx.X = X;
    ctx.y = y;
    ctx.ncols = ncols;
    ctx.objective = a->objective;
    ctx.maxDepth = a->maxDepth;
    ctx.minSamplesSplit = a->minSamplesSplit;
    ctx.minSamplesLeaf = a->minSamplesLeaf;
    ctx.nClasses = 1;
    if (a->objective == OWL_ML_OBJ_CLASSIFICATION) {
        int maxc = 0;
        for (int i = 0; i < nrows; i++) {
            int c = mlarbre_class_of(y[i]);
            if (c > maxc) maxc = c;
        }
        ctx.nClasses = maxc + 1;
    }
    /* Arbre seul : sous-échantillonnage de variables INACTIF (rngState NULL),
     * parcours de colonnes en ordre identité -> comportement CART inchangé. */
    ctx.maxFeatures = 0;
    ctx.rngState = NULL;
    int* featScratch = (int*)malloc((size_t)ncols * sizeof(int));
    for (int c = 0; c < ncols; c++) featScratch[c] = c;
    ctx.featScratch = featScratch;

    /* Aucune allocation OWL pendant la construction (X/y restent valides via
     * args, rootés par le dispatch) : arbre 100 % C-heap. */
    int* idx = (int*)malloc((size_t)nrows * sizeof(int));
    for (int i = 0; i < nrows; i++) idx[i] = i;
    a->root = mlarbre_build(&ctx, idx, nrows, 0);
    free(idx);
    free(featScratch);

    return args[0];
}

/* .Prédit(X) / .Predict(X) — rend un Tenseur 1D, une prédiction par ligne. */
static Value fn_mlarbre_predit(int argCount, Value* args) {
    (void)argCount;
    ObjMLArbre* a = AS_MLARBRE(args[0]);
    if (a->root == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""dit", NULL);
        return NIL_VAL;
    }
    const double* X; int nrows, ncols;
    if (!mlarbre_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""dit",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    /* Alloue d'abord le Tenseur de sortie (peut déclencher un GC ; X reste
     * rooté via args[1], l'arbre via args[0]). Ensuite, remplissage sans
     * aucune allocation -> X et le tenseur restent sains. */
    ObjTensor* out = obj_new_tensor(nrows);
    double* cells = tensor_cells(out->data);
    for (int i = 0; i < nrows; i++) {
        cells[i] = mlarbre_predict_one(a->root, &X[(size_t)i * ncols]);
    }
    return OBJ_VAL(out);
}

/* ================================================================
 * Forêt aléatoire — ensemble d'arbres CART
 * ================================================================ */

/* Factory interne émise par le compilateur pour `f est un MLForêtAléatoire`. */
static Value fn_mlforet_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_mlforet());
}

/* .Entraîne(X, y) / .Train(X, y) — construit ..NbArbres arbres CART, chacun sur
 * un échantillon bootstrap des lignes (tirage avec remise) et un sous-ensemble
 * aléatoire de ..VariablesParSéparation variables par split. Tout le hasard est
 * piloté par un PRNG local seedé par ..Graine (reproductible). Objectif
 * obligatoire (soft-error sinon), tailles X/y cohérentes. Rend le receveur. */
static Value fn_mlforet_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjMLForet* f = AS_MLFORET(args[0]);

    if (f->objective != OWL_ML_OBJ_CLASSIFICATION
        && f->objective != OWL_ML_OBJ_REGRESSION) {
        owl_emit_native_error(E_NATIVE_ML_OBJECTIF_NON_REGLE, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    const double* X; int nrows, ncols;
    if (!mlarbre_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    const double* y; int ny;
    if (!mlarbre_read_targets(args[2], &y, &ny)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "y",
                              "expected", "Tenseur 1D de cibles",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    if (nrows != ny) {
        owl_emit_native_error(E_NATIVE_ML_TAILLE_XY, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    /* Réentraînement : libère l'ensemble précédent. */
    obj_mlforet_free_trees(f);

    int nTrees = f->numTrees > 0 ? f->numTrees : 1;

    MLBuildCtx ctx;
    ctx.X = X;
    ctx.y = y;
    ctx.ncols = ncols;
    ctx.objective = f->objective;
    ctx.maxDepth = f->maxDepth;
    ctx.minSamplesSplit = f->minSamplesSplit;
    ctx.minSamplesLeaf = f->minSamplesLeaf;
    ctx.nClasses = 1;
    if (f->objective == OWL_ML_OBJ_CLASSIFICATION) {
        int maxc = 0;
        for (int i = 0; i < nrows; i++) {
            int c = mlarbre_class_of(y[i]);
            if (c > maxc) maxc = c;
        }
        ctx.nClasses = maxc + 1;
    }
    f->nClasses = ctx.nClasses;

    /* Variables candidates par split. 0 = auto : √nb variables en
     * classification (règle usuelle), toutes en régression. */
    int effMaxFeat = f->maxFeatures;
    if (effMaxFeat <= 0) {
        effMaxFeat = (f->objective == OWL_ML_OBJ_CLASSIFICATION)
                   ? (int)floor(sqrt((double)ncols))
                   : ncols;
    }
    if (effMaxFeat < 1) effMaxFeat = 1;
    if (effMaxFeat > ncols) effMaxFeat = ncols;
    ctx.maxFeatures = effMaxFeat;

    /* PRNG LOCAL seedé par ..Graine : séquence intégralement reproductible,
     * indépendante de tout état global. */
    uint64_t rng = ml_rng_seed(f->seed);
    ctx.rngState = &rng;

    /* Colonnes candidates (permutation de [0..ncols)), initialisée à l'identité
     * et mélangée par Fisher-Yates partiel à chaque split (cf. mlarbre_build). */
    int* featScratch = (int*)malloc((size_t)ncols * sizeof(int));
    for (int c = 0; c < ncols; c++) featScratch[c] = c;
    ctx.featScratch = featScratch;

    /* Aucune allocation OWL pendant toute la construction (X/y rootés via args) :
     * les arbres sont 100 % C-heap, libérés au finaliseur GC de la forêt. */
    f->trees = (MLTreeNode**)malloc((size_t)nTrees * sizeof(MLTreeNode*));
    f->treeCount = 0;
    int* bootRows = (int*)malloc((size_t)nrows * sizeof(int));
    for (int t = 0; t < nTrees; t++) {
        for (int i = 0; i < nrows; i++) {
            bootRows[i] = (int)(ml_rng_next(&rng) % (uint64_t)nrows);
        }
        f->trees[t] = mlarbre_build(&ctx, bootRows, nrows, 0);
        f->treeCount++;
    }
    free(bootRows);
    free(featScratch);

    return args[0];
}

/* .Prédit(X) / .Predict(X) — agrège les prédictions des arbres : vote
 * majoritaire (classification, plus petit indice sur égalité) ou moyenne
 * (régression). Rend un Tenseur 1D, une prédiction par ligne. */
static Value fn_mlforet_predit(int argCount, Value* args) {
    (void)argCount;
    ObjMLForet* f = AS_MLFORET(args[0]);
    if (f->trees == NULL || f->treeCount == 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""dit", NULL);
        return NIL_VAL;
    }
    const double* X; int nrows, ncols;
    if (!mlarbre_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""dit",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }

    /* Alloue d'abord le Tenseur de sortie (peut déclencher un GC ; X reste
     * rooté via args[1], la forêt via args[0]). Ensuite, agrégation sans
     * aucune allocation OWL -> out et X restent sains. */
    ObjTensor* out = obj_new_tensor(nrows);
    double* cells = tensor_cells(out->data);

    if (f->objective == OWL_ML_OBJ_CLASSIFICATION) {
        int nc = f->nClasses > 0 ? f->nClasses : 1;
        int* votes = (int*)calloc((size_t)nc, sizeof(int));
        for (int i = 0; i < nrows; i++) {
            memset(votes, 0, (size_t)nc * sizeof(int));
            for (int t = 0; t < f->treeCount; t++) {
                int cls = mlarbre_class_of(
                    mlarbre_predict_one(f->trees[t], &X[(size_t)i * ncols]));
                if (cls >= nc) cls = nc - 1;   /* garde-fou (jamais atteint) */
                votes[cls]++;
            }
            int best = 0;
            for (int c = 1; c < nc; c++)
                if (votes[c] > votes[best]) best = c;   /* > : plus petit indice sur égalité */
            cells[i] = (double)best;
        }
        free(votes);
    } else {
        for (int i = 0; i < nrows; i++) {
            double sum = 0.0;
            for (int t = 0; t < f->treeCount; t++)
                sum += mlarbre_predict_one(f->trees[t], &X[(size_t)i * ncols]);
            cells[i] = sum / (double)f->treeCount;
        }
    }
    return OBJ_VAL(out);
}

/* ================================================================
 * Gradient boosting — ensemble SÉQUENTIEL d'arbres CART
 *
 * Le boosting construit TOUJOURS des arbres de RÉGRESSION en interne (ils
 * fittent des pseudo-résidus continus), quel que soit l'objectif : il réutilise
 * donc le chemin CART régression de mlarbre_build (ctx.objective = REGRESSION,
 * ctx.y pointant sur les résidus de l'étape courante), et mlarbre_predict_one
 * pour la prédiction. DÉTERMINISTE : rngState NULL, aucun tirage.
 * ================================================================ */

/* Factory interne émise par le compilateur pour `gb est un MLGradientBoosting`. */
static Value fn_mlboosting_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_mlboosting());
}

/* .Entraîne(X, y) / .Train(X, y) — construit l'ensemble de boosting. En
 * régression : F0 = moyenne(y), puis à chaque étape un arbre fitté aux résidus
 * y − F(x), F += taux·arbre. En classification multi-classes (K = max(y)+1) :
 * softmax des K scores + un arbre par classe fitté aux pseudo-résidus
 * (one-hot − proba), F_k += taux·arbre. Objectif obligatoire (soft-error sinon),
 * tailles X/y cohérentes (soft-error sinon). Rend le receveur (chaînage). */
static Value fn_mlboosting_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjMLBoosting* b = AS_MLBOOSTING(args[0]);

    if (b->objective != OWL_ML_OBJ_CLASSIFICATION
        && b->objective != OWL_ML_OBJ_REGRESSION) {
        owl_emit_native_error(E_NATIVE_ML_OBJECTIF_NON_REGLE, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    const double* X; int nrows, ncols;
    if (!mlarbre_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    const double* y; int ny;
    if (!mlarbre_read_targets(args[2], &y, &ny)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "y",
                              "expected", "Tenseur 1D de cibles",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    if (nrows != ny) {
        owl_emit_native_error(E_NATIVE_ML_TAILLE_XY, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    /* Réentraînement : libère l'ensemble précédent. */
    obj_mlboosting_free_trees(b);

    int nSteps = b->numTrees > 0 ? b->numTrees : 1;
    double lr = b->learningRate;

    /* Contexte CART commun : TOUJOURS des arbres de régression (fittent des
     * pseudo-résidus continus), quel que soit l'objectif du boosting. Aucun
     * tirage (rngState NULL) -> parcours de colonnes en ordre identité,
     * comportement CART strictement déterministe. */
    MLBuildCtx ctx;
    ctx.X = X;
    ctx.y = NULL;                          /* re-pointé sur les résidus à chaque arbre */
    ctx.ncols = ncols;
    ctx.objective = OWL_ML_OBJ_REGRESSION;
    ctx.maxDepth = b->maxDepth;
    ctx.minSamplesSplit = b->minSamplesSplit;
    ctx.minSamplesLeaf = b->minSamplesLeaf;
    ctx.nClasses = 1;
    ctx.maxFeatures = 0;
    ctx.rngState = NULL;
    int* featScratch = (int*)malloc((size_t)ncols * sizeof(int));
    for (int c = 0; c < ncols; c++) featScratch[c] = c;
    ctx.featScratch = featScratch;

    /* Toutes les lignes participent à chaque arbre (pas de bootstrap). */
    int* idx = (int*)malloc((size_t)nrows * sizeof(int));
    for (int i = 0; i < nrows; i++) idx[i] = i;

    /* Aucune allocation OWL pendant toute la construction (X/y rootés via args) :
     * arbres, scores et tampons sont 100 % C-heap, libérés au finaliseur GC. */
    if (b->objective == OWL_ML_OBJ_REGRESSION) {
        b->nClasses = 1;
        b->nSteps = nSteps;
        b->init0 = (double*)malloc(sizeof(double));
        b->trees = (MLTreeNode**)malloc((size_t)nSteps * sizeof(MLTreeNode*));
        b->treeCount = 0;

        double mean = 0.0;
        for (int i = 0; i < nrows; i++) mean += y[i];
        mean /= (double)nrows;
        b->init0[0] = mean;

        double* F     = (double*)malloc((size_t)nrows * sizeof(double));
        double* resid = (double*)malloc((size_t)nrows * sizeof(double));
        for (int i = 0; i < nrows; i++) F[i] = mean;

        for (int m = 0; m < nSteps; m++) {
            for (int i = 0; i < nrows; i++) resid[i] = y[i] - F[i];
            ctx.y = resid;
            MLTreeNode* h = mlarbre_build(&ctx, idx, nrows, 0);
            b->trees[m] = h;
            b->treeCount++;
            for (int i = 0; i < nrows; i++)
                F[i] += lr * mlarbre_predict_one(h, &X[(size_t)i * ncols]);
        }
        free(F);
        free(resid);
    } else {
        /* Classification multi-classes : K scores par échantillon, softmax + un
         * arbre par classe et par étape. Le binaire est le cas K=2. */
        int K = 1;
        for (int i = 0; i < nrows; i++) {
            int c = mlarbre_class_of(y[i]);
            if (c + 1 > K) K = c + 1;
        }
        b->nClasses = K;
        b->nSteps = nSteps;
        b->init0 = (double*)calloc((size_t)K, sizeof(double));   /* F0_k = 0 */
        b->trees = (MLTreeNode**)malloc((size_t)nSteps * (size_t)K * sizeof(MLTreeNode*));
        b->treeCount = 0;

        double* F     = (double*)calloc((size_t)nrows * (size_t)K, sizeof(double));
        double* P     = (double*)malloc((size_t)nrows * (size_t)K * sizeof(double));
        double* resid = (double*)malloc((size_t)nrows * sizeof(double));

        for (int m = 0; m < nSteps; m++) {
            /* softmax(F) par échantillon (stabilité : soustraction du max). */
            for (int i = 0; i < nrows; i++) {
                double* Fi = &F[(size_t)i * K];
                double mx = Fi[0];
                for (int k = 1; k < K; k++) if (Fi[k] > mx) mx = Fi[k];
                double sum = 0.0;
                for (int k = 0; k < K; k++) {
                    double e = exp(Fi[k] - mx);
                    P[(size_t)i * K + k] = e;
                    sum += e;
                }
                for (int k = 0; k < K; k++) P[(size_t)i * K + k] /= sum;
            }
            /* Un arbre de régression par classe sur ses pseudo-résidus. La
             * softmax de l'étape est figée : mettre à jour F_k n'affecte pas les
             * résidus des classes suivantes de la même étape. */
            for (int k = 0; k < K; k++) {
                for (int i = 0; i < nrows; i++) {
                    double onehot = (mlarbre_class_of(y[i]) == k) ? 1.0 : 0.0;
                    resid[i] = onehot - P[(size_t)i * K + k];
                }
                ctx.y = resid;
                MLTreeNode* h = mlarbre_build(&ctx, idx, nrows, 0);
                b->trees[(size_t)m * K + k] = h;
                b->treeCount++;
                for (int i = 0; i < nrows; i++)
                    F[(size_t)i * K + k] +=
                        lr * mlarbre_predict_one(h, &X[(size_t)i * ncols]);
            }
        }
        free(F);
        free(P);
        free(resid);
    }

    free(idx);
    free(featScratch);
    return args[0];
}

/* .Prédit(X) / .Predict(X) — rend un Tenseur 1D, une prédiction par ligne.
 * Régression : F0 + taux·Σ arbresₘ(x). Classification : argmax_k des K scores
 * F0_k + taux·Σ arbresₘ,ₖ(x) (plus petit indice sur égalité). */
static Value fn_mlboosting_predit(int argCount, Value* args) {
    (void)argCount;
    ObjMLBoosting* b = AS_MLBOOSTING(args[0]);
    if (b->trees == NULL || b->treeCount == 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""dit", NULL);
        return NIL_VAL;
    }
    const double* X; int nrows, ncols;
    if (!mlarbre_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""dit",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }

    /* Alloue d'abord le Tenseur de sortie (peut déclencher un GC ; X reste rooté
     * via args[1], le modèle via args[0]). Ensuite, remplissage sans aucune
     * allocation OWL -> out et X restent sains. */
    ObjTensor* out = obj_new_tensor(nrows);
    double* cells = tensor_cells(out->data);

    double lr = b->learningRate;
    int nSteps = b->nSteps;
    int K = b->nClasses;

    if (b->objective == OWL_ML_OBJ_REGRESSION) {
        for (int i = 0; i < nrows; i++) {
            double f = b->init0[0];
            for (int m = 0; m < nSteps; m++)
                f += lr * mlarbre_predict_one(b->trees[m], &X[(size_t)i * ncols]);
            cells[i] = f;
        }
    } else {
        for (int i = 0; i < nrows; i++) {
            int best = 0;
            double bestScore = 0.0;
            for (int k = 0; k < K; k++) {
                double fk = b->init0[k];
                for (int m = 0; m < nSteps; m++)
                    fk += lr * mlarbre_predict_one(b->trees[(size_t)m * K + k],
                                                   &X[(size_t)i * ncols]);
                if (k == 0 || fk > bestScore) { bestScore = fk; best = k; }
            }
            cells[i] = (double)best;
        }
    }
    return OBJ_VAL(out);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const char* const ALIAS_mlarbre[] = { "MLDecisionTree", NULL };
static const char* const ALIAS_mlforet[] = { "MLRandomForest", NULL };

/* Objectif de la tâche (valeurs de ..Objectif). Constantes en zone sentinelle,
 * mutuellement exclusives. Câblées en OwlEnum strict `objectif`/`objective` sur
 * le champ ..Objectif (validation compile-time des membres + filtre LSP), sur
 * le même modèle que ..PlanTaux ∈ planTaux d'OptionsEntraînement. objClassification
 * / objRégression restent des constantes globales normales et utilisables telles
 * quelles. */

/* Valeurs d'objectif : objClassification (même identifiant FR/EN) et
 * objRégression / objRegression. */
static const OwlConstDef MLARBRE_CONSTS[] = {
    { "objClassification", "Apprentissage automatique",
      INTEGER_VAL(OWL_ML_OBJ_CLASSIFICATION),
      "Objectif de classification d'un MLArbreDécision : la cible est un indice "
      "de classe (0-based) et chaque feuille prédit la classe majoritaire de ses "
      "échantillons. Critère de séparation : impureté de Gini.",
      "Classification objective of an MLDecisionTree: the target is a class index "
      "(0-based) and each leaf predicts the majority class of its samples. Split "
      "criterion: Gini impurity.",
      "objClassification" },
    { "objR\xc3\xa9gression", "Apprentissage automatique",
      INTEGER_VAL(OWL_ML_OBJ_REGRESSION),
      "Objectif de régression d'un MLArbreDécision : la cible est une valeur "
      "continue et chaque feuille prédit la moyenne de ses échantillons. Critère "
      "de séparation : somme des carrés des écarts (erreur quadratique).",
      "Regression objective of an MLDecisionTree: the target is a continuous value "
      "and each leaf predicts the mean of its samples. Split criterion: sum of "
      "squared errors.",
      "objRegression" },
};

/* Enum strict de l'objectif : ..Objectif n'accepte qu'un de ses deux membres
 * (validation compile-time + filtre LSP). Les membres sont des OwlConstDef
 * (MLARBRE_CONSTS) ; l'enum ne stocke que les références de noms, dans l'ordre
 * de déclaration (objClassification puis objRégression). */
static const char* const E_objectif_values[] = {
    "objClassification", "objR\xc3\xa9gression", NULL
};
static const OwlEnum E_objectif = {
    .name="objectif", .nameAscii="objectif", .nameEN="objective",
    .category="Apprentissage automatique",
    .descFR="Objectif d'un MLArbreDécision : objClassification (feuille = classe "
            "majoritaire, critère Gini) ou objRégression (feuille = moyenne, "
            "critère erreur quadratique).",
    .descEN="Objective of an MLDecisionTree: objClassification (leaf = majority "
            "class, Gini criterion) or objRegression (leaf = mean, squared-error "
            "criterion).",
    .values=E_objectif_values, .combinable=false,
};

static const OwlEnum* const MLARBRE_ENUMS[] = { &E_objectif };

/* Hyperparamètres réglables par double point. Le STOCKAGE réel vit dans
 * ObjMLArbre et la lecture/écriture double-point est câblée dans vm.c
 * (OP_STRUCT_GET/SET, branche IS_MLARBRE) : ces entrées ne servent pas de
 * template runtime. Elles publient la paire bilingue FR/EN pour l'explain LSP
 * et `owl translate`, ET activent la validation compile-time des membres —
 * dont la contrainte enum sur ..Objectif (allowedBase). Aucun défaut pour
 * ..Objectif : il DOIT être réglé avant .Entraîne (pas d'inférence implicite). */
static const OwlField FIELDS_MLArbre[] = {
    {"Objectif", NULL, "entier", "", "objectif de la tâche : objClassification (classification, critère Gini) ou objRégression (régression, critère erreur quadratique). À régler avant l'entraînement", "task objective: objClassification (classification, Gini criterion) or objRégression (regression, squared-error criterion). Must be set before training", .allowedBase=&E_objectif, .nameEN="Objective"},
    {"ProfondeurMax", NULL, "entier", "0", "profondeur maximale de l'arbre (0 = illimité)", "maximum tree depth (0 = unlimited)", .nameEN="MaxDepth"},
    {"MinEchantillonsSeparation", "MinÉchantillonsSéparation", "entier", "2", "nombre minimal d'échantillons dans un nœud pour tenter une séparation", "minimum number of samples in a node to attempt a split", .nameEN="MinSamplesSplit"},
    {"MinEchantillonsFeuille", "MinÉchantillonsFeuille", "entier", "1", "nombre minimal d'échantillons que chaque feuille produite par une séparation doit contenir", "minimum number of samples each leaf produced by a split must hold", .nameEN="MinSamplesLeaf"},
    {0},
};

/* Hyperparamètres de la forêt réglables par double point. Comme pour l'arbre,
 * le STOCKAGE vit dans ObjMLForet et la lecture/écriture est câblée dans vm.c
 * (OP_STRUCT_GET/SET, branche IS_MLFORET) : ces entrées publient la paire
 * bilingue FR/EN pour l'explain LSP et activent la validation compile-time des
 * membres — dont la contrainte enum sur ..Objectif (allowedBase). Aucun défaut
 * pour ..Objectif : il DOIT être réglé avant .Entraîne. */
static const OwlField FIELDS_MLForet[] = {
    {"Objectif", NULL, "entier", "", "objectif de la tâche : objClassification (classification, vote majoritaire des arbres) ou objRégression (régression, moyenne des arbres). À régler avant l'entraînement", "task objective: objClassification (classification, majority vote of trees) or objRégression (regression, mean of trees). Must be set before training", .allowedBase=&E_objectif, .nameEN="Objective"},
    {"NbArbres", NULL, "entier", "100", "nombre d'arbres de l'ensemble", "number of trees in the ensemble", .nameEN="NumTrees"},
    {"VariablesParSeparation", "VariablesParSéparation", "entier", "0", "nombre de variables candidates tirées au hasard à chaque séparation (0 = auto : racine du nombre de variables en classification, toutes en régression)", "number of features randomly drawn as candidates at each split (0 = auto: square root of the feature count in classification, all in regression)", .nameEN="MaxFeatures"},
    {"Graine", NULL, "entier", "0", "graine du générateur pseudo-aléatoire pilotant le tirage bootstrap et le tirage des variables (0 = graine fixe reproductible)", "seed of the pseudo-random generator driving bootstrap sampling and feature sampling (0 = fixed reproducible seed)", .nameEN="Seed"},
    {"ProfondeurMax", NULL, "entier", "0", "profondeur maximale de chaque arbre (0 = illimité)", "maximum depth of each tree (0 = unlimited)", .nameEN="MaxDepth"},
    {"MinEchantillonsSeparation", "MinÉchantillonsSéparation", "entier", "2", "nombre minimal d'échantillons dans un nœud pour tenter une séparation", "minimum number of samples in a node to attempt a split", .nameEN="MinSamplesSplit"},
    {"MinEchantillonsFeuille", "MinÉchantillonsFeuille", "entier", "1", "nombre minimal d'échantillons que chaque feuille produite par une séparation doit contenir", "minimum number of samples each leaf produced by a split must hold", .nameEN="MinSamplesLeaf"},
    {0},
};

/* Hyperparamètres du boosting réglables par double point. Comme pour l'arbre et
 * la forêt, le STOCKAGE vit dans ObjMLBoosting et la lecture/écriture est câblée
 * dans vm.c (OP_STRUCT_GET/SET, branche IS_MLBOOSTING) : ces entrées publient la
 * paire bilingue FR/EN pour l'explain LSP et activent la validation compile-time
 * des membres — dont la contrainte enum sur ..Objectif (allowedBase). Aucun
 * défaut pour ..Objectif : il DOIT être réglé avant .Entraîne. */
static const OwlField FIELDS_MLBoosting[] = {
    {"Objectif", NULL, "entier", "", "objectif de la tâche : objClassification (classification, softmax + un arbre par classe et par étape) ou objRégression (régression, arbre fitté aux résidus à chaque étape). À régler avant l'entraînement", "task objective: objClassification (classification, softmax + one tree per class and per step) or objRégression (regression, tree fit to residuals at each step). Must be set before training", .allowedBase=&E_objectif, .nameEN="Objective"},
    {"NbArbres", NULL, "entier", "100", "nombre d'étapes de boosting (arbres successifs)", "number of boosting steps (successive trees)", .nameEN="NumTrees"},
    {"TauxApprentissage", NULL, "réel", "0.1", "shrinkage : pondération appliquée à la contribution de chaque arbre (un taux plus faible demande plus d'étapes mais généralise mieux)", "shrinkage: weighting applied to each tree's contribution (a lower rate needs more steps but generalizes better)", .nameEN="LearningRate"},
    {"ProfondeurMax", NULL, "entier", "3", "profondeur maximale de chaque arbre (0 = illimité ; les arbres de boosting sont volontairement peu profonds)", "maximum depth of each tree (0 = unlimited; boosting trees are deliberately shallow)", .nameEN="MaxDepth"},
    {"MinEchantillonsSeparation", "MinÉchantillonsSéparation", "entier", "2", "nombre minimal d'échantillons dans un nœud pour tenter une séparation", "minimum number of samples in a node to attempt a split", .nameEN="MinSamplesSplit"},
    {"MinEchantillonsFeuille", "MinÉchantillonsFeuille", "entier", "1", "nombre minimal d'échantillons que chaque feuille produite par une séparation doit contenir", "minimum number of samples each leaf produced by a split must hold", .nameEN="MinSamplesLeaf"},
    {0},
};

static const OwlTypeDef MLARBRE_TYPES[] = {
    { .canonical      = "MLArbreD\xc3\xa9""cision",   /* MLArbreDécision */
      .canonicalAscii = "MLArbreDecision",
      .aliases        = ALIAS_mlarbre,
      .category       = "Apprentissage automatique",
      .descFR         = "Arbre de décision (CART) pour données tabulaires, en "
                        "classification comme en régression. L'objectif de la "
                        "tâche se règle explicitement avant l'entraînement : "
                        "`arbre..Objectif = objClassification` (feuille = classe "
                        "majoritaire, critère Gini) ou `objRégression` (feuille = "
                        "moyenne, critère erreur quadratique). `.Entraîne(X, y)` "
                        "construit l'arbre par séparation gloutonne (X = Matrice "
                        "échantillons × variables, y = Tenseur 1D de cibles : "
                        "indices de classe 0-based en classification, valeurs "
                        "continues en régression). `.Prédit(X)` rend un Tenseur 1D, "
                        "une prédiction par ligne. Hyperparamètres réglables par "
                        "double point : `..ProfondeurMax` (0 = illimité), "
                        "`..MinÉchantillonsSéparation` (défaut 2), "
                        "`..MinÉchantillonsFeuille` (défaut 1).",
      .descEN         = "Decision tree (CART) for tabular data, for both "
                        "classification and regression. The task objective is set "
                        "explicitly before training: `tree..Objective = "
                        "objClassification` (leaf = majority class, Gini criterion) "
                        "or `objRegression` (leaf = mean, squared-error criterion). "
                        "`.Train(X, y)` grows the tree by greedy splitting (X = "
                        "samples × features Matrix, y = 1D targets Tensor: 0-based "
                        "class indices for classification, continuous values for "
                        "regression). `.Predict(X)` returns a 1D Tensor, one "
                        "prediction per row. Hyperparameters set via the double "
                        "dot: `..MaxDepth` (0 = unlimited), `..MinSamplesSplit` "
                        "(default 2), `..MinSamplesLeaf` (default 1).",
      .fields         = FIELDS_MLArbre,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "MLDecisionTree",
      .genre          = OWL_GENRE_M,          /* un MLArbreDécision */
      .defaultFactoryNative = "__MLArbreDecisionNouveau" },
    { .canonical      = "MLFor\xc3\xaatAl\xc3\xa9""atoire",   /* MLForêtAléatoire */
      .canonicalAscii = "MLForetAleatoire",
      .aliases        = ALIAS_mlforet,
      .category       = "Apprentissage automatique",
      .descFR         = "Forêt aléatoire : ensemble d'arbres de décision (CART) "
                        "pour données tabulaires, en classification comme en "
                        "régression. Chaque arbre est entraîné sur un échantillon "
                        "bootstrap des lignes (tirage avec remise) et, à chaque "
                        "séparation, sur un sous-ensemble aléatoire de variables — "
                        "ce double aléa réduit le surapprentissage d'un arbre seul. "
                        "L'objectif se règle avant l'entraînement : "
                        "`foret..Objectif = objClassification` (prédiction = vote "
                        "majoritaire des arbres) ou `objRégression` (prédiction = "
                        "moyenne des arbres). `.Entraîne(X, y)` construit "
                        "l'ensemble (X = Matrice échantillons × variables, y = "
                        "Tenseur 1D de cibles). `.Prédit(X)` rend un Tenseur 1D, "
                        "une prédiction par ligne. Hyperparamètres réglables par "
                        "double point : `..NbArbres` (défaut 100), "
                        "`..VariablesParSéparation` (défaut 0 = auto), `..Graine` "
                        "(défaut 0 = graine fixe reproductible), `..ProfondeurMax` "
                        "(0 = illimité), `..MinÉchantillonsSéparation` (défaut 2), "
                        "`..MinÉchantillonsFeuille` (défaut 1). À graine fixe, "
                        "l'entraînement est intégralement reproductible.",
      .descEN         = "Random forest: an ensemble of decision trees (CART) for "
                        "tabular data, for both classification and regression. Each "
                        "tree is trained on a bootstrap sample of the rows (sampling "
                        "with replacement) and, at each split, on a random subset of "
                        "features — this twofold randomness reduces the overfitting "
                        "of a single tree. The objective is set before training: "
                        "`forest..Objective = objClassification` (prediction = "
                        "majority vote of the trees) or `objRegression` (prediction "
                        "= mean of the trees). `.Train(X, y)` builds the ensemble (X "
                        "= samples × features Matrix, y = 1D targets Tensor). "
                        "`.Predict(X)` returns a 1D Tensor, one prediction per row. "
                        "Hyperparameters set via the double dot: `..NumTrees` "
                        "(default 100), `..MaxFeatures` (default 0 = auto), `..Seed` "
                        "(default 0 = fixed reproducible seed), `..MaxDepth` (0 = "
                        "unlimited), `..MinSamplesSplit` (default 2), "
                        "`..MinSamplesLeaf` (default 1). With a fixed seed, training "
                        "is fully reproducible.",
      .fields         = FIELDS_MLForet,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "MLRandomForest",
      .genre          = OWL_GENRE_F,          /* une MLForêtAléatoire */
      .defaultFactoryNative = "__MLForetAleatoireNouveau" },
    { .canonical      = "MLGradientBoosting",
      .canonicalAscii = "MLGradientBoosting",
      .aliases        = NULL,
      .category       = "Apprentissage automatique",
      .descFR         = "Gradient boosting : ensemble SÉQUENTIEL d'arbres de "
                        "décision (CART) pour données tabulaires, en classification "
                        "comme en régression. Contrairement à la forêt (arbres "
                        "indépendants agrégés), chaque étape ajoute un arbre qui "
                        "corrige les erreurs résiduelles de l'ensemble courant, "
                        "pondéré par `..TauxApprentissage`. Le boosting est "
                        "DÉTERMINISTE (aucune graine) : deux entraînements "
                        "identiques produisent des prédictions identiques. "
                        "L'objectif se règle avant l'entraînement : "
                        "`gb..Objectif = objClassification` (softmax + un arbre par "
                        "classe à chaque étape, prédiction = classe la plus probable) "
                        "ou `objRégression` (arbre fitté aux résidus, prédiction = "
                        "somme pondérée des arbres). `.Entraîne(X, y)` construit "
                        "l'ensemble (X = Matrice échantillons × variables, y = "
                        "Tenseur 1D de cibles). `.Prédit(X)` rend un Tenseur 1D, une "
                        "prédiction par ligne. Hyperparamètres réglables par double "
                        "point : `..NbArbres` (défaut 100), `..TauxApprentissage` "
                        "(défaut 0.1), `..ProfondeurMax` (défaut 3), "
                        "`..MinÉchantillonsSéparation` (défaut 2), "
                        "`..MinÉchantillonsFeuille` (défaut 1).",
      .descEN         = "Gradient boosting: a SEQUENTIAL ensemble of decision trees "
                        "(CART) for tabular data, for both classification and "
                        "regression. Unlike a forest (independent trees aggregated), "
                        "each step adds a tree that corrects the residual errors of "
                        "the current ensemble, weighted by `..LearningRate`. Boosting "
                        "is DETERMINISTIC (no seed): two identical training runs "
                        "produce identical predictions. The objective is set before "
                        "training: `gb..Objective = objClassification` (softmax + one "
                        "tree per class at each step, prediction = most likely class) "
                        "or `objRegression` (tree fit to residuals, prediction = "
                        "weighted sum of the trees). `.Train(X, y)` builds the "
                        "ensemble (X = samples × features Matrix, y = 1D targets "
                        "Tensor). `.Predict(X)` returns a 1D Tensor, one prediction "
                        "per row. Hyperparameters set via the double dot: `..NumTrees` "
                        "(default 100), `..LearningRate` (default 0.1), `..MaxDepth` "
                        "(default 3), `..MinSamplesSplit` (default 2), "
                        "`..MinSamplesLeaf` (default 1).",
      .fields         = FIELDS_MLBoosting,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "MLGradientBoosting",
      .genre          = OWL_GENRE_M,          /* un MLGradientBoosting */
      .defaultFactoryNative = "__MLGradientBoostingNouveau" },
};

void stdlib_mlarbre_init(void) {
    owl_registry_register_types(MLARBRE_TYPES,
        (int)(sizeof(MLARBRE_TYPES) / sizeof(MLARBRE_TYPES[0])));
    owl_registry_register_consts(MLARBRE_CONSTS,
        (int)(sizeof(MLARBRE_CONSTS) / sizeof(MLARBRE_CONSTS[0])));
    owl_registry_register_enums(MLARBRE_ENUMS,
        (int)(sizeof(MLARBRE_ENUMS) / sizeof(MLARBRE_ENUMS[0])));

    /* Factory de déclaration `arbre est un MLArbreDécision` (instance fraîche).
     * Interne : émise par le compilateur, jamais appelée explicitement. */
    owl_define_native("__MLArbreDecisionNouveau", fn_mlarbre_nouveau, 0);

    /* Entraînement : receveur + X + y (arité 3). Bilingue Entraîne / Train. */
    owl_register_method(METHOD_TYPE_MLARBRE, "Entra\xc3\xaene",
                        fn_mlarbre_entraine, 3);
    owl_register_method(METHOD_TYPE_MLARBRE, "Train",
                        fn_mlarbre_entraine, 3);

    /* Prédiction : receveur + X (arité 2). Bilingue Prédit / Predict. */
    owl_register_method(METHOD_TYPE_MLARBRE, "Pr\xc3\xa9""dit",
                        fn_mlarbre_predit, 2);
    owl_register_method(METHOD_TYPE_MLARBRE, "Predict",
                        fn_mlarbre_predit, 2);

    /* Forêt aléatoire. Factory de déclaration + méthodes. */
    owl_define_native("__MLForetAleatoireNouveau", fn_mlforet_nouveau, 0);
    owl_register_method(METHOD_TYPE_MLFORET, "Entra\xc3\xaene",
                        fn_mlforet_entraine, 3);
    owl_register_method(METHOD_TYPE_MLFORET, "Train",
                        fn_mlforet_entraine, 3);
    owl_register_method(METHOD_TYPE_MLFORET, "Pr\xc3\xa9""dit",
                        fn_mlforet_predit, 2);
    owl_register_method(METHOD_TYPE_MLFORET, "Predict",
                        fn_mlforet_predit, 2);

    /* Gradient boosting. Factory de déclaration + méthodes. */
    owl_define_native("__MLGradientBoostingNouveau", fn_mlboosting_nouveau, 0);
    owl_register_method(METHOD_TYPE_MLBOOSTING, "Entra\xc3\xaene",
                        fn_mlboosting_entraine, 3);
    owl_register_method(METHOD_TYPE_MLBOOSTING, "Train",
                        fn_mlboosting_entraine, 3);
    owl_register_method(METHOD_TYPE_MLBOOSTING, "Pr\xc3\xa9""dit",
                        fn_mlboosting_predit, 2);
    owl_register_method(METHOD_TYPE_MLBOOSTING, "Predict",
                        fn_mlboosting_predit, 2);
}
