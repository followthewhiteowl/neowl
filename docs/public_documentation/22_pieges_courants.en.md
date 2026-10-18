# OWL — Common pitfalls

An OWL program is predictable to read, but a few rules shape the writing from
the very first file — the ones that do not guess themselves. Here are those
that surprise most often.

---

## A bare decimal literal is a real

A decimal number written as-is produces a real (floating point). For an exact
value (monetary amount, numeric computation without drift), type the variable
as `numeric` **or** prefix the literal with `0n`.

```owl
nReal is a real = 154.49
nExact is a numeric = 0n154.49
```

## Real precision: `numeric` for exact values

The `real` type is a binary float (IEEE-754). Many decimal values have no
finite binary representation: they are stored to the nearest, with a tiny
error. So `0.1 + 0.2` does not exactly equal `0.3`, and a strict equality
comparison fails:

```owl
nA is a real = 0.1
nB is a real = 0.2
Trace(nA + nB)                               // 0.30000000000000004
IF (nA + nB) = 0.3 THEN Trace("equal") END   // never displayed
```

For any computation that must be exact — amounts, counting, equality
comparisons — use `numeric`. The same operation is exact there and the
comparison is true:

```owl
mA is a numeric = 0n0.1
mB is a numeric = 0n0.2
Trace(mA + mB)                                  // 0.3
IF (mA + mB) = 0n0.3 THEN Trace("equal") END    // displays "equal"
```

Reserve `real` for measurement, scientific computation and geometry, where a
tiny margin of error is acceptable and speed matters.

## The number model in a nutshell

Four numeric types, each with a precise role:

- **`integer`**: signed integer, bounded to about ±9.2×10¹⁸.
- **`numeric`**: exact decimal with *unlimited* precision — it grows with the
  available memory. The pivot type of exact computation.
- **`currency`**: decimal with *fixed* precision (6 decimals), large
  magnitude. Designed for amounts.
- **`real`**: binary float (see the previous section).

Two mechanisms govern moving from one type to another:

**Automatic promotion — the safe widening.** A computation on `integer` that
exceeds the bound never overflows silently: the result automatically becomes a
`numeric`. The value is preserved, exact.

```owl
nBig is an integer = 9223372036854775807     // integer upper bound
nProm is a numeric = nBig + 1                 // promoted to exact numeric: 9223372036854775808
```

**The exception to narrowing — the wall.** The reverse path is not always
possible. Assigning a `numeric` whose *magnitude* does not fit into a
fixed-size target (`integer`, `currency`) raises an exception, instead of
silently writing a truncated value:

```owl
nHuge   is a numeric = 0n100000000000000000000   // 10^20
nTarget is an integer = nHuge                     // exception: magnitude out of capacity
```

Losing only the *decimals* (the magnitude does fit) stays silent: it is an
accepted soft conversion — an `integer` that receives `3.75` keeps `3`. To
drop the decimal part deliberately and visibly in the code, go through an
explicit function (`NumericTruncate`, `Round`) rather than relying on the
implicit conversion.

## `numeric` and writing to a database

A `numeric` is never written silently to a database: if it cannot be
converted for the target column, the write is rejected — no truncated value
quietly stored. On the other hand, respecting a column's *declared* precision
depends on the engine: some engines store loosely and accept values beyond the
announced precision. That is the responsibility of the database engine and the
developer, not a uniform guarantee of the language.

## Prefixing variables

A name that collides with a type or a keyword is rejected at compilation:
`to` clashes with the `TO` keyword, `list` clashes with the `List` type. A
single-letter name is not rejected in itself — `n` or `x` pass without
trouble; only collisions with a reserved word are a problem. The convention is
to prefix by type — `sName`, `nAge`, `oAccount`, `tabNames` — which removes
the ambiguity and makes the code readable.

## Reserved words of the language

Some names are locked by the language and cannot serve as a variable,
parameter or field name. The list below is exhaustive. All forms are
accent-insensitive and case-insensitive: `À`, `a`, `À` mean the same keyword,
and so do `IF`, `if`, `If`. Each EN entry has its FR equivalent, also
reserved.

