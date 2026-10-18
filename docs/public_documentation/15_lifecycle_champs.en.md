# Lifecycle of a Window

The code of a window is dispatched across several places that run at
different moments: the body, `EndInitialization`, the event
procedures of the sub-controls, `CloseAuthorization`, `Closing`. This
page explains what goes where, and in what order it fires.

---

## 1. Anatomy of a declaration

A window is declared in **its own file**: a source file may contain
only a single `is a Window`. A second window declaration in the same
file is rejected at compile time.

The body of an `is a Window` accepts only certain constructs:

| Construct | Effect |
|---|---|
| `PROPERTIES ... END` | Static properties of the window (title, size, ...) |
| `CONSTANT ... END` | Constants hoisted into the scope |
| `name is a Type [= expr]` | Variable scoped to the window (cf. §2) |
| `NAME is a Control <Kind>` | Declaration of a sub-control |
| `PROCEDURE` / `FUNCTION` | Event handlers, lifecycle hooks, helpers |

Anything that falls into none of these categories is handled
differently:

- A call (`Foo()`), a `Trace(...)`, an access to a sub-control
  (`LIB_x..Caption = "y"`) produces a **warning** (*unknown construct
  in the window body; the line is skipped*) — the line is ignored and
  compilation continues.
- A **bare assignment** such as `n = 5` is interpreted as an attempt
  at a **window property** and produces a **compile error** (*unknown
  property 'n' for Window*).

To run code on opening, use `EndInitialization` (cf. §3).

---

## 2. Variables at the top level of the body

Variables declared at the top level of a window are **scoped to the
window**, not program globals. Their initialization expression is
evaluated **on every opening** (the top level *is* the initialization
code). From the outside they are accessed in a **qualified** way,
`FEN_X.myVariable`, once the window is open — never by their bare name.

```owl
FEN_Account is a Window
    PROPERTIES
        Title = "Account"
        Layout = dispFree
    END

    nbTries is an integer = 0

    SAI_Login is a Control Input
        PROPERTIES
            X = 10
            Y = 10
        END
        PROCEDURE onValidation()
            nbTries = nbTries + 1   // bare name: internal to the window
        END
    END
END

Open(FEN_Account)
Trace("tries: " + FEN_Account.nbTries)   // qualified, after opening
```

Inside the window (top level, sub-controls, procedures), the variable
is used by its **bare name**. From the script, only the **qualified**
access is valid, and only after `Open`: accessing the variable while
the window is not open **raises an exception** — the window must be
open before any access from the outside.

Consequence: re-opening a window **re-initializes** its top-level
variables (the init code re-runs on every opening). To keep state
between two openings, store it outside the window.

### Confining a variable to the init flow: `LOCAL`

By default, a top-level variable is **scoped to the window** (visible
to its sub-controls and procedures by their bare name). For a temporary
that only the initialization code needs — a loop index, an
accumulator — the `LOCAL` switch **confines it to the top-level flow**:
it is then neither scoped to the window, nor visible to the
procedures, nor reachable from the outside.

`GLOBAL` and `LOCAL` stand **alone on their own line** and toggle the
mode of the declarations that follow, until the reverse toggle — there
is no block to close. `GLOBAL` restores the default (variable scoped to
the window). The two alternate freely.

```owl
FEN_List is a Window
    PROPERTIES
        Title = "List"
    END

    nbLines is an integer = 0        // default: scoped to the window

    LOCAL
    i is an integer = 0               // confined to the init flow
    WHILE i < 10
        nbLines = nbLines + 1
        i = i + 1
    END

    GLOBAL
    total is an integer = nbLines    // scoped to the window again
END
```

Here `nbLines` and `total` are reachable as `FEN_List.nbLines` after
opening; `i`, confined, is not.

---

## 3. Opening: `Open(FEN_X)`

A declared window does not display on its own. It is an opening call
that **mounts** it (instantiates it and makes it active). Three forms
exist:

| Call | Modal | Effect |
|---|---|---|
| `Open(FEN_X)` | Yes | Blocks the caller until close. Returns the value passed to `Close`. |
| `OpenChild(FEN_X)` | No | Opens in parallel and returns control immediately. |
| `OpenSibling(FEN_X)` | No | Same as `OpenChild`, with no parent-child link to the current window. |

**Principle to remember: a control lives in a mounted window.** As
long as a window has not been opened, its sub-controls do not exist in
memory. Reading or writing `SAI_Login..Value` before opening its
window is an **error**: there is no instance in which to look for the
value. Open first, access afterward.

