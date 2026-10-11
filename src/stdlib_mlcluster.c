// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_mlcluster.c — Type `MLKMeans` / `MLKMeans`.
 *
 * Premier modèle NON-SUPERVISÉ de la famille ML : partitionne les lignes d'une
 * Matrice en `..NbClusters` groupes, SANS étiquette. `.Entraîne` ne prend QUE X
 * (un seul argument, pas de y). Algorithme :
 *
 *   - INIT k-means++ : le premier centre est un point tiré au hasard ; chaque
 *     centre suivant est tiré parmi les points avec une probabilité
 *     proportionnelle au carré de la distance au centre déjà choisi le plus
 *     proche (D²). Meilleure convergence que l'init purement aléatoire, et
 *     surtout DÉTERMINISTE à graine fixe.
 *
 *   - LLOYD : jusqu'à convergence (plus aucune réaffectation) ou `..NbItérations`
 *     étapes. (1) chaque point est affecté au centroïde le plus proche (distance
 *     euclidienne au carré) ; (2) chaque centroïde est recalculé = moyenne des
 *     points affectés. Un cluster resté vide est réinitialisé sur le point le
 *     plus éloigné de son propre centroïde (jamais de NaN ni de division par 0).
 *
 * L'aléa vient EXCLUSIVEMENT d'un PRNG LOCAL xorshift seedé par `..Graine`
 * (jamais l'état global Hasard) : deux modèles entraînés avec la même graine,
 * les mêmes données et les mêmes hyperparamètres consomment la MÊME séquence et
 * produisent des affectations bit-exactes. `..Graine=0` = une graine fixe
 * reproductible (splitmix64 mappe 0 sur un état non nul déterministe).
 *
 * Les centroïdes appris sont du C-heap pur (aucune référence OWL sortante) :
 * FEUILLE GC, `centroids` libéré au finaliseur (cf. object.c / gc.c).
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "object.h"
#include "vm.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* ================================================================
 * PRNG local reproductible (même patron que stdlib_mlarbre.c / )
 * ================================================================ */

/* Dérive un état 64 bits non nul depuis la graine (splitmix64). Seed 0 -> état
 * fixe reproductible ; l'état résultant est toujours ≠ 0 (xorshift l'exige). */
static uint64_t mlk_rng_seed(int64_t seed) {
    uint64_t z = (uint64_t)seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return z | 1ULL;   /* jamais 0 */
}

/* Prochain entier pseudo-aléatoire (xorshift64, période 2^64-1). */
static uint64_t mlk_rng_next(uint64_t* s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* Réel pseudo-aléatoire dans [0, 1) (53 bits de mantisse). */
static double mlk_rng_double(uint64_t* s) {
    return (double)(mlk_rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* ================================================================
 * Lecture des données d'entrée (mêmes conventions que stdlib_mllineaire.c)
 * ================================================================ */

/* Extrait d'une valeur X une matrice d'échantillons row-major : `data`
 * (rows×cols contigus), `rows`, `cols`. Accepte une Matrice ou un Tenseur 2D. */
static bool mlk_read_matrix(Value v, const double** data, int* rows, int* cols) {
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

/* Distance euclidienne au carré entre deux vecteurs de `ncols` réels. */
static double mlk_sqdist(const double* a, const double* b, int ncols) {
    double s = 0.0;
    for (int j = 0; j < ncols; j++) {
        double d = a[j] - b[j];
        s += d * d;
    }
    return s;
}

/* Indice du centroïde le plus proche du point `x` (plus petit indice sur
 * égalité), parmi `k` centroïdes de `ncols` variables. */
static int mlk_nearest(const double* x, const double* centroids, int k, int ncols) {
    int best = 0;
    double bestD = mlk_sqdist(x, &centroids[0], ncols);
    for (int c = 1; c < k; c++) {
        double d = mlk_sqdist(x, &centroids[(size_t)c * ncols], ncols);
        if (d < bestD) { bestD = d; best = c; }
    }
    return best;
}

/* ================================================================
 * Init k-means++ (déterministe, PRNG local seedé)
 * ================================================================ */

/* Remplit `centroids` (k×ncols, déjà alloué) par k-means++ : premier centre
 * tiré uniformément, chaque suivant tiré ∝ D² (carré de la distance au centre
 * choisi le plus proche). `dist2` (nrows, déjà alloué) sert de tampon. */
static void mlk_init_pp(const double* X, int nrows, int ncols, int k,
                        double* centroids, double* dist2, uint64_t* rng) {
    int first = (int)(mlk_rng_next(rng) % (uint64_t)nrows);
    for (int j = 0; j < ncols; j++)
        centroids[j] = X[(size_t)first * ncols + j];

    for (int i = 0; i < nrows; i++)
        dist2[i] = mlk_sqdist(&X[(size_t)i * ncols], &centroids[0], ncols);

    for (int c = 1; c < k; c++) {
        double total = 0.0;
        for (int i = 0; i < nrows; i++) total += dist2[i];

        int chosen;
        if (total <= 0.0) {
            /* Tous les points restants coïncident avec un centre : tirage
             * uniforme (évite une division par zéro). */
            chosen = (int)(mlk_rng_next(rng) % (uint64_t)nrows);
        } else {
            double r = mlk_rng_double(rng) * total;
            double cum = 0.0;
            chosen = nrows - 1;   /* garde-fou d'arrondi : dernier point */
            for (int i = 0; i < nrows; i++) {
                cum += dist2[i];
                if (cum >= r) { chosen = i; break; }
            }
        }
        for (int j = 0; j < ncols; j++)
            centroids[(size_t)c * ncols + j] = X[(size_t)chosen * ncols + j];

        /* Met à jour la distance au centre le plus proche pour chaque point. */
        for (int i = 0; i < nrows; i++) {
            double d = mlk_sqdist(&X[(size_t)i * ncols],
                                  &centroids[(size_t)c * ncols], ncols);
            if (d < dist2[i]) dist2[i] = d;
        }
    }
}

/* ================================================================
 * MLKMeans — factory + méthodes
 * ================================================================ */

/* Factory interne émise par le compilateur pour `km est un MLKMeans`. */
static Value fn_mlkmeans_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_mlkmeans());
}

/* .Entraîne(X) / .Train(X) — UN SEUL argument (non-supervisé, pas de y).
 * Calcule les centroïdes par init k-means++ puis itérations de Lloyd. X vide ou
 * ..NbClusters hors de [1, nb lignes] -> soft-error (ErreurDétectée). Rend le
 * receveur (chaînage fluide). */
static Value fn_mlkmeans_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjMLKMeans* km = AS_MLKMEANS(args[0]);

    const double* X; int nrows, ncols;
    if (!mlk_read_matrix(args[1], &X, &nrows, &ncols) || nrows < 1 || ncols < 1) {
        owl_emit_native_error(E_NATIVE_ML_X_VIDE, "Entra\xc3\xaene", NULL);
        return args[0];
    }
    int k = km->numClusters;
    if (k < 1 || k > nrows) {
        owl_emit_native_error(E_NATIVE_ML_CLUSTERS_INVALIDE, "Entra\xc3\xaene", NULL);
        return args[0];
    }

    /* Toute l'allocation est C-heap : aucune allocation OWL pendant
     * l'entraînement (X reste rooté via args[1]). */
    obj_mlkmeans_free_centroids(km);
    double* centroids = (double*)malloc((size_t)k * (size_t)ncols * sizeof(double));
    double* sums      = (double*)malloc((size_t)k * (size_t)ncols * sizeof(double));
    int*    counts    = (int*)malloc((size_t)k * sizeof(int));
    int*    labels    = (int*)malloc((size_t)nrows * sizeof(int));
    double* dist2     = (double*)malloc((size_t)nrows * sizeof(double));

    uint64_t rng = mlk_rng_seed(km->seed);
    mlk_init_pp(X, nrows, ncols, k, centroids, dist2, &rng);

    for (int i = 0; i < nrows; i++) labels[i] = -1;

    int maxIter = km->numIterations > 0 ? km->numIterations : 1;
    for (int it = 0; it < maxIter; it++) {
        /* (1) Affectation au centroïde le plus proche. */
        bool changed = false;
        for (int i = 0; i < nrows; i++) {
            int c = mlk_nearest(&X[(size_t)i * ncols], centroids, k, ncols);
            if (labels[i] != c) { labels[i] = c; changed = true; }
        }
        if (!changed) break;   /* convergence : plus aucune réaffectation */

        /* (2) Recalcul des centroïdes = moyenne des points affectés. */
        for (int c = 0; c < k; c++) {
            counts[c] = 0;
            for (int j = 0; j < ncols; j++) sums[(size_t)c * ncols + j] = 0.0;
        }
        for (int i = 0; i < nrows; i++) {
            int c = labels[i];
            counts[c]++;
            const double* xi = &X[(size_t)i * ncols];
            for (int j = 0; j < ncols; j++) sums[(size_t)c * ncols + j] += xi[j];
        }
        for (int c = 0; c < k; c++) {
            if (counts[c] > 0) {
                for (int j = 0; j < ncols; j++)
                    centroids[(size_t)c * ncols + j] =
                        sums[(size_t)c * ncols + j] / (double)counts[c];
            } else {
                /* Cluster vide : réinitialiser sur le point le plus éloigné de
                 * son propre centroïde (déterministe, pas de NaN). On exclut les
                 * points déjà réutilisés ce tour-ci pour éviter les doublons. */
                int far = -1;
                double farD = -1.0;
                for (int i = 0; i < nrows; i++) {
                    double d = mlk_sqdist(&X[(size_t)i * ncols],
                                          &centroids[(size_t)labels[i] * ncols], ncols);
                    if (d > farD) { farD = d; far = i; }
                }
                if (far >= 0) {
                    for (int j = 0; j < ncols; j++)
                        centroids[(size_t)c * ncols + j] = X[(size_t)far * ncols + j];
                    /* Rattache ce point au cluster ressuscité pour ne pas le
                     * réélire pour un autre cluster vide du même tour. */
                    labels[far] = c;
                }
            }
        }
    }

    km->centroids = centroids;
    km->nFeatures = ncols;
    km->trained   = true;

    free(sums);
    free(counts);
    free(labels);
    free(dist2);
    return args[0];
}

/* .Prédit(X) / .Predict(X) — rend un Tenseur 1D : pour chaque ligne, l'indice
 * (0-based) du cluster dont le centroïde est le plus proche. */
static Value fn_mlkmeans_predit(int argCount, Value* args) {
    (void)argCount;
    ObjMLKMeans* km = AS_MLKMEANS(args[0]);
    if (!km->trained) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""dit", NULL);
        return NIL_VAL;
    }
    const double* X; int nrows, ncols;
    if (!mlk_read_matrix(args[1], &X, &nrows, &ncols)) {
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
    int k = km->numClusters;
    int m = ncols < km->nFeatures ? ncols : km->nFeatures;
    for (int i = 0; i < nrows; i++) {
        cells[i] = (double)mlk_nearest(&X[(size_t)i * ncols], km->centroids, k, m);
    }
    return OBJ_VAL(out);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const char* const ALIAS_mlkmeans[] = { NULL };   /* même terme FR/EN */

/* MLKMeans : hyperparamètres réglables + centroïdes en lecture seule. Le
 * STOCKAGE réel vit dans ObjMLKMeans et la lecture/écriture double-point est
 * câblée dans vm.c (OP_STRUCT_GET/SET, branche IS_MLKMEANS) : ces entrées
 * publient les paires bilingues FR/EN pour l'explain LSP et `owl translate`. */
static const OwlField FIELDS_MLKMeans[] = {
    {"NbClusters", NULL, "entier", "8", "nombre de groupes à former (le k)", "number of groups to form (the k)", .nameEN="NumClusters"},
    {"NbIterations", "NbIt\xc3\xa9rations", "entier", "300", "nombre maximal d'itérations de l'algorithme avant l'arrêt", "maximum number of algorithm iterations before stopping", .nameEN="NumIterations"},
    {"Graine", NULL, "entier", "0", "graine du générateur pseudo-aléatoire qui initialise les centres (fixe la reproductibilité)", "seed of the pseudo-random generator that initializes the centers (fixes reproducibility)", .nameEN="Seed"},
    {"Centroides", "Centro\xc3\xaf""des", "Matrice", "", "centres appris, une ligne par cluster et une colonne par variable, disponibles après l'entraînement", "learned centers, one row per cluster and one column per feature, available after training", .nameEN="Centroids"},
    {0},
};

static const OwlTypeDef MLKMEANS_TYPES[] = {
    { .canonical      = "MLKMeans",
      .canonicalAscii = "MLKMeans",
      .aliases        = ALIAS_mlkmeans,
      .category       = "Apprentissage automatique",
      .descFR         = "Clustering non-supervisé par l'algorithme de Lloyd "
                        "(k-means), pour données tabulaires. Regroupe les lignes "
                        "d'une Matrice en `..NbClusters` groupes SANS étiquette : "
                        "`.Entraîne(X)` ne prend qu'un seul argument. Les centres "
                        "initiaux sont choisis par k-means++ (tirage proportionnel "
                        "au carré de la distance), à partir d'un générateur "
                        "pseudo-aléatoire local seedé par `..Graine` — d'où une "
                        "reproductibilité exacte à graine fixe. `.Prédit(X)` rend "
                        "un Tenseur 1D d'indices de cluster 0-based (le centroïde "
                        "le plus proche de chaque ligne). Les indices de cluster "
                        "sont arbitraires : rien ne garantit qu'un groupe reçoive "
                        "toujours le même numéro. Hyperparamètres réglables par "
                        "double point : `..NbClusters` (défaut 8), `..NbItérations` "
                        "(défaut 300), `..Graine` (défaut 0). Les centres appris se "
                        "lisent par `..Centroïdes` (Matrice k×variables).",
      .descEN         = "Unsupervised clustering by Lloyd's algorithm (k-means), "
                        "for tabular data. Groups the rows of a Matrix into "
                        "`..NumClusters` groups WITHOUT labels: `.Train(X)` takes a "
                        "single argument. Initial centers are chosen by k-means++ "
                        "(draw proportional to squared distance), from a local "
                        "pseudo-random generator seeded by `..Seed` — hence exact "
                        "reproducibility at a fixed seed. `.Predict(X)` returns a "
                        "1D Tensor of 0-based cluster indices (the nearest centroid "
                        "of each row). Cluster indices are arbitrary: nothing "
                        "guarantees that a group always gets the same number. "
                        "Hyperparameters set via the double dot: `..NumClusters` "
                        "(default 8), `..NumIterations` (default 300), `..Seed` "
                        "(default 0). Learned centers are read via `..Centroids` "
                        "(a k×features Matrix).",
      .fields         = FIELDS_MLKMeans,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "MLKMeans",
      .genre          = OWL_GENRE_M,          /* un MLKMeans */
      .defaultFactoryNative = "__MLKMeansNouveau" },
};

void stdlib_mlcluster_init(void) {
    owl_registry_register_types(MLKMEANS_TYPES,
        (int)(sizeof(MLKMEANS_TYPES) / sizeof(MLKMEANS_TYPES[0])));

    owl_define_native("__MLKMeansNouveau", fn_mlkmeans_nouveau, 0);
    owl_register_method(METHOD_TYPE_MLKMEANS, "Entra\xc3\xaene",
                        fn_mlkmeans_entraine, 2);
    owl_register_method(METHOD_TYPE_MLKMEANS, "Train",
                        fn_mlkmeans_entraine, 2);
    owl_register_method(METHOD_TYPE_MLKMEANS, "Pr\xc3\xa9""dit",
                        fn_mlkmeans_predit, 2);
    owl_register_method(METHOD_TYPE_MLKMEANS, "Predict",
                        fn_mlkmeans_predit, 2);
}
