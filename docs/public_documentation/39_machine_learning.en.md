# Machine learning on tabular data

## 1. In brief

Where the [`Tensor`](37_tenseurs_et_donnees.md) and the [`AIModel`](34_iamodele.md)
cover neural networks, a second family handles **tabular data** — rows
of observations described by columns of numeric variables. It gathers
the classic machine learning models: decision trees, tree ensembles,
regressions and clustering.

All these models share one **estimator pattern**: a type prefixed
`ML`, tuned by its properties, trained by `.Train`, queried by
`.Predict`. This sheet describes that pattern, reviews the six models,
then shows how to prepare the data and evaluate a model on examples it
has never seen.

```owl
// A classifier, from tuning to prediction, in four lines.
X is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5]]
y is a Tensor = Tensor([[0.0], [0.0], [1.0], [1.0]])
tree is an MLDecisionTree
tree..Objective = objClassification
tree.Train(X, y)
Trace("accuracy " + Accuracy(tree.Predict(X), y))       // 1
```

---

## 2. The estimator pattern

Each model follows the same life cycle, whatever the task.

1. **Declare** the model by its `ML…` type.
2. **Tune** its properties with the double dot (`model..Property = …`):
   number of trees, seed, maximum depth — each has a reasonable
   default, to adjust as needed.
3. **Train** with `.Train(X, y)`, where `X` is a `Matrix` of
   observations (one row per example, one column per variable) and `y`
   a column `Tensor` of targets (one value per row of `X`).
4. **Predict** with `.Predict(X)`, which returns a `Tensor` of
   predictions.

`.Train` and `.Predict` are **methods** (single dot); the settings and
the learned results are **properties** (double dot). An unsupervised
model like `MLKMeans` trains on `X` alone, without a target.

### The explicit objective

Three models can handle **two distinct tasks** — predict a class
(classification) or a continuous value (regression). For those, the
objective is set explicitly before training, it is never guessed:

```owl
// The same tree type, on both objectives.
Xc is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5]]
yc is a Tensor = Tensor([[0.0], [0.0], [1.0], [1.0]])
classif is an MLDecisionTree
classif..Objective = objClassification
classif.Train(Xc, yc)
Trace("predicted class " + classif.Predict(Xc)..Value[3])       // 1

Xr is a Matrix = [[1.0], [2.0], [3.0], [4.0], [5.0], [6.0]]
yr is a Tensor = Tensor([[10.0], [20.0], [30.0], [40.0], [50.0], [60.0]])
regress is an MLDecisionTree
regress..Objective = objRegression
regress.Train(Xr, yr)
Trace("regression R² " + Round(RSquared(regress.Predict(Xr), yr), 3))       // 1
```

The objective takes one of the two values `objClassification` or
`objRegression`. It must be set before `.Train`: a multi-task model
without an objective does not train.

---

## 3. The six models

| Model | Tasks | Main settings |
|---|---|---|
| `MLDecisionTree` | classification, regression | `..Objective`, `..MaxDepth`, `..MinSamplesLeaf` |
| `MLRandomForest` | classification, regression | `..Objective`, `..NumTrees`, `..Seed` |
| `MLGradientBoosting` | classification, regression | `..Objective`, `..NumTrees`, `..LearningRate` |
| `MLLinearRegression` | regression | `..Coefficients`, `..Intercept` (learned) |
| `MLLogisticRegression` | classification | `..LearningRate`, `..NumIterations` |
| `MLKMeans` | clustering (unsupervised) | `..NumClusters`, `..Seed`, `..Centroids` (learned) |

The first three carry an explicit `..Objective`. The two regressions
are single-task — one continuous, the other binary — and so have no
objective to set. `MLKMeans` is unsupervised: it trains without a
target.

### Decision tree

A tree recursively splits the variable space by thresholds. Readable
and requiring no prior scaling, it serves as the building block of the
two ensembles that follow.

```owl
X is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y is a Tensor = Tensor([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
tree is an MLDecisionTree
tree..Objective = objClassification
tree..MaxDepth = 3
tree.Train(X, y)
Trace("accuracy " + Accuracy(tree.Predict(X), y))       // 1
```

### Random forest

A forest trains many trees on random draws of the data and the
variables, then aggregates their votes. More robust than a single
tree. The seed fixes the reproducibility of the draw.

```owl
X is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y is a Tensor = Tensor([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
forest is an MLRandomForest
forest..Objective = objClassification
forest..NumTrees = 50
forest..Seed = 13
forest.Train(X, y)
Trace("accuracy " + Accuracy(forest.Predict(X), y))       // 1
```

### Gradient boosting

Boosting stacks shallow trees, each correcting the residual errors of
the previous ones. The learning rate weights each tree's contribution.

```owl
X is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y is a Tensor = Tensor([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
boost is an MLGradientBoosting
boost..Objective = objClassification
boost..LearningRate = 0.1
boost.Train(X, y)
Trace("accuracy " + Accuracy(boost.Predict(X), y))       // 1
```

