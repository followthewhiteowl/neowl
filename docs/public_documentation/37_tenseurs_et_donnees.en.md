# Tensors and data: the foundation of learning

## 1. In brief

A `Tensor` is the **differentiable numeric container** on which machine
learning rests in the language. Where the [`Vector`](33_vecteurs.md) is a
one-dimensional sequence of reals, the `Tensor` takes **any shape** —
scalar, 1D, 2D (a matrix), or N dimensions — and **remembers the graph of
the computations** that produced it. It is this graph that makes it
possible to automatically trace gradients back, the central ingredient of
training a network.

This sheet describes two complementary foundations: the `Tensor` in depth
(shapes, operations, autograd, losses), then the **data** ready for
learning (`Dataset`, labeled or annotated images, loaders, batches,
splitting, augmentation). Assembling these bricks into a trained network
is described in [`34_iamodele.md`](34_iamodele.md); the low-level training
loop and trainable object detection in
[`38_entrainement_et_detection.md`](38_entrainement_et_detection.md).

```owl
t is a Tensor = Tensor([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Reshape([2, 3])
Trace(t..Value[1][1])       // 1
Trace(t..Value[2][3])       // 6
```

---

## 2. The `Tensor` beyond the `Vector`

A `Tensor` is built from a **flat array of reals** passed to
`Tensor([...])`: the data supplied becomes its value, its gradient starts
at zero. Such a tensor is **scalar or 1D**. For a 2D or N-D shape, chain
`.Reshape([shape])`: the elements and their reading order do not change —
the first axis varies the slowest, the last the fastest — only their
layout changes. The product of the requested dimensions must equal the
number of elements.

```owl
flat is a Tensor = Tensor([1.0, 2.0, 3.0, 4.0, 5.0, 6.0])
image is a Tensor = flat.Reshape([2, 3])
Trace(image..Value[1][2])       // 2
Trace(image..Value[2][1])       // 4
```

Four accessors expose the tensor, all through the double dot:

| Property | Returns |
|---|---|
| `t..Value` / `t..Valeur` | The data, as a real, `Vector` (1D), `Matrix` (2D) or nested (N-D) |
| `t..Gradient` / `t..Gradient` | The gradient, of the **same shape** as the value, at zero as long as no backpropagation has happened |
| `t..Shape` / `t..Forme` | The sequence of axis sizes, as a 1-based array |
| `t..Rank` / `t..Rang` | The number of axes |

```owl
t is a Tensor = Tensor([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Reshape([2, 3])
Trace(t..Rank)               // 2
Trace(t..Shape[1])           // 2    — 2 rows
Trace(t..Shape[2])           // 3    — 3 columns
```

The `Vector` remains the right tool for everyday geometry and statistics
([`33_vecteurs.md`](33_vecteurs.md)); the `Tensor` takes over as soon as
several dimensions or automatic differentiation are needed.

---

## 3. Operations and broadcasting

