# Internationalization

OWL treats multilingual text as a **native language type**, not as an
external library or a separate file format. Translations live **inside
the `.owl` file**, next to the string they describe. No `.po`/`.ts`/`.json`
file to keep in sync.

Three pillars structure the API:

| Pillar | Surface |
|---|---|
| Two first-class types | `multilingualString`, `language` |
| BCP 47 indexing (RFC 5646) | enum `languageXxx` (34 constants) or free string tag |
| Automatic two-way coercion | `string` ⇄ `multilingualString` depending on context |

> **Architectural rationale**: dedicated architecture decision (first-class multilingual type, BCP 47).

## Types

### `multilingualString`

Internal dictionary tag → string. Indexable by a `languageXxx` constant
(`m[languageFrench]`) or by a free BCP 47 tag (`m["fr-CH"]`).

```owl
m is a multilingualString
```

Default init: empty dictionary.

### `language`

Scalar alias: a typed reference to a language, stored as a BCP 47 tag
(`"fr-FR"`, `"en-US"`, `"zh-CN"`...). Comparable like a string, passable
as a parameter, returnable from a function.

```owl
lng    is a language
lngFR  is a language = languageFrench        // tag "fr-FR"
lngCH  is a language = "fr-CH"               // free tag accepted
```

Default init: empty string.

### Enum `languageXxx`

34 constants — 30 main languages + 4 common regional variants.

**Convention "default without suffix = main variant"**:

| Constant | BCP 47 tag |
|---|---|
| `languageFrench` | `"fr-FR"` |
| `languageEnglish` | `"en-US"` |
| `languageGerman` | `"de-DE"` |
| `languagePortuguese` | `"pt-PT"` |
| `languageChineseSimplified` | `"zh-CN"` |

Regional variants carry their explicit suffix:
`languageFrenchCanada` (`"fr-CA"`),
`languageEnglishUK` (`"en-GB"`),
`languagePortugueseBrazil` (`"pt-BR"`),
`languageSpanishMexico` (`"es-MX"`).

Full list via `owl list constants | grep language`.

## Syntax

### Inline literal `{ ... }`

Compact form to initialize a `multilingualString`. The separators
between entries are **comma** or **newline** (mixing OK).

```owl
m is a multilingualString = {
    languageFrench:  "Bonjour",
    languageEnglish: "Hello",
    languageGerman:  "Hallo"
}
```

The literal **must** have an explicit target type: typed declaration,
`PROPERTIES` DSL on a property typed `multilingualString`, or argument
of a native function that expects a `multilingualString`. No inference
from the expression — this is typing by target context, consistent with
the language's "no implicit inference" pillar.

```owl
// Rejected: no target type.
Info({languageFrench: "Bonjour"})       // compile error

// OK: explicit target type.
m is a multilingualString = {languageFrench: "Bonjour"}
Info(m)
```

### Values on multiple lines

The value of an entry is any string expression: both multiline string
forms of the language apply directly.

Bracketed block — raw content, no interpolation. The closing `]` carries
the literal's punctuation (`],` between two entries, `]}` for the last).

```owl
m is a multilingualString = {
    languageFrench: [
Ligne 1
Ligne 2
    ],
    languageEnglish: "Hello"
}
```

Backtick string — multiline as well, with interpolation `[%expression%]`
active.

```owl
name is a string = "Alice"
m is a multilingualString = {
    languageFrench:  `Bonjour [%name%],
bienvenue.`,
    languageEnglish: `Hello [%name%],
welcome.`
}
```

In both cases, a source line break is worth `LF`. Details of the string
forms in `00_apercu_du_langage.md`, section "Strings".

### Read/write indexing

Reading and writing by a `languageXxx` constant or by a free BCP 47 tag.
Both forms are equivalent internally.

```owl
m is a multilingualString = {languageFrench: "Bonjour"}

// Read
sFR is a string = m[languageFrench]         // "Bonjour"
sFR2 is a string = m["fr-FR"]               // "Bonjour" (same thing)

// Write
m[languageEnglish] = "Hello"
m["zh-CN"]         = "你好"                  // free tag
m["fr-jura"]       = "Bondjoû"               // local dialect — accepted
```

Free tags let you store a local dialect, a fine regional variant or a
custom identifier without modifying the enum.

Tag indexing is **case-insensitive**: tags are handled in canonical form
(language in lowercase, region in uppercase). `m["en-gb"]`, `m["EN-GB"]`
and `m[languageEnglishUK]` therefore refer to the same `"en-GB"` entry.