### Linear regression

Linear regression fits a weighted combination of the variables plus a
bias. After training, the learned weights read in `..Coefficients`
(one per variable) and the bias in `..Intercept`.

```owl
X is a Matrix = [[1.0], [2.0], [3.0], [4.0]]
y is a Tensor = Tensor([[7.0], [9.0], [11.0], [13.0]])
lin is an MLLinearRegression
lin.Train(X, y)
Trace("coefficient " + Round(lin..Coefficients[1], 2))       // 2
Trace("intercept " + Round(lin..Intercept, 2))               // 5
Trace("R² " + Round(RSquared(lin.Predict(X), y), 3))       // 1
```

### Logistic regression

Despite its name, logistic regression is a **classifier**: it learns a
linear boundary between classes by gradient descent.

```owl
X is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y is a Tensor = Tensor([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
logit is an MLLogisticRegression
logit.Train(X, y)
Trace("accuracy " + Accuracy(logit.Predict(X), y))       // 1
```

### KMeans

`MLKMeans` groups rows by proximity, **without a label**: it trains on
`X` alone. `.Predict` returns the cluster number of each row (0-based),
and the learned centers read in `..Centroids`.

```owl
X is a Matrix = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
km is an MLKMeans
km..NumClusters = 2
km..Seed = 13
km.Train(X)
assign is a Tensor = km.Predict(X)
Trace("cluster of row 1 " + assign..Value[1])       // 0
centers is a Matrix = km..Centroids
Trace("number of centers " + centers..Rows)          // 2
```

---

## 4. Preparing the data

### Loading a numeric CSV

`CSVToMatrix(csv, separator, ignoreHeader)` converts an all-numeric
CSV content — a string already in memory, not a path — into a
`Matrix`.

```owl
csv is a string = "1.0;2.0" + CRLF + "3.0;4.0" + CRLF + "5.0;6.0"
X is a Matrix = CSVToMatrix(csv, ";", False)
Trace("dimensions " + X..Rows + " × " + X..Columns)       // 3 × 2
```

### Putting the variables on the same scale

Distance-based models (including `MLKMeans`) are sensitive to variable
magnitudes. Two scalings, column by column:

| Native | Effect |
|---|---|
| `MatrixStandardizeColumns(m)` | Centers and reduces each column: mean 0, standard deviation 1 |
| `MatrixNormalizeColumns(m)` | Brings each column onto the interval `[0, 1]` (min-max) |

```owl
csv is a string = "1.0;2.0" + CRLF + "3.0;4.0" + CRLF + "5.0;6.0"
X is a Matrix = CSVToMatrix(csv, ";", False)
Xstd is a Matrix = MatrixStandardizeColumns(X)
Trace("standardized [1][1] " + Round(Xstd[1][1], 3))       // -1.225
Xnorm is a Matrix = MatrixNormalizeColumns(X)
Trace("normalized [1][1] " + Round(Xnorm[1][1], 3))        // 0
Trace("normalized [3][1] " + Round(Xnorm[3][1], 3))        // 1
```

Each native returns a **new** `Matrix`; the source is not modified. A
constant column comes out at `0.0` everywhere, never undefined.

### Splitting into training and test

Honestly evaluating a model means judging it on rows **held apart**,
never seen at training. `MatrixSubRows(m, indices)` extracts the rows
of a `Matrix` by their indices (1-based), in the array's order and
with repetition allowed: enough to materialize a subset directly,
without a hand-written loop.

```owl
X is a Matrix = [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0], [7.0, 8.0]]
Xtrain is a Matrix = MatrixSubRows(X, [1, 2, 3])
Xtest is a Matrix = MatrixSubRows(X, [4])
Trace("training rows " + Xtrain..Rows)       // 3
Trace("test [1][2] " + Xtest[1][2])          // 8
```

The same native materializes the test subset. To shuffle the order
before splitting, shuffling the indices with a fixed seed
(`InitRandom`) keeps the split reproducible.

A target is stored in a column `Matrix` with `ArrayToColumnMatrix`: a
flat array of reals becomes an `N × 1` `Matrix`, ready to pass as a
`Tensor`.

```owl
targets is an array = [0.0, 1.0, 1.0, 0.0]
y is a Matrix = ArrayToColumnMatrix(targets)
Trace("dimensions " + y..Rows + " × " + y..Columns)       // 4 × 1
Trace("y[2][1] " + y[2][1])                               // 1
```

---

## 5. Evaluating a model

A prediction is judged by **metrics**, without gradient: simple
quality measures. Classification metrics compare class indices
(0-based); regression ones compare continuous values.

| Metric | Native | Task |
|---|---|---|
| Accuracy | `Accuracy(pred, targets)` | Classification — fraction correctly classified, in `[0, 1]` |
| Per-class precision | `Precision(pred, targets)` | Classification — `Vector`, true positives / predicted positives |
| Per-class recall | `Recall(pred, targets)` | Classification — `Vector`, true positives / actual positives |
| Per-class F1 score | `F1(pred, targets)` | Classification — `Vector`, harmonic mean of the two |
| Coefficient of determination | `RSquared(pred, targets)` | Regression — R², share of variance explained |
| Mean squared error | `MeanSquaredError(pred, targets)` | Regression — mean of the squared deviations |
| Mean absolute error | `MeanAbsoluteError(pred, targets)` | Regression — mean of the absolute deviations |

