# Modern layouts

The language offers a semantic layout model inspired by modern
frameworks (Flutter, SwiftUI, Jetpack Compose, CSS Flexbox/Grid). In
standard mode, the children of a container are positioned by alignment
and stretch rules — `X` / `Y` are not required. A pixel-perfect
`dispFree` mode remains available as opt-in for the cases that warrant
it (cf. § 6).

Keywords involved: `Layout`, `Container`, `Stretch`, `Alignment`,
`Margin`, `Padding`, `Spacing`, `AlignH`, `AlignV`, `Columns`, `Rows`,
`ColumnSpan`, `RowSpan`, `MinWidth/MaxWidth`, `MinHeight/MaxHeight`.

## 1. Concept

Every container (Window or `Container`) has a **`Layout`** property
that decides how its direct children are organized:

| Value             | Effect | CSS equivalent |
|-------------------|-------|----------------|
| `dispVertical`    | Stacks in a column (default) | `flex-direction: column` |
| `dispHorizontal`  | Aligns in a row | `flex-direction: row` |
| `dispGrid`        | Grid of `Columns` × `Rows` | `display: grid` |
| `dispFree`        | No layout — absolute `X` / `Y` | `position: absolute` |

**Fundamental rule**: `X` and `Y` are **forbidden** on a child control
**unless** its direct parent is in `Layout = dispFree`. The compiler
checks this rule and emits an error otherwise — interfaces stay
responsive by default.

`X` and `Y` on the **Window** itself stay valid — they set the
window's position on screen (`-1` = centered).

**Important**: if `Layout` is not specified on the Window, the default
is **`dispVertical`**. Children with `X`/`Y` will then produce a
**compile error**. For explicit pixel-perfect mode, add
`Layout = dispFree` to the Window's PROPERTIES.

## 2. The `Container` control

`Container` is a generic grouping box used to nest layouts. The modern
equivalent of the HTML `<div>` or the React Native `View`.

```owl
FEN_Form is a Window
    PROPERTIES
        Layout     = dispVertical
        Padding    = 12
        Spacing    = 8
    END

    LIB_Title is a Control Label
        PROPERTIES
            Caption = "Enter the user"
        END
    END

    SAI_Name is a Control Input
        PROPERTIES
            Stretch = True
        END
    END

    BAR_Buttons is a Control Container
        PROPERTIES
            Layout = dispHorizontal
            AlignH = alignEnd
        END

        BTN_OK is a Control Button
            PROPERTIES
                Caption = "OK"
            END
        END

        BTN_Cancel is a Control Button
            PROPERTIES
                Caption = "Cancel"
            END
        END
    END
END
```

Three nested layouts: the vertical Window, a horizontal Container for
the buttons. No `X`/`Y`. Readable, resizable.

## 3. Container properties

| Prop | Type | Default | Description |
|------|------|--------|-------------|
| `Layout`       | enum  | `dispVertical` | Layout mode |
| `Padding`      | int   | 0 | Uniform inner margin (px) |
| `PaddingTop/Bottom/Left/Right` | int | inherits `Padding` | Per-side override |
| `Spacing`      | int   | 6 | Gap between successive children |
| `AlignH`       | enum  | (free) | Default horizontal alignment of children |
| `AlignV`       | enum  | (free) | Default vertical alignment |
| `Columns`      | int   | 1 | Number of columns (`dispGrid` only) |
| `Rows`         | int   | computed | Number of rows (optional) |

`AlignH/AlignV` values: `alignStart`, `alignCenter`, `alignEnd`, `alignStretch`.

## 4. Child properties

Every control (Label/Button/Input/Table/Container) accepts these props
when its parent is in layout mode:

| Prop | Type | Default | Description |
|------|------|--------|-------------|
| `Stretch`      | bool  | False | Takes all remaining space on the main axis |
| `Margin`       | int   | 0 | Uniform outer margin |
| `TopMargin/BottomMargin/LeftMargin/RightMargin` | int | inherits `Margin` | Per-side override |
| `Alignment`    | enum  | inherits parent | Override of the cross alignment for this control |
| `ColumnSpan`   | int   | 1 | Column merge in `dispGrid` |
| `RowSpan`      | int   | 1 | Row merge |
| `Width`        | int   | (auto) | Fixed width; constrains the size on the main axis in `dispHorizontal` |
| `Height`       | int   | (auto) | Fixed height; constrains the size on the main axis in `dispVertical` |
| `MinWidth/MaxWidth` | int | -1 | Width bounds |
| `MinHeight/MaxHeight` | int | -1 | Height bounds |