## Automatic two-way coercion

OWL automatically coerces between `string` and `multilingualString`
depending on the usage context. The developer never writes a manual
conversion call.

### `multilingualString → string` at use

Any function that expects a `string` and receives a `multilingualString`
resolves through the **current runtime language** (`Language()`). Covers
`Info`, `Trace`, concatenation, display in UI controls, etc.

```owl
m is a multilingualString = {
    languageFrench:  "Bonjour",
    languageEnglish: "Hello"
}
Info(m)                          // shows "Bonjour" if Language() = "fr-FR"
Language(languageEnglish)
Info(m)                          // shows "Hello"

s is a string = "Salut " + m     // concat: uses the current language
```

### `string → multilingualString` at assignment

Assigning a `string` to a variable typed `multilingualString` fills
**only the project's source language** (see `SourceLanguage()`). Memory
saving — the fallback handles the rest.

```owl
m is a multilingualString = "Bonjour"
// equivalent to: m = {languageFrench: "Bonjour"}  in a fr-FR source project

Info(m[languageFrench])          // "Bonjour"
Info(m[languageEnglish])         // "Bonjour" (source language fallback)
```

## Source language fallback

Reading a missing key → falls back to the **project's source language**
(`SourceLanguage()`, `"fr-FR"` by default). If the source language is
itself absent from the dictionary, returns an empty string.

```owl
m is a multilingualString = {
    languageFrench:  "Bonjour",
    languageEnglish: "Hello"
}

Info(m[languageGerman])          // "Bonjour" — fr-FR fallback
Info(m["fr-CH"])                 // "Bonjour" — fr-FR fallback
```

No hierarchical BCP 47 cascade: a `"fr-CH"` lookup miss does **not** try
an intermediate `"fr"` before the fallback. Simple, predictable
behavior.

## Runtime functions

| Function | Effect |
|---|---|
| `Language()` | Returns the current BCP 47 tag |
| `Language(<language>)` | Sets the current language, returns the previous value |
| `SourceLanguage()` | Returns the project's source language |
| `SystemLanguage()` | Detects the OS language, returns a BCP 47 tag |
| `LanguageWritingDirection(<language>)` | Returns `sensLeftRight` or `sensRightLeft` |
| `LanguageList()` | Returns the array of the project's active languages |

### `Language()` — getter and setter

With no argument, returns the current language. With an argument
(`languageXxx` constant or free tag), sets the language and returns the
previous value — handy for a temporary switch.

```owl
cur is a string = Language()                 // "fr-FR" by default
previous is a string = Language(languageEnglish)
// ... section in English ...
Language(previous)                            // restores the previous language
```

### `SystemLanguage()` — OS detection

Detects the OS language cross-platform (Windows:
`GetUserDefaultLocaleName`, macOS / POSIX: `LANG`/`LC_ALL`). Falls back
to `"en-US"` if detection fails (missing variable, C locale).

```owl
Language(SystemLanguage())       // aligns the runtime language with the OS
```

### `LanguageWritingDirection()` and the `writingDirection` enum

Returns a language's writing direction: `sensRightLeft` (RTL) for
Arabic, Hebrew, Persian and Urdu; `sensLeftRight` (LTR) otherwise. The
enum has a 3rd value `sensDefault` meaning "inherit the writing
direction of the current language" — useful on the `..TextDirection`
properties of UI controls.

```owl
dir is an integer = LanguageWritingDirection(languageArabic)
IF dir = sensRightLeft THEN
    Trace("Right-to-left language")
END
```

### `SourceLanguage()` and `LanguageList()`

`SourceLanguage()` returns the language in which texts were originally
entered. `LanguageList()` returns the project's active languages. Both
are **project parameters**; a future project configuration sheet will
make these values adjustable.

## Multilingual UI controls

Several textual properties of UI controls are **natively typed
`multilingualString`**: window titles, captions and tooltips of
buttons/labels/inputs/combos/radio groups/switches/loopers/tables,
column captions, report captions.

The developer can therefore type the dictionary directly in
`PROPERTIES`:

```owl
FEN_Article is a Window
    PROPERTIES
        Title = {
            languageFrench:  "Article",
            languageEnglish: "Item",
            languageGerman:  "Artikel"
        }
    END

    BTN_Confirm is a Control Button
        PROPERTIES
            Caption = {
                languageFrench:  "Valider",
                languageEnglish: "Confirm"
            }
            Tooltip = "Cliquez pour valider"   // plain string OK too
        END
    END
END
```

