# Low-level training, evaluation, and detection

## 1. In brief

The [`AIModel`](34_iamodele.md) type offers turnkey training
(`.Train` with optimizer, rate scheduling, validation and early
stopping). This sheet goes one level down: it shows how to **write
the training loop yourself** from tensors and autograd
([`37_tenseurs_et_donnees.md`](37_tenseurs_et_donnees.md)), how to
**measure** a model's quality, and how to assemble an **end-to-end
trainable object detection** — the task `AIModel` can infer, here
learned from scratch.

Three building blocks structure the discussion: the explicit learning
loop, the evaluation metrics, then the complete detection pipeline.

---

## 2. An explicit training loop

Writing the loop by hand gives full control over what is learned. The
pattern is always the same at each step: **predict**, **measure the
loss**, **backpropagate**, **step the parameters**. The example below
learns a linear relation `y = 2x` through a single scalar weight,
watching the loss melt away.

```owl
// 1D linear regression by explicit autograd: learn y = 2x.
InitRandom(3)
inputs is a Tensor = Tensor([1.0, 2.0, 3.0, 4.0])
targets is a Tensor = Tensor([2.0, 4.0, 6.0, 8.0])
w is a Tensor = Tensor([0.0])
rate is a real = 0.05
loss0 is a real = 0.0
lossF is a real = 0.0
FOR ep = 1 TO 200
	pred is a Tensor = inputs * w              // broadcasting: w applied to each input
	loss is a Tensor = pred.MeanSquaredError(targets)
	loss.Backpropagate()
	IF ep = 1 THEN
		loss0 = loss..Value[1]
	END
	lossF = loss..Value[1]
	w      = w.GradientDescent(rate)
END
Trace("initial loss " + Round(loss0, 4))       // 30
Trace("final loss " + Round(lossF, 6))         // 0
Trace("learned w " + Round(w..Value[1], 3))    // 2
```

