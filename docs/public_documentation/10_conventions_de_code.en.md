# OWL code conventions

This page sets the conventions for writing OWL code: case, accents,
bilingualism, naming, layout, comments. It is the reference enforced by
the formatter and by the agents that produce code.

Guiding principle: **the formatter is the final judge.** Any form
tolerated on input is restored to its canonical form at the next
`owl fmt`. The conventions below describe what the formatter writes, not
what it accepts as input — it accepts far more.

---

## 1. Case and accents

For **identifiers** (variables, parameters, user procedures,
user-defined types), the scanner is:

- **case-insensitive**: `prix`, `Prix`, `PRIX` denote the same entity.
- **accent-insensitive**: `cle`, `clé`, `Clé`, `CLE` denote the same
  entity.

This tolerance does not license sloppiness: on save, the formatter
normalizes the **known symbols** (keywords, built-in types, native
functions, registry constants) to their exact canonical form, accents
included.

The identifiers **invented** by the user (variables, parameters,
fields, constants, procedure and class names) are also canonicalized by
the formatter: each use is brought back to the **declaration spelling**.
The declaration spelling is the one written where the identifier is
introduced (variable declaration, procedure signature, field or constant
declaration). The formatter resolves:

- **Intra-file**: the declaration is looked up in the current file. For
  a variable or a parameter local to a procedure, resolution is bounded
  to the procedure scope (a homonym in another procedure keeps its own
  spelling).
- **Cross-file (project)**: if the file belongs to a project
  (`projet.owl` ancestor, see the project model page), the declaration
  is looked up across all files of the project. A procedure or a field
  declared in a neighboring file is canonicalized to its declaration
  spelling in the files that use it. When there are multiple declarations
  with diverging cases, the formatter abstains (verbatim).

Deliberately choosing the declaration spelling therefore matters: it is
the one that appears at every use after formatting.

| Category | Canonical form |
|---|---|
| Control keyword (IF, FOR, LOOP, END, …) | UPPERCASE |
| Built-in type (`chaîne`, `monétaire`, `réel`, `dateheure`, …) | lowercase, accents included when canonical |
| Native function (`ChaineDecoupe`, `HTTPRequête`, `MockCrée`, …) | PascalCase, accents included |
| Registry constant (`httpGet`, `colTypeTexte`, `errMessage`, …) | lowercase prefix + descriptive suffix |
| Structure keyword (`Structure`, `Class`, `Enumeration`, `Combination`) | Initial uppercase |
| Modifier (`PUBLIC`, `PRIVATE`, `PROTECTED`) | UPPERCASE |

---

## 2. FR / EN bilingualism

Every keyword, built-in type, native function, constant and property
exists in a canonical French form and a canonical English form. The two
are strictly equivalent for the compiler. For readability:

- **Prefer one main language per file.** Mixing `SI` and `END` in the
  same block is technically accepted but visually unpleasant.
- **The formatter respects the language of the line**, not of the file:
  a line written in FR stays FR, an EN line stays EN. No automatic
  conversion from one language to the other.
- **For the generated documentation and the public examples**, the FR
  language is the reference. Test scenarios are expected in FR unless
  there is an explicit reason.

---

## 3. Naming

OWL adopts a system of prefixes for variables and interface controls,
inherited from a proven industry convention. The full detail of the
prefixes (scope, type, control) is in the
`12_prefixes_de_nommage.md` page. The summary principles:

### 3.1 Variables

A variable carries a **scope prefix** (lowercase, optional) followed by
a **type prefix** (lowercase, optional), then the name in PascalCase.

```owl
sName is a string                    // local, string
nAge is an integer                   // local, integer
gbTestMode is a boolean              // project global, boolean
m_balance is a currency              // instance member, currency
```

For loop indices (`i`, `j`, `k`) or very short-lived variables, the
prefix is optional.

### 3.2 Procedures

Convention: **PascalCase**, with no prefix.

```owl
PROCEDURE ComputeTotal(tabCart is an array of currency) : currency
PROCEDURE SendEmail(sRecipient is a string)
```

For test scenario procedures, the `scenario_`, `test_` or `should_`
prefix is mandatory (see `11_charte_de_test.md`).

### 3.3 User-defined types

By convention the **type** is distinguished from the **instance** by the
case of the prefix:

| Category | Type | Instance |
|---|---|---|
| Enumeration | `EStatus` | `eCurrentStatus` |
| Combination | `COPermissions` | `coUserRights` |
| Structure | `STAddress` | `stDelivery` |
| Class | `cAccount` | `oCurrentAccount` |

Detail and full examples in `12_prefixes_de_nommage.md`.

### 3.4 Constants

User-defined scalar constants use UPPERCASE with an underscore
separator:

```owl
VAT_RATE is a real = 20.0
MAX_ATTEMPTS is an integer = 3
```

For families of values, prefer a named Enumeration over a series of flat
constants.