The plain string form (`Tooltip = "Cliquez..."`) stays accepted: auto
coercion fills the source language.

## Teaching examples

### Internationalized error message

```owl
PROCEDURE ValidateEmail(sEmail is a string)
    errMsg is a multilingualString = {
        languageFrench:  "Adresse email invalide.",
        languageEnglish: "Invalid email address.",
        languageGerman:  "Ungültige E-Mail-Adresse."
    }
    IF NOT RegexMatch(sEmail, "[a-z]+@[a-z]+\\.[a-z]+") THEN
        Error(errMsg)                         // shown in the current language
    END
END
```

### Runtime language switch

```owl
PROCEDURE ShowInTwoLanguages()
    greeting is a multilingualString = {
        languageFrench:  "Bonjour le monde",
        languageEnglish: "Hello world"
    }

    Language(languageFrench)
    Info(greeting)                            // "Bonjour le monde"

    Language(languageEnglish)
    Info(greeting)                            // "Hello world"
END
```

### One-off explicit lookup

To fetch a specific translation without touching the current language:

```owl
m is a multilingualString = {
    languageFrench:  "Annuler",
    languageEnglish: "Cancel"
}

sEN is a string = m[languageEnglish]          // "Cancel" — always
sFR is a string = m[languageFrench]           // "Annuler" — always
```

### Align OWL with the OS at startup

```owl
PROCEDURE Main()
    Language(SystemLanguage())                // aligns the runtime with the OS

    IF LanguageWritingDirection(Language()) = sensRightLeft THEN
        FEN_Main..TextDirection = sensRightLeft
    END

    Open(FEN_Main)
END
```

## Known limits

OWL multilingual support covers common needs. A few explicit
restrictions:

- **No hierarchical BCP 47 cascade.** A `"fr-CH"` lookup miss falls back
  directly to the source language, not to an intermediate `"fr"`. A
  developer who wants a cascade can code it in OWL.
- **No native pluralization.** Plurals are handled in standard OWL:
  `IF n = 1 THEN msgUn ELSE msgPlusieurs END`.
- **`SourceLanguage()` and `LanguageList()` not configurable.** Default
  values `"fr-FR"` and `[languageFrench]`; a project configuration sheet
  will make these values adjustable.
- **Reassigning a dict literal = replacement.** The literal
  `m = {languageFrench: "..."}` **fully** replaces the value, including
  after a separate declaration: languages absent from the literal are
  overwritten. To add or update a language **without overwriting** the
  others, use field-by-field indexing `m[languageFrench] = "..."`. The
  scalar form `m = "text"` stays distinct: it sets the source language
  and preserves the other translations.

## Enum `languageXxx` — recap

Thirty main languages without suffix (the "default" variant of the table
below) plus four explicit regional variants.

| Category | Constants |
|---|---|
| Romance | `languageFrench`, `languageSpanish`, `languageItalian`, `languagePortuguese`, `languageRomanian` |
| Germanic | `languageEnglish`, `languageGerman`, `languageDutch`, `languageSwedish`, `languageDanish`, `languageNorwegian` |
| Slavic | `languageRussian`, `languageUkrainian`, `languagePolish`, `languageCzech`, `languageSlovak`, `languageBulgarian` |
| Other European | `languageGreek`, `languageFinnish`, `languageTurkish` |
| Asian | `languageChineseSimplified`, `languageChineseTraditional`, `languageJapanese`, `languageKorean`, `languageHindi`, `languageThai`, `languageVietnamese`, `languageIndonesian` |
| Semitic (RTL) | `languageArabic`, `languageHebrew` |
| Regional variants | `languageFrenchCanada`, `languageEnglishUK`, `languagePortugueseBrazil`, `languageSpanishMexico` |

`owl explain langueXxx --json` returns the full list with the associated
BCP 47 tags.

---

## Going further

| Topic | Reference |
|---|---|
| Multilingual architectural rationale | dedicated architecture decision |
| Enum as a variable type | dedicated architecture decision |
| Literal initialization `{ ... }` | `17_init_litterale.md` |
| Type system and coercions | `01_types_et_coercions.md` |
| Code conventions | `10_conventions_de_code.md` |

For the exact signature of a function or the values of an enum:
`owl explain <symbol>` (`--json` option for machine output).
