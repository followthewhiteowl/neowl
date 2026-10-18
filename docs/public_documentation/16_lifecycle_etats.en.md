# Lifecycle of a Report

A Report produces a **laid-out document** — typically a PDF.
Unlike a Window, it has no user event loop: its code runs along a
fixed sequence driven by `iImprimeEtat`, punctuated by hooks and
sub-blocks where the developer grafts in their own logic.

---

## 1. Anatomy of a declaration

```owl
ETAT_Invoice is a Report
    PROPERTIES
        Format      = formatA4
        Orientation = orientPortrait
        TopMargin    = 20
        BottomMargin = 20
        LeftMargin   = 20
        RightMargin  = 20
    END

    intro is a DocumentHeader
        PROPERTIES
            Height = 40
        END
        Trace("document header")
    END

    line is a Body
        Trace("rendering a line")
    END

    PROCEDURE Read()
        // business logic (print loop, derived calculations, etc.)
    END
END
```

The canonical prefix is `ETAT_`. A single file may contain
**several Reports**, but **may not mix Report and Window** in the
same file (compile error).

### Constructs accepted in the body

| Construct | Effect |
|---|---|
| `PROPERTIES ... END` | Root properties of the Report (format, margins, ...) |
| `name is a Type [= expr]` | Variable scoped to the Report (initialized on the 1st print) |
| `<NAME> is a <BlockType>` | Declaration of a sub-block (cf. §2) |
| `PROCEDURE Filter/Read/Closing` | Lifecycle hook (cf. §5) |
| Any other `PROCEDURE` / `FUNCTION` | User helper |
| Any other statement (`Trace`, call, `IF`, `FOR`, ...) | Compiled into the **implicit `Opening` event**, run **once**, on the 1st print |

This is a major difference from the Window body: here top-level
instructions **are accepted**, and gathered into an implicit
`Opening` event.

### Confining a variable to the init flow: `LOCAL`

By default, a top-level variable is **scoped to the Report** (visible
to its sub-blocks and procedures by their bare name). For a temporary
that only the initialization code needs — a loop index, an
accumulator — the `LOCAL` switch **confines it to the top-level flow**:
it is then neither scoped to the Report, nor visible to the
procedures, nor reachable from the outside.

`GLOBAL` and `LOCAL` stand **alone on their own line** and toggle the
mode of the declarations that follow, until the reverse toggle — there
is no block to close. `GLOBAL` restores the default (variable scoped to
the Report). The two alternate freely.

```owl
ETAT_Sales is a Report
    nbLines is an integer = 0        // default: scoped to the Report

    LOCAL
    i is an integer = 0               // confined to the init flow
    WHILE i < 10
        nbLines = nbLines + 1
        i = i + 1
    END

    line is a Body
        Trace("lines: " + nbLines)
    END
END
```

Here `nbLines` is reachable throughout the Report; `i`, confined, is not.

### Constraint: `Body` mandatory

A Report with no `Body` block is rejected at compile time. It is the
only mandatory sub-block; the five others are optional.

### Constraint: no window opening from a Report

`Open`, `OpenChild` and `OpenSibling` are **rejected at compile time**
in any code lexically contained in an `is a Report ... END` —
top-level body, sub-blocks (`Body`, `DocumentHeader`, ...), reserved
procedures (`Read`, `Filter`, `Closing`) and user helpers. A Report
produces a document, it has no UI event loop; these three calls make
no sense in this context.

---

## 2. The 6 canonical sub-blocks

| Block | Role |
|---|---|
| `DocumentHeader` | Once at the start of the document — title, period, logo |
| `PageHeader` | Once per page — page number, short title |
| `Body` | Main content — **mandatory** |
| `PageFooter` | Once per page — print date, footer |
| `DocumentFooter` | Once at the end of the document — totals, signatures |
| `Iteration` | Free block, triggered only by `iImprimeBloc(…)` |

> The article (`a`/`an`) simply follows English usage; the compiler
> accepts either, so pick the one that reads naturally.

Each sub-block may contain:
- A `PROPERTIES ... END` block (cf. §9)
- **OWL code** in its body, run on every render of the block
- **WYSIWYG controls** (labels, images) declared as in a Window,
  in absolute positioning

```owl
totals is a DocumentFooter
    PROPERTIES
        Height = 30
    END
    nTotalAmount = ComputeTotal()
    LIB_Total..Caption = "Total: " + nTotalAmount
END
```

