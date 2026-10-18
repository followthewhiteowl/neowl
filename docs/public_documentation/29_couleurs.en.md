# Colors

A color is declared in several interoperable ways and is handled like an
ordinary value. The composite type `Color` (French form `Couleur`) carries
the components as well as the transparency channel (`Opacity` / `Opacité`).

## The four declaration forms

The four forms below all produce a value assignable to a color property
(`BackgroundColor`, `Color`, `ProgressBarColor`…).

### 1. Named constant

```owl
c is a Color = Red
```

A catalog of predefined colors is available: `White` / `Blanc`,
`Black` / `Noir`, `Red` / `Rouge`, `Green` / `Vert`, `Blue` / `Bleu`,
`Gold` / `JauneOr`, the `Light…` / `…Clair`, `Dark…` / `…Foncé`,
`Pastel…` / `…Pastel` variants, as well as `Transparent`. The catalog also
includes sentinel constants (`DefaultColor` / `CouleurDéfaut`,
`iSystem` / `iSystème`) described under "Sentinel colors" below.

### 2. `RGB(red, green, blue)` / `RVB(rouge, vert, bleu)`

```owl
c is a Color = RGB(255, 128, 0)
```

Each component ranges from 0 to 255. The French form `RVB` is equivalent.

### 3. Compact integer

```owl
c is a Color = 33023
```

The integer value encodes the three components: `red + green * 256 +
blue * 65536`. The example above is therefore identical to `RGB(255, 128, 0)`
(`255 + 128 * 256 = 33023`).

### 4. Inline literal `{ ... }`

```owl
c is a Color = { Red = 255, Green = 128, Blue = 0, Opacity = 200 }
```

Only this form lets the opacity be set at declaration time. Absent fields
take their default value (`Opacity` is 255, i.e. opaque). See
`17_init_litterale.md` for the general syntax of the instance literal.

## Sentinel colors (default fallback)

Beyond the fixed-value colors, the catalog exposes **sentinel** constants:
they do not carry frozen RGB components but indicate a render mode. The final
render then depends on the property the sentinel is assigned to.

| Constant | French form | Role |
|---|---|---|
| `DefaultColor` | `CouleurDéfaut` | The property falls back to its own render default |
| `iSystem` | `iSystème` | The property takes the system / theme color |
| `Transparent` | `Transparent` | No background painted: what is behind shows through |

### Fallback per property, not one shared color

`DefaultColor` means "render this property with **its** default", not "this
specific color". The fallback is therefore computed **per property**: two
different properties assigned `DefaultColor` do not take the same color, each
falls back to the default that is its own.

```owl
LIB_Card..BackgroundColor = DefaultColor   // control's default background
LIB_Card..Color           = DefaultColor   // default text color
```

In this example, `BackgroundColor` (the background) and `Color` (the text) do
**not** become identical: the background falls back to the control's default
background, the text falls back to the default text color. There is no single
shared color toward which `DefaultColor` would make every property converge.

`iSystem` follows the same per-property fallback principle, delegating the
choice to the system or the active theme rather than to the control's default.

## The `Color` type

The type exposes eight properties, all integers. The English form of each name
is shown for reference (both forms are accepted).

| Property | French form | Range | Role |
|---|---|---|---|
| `Red` | `Rouge` | 0–255 | Red component |
| `Green` | `Vert` | 0–255 | Green component |
| `Blue` | `Bleu` | 0–255 | Blue component |
| `Hue` | `Teinte` | 0–240 | Hue |
| `Saturation` | `Saturation` | 0–240 | Saturation |
| `Luminance` | `Luminosité` | 0–240 | Luminance |
| `Opacity` | `Opacité` | 0–255 | Alpha channel: 0 = transparent, 255 = opaque |
| `Color` | `Couleur` | integer | Compact integer value (default property) |

`Opacity` is independent of `Color` (which only encodes the three visible
components). Assigning an integer or a named constant to a `Color` variable
automatically decomposes the components; the opacity then stays at 255.

### Mutation

The properties are read and written after declaration:

```owl
c is a Color = RGB(255, 128, 0)
c..Opacity = 128           // becomes semi-transparent
c..Red     = 200           // adjust one component
```

## Translucent background (glassmorphism)

Setting a `Color` with `Opacity` below 255 on a background property makes that
background translucent: what is behind shows through. This is the principle of
glassmorphism ("frosted glass" surface).

```owl
FEN_Glass is a Window
    PROPERTIES
        Title       = "Translucent background"
        Width       = 360
        Height      = 220
        Layout      = dispFree
        BackgroundColor = Red
    END

    // White at 50% opacity: the red background shows through → pink.
    glassLight is a Color = { Red = 255, Green = 255, Blue = 255, Opacity = 128 }

    LIB_Card is a Control Label
        PROPERTIES
            X       = 40
            Y       = 40
            Width   = 280
            Height  = 120
            Caption = "Translucent card"
        END

        LIB_Card..BackgroundColor = glassLight
    END
END
```

An opaque background (opacity 255) is still rendered identically: transparency
only comes into play below 255.

## Current limitations

Two limitations are worth knowing to avoid surprises.

### The `{ ... }` literal requires a named variable

The inline color literal is only recognized in a **typed declaration**
(`c is a Color = { ... }`). It is **not** accepted directly in a color
property assignment, nor nested inside another literal:

```owl
// ✗ not supported
LIB_Card..BackgroundColor = { Red = 255, Opacity = 128 }
myStyle is a Style = { OuterFrame = { BackgroundColor = { Red = 255, Opacity = 128 } } }

// ✓ workaround: go through a named Color variable
glass is a Color = { Red = 255, Opacity = 128 }
LIB_Card..BackgroundColor = glass
```

### Scope of background opacity

The opacity of a translucent background is applied to: the window, a container
(via its Style), a generic control, the input and label bricks, and gradients.

It is **not yet** taken into account on: table rows (even / odd), the selected
row, and check marks / bullets. On those surfaces, a translucent color is
treated as opaque.

---

## Further reading

| Topic | Sheet |
|---|---|
| Literal initialization `{ ... }` | `17_init_litterale.md` |
| Lifecycle of fields | `15_lifecycle_champs.md` |
| Type system and coercions | `01_types_et_coercions.md` |
