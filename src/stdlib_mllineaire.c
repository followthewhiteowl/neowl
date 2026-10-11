// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_mllineaire.c — Types `MLRégressionLinéaire` / `MLLinearRegression` et
 * `MLRégressionLogistique` / `MLLogisticRegression`.
 *
 * Les deux modèles linéaires canoniques, task-specific PAR LE NOM (pas d'objectif
 * à régler, contrairement aux arbres/forêt/boosting) :
 *
 *   - MLRégressionLinéaire : moindres carrés ordinaires (OLS) en FORME CLOSE.
 *     Résout les équations normales (XᵀX)·θ = Xᵀy où X est augmentée d'une
 *     colonne de 1 (l'intercept), par élimination de Gauss avec pivot partiel.
 *     Un léger ridge diagonal (1e-8) rend le système toujours résoluble.
 *     ZÉRO hyperparamètre, déterministe, EXACT : sur un jeu strictement linéaire
 *     sans bruit, retrouve les coefficients d'origine au réel près.
 *
 *   - MLRégressionLogistique : régression softmax (K = max(y)+1 classes ; binaire
 *     = K=2) entraînée par DESCENTE DE GRADIENT sur la cross-entropy. Poids et
 *     biais initialisés à zéro (déterminisme : pas de RNG), mis à jour
 *     échantillon par échantillon. Softmax stable (soustraction du max). La
 *     prédiction rend l'argmax des scores (indice de classe).
 *
 * Les deux types stockent leurs poids en C-heap pur (aucune référence OWL
 * sortante) : FEUILLES GC, poids libérés au finaliseur (cf. object.c / gc.c).
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "object.h"
#include "vm.h"

#include <math.h>
#include <stdlib.h>

/* ================================================================
 * Lecture des données d'entrée (mêmes conventions que stdlib_mlarbre.c)
 * ================================================================ */

/* Extrait d'une valeur X une matrice d'échantillons row-major : `data`
 * (rows×cols contigus), `rows`, `cols`. Accepte une Matrice ou un Tenseur 2D. */
static bool mllin_read_matrix(Value v, const double** data, int* rows, int* cols) {
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
static bool mllin_read_targets(Value v, const double** data, int* count) {
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
 * Solveur linéaire dense (élimination de Gauss, pivot partiel)
 * ================================================================ */

/* Résout le système M·x = b (n×n) par élimination de Gauss avec pivot partiel,
 * EN PLACE : M (row-major n×n) est triangularisée, la solution écrase b. Le pivot
 * partiel (plus grand |coefficient| de la colonne) borne l'amplification des
 * erreurs d'arrondi. Rend false si un pivot reste quasi nul (système singulier) —
 * en pratique jamais atteint côté équations normales grâce au ridge diagonal. */
static bool mllin_solve_gauss(double* M, double* b, int n) {
    for (int col = 0; col < n; col++) {
        /* Pivot partiel : ligne au plus grand |M[row][col]| sur/sous la diagonale. */
        int piv = col;
        double maxAbs = fabs(M[(size_t)col * n + col]);
        for (int row = col + 1; row < n; row++) {
            double a = fabs(M[(size_t)row * n + col]);
            if (a > maxAbs) { maxAbs = a; piv = row; }
        }
        if (maxAbs < 1e-300) return false;   /* singulier */

        if (piv != col) {                    /* échange des lignes col <-> piv */
            for (int k = 0; k < n; k++) {
                double t = M[(size_t)col * n + k];
                M[(size_t)col * n + k] = M[(size_t)piv * n + k];
                M[(size_t)piv * n + k] = t;
            }
            double tb = b[col]; b[col] = b[piv]; b[piv] = tb;
        }

        double pivVal = M[(size_t)col * n + col];
        for (int row = col + 1; row < n; row++) {   /* élimination sous la diagonale */
            double factor = M[(size_t)row * n + col] / pivVal;
            if (factor == 0.0) continue;
            for (int k = col; k < n; k++)
                M[(size_t)row * n + k] -= factor * M[(size_t)col * n + k];
            b[row] -= factor * b[col];
        }
    }
    /* Remontée : x_row = (b_row - Σ_{k>row} M[row][k]·x_k) / M[row][row]. */
    for (int row = n - 1; row >= 0; row--) {
        double s = b[row];
        for (int k = row + 1; k < n; k++)
            s -= M[(size_t)row * n + k] * b[k];
        b[row] = s / M[(size_t)row * n + row];
    }
    return true;
}

/* ================================================================
 * MLRégressionLinéaire — OLS forme close
 * ================================================================ */

/* Factory interne émise par le compilateur pour `reg est une MLRégressionLinéaire`. */
static Value fn_mllineaire_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_mllineaire());
}

/* .Entraîne(X, y) / .Train(X, y) — résout les équations normales. X = Matrice
 * échantillons × variables, y = Tenseur 1D de cibles continues. Tailles X/y
 * cohérentes (soft-error sinon), système singulier malgré le ridge (soft-error).
 * Rend le receveur (chaînage fluide). */
static Value fn_mllineaire_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjMLLineaire* r = AS_MLLINEAIRE(args[0]);

    const double* X; int nrows, ncols;
    if (!mllin_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    const double* y; int ny;
    if (!mllin_read_targets(args[2], &y, &ny)) {
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

    /* Système des équations normales AᵀA·θ = Aᵀy avec A = [X | 1] (p = ncols+1,
     * dernière colonne = 1 pour l'intercept). La ligne augmentée A[i] est
     * construite à la volée sans matérialiser A. Aucune allocation OWL ici (X/y
     * rootés via args) : tout est C-heap. */
    int p = ncols + 1;
    double* M   = (double*)calloc((size_t)p * (size_t)p, sizeof(double));
    double* rhs = (double*)calloc((size_t)p, sizeof(double));
    for (int i = 0; i < nrows; i++) {
        const double* xi = &X[(size_t)i * ncols];
        for (int a = 0; a < p; a++) {
            double Aa = (a < ncols) ? xi[a] : 1.0;
            for (int c = a; c < p; c++) {           /* symétrie : moitié haute */
                double Ac = (c < ncols) ? xi[c] : 1.0;
                M[(size_t)a * p + c] += Aa * Ac;
            }
            rhs[a] += Aa * y[i];
        }
    }
    for (int a = 0; a < p; a++)                       /* recopie la moitié basse */
        for (int c = 0; c < a; c++)
            M[(size_t)a * p + c] = M[(size_t)c * p + a];
    for (int a = 0; a < p; a++) M[(size_t)a * p + a] += 1e-8;   /* ridge de stabilité */

    bool ok = mllin_solve_gauss(M, rhs, p);
    if (!ok) {
        free(M); free(rhs);
        owl_emit_native_error(E_NATIVE_ML_SYSTEME_SINGULIER, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    /* Réentraînement : libère les poids précédents avant de poser les nouveaux. */
    obj_mllineaire_free_weights(r);
    r->coef = (double*)malloc((size_t)ncols * sizeof(double));
    for (int j = 0; j < ncols; j++) r->coef[j] = rhs[j];
    r->nCoef = ncols;
    r->intercept = rhs[ncols];
    r->trained = true;

    free(M); free(rhs);
    return args[0];
}

/* .Prédit(X) / .Predict(X) — rend un Tenseur 1D : pour chaque ligne,
 * X·w + intercept. */
static Value fn_mllineaire_predit(int argCount, Value* args) {
    (void)argCount;
    ObjMLLineaire* r = AS_MLLINEAIRE(args[0]);
    if (!r->trained) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""dit", NULL);
        return NIL_VAL;
    }
    const double* X; int nrows, ncols;
    if (!mllin_read_matrix(args[1], &X, &nrows, &ncols)) {
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
    int m = ncols < r->nCoef ? ncols : r->nCoef;
    for (int i = 0; i < nrows; i++) {
        const double* xi = &X[(size_t)i * ncols];
        double s = r->intercept;
        for (int j = 0; j < m; j++) s += r->coef[j] * xi[j];
        cells[i] = s;
    }
    return OBJ_VAL(out);
}

/* ================================================================
 * MLRégressionLogistique — softmax + descente de gradient
 * ================================================================ */

/* Indice de classe 0-based d'une cible (les cibles sont des réels entiers). */
static int mllog_class_of(double yv) {
    int c = (int)lround(yv);
    return c < 0 ? 0 : c;
}

/* Factory interne émise par le compilateur pour `reg est une MLRégressionLogistique`. */
static Value fn_mllogistique_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_mllogistique());
}

/* .Entraîne(X, y) / .Train(X, y) — descente de gradient softmax. K = max(y)+1
 * classes, poids W (K×variables) et biais (K) initialisés à zéro. Pour
 * ..NbItérations étapes, chaque échantillon met à jour W et b par le gradient de
 * la cross-entropy (p − one-hot(y)). Softmax stable. Tailles X/y cohérentes
 * (soft-error sinon). Rend le receveur (chaînage fluide). */
static Value fn_mllogistique_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjMLLogistique* r = AS_MLLOGISTIQUE(args[0]);

    const double* X; int nrows, ncols;
    if (!mllin_read_matrix(args[1], &X, &nrows, &ncols)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "X",
                              "expected", "Matrice ou Tenseur 2D",
                              "actual", "type non pris en charge", NULL);
        return args[0];
    }
    const double* y; int ny;
    if (!mllin_read_targets(args[2], &y, &ny)) {
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

    int K = 1;
    for (int i = 0; i < nrows; i++) {
        int c = mllog_class_of(y[i]);
        if (c + 1 > K) K = c + 1;
    }

    /* Réentraînement : libère les poids précédents. Init à ZÉRO (calloc) =
     * déterminisme intégral (aucun tirage aléatoire). */
    obj_mllogistique_free_weights(r);
    r->nFeatures = ncols;
    r->nClasses  = K;
    r->W    = (double*)calloc((size_t)K * (size_t)ncols, sizeof(double));
    r->bias = (double*)calloc((size_t)K, sizeof(double));

    double lr = r->learningRate;
    int iters = r->numIterations;

    /* Tampons C-heap réutilisés à chaque échantillon : aucune allocation OWL
     * pendant tout l'entraînement (X/y rootés via args). */
    double* logits = (double*)malloc((size_t)K * sizeof(double));
    double* prob   = (double*)malloc((size_t)K * sizeof(double));

    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < nrows; i++) {
            const double* xi = &X[(size_t)i * ncols];

            /* Scores W·x + b par classe. */
            for (int k = 0; k < K; k++) {
                double z = r->bias[k];
                const double* Wk = &r->W[(size_t)k * ncols];
                for (int j = 0; j < ncols; j++) z += Wk[j] * xi[j];
                logits[k] = z;
            }
            /* Softmax stable (soustraction du max). */
            double mx = logits[0];
            for (int k = 1; k < K; k++) if (logits[k] > mx) mx = logits[k];
            double sum = 0.0;
            for (int k = 0; k < K; k++) {
                double e = exp(logits[k] - mx);
                prob[k] = e;
                sum += e;
            }
            for (int k = 0; k < K; k++) prob[k] /= sum;

            /* Gradient cross-entropy = (p − one-hot(y)) ⊗ x ; W -= taux·grad. */
            int yi = mllog_class_of(y[i]);
            if (yi >= K) yi = K - 1;
            for (int k = 0; k < K; k++) {
                double g = prob[k] - (k == yi ? 1.0 : 0.0);
                double lrg = lr * g;
                double* Wk = &r->W[(size_t)k * ncols];
                for (int j = 0; j < ncols; j++) Wk[j] -= lrg * xi[j];
                r->bias[k] -= lrg;
            }
        }
    }
    free(logits);
    free(prob);
    r->trained = true;
    return args[0];
}