The rest of this section describes `Open`. The multi-instance aspect
of the non-modal forms is covered in §5.

`Open(FEN_X)` triggers the modal opening. The sequence is:

1. Instantiation of the **sub-controls** in the lexical order of the body.
2. Application of the **properties** of each sub-control.
3. Display of the window if `Visible = True` (default).
4. Call to **`EndInitialization()`** if declared.
5. **Event loop**: the handlers (`onClick`, `onValidation`, ...) are
   called on every user interaction until close.

`EndInitialization` is the **canonical place** for anything that must
run once the window is ready: dynamic configuration of sub-controls,
initial loading, focus, etc.

```owl
PROCEDURE EndInitialization()
    LIB_Title..Caption = "Hi " + sCurrentUser
    SAI_Login..Value  = sLastLogin
END
```

Before `EndInitialization` is called, the sub-controls exist in
memory but their properties are only those declared in `PROPERTIES`.
Any dynamic configuration goes here.

> The name `Initialization` is not a lifecycle procedure: the compiler
> rejects it and directs the initialization code to `EndInitialization`
> or to the element body directly.

### Under `owl test`: headless mode

Under the `owl test` runner, the constant `TestHeadlessMode` is on by
default. In this mode, `Open`, `OpenChild` and `OpenSibling`
short-circuit the event loop: the window is mounted in memory,
`EndInitialization` runs, then the call returns without blocking. Test
scenarios can therefore instantiate a window, read/write its fields
and call `Close()` without triggering a graphical UI. In normal
execution (`owl <file.owl>`), the event loop is active and `Open` is
blocking as described above.

### Window parameters

A window can declare **parameters on its first line**, between
parentheses after `Window`, exactly like a Report:

```owl
FEN_Record is a Window(clientId is an integer, title is a string)
    PROPERTIES
        Title = "Client record"
        Layout = dispFree
    END

    // clientId and title are available here, by their bare name, in the
    // whole scope of the window: top level, sub-controls, events and procedures.
    Trace("opening record " + clientId)

    PROCEDURE EndInitialization()
        LIB_Title..Caption = title
    END
END
```

The type annotation (`is an integer`) is **optional**: `Window(message,
priority is an integer)` mixes an untyped parameter and a typed one.

The values are passed at opening, after the window name:

```owl
Open(FEN_Record, 42, "Details")   // clientId = 42, title = "Details"
```

Injection happens **before** the top-level code runs, so the top level
and `EndInitialization` already read the received values. The three
opening forms (`Open`, `OpenChild`, `OpenSibling`) accept these
arguments.

Contracts on the number of arguments:

- **Fewer arguments than parameters**: the parameters not supplied
  stay at `Null` (`Open(FEN_Record)` leaves `clientId` and `title` at `Null`).
- **More arguments than parameters**: the surplus is ignored without error.

---

## 4. Closing: `Close([FEN_X] [, value])`

Three possible triggers: the window's close button, a system shortcut,
an explicit `Close()` call (optionally with a return value for
`Open`). `Close()` with no argument, or `Close(FEN_X)`, closes the
**current window** — the last one opened (cf. §5 on the stack). The
sequence is:

1. **`CloseAuthorization()`** is called if declared. It can **cancel**
   the close by returning `False`. Any other return (or no `RESULT`)
   authorizes the rest.
2. **`Closing()`** is called if declared. Last chance to observe the
   window alive (cleanup, log, save). **Cannot cancel**.
3. The window and its sub-controls are destroyed.

```owl
PROCEDURE CloseAuthorization()
    IF bUnsavedChanges THEN
        RESULT False                  // cancels the close
    END
    // no RESULT → close authorized
END

PROCEDURE Closing()
    Trace("FEN_Account closed, " + nbTries + " tries.")
END
```

---

## 5. Several windows: stack and multi-instance

The non-modal forms `OpenChild` and `OpenSibling` do not wait for the
close: they return control at once. So you can have **several windows
open at the same time**.

### Window stack

Open windows stack up. The **last one opened is the current window**:
it is the one that field accesses target and the one that `Close()`
closes. Once it is closed, the previous one becomes current again.

```owl
OpenChild(FEN_List)      // FEN_List current
OpenChild(FEN_Record)    // FEN_Record current (stacked on top)
Close()                  // closes FEN_Record ; FEN_List current again
Close()                  // closes FEN_List
```