Each iteration reassigns `w` with the leaf returned by
`.GradientDescent`: the new tensor starts over with a zero gradient,
detached from the previous graph, ready for the next step. The same
mechanics extend to a whole network — several weight tensors, a
forward pass composed of convolutions and activations — as in the
detection in [section 4](#4-end-to-end-trainable-detection).

---

## 3. Evaluating a model

Once a model is trained, several **metrics** judge its quality. All
are a simple count, without gradient: these are evaluation measures,
not losses. Predictions are either a 2D `Tensor` `batch × classes`
(the chosen class per row being the one with the highest score), or a
1D `Tensor` of indices; targets are a 1D `Tensor` of expected indices,
**0-based**.

| Metric | Native | Returns |
|---|---|---|
| Accuracy | `Accuracy(pred, targets)` | Fraction of correctly classified examples, in `[0, 1]` |
| Confusion matrix | `ConfusionMatrix(pred, targets)` | `Matrix` `classes × classes`: actual in rows, predicted in columns |
| Per-class precision | `Precision(pred, targets)` | `Vector` — true positives / predicted positives |
| Per-class recall | `Recall(pred, targets)` | `Vector` — true positives / actual positives |
| Per-class F1 score | `F1(pred, targets)` | `Vector` — harmonic mean of precision and recall |

```owl
// 4 examples, 3 classes. 2D scores (logits) batch x classes.
scores is a Tensor = Tensor([3.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 3.0]).Reshape([4, 3])
targets is a Tensor = Tensor([0.0, 1.0, 2.0, 2.0])
Trace("accuracy " + Round(Accuracy(scores, targets), 3))            // 0.75
conf is a Matrix = ConfusionMatrix(scores, targets)
Trace("conf[3][2] (actual 2 predicted 1) " + conf[3][2])            // 1
precisions is a Vector = Precision(scores, targets)
recalls is a Vector = Recall(scores, targets)
scoresF1 is a Vector = F1(scores, targets)
Trace("precision class 0 " + Round(precisions[1], 3))               // 1
Trace("macro recall " + Round(recalls.Average(), 3))                // 0.833
Trace("macro F1 " + Round(scoresF1.Average(), 3))                   // 0.778
```

`Precision`, `Recall` and `F1` return a `Vector` indexed by class
(1-based position = 0-based class + 1). The **macro average** —
unweighted mean over classes — is obtained with a `.Average()` on that
vector. A class with no instance is `0.0`, never an undefined value.
The `ConfusionMatrix` reads in 1-based position: cell `[i][j]` counts
examples of actual class `i - 1` predicted as class `j - 1`.

---

## 4. End-to-end trainable detection

Detecting objects means finding **where** they are and **which
class**. A grid detector divides the image into cells and has each one
predict a box, an **object presence** (objectness) and a class.
Training it requires three pieces: build the per-cell targets, compose
a loss that only penalizes the right cells, then measure quality by
mean average precision (mAP).

### Building the per-cell targets

`AssignDetectionTargets(boxes, gridHeight, gridWidth, numClasses)`
turns annotated boxes into a `Tensor` of targets `[H×W, 4 + 1 + K]`,
laid out in row-major order. Each box is assigned to the **responsible
cell** — the one that contains its center. The center is stored there
in **cell-relative** coordinates, which makes the position learnable
by a shared-weight head; width and height stay as a fraction of the
whole image.

```owl
// One box class 0, center (0.7,0.3), size 0.4x0.3, on a 2x2 grid, K=1 class.
boxes is an array = [[0.0, 0.7, 0.3, 0.4, 0.3]]
target is a Tensor = AssignDetectionTargets(boxes, 2, 2, 1)
// responsible cell: col=floor(0.7*2)=1, row=floor(0.3*2)=0
// index = row*W+col = 0*2+1 = 1 (0-based) -> row 2 (1-based)
obj is a Tensor = target.ChannelSlice(5, 1)
Trace(obj..Value[2][1])         // 1    — responsible cell
Trace(obj..Value[1][1])         // 0    — background cell
targetBox is a Tensor = target.ChannelSlice(1, 4)
Trace(Round(targetBox..Value[2][1], 3))       // 0.4   — relative cx: 0.7*2 - 1
Trace(Round(targetBox..Value[2][2], 3))       // 0.6   — relative cy: 0.3*2 - 0
```

The objectness channel (channel 5) also serves as a **mask**: it is 1
on the responsible cell, 0 everywhere else.

### Composing a masked loss

The loss adds two terms: objectness over **all** cells (binary
cross-entropy) and the box over the **single** responsible cell (L1).
The mask, broadcast over the four box channels
([broadcasting](37_tenseurs_et_donnees.md#la-diffusion-broadcasting)),
cancels the contribution of background cells.

```owl
boxes is an array = [[0.0, 0.7, 0.3, 0.4, 0.3]]
target is a Tensor = AssignDetectionTargets(boxes, 2, 2, 1)
targetBox is a Tensor = target.ChannelSlice(1, 4)
targetObj is a Tensor = target.ChannelSlice(5, 1)
mask is a Tensor = target.ChannelSlice(5, 1)

// Dummy prediction at zero (before any learning).
predObj is a Tensor = Tensor([0.0, 0.0, 0.0, 0.0]).Reshape([4, 1])
predBox is a Tensor = Tensor([0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]).Reshape([4, 4])

loss is a Tensor = predObj.BinaryCrossEntropy(targetObj) + (predBox * mask).L1(targetBox)
loss.Backpropagate()
Trace("loss " + Round(loss..Value[1], 4))       // 0.7994
```

### Measuring by mean average precision (mAP)

`MeanAveragePrecision(detections, truths, iouThreshold)` computes the
mean average precision of a detector. Detections are `[class, score,
x, y, width, height]`, truths are `[class, x, y, width, height]`,
corner boxes in the same space. Each detection is greedily matched to
the best free truth whose overlap (IoU) reaches the threshold.

```owl
detections is an array = [[0.0, 0.9, 0.10, 0.10, 0.20, 0.20], [0.0, 0.3, 0.60, 0.60, 0.20, 0.20]]
truths is an array = [[0.0, 0.11, 0.11, 0.20, 0.20]]
Trace("mAP " + Round(MeanAveragePrecision(detections, truths, 0.5), 3))       // 1

overlap is a real = IoU([10.0, 10.0, 50.0, 50.0], [10.0, 10.0, 50.0, 50.0])
Trace("IoU " + overlap)          // 1    — identical boxes
kept is an array = NonMaxSuppression([[10.0, 10.0, 50.0, 50.0], [12.0, 12.0, 48.0, 48.0]], [0.9, 0.7], 0.5)
Trace("NMS kept " + Length(kept))       // 1    — the 2nd box, nearly coincident, is discarded
```

`IoU` and `NonMaxSuppression` are the reusable building blocks of
detection filtering (see also [`34_iamodele.md`](34_iamodele.md#5-détecter-des-objets)
for inference with an imported detector).

### A complete example

An **end-to-end** trained detector — generating a synthetic dataset, a
two-convolution forward pass, a training loop, mAP climbing over the
epochs, then rendering the predicted boxes — accompanies the
`detection_entrainement` demonstration folder. It requires no external
file and finishes in a few seconds.

---

## 5. The goal: train elsewhere, run here

The language's machine learning follows a clear line: **import and
run** real models locally, and **train light** on CPU. A heavy model
is trained where the power is (dedicated hardware), then imported in
ONNX format to run without external dependency
([`34_iamodele.md`](34_iamodele.md)). On CPU, the language covers
training modest-sized networks, transfer learning (reuse a frozen
body, retrain a head), and the light detection shown here.

The computation runs on CPU, without dedicated hardware acceleration:
a small network trains in a few seconds; a large network stays slower
than an accelerated engine — a constraint to factor into sizing a
task.

---

## 6. Current out-of-scope

These uses are **not yet covered**. They are named so that an
unhandled need is understood as an upcoming milestone, not a silent
defect:

- **Image segmentation** — delineating objects to the pixel is not
  supported; detection returns boxes, not masks.
- **Image generation** — the language trains and queries recognition
  networks, it does not synthesize images.
- **Training a language model** — loading and text generation exist
  through a dedicated type, `LLM` ([`35_llm.md`](35_llm.md)), but
  training such a model is out of scope.
- **Hardware acceleration (GPU)** — all computation runs on CPU.

---

## 7. French aliases

| EN canon | FR canon |
|---|---|
| `Accuracy` | `Exactitude` |
| `ConfusionMatrix` | `MatriceDeConfusion` |
| `Precision` / `Recall` / `F1` | `Précision` / `Rappel` / `F1` |
| `MeanAveragePrecision` | `PrécisionMoyenne` |
| `AssignDetectionTargets` | `AssocieCiblesDétection` |
| `IoU` | `IoU` |
| `NonMaxSuppression` | `SuppressionNonMax` |
| `.Backpropagate` / `.GradientDescent` | `.Rétropropage` / `.DescenteGradient` |
| `.Average` | `.Moyenne` |

---

## Further reading

| Topic | Sheet |
|---|---|
| The `Tensor` foundation, autograd, losses and data | [`37_tenseurs_et_donnees.md`](37_tenseurs_et_donnees.md) |
| Build, train and infer a network (`AIModel`) | [`34_iamodele.md`](34_iamodele.md) |
| Detect objects with an imported model | [`34_iamodele.md`](34_iamodele.md#5-détecter-des-objets) |
| Load a language model and generate text (`LLM`) | [`35_llm.md`](35_llm.md) |
| Exception handling (`WHEN EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