**Flow control**: `IF`, `THEN`, `ELSE`, `END`, `FOR`, `EACH`/`ALL`/`EVERY`,
`OF`, `IN`, `WHILE`, `LOOP`, `DO`, `SWITCH`, `CASE`, `DEFAULT`, `BREAK`,
`CONTINUE`, `RETURN`, `WITH`, `TO`, `STEP`, `NOT`. French equivalents: `SI`,
`ALORS`, `SINON`, `FIN`, `POUR`, `TOUT`, `DE`, `DANS`, `TANTQUE`, `BOUCLE`,
`FAIRE`, `SELON`, `CAS`, `AUTRE`, `SORTIR`, `CONTINUER`, `RENVOYER`/`RETOUR`,
`AVEC`, `À` (bounds separator), `PAS` (negation and loop increment).

**Exceptions**: `WHEN`, `EXCEPTION` (FR: `QUAND`).

**Logic and arithmetic**: `AND`, `OR`, `_AND_`, `_OR_` (short-circuit forms).
FR: `ET`, `OU`, `_ET_`, `_OU_`.

**Primitive scalars**: `string`, `integer`, `real`, `currency`, `numeric`,
`character`, `boolean`, `buffer`, `byte`, `bytes`.

**Structural composites**: `array`, `structure`, `enumeration`,
`combination`, `list`, `stack`, `queue`, `associative`.

**Type modifiers**: `fixed`, `dynamic`, `by`, `unsigned`, `signe`.

**OOP / procedure declarators**: `class`, `interface`, `procedure`,
`function`.

**Scope and namespace**: `local`, `global` (FR `globale` is also reserved).
`global` and `local` are **scope toggles** placed alone on their line at the
top-level of a container or a script (see `15_lifecycle_champs.md` §2,
`16_lifecycle_etats.md` and `24_projet.md`); the static-method modifier of a
class uses the same `global` keyword (`globale` in French). All three remain
reserved keywords: none can serve as a variable name.

**Instance reference**: `object`, `ancestor`, `MySelf` (FR alias `MoiMême`).
`MySelf` designates, in an event handler or a sub-control top-level, the
current control — see `15_lifecycle_champs.md` §7bis and
`16_lifecycle_etats.md` §10bis.

A forbidden use produces an explicit error at compilation:

```
The word 'string' is a reserved language keyword and cannot be used as a
variable name.
```

Conversely, value type names (`date`, `time`, `datetime`, `duration`,
`json`, `uuid`) and the types coming from the registry (`httpRequest`,
`chrono`, `image`, `color`, …) stay **free** as identifier names. The full
rule is in the conventions chapter.

> **Typical pitfall — silent collision at runtime.** A few examples that
> surprise newcomers:
>
> - `signe is an integer` is rejected (`signe` is a type modifier, used in
>   `integer unsigned on 4 bytes`).
> - `TO is a real` is rejected (`TO` is the loop bounds separator in
>   `FOR i = 1 TO 10`).
> - `STEP is an integer` is rejected (`STEP` is the loop increment; `NOT`,
>   the boolean negation, is reserved for the same reason).
> - `list is an array of strings` is rejected (`list` is a structural
>   composite; prefer `tabNames` with a prefix).
>
> The reflex: prefixing variables by their type (`sName`, `nAge`,
> `tabFruits`) removes any collision risk and stays the recommended OWL
> convention (see `12_prefixes_de_nommage.md`).

> **Important — reserved words also block procedure, method, function and
> class field names**, not just variables. A `PROCEDURE Report()` (keyword of
> the Report sub-block) or `PROCEDURE Signe()` (modifier `signe` of the
> `integer` type) are rejected just like a variable `signe is an integer`.
> The emitted error points straight at the procedure declaration token — no
> misleading cascade.

## `owl test` loads the whole project

`owl test` loads **and runs** the top-level of the whole project, not only the
`test_*` files. A script that produces a side effect at the top-level will run
during the test campaign. Keep side effects inside procedures called
explicitly.

## Testing a window: activating the real controls

During `owl test`, windows do not mount their controls by default: tests are
fast and open no window on screen. A read such as `SAI_X..Value` or an
`onEachChange` event then does not reflect the real control. To write a test
that exercises an input, a click or a focus like a user would, activate the
real controls at the start of the test:

```owl
TestHeadlessMode(False)             // enable the real controls for this test

OpenSibling(FEN_Input)              // opens without blocking, controls ready
TestSimulateInput(SAI_Name, "Léa")  // real typing, character by character
dbgCheckEqual(SAI_Name..Value, "Léa", "input read from the control")
Close(FEN_Input)
```