> **Note**: a property declared as an **expression in `PROPERTIES`**
> (e.g. `Caption = "Total: " + n`) is **re-evaluated on every render of the
> block**, with the current value of the variables it references. A single
> block printed N times (`iImprimeBloc` loop, automatic Body render) therefore
> shows N distinct values if the expression depends on them. A **literal**
> property (`Caption = "Fixed"`) stays identical on every render. Imperative
> assignment through **code in the body** (`LIB_Total..Caption = …`, as above)
> remains available and **takes precedence** over the `PROPERTIES` expression:
> when both coexist on the same control, the value assigned by code wins.

---

## 3. Printing a Report

Printing takes **three steps**:

```owl
iDestination(iPDFGénérique, "/tmp/invoice.pdf")
bOK is a boolean = iImprimeEtat(ETAT_Invoice)
iFinImprime()
```

- `iDestination` opens the PDF output.
- `iImprimeEtat(ETAT_X [, p1, p2, ...])` launches the sequence (cf. §4).
  Returns `True` if printing succeeded.
- `iFinImprime()` finalizes the PDF.

Calling `iImprimeEtat` without a prior `iDestination` produces a
runtime error.

---

## 4. Execution sequence

The actual execution order inside `iImprimeEtat`, for a single-page
report with no `iSautePage`:

```
1.  Parameter injection (if is a Report(p1, p2, ...))
2.  Application of the PROPERTIES (Format, Orientation, margins)
3.  Opening event (= top-level body code, excluding declarations) — 1st print only
4.  Initialization of each WYSIWYG control of the sub-blocks
5.  DocumentHeader
6.  PageHeader
7.  PROCEDURE Read        (once)
8.  PROCEDURE Filter      (once)
9.  Body                  (once, in addition to the iImprimeBloc in Read)
10. DocumentFooter
11. PageFooter
12. PROCEDURE Closing
```

Four points to note:

- **`Read` is called before `Filter`**, in the order shown.
- **The `Read` and `Filter` hooks are 1× slots**. No automatic loop
  over a data source — it is up to the developer to produce the
  iteration (cf. §6).
- **`Body` is rendered once automatically** after `Filter`,
  **in addition** to the manual calls to `iImprimeBloc("Body")` made
  from `Read`. If you loop N times over Body in Read, you get
  **N+1** renders.
- **`PageFooter` is printed at the end of every page**. With
  `iSautePage` (§7), the sequence becomes: `PageFooter` → page break →
  `PageHeader`. A final `PageFooter` is also printed after
  `DocumentFooter`.

---

## 5. Lifecycle hooks: `Filter`, `Read`, `Closing`

Three reserved procedures that the runtime calls at fixed points of
the sequence above:

| Procedure | Called | Current runtime semantics |
|---|---|---|
| `Read()` (alias `Lecture`) | Step 7 | Canonical place for the **iteration loop** + derived calculations |
| `Filter()` (alias `Filtre`) | Step 8 | Returns a boolean — **return value not consumed** |
| `Closing()` (alias `Fermeture`) | Step 12 | Final cleanup. Cannot cancel anything |

> **`Filter` does not act as an active filter.** The procedure is
> called at step 8 and its return is ignored: returning `False` does
> not skip the record. To filter effectively, manually condition the
> call to `iImprimeBloc("Body")` in `Read`.

---

## 6. Manual iteration: `iImprimeBloc`

To produce an N-line report, the developer manages their own loop in
`Read` (or elsewhere), and calls `iImprimeBloc("block_name")` on each
pass. The target can be:

- A canonical sub-block (`Body`, `DocumentHeader`, ...) — re-printed
- A named `Iteration` block — printed on demand

```owl
nCurrentLine is an integer = 0

ETAT_List is a Report
    line is a Body
        Trace("line " + nCurrentLine)
    END

    PROCEDURE Read()
        // 4 explicit iterations + 1 automatic Body render = 5 lines
        FOR i = 1 TO 4
            nCurrentLine = i
            iImprimeBloc("line")
        END
        nCurrentLine = 5     // for the final automatic render
    END
END
```

Several `Iteration` blocks may coexist with distinct names — handy
for intermediate recaps, separators, conditional insets.

`iImprimeBloc` called outside a Report currently printing returns
`False` and does nothing.

---

## 7. Multi-page: `iSautePage`

`iSautePage()` forces a page break in the print sequence. The runtime
prints `PageFooter`, opens a new page, and prints `PageHeader` on that
new page.

```owl
PROCEDURE Read()
    FOR i = 1 TO 3
        iImprimeBloc("line")
        IF i < 3 THEN iSautePage()
    END
END
```

The property `SautDePageApresBloc = True` on a sub-block triggers the
same mechanism automatically after its render (cf. §9).