### Several instances of the same window

Each opening creates an **independent instance**. Opening the same
window twice gives two distinct instances, each with its own state:
modifying a field in one does not affect the other.

```owl
FEN_Record is a Window
    PROPERTIES
        Title       = "Client record"
        Layout      = dispFree
    END

    SAI_Name is a Control Input
        PROPERTIES
            X = 10
            Y = 10
        END
    END
END

OpenSibling(FEN_Record)           // instance A (current)
SAI_Name..Value = "Smith"
dbgCheckEqual(SAI_Name..Value, "Smith", "instance A")

OpenSibling(FEN_Record)           // instance B, independent (current)
SAI_Name..Value = "Jones"
dbgCheckEqual(SAI_Name..Value, "Jones", "instance B")

Close()                           // closes B ; A current again
dbgCheckEqual(SAI_Name..Value, "Smith", "instance A intact")
Close()                           // closes A
```

This is what allows, for example, displaying two client records side
by side without them sharing their values.

---

## 6. Window hooks recap

| Procedure | When | Cancellable | Typical use |
|---|---|---|---|
| `EndInitialization` | After the sub-controls are created | — | Dynamic config, initial loading |
| `CloseAuthorization` | Before close | Yes (`RESULT False`) | "save?" guard |
| `Closing` | During close | No | Cleanup, log |
| `RequestRefresh` | On a call to `RequestUpdateUI` (cf. §8) | — | Refresh the display |
| `onResize` | On resize | — | React to the new size (cf. §6bis) |

---

## 6bis. Controlling a window's size and state

An open window can be driven by code: its **geometry** (size,
position) and its **display state** (normal, minimized, maximized).

### Geometry: read/write properties

The `..Width`, `..Height`, `..X` and `..Y` properties of an open
window are **modifiable at runtime**. Rewriting them resizes or moves
the window:

```owl
FEN_Editor..Width = 800     // resizes
FEN_Editor..Height = 600
FEN_Editor..X = 100           // moves the top-left corner
FEN_Editor..Y = 50
```

### `WinSize` — resize, move, change state

`WinSize` bundles these operations into a single function, with four
forms:

| Call | Effect |
|---|---|
| `WinSize(FEN_X, width, height)` | Resizes |
| `WinSize(FEN_X, x, y, width, height)` | Moves then resizes |
| `WinSize(FEN_X, mode)` | Sets the display mode |
| `WinSize(FEN_X)` | Returns the current display mode (integer) |

The target window can be omitted or passed as an empty string (`""`)
to target the **active window**.

The `mode` is one of the constants:

| Constant | Value | Effect |
|---|---|---|
| `normalSize` | 0 | Normal size (neither minimized nor maximized) |
| `minSize` | 1 | Minimized to an icon |
| `maxSize` | 2 | Maximized to the fullest |

```owl
nMode is an integer = WinSize(FEN_Editor)   // reads the current mode
WinSize(FEN_Editor, maxSize)                 // maximizes
WinSize(FEN_Editor, 640, 480)                // back to a fixed size
```

### State shortcuts

Three shortcuts cover the usual state changes. The window is optional
(active window by default):

| Call | Equivalent |
|---|---|
| `Maximize([FEN_X])` | `WinSize(FEN_X, maxSize)` |
| `Iconize([FEN_X])` | `WinSize(FEN_X, minSize)` |
| `Restore([FEN_X])` | `WinSize(FEN_X, normalSize)` |

### The `onResize` event

A window can react to its own resize by declaring the procedure
`onResize(nWidth, nHeight)` in its body. It fires every time the size
actually changes — whether the change comes from rewriting
`..Width`/`..Height`, from a `WinSize` call, or from a user action on
the window edges. The new dimensions are received as parameters (pixels).

```owl
FEN_Editor is a Window
    PROPERTIES
        Title       = "Editor"
        Width       = 400
        Height      = 300
        Layout      = dispFree
    END

    PROCEDURE onResize(nWidth is an integer, nHeight is an integer)
        LIB_Dim..Caption = nWidth + " x " + nHeight
    END
END
```

> The event's French alias is `surChangementTaille`. The functions and
> constants follow the same bilingual rule, with French forms
> `FenTaille`, `Maximise`, `Iconise`, `Restaure`, `TailleNormale`,
> `TailleMini`, `TailleMaxi`.

---

## 6ter. Reading the real geometry of a control