### 3.5 Reserved words of the language

Some names are locked at the scanner level and cannot serve as a
variable, parameter or field name (structure or class). The list is
deliberately finite and stable: it matches the types and modifiers that
are part of the language core.

**Primitive scalars**:

- `string` (FR alias: `chaîne`)
- `integer` (`entier`)
- `real` (`réel`)
- `currency` (`monétaire`)
- `numeric` (`numérique`)
- `character` (`caractère`)
- `boolean` (`booléen`)
- `buffer`
- `byte` (`octet`)
- `bytes` (`octets`)

**Structural composites**:

- `array` (`tableau`)
- `structure`
- `enumeration` (`énumération`)
- `combination` (`combinaison`)
- `list` (`liste`)
- `stack` (`pile`)
- `queue` (`file`)
- `associative` (`associatif`)

**Modifiers**:

- `fixed` (`fixe`)
- `dynamic` (`dynamique`)
- `by` (`par`)
- `unsigned` (`sans`)
- `signe`

**Declarators**:

- `class` (`classe`)
- `interface`
- `function` (`fonction`)
- `procedure`

Trying to use one of these names in a variable position produces an
explicit compile error:

```owl
string is a string = "Hello"   // refused: reserved word
```

```
The word 'string' is a reserved keyword of the language and cannot be
used as a variable name.
```

**Free names** as identifiers, despite looking like a type:

- Datetime value types: `date`, `heure`, `dateheure`, `durée`
- Generic value types: `json`, `uuid`
- All registry types: `httpRequête`, `chrono`, `image`, `couleur`, …
- All user-defined types (Structures, Classes, Enumerations,
  Combinations declared in the project).

These names stay available as variables, parameters or fields
(`durée est une durée`, `date est une date`, `httpRequête est une
chaîne`), even though the context lets the compiler tell a declaration
apart from a use of the name as a type. This tolerance reflects common
practice and introduces no ambiguity: the language grammar decides.

### 3.6 Window controls, report controls, other fields

The names of UI controls carry an UPPERCASE prefix followed by an
underscore that denotes the control type: `FEN_` for the window itself,
`LIB_` for a label, `BTN_` for a button, `SAI_` for an input, `TABLE_`
for a table, etc. The complete list of supported controls is in
`12_prefixes_de_nommage.md`.

```owl
FEN_Login is a Window
    LIB_Title is a Control Label
    SAI_User is a Control Input
    SAI_Password is a Control Input
    BTN_Validate is a Control Button
END
```

---

## 4. Indentation and layout

For **the compiler**, OWL is indentation-independent: code structure is
carried by keywords (`FIN`, `END`, etc.), not by indentation. All code
compiles, whatever the layout.

For **the formatter**, layout is fixed and imposed: on save, the code is
re-indented according to block depth. The canonical form used by the
formatter:

- **One tab per nesting level.** No space replacement.
- **One instruction per line**, except in special cases. The `;`
  separator allows two instructions to be chained explicitly on the same
  logical line (`sTest is a string ; sTest = "Hello"`); the
  formatter respects this choice and does not break it up. Reserve it for
  cases where concision improves reading, otherwise prefer a line break.
- **Space around binary operators**: `a + b`, not `a+b`.
- **No space between a function name and its call parenthesis**:
  `Trace("…")`, not `Trace ("…")`.
- **No space inside the call parentheses**: `f(x)`, not `f( x )`.
- **A single `FIN` closes one block at a time**; `SI … FIN FIN` is not
  factored onto a single line.

A user who writes in a messy way sees the file reformatted at the next
`owl fmt`. There is no need to aim for perfect layout while writing: the
formatter takes care of it.

---

## 5. Comments

Two forms:

```owl
// Comment to the end of the line.

/* Block comment, can span
   several lines and be nested:
   /* valid inner comment */
   the block continues here. */
```

The block comment supports nesting, which is useful for temporarily
commenting out code that already contains block comments.

Writing convention:

- No comment that paraphrases the code (`// incrémente i`).
- A comment explains the **why**, not the **what**.
- No project-history comments in user code ("Phase n", "added for
  issue #42", "internal batch"). History lives in version control, not
  in the source.
- For test scenarios, the comment preceding the procedure is the
  **public description**: rule described in `11_charte_de_test.md`.

---

## 6. Line continuation

A logical line that is too long can be split across several physical
lines with the `...` marker placed at the end of a line. The next line is
then treated as the direct continuation of the previous one.

```owl
moResult is a currency
moResult = moAmountExclTax * (1 + rVatRate / 100)...
           - moDiscount...
           - moShippingCost
```

The formatter automatically aligns continuation lines under the first
operand of the assigned value, so the rest of the computation stays
readable. When the header is not a simple assignment (combined
declaration and initialization, call spread over several lines), the
following lines receive a one-level indent rather than an alignment.

The linking operator (`+`, `-`, `*` …) can be placed either **before**
the `...` or at the start of the next line: neither form is imposed.
Choose one convention per file for readability.

Continuation is neither a shortcut nor a stylistic nicety: it exists for
lines that become materially too wide. For everything else, prefer a
well-named intermediate variable over a fragmented line.

---

## 7. Global form or method form

Many native functions exist in two equivalent forms:

```owl
parts is an array of strings = StringSplit(line, ",")
parts is an array of strings = line.Split(",")
```

Convention:

- **Method form** when the subject is clearly the receiver and the
  operation reads naturally as "the receiver does something".
- **Global form** when several arguments are equally important or when
  the call expression chains poorly.
- **Chained method form** when several transformations follow one
  another on the same object: `s.NoSpace().Upper().Reverse()`.

---

## 8. File organization

Some conventions are **imposed by the language** (the runtime uses them
for discovery and launch). Others are **suggestions** of organization
that help reading but that nothing forces to follow.

### 8.1 Imposed by the language

- **`projet.owl`** at the project root for the execution mode. It is the
  project root marker **and** the starting point: the code of its block
  is the program, executed last (after the init of the collections).
  Without a `projet.owl` up the tree, `owl` alone (with no named file)
  opens the REPL instead of launching a project.
- **Top-level code is the startup.** No `Main()` procedure is called
  automatically: `Main` is an ordinary procedure name with no special
  status.
- **`Fermeture()` procedure (or `Closing()`)** as the exit point
  automatically called at the end of the project execution, to release
  resources (defined in `projet.owl`).
- **`test_` prefix** on the files run by `owl test`. A file without this
  prefix is not considered a test, even if it contains assertions.

These elements are **contracts** with the runtime. Departing from them
breaks the execution mechanics.

### 8.2 Organization suggestions

- **Split by subject, not by layer.** A `utilisateurs.owl` file that
  gathers the structures and procedures related to users is preferable to
  `structures.owl` + `procedures_utilisateur.owl`.
- **One class per file** when the class is non-trivial, named after the
  class: `cCompte.owl`. The compiler does not care at all; it is purely a
  navigation aid.
- **One window or one report per file**, named after the control it
  defines: `FEN_Login.owl`, `ETAT_Facture.owl`. Here too, for human
  reading.
- **Tests by nature**: scenarios, regression, verification, formatting.
  See `11_charte_de_test.md`.

OWL walks the project recursively and concatenates **all** the files
that do not start with `test_`. The layout in subfolders is therefore a
reading tool for humans and agents, not a language modularization
mechanism.

---

## 9. Declaration markers

A **declaration marker** is an annotation placed after the type of a
declaration, between angle brackets (`<...>`), that adjusts how the
compiler treats that declaration without changing the type itself.

### 9.1 `<utile>` / `<useful>` — deliberately unused variable

The compiler emits a non-blocking warning when a variable or a parameter
is declared but never read:

```
Variable 'x' is declared but never used.
```

This is a useful reminder to spot dead code, but some declarations are
unused **on purpose**: a parameter imposed by a shared signature, a
variable reserved for an upcoming change, a documentary companion. For
these cases, the `<utile>` marker (English form `<useful>`) annotates the
declaration as deliberately unused and silences the warning for it alone:

```owl
n is an integer <useful>         // no warning, even if never read
m is an integer                  // warned: declared but never used
```

The marker is placed in the same spots as an ordinary declaration:

- **Local variables** of a procedure.
- **Global variables** of a file or a project.
- **Procedure parameters**, after the parameter type:

```owl
PROCEDURE OnClick(sSource is a string <useful>)
    // sSource is not used here, but the signature imposes it
    Trace("click")
END
```

A free-value form lets the reason be recorded directly in the
declaration; the string is purely documentary:

```owl
nReturnCode is an integer <useful="reserved for future error handling">
```

The marker is **case-insensitive** (`<UTILE>`, `<Useful>` are accepted)
and available in both languages (`<utile>` in French, `<useful>` in
English), like any language symbol.

An **initialized** declaration (`n is an integer = 3`) is never
concerned by the warning, marker or not: the warning targets only
declarations that are not read and not initialized.

### 9.2 Other markers

The `<nullable>` marker lets a typed variable hold `Null` distinctly
from the default value of its type; it shares this semantics with the `?`
suffix. Both are described in the `02_null_et_nullable.md` and
`06_poo.md` pages.

---

## Further reading

| Topic | Page |
|---|---|
| General language overview | `00_apercu_du_langage.md` |
| Test charter | `11_charte_de_test.md` |
| Naming prefixes (full detail) | `12_prefixes_de_nommage.md` |
| Type system | `01_types_et_coercions.md` |
| Line endings, encoding and input/output | `26_encodage_et_io.md` |

To check that a file follows the canonical conventions:
`owl fmt --check <fichier>` (diff without writing).