---

## 8. Report parameters

A declaration can take typed parameters; they are reachable as
**globals** throughout the Report code (sub-blocks and procedures):

```owl
ETAT_Invoice is a Report(sClient is a string, mAmount is a real)
    intro is a DocumentHeader
        Trace("Client: " + sClient)
    END
    line is a Body
        Trace("Amount: " + mAmount)
    END
END

iDestination(iPDFGénérique, "/tmp/f.pdf")
iImprimeEtat(ETAT_Invoice, "Alice", 1499.99)
iFinImprime()
```

The values passed to `iImprimeEtat` are injected into the matching
globals **at the start of the sequence** (step 1), before `Opening`.

---

## 9. Properties

### At the Report root level

| Property | Type | Values |
|---|---|---|
| `Format` | enum | `formatA4` (default), `formatA3`, `formatA5`, `formatLetter`, `formatLegal` |
| `Orientation` | enum | `orientPortrait` (default), `orientLandscape` |
| `TopMargin`, `BottomMargin`, `LeftMargin`, `RightMargin` | integer (mm) | — |

### At each sub-block level

| Property | Type | Effect |
|---|---|---|
| `Height` | integer (mm) | Reserved height |
| `Visible` | boolean (default True) | False → block skipped at print |
| `BackgroundColor` | color | Block background |
| `Splittable` | boolean | May be split across two pages |
| `PageBreakAfterBlock` | boolean | Automatic page break after this block |

---

## 10. Main differences from Window

| Aspect | Window | Report |
|---|---|---|
| Event loop | Yes (click, focus, ...) | None — linear print sequence |
| Top-level body code | Rejected | Accepted → compiled into an `Opening` event |
| Hook equivalent to `EndInitialization` | Yes (`EndInitialization`) | No. The `Opening` event plays that role |
| Several declarations / file | No (1 Window per file) | Yes |
| Mix Window + Report in a file | Forbidden | Forbidden |
| `Open` / `OpenChild` / `OpenSibling` | Legitimate (opening another window) | Rejected compile-time |
| Cancellation possible | `CloseAuthorization` | No mechanism |

---

## 10bis. `MySelf` in a Report

The keyword `MySelf` (alias `MoiMême`) also works in the sub-blocks
and top-levels of a Report, with semantics strictly analogous to
those of windows (cf. `15_lifecycle_champs.md` §7bis):

- **Top-level of a sub-control** of a Report (level-0 code in the
  control body, excluding `PROCEDURE`): `MySelf` designates the
  current sub-control. `MySelf..Caption = "..."` there is equivalent
  to `LIB_X..Caption = "..."`.
- **Top-level of the Report** (level-0 code, the compiled equivalent
  of `Opening`) and the `Read` / `Filter` / `Closing` hooks:
  `MySelf` references the Report itself. Accessing properties via
  `MySelf..Prop` there is reserved for a later version; for now, read
  and write parameters and properties by their direct name.

`MySelf` is inherited along the call chain: a procedure called from a
sub-control top-level sees the same `MySelf` as its caller.

---

## 11. Common pitfalls

- **Filter does not filter**. The return of `Filter` is not consumed
  by the current runtime. To filter effectively, test the condition
  and call `iImprimeBloc` conditionally in `Read`.
- **No automatic loop over the data**. `Body` is called only once by
  default. For N lines, loop in `Read` with `iImprimeBloc("Body")`
  (or via `iSautePage` between pages).
- **Body fires N+1 times if you loop N times in Read**. The automatic
  render of `Body` at step 9 adds to the manual calls made from
  `Read`. If you want exactly N renders, do not call
  `iImprimeBloc("Body")` the last time — let the automatic render do
  it.
- **`iImprimeEtat` without a prior `iDestination`** → runtime error.
  Always `iDestination(...)`, then `iImprimeEtat`, then
  `iFinImprime()`.
- **`Body` block missing** → compile error. It is the only mandatory
  sub-block.
- **Top-level body variables**. They are **scoped to the Report**
  (not program globals) and initialized on the **1st print** only;
  their value then **persists** from one print to the next. The
  top-level (`Opening`) does not replay on subsequent prints:
  reinitializing there refreshes nothing beyond the first. To start
  from a fresh state on every print, reinitialize in a hook replayed
  on every print, for example `Read`.

---

## Further reading

| Topic | Sheet |
|---|---|
| Language overview | `00_apercu_du_langage.md` |
| Control flow | `03_controle_de_flux.md` |
| Lifecycle of a Window | `15_lifecycle_champs.md` |
| Code conventions | `10_conventions_de_code.md` |
