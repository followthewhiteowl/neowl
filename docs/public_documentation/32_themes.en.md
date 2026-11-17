# Themes and named styles

A **theme** groups named, typed and reusable styles in an `is a Theme` block
(French form `est un Thème`). A project designates one; each control draws its
appearance from it via the `Style` property. A style only redefines what must
change — the rest keeps the control's default value, so that adding a property
later breaks no existing theme.

## Declaring a theme

The block carries the theme name and chains named styles up to `END` / `FIN`.
A single `is a Theme` block is allowed per file; by convention it lives in a
`theme.owl` file at the project root.

```owl
ThemeDemo is a Theme
	primaryButton is a styleButton = {
		OuterFrame = { BackgroundColor = Blue }
		Caption    = { Color = White }
	}

	dangerButton is a styleButton = {
		OuterFrame = { BackgroundColor = Red }
		Caption    = { Color = White }
	}

	compactInput is a styleInput = {
		InputZone = { BackgroundColor = LightYellow }
	}
END
```

Each entry declares a variable typed by a **style descriptor** and initialized
by a `{ ... }` literal (cf. `17_init_litterale.md`).

## The style descriptors

To each type of stylable control corresponds a descriptor. The most common
ones, with their French form (both forms are accepted):

| Descriptor | French form | Target control | Main sub-elements |
|---|---|---|---|
| `styleButton` | `styleBouton` | Button | `Caption`, `Image`, `OuterFrame` |
| `styleInput` | `styleSaisie` | Input | `InputZone`, `Caption`, `OuterFrame` |
| `styleLabel` | `styleLibellé` | Label | `Caption`, `OuterFrame` |
| `styleContainer` | `styleConteneur` | Container | `OuterFrame` |

The other controls follow the same scheme (`styleTable`, `styleCombobox`,
`styleList`, `styleGauge`, `styleSwitch`, `styleSelector`…).

Each sub-element is itself a `{ ... }` literal:

- `OuterFrame` / `CadreExtérieur` — the box: `BackgroundColor` / `CouleurFond`,
  `Border` (color, thickness, radius), `Shadow`, `Gradient`, `BackgroundImage`
  / `ImageFond` (background image: `Source` + `Framing`), paddings.
- `Caption` / `Libellé` — the text: `Color` / `Couleur`, `Font` / `Police`…
- `InputZone` / `ZoneSaisie` — the editable zone of an input.

A style mentions only the sub-elements and properties it changes; everything
else keeps its default value (cf. `29_couleurs.md` for colors).

For a control made of several parts, like the Table control, a global font and
color set on the control (`TBL..Font..Size`, `TBL..Font..Color`, …) apply as
the base for rendering all parts (cells and headers). The detailed style then
overrides this base per sub-element — `ColumnTitle` (the column header),
`RowLabel` (the cell text), `SelectedRow`, etc. Both coexist: the control's
font sets the default, the sub-element refines it. The font of a part is set
via its sub-element:

```owl
clientsTable is a styleTable = {
	ColumnTitle  = { Font = { Name = "Arial", Size = 11, Bold = True } }
	RowLabel     = { Font = { Size = 10 }, Color = DarkGray }
	SelectedRow  = { Color = White }
}
```

A Column can locally override these same sub-elements via its own `styleColumn`
(cf. `owl explain styleColumn`).

### Per-state overrides

An interactive sub-element accepts a `States` member that redefines the
appearance according to the control's state: `Hover`, `Pressed`, `Focus`,
`Grayed`, `ReadOnly` (the available states depend on the control).

```owl
primaryButton is a styleButton = {
	OuterFrame = {
		BackgroundColor = Blue
		States = {
			Hover   = { BackgroundColor = LightBlue }
			Pressed = { BackgroundColor = DarkBlue }
		}
	}
	Caption = { Color = White }
}
```

### Background image

The `OuterFrame` accepts a **background image** through the `BackgroundImage` /
`ImageFond` sub-element, with two members: `Source` / `Source` (file path,
absolute or relative to the executable directory) and `Framing` / `Cadrage`
(fill mode: `cadCenter`, `cadStretch`, `cadProportional`, `cadCover`).