A control exposes its geometry for reading through four properties:
`..X`, `..Y` (position) and `..Width`, `..Height` (size). These reads
return the **real geometry** of the control once the window is
mounted — what the control actually occupies on screen, not what the
file declares. Three points guide their interpretation, and the common
intuition is often the wrong one.

### Reference frame of `..X` / `..Y`: the window's client area

`..X` and `..Y` are expressed **relative to the top-left corner of the
window's client area**, never relative to the parent that holds the
control. Two cases make the two frames diverge — those are the
surprising ones:

- **Control in a container**: a control placed at the top of a
  container that is itself offset by 100 pixels returns `..X = 100`,
  not `0`. The container offset is included.
- **Control in a tab pane**: the control is additionally offset by the
  tab position, its tab bar and the pane frame. Here again, `..X` /
  `..Y` count from the window, not from the pane.

### `..Width` / `..Height`: the size of the real render

`..Width` and `..Height` return the size actually occupied, which may
**differ from the declared value** in the file:

- a stretched control (`Alignment = alignStretch`, or `Stretch = True`)
  takes the size the layout grants it, even without a declared `Width`;
- in automatic layout, the final size results from the layout
  computation, not from a written value.

In free layout (`dispFree`), the declared geometry coincides with the
real geometry: these reads then return the placed values.

### Reading before opening: an absence of value

A control only exists in memory once its window is mounted (cf. §3).
Reading its geometry **before opening** returns `Null` — an **absence
of value**, not a `0` that would look like a measurement. This
contract lets you tell "not measurable yet" from a real position at the
origin.

From `EndInitialization` on, however, the returned geometry is already
the final geometry: the read is reliable there, without waiting for the
display.

### Example

```owl
// FEN_Record.owl — the window alone (a file contains only one window)

FEN_Record is a Window
    PROPERTIES
        Title       = "Record"
        Width       = 400
        Height      = 300
        Layout      = dispVertical
        Spacing     = 4
    END

    BTN_Stretch is a Control Button
        PROPERTIES
            Caption    = "Save"
            Height     = 30
            Alignment  = alignStretch     // takes the full width, no declared Width
        END
    END

    CONT_Box is a Control Container
        PROPERTIES
            Width       = 300
            Height      = 100
            Layout      = dispVertical
            Alignment   = alignEnd        // pinned to the right of a 400-wide window
        END

        LIB_Inside is a Control Label
            PROPERTIES
                Caption = "Inside the container"
            END
        END
    END
END
```

```owl
// demo.owl — neighboring script that opens the window and reads its geometry

// Before opening: the control is not mounted -> absence of value.
Trace(BTN_Stretch..Width = Null)          // True

OpenSibling(FEN_Record)

Trace(BTN_Stretch..Width)                 // 400: real render, no declared Width
Trace(LIB_Inside..X)                      // 100: relative to the window, not the container (0)

Close(FEN_Record)
```

---

## 6quater. Writing the geometry of a control

The four properties `..X`, `..Y`, `..Width`, `..Height` can also be
written, not just read. What the write produces depends on the
**layout mode** of the container that holds the control. The rule fits
in one sentence: **a written size is honored, a written position in
automatic layout is refused.**

### Size in automatic layout: a persistent constraint

In automatic layout (`dispVertical`, `dispHorizontal`), writing
`..Width` or `..Height` on a control sets a **real constraint** on its
size. The requested dimension is applied immediately, and above all it
**survives layout recomputations** — a window resize, the addition or
removal of a neighboring control do not reset it to the computed value.
The written size wins over stretch or alignment on the concerned axis:
it freezes the dimension.

Fixing a control's size shifts its neighbors in the stack: increasing
the height of a control in vertical layout pushes down the one that
follows by the same amount.

### Position in automatic layout: a refused write

In automatic layout, a control's position is **entirely derived** by
the engine: it is the one that decides where each control lands on the
stack axis as on the transverse axis. Writing `..X` or `..Y` on such a
control is therefore **refused with an explicit error**: the control
**does not move**, and the write raises a recoverable error through
`ErrorOccurred` / `ErrorInfo`. The read-back always returns the
position the layout imposes.

A move that would undo itself on the next recompute would be the worst
outcome — the user would believe they succeeded. So the refusal is
clear-cut. To position a control by hand, switch its container to free
layout (see below).

### Free layout: size and position persist

In free layout (`dispFree`), no engine computes the geometry: all four
properties can be written and **persist**, all four. This is the mode
to choose when pixel-precise placement is the need.