Three points to remember:

- `TestHeadlessMode(False)` only holds for the test where it is written; the
  next test starts back in fast mode.
- Use `OpenSibling` (non-blocking), not `Open` (modal) which would freeze the
  test waiting for the window to close.
- `TestSimulateInput` and `TestSimulateMouse` replay the real input: each
  keystroke triggers `onEachChange`, a click takes the focus. `Wait(n)` then
  lets the pending timers and events run.

Reserve this for tests that really target the interface: leave fast mode on
for everything else.

## `OR` / `AND` evaluate both sides

The full forms `OR`, `AND` evaluate **both operands**, even if the first is
enough to determine the result. To protect an access when an object may be
`Null`, use the short-circuit forms `_OR_`, `_AND_`:

```owl
IF oAccount = Null OR oAccount.balance > 0 THEN …    // crashes if Null
IF oAccount = Null _OR_ oAccount.balance > 0 THEN …  // safe
```

Full doctrine in `05_operateurs.md` §3.

## `ErrorOccurred`: read right after the call

`ErrorOccurred` reflects the state of the last native operation. Each
following call resets the flag (automatic reset). The read must therefore come
**immediately** after the call to test:

```owl
sRes is a string = StringToDate("gibberish")
IF ErrorOccurred THEN
    Trace("oops")    // ✓
END
```

Not after two chained `Trace` calls: the first consumes the freshness
sentinel, the second re-reads the already-reset value. To reuse the flag later
on, capture it into a boolean variable right after the call:

```owl
sRes is a string = NativeThatCanFail(arg)
bCheck is a boolean = ErrorOccurred      // immediate capture
Trace("result:", sRes, " — error:", bCheck)
```

## `.` member, `..` property

A single dot calls a method or accesses a member (`oObject.Method()`,
`jDoc.field`). Two dots read a property (`tabNames..Occurrence`,
`SAI_X..Value`). Confusing the two produces a compilation error.

## `Trace(currency)` prints the raw form

`Trace` (and the implicit conversion to string) prints a `currency` in its
**raw form**, without trailing zeros. `150.00 - 49.50` produces `100.5`, not
`100.50`. This is deliberate: the stored value is exact, only the textual
representation is compacted.

For a typical accounting 2-decimal display, go through `NumToString` after a
cast to `real` (or to `numeric`):

```owl
mBalance is a currency = 150.00
mDelta   is a currency =  49.50
mNet     is a currency = mBalance - mDelta

Trace(mNet)                              // 100.5  (raw)

rNet is a real = mNet
Trace(NumToString(rNet, ".2f"))          // 100.50
```

The intermediate cast to `real` is necessary: `NumToString` applied directly
to a `currency` treats the value in internal units and produces an oversized
result.

## Everything is explicitly typed

No type inference, no `Variant`. Each variable declares its type at
declaration. This constraint is deliberate: it makes every file readable
without guessing, for a human as well as for an agent.

## `StringToDate` without a mask — accepted forms

Without an explicit mask, `StringToDate` auto-detects five forms:

- `AAAAMMJJ`     — compact form (`"20261225"`)
- `JJ/MM/AAAA`   — European with slashes (`"25/12/2026"`)
- `JJ-MM-AAAA`   — European with dashes (`"25-12-2026"`)
- `AAAA-MM-JJ`   — ISO 8601 date-only (`"2026-12-25"`)
- `AAAA/MM/JJ`   — ISO variant with slashes (`"2026/12/25"`)

The ISO form `AAAA-MM-JJ` is recommended for new scripts: unambiguous,
international, aligned with the standard exchange formats (logs, JSON, CSV).

Any other form (datetime, RFC-3339, alternative regional format) requires an
explicit mask or a dedicated constant:

```owl
dh is a datetime = StringToDate("2019-08-25T17:15:00+02:00", maskDateInternet)
d  is a date     = StringToDate("12 May 2026", "JJ MMMM AAAA")
```

A string that matches no accepted form returns an empty string and sets
`ErrorOccurred` to `True`.

## Testing a blocking or destructive native: `MockCreate` covers it all

