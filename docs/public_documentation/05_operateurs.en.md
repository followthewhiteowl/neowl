# Operators (guide)

This page lists the OWL operators grouped by family, and ends with a
precedence table. All the examples are verified against the binary.

---

## 1. Arithmetic

| Operator | Effect | Example |
|---|---|---|
| `+` | Addition (or concatenation if one of the operands is a string, see section 4) | `7 + 3 → 10` |
| `-` | Subtraction, or unary | `7 - 3 → 4`, `-x` |
| `*` | Multiplication | `7 * 3 → 21` |
| `/` | Division (always real, never integer) | `7 / 3 → 2.333…` |
| `%` or `modulo` | Remainder of integer division | `7 % 3 → 1`, `7 modulo 3 → 1` |
| `^` | Power, left-associative | `2 ^ 3 ^ 2 → 64` (= `(2^3)^2`) |

**Automatic promotions**: an `integer` overflow promotes to `numeric`;
a mix with `real` produces a `real`. See `01_types_et_coercions.md`
for details.

**Unary minus and power convention**: `-2 ^ 2` is `4`, not `-4`.
Unary minus binds tighter than `^`, so the expression reads `(-2)^2`.
This is the spreadsheet convention (Excel gives the same result). It
differs from the handwritten mathematical convention and from several
programming languages where unary minus is weaker than power. When in
doubt, parenthesize.

---

## 2. Comparison

| Operator | Effect |
|---|---|
| `=` | Equality (in a test context, not assignment) |
| `<>` | Inequality |
| `<` | Strictly less than |
| `<=` | Less than or equal |
| `>` | Strictly greater than |
| `>=` | Greater than or equal |

`!=` is **not** recognized — use `<>`. The result of a comparison is a
boolean (`True`/`False`).

```owl
IF age >= 18 THEN …
IF status <> "active" THEN …
```

**List membership** — `IN` and `NOT IN`:

```owl
IF sStatus IN ("draft", "validated", "archived") THEN …
IF sCommand NOT IN ("SELECT", "INSERT", "UPDATE") THEN …
```

Sugar for a cascade of equalities. The list cannot be empty (compile
error). The two variants `IN` (full-eval) and `_IN_` (short-circuit)
follow the same doctrine as `AND` / `_AND_` — see §3.

---

## 3. Logical

| Operator | Effect | Evaluation |
|---|---|---|
| `AND` / `ET` | Full-eval conjunction | Both operands evaluated |
| `OR` / `OU` | Full-eval disjunction | Both operands evaluated |
| `NOT` / `PAS` | Unary negation | — |
| `_AND_` | Short-circuit conjunction | If left is false, right ignored |
| `_OR_` | Short-circuit disjunction | If left is true, right ignored |

The short-circuit versions (`_AND_`, `_OR_`) are essential when the
second operand may raise an error or be expensive:

```owl
IF IsNull(oAccount) _OR_ oAccount.balance > 0 THEN …
```

`AND` has higher priority than `OR`: `True OR False AND False`
evaluates as `True OR (False AND False) → True`.

### The `X` / `_X_` doctrine

OWL offers five operator pairs where the named form performs
**full-eval / re-evaluation** and the underscored form performs
**short-circuit / single evaluation**:

| Named form | `_X_` form | Context |
|---|---|---|
| `AND`, `OR` | `_AND_`, `_OR_` | Boolean expressions |
| `IN` | `_IN_` | Membership test (§2) |
| `TO` | `_TO_` | Bound in `FOR i = a TO b` (see `03_controle_de_flux.md` §2) |
| `STEP` | `_STEP_` | Step in `FOR ... STEP s` (see `03_controle_de_flux.md` §2) |

The distinction is **semantic**, not cosmetic: the two forms produce
different bytecode and can give different results as soon as an operand
carries a side effect, or a bound varies between two iterations. The
named form is the written form; switching to `_X_` is an explicit
choice to avoid re-evaluation.

For the types allowed in a condition (boolean, number, `Null`), see
`01_types_et_coercions.md` §2. A string or an array used as a condition
produces an explicit runtime error.

---

## 4. Strings

The `+` operator concatenates as soon as at least one operand is a
string. The result is always a string, even if the other operand is a
number:

```owl
Trace("score : " + 99)        // "score : 99"
Trace(5 + "x")                // "5x"
```

OWL provides three **fixed** operators dedicated to positional
comparison, available in three tolerance variants:

| Position | Strict | Case-insensitive | Case + accent insensitive |
|---|---|---|---|
| Starts with | `[=` | `[~` | `[~~` |
| Ends with | `=]` | `~]` | `~~]` |
| Contains | `[=]` | `[~]` | `[~~]` |