/* .Prédit(X) / .Predict(X) — rend un Tenseur 1D d'indices de classe : argmax_k
 * des scores W·x + b (plus petit indice sur égalité). */
static Value fn_mllogistique_predit(int argCount, Value* args) {
    (void)argCount;
    ObjMLLogistique* r = AS_MLLOGISTIQUE(args[0]);
    if (!r->trained) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""dit", NULL);
        return NIL_VAL;
    }
    const double* X; int nrows, ncols;
    if (!mllin_read_matrix(args[1], &X, &nrows, &ncols)) {
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
    int K  = r->nClasses;
    int nf = r->nFeatures;
    int m  = ncols < nf ? ncols : nf;
    for (int i = 0; i < nrows; i++) {
        const double* xi = &X[(size_t)i * ncols];
        int best = 0;
        double bestScore = 0.0;
        for (int k = 0; k < K; k++) {
            double z = r->bias[k];
            const double* Wk = &r->W[(size_t)k * nf];
            for (int j = 0; j < m; j++) z += Wk[j] * xi[j];
            if (k == 0 || z > bestScore) { bestScore = z; best = k; }
        }
        cells[i] = (double)best;
    }
    return OBJ_VAL(out);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const char* const ALIAS_mllineaire[]   = { "MLLinearRegression", NULL };
static const char* const ALIAS_mllogistique[] = { "MLLogisticRegression", NULL };

/* MLRégressionLinéaire : deux propriétés en lecture seule, aucun hyperparamètre. */
static const OwlField FIELDS_MLLineaire[] = {
    {"Coefficients", NULL, "Vecteur", "", "poids appris (un par variable, dans l'ordre des colonnes de X), disponibles après l'entraînement", "learned weights (one per feature, in the column order of X), available after training", .nameEN="Coefficients"},
    {"Ordonnee", "Ordonn\xc3\xa9""e", "r\xc3\xa9""el", "", "ordonnée à l'origine (le biais du modèle), disponible après l'entraînement", "intercept (the model bias), available after training", .nameEN="Intercept"},
    {0},
};

/* MLRégressionLogistique : hyperparamètres de la descente de gradient. Le
 * STOCKAGE réel vit dans ObjMLLogistique et la lecture/écriture double-point est
 * câblée dans vm.c (OP_STRUCT_GET/SET, branche IS_MLLOGISTIQUE) : ces entrées
 * publient la paire bilingue FR/EN pour l'explain LSP et `owl translate`. */
static const OwlField FIELDS_MLLogistique[] = {
    {"TauxApprentissage", NULL, "r\xc3\xa9""el", "0.1", "pas de la descente de gradient appliqué à chaque mise à jour des poids", "gradient-descent step applied to each weight update", .nameEN="LearningRate"},
    {"NbIterations", "NbIt\xc3\xa9rations", "entier", "500", "nombre de passes de descente de gradient sur l'ensemble d'entraînement", "number of gradient-descent passes over the training set", .nameEN="NumIterations"},
    {0},
};

static const OwlTypeDef MLLINEAIRE_TYPES[] = {
    { .canonical      = "MLR\xc3\xa9gressionLin\xc3\xa9""aire",   /* MLRégressionLinéaire */
      .canonicalAscii = "MLRegressionLineaire",
      .aliases        = ALIAS_mllineaire,
      .category       = "Apprentissage automatique",
      .descFR         = "Régression linéaire par moindres carrés ordinaires (OLS) "
                        "en forme close, pour données tabulaires. Aucun "
                        "hyperparamètre, aucun objectif à régler : le modèle est "
                        "dédié à la régression. `.Entraîne(X, y)` résout les "
                        "équations normales par élimination de Gauss (X = Matrice "
                        "échantillons × variables, y = Tenseur 1D de cibles "
                        "continues) ; la solution est exacte et déterministe. "
                        "`.Prédit(X)` rend un Tenseur 1D, une prédiction par ligne "
                        "(X·w + ordonnée). La solution se lit par double point : "
                        "`..Coefficients` (Vecteur des poids, un par variable) et "
                        "`..Ordonnée` (réel, le biais). Sur un jeu strictement "
                        "linéaire sans bruit, retrouve les coefficients d'origine "
                        "au réel près.",
      .descEN         = "Linear regression by ordinary least squares (OLS) in "
                        "closed form, for tabular data. No hyperparameter, no "
                        "objective to set: the model is dedicated to regression. "
                        "`.Train(X, y)` solves the normal equations by Gaussian "
                        "elimination (X = samples × features Matrix, y = 1D "
                        "continuous targets Tensor); the solution is exact and "
                        "deterministic. `.Predict(X)` returns a 1D Tensor, one "
                        "prediction per row (X·w + intercept). The solution is read "
                        "via the double dot: `..Coefficients` (Vector of weights, "
                        "one per feature) and `..Intercept` (real, the bias). On a "
                        "strictly linear noise-free dataset, it recovers the "
                        "original coefficients up to floating-point precision.",
      .fields         = FIELDS_MLLineaire,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "MLLinearRegression",
      .genre          = OWL_GENRE_F,          /* une MLRégressionLinéaire */
      .defaultFactoryNative = "__MLRegressionLineaireNouveau" },
    { .canonical      = "MLR\xc3\xa9gressionLogistique",   /* MLRégressionLogistique */
      .canonicalAscii = "MLRegressionLogistique",
      .aliases        = ALIAS_mllogistique,
      .category       = "Apprentissage automatique",
      .descFR         = "Régression logistique softmax pour la classification, "
                        "binaire comme multi-classes (K = plus grand indice de "
                        "classe + 1), pour données tabulaires. Aucun objectif à "
                        "régler : le modèle est dédié à la classification. "
                        "L'entraînement est une descente de gradient sur la "
                        "cross-entropy, poids initialisés à zéro (déterministe : "
                        "pas de tirage aléatoire). `.Entraîne(X, y)` ajuste les "
                        "poids (X = Matrice échantillons × variables, y = Tenseur "
                        "1D d'indices de classe 0-based). `.Prédit(X)` rend un "
                        "Tenseur 1D d'indices de classe prédits (classe la plus "
                        "probable). Hyperparamètres réglables par double point : "
                        "`..TauxApprentissage` (défaut 0.1), `..NbItérations` "
                        "(défaut 500). Deux entraînements identiques produisent des "
                        "prédictions identiques.",
      .descEN         = "Softmax logistic regression for classification, both "
                        "binary and multi-class (K = highest class index + 1), for "
                        "tabular data. No objective to set: the model is dedicated "
                        "to classification. Training is gradient descent on the "
                        "cross-entropy, with weights initialized to zero "
                        "(deterministic: no random draw). `.Train(X, y)` fits the "
                        "weights (X = samples × features Matrix, y = 1D 0-based "
                        "class indices Tensor). `.Predict(X)` returns a 1D Tensor of "
                        "predicted class indices (most likely class). "
                        "Hyperparameters set via the double dot: `..LearningRate` "
                        "(default 0.1), `..NumIterations` (default 500). Two "
                        "identical training runs produce identical predictions.",
      .fields         = FIELDS_MLLogistique,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "MLLogisticRegression",
      .genre          = OWL_GENRE_F,          /* une MLRégressionLogistique */
      .defaultFactoryNative = "__MLRegressionLogistiqueNouveau" },
};

void stdlib_mllineaire_init(void) {
    owl_registry_register_types(MLLINEAIRE_TYPES,
        (int)(sizeof(MLLINEAIRE_TYPES) / sizeof(MLLINEAIRE_TYPES[0])));

    /* MLRégressionLinéaire : factory de déclaration + méthodes bilingues. */
    owl_define_native("__MLRegressionLineaireNouveau", fn_mllineaire_nouveau, 0);
    owl_register_method(METHOD_TYPE_MLLINEAIRE, "Entra\xc3\xaene",
                        fn_mllineaire_entraine, 3);
    owl_register_method(METHOD_TYPE_MLLINEAIRE, "Train",
                        fn_mllineaire_entraine, 3);
    owl_register_method(METHOD_TYPE_MLLINEAIRE, "Pr\xc3\xa9""dit",
                        fn_mllineaire_predit, 2);
    owl_register_method(METHOD_TYPE_MLLINEAIRE, "Predict",
                        fn_mllineaire_predit, 2);

    /* MLRégressionLogistique : factory de déclaration + méthodes bilingues. */
    owl_define_native("__MLRegressionLogistiqueNouveau", fn_mllogistique_nouveau, 0);
    owl_register_method(METHOD_TYPE_MLLOGISTIQUE, "Entra\xc3\xaene",
                        fn_mllogistique_entraine, 3);
    owl_register_method(METHOD_TYPE_MLLOGISTIQUE, "Train",
                        fn_mllogistique_entraine, 3);
    owl_register_method(METHOD_TYPE_MLLOGISTIQUE, "Pr\xc3\xa9""dit",
                        fn_mllogistique_predit, 2);
    owl_register_method(METHOD_TYPE_MLLOGISTIQUE, "Predict",
                        fn_mllogistique_predit, 2);
}