### Example

```owl
// FEN_Panel.owl — the window alone, in vertical layout.

FEN_Panel is a Window
    PROPERTIES
        Title       = "Panel"
        Width       = 300
        Height      = 200
        Layout      = dispVertical
        Spacing     = 4
    END

    LIB_Line is a Control Label
        PROPERTIES
            Caption    = "Line"
            Height     = 30
            Alignment  = alignStretch
        END
    END
END
```

```owl
// demo.owl — neighboring script that writes the geometry.

OpenSibling(FEN_Panel)

// Writing a size: constraint applied and persistent.
LIB_Line..Height = 60
Trace(LIB_Line..Height)                // 60: the constraint holds

// Writing a position in automatic layout: refused.
LIB_Line..X = 40
IF ErrorOccurred THEN
    Trace("position refused: " + ErrorInfo())
END
Trace(LIB_Line..X)                     // 0: the stack imposes the position, the control did not move

Close(FEN_Panel)
```

For `LIB_Line..X = 40` to take effect, declare `FEN_Panel` with
`Layout = dispFree`: there both position and size are written freely
and persist.

---

## 7. Events by control type

The events below are declared as `PROCEDURE` inside the body of the
sub-control. The name must match exactly (case aside); an unrecognized
name compiles without error but will never fire.

**Button, Label**
- `onClick()` — left click
- `onDoubleClick()` — left double-click

**Input**
- `onEntry()` / `onExit()` — focus gained / lost
- `onValidation()` — Enter or Tab
- `onEachChange()` — on every character typed or pasted

**ComboBox**
- `onEntry()` / `onExit()`
- `onOpen()` / `onClose()` — of the dropdown (not the window)
- `onSelection()` — the selected value changes
- `onEachChange()` — editable combo, on every keystroke

**List**
- `onEntry()` / `onExit()`
- `onSelection()`

**Table**
- `onEntry()` / `onExit()` — focus gained / lost by the control
- `onClick()` / `onDoubleClick()`
- `onSelectionChange()`, `onRowEnter()`, `onRowExit()`
- `onRowDisplay(nIndex)` — fired on the render of each row
  (`nIndex` 1-based), to customize appearance row by row

**RadioGroup, CheckBox**
- `onEntry()` / `onExit()`
- `onClick()` / `onDoubleClick()`
- `onEachChange()`

**Looper**
- `onEntry()` / `onExit()`
- `onClick()` / `onDoubleClick()`
- `onRowDisplay(nIndex)`, `onRowSelection(nIndex)`

**Image** — full mouse-tracking palette:
- `onClick()` / `onDoubleClick()`
- `onMouseEnter()` / `onMouseLeave()` — bracket the mouse session
- `onMouseMove(nX, nY)` — continuous movement in the area
- `onLeftButtonDown(nX, nY)` / `onLeftButtonUp(nX, nY)`
- `onRightButtonDown(nX, nY)` / `onRightButtonUp(nX, nY)`
- `onRightDoubleClick(nX, nY)`
- `onWheel(nMovement, nX, nY)`
- `onContextMenu(nX, nY)`
- `onMove(nX, nY)`, `onResize(nWidth, nHeight)`

**Container**: no event exposed.

---

## 7bis. `MySelf` — reference to the current control

Inside an event handler, the keyword `MySelf` (alias `MoiMême`)
designates the control running the code at that instant. The same
procedure can therefore be shared between several controls and
distinguish the one that triggered the event.

```owl
LIB_Red is a Control Label
    PROPERTIES
        Caption = "Red"
        Color = RGB(255, 0, 0)
    END
    PROCEDURE onClick()
        Trace("Clicked: " + MySelf..Caption)
    END
END
```

`MySelf` also resolves in the **top level** of a sub-control (the
level-0 code in the control body, outside `PROCEDURE`), where it
references the current control — `MySelf..Caption = "..."` there is
strictly equivalent to `LIB_X..Caption = "..."`. Same thing in the
window's `EndInitialization`, where `MySelf` is the window itself.

Outside a handler and outside a top level (for example, a direct call
from the main script), `MySelf` has no current control and raises the
runtime error *MySelf has no current control*; the error is
recoverable through `ErrorOccurred` / `ErrorInfo`.

`MySelf` is inherited along the call chain: a procedure called from a
handler sees the same `MySelf` as its caller. Handy to factor a
treatment across several controls.

---

## 7ter. Triggering an event by code: `TriggerEvent`