```owl
"alpha" [= "alp"      // True
"ALPHA" [~ "alp"      // True  (case ignored)
"héllo" [~ "hel"      // False  (accents still distinct)
"Héllo" [~~ "hello"   // True  (case + accents ignored)
"alpha" [=] "ph"      // True  (contains)
"alpha" =] "ha"       // True  (ends with)
```

For more complex comparisons (patterns, expressions), use the
`StartsWith`, `Position`, `Contains` natives, or regular expressions
via the regex module.

---

## 5. Assignment and compounds

`=` serves both as assignment and comparison; the syntactic context
decides. As a statement, it is an assignment; in a condition
expression, it is a comparison.

Compound operators:

| Operator | Equivalent |
|---|---|
| `+=` | `x = x + y` |
| `-=` | `x = x - y` |
| `*=` | `x = x * y` |
| `/=` | `x = x / y` |
| `%=` | `x = x modulo y` |

Increments/decrements, **postfix only**:

```owl
n is an integer = 5
n++           // n is 6
n--           // n is 5
```

`++n` (prefix) is not recognized. `n++` is used as a statement, not as
an expression — `m = n++` is not accepted.

---

## 6. Bitwise operations

OWL provides the bitwise operators in addition to the named functions.
Both forms are equivalent and interchangeable.

| Operator | Function | Effect |
|---|---|---|
| `a & b` | `BinaryAND(a, b)` | Bitwise AND |
| `a \| b` | `BinaryOR(a, b)` | Bitwise OR |
| `a \|\| b` | `BinaryXOR(a, b)` | Bitwise XOR |
| `~a` | `BinaryNOT(a)` | One's complement (unary) |
| `a bitShiftLeft n` | `bitShiftLeft(a, n)` | Left shift |
| `a bitShiftRight n` | `bitShiftRight(a, n)` | Right shift |

```owl
mask     is an integer = flags & 0x0F           // = BinaryAND(flags, 0x0F)
position is an integer = 1 bitShiftLeft 4       // = 16
inverse  is an integer = ~flags                 // one's complement
```

### Individual bit access

To read or modify a specific bit of an integer, OWL exposes four prefix
methods. The position is 1-based, least significant bit first (`pos=1`
→ weight 2⁰, `pos=4` → weight 2³). A position outside `[1..64]` returns
the value unchanged for the setters and `0` for `BitRead`. **Immutable**
style: no mutation of the variable, the method returns a new value.

| Method | FR alias | Effect |
|---|---|---|
| `n.BitRead(pos)` | `BitLit` | Returns `0` or `1` |
| `n.BitEnable(pos)` | `BitActive` | Returns `n` with the bit turned on |
| `n.BitDisable(pos)` | `BitDesactive` | Returns `n` with the bit turned off |
| `n.BitFlip(pos)` | `BitInverse` | Returns `n` with the bit toggled |

```owl
n is an integer = 10                  // 1010
Trace(n.BitRead(2))                   // 1
n = n.BitEnable(1)                    // 1011 = 11
n = n.BitDisable(4)                   // 0011 = 3
```

---

## 7. Precedence

From lowest to highest. A high-precedence operator binds tighter, so it
is evaluated first.

| Level | Operators | Associativity |
|---|---|---|
| Assignment | `=`, `+=`, `-=`, `*=`, `/=`, `%=` | Right |
| Logical OR | `OR`, `_OR_` | Left |
| Logical AND | `AND`, `_AND_` | Left |
| Bitwise OR | `\|` | Left |
| Bitwise XOR | `\|\|` | Left |
| Bitwise AND | `&` | Left |
| Equality / membership | `=`, `<>`, `IN`, `_IN_` | Left |
| Comparison | `<`, `<=`, `>`, `>=`, `[=`, `=]`, `[=]` (and flex variants) | Left |
| Bitwise shift | `bitShiftLeft`, `bitShiftRight` | Left |
| Addition | `+`, `-` | Left |
| Multiplication | `*`, `/`, `%`, `modulo` | Left |
| Power | `^` | Left |
| Unary | `-`, `NOT`, `~` | — |
| Call / access | `.`, `()`, `[]`, `[[ ]]` | Left |

When in doubt, parenthesize. The performance cost is zero, the clarity
gain is immediate.

---

## Further reading

| Topic | Page |
|---|---|
| Language overview | `00_apercu_du_langage.md` |
| Types and conversions | `01_types_et_coercions.md` |
| Null and nullable types | `02_null_et_nullable.md` |
| Control flow (`TO` / `_TO_`, `STEP` step) | `03_controle_de_flux.md` |
| Access by computed name (`{ name }`, `{ name }..prop`) | `31_indirection.md` |
| Code conventions | `10_conventions_de_code.md` |
