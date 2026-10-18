# Neural networks: `AIModel`

## 1. In brief

An `AIModel` is a **neural network** that you build by stacking layers,
**train** on data, then query to **predict**. It targets the classic
supervised learning tasks — classification, regression — on numeric
data.

The API comes down to four moves: **build** the layers, **train** the
model, **infer** an output, **save / load** the model to disk. The values
that flow through it — inputs, outputs, weights — are `Tensor` (see
[`33_vecteurs.md`](33_vecteurs.md) for the numeric foundation).

```owl
model is an AIModel
model.AddDenseLayer(4, 8)
model.AddReLULayer()
model.AddDenseLayer(8, 3)
Trace(model..LayerCount)      // 3
```

The current scope is **explicit**: `AIModel` handles **dense networks**
(multilayer perceptrons) today. What it does not handle yet is named
plainly in [section 9](#9-out-of-current-scope) — an out-of-scope case
raises an error that points at the gap, never an obscure crash.

---

## 2. Building a network

A model is built by **stacking layers** through fluent calls, in the order
the signal travels through them. Two families of layers exist today.

**Dense layers** (`AddDenseLayer(inputs, outputs)`) connect each input to
each output. They carry the learned **weights**. The number of `outputs`
of a layer must match the number of `inputs` of the next dense layer.

**Activation layers** introduce non-linearity, without which a stack of
dense layers would collapse into a single one. Three activations are
available: `AddReLULayer`, `AddSigmoidLayer`, `AddTanhLayer`.

```owl
model is an AIModel
model.AddDenseLayer(4, 8)
model.AddReLULayer()
model.AddDenseLayer(8, 3)
```

| Layer | Role |
|---|---|
| `.AddDenseLayer(i, o)` / `.AjouteCoucheDense` | Dense layer, `i` inputs to `o` outputs |
| `.AddReLULayer()` / `.AjouteCoucheReLU` | ReLU activation (zeroes out negative values) |
| `.AddSigmoidLayer()` / `.AjouteCoucheSigmoïde` | Sigmoid activation (maps into `]0, 1[`) |
| `.AddTanhLayer()` / `.AjouteCoucheTanh` | Hyperbolic tangent activation (maps into `]-1, 1[`) |

### Weight initialization

The weights of a dense layer are drawn **at random when it is created**,
following a strategy that shapes the learning speed. An optional third
argument picks that strategy; by default, `initHe` applies.

| Strategy | Suits |
|---|---|
| `initHe` (default) | Layers followed by a ReLU activation |
| `initXavier` | Layers followed by a sigmoid or hyperbolic tangent activation |

```owl
model is an AIModel
model.AddDenseLayer(4, 8, initHe)       // explicit, but this is the default
model.AddReLULayer()
model.AddDenseLayer(8, 3, initXavier)
```

### Building parallel branches

Not every architecture is a plain stack. The signal can **split** into
several parallel paths from a single point, then those paths **converge**
into a single output. Such a block is delimited by four markers, inserted
into the construction like layers.

| Marker | Role |
|---|---|
| `.BeginBranches()` / `.DébutBranches` | Captures the current activation — the **divergence point** — and opens the first branch |
| `.NextBranch()` / `.BrancheSuivante` | Closes the current branch and opens a new one, starting again from the **same** divergence point |
| `.MergeByAdd()` / `.FusionneParAddition` | Closes the block and **adds** the branch outputs element by element |
| `.MergeByConcat()` / `.FusionneParConcaténation` | Closes the block and lays the outputs **end to end** along the channel axis |

Layers added between two markers feed the current branch. An **empty**
branch — no layers — lets the input pass through unchanged: it acts as the
**identity**. An identity shortcut is therefore expressed with one branch
carrying layers and one empty branch, merged by addition. Branches merge in
their declaration order.

Merge preconditions differ by mode:

- **Merge by addition**: the outputs of all branches must have exactly the
  **same shape** — same spatial dimensions *and* same number of channels —,
  the sum being computed cell by cell.
- **Merge by concatenation**: the spatial dimensions must coincide (same
  batch size, same height, same width), but the **number of channels may
  differ** — the output carries the sum. Incompatible spatial dimensions
  raise an error rather than producing an inconsistent result.

A **residual block with projection** adds a main path and a shortcut that
adapts the number of channels. The main path transforms the input (a 3×3
convolution followed by batch normalization); the shortcut reprojects it
(a 1×1 convolution + batch normalization) so the two outputs share the same
shape before the addition.

```owl
model is an AIModel
model.BeginBranches()
    model.AddConvolution2DLayer(3, 8, 3, 1, 1)   // main path: F(x)
    model.AddBatchNormLayer(8)
model.NextBranch()
    model.AddConvolution2DLayer(3, 8, 1, 1, 0)   // shortcut: 1×1 projection
    model.AddBatchNormLayer(8)
model.MergeByAdd()
```

The arguments of `AddConvolution2DLayer` are, in order: number of input
channels, number of output channels, kernel size, stride and padding. Here
both branches produce 8 channels at identical spatial dimensions, which
merging by addition requires.

A **two-branch module** applies two convolutions with different kernel
sizes to the same input, then places their outputs side by side.
Concatenation allows distinct channel counts — here 4 and 6 —, the output
carrying the sum (10).

```owl
model is an AIModel
model.BeginBranches()
    model.AddConvolution2DLayer(3, 4, 1, 1, 0)   // 1×1 kernel branch
model.NextBranch()
    model.AddConvolution2DLayer(3, 6, 3, 1, 1)   // 3×3 kernel branch
model.MergeByConcat()
```

`.Infer` traverses a branch block with no special handling: it replays each
branch from the divergence point, then merges according to the closing
marker. Calling `.NextBranch`, `.MergeByAdd` or `.MergeByConcat` **without**
an open `.BeginBranches` raises an error.

---

## 3. Inferring

`.Infer(input)` runs an input through all the layers, in stacking order, and
returns the network output. The output shape follows that of the input.

**A single sample** is a one-dimensional (1D) `Tensor` whose size is the
number of inputs of the first layer. The output is a 1D `Tensor` the size of
the last layer.

```owl
sample is a Tensor = Tensor([1.0, 0.5, -1.0, 2.0])
output is a Tensor = model.Infer(sample)
prediction is a Vector = output..Value
Trace(prediction..Length)      // 3
```

**A batch of samples** is a two-dimensional (2D) `Tensor`, `B` rows of
`inputs` columns. The output is a 2D `Tensor` `B × outputs`: the batch
dimension traverses the network unchanged. Processing a batch in a single
call is more efficient than a loop of sample-by-sample calls.

```owl
batch is a Tensor = Tensor([[1.0, 0.5, -1.0, 2.0], [0.0, 1.0, 1.0, -1.0]])
outputs is a Tensor = model.Infer(batch)      // 2 output rows
```

`.Infer` is a **pure** computation: two calls with the same input return
exactly the same output. Weights only change during training.

---

## 4. Training

Training adjusts the weights so that predictions move closer to the expected
targets. It is driven by a `TrainingOptions` object, then a single call
`.Train(inputs, targets, options)`.

The **inputs** and the **targets** are two aligned 2D `Tensor`:
`N` samples in rows. Each target row is the expected output for the input
row of the same rank — one-hot encoded for classification, raw value for
regression.

`.Train` **modifies the model in place**: the learned weights stay in the
model, there is no value to reassign.

```owl
inputs is a Tensor = Tensor([[1.0, 0.5, -1.0, 2.0], [0.0, 1.0, 1.0, -1.0]])
targets is a Tensor = Tensor([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])

options is a TrainingOptions
options..Epochs       = 50
options..BatchSize    = 32
options..LearningRate = 0.01
options..Optimizer    = optimAdam
options..Loss         = lossCrossEntropy

model.Train(inputs, targets, options)
```

### TrainingOptions settings

The default values allow a first training run without tuning everything.

| Setting | Role | Default |
|---|---|---|
| `..Epochs` / `..Époques` | Number of complete passes over the data | `1` |
| `..BatchSize` / `..TailleLot` | Mini-batch size | `32` |
| `..LearningRate` / `..TauxApprentissage` | Weight update step | `0.01` |
| `..Optimizer` / `..Optimiseur` | Optimization strategy | `optimSGD` |
| `..Loss` / `..Perte` | Loss function | `lossCrossEntropy` |
| `..OnEpoch` / `..SurÉpoque` | Tracking procedure called at each epoch | *(none)* |

**Optimizers** determine how the weights step back at each mini-batch:

| Optimizer | Behaviour |
|---|---|
| `optimSGD` | Stochastic gradient descent: fixed step proportional to the gradient |
| `optimAdam` | Per-weight adapted step; often converges faster without fine-tuning the rate |

**Loss functions** measure the gap between output and target:

| Loss | Use |
|---|---|
| `lossCrossEntropy` / `perteEntropieCroisée` | Multi-class classification (one-hot target) |
| `lossQuadraticError` / `perteErreurQuadratique` | Regression (continuous value) |

### Tracking progress

`..OnEpoch` receives a **procedure** called at the end of each epoch, with
the current epoch number, the total number of epochs and the average loss of
the epoch just finished. Enough to feed a gauge or a trace without unrolling
the loop by hand. With no assignment, training calls nothing back.

```owl
PROCEDURE TrackEpoch(number is an integer, total is an integer, loss is a real)
    Trace("Epoch " + number + " / " + total + " — loss " + loss)
END

options..OnEpoch = TrackEpoch      // the bare name, no parentheses
```

### Transfer learning

Retraining a network from scratch is expensive in data and time. When a
model has already learned useful representations on a neighbouring task, it
is cheaper to **reuse its body** and only relearn the deciding part — the
**head**. That is transfer learning: keep the feature-extracting layers,
replace the original head with one adapted to your own categories, then tune
only the latter.

Four moves suffice, in this order.

**Freeze the body.** `.Freeze()` / `.Gèle()` marks as non-trainable **all
the layers present** at the time of the call: inference traverses them
normally, but training skips their update. Their weights therefore stay
strictly unchanged. `.Unfreeze()` / `.Dégèle()` re-enables training on all
layers. Freezing concerns training, not structure.

**Remove the original head.** `.RemoveLastLayer()` / `.RetireDernièreCouche()`
removes the **last** stacked layer: `..LayerCount` drops by one, and
inference now produces the output of the preceding layer. A repeated call
removes layers one by one, in reverse order of addition. Removing the layer
of an empty model, or removing a branch-block marker (cutting in the middle
of a block would be ambiguous), raises an error and leaves the model intact.

**Graft a new head.** Any layer added **after** the freeze is trainable —
the freeze applies only to the already-present layers. A new dense layer
sized on your own categories thus becomes the head to learn.

**Tune.** A `.Train` call now changes only the new head: the frozen body
stays bit-for-bit what it was. Training converges faster than a full run,
since it adjusts only a fraction of the weights.

```owl
// A pre-trained body and its original 5-category head.
model is an AIModel
model.AddDenseLayer(4, 8)
model.AddReLULayer()
model.AddDenseLayer(8, 5)      // original head

model.Freeze()                 // freeze body + original head

model.RemoveLastLayer()        // remove the original head
model.AddDenseLayer(8, 2)      // new head: 2 categories, trainable

// Fine-tune: only the frozen body stays unchanged, the new head learns.
model.Train(inputs, targets, options)
```

The frozen body acts as a fixed feature extractor; only the head grafted
after the freeze adapts to the new data. `.RemoveLastLayer` and `.Freeze`
are also useful outside transfer: the first to fix a badly built stack, the
second to freeze part of a network during a training phase.

---

## 5. Detecting objects

Some tasks do not ask *which* category an image represents, but *which
objects* are in it and *where*. A detection model imported in ONNX format
(`.LoadONNX`, see [section 9](#9-out-of-current-scope) for the import scope)
locates several objects in an image in a single move: `.Detect`.

Where `.Infer` returns a raw output tensor, `.Detect` chains the whole
pipeline of a detector: resize the image to the model input, infer, decode
the predictions, discard those below a confidence threshold, suppress
redundant overlapping boxes (non-maximum suppression, per class), then
rescale the boxes back to the original image. The result is a list of
directly usable objects.

### `.Detect`

`.Detect(image, scoreThreshold, iouThreshold)` takes an `Image` and two
thresholds, and returns an **array of `Detection`** sorted by decreasing
score.

| Argument | Role |
|---|---|
| `image` | The `Image` to analyze (loaded by `dLoadImage`). It is resized to the model input internally; the returned boxes stay expressed in original pixels. |
| `scoreThreshold` | Minimum confidence, in `[0, 1]`: any detection below is discarded. The higher it is, the fewer boxes, but the more certain. |
| `iouThreshold` | Overlap threshold for duplicate suppression: two boxes of the same class whose overlap exceeds this threshold are reduced to the best-scored one. |

| Fluent form | Prefixed form |
|---|---|
| `model.Detect(image, scoreThreshold, iouThreshold)` / `.Détecte` | *(method form only)* |

### The `Detection` type

Each element of the returned array is a `Detection`, read with the double
dot.

| Property | Returns |
|---|---|
| `..Box` / `..Boîte` | Array of 4 reals `[x, y, width, height]`, in original image pixels: top-left corner, then size. |
| `..Class` / `..Classe` | Index of the predicted class — a **0-based** integer. |
| `..Score` / `..Score` | Detection confidence, a real in `[0, 1]`. |

The class is an **index**, not a label: the model only knows numbers.
Mapping each index to a readable name is up to the caller — a plain array of
names is enough. That index is 0-based while an OWL array is 1-based, hence
the access `names[class + 1]`.

### End-to-end example

Load an image and a detection model, detect, then walk through the objects
found:

```owl
names is an array of string = ["person", "bike", "car", "bus", "dog"]

image is an Image = dLoadImage("photo.jpg")

model is an AIModel
model.LoadONNX("detector.onnx")

detections is an array of Detection = model.Detect(image, 0.25, 0.45)
Trace(Length(detections) + " object(s) found")

FOR EACH d OF detections
    className is a string = names[d..Class + 1]
    posX is a real = d..Box[1]
    posY is a real = d..Box[2]
    Trace(className + " (" + Round(d..Score, 2) + ") at [" + posX + ", " + posY + "]")
END
```

Drawing the boxes onto the image is then done with the drawing primitives
`dRect` (the outline) and `dText` (the label). A complete annotated example —
loading the model, drawing the boxes, saving the image — ships with the
`detection_yolo` demo folder.

### Primitives: `NonMaxSuppression` and `IoU`

`.Detect` already applies all the filtering. Two natives expose the
underlying bricks for a custom pipeline — filtering the hand-decoded outputs
of a model, or merging detections from several sources.

**`IoU(boxA, boxB)`** / `IoU` measures the overlap of two boxes, each given
by an array of 4 reals `[x, y, width, height]`. The result is a real in
`[0, 1]`: `0` when the boxes are disjoint, `1` when they coincide, an
intermediate value according to the shared area relative to the total area.

**`NonMaxSuppression(boxes, scores, iouThreshold)`** / `SuppressionNonMax`
removes overlapping duplicates. It walks the boxes from best-scored to
least-scored and keeps a box as long as its overlap (IoU) with each
already-kept box stays below `iouThreshold`. It returns an array of the
(1-based) **indices** of the kept boxes, sorted by decreasing score; an empty
list returns an empty array, a single box is always kept.

```owl
boxes is an array of array = [[10.0, 10.0, 50.0, 50.0], [12.0, 12.0, 48.0, 48.0], [200.0, 80.0, 40.0, 90.0]]
scores is an array of real = [0.9, 0.75, 0.6]

kept is an array of integer = NonMaxSuppression(boxes, scores, 0.5)
// kept is [1, 3]: the 2nd box, nearly identical to the 1st, is dropped.

overlap is a real = IoU(boxes[1], boxes[2])   // close to 1: strong overlap
```

---

## 6. Saving and loading

A trained model is **serialized to JSON** on disk, then reloaded
identically — without retraining. Reals are written at full precision: a
model saved then reloaded gives back exactly the same weights, hence the
same `.Infer` output.

```owl
model.Save("model.json")
```

`.Load` fills a model **of the same architecture** with the read weights.
The receiving model is first built with the same layers, then receives the
weights from the file.

```owl
fresh is an AIModel
fresh.AddDenseLayer(4, 8)
fresh.AddReLULayer()
fresh.AddDenseLayer(8, 3)
fresh.Load("model.json")
```

| Fluent form | Prefixed form | Role |
|---|---|---|
| `model.Save(path)` / `.Sauve` | `AIModelSave(model, path)` / `IAModèleSauve` | Writes the model as JSON |
| `model.Load(path)` / `.Charge` | `AIModelLoad(model, path)` / `IAModèleCharge` | Reloads the weights into a model |

Both forms are equivalent. They return `True` on success, `False` with
`ErrorOccurred` set if the path is invalid or the file unreadable.

---

## 7. Introspection

Three accessors expose the internal structure, useful to check a model or
reproduce a computation by hand.

| Accessor | Returns |
|---|---|
| `model..LayerCount` / `..NbCouches` | Total number of stacked layers (dense + activations) |
| `model.Weights(n)` / `.Poids(n)` | 2D `Tensor` of the weights of the n-th **dense** layer (1-based) |
| `model.Bias(n)` / `.Biais(n)` | 1D `Tensor` of the biases of the n-th **dense** layer (1-based) |

The index `n` of `.Weights` / `.Bias` counts only the **dense** layers,
ignoring the activations: `.Weights(1)` denotes the first dense layer,
whatever its position in the stack.

```owl
weights is a Tensor = model.Weights(1)
bias is a Tensor = model.Bias(1)
```

---

## 8. Capability matrix

`AIModel` is a foundation that **grows in stages**. This table sets the
public contract: what works today, and what is coming. Each stage adds its
up-to-date row; no date is promised.

| Capability | Dense networks (today) | Vision / classification | Language models |
|---|---|---|---|
| Build the layers | yes | yes | no (external model) |
| Train from data | yes | coming | no |
| Infer | yes | yes (imported model) | yes (text generation) |
| Save / load (OWL format) | yes | coming | — |
| Load an external model | no | yes (ONNX) | yes (GGUF file) |
| Tune a loaded model | — | coming | no |

Today, the **dense networks** column is complete: building, training,
inferring, saving and loading work end to end. The **language models** column
is covered by a distinct type, `LLM`, which loads a language model in GGUF
format and generates text — see the dedicated page
[`35_llm.md`](35_llm.md). The **vision / classification** column advances on
two fronts. On the **import** side, a pre-trained convolutional network loads
from an ONNX file (`.LoadONNX`) and is queried right away with `.Infer` or,
for object localization, with `.Detect`
([section 5](#5-detecting-objects)). On the **native training** side, the
convolutional layers (2D convolution, pooling, batch normalization,
flattening) ship as bricks; assembling an image classifier trained end to
end, its dedicated page, and persisting convolutional layers in OWL format
make up the stage in progress.

### Performance

The computation runs on the CPU, with no dedicated hardware acceleration.
Modest-sized dense networks train in a few seconds; a larger network stays
slower than an accelerated engine — a real constraint to factor into the
sizing of a task.

---

## 9. Out of current scope

These uses are **not yet supported**. They are named here so that an
uncovered need is understood as a stage to come, not as a defect:

- **End-to-end image classifier** — the convolutional layers (2D
  convolution, pooling, batch normalization, flattening) are now available
  as bricks; on the other hand the complete assembly of a trained, documented
  image classifier, and the persistence (`Save`/`Load`) of convolutional
  layers, remain a stage in progress.
- **Training from an imported model** — a network loaded from an ONNX file
  (`.LoadONNX`, [section 5](#5-detecting-objects)) serves to infer and
  detect, but its native training or tuning is not supported: it is imported
  to be used, not extended. Persistence in OWL format (`Save`/`Load`,
  [section 6](#6-saving-and-loading)) remains reserved for models built in
  the language.
- **Language models inside `AIModel`** — `AIModel` neither builds nor trains
  a large language model. Loading and text generation by a language model do
  exist, but through a dedicated type, `LLM` (page
  [`35_llm.md`](35_llm.md)), and not through `AIModel`.

A dense model remains, however, **complete for what it does**: build, train
on real data, infer and persist — the full cycle, illustrated at the
[end of this page](#going-further).

---

## Going further

A complete case study: a dense network that learns to read handwritten
digits (28 × 28 images). On a reduced set, it trains in a few seconds for
about 90 % accuracy; on the full set, that same network reaches about
96.8 %. The **build → train → infer** cycle is written in a handful of
lines.

| Topic | Page |
|---|---|
| Load a language model and generate text (`LLM`) | [`35_llm.md`](35_llm.md) |
| The `Tensor` / `Vector` numeric foundation | [`33_vecteurs.md`](33_vecteurs.md) |
| Number types and coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Exception handling (`WHEN EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
| Wire an AI agent onto the language | [`25_agents_ia.md`](25_agents_ia.md) |