A fixed `Height` (or `Width`) on a child sets its size along the main
axis of the parent: in `dispVertical`, `Height` locks the control's
height; in `dispHorizontal`, `Width` locks its width. On the cross
axis, the size stays governed by the alignment (`alignStretch` fills
the whole cross dimension, the other values take the natural size).
Combine a fixed `Height`/`Width` with `MinHeight/MaxHeight` /
`MinWidth/MaxWidth` to bound a size without fully locking it.

These properties are **declared values**. Once the window is mounted,
`..Width` / `..Height` / `..X` / `..Y` of a control read at runtime and
return its **actual geometry**, which may differ from what is declared
(stretched control, size computed by the layout). Reading this actual
geometry is detailed in `15_lifecycle_champs.md` §6ter, and its
**writing** (constrained-persistent size, position rejected in
automatic layout) in `15_lifecycle_champs.md` §6quater.

## 5. Distribution of space on the main axis

By default, each child takes its natural size (or the fixed size
imposed by `Height`/`Width`) and the children **stack at the start** of
the main axis:

- in `dispVertical`, the children stack **at the top**; the vertical
  surplus stays empty at the bottom;
- in `dispHorizontal`, the children stack **at the left**; the
  horizontal surplus stays empty at the right.

To consume this surplus, mark a child `Stretch = True`: that control
then absorbs all the remaining space on the main axis (a `Container`
with `Stretch = True` expands to fill the zone). Several children with
`Stretch = True` share the surplus equally.

`alignStretch` does **not** play the same role: it is a **cross**
alignment. It makes a control fill the dimension perpendicular to the
main axis (the width in `dispVertical`, the height in `dispHorizontal`)
**without** consuming space along the main axis. For a control to take
the remaining space in the direction of the flow, use `Stretch = True`;
for it to occupy the whole cross width (or height), use `alignStretch`.

```owl
FEN_Stack is a Window
    PROPERTIES
        Layout = dispVertical
    END

    LIB_Top is a Control Label
        PROPERTIES
            Caption = "At the top, natural size"
        END
    END

    SAI_Middle is a Control Input
        PROPERTIES
            Alignment = alignStretch
        END
    END
END
```

`LIB_Top` and `SAI_Middle` stick to the top of the window; `SAI_Middle`
occupies the whole width thanks to `alignStretch`, but the surplus
vertical space stays empty at the bottom (no child is `Stretch = True`).

## 6. A few use cases

### Expandable list (element that takes the remaining space)

```owl
FEN_List is a Window
    PROPERTIES
        Layout = dispVertical
    END

    BAR_Filter is a Control Container
        PROPERTIES
            Layout = dispHorizontal
        END
    END

    TBL_Rows is a Control Table
        PROPERTIES
            Stretch = True
        END
    END
END
```

`Stretch = True` on `TBL_Rows` consumes the space remaining between the
filter bar (fixed size) and the bottom of the window.

### Grid (calculator)

```owl
FEN_Calc is a Window
    PROPERTIES
        Layout     = dispGrid
        Columns    = 4
        Spacing    = 4
    END

    BTN_0 is a Control Button
        PROPERTIES
            Caption   = "0"
            ColumnSpan = 2
        END
    END

    BTN_Dot is a Control Button
        PROPERTIES
            Caption = "."
        END
    END

    BTN_Eq is a Control Button
        PROPERTIES
            Caption = "="
        END
    END
END
```

The other buttons (1-9, operators) follow the same structure; the
engine places each control in the next free cell, respecting
`ColumnSpan`.

### Pixel-perfect mode (opt-in)

When the use warrants it (decorative badge, splash screen done exactly
to the pixel, floating control):

```owl
FEN_Card is a Window
    PROPERTIES
        Layout  = dispFree
        Width   = 300
        Height  = 200
    END

    LIB_Badge is a Control Label
        PROPERTIES
            X       = 240
            Y       = 10
            Caption = "NEW"
        END
    END
END
```

`dispFree` is an explicit opt-in and **should not** be the default.

## 7. Special case: Reports

Reports (paginated A4 / A5 documents) **do not follow** this layout
model. They keep an absolute `X` / `Y` system organized in bands
(DocumentHeader, PageHeader, Body, PageFooter, DocumentFooter) that fits
the constraints of PDF publishing better.

---

## Further reading

| Topic | Sheet |
|---|---|
| Lifecycle of controls | `15_lifecycle_champs.md` |
| Lifecycle of Reports (bands, `X`/`Y` model) | `16_lifecycle_etats.md` |
| Control groups (broadcast of a property) | `20_groupes_de_champs.md` |
| Rich text (extended Markdown for labels) | `19_texte_riche.md` |
| Code conventions | `10_conventions_de_code.md` |