`TriggerEvent` (French `DéclencheÉvénement`) runs by code the handler of
a control's event, without going through an interaction. Useful when the
logic lives in the handler itself and must be re-run from code
(orchestration, tests).

```owl
TriggerEvent(BTN_Validate, evtClick)          // object form
TriggerEvent("BTN_Validate", evtClick)         // string form (name resolved at runtime)
TriggerEvent(SAI_Age, evtKeyDown, 65)          // event with a parameter
```

- **Target**: the control as an object (`BTN_Validate`) or its name as a string (indirection).
- **`evtXxx`**: an event constant. The `evt*` set covers the events of
  controls (`evtClick`, `evtDoubleClick`, `evtEntry`, `evtExit`, `evtEachChange`,
  `evtKeyDown`...) and follows the event catalog: every control event has
  its constant.
- **Parameters**: passed to the handler if the event expects one (for example the
  key code for `evtKeyDown`).

Three checks are done **at compile time**:

1. unknown target (object not declared, or string literal designating no control);
2. event absent from the targeted control type (for example a table event on a
   button);
3. invalid parameters (number or type), checked against the signature carried by the
   constant `evtXxx` — so checked even in string form.

`TriggerEvent` only serves to trigger an event. To share logic without
going through an event, extract a named procedure and call it directly.

---

## 7quater. The Table control: declaration and formatting

### Declaring a Table and its columns

A Table control groups columns declared as `Column` sub-blocks, nested
in the Table block. Each column carries its usual properties:
`Caption` (the header text), `Type` (the nature of the cells, via the
`colTypes` enumeration) and `Width` (in pixels).

```owl
TABLE_Directory is a Control Table
    PROPERTIES
        Stretch = True
    END

    COL_Name is a Column
        PROPERTIES
            Caption = "Name"
            Type    = colTypeText
            Width   = 220
        END
    END

    COL_Age is a Column
        PROPERTIES
            Caption = "Age"
            Type    = colTypeNumeric
            Width   = 100
        END
    END
END
```

A row is added with `TableAddLine`, one value per column in the order
of declaration. A cell is read by its column name:

```owl
TableAddLine(TABLE_Directory, "Ada", 36)
TableAddLine(TABLE_Directory, "Grace", 41)

sName  is a string  = TABLE_Directory[1].COL_Name     // "Ada"
nAge   is an integer = TABLE_Directory[2].COL_Age     // 41
nCount is an integer = TableCount(TABLE_Directory)    // 2
```

The single dot designates the column name (`TABLE_X.COL_A`); the
display properties are written with the double dot (see below). The
`owl explain Colonne` page details the properties of a column.

### Conditional formatting

The style of a Table (`TABLE_X..Style`) and that of a column
(`COL_X..Style`) describe a fixed appearance, decided at declaration.
To make a value stand out depending on its data — a negative balance,
a low stock, a missed deadline — a Table exposes two properties that
are written at runtime, row by row or cell by cell.

### The two properties

| property | French alias | effect |
|---|---|---|
| `..Color` | `..Couleur` | **text** color |
| `..BackgroundColor` | `..CouleurFond` | **background** color |

They accept anything that is a color: a named constant, `RGB(r, g, b)`
or a compact integer (cf. chapter *Colors*). They can be read back: a
cell that no write has targeted returns `Null`.

### The four writes

```owl
TABLE_Summary[nLine][nColumn]..Color = RGB(255, 255, 255)  // one cell
TABLE_Summary[nLine, nColumn]..Color = RGB(255, 255, 255)  // one cell (compact form)
TABLE_Summary.COL_Stock[nLine]..Color = RGB(255, 255, 255)  // one cell
TABLE_Summary[nLine]..Color           = RGB(255, 255, 255)  // the whole row
```

- **Cell, row first** — `TABLE_X[row][column]`: two numeric indices,
  the column being designated by its rank (1 for the first declared
  column).
- **Cell, compact form** — `TABLE_X[row, column]`: both coordinates
  inside the same brackets, separated by a comma. The column is
  designated by its rank **or** by its name (`TABLE_X[row, COL_A]`).
  Equivalent to the two-bracket form.
- **Cell, column first** — `TABLE_X.COL_A[row]`: the column is
  designated by its name, which avoids counting ranks and survives a
  reorganization of the columns. These forms target the same cell: what
  one writes the others read back.
- **Whole row** — `TABLE_X[row]`: a single index. All the columns of
  the row are tinted.

