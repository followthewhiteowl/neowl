# The project.owl file

An OWL project is delimited by a file named `project.owl` placed at the
project root. This file plays three complementary roles: it **marks the
root** of the project (from it on, sub-folders are included in the
project), it **declares the metadata** valid for the whole project, and
the **code of its block is the program** — the project's start point
(see § 4bis). The boundary rule and the two levels of organization
(script, project) are described in `00_apercu_du_langage.md` § 11; this
sheet details the content of the file itself.

The name `project.owl` is **reserved**: it is never launched as an
isolated script. `owl`, `owl <folder>` and `owl project.owl` all launch
the **project** (metadata, sub-folder inclusion, execution order). A
`project.owl` must declare an `is a Project` block with at least the
`Languages` field; the rest of the metadata and the startup code are
optional.

## 1. Form of the declaration

The metadata is written in a declarative block in the canonical format
of composite types:

```owl
MyApplication is a Project
	PROPERTIES
		Languages   = [languageFrench]
		Description = "Inventory management"
		Author      = "Scumm Solution"
		License     = "MIT"
	END
END
```

The identifier placed before `is a Project` (here `MyApplication`)
becomes a **global variable of the project**: it is readable from any
file of the project (see § 5). The field catalog is **frozen** and
covers several metadata families: project identity (`Languages`, the
only mandatory field, plus `Description`, `Author`, `License`), the
presentation (`Theme`, `StartupWindow`), the default display formats
(`DateFormat`, `TimeFormat`, `DurationFormat`, `NumberFormat`,
`CurrencyFormat`) and a naming safeguard
(`EnableNamingConventionWarning`). Each is detailed below. A field
outside the catalog is rejected at compilation. The live list of fields
and their types is obtained with `owl explain Projet`.

## 2. The `Languages` field (mandatory)

`Languages` declares the languages of the project as an **ordered
array**, always between brackets `[ ]`, each entry being a `languageXxx`
constant or a literal tag (see below). As soon as a `Project` block is
declared, this field is **mandatory**: at least one language must be
present.

