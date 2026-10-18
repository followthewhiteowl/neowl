# Types and conversions

OWL has **explicit typing at declaration** and **loose typing at
casting**. Every variable is declared with its type; but the actual
value it holds is converted at the moment an operation requires it, not
at assignment. This combination gives:

- safety at reading (you know what a variable is supposed to contain),
- flexibility at writing (no need for `StringToInteger(...)`
  everywhere).

---

## 1. Fundamental types

| Type | Description |
|---|---|
| `string` | UTF-8 text, size measured in bytes |
| `integer` | Signed 64-bit integer, promoted to `numeric` on overflow |
| `real` | Double-precision float (8 bytes) |
| `currency` | Exact decimal, 6 digits after the decimal point |
| `numeric` | Exact decimal with unlimited precision (grows with memory) |
| `boolean` | `True` or `False` |
| `date`, `time`, `datetime` | Civil calendar, time of day, combination |
| `duration` | Interval, in milliseconds |
| `buffer` | Sequence of raw bytes |
| `array of <T>` | Ordered collection, 1-based index |
| `Vector` | Contiguous sequence of reals for numeric computation (distinct from `array`) |
| `JSON` | Structured document with dot access |

OWL has **no** sized integer types: forms such as `integer on 2 bytes`,
`unsigned integer`, `real on 4 bytes` are **rejected at compilation**,
as is `byte` used as a declared type. The only integer type is
`integer` (64-bit, promoted to `numeric` on overflow).

