# Literal initialization `{ ... }`

Compact form to initialize a composite type (`Frame`, `Font`, `Style`,
`Corner`, `Stroke`) without declaring a temporary variable nor
assigning the sub-properties one by one.

## Syntax

```owl
myVar is a T = {
    field1 = expression
    field2 = expression
    ...
}
```

The accepted separators between fields: **comma** or **newline**.
Mixing is OK.

## Target type sources

The `{ ... }` literal **must** have an explicit target type — no
inference is done. Three sources are recognized:

| Source | Example | Status |
|---|---|---|
| Variable declaration | `var is a Frame = { ... }` | ✅ |
| Typed variable assignment | `var = { ... }` (declared separately) | ✅ (replacement) |
| Native function argument | `f({ ... })` | ✅ |
| Assignment to a duration-typed property | `t..Interval = { ... }` | ✅ (see "Duration by components") |
| Assignment to a structure field | `cfg.delay = { ... }` | ✅ for a duration-typed field |
| DSL property in `PROPERTIES` | `Frame = { ... }` | ❌ not supported — use a separate variable |
| User-procedure argument | `f({ ... })` | ❌ not supported |

## Recursion

Sub-objects (`Corner`, `Stroke`, `Font`, `Frame` inside `Style`)
accept their own `{ ... }` literal:

```owl
myFrame is a Frame = {
    Color = DarkBlue
    TopLeftCorner = {
        Width = 8
        Height = 4
    }
    BorderBottom = {
        Color     = DarkRed
        Thickness = 3
        Type      = LineDash
    }
}
```

The sub-type is deduced from the name of the target field
(`Frame..TopLeftCorner` → `Corner`, `Frame..BorderBottom` → `Stroke`,
`Style..Font` → `Font`, etc.).

## Complex expressions

Field values accept **all** OWL expressions: function calls, operators,
variable references, chained accesses.

```owl
baseThick is an integer = 2

myFrame is a Frame = {
    Color     = RGB(180, 30, 30)             // function call
    Thickness = baseThick + 1                // arithmetic
    Edges     = BorderTop + BorderBottom     // combinable enum
}
```

## Duration by components

The `duration` type accepts a particular `{ ... }` literal form: it is
not a field fill but a **construction by components**. The accepted
names are exactly the readable components of a duration:

| Component | Weight |
|---|---|
| `Day` | 86,400,000 ms |
| `Hour` | 3,600,000 ms |
| `Minute` | 60,000 ms |
| `Second` | 1,000 ms |
| `Millisecond` | 1 ms |

```owl
d is a duration = { Minute = 2, Second = 30 }    // 150,000 ms
e is a duration = { Hour = 1, Minute = 30 }      // 5,400,000 ms
empty is a duration = { }                        // 0 ms
```

The produced value is the weighted sum of the components in
milliseconds. An integer stays accepted in parallel (`d is a duration =
5000` → 5,000 ms).

**Rules**:

- The read-access totals (`InSeconds`, `InMinutes`…) are **not**
  construction components and are rejected.
- `Hundredth` does not exist: the finest grain is `Millisecond`.
- A component may appear only once (`{ Second = 1, Second = 2 }` is
  rejected at compilation).
- An unknown component is reported at compilation with the list of
  valid components.

### Assignment to a duration-typed property

Unlike composite types, the duration literal is also accepted in
**property assignment** when the property expects a duration (for
example `..Interval` of a `Timer`):

```owl
t is a Timer
t..Interval = { Minute = 2, Second = 30 }   // 150,000 ms
t..Interval = 5000                            // integer coerced → 5,000 ms
```

Same for a `duration`-typed structure field:

```owl
ConfigTimer is a structure
    delay is a duration
END

cfg is a ConfigTimer
cfg.delay = { Second = 3 }   // 3,000 ms
```

## Disambiguating `{ ... }` (3 contexts)

The language already uses `{ }` in two places. The instance literal is
distinguished by the syntactic context:

| Context | Form | Meaning |
|---|---|---|
| **LHS of a statement** | `{a, b} = ...` | Indirection: assignment by dynamic name |
| **RHS after `.`** | `obj.{expr}` | Dynamic field name |
| **RHS after `=` / `(` / `,`** | `... = { Color = X }` | **Instance literal init** |

## Points of attention

- **Reassignment `var = { ... }`**: on an already-declared variable,
  the literal **fully replaces** the instance — the fields not cited
  revert to their default value (this is not a merge).
- **Init inside `PROPERTIES`** (Window / Report DSL): go through a
  separate variable — declare `c is a Frame = { ... }` then
  `control..Border = c`.
- **User-procedure argument**: `myProc({ ... })` requires a prior
  declaration of the variable.
- **Type-prefixed form** (`Frame { Color = X }`, C / JS style): the
  form with `=` (declaration or assignment) is the only one
  recognized.
- **Automatic recursion of sub-types**: available on the framework
  composite types (`Frame`, `Style`, `Font`, `Corner`, `Stroke`). For
  a user type, declare each sub-object explicitly.

---

## Further reading

| Topic | Page |
|---|---|
| Type system and coercions | `01_types_et_coercions.md` |
| Field lifecycle | `15_lifecycle_champs.md` |
| Modern layouts (`PROPERTIES` DSL) | `18_layouts.md` |
| Code conventions | `10_conventions_de_code.md` |
