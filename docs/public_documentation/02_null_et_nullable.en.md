# Null and nullable types

OWL explicitly distinguishes two concepts:

- **A type's default value**: `""` for a string, `0` for an integer,
  `False` for a boolean, etc. This is what a freshly declared typed
  variable holds when left uninitialized.
- **The absence of a value**: represented by the `Null` keyword. An
  ordinary variable **cannot** hold `Null`; only variables explicitly
  marked nullable can.

This separation makes programs predictable: `n is an integer`
guarantees that `n` holds an integer, never `Null`. To allow absence,
it must be declared.

---

## 1. The `Null` value

`Null` is a language keyword. The `=` comparison only recognizes
equality between two `Null` values:

```owl
Trace(Null = 0)        // False
Trace(Null = "")       // False
Trace(Null = False)    // False
Trace(Null = Null)     // True
```

To test whether a variable holds `Null`, two equivalent forms:

```owl
IF IsNull(v) THEN …       // explicit form, preferred
IF v = Null THEN …        // direct form
```

`IsNull(v)` is more readable and signals the intent clearly. On a
non-nullable variable both always return `False` (see next section).

---

## 2. Non-nullable variables (default)

Without a special marker, a typed variable **cannot** hold `Null`.
When declared without initialization, it receives the **default value
of its type**:

| Type | Default value |
|---|---|
| `string` | `""` |
| `integer`, `real`, `currency`, `numeric`, `duration` | `0` |
| `boolean` | `False` |
| `date`, `time`, `datetime` | current instant |
| `buffer` | empty buffer |
| `array of <T>` | empty array |
| User-defined structure | clone of the template, fields at default value |

```owl
sName is a string
Trace("[", sName, "]")        // []
Trace(IsNull(sName))          // False

nAge is an integer
Trace(nAge)                   // 0
```

**Assigning `Null` afterwards** to a non-nullable variable is
accepted: it is sugar that **resets to the default value**. The
variable is never actually `Null` afterwards.

```owl
sName is a string = "Alice"
sName = Null
Trace("[", sName, "]")        // []   — reset to ""
Trace(IsNull(sName))          // False
```

---

## 3. Nullable variables with `?`

For a variable to represent the absence of a value distinctly from its
default value, annotate it with `?` after the type:

```owl
sComment is a string?
Trace(IsNull(sComment))       // True   — uninitialized = Null

sComment = "OK"
Trace(IsNull(sComment))       // False

sComment = Null
Trace(IsNull(sComment))       // True

sComment = ""                 // empty string, distinct from Null
Trace(IsNull(sComment))       // False
```

Canonical use case: an optional form field where you want to
distinguish "not entered" from "entered then cleared".

```owl
sNote is a string?

IF IsNull(sNote) THEN
    // never entered
ELSE IF sNote = "" THEN
    // entered then cleared
ELSE
    // actual entry
END
```

Without the `?`, the three cases would be indistinguishable: the
variable would hold `""` everywhere.

---

## 4. Containers and structures

A typed array holds only values of the type — never `Null`. The cells
created by extension receive the default value:

```owl
tabNames is an array of 3 strings
Trace("[", tabNames[1], "]")  // []  — not Null

tabScores is an array of integers
tabScores[5] = 100
Trace(tabScores[2])           // 0  — auto-extension at default value
```

For nullable elements, type the collection as `array of strings?`.

The scalar fields of a user-defined structure follow the same rule:
default value at clone time, except for a field explicitly annotated
nullable.

```owl
STPerson is a structure
    sName is a string
    sOptionalNote is a string?
END

stClient is an STPerson
Trace(IsNull(stClient.sName))            // False  — "" by default
Trace(IsNull(stClient.sOptionalNote))    // True
```

---

## 5. Summary

| Situation | Result |
|---|---|
| Non-nullable variable without init | Type's default value |
| Non-nullable variable `= Null` (init or after) | Reset to the default value |
| Nullable variable (`?`) without init | `Null` |
| Nullable variable `= Null` | `Null` |
| `Null = Null` | `True` |
| `Null = <typed value>` | `False` |
| `IsNull(v)` on a typed default value | `False` |

---

## 6. Common mistakes

- **Confusing empty string and `Null`**: `""` is a value, distinct
  from `Null`. Use `s = ""` to test the former, `IsNull(s)` for the
  latter.
- **Initializing to the default value for no reason**: `nAge is an
  integer = 0` needlessly duplicates the default. Write `nAge is an
  integer`.
- **Assuming a structure field can "be missing"**: no, unless annotated
  `?`. Otherwise it always holds the type's default value.

---

## Further reading

| Topic | Page |
|---|---|
| Language overview | `00_apercu_du_langage.md` |
| Detailed type system | `01_types_et_coercions.md` |
| Code conventions | `10_conventions_de_code.md` |