A **byte** is therefore not a type: it is a **logical value 0–255**
carried by an `integer`. To store a sequence of raw bytes, use a
`buffer`; to read or write a specific byte, the `buffer[[i]]` operator
and the natives described in
[section 6](#6-working-at-the-byte-level).

---

## 2. Implicit conversions

OWL silently converts between compatible types, **at the moment the
operation takes place**, not at declaration.

### String and number

The `+` operator is **concatenation** as soon as at least one operand
is a string, whatever the order:

```owl
Trace("x" + 5)            // "x5"
Trace(5 + "x")            // "5x"
Trace("12" + 30)          // "1230"   — concat, not addition
```

**Numeric addition** requires two numeric operands. If the string
contains a valid number, it is parsed on use:

```owl
nResult is an integer = "10" + 5      // 15
                                       // OWL parses "10" → 10 in arithmetic context
                                       // (the LHS expects an integer)
```

### Booleans

In a boolean context (`IF`, `WHILE`), three types are accepted:
`boolean`, number (integer, real, currency, numeric) and `Null`. Any
other value — string, array, structure, instance — produces an explicit
runtime error.

Coercion rules:

- `0`, `0.0`, `False`, `Null` are falsy.
- Any other number is truthy (including negative).

```owl
IF 1 THEN …               // runs
IF 0 THEN …               // skips
IF 3.5 THEN …             // runs
IF Null THEN …            // skips

IF "abc" THEN …           // error: string in condition
IF t THEN …               // error: array in condition
```

To test a non-empty string or a non-empty array, be explicit:

```owl
IF Length(s) > 0 THEN …
IF t..Occurrence > 0 THEN …
```

### Numbers together

`integer`, `real`, `currency`, `numeric` add and multiply without
concern. Precision follows the widest operand. On loop bounds, a real
is truncated to the integer.

```owl
moPrice is a currency = 10
moPrice = moPrice + 0.5   // 10.5
```

---

## 3. When the conversion happens

For `integer`, `real` and `boolean` targets, OWL converts the value
**at assignment**. The variable then holds a value of the declared
type, verifiable via `TypeVar`.

```owl
nT is an integer = 3.7
Trace(nT, " type=", TypeVar(nT))      // 3  type=8 (integer) — truncated at init

n is an integer = "abc"
Trace(n)                               // 0  — parse failed, silent default
```

Conversion to `integer` truncates (does not round) a real. Conversion
to `integer` from an unparseable string returns `0` without error;
check the content beforehand if you want to detect the failure.

For other targets, the current behavior is to **keep the native type of
the value**, the conversion being performed on use by the operations
that need it (`Trace`, `Length`, arithmetic). This point is being
stabilized.

To force an explicit conversion, use the dedicated functions
(section 5).

---

## 4. Automatic promotions

A few promotions happen automatically, without a cast:

- **Integer overflow**: a computation on `integer` that exceeds 2⁶³−1
  promotes the result to `numeric` (arbitrary precision).
  ```owl
  nBig is an integer = 9223372036854775807
  nBig = nBig + 1
  Trace(TypeVar(nBig))          // 14 (numeric)
  ```
- **Mixed integer + real**: the result is `real`.
- **Mixed integer + currency**: the result is `currency`.

These promotions are safe: they never lose precision, and they are
**automatic**. They contrast with the controlled overflow of a value
into a range or a byte ([section 6](#6-working-at-the-byte-level)),
which is on the contrary **explicit and chosen**: without an overflow
mode, an out-of-range value raises an error; the fallback (cyclic,
clamp…) applies only if requested. Widening a number is safe and
silent; shrinking it into a byte is a deliberate act.

---

## 5. Explicit conversions

When you want an **unambiguous** conversion at the desired moment, OWL
provides dedicated natives:

| From → to | Function |
|---|---|
| String → integer | `StringToInteger(s)`, `Val(s)` |
| String → real | `ValR(s)`, `Val(s)` |
| String → numeric | `StringToNum(s)` |
| String → date / time | `StringToDate`, `StringToTime` |
| String → JSON | `StringToJSON(s)` |
| Integer / real / numeric → string | `NumToString(n)` |
| Date / time / datetime → string | `DateToString`, `TimeToString`, `DateTimeToString` |
| Numeric / real → integer | `IntegerPart(x)` (integer part, rounded toward `-∞`) |
| Buffer ↔ string | `StringToBuffer`, `BufferToString` |

For the up-to-date list: `owl explain` on each family.

The `string` ↔ bytes transition at input/output boundaries (files,
buffers, URLs) and the available encodings are detailed in
[`26_encodage_et_io.md`](26_encodage_et_io.md).

---

## 6. Working at the byte level

For binary formats, network protocols or encryption, OWL exposes bytes
directly: a `buffer` is a sequence of raw bytes, and each byte is an
integer value 0–255. This section covers byte-by-byte access; the
buffer as a binary container (scalar marshalling, pre-sizing, file I/O)
is covered in [`27_buffers.md`](27_buffers.md).

### Direct access: `buffer[[i]]`

The `buffer[[i]]` operator reads or writes **one byte** at position `i`
(1-based index). Reading returns an **integer** 0–255, without going
through text encoding; writing sets exactly one byte.

```owl
b is a buffer = "Hé"            // "é" takes 2 bytes in UTF-8
Trace(b[[1]])                    // 72  ('H')
Trace(b[[2]])                    // 195 (1st byte of 'é')
Trace(b[[3]])                    // 169 (2nd byte of 'é')

b[[1]] = 200                     // sets byte 200 at position 1
Trace(b[[1]])                    // 200
```

Writing is **strict**: a value outside `[0, 255]` raises an exception
(no silent truncation). An index outside the buffer raises a
recoverable error (`ErrorOccurred`).

```owl
b is a buffer = "ABC"
WHEN EXCEPTION IN
    b[[1]] = 300                 // outside [0,255] → exception
DO
    Trace("invalid byte value")
END
```

### Byte and range natives

Four natives complete the operator, each with a fluent form and a
bilingual alias:

| Native | Role |
|---|---|
| `IntegerInRange(v, min, max, mode)` | Constrains `v` to `[min, max]` inclusive |
| `IntegerToByte(v, mode)` | Special case: constrains `v` to `[0, 255]` |
| `BufferReadByte(buf, pos)` | Reads the byte at `pos` → integer 0–255 |
| `BufferWriteByte(buf, pos, v, mode)` | Writes byte `v` at `pos` |

```owl
nClamp is an integer = IntegerInRange(300, 0, 255, overflowClamp)
Trace(nClamp)                    // 255 (brought back to the upper bound)

nByte is an integer = (300).ToByte(overflowWrap)
Trace(nByte)                     // 44
```

### Overflow modes

A value outside the requested range must be handled explicitly. The
mode passed as the last argument decides the behavior; **without a
mode, the default raises an error**.

| Mode | Behavior out of range |
|---|---|
| `overflowError` | Default — raises an exception |
| `overflowWrap` | Modulo fallback: the value "wraps" within the range |
| `overflowClamp` | Brings to the nearest bound (clamp) |
| `overflowLowByte` | Keeps the low 8 bits (byte only) |

`overflowLowByte` is reserved for byte work: it is **rejected at
compilation** in `IntegerInRange` (arbitrary range).

```owl
// 8-bit counter that wraps back to zero after 255
nNext is an integer = IntegerToByte(255 + 1, overflowWrap)
Trace(nNext)                     // 0

// 16-bit counter, same principle over a wider range
nSixteen is an integer = IntegerInRange(70000, 0, 65535, overflowWrap)
Trace(nSixteen)                  // 4464

// low 8 bits of a negative value
nLow is an integer = IntegerToByte(-1, overflowLowByte)
Trace(nLow)                      // 255
```

### Bitwise operations

On integers treated as bytes, the bitwise operators apply directly:

| Operator | Effect |
|---|---|
| `&` | Binary AND |
| `\|` | Binary OR |
| `\|\|` | Exclusive OR (XOR) |
| `bitShiftLeft(x, n)` | Shifts `x` left by `n` bits |
| `bitShiftRight(x, n)` | Shifts `x` right by `n` bits |

```owl
Trace(0xF0 & 0x0F)               // 0   (no common bit)
Trace(0xF0 | 0x0F)               // 255 (0xFF)
Trace(0xFF || 0x0F)              // 240 (0xF0 — XOR)
Trace(bitShiftLeft(1, 4))        // 16
Trace(bitShiftRight(256, 4))     // 16
```

### Examples

Reading the header of a mini binary format `[version][length][data…]`:

```owl
packet is a buffer = "XXXXX"
packet[[1]] = 2                  // version
packet[[2]] = 3                  // data length
packet[[3]] = 170                // 0xAA
packet[[4]] = 187                // 0xBB
packet[[5]] = 204                // 0xCC

nVersion is an integer = packet[[1]]
nLength is an integer = packet[[2]]
Trace("version ", nVersion, ", ", nLength, " bytes of data")
```

Simple symmetric encryption: XOR applied twice with the same key
restores the original value.

```owl
nPlain is an integer = 0x41      // 'A'
nKey is an integer = 0x5A
nCipher is an integer = nPlain || nKey       // 27
nBack is an integer = nCipher || nKey        // 65 — back to 'A'
Trace(nPlain, " → ", nCipher, " → ", nBack)
```

---

## 7. Common pitfalls

- **Concat vs addition**: `"12" + 30 = "1230"`, not `42`. To add, use
  `Val("12") + 30` or assign to an integer-typed variable (which then
  parses the string).
- **Silent parse to integer**: `n is an integer = "abc"` sets `0`
  without error. Check the string beforehand if you want to detect the
  failure.
- **Real literal without a leading zero**: `.5` is **not** a valid real
  literal — it produces an unexpected value. Always write `0.5`.
- **Comparing an integer and a string**: `5 = "5"` returns `True`
  (coercion). To distinguish the type, also compare `TypeVar`.
- **Non-boolean condition**: `IF` and `WHILE` only accept boolean,
  number or `Null`. A string or an array passed as a condition produces
  an explicit runtime error; be explicit with `Length(s) > 0` or
  `t..Occurrence > 0`.
- **Operators `*`, `-`, `/`, `modulo`, `^` with a string**: explicit
  runtime rejection (the operands must be numbers). Implicit
  string→number coercion only operates at assignment to a
  numeric-typed target (see §3), not inside an arithmetic expression
  with no typed target. To multiply or subtract two values one of which
  is a string, convert explicitly with `Val(s)` or
  `StringToInteger(s)`.

---

## Further reading

| Topic | Page |
|---|---|
| General language overview | `00_apercu_du_langage.md` |
| Null and nullable types | `02_null_et_nullable.md` |
| Code conventions | `10_conventions_de_code.md` |
| Operators and precedence | `05_operateurs.md` |
| Numeric computation and vectors | `33_vecteurs.md` |