The **first language in the list is the default language** (the
project's source language): it is the one the runtime considers as the
writing language of string literals.

The list form is the **only** one accepted, including for a single
language:

```owl
MyApplication is a Project
	PROPERTIES
		Languages = [languageFrench]
	END
END
```

Several languages are declared in the same array, the order being
meaningful:

```owl
MyApplication is a Project
	PROPERTIES
		Languages = [languageFrench, languageEnglish, languageGerman]
	END
END
```

Here `languageFrench` is the default language, `languageEnglish` and
`languageGerman` are additional languages. The bare scalar form
(`Languages = languageFrench`, without brackets) is refused at
compilation.

Each entry of the list accepts two equivalent forms: an official
language constant `languageXxx` (for example `languageFrench`,
`languageEnglish`, `languageGerman`, `languageSpanish`…) **or** a
non-empty literal tag. The tag may follow the standard region/language
convention (`"en-GB"`, `"fr-CH"`) or name a custom variant or a dialect
(`"fr-jura"`, `"sindarin"`). The complete list of constants is reachable
via `owl list constants` (filter on `langue`).

The case of tags is **normalized** to its canonical form: the language
subtag in lowercase, the region subtag in uppercase (`"fr-fr"` becomes
`"fr-FR"`). A constant and a literal that name the same value are
therefore interchangeable.

Refused at compilation are: an **empty list**, a **duplicate** (two
entries that normalize to the same tag) and any value that **is not a
constant** (function call, variable) — the metadata must stay readable
without executing the code. The first language of the list is the
**source language** of the project.

The management of translated strings itself is described in
`21_internationalisation.md`.

## 3. Optional fields

Apart from `Languages`, all the catalog fields are optional. They can be
filled in any order, or omitted.

### 3.1 Identity (strings)

| Field         | Role                                   |
|---------------|----------------------------------------|
| `Description` | Short project description.             |
| `Author`      | Project author or publisher.           |
| `License`     | Project license.                       |

```owl
MyApplication is a Project
	PROPERTIES
		Languages   = [languageFrench]
		Description = "Inventory management"
	END
END
```

### 3.2 Presentation

Two fields point to instances declared in a companion file (a typed
reference, not a string):

| Field           | Role                                                        |
|-----------------|-------------------------------------------------------------|
| `Theme`         | Style theme applied to the project (instance `is a Theme`). |
| `StartupWindow` | Window opened at launch (instance `is a Window`).           |

Left unset, these fields mean "none": no theme, and a project without a
startup window opens no interface automatically.

### 3.3 Default display formats

These fields set a format inherited by the controls that do not carry
one of their own:

| Field            | Role                                                 |
|------------------|------------------------------------------------------|
| `DateFormat`     | Default date template (`JJ`, `MM`, `AAAA`…).         |
| `TimeFormat`     | Default time template (`HH`, `MI`, `SS`…).           |
| `DurationFormat` | Default duration template (`JJ`, `HH`, `MI`…).       |
| `NumberFormat`   | Inline numeric format (separators, decimals).        |
| `CurrencyFormat` | Inline currency format (symbol, decimals).           |

### 3.4 Naming safeguard

| Field                            | Role                                        |
|----------------------------------|---------------------------------------------|
| `EnableNamingConventionWarning`  | Enables the checking of field prefixes.     |

At `True`, every interface control whose name does not follow the
canonical prefix of its type triggers a non-blocking warning. The
complete charter of prefixes is described in
`12_prefixes_de_nommage.md`.

### 3.5 Example of the advanced fields

A `project.owl` that opens a window at launch and enables the naming
check:

```owl
// project.owl
MyApplication is a Project
	PROPERTIES
		Languages     = [languageFrench]
		Description   = "Inventory management"

		// opens this window at startup (instance declared elsewhere)
		StartupWindow = FEN_Home

		// flags badly prefixed controls (non-blocking warning)
		EnableNamingConventionWarning = True
	END
END
```

`FEN_Home` references an instance `is a Window` declared in a companion
file of the project; the reference is written **without quotes**. The
bilingual spelling of each field (English form included) is given by
`owl explain Projet`.

## 4. Cascading access

The project identifier is a global reachable from any file of the
project, with the property operator `..`:

```owl
// FEN_Home.owl, elsewhere in the project
PROCEDURE EndInitialization()
	Trace(MyApplication..Description)
	Trace(MyApplication..Author)
	Trace(MyApplication..License)
END
```

Optional fields left unset are the empty string. `Languages` returns the
ordered list of the declared constants. These values are available
**very early** — established before any executable code — because they
are constant expressions materialized at compilation.

## 4bis. The program and the execution order

The code written **inside the block** `is a Project` of `project.owl`
**is the program**: it is the start point. There is **no `Main()`
procedure called automatically**; `Main` is an ordinary procedure name,
with no special status. The startup is this code — and nothing is
written outside the block.

```owl
// project.owl — the metadata AND the program are in the same block
MyApplication is a Project
	PROPERTIES
		Languages   = [languageFrench]
		Description = "Inventory management"
	END

	// the block's code is the program, executed last
	Trace("Starting " + MyApplication..Description)
	Open(FEN_Home)

	PROCEDURE Closing()
		Trace("Closing the project")
	END
END
```

The execution of a project follows a deterministic order:

1. **Census** — OWL records what each file of the project declares
   (windows, reports, classes, collections, themes, project). The files
   that declare none of these elements (plain scripts) are set aside.
   Taking a census is not executing: at this stage, nothing runs yet.
2. **Metadata and source language** — the values of the `is a Project`
   block are established before any executable code, and stay readable
   in cascade from any file.
3. **Collections** — the code at the top of each collection runs
   **first**: these are the services the application consumes.
4. **`project.owl`** — its code runs **last**, once the services are in
   place. It is the one that starts the application.
5. **`Fermeture()` / `Closing()`** — called when the project closes to
   free the resources, if `project.owl` defines it.

This order — dependencies first, orchestrator last — guarantees that the
program starts in an already-initialized environment.

### A project global read before startup: default value

A variable declared at the top level of the `is a Project` block is a
project global, **resolvable from any file**. Its *declaration* (name
and type) is established very early, but its *initialization* (`= …`) is
only played by the code of the `project.owl` block, executed **last**. A
file that runs **before** this code — the top of a collection, for
example — therefore reads the global at its **type default value**:
empty string, `0`, `0.0`, `False`, or `Null` for a nullable or composite
type. The initialization value only appears once the code of
`project.owl` has run.

```owl
// project.owl
MyApplication is a Project
	PROPERTIES
		Languages = [languageFrench]
	END

	alertThreshold is an integer = 80     // project global, initialized here (last)
END
```

```owl
// stock_services.owl — collection, run BEFORE the code of project.owl
PROCEDURE onInitialization()
	Trace(alertThreshold)     // prints 0: the initialization to 80 has not happened yet
END
```

The **metadata** of the `is a Project` block (§ 4) escapes this rule:
these are constant expressions, available from the start in cascading
access.

### Confining a variable to the startup flow: `LOCAL`

By default, a variable declared at the top level of the `is a Project`
block is a **project global**, readable from any file. For a temporary
that only the startup code needs — a loop index, an accumulator — the
`LOCAL` switch **confines it to the top-level flow**: it is then neither
a project global, nor visible to the procedures, nor reachable from the
other files.

`GLOBAL` and `LOCAL` sit **alone on their line** and switch the mode of
the declarations that follow, until the reverse switch — there is no
block to close. `GLOBAL` restores the default (project global). The two
alternate freely.

```owl
MyApplication is a Project
	PROPERTIES
		Languages = [languageFrench]
	END

	serviceCount is an integer = 0     // default: project global

	LOCAL
	i is an integer = 0               // confined to the startup flow
	WHILE i < 3
		serviceCount = serviceCount + 1
		i = i + 1
	END

	GLOBAL
	Trace("services started: " + serviceCount)
END
```

Here `serviceCount` is readable from the other files of the project;
`i`, confined, is not.

## 5. Bilingual spelling

Like any symbol of the language, the project marker exists in two
equivalent canonical spellings: `projet.owl` in French and
`project.owl` in English. The declared type follows the same rule:
`Projet` / `Project`. A project chooses one spelling and sticks to it;
the two forms do not coexist in the same folder.

```owl
MyApplication est un Project
	PROPRIETES
		Languages   = [languageFrench]
		Description = "Inventory management"
	FIN
FIN
```

## 6. Creating a project

The `owl new` command generates a `project.owl` marker in the current
folder:

```bash
owl new project MyApplication
owl new projet MonApplication   # French spelling
```

The name is optional: by default, it reuses the name of the current
folder. The generated file contains a minimal block with the
`Languages` field filled in. The command refuses to overwrite an
existing marker.

## 7. Coming up

The `project.owl` file is meant to welcome other project metadata,
notably the notion of configurations. These extensions will be
documented here as they open up.