```owl
heroCard is a styleContainer = {
	OuterFrame = {
		BackgroundColor = RGB(10, 12, 16)
		BackgroundImage = { Source = "images/bg.png", Framing = cadProportional }
	}
}
```

The background image is painted **above** `BackgroundColor` and the `Gradient`,
and **below** the control's content — the stacking order of an `OuterFrame`'s
background layers is `BackgroundColor → Gradient → BackgroundImage → content`. A
missing or unreadable file simply drops the image layer (the solid fill or
gradient shows through), without interrupting the program.

## Loading a custom font

`Police..Nom` (Font..Name) accepts a family shipped with the application
(Liberation Sans, Liberation Mono…) or a family made available at startup. To
use a font provided as a `.ttf`/`.otf` file, load it first with `FontLoad`
(French form `PoliceCharge`): the function returns the **family name** to reuse
as the value of `Police..Nom`.

```owl
sFamily is a string = FontLoad("fonts/JetBrainsMono-Regular.ttf")
IF sFamily <> "" THEN
	SAI_Code..Police..Nom = sFamily
END
```

On failure — missing file or unrecognised format — `FontLoad` returns an empty
string and reports an error (`ErrorOccurred`, `ErrorInfo()`), without
interrupting the program. A loaded font stays available for the whole run.

## Applying a style to a control

In a control's `PROPERTIES`, the `Style` property receives the named style via
the `theme.` prefix:

```owl
BTN_Validate is a Control Button
	PROPERTIES
		Caption = "Validate"
		Style   = theme.primaryButton
	END
END
```

The `theme.` prefix designates the project's active theme (the one it has
attached, below). The control then inherits all the style's properties.

## Styling a window

A window is styled like a control: it carries a `Style` property typed
`styleContainer` (French form `styleConteneur`), the same descriptor as the
Container. This descriptor has a single sub-element, `OuterFrame` /
`CadreExtérieur`, which describes the window background: `BackgroundColor`,
paddings (`TopPadding`, `BottomPadding`, `LeftPadding`, `RightPadding`),
`Border`, `Shadow`, `Gradient` and `BackgroundImage`.

The `Style` block is written in the window's `PROPERTIES`, as a `{ ... }`
literal, and mentions only what changes:

```owl
FEN_Home is a Window
	PROPERTIES
		Title = "Home"
		Style = {
			OuterFrame = {
				BackgroundColor = LightGray
				TopPadding = 12
				Border = { Color = DarkGray, Thickness = 1 }
			}
		}
	END
END
```

The window can also draw its appearance from a named theme style: like any
control, its `Style` property accepts `theme.<name>` pointing to a style typed
`styleContainer` declared in the theme block.

## Attaching the theme to the project

A project designates its active theme via the `Theme` property (French form
`Thème`) of its `is a Project` block, pointing to the **name of the theme
block**:

```owl
MyThemedApp is a Project
	PROPERTIES
		Description = "Theme demo"
		Languages = [languageFrench]
		Theme = ThemeDemo
	END
END
```

The `theme.owl` declares `ThemeDemo is a Theme`, the `projet.owl` designates
it, and every control that writes `Style = theme.<name>` receives its
appearance. Cf. `24_projet.md` for the project block.

## Generating a skeleton

`owl new theme <Name>` emits a sample theme ready to complete:

```
owl new theme MyTheme
```

In VS Code, the **"New element"** command offers a **Theme** card that does the
same and opens the created file.

## Current limitations

- **A single `is a Theme` block per file.** A second block in the same file is
  refused at compilation.
- A theme is only **active** if it is designated by the project
  (`Theme = <name>`). A theme block that is present but not designated compiles
  without error; its styles simply do not apply.

---

## Further reading

| Topic | Sheet |
|---|---|
| Colors and transparency | `29_couleurs.md` |
| Literal initialization `{ ... }` | `17_init_litterale.md` |
| Lifecycle of fields | `15_lifecycle_champs.md` |
| The project block | `24_projet.md` |