The operators `+` and `*` between two tensors operate **element by
element** and **chain the graph** of computation, the condition for
autograd ([section 4](#4-autograd-gradients-without-writing-them)).

```owl
u is a Tensor = Tensor([1.0, 2.0, 3.0])
v is a Tensor = Tensor([10.0, 20.0, 30.0])
Trace((u + v)..Value[1])        // 11
Trace((u * v)..Value[3])        // 90
```

### Broadcasting

When an axis is `1` on one side and more on the other, the value is
**broadcast** along that axis: a column applies to all columns, a scalar
to all elements. Broadcasting avoids recopying the data by hand and
serves, in particular, to mask an output.

```owl
mat is a Tensor = Tensor([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Reshape([2, 3])
col is a Tensor = Tensor([10.0, 100.0]).Reshape([2, 1])
prod is a Tensor = mat * col       // each row multiplied by its scalar
Trace(prod..Value[1][3])        // 30   — row 1 × 10
Trace(prod..Value[2][1])        // 400  — row 2 × 100
```

### Reductions and rearrangements

| Operation | Native / method | Effect |
|---|---|---|
| Sum along an axis | `TensorSum(t, axis)` / `t.Sum(axis)` | Reduces the axis (1-based), lowers the rank by 1 |
| Mean along an axis | `TensorMean(t, axis)` / `t.Mean(axis)` | Reduces the axis, lowers the rank by 1 |
| Reorder all axes | `TensorPermute(t, order)` / `t.Permute(order)` | Full permutation of the axes |
| Swap two axes | `TensorTranspose(t, a, b)` / `t.Transpose(a, b)` | Generalized transposition |
| Extract channels | `TensorChannelSlice(t, start, length)` / `t.ChannelSlice(start, length)` | Contiguous range on the channel axis (2nd axis), `start` 1-based |

```owl
m is a Tensor = Tensor([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Reshape([2, 3])
columns is a Tensor = m.Sum(1)          // reduces the rows -> 3 columns
Trace(columns..Value[1])       // 5    — 1 + 4
Trace(columns..Value[3])       // 9    — 3 + 6
rows is a Tensor = m.Mean(2)            // reduces the columns -> 2 rows
Trace(rows..Value[1])         // 2    — (1 + 2 + 3) / 3
Trace(rows..Value[2])         // 5    — (4 + 5 + 6) / 3
```

`ChannelSlice` serves to split a multi-channel output into groups — for
example separating a detector's head into box, object presence and class
(see [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md)).
All these operations are differentiable: backpropagation returns each
gradient to its original position.

---

## 4. Autograd: gradients without writing them

Each tensor produced by an operation remembers its parents. A call
`.Backpropagate()` on a scalar tensor walks this graph **backward** and
fills the `..Gradient` of every tensor involved, by the chain rule. No
derivative is written by hand.

```owl
x is a Tensor = Tensor([1.0, 2.0, 3.0])
loss is a Tensor = (x * x).Sum(1)             // sum of squares
loss.Backpropagate()
Trace(x..Gradient[1])            // 2    — d(x²)/dx = 2x
Trace(x..Gradient[2])            // 4
Trace(x..Gradient[3])            // 6
```

Once the gradients are filled, `.GradientDescent(rate)` applies **one
optimization step** to a parameter tensor: it returns a new leaf equal to
`value - rate × gradient`, whose gradient restarts at zero, detached from
the previous graph. It is the elementary gesture of all training.

```owl
w is a Tensor = Tensor([1.0, 2.0, 3.0])
loss is a Tensor = (w * w).Sum(1)
loss.Backpropagate()
wUpd is a Tensor = w.GradientDescent(0.1)      // w - 0.1 × 2w
Trace(Round(wUpd..Value[1], 4))       // 0.8
Trace(Round(wUpd..Value[2], 4))       // 1.6
Trace(Round(wUpd..Value[3], 4))       // 2.4
```

---

## 5. Loss functions

A **loss** measures the gap between a prediction and a target and returns
a scalar `Tensor`, the starting point of backpropagation. The target is a
fixed truth: it never receives a gradient.

| Loss | Native / method | Use |
|---|---|---|
| Quadratic error (MSE) | `TensorMeanSquaredError(p, c)` / `p.MeanSquaredError(c)` | Regression: mean of the squared gaps |
| Cross-entropy | `TensorCrossEntropy(p, c)` / `p.CrossEntropy(c)` | Classification: logits against a one-of-n target, fused softmax |
| Binary cross-entropy (BCE) | `TensorBinaryCrossEntropy(p, c)` / `p.BinaryCrossEntropy(c)` | Binary presence score: logit against a target in `[0, 1]`, fused sigmoid |
| Absolute error (L1) | `TensorL1(p, c)` / `p.L1(c)` | Regression robust to outliers, suited to a bounding box |

```owl
pred is a Tensor = Tensor([2.0, 0.0, 0.0])
target is a Tensor = Tensor([1.0, 0.0, 0.0])
Trace(Round(pred.MeanSquaredError(target)..Value[1], 4))       // 0.3333
Trace(Round(pred.L1(target)..Value[1], 4))                     // 0.3333
```

Cross-entropy receives **logits** (raw scores, before softmax) and a
one-of-n target; its binary variant receives a logit and a target between
0 and 1. The internal fusion of softmax (or sigmoid) with the loss avoids
any numeric overflow.

```owl
logits is a Tensor = Tensor([2.0, 0.0, 0.0])
target is a Tensor = Tensor([1.0, 0.0, 0.0])
Trace(Round(logits.CrossEntropy(target)..Value[1], 4))       // 0.2395

logit is a Tensor = Tensor([2.0])
presence is a Tensor = Tensor([1.0])
Trace(Round(logit.BinaryCrossEntropy(presence)..Value[1], 4))    // 0.1269
```

A complete training loop, chaining prediction, loss, backpropagation and
a descent step, is laid out in
[`38_entrainement_et_detection.md`](38_entrainement_et_detection.md).

---

## 6. The data: `Dataset` and images

A `Dataset` is an **ordered collection of examples** ready for learning.
It is declared with the type of its elements — `Dataset of LabeledImage`
for classification, `Dataset of AnnotatedImage` for detection — and
filled by a loader.

A **`LabeledImage`** carries a disk link `..Path`, a `..Label` (class name
engraved at import) and an `..Image` **materialized lazily**: the file is
read only at the first access to `..Image`, not at import. An
**`AnnotatedImage`** carries instead `..Boxes` (array of
`[class, cx, cy, width, height]` normalized within `[0, 1]`), for object
detection.

| Loader | Source |
|---|---|
| `LoadLabeledImagesFromFolder(root)` | "One folder = one class" tree, classes sorted alphabetically |
| `LoadLabeledImagesFromCSV(file)` | `path;label` manifest, one line per example, order preserved |
| `LoadAnnotatedImagesFromYOLO(folder)` | Images + `.txt` box files (`class cx cy width height`) |

```owl
root is a string = fTempDir() + "\doc_ia_ds"
fMakeDir(root + "\cat")
fMakeDir(root + "\dog")
dSaveImage(dCreateImage(4, 4, RGB(0, 200, 0)), root + "\cat\c1.png")
dSaveImage(dCreateImage(4, 4, RGB(0, 200, 0)), root + "\cat\c2.png")
dSaveImage(dCreateImage(4, 4, RGB(200, 200, 0)), root + "\dog\d1.png")

ds is a Dataset of LabeledImage = LoadLabeledImagesFromFolder(root)
Trace(ds..Count)                 // 3
Trace(ds..ClassCount)            // 2
Trace(ds..Classes[1])            // cat    — alphabetical order
ex is a LabeledImage = ds[1]
Trace(ex..Label)                 // cat
fRemoveDir(root, frRecursive)
```

A `Dataset` exposes `..Count`, `..ClassCount`, `..Classes` (sorted array)
and `..Source`; the indexing `ds[i]` returns the example at rank `i`.

---

## 7. From disk to tensor

The bridge between an image and the network is `ImageToTensor`, which
returns a **channels-first** `Tensor` `C × H × W`: the pixel values are
normalized within `[0, 1]` (divided by 255) over 3 RGB channels, any
alpha channel being ignored. The source image is not modified.

```owl
img is an Image = dCreateImage(2, 2, RGB(255, 0, 0))
t is a Tensor = ImageToTensor(img)
Trace(t..Value[1][1][1])        // 1    — red channel saturated
Trace(t..Value[2][1][1])        // 0    — green channel
Trace(t..Value[3][1][1])        // 0    — blue channel
```

For training, a `Dataset` supplies the data **in batches**, without
materializing the whole set at once:

| Method | Returns |
|---|---|
| `ds.BatchCount(size)` / `.NombreLots` | Number of batches to cover all examples (the last one may be partial) |
| `ds.BatchInputs(size, i)` / `.LotEntrées` | The i-th batch of inputs, `Tensor` `B × C × H × W` |
| `ds.BatchTargets(size, i)` / `.LotCibles` | The i-th batch of targets, class indices |
| `ds.Subset(start, end)` / `.SousEnsemble` | Slice `[start, end[` in fractions, as a new `Dataset` |

```owl
root is a string = fTempDir() + "\doc_ia_lots"
fMakeDir(root + "\a")
fMakeDir(root + "\b")
FOR n = 1 TO 3
	dSaveImage(dCreateImage(4, 4, RGB(0, 200, 0)), root + "\a\a" + n + ".png")
	dSaveImage(dCreateImage(4, 4, RGB(200, 0, 0)), root + "\b\b" + n + ".png")
END
ds is a Dataset of LabeledImage = LoadLabeledImagesFromFolder(root)
Trace(ds..Count)                 // 6
Trace(ds.BatchCount(4))          // 2    — 6 examples / batch of 4 = 2 batches
X is a Tensor = ds.BatchInputs(4, 1)
Y is a Tensor = ds.BatchTargets(4, 1)
Trace(Y..Value[1])               // 0    — class index of the 1st example
sub is a Dataset of LabeledImage = ds.Subset(0.0, 0.5)
Trace(sub..Count)                // 3    — first half
fRemoveDir(root, frRecursive)
```

`.Subset` serves a **deterministic train/test split**: two disjoint
slices of the same set. To shuffle the order before splitting,
`.Shuffle()` reorders the set; the draw follows the seed (`InitRandom`),
so it stays reproducible.

### On-the-fly augmentation

Two methods, **opt-in** and applied on the fly when each batch is built,
diversify a training set without growing the disk:

| Method | Effect |
|---|---|
| `ds.RandomHorizontalFlip()` / `.RetournementHorizontalAléatoire` | Horizontal mirror, probability 0.5 per image |
| `ds.RandomCrop(margin)` / `.RecadrageAléatoire` | Zero-padding of `margin` px then random crop to the original size |

```owl
InitRandom(1)
root is a string = fTempDir() + "\doc_ia_aug"
fMakeDir(root + "\a")
FOR n = 1 TO 4
	dSaveImage(dCreateImage(8, 8, RGB(0, 200, 0)), root + "\a\a" + n + ".png")
END
ds is a Dataset of LabeledImage = LoadLabeledImagesFromFolder(root)
ds.Shuffle()
ds.RandomHorizontalFlip()
ds.RandomCrop(4)
X is a Tensor = ds.BatchInputs(4, 1)          // batch augmented on the fly
Trace(ds..Count)                 // 4
fRemoveDir(root, frRecursive)
```

Augmentation applies only to the training set; the evaluation set stays
as is, for an honest measurement.

A complete example that loads a `Dataset` of images from a folder, splits
it, augments it and trains a convolutional network on it accompanies the
demonstration folder `reseau_cifar`.

---

## 8. French aliases

| EN canon | FR canon |
|---|---|
| `Tensor` | `Tenseur` |
| `..Value` / `..Gradient` | `..Valeur` / `..Gradient` |
| `..Shape` / `..Rank` | `..Forme` / `..Rang` |
| `.Reshape` (`TensorReshape`) | `.Redimensionne` |
| `.Sum` / `.Mean` (`TensorSum` / `TensorMean`) | `.Somme` / `.Moyenne` |
| `.Permute` / `.Transpose` (`TensorPermute` / `TensorTranspose`) | `.Permute` / `.Transpose` |
| `.ChannelSlice` (`TensorChannelSlice`) | `.TrancheCanaux` |
| `.Backpropagate` | `.Rétropropage` |
| `.GradientDescent` (`TensorGradientDescent`) | `.DescenteGradient` |
| `.MeanSquaredError` (`TensorMeanSquaredError`) | `.ErreurQuadratique` |
| `.CrossEntropy` (`TensorCrossEntropy`) | `.EntropieCroisée` |
| `.BinaryCrossEntropy` (`TensorBinaryCrossEntropy`) | `.EntropieCroiséeBinaire` |
| `.L1` (`TensorL1`) | `.L1` |
| `Dataset` | `Dataset` |
| `LabeledImage` / `AnnotatedImage` | `ImageÉtiquetée` / `ImageAnnotée` |
| `ImageToTensor` | `ImageVersTenseur` |
| `LoadLabeledImagesFromFolder` | `ChargeImagesÉtiquetéesDepuisDossier` |
| `LoadLabeledImagesFromCSV` | `ChargeImagesÉtiquetéesDepuisCSV` |
| `LoadAnnotatedImagesFromYOLO` | `ChargeImagesAnnotéesDepuisYOLO` |
| `..Count` / `..ClassCount` / `..Classes` (Dataset) | `..Taille` / `..NbClasses` / `..Classes` |
| `.BatchCount` / `.BatchInputs` / `.BatchTargets` | `.NombreLots` / `.LotEntrées` / `.LotCibles` |
| `.Subset` / `.Shuffle` | `.SousEnsemble` / `.Mélange` |
| `.RandomHorizontalFlip` / `.RandomCrop` | `.RetournementHorizontalAléatoire` / `.RecadrageAléatoire` |

---

## Further reading

| Topic | Sheet |
|---|---|
| The `Vector`, 1D numeric foundation | [`33_vecteurs.md`](33_vecteurs.md) |
| Build, train and query a network (`AIModel`) | [`34_iamodele.md`](34_iamodele.md) |
| Low-level training loop, evaluation and detection | [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md) |
| Number types and coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Exception handling (`WHEN EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