In all four forms, the property is written with the **double dot**:
`[1][2]..Color`. The single dot there designates a column name
(`TABLE_X.COL_A[1]`), never a property.

### The indices are expressions

This is the point that makes the feature usable: `row` and `column`
need not be literals. A loop variable, a procedure parameter, the
result of a call will do. Formatting is thus written in a loop that
walks the rows and decides the color from the displayed data.

```owl
PROCEDURE ApplyFormatting()
    nLine is an integer
    nBalance is an integer

    FOR nLine = 1 TO TableCount(TABLE_Summary)
        nBalance = TABLE_Summary[nLine].COL_Cash

        IF nBalance < 0 THEN
            TABLE_Summary[nLine][2]..Color           = RGB(255, 255, 255)
            TABLE_Summary[nLine][2]..BackgroundColor = RGB(200, 30, 30)
        END
    END
END
```

Setting a formatting does not modify the cell content:
`TABLE_X[row].COL_A` keeps returning the displayed value.

### What is not a cell designation

`TABLE_X[row].COL_A` designates the **value** of the cell, not the
cell itself: appending a property to it (`TABLE_X[row].COL_A..Color`)
is refused at compile time, with a message that recalls the two writes
above. A third index level (`TABLE_X[r][c][x]`) is refused for the same
reason.

---

## 7quinquies. Input control: input mask

A `Control Input` can constrain typing character by character through
its `InputMask` property (`MasqueSaisie`). The mask is a template with
**fixed positions**: each position of the text matches a position of
the mask and accepts only what that position allows.

Three characters are **metacharacters** (slots to fill):

| Metacharacter | Accepts |
|---|---|
| `9` | A digit (`0`–`9`) |
| `A` | A letter |
| `C` | A letter **or** a digit |

**Every other character is a literal**: it is displayed as is, at a
fixed position, and the user cannot modify it. The cursor jumps over it
while typing. A mask made only of literals therefore has no input slot;
`AFFF` for example is a letter (`A`) followed by the literal `FFF`.

Examples:

```owl
SAI_Date..InputMask = "99/99/9999"        // raw date: 8 digits, fixed /
SAI_Initials..InputMask = "AAA"           // exactly 3 letters
SAI_ItemCode..InputMask = "CCCCCC"        // 6 alphanumeric characters
SAI_Phone..InputMask = "+99 999 99 99 99"  // + and spaces as literals
```

The mask constrains the **input**. To drive only the *displayed
format* of a Date control without imposing a position-by-position
typing, use `DisplayMask` (`MasqueAffichage`) instead.

---

## 8. `RequestRefresh` — out of flow

This event has no fixed place in the cycle: it fires on demand, by a
call to `RequestUpdateUI` (`DemandeMiseAJourIHM`). To be implemented in
a window whose display depends on external sources (file, HTTP,
database) that may change.

```owl
FEN_Dashboard is a Window
    PROCEDURE RequestRefresh()
        LIB_Balance..Caption = FormattedBalance()      // re-reads the data source
    END
END
```

```owl
Open(FEN_Dashboard)
RecordSale(nAmount)
RequestUpdateUI()          // refreshes the display
```

`RequestUpdateUI` is called **with no parameter** and targets the
**current window**. Execution is **synchronous**: the `RequestRefresh`
event runs entirely before the call returns. With no open window, or if
the current window does not declare this event, the call has no effect
and no error.

---

## 9. Wiring a multi-file application

A real GUI application is rarely written in a single file. The OWL
canon sets a simple rule: **a source file contains only one window**,
and its rendering or logic helpers live **inside the `is a Window`
block**, not after `END`. The execution code (`Open`, shared state)
lives in a neighboring file.

Here is the minimal layout for a multi-file GUI project — a small
counter with a business engine in a dedicated class:

```
my_project/
    projet.owl          ← boundary + program (global state + startup)
    FEN_Counter.owl     ← window + helpers INSIDE the block
    cEngine.owl         ← business logic (OOP class)
```

```owl
// projet.owl — boundary, global state and startup point.
// The top-level code is the program: it runs last, after the init of
// the collections (the cEngine class below is a mere declaration, it
// does not run at startup).

oEngine is a cEngine                         // global to the project

Open(FEN_Counter)
```

```owl
// cEngine.owl — isolated business logic, no UI dependency

cEngine is a Class
    nValue is an integer = 0

    PROCEDURE Increment()
        object.nValue = object.nValue + 1
    END

    PROCEDURE Value() : integer
        RESULT object.nValue
    END
END
```