`MockCreate` intercepts **any native** by pattern matching. No need to look
for a dedicated system per use case — a single mechanism covers the natives
that block (`ConsoleRead` which waits for stdin, `Warning` which opens a
dialog box) as well as those with destructive side effects (`ToClipboard`
which overwrites the user's clipboard, `HTTPRequest` which issues a real
network call).

Typical case: testing a script that reads standard input without blocking the
rest of the test suite.

```owl
// Fixed return
mRead is a MockNative = MockCreate(ConsoleRead)
mRead.When().Returns("Roger").Activate()

// Multi-read sequence (each call consumes the next element)
mEntry is a MockNative = MockCreate(ConsoleRead)
mEntry.When().ReturnsSequence(["input 1", "input 2", "end"]).Activate()
```

Same pattern to neutralize a network call, a native dialog box, or a
clipboard write: `MockCreate(HTTPRequest)`, `MockCreate(Warning)`,
`MockCreate(ToClipboard)`. A single infrastructure, declarative and chainable.

## `PROCEDURE Helper()` outside the Window block — compile error

In a window file (`FEN_X.owl`), the `PROCEDURE` helpers that address the
sub-controls must live **inside the `is a Window` block**, not after its
`END`. The canon is strict:

```owl
// ✗ Rejected at compilation
FEN_Counter is a Window
    PROPERTIES Title = "Counter" END
    LIB_Value is a Label END
END

PROCEDURE Refresh()                          // outside the block → error
    LIB_Value..Caption = "..."
END
```

```owl
// ✓ Canon
FEN_Counter is a Window
    PROPERTIES Title = "Counter" END

    PROCEDURE Refresh()                      // inside the block → ok
        LIB_Value..Caption = "..."
    END

    LIB_Value is a Label END
END
```

The rule is set by the strict canon of OWL: a window file contains one window,
its sub-controls and its context helpers — nothing else. The full wiring of a
multi-file GUI application is documented in `15_lifecycle_champs.md` §9.

## Addressing a control from a neighboring file — impossible

The sub-controls of a window (`LIB_X`, `SAI_Y`, `BTN_Z`) are visible **inside
the `is a Window` block** that declares them. A neighboring file cannot access
them by their name:

```owl
// FEN_Detail.owl
FEN_Detail is a Window
    LIB_Total is a Label END
END
```

```owl
// other_file.owl — does not compile
LIB_Total..Caption = "..."           // LIB_Total unknown here
```

To share information between files, two canonical paths:

1. **Shared state via `project.owl`**: a project-global variable,
   read/written from the Window block that needs it.
2. **Business class** (OOP): isolates the logic without knowing the
   controls, and stays reusable across windows.

The end-to-end example is in `15_lifecycle_champs.md` §9.

## No long loop in `EndInitialization`

`EndInitialization` runs **before** the window enters its event loop (step 4
of 5 in the opening cycle, see `15_lifecycle_champs.md`). A long loop placed
there blocks everything: the window never paints, nothing shows. `Wait` does
not save this case — it only keeps the display alive from the already-living
event loop.

The canon: `EndInitialization` prepares the state then **hands back control**.
Any long loop (animation, iterative processing) is launched from an event
handler (`onClick`, `onEntry`, ...), which itself runs in the living loop.

## A busy handler does not re-enter

A handler that does not hand back control (a loop with `Wait`) will not be
re-triggered by new events on its own control: keystrokes do feed the
control's content, but the modification event is not re-emitted while the
handler is busy. To read an input continuously during a loop, read the
control's property directly from the loop (polling `SAI_X..Value`), not
through an event.

## `dCopy(source, destination, …)` — the source comes first

The argument order of `dCopy` is **source first, destination next**. Many
common APIs (memcpy, file copy, etc.) reverse it: destination then source.
Reversing here triggers **no error** — the render comes out empty silently,
which can cost time to diagnose.

```owl
imgSrc  is an Image = dCreateImage(64, 64, Red)
imgDest is an Image = dCreateImage(128, 128, White)

dCopy(imgSrc, imgDest, 0, 0, 64, 64, 32, 32, 64, 64)    // OK: copies src to dest
dCopy(imgDest, imgSrc, 0, 0, 64, 64, 32, 32, 64, 64)    // PITFALL: reversed order, empty render
```

The reflex: re-read the order with the mnemonic "**source first, like a
left-to-right read**". `owl explain dCopy` confirms the signature at any time.