```owl
// Classification: 4 examples, 2 classes.
pred is a Tensor = Tensor([0.0, 0.0, 1.0, 1.0])
targets is a Tensor = Tensor([0.0, 1.0, 1.0, 1.0])
Trace("accuracy " + Round(Accuracy(pred, targets), 3))       // 0.75
precisions is a Vector = Precision(pred, targets)
recalls is a Vector = Recall(pred, targets)
f1s is a Vector = F1(pred, targets)
Trace("precision class 1 " + Round(precisions[2], 3))       // 1
Trace("recall class 1 " + Round(recalls[2], 3))            // 0.667
Trace("macro F1 " + Round(f1s.Average(), 3))                // 0.733
```

`Precision`, `Recall` and `F1` return a `Vector` indexed by class
(1-based position = 0-based class + 1). The **macro average** —
unweighted mean over classes — is obtained with a `.Average()` on that
vector.

```owl
// Regression: perfect predictions -> R² = 1, zero errors.
pred is a Tensor = Tensor([10.0, 20.0, 30.0])
targets is a Tensor = Tensor([10.0, 20.0, 30.0])
Trace("R² " + Round(RSquared(pred, targets), 3))       // 1
Trace("MSE " + Round(MeanSquaredError(pred, targets), 3))      // 0
Trace("MAE " + Round(MeanAbsoluteError(pred, targets), 3))          // 0
```

---

## 6. The complete workflow

End to end, a tabular pipeline chains: building or loading the data,
scaling the variables, train/test split, training several models on
the train set, then measuring on the held-apart test set. Comparing
the models on the same test rows reveals which one generalizes best.

A complete example — a dataset built on the fly, standardization,
split, comparison of the six models across the three tasks
(classification, regression, clustering) — accompanies the
`ml_tabulaire` demonstration folder. It depends on no external file and
finishes in a fraction of a second.

---

## 7. Current out-of-scope

These uses are **not yet covered**. They are named so that an
unhandled need is understood as an upcoming milestone, not a silent
defect:

- **Cross-validation** — evaluation relies on a hand-composed
  train/test split; there is no automated k-fold cross-validation.
- **Dimensionality reduction (PCA)** — principal component analysis
  and other projections are not provided.
- **Nearest neighbors and support vector machines** — kNN and SVM
  models are not part of this family.

---

## 8. French aliases

| EN canon | FR canon |
|---|---|
| `MLDecisionTree` | `MLArbreDécision` |
| `MLRandomForest` | `MLForêtAléatoire` |
| `MLGradientBoosting` | `MLGradientBoosting` |
| `MLLinearRegression` | `MLRégressionLinéaire` |
| `MLLogisticRegression` | `MLRégressionLogistique` |
| `MLKMeans` | `MLKMeans` |
| `.Train` / `.Predict` | `.Entraîne` / `.Prédit` |
| `..Objective` | `..Objectif` |
| `objClassification` / `objRegression` | `objClassification` / `objRégression` |
| `..NumTrees` / `..Seed` | `..NbArbres` / `..Graine` |
| `..MaxDepth` | `..ProfondeurMax` |
| `..LearningRate` / `..NumIterations` | `..TauxApprentissage` / `..NbItérations` |
| `..Coefficients` / `..Intercept` | `..Coefficients` / `..Ordonnée` |
| `..NumClusters` / `..Centroids` | `..NbClusters` / `..Centroïdes` |
| `CSVToMatrix` | `CSVVersMatrice` |
| `MatrixStandardizeColumns` | `MatriceStandardiseColonnes` |
| `MatrixNormalizeColumns` | `MatriceNormaliseColonnes` |
| `MatrixSubRows` | `MatriceSousLignes` |
| `ArrayToColumnMatrix` | `TableauVersMatriceColonne` |
| `Accuracy` | `Exactitude` |
| `Precision` / `Recall` / `F1` | `Précision` / `Rappel` / `F1` |
| `RSquared` | `CoefficientDétermination` |
| `MeanSquaredError` | `ErreurQuadratiqueMoyenne` |
| `MeanAbsoluteError` | `ErreurAbsolueMoyenne` |

---

## Further reading

| Topic | Sheet |
|---|---|
| The `Tensor` foundation, autograd, losses and data | [`37_tenseurs_et_donnees.md`](37_tenseurs_et_donnees.md) |
| Build, train and query a network (`AIModel`) | [`34_iamodele.md`](34_iamodele.md) |
| Low-level training loop, evaluation and detection | [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md) |
| The `Matrix` and `Vector`, numeric foundations | [`33_vecteurs.md`](33_vecteurs.md) |
| Number types and coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