```owl
// FEN_Counter.owl — window alone, helpers INSIDE the block

FEN_Counter is a Window
    PROPERTIES
        Title       = "Counter"
        Width       = 240
        Height      = 120
        Layout      = dispVertical
        Padding     = 12
        Spacing     = 8
    END

    // Helper INSIDE the block. Addresses the sub-controls LIB_Value and
    // BTN_Plus declared further down — the order of declaration within
    // the block does not matter (helpers like controls).
    PROCEDURE RefreshDisplay()
        LIB_Value..Caption = "Value: " + oEngine.Value()
    END

    LIB_Value is a Control Label
        PROPERTIES
            Caption = "Value: 0"
            Stretch = True
        END
    END

    BTN_Plus is a Control Button
        PROPERTIES
            Caption = "+1"
            Width   = 80
        END
        PROCEDURE onClick()
            oEngine.Increment()
            RefreshDisplay()
        END
    END

    PROCEDURE EndInitialization()
        RefreshDisplay()
    END
END
```

A few rules to keep in mind:

- **`projet.owl` marks the project root**, includes the sub-folders **and
  carries the program**: the code of its block is the startup point,
  run last (after the init of the collections). Without it, a
  multi-file folder is not seen as a project (cf.
  `00_apercu_du_langage.md` §11).
- **Global state lives in `projet.owl`** (or another non-class file of
  the project). The top-level variables of `projet.owl` are **global to
  the project**: visible from any Window block.
- **The PROCEDURE helpers that address sub-controls live INSIDE the
  `is a Window` block**. Putting a `PROCEDURE Helper()` after the
  `END` of the window, in order to manipulate `LIB_Value` or
  `BTN_Plus`, is rejected at compile time: the window file is
  reserved for the declaration and its context helpers.
- **No cross-file helpers that address the controls of another
  window.** To share computation or logic code between several
  windows, go through a **class** (the `cEngine` of the example) — the
  class does not know the controls and stays reusable.
- **Refreshing the display = reassigning the property**. After a
  computation, copy the result into the control (`LIB_Value..Caption =
  …`, `IMG_View..Image = …`). The window redraws on the
  reassignment.

### Local GO mode: opening a window file directly

For a demo or a quick test of a window **that needs no external global
state**, running the window file directly is enough:

```
owl FEN_Counter.owl
```

OWL mounts the window and enters the event loop without going through
`projet.owl`. Handy to iterate on the visuals of an isolated window.
As soon as shared state or several orchestrated windows are needed, go
back to the project's `projet.owl`.

---

## 10. Common pitfalls

- **Body code at the top level**. `Trace(...)`, a procedure call or
  `LIB_Title..Caption = "x"` directly in the body are rejected with a
  warning and the line is skipped. A bare assignment (`n = 5`) triggers
  the sharper error *unknown property for Window*. All execution code
  goes in `EndInitialization` or an event handler.
- **`CloseAuthorization` with no `RESULT`**. The default is *authorized*.
  To cancel, write `RESULT False` explicitly.
- **`Closing` does not cancel**. A `RESULT False` inside it has no
  effect on the close in progress.
- **Persistent window variable**. Since it is a global, its value
  survives the close. Reinitialize in `EndInitialization` if you want a
  fresh state on every opening.
- **Misspelled event name**. `onCLick`, `OnClick` or `onClic` compile
  but do not fire — the procedure is never called. Check the exact
  spelling (`onClick`, `onDoubleClick`, ...).
- **`onClose` ≠ `Closing`**. `onClose` is the ComboBox event (close of
  the dropdown), not the window hook. For window cleanup, it is
  `Closing`.
- **Accessing a control without opening its window**. A control only
  exists in a mounted window. `SAI_Name..Value` before any `Open`/
  `OpenChild`/`OpenSibling` of its window fails: you must open
  first.
- **Confusing two instances**. Two openings of the same window = two
  separate states. Accesses always target the current instance (the
  last opened); a `Close()` makes the previous one current again.

---

## Further reading

| Topic | Page |
|---|---|
| General language overview | `00_apercu_du_langage.md` |
| Control flow (`IF`, `FOR`, ...) | `03_controle_de_flux.md` |
| Lifecycle of a Report | `16_lifecycle_etats.md` |
| Colors (`RGB`, named constants) | `29_couleurs.md` |
| Code conventions | `10_conventions_de_code.md` |
