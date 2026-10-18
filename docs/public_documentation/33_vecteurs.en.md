# Vectors: the foundation of numeric computing

## 1. The vector, in brief

A `Vector` is a **sequence of reals** laid out side by side, built for
computation: geometry, statistics, numeric data processing. Each element
is a `real` (double-precision float); storage is **contiguous** and the
size is **fixed at construction**.

A `Vector` is a **full-fledged type**, distinct from the `array of
reals`. Both are declared with brackets and indexed from base 1, but they
do not mix: the array is a versatile ordered collection; the vector is a
numeric object that knows how to add itself, normalize itself, measure
its norm. This chapter describes this computation surface and the
**numeric contract** that governs it (see
[section 7](#7-the-numeric-contract)).

```owl
v is a Vector = [1.0, 2.0, 3.0]
Trace(v..Length)          // 3
Trace(v[1])               // 1
```

---

## 2. Build and access

A `Vector` is built from a **literal**: a sequence of values between
brackets, which immediately fixes its size. An integer element is
**coerced to real** at construction, exactly like a `real` variable
assigned from an integer.

```owl
v is a Vector = [1.0, 2.0, 3.0]
vInts is a Vector = [1, 2, 3]              // each integer coerced to real
r is a real = vInts[1]                     // 1.0
```

**Indexing** `v[i]` reads and writes an element, in **base 1**. Writing
at a position does not touch the others.

```owl
v is a Vector = [10.0, 20.0, 30.0]
v[2] = 99.0
Trace(v[1])               // 10   — intact
Trace(v[2])               // 99   — written
```

An **out-of-bounds** index raises a recoverable exception, never a silent
read of a neighboring value:

```owl
v is a Vector = [1.0, 2.0, 3.0]
WHEN EXCEPTION IN
    x is a real = v[10]
DO
    Trace("index out of bounds")
END
```

Two structural attributes are read with the double dot `..`, in O(1):

| Property | Role |
|---|---|
| `v..Length` / `v..Taille` | Number of elements |
| `v..Empty` / `v..Vide` | `True` if the vector contains no element |

```owl
v is a Vector = [1.0, 2.0, 3.0]
Trace(v..Length)          // 3
Trace(v..Empty)           // False

empty is a Vector = []
Trace(empty..Length)      // 0
Trace(empty..Empty)       // True
```

---

## 3. Convert to and from an array

The passage between `array of reals` and `Vector` is **always explicit**:
no implicit conversion happens mid-expression (see
[section 7](#7-the-numeric-contract)). Two symmetric natives handle the
round trip.

| Native | Role |
|---|---|
| `ArrayToVector(array)` / `TableauVersVecteur` | Array of reals → Vector of the same size |
| `VectorToArray(vector)` / `VecteurVersTableau` | Vector → array of reals of the same size |

```owl
t is an array of reals = [1.0, 2.0, 3.0]
v is a Vector = ArrayToVector(t)
Trace(v..Length)          // 3

output is an array of reals = VectorToArray(v)
Trace(output[1])          // 1
```

The coercion `[...]` → `Vector` operates only **at the declaration** of a
variable typed `Vector`, where the target is explicit. An already-built
array goes through `ArrayToVector` — no shortcut.

---

## 4. Arithmetic

The operators `+ - * /` apply to **two vectors of the same size** and
operate **element by element**. The result is a **new vector**: the
operands are never modified (immutability).

```owl
v1 is a Vector = [1.0, 2.0, 3.0]
v2 is a Vector = [4.0, 5.0, 6.0]

sum is a Vector = v1 + v2
Trace(sum[1])             // 5    — 1.0 + 4.0
Trace(v1[1])              // 1    — v1 untouched
```

Combined with a **real scalar**, the operator spreads the operation over
each element (broadcast). `+` and `*` are commutative with the scalar.

```owl
v is a Vector = [1.0, 2.0, 3.0]
Trace((v * 2.0)[2])       // 4
Trace((2.0 * v)[2])       // 4    — commutative
Trace((v / 2.0)[3])       // 1.5
```

The **unary negation** `-v` flips the sign of each element:

```owl
v is a Vector = [1.0, -2.0, 3.0]
Trace((-v)[1])            // -1
Trace((-v)[2])            // 2
```

Each operator has an equivalent **fluent method** — `.Add` / `.Subtract`
/ `.Multiply` / `.Divide` (French forms `.Ajoute` / `.Soustrait` /
`.Multiplie` / `.Divise`). They produce the same new vector, also without
mutating the operand.

```owl
v1 is a Vector = [1.0, 2.0, 3.0]
v2 is a Vector = [4.0, 5.0, 6.0]
Trace(v1.Add(v2)[1])          // 5    — same as (v1 + v2)[1]
Trace(v1.Multiply(v2)[3])     // 18
```

Two vectors of **different sizes** in a binary operation raise an
exception.

---

## 5. Vector computations

Linear-algebra operations come in **two forms** to choose from, for the
same computation:

| Form | Example | When to prefer it |
|---|---|---|
| **Prefixed native** | `VectorMagnitude(v)` | Standalone call, spotted by its prefix |
| **Fluent method** | `v.Magnitude()` | Chaining from a vector |

The parentheses signal a **computation** (as opposed to the `..` of a
structural attribute, [section 2](#2-build-and-access)).

| Computation | Native / method | Result |
|---|---|---|
| Dot product | `VectorDotProduct(v1, v2)` / `v1.DotProduct(v2)` | real |
| Euclidean norm | `VectorMagnitude(v)` / `v.Magnitude()` | real |
| Normalization | `VectorNormalize(v)` / `v.Normalize()` | Vector of norm 1 |
| Euclidean distance | `VectorDistance(v1, v2)` / `v1.Distance(v2)` | real |
| Cosine similarity | `VectorCosineSimilarity(v1, v2)` / `v1.CosineSimilarity(v2)` | real between -1 and 1 |

French forms: `VecteurProduitScalaire`, `VecteurMagnitude`,
`VecteurNormalise`, `VecteurDistance`, `VecteurSimilaritéCosinus`.

```owl
p is a Vector = [3.0, 4.0]
Trace(VectorMagnitude(p))             // 5    — √(3² + 4²)
Trace(p.Magnitude())                  // 5    — same computation, fluent form

q is a Vector = [1.0, 0.0]
Trace(p.DotProduct(q))                // 3

u is a Vector = [3.0, 0.0]
Trace(VectorNormalize(u)[1])          // 1    — direction kept, norm 1

origin is a Vector = [0.0, 0.0]
Trace(VectorDistance(origin, p))      // 5
```

The computations that divide by the norm — `Normalize`,
`CosineSimilarity` — raise an exception on a **null vector**
(magnitude 0), which has no defined direction. Binary operations require
two vectors of the **same size**.

---

## 6. Reductions

A **reduction** condenses a vector into a single value. They are
expressed as fluent methods.

| Reduction | Role | Result |
|---|---|---|
| `v.Sum()` / `v.Somme()` | Sum of the elements | real |
| `v.Average()` / `v.Moyenne()` | Arithmetic mean | real |
| `v.Min()` | Smallest value | real |
| `v.Max()` | Largest value | real |
| `v.ArgMax()` | **Index** (base 1) of the maximum | integer |

```owl
v is a Vector = [3.0, 1.0, 2.0]
Trace(v.Sum())            // 6
Trace(v.Average())        // 2
Trace(v.Min())            // 1
Trace(v.Max())            // 3
Trace(v.ArgMax())         // 1    — the index of 3.0, not the value
```

`ArgMax` returns the **index** of the maximum, not its value, and the
**first** position in case of a tie.

On an **empty vector**, the reductions behave as follows:

- `Sum()` and `Average()` return **0**;
- `Min()`, `Max()` and `ArgMax()` **raise an exception**: the result has
  no defined value, no arbitrary value is invented.

```owl
empty is a Vector = []
Trace(empty.Sum())        // 0
WHEN EXCEPTION IN
    m is a real = empty.Min()
DO
    Trace("Min undefined on an empty vector")
END
```

---

## 7. The numeric contract

### An element is an IEEE `real`

Each element of a `Vector` behaves **exactly** like a scalar `real`:
double-precision float, with the normal imprecision of floating-point
computation. The `=` comparison is strict; an irrational result (a
`Magnitude`, a `Normalize`) is compared **with tolerance**, never by
exact equality.

An assumed difference from raw floating point: **dividing by zero raises
a recoverable exception**, rather than producing `Inf` or `NaN`. The
`Vector` walls off division by zero just as the scalar `real` already
does.

```owl
v is a Vector = [4.0, 5.0, 6.0]
withZero is a Vector = [2.0, 0.0, 3.0]
WHEN EXCEPTION IN
    res is a Vector = v / withZero
DO
    Trace("division by a null element")
END
```

### Choosing the right number type

The `Vector` is made for **geometry, statistics and numeric
computation**, where IEEE floating point is the right tool and where its
imprecision is normal and expected.

For an entirely different need — **money** or **exact counting**, where
the slightest rounding error is unacceptable — the `currency` and
`numeric` types (exact decimals) are the fit, outside the `Vector`. Each
domain its type: IEEE for computation, exact decimal for the accounting
value. The detail of number types lives in
[`01_types_et_coercions.md`](01_types_et_coercions.md).

### No `array` ↔ `Vector` mixing in an expression

An `array` operand — typed variable or bare literal — is **refused** in
the arithmetic and operations of a `Vector`. Accepting `v + t` or
`v + [1.0, 2.0]` mid-expression would mean **guessing** whether one is
handling an array or a vector; OWL does not guess types.

```owl
v is a Vector = [1.0, 2.0, 3.0]
t is an array of reals = [1.0, 2.0, 3.0]

// v + t          → exception: array operand refused
sum is a Vector = v + ArrayToVector(t)   // explicit conversion
```

Only `Vector op Vector` and `Vector op scalar` are legal. To combine an
array, convert it explicitly
([section 3](#3-convert-to-and-from-an-array)) or declare it as a
`Vector` from the start.

---

## Further reading

| Topic | Sheet |
|---|---|
| Number types, `real` / `currency` / `numeric`, coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Operators and their semantics | [`05_operateurs.md`](05_operateurs.md) |
| Exception handling (`WHEN EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
| Coding conventions | [`10_conventions_de_code.md`](10_conventions_de_code.md) |
