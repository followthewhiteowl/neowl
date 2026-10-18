# Rich text

An extended Markdown dialect (colors, font size, alignment) is
interpreted on the labels of windows and Reports. It lets strings be
formatted without manually driving the Font / Color properties of the
controls.

## Activation

Rich text is only interpreted on controls whose `DisplayFormat`
property is `formatRichText`:

```owl
LIB_X is a Label
    PROPERTIES
        Caption         = "**Important**: [c:LightRed]error[/c]"
        DisplayFormat   = formatRichText
    END
END
```

Default: `formatText` (literal text). No surprise for ordinary strings
(window `Title`, keyboard shortcuts, etc.) which stay literal.

Controls that support the property:

- `Label` (Window)
- Multi-line `Input` (`InputType = typeInputMultiline`) — display only
- `Label` of a Report (PDF generation)

## Inline (fragments)

The CommonMark Markdown subset is taken as-is:

| Effect | Syntax |
|---|---|
| Bold | `**…**` |
| Italic | `*…*` or `_…_` |
| Underline | `__…__` |
| Strikethrough | `~~…~~` |
| Inline code | `` `…` `` |
| Link | `[text](url)` |

**Free nesting**: `**[c:red]red bold[/c]**` produces bold + color.

## Markdown extensions

Tags `[tag:val]…[/tag]` — the syntax is chosen so as not to conflict
with Markdown links `[text](url)`.

| Tag | Effect | Example |
|---|---|---|
| `c` | Text color | `[c:DarkRed]error[/c]` |
| `cf` | Background color | `[cf:TooltipYellow]highlight[/cf]` |
| `s` | Font size (pt) | `[s:14]big[/s]` |
| `p` | Font name | `[p:iPoliceArial]…[/p]` |
| `esp` | Letter spacing (px) | `[esp:2]SPACED[/esp]` |
| `j` | Paragraph alignment | `[j:justify]…[/j]` |

### Colors: 3 accepted syntaxes

```
[c:DarkRed]         → named color constant
[c:#FF6600]         → direct hex, 6 digits
[c:#F60]            → short direct hex, 3 digits (expanded to #FF6600)
```

### Alignment modes (`j`)

| FR | EN |
|---|---|
| `gauche` | `left` |
| `droite` | `right` |
| `centre` | `center` |
| `justifie` | `justified`, `justify` |

## Blocks (paragraphs)

Taken from standard Markdown:

| Construction | Syntax |
|---|---|
| Bulleted list | `- …` lines |
| Numbered list | `1. …` lines |
| Tables | GitHub-Flavored Markdown (pipes `\|`) |
| Blockquote | `> …` lines |
| Code block | fenced by triple backtick |

## Escaping

`\[` and `\]` for literal characters. Other Markdown characters follow
standard CommonMark escaping (`\*`, `\_`, etc.).

## Invalid tag → literal

If a tag is not recognized (e.g. `[thing:whatever]`) or if the value
does not resolve (e.g. `[c:NonexistentColor]`), the preprocessor
**emits the text as-is** with no failure or warning.

## Points of attention

- **Nesting the same tag at several levels**: a closing tag always
  targets the last opening of the **same** tag.
  `[c:red]A [c:blue]B[/c] C[/c]` treats the first closing as closing
  `[c:blue]`, not `[c:red]`. A single nesting level for a given tag is
  enough in practice.
- **Color via `RGB(r, g, b)` inside `[c:…]`**: use the hex form
  (`[c:#FF6600]`) or a named color constant.
- **Hex `[c:#…]`**: case-sensitive — `#FF6600` must be uppercase in
  the source.
- **Multi-line input**: rich text is rendered in display; WYSIWYG
  editing (contextual **B**/*I*/__U__ toolbar) will be added later.

---

## Further reading

| Topic | Sheet |
|---|---|
| Lifecycle of fields | `15_lifecycle_champs.md` |
| Lifecycle of Reports | `16_lifecycle_etats.md` |
| Code conventions | `10_conventions_de_code.md` |
| Naming prefixes (`LIB_`, `SAI_`…) | `12_prefixes_de_nommage.md` |
