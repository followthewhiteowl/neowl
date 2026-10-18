# OWL — Language overview

OWL is a general-purpose language with explicit typing, compiled to
bytecode and executed by a virtual machine. It is designed to be read,
written and maintained by humans and AI agents alike. The syntax is
verbose by choice: an OWL program should read like a description of what
it does.

This overview is deliberately short. It is a first contact. For each
topic touched on here, a dedicated page exists.

---

## 1. First program

```owl
Trace("Hello, OWL.")
```

Any `.owl` file can be run directly. There is no module header to write
and no entry point to declare for a short script: the code written at
the top of a file is executed from top to bottom.

```bash
owl hello.owl
```

To experiment without creating a file, `owl` launched on its own opens
an interactive loop (REPL).

---

## 2. Variables and types

A variable is declared in an explicit form: its name, the word `is`,
its type. Initialization is optional at declaration; the type never is.
There is no inference: it is the programmer who states what they want.

> **Reserved names.** The language is case- and accent-insensitive, so
> a keyword stays reserved in all its forms: `a`, `A` and `à` are the
> same keyword. A reserved name used as a variable is rejected at
> compilation with an explicit message. The complete list of reserved
> keywords appears among the language catalogues, to consult when in
> doubt about a name.

```owl
name is a string = "Alice"
age is an integer = 32
rate is a real = 19.6
amount is a currency = 1499.99
```

The fundamental types are:

| Type | Usage |
|---|---|
| `string` | UTF-8 text, size measured in bytes |
| `integer` | 64-bit integer; automatic promotion to `numeric` on overflow |
| `real` | Double-precision float |
| `currency` | Exact decimal, 6 digits after the decimal point |
| `numeric` | Exact decimal with unlimited precision (grows with memory) |
| `boolean` | `True` or `False` |
| `date` | Civil date without timezone |
| `time` | Time of day, millisecond precision |
| `datetime` | Date + time combination |
| `duration` | Time interval |
| `array of <T>` | Ordered collection, 1-based index |
| `JSON` | Structured document with dot access |

Composite types (`httpRequest`, `Frame`, `Style`, `Chrono`, etc.) are
each described in their dedicated page.

---

## 3. Implicit conversions

Between compatible types, OWL converts without asking. A string that
contains a number becomes a number in an arithmetic context. An integer
becomes a string in a text context.

The `+` operator is **always** a concatenation when one of the operands
is a string. It is explicit, not a promotion:

```owl
total is a string = "42" + 8        // "428"
double is an integer  = 42 + 8      // 50
message is a string = "Score: " + 99   // "Score: 99"
```

Explicit conversions exist when the need is clear:
`StringToInteger`, `NumToString`, `Val`, etc.

---

## 4. Control flow

```owl
IF age >= 18 THEN
    Trace("Adult")
ELSE
    Trace("Minor")
END

FOR i = 1 TO 10
    Trace(i)
END

FOR EACH element OF list
    Trace(element)
END

WHILE counter > 0
    counter--
END

SWITCH status
    CASE "active" : Trace("OK")
    CASE "suspended", "closed" : Trace("Inactive")
    OTHER CASE : Trace("Unknown")
END
```

The generic block `LOOP … END` exists for iterations without an initial
condition, terminated by `BREAK`.

---

## 5. Procedures

A procedure is named, may or may not return a value, and accepts typed
parameters.

```owl
PROCEDURE PriceInclTax(net is a currency, rate is a real) : currency
    RESULT net * (1 + rate / 100)
END
```

Procedures are first-class values: they can be passed as a parameter,
stored, composed.

```owl
PROCEDURE Double(n is an integer) : integer
    RESULT n * 2
END

f is a Procedure = Double
Trace(f(7))   // 14
```

Multiple return values are possible. The receiving variables must be
declared beforehand:

```owl
PROCEDURE MinMax(t is an array of integers)
    minVal is an integer = t[1]
    maxVal is an integer = t[1]
    FOR EACH v OF t
        IF v < minVal THEN
            minVal = v
        END
        IF v > maxVal THEN
            maxVal = v
        END
    END
    RESULT (minVal, maxVal)
END

low is an integer = 0
high is an integer = 0
(low, high) = MinMax(scores)
```

### Shadowing a native function

A global procedure can bear the same name as a native function. It then
**shadows** it: an unprefixed call runs the procedure, not the native. The
**`OWL.`** prefix (single dot) recalls the original native function, even
when a procedure of the same name shadows it.

This opens the "enrich then delegate" pattern: add a behavior, then hand
off to the original function.

```owl
PROCEDURE Length(x is a string) : integer
    Trace("intercepted call")
    RESULT OWL.Length(x)   // delegates to the original native function
END

Trace(Length("abc"))   // prints "intercepted call" then 3
```

Without the prefix, the procedure would call itself endlessly. `OWL.`
targets only a known native function: `OWL.<unknown name>` is rejected at
compilation.

---

## 6. Strings

A string is written in one of four forms depending on content and the
need for interpolation. All forms are **raw** by default: no escaping
such as `\n` or `\t`. The only two forms that enable `[%expression%]`
interpolation are backticks and the `%` prefix.

**Double quotes** `"…"`: the historical form, raw. To insert a literal
quote, double it (`""`). **Interpolation `[%…%]` is NOT active** — the
sequence stays literal in the string.

```owl
name is a string = "Alice"
template is a string = "Hello [%name%], nice weather"   // "[%name%]" stays literal
quote is a string = "He said ""hi""."
```

**Brackets** `[ … ]`: for multi-line without escaping, raw content, no
interpolation.

```owl
template is a string = [
    Line 1
    Line 2 with "quotes" and \ literals
]
```

The block ends on the first line whose first non-blank character is a
`]`, **and** whose first non-blank character after that `]` is either an
end of line or a structural punctuation: `,` `)` `}` `]`. What comes
after that punctuation is free — a block can thus serve as the first
argument to a call, or as a value in a literal.

```owl
message is a string = StringBuild([
Hello %1,
You have %2 messages.
], "Alice", 3)
```

Any other line beginning with `]` remains content, including
`] followed by text`. A template containing JSON or closing brackets at
the start of a line is thus preserved.

```owl
excerpt is a string = [
] this line is part of the text
]
```

**Backticks** `` ` … ` ``: raw + **`[%var%]` interpolation enabled
automatically**, multi-line natively supported, and — unlike `[ … ]` —
an inner `]` closes nothing. Ideal for JSON, SQL, HTML, or to interpolate
in a readable message.

```owl
config is a string = `{"name":"Alice","tags":["admin","dev"]}`
greeting is a string = `Hello [%name%], welcome.`
```

**Line endings of the multi-line forms.** In a `[ … ]` block as between
backticks, a line break is always **`LF`**, whatever the source file's
recording (Unix-style or Windows-style). What is read on screen is what
the program sees. A Windows pair in a string is therefore written
explicitly, with `CRLF`; for byte 13 alone, `Charact(13)`.

```owl
excerpt is a string = `line 1
line 2`
Trace(excerpt = "line 1" + LF + "line 2")     // 1 (True)
windows is a string = "line 1" + CRLF + "line 2"
Trace(Length(windows))                          // 14
```

The rule concerns only the program text: data is never altered, text
that is entered, a file that is read or a network response keep their
exact bytes. Details in `26_encodage_et_io.md`, section "Line endings".

**`%` prefix** on double quotes, `%"…"`: strictly equivalent to `"…"`
for accepted characters, **but enables `[%expression%]` interpolation**.
Use it when you want a short, interpolated string without switching to
backticks.

```owl
greeting is a string = %"Hello [%name%], welcome."
```

Interpolation accepts an identifier or a member access
(`[%person.name%]`), not a function call. To insert the result of a
call, store it in a variable first.

> **Simple rule.** To interpolate, use **backticks** or the **`%"…"`
> prefix**. Bare quotes `"…"` never interpolate: this is an explicit
> design choice (cf. `docs/VISION.md`, AI-first principle — no string
> behavior depending on a project configuration). It is a deliberate
> divergence from the historical convention of the business-oriented
> languages OWL draws inspiration from.

Indexing and slicing use the double bracket, 1-based, with the `TO`
separator (canonical form, case- and accent-insensitive; `ON` to specify
a length):

```owl
s is a string = "abcdefgh"
Trace(s[[3]])         // "c"     — character at index 3
Trace(s[[2 TO 4]])    // "bcd"   — substring from the 2nd to the 4th inclusive
Trace(s[[5 TO]])      // "efgh"  — from index 5 to the end
Trace(s[[TO 4]])      // "abcd"  — from the start to index 4
Trace(s[[2 ON 3]])    // "bcd"   — 3 characters from index 2
```

String methods chain naturally:

```owl
clean is a string = "  Hello  ".NoSpace().Upper()
```

---

## 7. Arrays and JSON

Arrays are 1-based. The size is dynamic unless explicitly constrained.
Manipulation methods chain.

```owl
basket is an array of strings
basket.Add("apple").Add("pear").Add("banana")

FOR EACH fruit OF basket
    Trace(fruit)
END
```

The `JSON` type allows the manipulation of structured documents without
a prior model. A JSON string is parsed explicitly:

```owl
doc is a JSON = StringToJSON("{""user"": {""name"": ""Alice"", ""age"": 32}}")
Trace(doc.user.name)   // "Alice"
doc.user.age = 33
```

---

## 8. Exceptions

The `WHEN EXCEPTION` block captures the errors raised in its body. The
optional `ELSE` block runs only when no error occurred.

```owl
WHEN EXCEPTION IN
    ExceptionThrow(7, "resource unavailable")
DO
    Trace("Failure: " + ExceptionInfo(errMessage))
ELSE
    Trace("Everything went fine")
END
```

Exceptions carry a structured context: message, line, calling procedure,
stack. The `ExceptionInfo` function accesses it field by field.

Not all errors are exceptions. Some natives (file reading, JSON parsing,
SQL queries…) signal failure by returning a neutral value (empty string,
Null…) and setting the global variable `ErrorOccurred`. This is a
*soft-error* — non-blocking, to be tested explicitly with
`IF ErrorOccurred THEN …`. The detail is provided by `ErrorInfo()`. See
the dedicated chapter for the soft-error vs exception distinction.

---

## 9. Classes, structures, enumerations

OWL supports object-oriented programming and named types. A class's
members and methods are declared **in the body** of the class, inside a
visibility block (`PUBLIC` or `PRIVATE`). The constructor is a procedure
named `Constructor`. The `:` prefix designates the member of the current
instance.

```owl
Person is a Structure
    name is a string
    age is an integer
END

Status is an Enumeration
    active
    suspended
    closed
END

cAccount is a Class
PUBLIC
    owner is a string
    balance is a currency

    PROCEDURE Constructor(name is a string)
        :owner = name
        :balance = 0
    END

    PROCEDURE Credit(amount is a currency)
        :balance += amount
    END
END
```

An instance is created by calling the type as a constructor:

```owl
oAccount is a cAccount("Alice")
oAccount.Credit(150.00)
```

Classes support single inheritance and interface implementation. The
detail is in the page dedicated to objects.

### Member access: `.` and `..`

OWL distinguishes two access operators, and the distinction is checked
at compilation:

- **one dot** `.` calls a **method** or reads a **member** (field of an
  object, of a structure, or of a JSON document):
  `oAccount.Credit(...)`, `person.name`, `doc.user`.
- **two dots** `..` read or write a **property** — a predefined
  characteristic of an object or a container:
  `array..Occurrence`, `SAI_Name..Value`.

Using the wrong operator produces an explicit compile error.

---

## 10. Bilingualism and writing tolerance

Every symbol of the language exists in two equivalent forms: canonical
French and canonical English. The two can be mixed in the same file,
line by line.

```owl
SI x > 0 THEN
    Trace("positive")
END
```

Beyond bilingualism, the scanner is case- and accent-insensitive for
identifiers: `chaine`, `Chaîne` and `CHAINE` designate the same type.
This tolerance is not an invitation to sloppiness: on save, the formatter
restores the exact canonical form. Tolerance serves fast writing; the
canon serves reading and diffing.

---

## 11. Organizing a project

OWL distinguishes **two levels** of organization, determined by walking
up the parent directories from the targeted file:

1. **Script** — no marker. The context is limited to the **current
   directory**: files that are neighbors in the same directory see each
   other, sub-directories are ignored. Maximum freedom, "scripting
   language" spirit. The file to run must be named explicitly:
   `owl my_file.owl` runs its top-level code and loads its directory
   neighbors. There is **no default entry**: `owl` alone (or
   `owl <directory>`) without a `projet.owl` produces an error — a file
   must be named, or a `projet.owl` created.
2. **Project** — a `projet.owl` file marks the **root** of the project
   and **extends the scope to all sub-directories**: the project groups
   all files located in the `projet.owl` directory and in its
   sub-directories. It also designates the **start point**: `owl`,
   `owl <directory>` or `owl projet.owl` launch the project. The
   `projet.owl` file is **reserved** (protected, case-insensitive name):
   it never runs as an isolated script. Its form (boundary marker and
   metadata: languages, description, author, license) and its role as a
   program are described in the `24_projet.md` page.

For any file, the first `projet.owl` encountered while walking up defines
the root. Without an ancestor `projet.owl`, the scope falls back to the
local script mode.

The `owl run`, `owl --check`, `owl fmt` and `owl test` tools resolve the
scope of each file passed as an argument through this boundary rule; they
never analyze the file in isolation.

**Execution order of a project.** OWL proceeds in deterministic steps:

1. **Census** — OWL records what each project file declares: windows,
   reports, classes, collections, themes, and the project itself. Files
   that declare none of these elements (plain scripts) are set aside —
   see "Script files" below. Taking a census is not executing: at this
   stage, nothing runs yet.
2. **Metadata and source language** — the values of the `is a Project`
   block are established very early, before any executable code, and
   stay readable in cascade (`MyApp..Version`) from any file.
3. **Collections** — the code at the top of each collection runs
   **before** the main program: these are the services the application
   depends on (shared data, utility procedures). A window or a report,
   for its part, initializes only when it opens, not at startup.
4. **`projet.owl`** — its code runs **last**. The code at the top of
   `projet.owl` **is the main program**: it starts once the services are
   in place.
5. **`Fermeture()` / `Closing()`** — when the project closes, if
   `projet.owl` defines this procedure, it is called to release
   resources.

There is **no `Main()` procedure called automatically**: the top-level
code is the startup.

**Script files.** In a project, a file is part of the project as soon as
it declares a window, a report, a class, a collection, a theme or the
project. A file that contains only free code (a file without a particular
type: neither window, nor report, nor class, nor collection) — variables,
procedures or statements written directly, without grouping them into one
of these elements — is a **script**: it stays private, invisible to the
rest of the project, and runs only when launched on its own
(`owl my_script.owl`). Code meant to be shared (procedures, constants,
types, common data) therefore goes into a **collection**, made for that.
A script that is launched sees the rest of the project — the collections
(already initialized) and the types of windows and reports — but not the
other neighboring scripts. Outside a project, in a plain directory
without a `projet.owl`, this rule does not apply: all files in the
directory see each other and their code runs.

`owl test` walks the targeted tree recursively to **find** all
`test_*.owl` files, and runs **each test in the context of its own
boundary** (not a global mega-project): a test located under a
`projet.owl` sees the project tree; an isolated test sees only its
directory. The distribution of tests by nature stays free.

Important note: the editor (VSCode) opens a **workspace** that can
contain several independent projects and scripts. The tools resolve the
project of **each file** through the boundary rule above, never the
workspace as a single project. For a multi-file directory to work with
its sub-directories, adding a `projet.owl` (with its `is a Project`
block) at its root is enough.

**A file that declares a class contains only that class and comments** —
no top-level code, no other class. Executable code goes into
`projet.owl` or into a Collection. Any violation produces a compile
error. See `06_poo.md` § 1.

```owl
// projet.owl — the block body is the program
MyApp is a Project
    PROPERTIES
        Languages = [languageFrench]
    END

    activeConfig is a string = "production"
    Trace("Starting in mode " + activeConfig)

    PROCEDURE Closing()
        Trace("Goodbye")
    END
END
```

`projet.owl` always declares an `is a Project` block; it is the body of
this block that constitutes the program (start point). Nothing is
accepted outside the block. A window is declared elsewhere in the
project; the recommended pattern is for `projet.owl` to *launch* the
windows via `Open(...)` rather than being a window itself — which keeps
the startup distinct from the views.

**Scope: it depends on the element surrounding the declaration.** The
rule reads at the level of the block that contains the declaration:

- In `projet.owl` and in a **collection**, what is declared at the top
  (variable, constant, type, procedure) is **visible everywhere in the
  project**.
- In a **window** or a **report**, the variables declared at the top are
  **private**: they exist only for that file. Only the **types** declared
  there (structure, enumeration) stay accessible from the outside, by
  prefixing them with the window name: `FEN_Login.EStatus`, or
  `FEN_Login.EStatus.active` for a value. The view thus keeps its state
  to itself, while sharing its types.
- A **script file** keeps everything to itself: nothing it declares is
  visible from the project (see "Script files" above).

In a window file (`FEN_X.owl`), the rendering or logic procedures live
**inside the `is a Window` block**, not after its `END`. The window file
serves to declare the window and its related procedures; writing a
procedure at the top of the file, outside the block, to access the fields
declared in the window, is rejected at compilation. The complete wiring
of a graphical application spread over several files (state in
`projet.owl`, procedures in the window block, shared logic in a class) is
detailed in `15_lifecycle_champs.md` §9.

In a directory without a `projet.owl`, launching `owl` without naming a
file opens the REPL.

---

## 12. Discovering the language

The `owl` binary is its own documentation.

```bash
owl primer                      # this document
owl learn <topic>               # a fundamental page
owl explain <symbol>            # detail of a type, a function, a constant
owl examples <symbol>           # concrete associated scenarios
owl search <keyword>            # cross-cutting search
owl spec                        # complete grammar
```

No network connection is required. No external file to download. The
documentation ships with the binary.

---

## 13. Built-in tests

OWL treats testing as a first-class citizen. Any file named `test_*.owl`
is run by `owl test`. The canonical scenario format, the fixtures and the
naming convention are detailed in the `11_charte_de_test.md` page.

```owl
PROCEDURE scenario_basic_addition()
    dbgCheckEqual(2 + 2, 4, "integer addition")
END
```

The scenarios are both executable tests and reference examples. It is
through them that the documentation of native functions is illustrated.

---

## 14. Designed for AI agents

OWL is explicitly designed so that an AI agent can read, write and verify
code without having to guess. The design combines strict constraints
(which eliminate ambiguity) and targeted tolerances (which make writing
fast).

**Strict — what is forbidden:**

- No type inference. Every variable is declared with its type.
- No variant type nor dynamic objects in the sense that a value would
  change structure at runtime.
- No hidden state: no invisible meta-programming, no import effect, no
  macro that rewrites the code.
- Deterministic diagnostics: `owl --check` produces structured JSON
  errors, identical from one run to the next.

**Flexible — what is made easier:**

- A single canonical way to write each concept, restored by the
  formatter on save.
- FR/EN bilingualism, accent and case tolerance for writing.
- Automatic coercions between numeric types and between numeric and text
  in unambiguous contexts.
- Native introspection available at runtime: `TypeVar(x)`,
  `VariableExists(x)`, `ExceptionInfo`, etc.
- Discovering the language through the binary, without an external
  dependency.

An agent that encounters OWL for the first time can, by reading this
document and using `owl explain` and `owl examples`, write its first
correct program without resorting to third-party sources.

The concrete how-to — connecting an AI to a project via the built-in MCP
server — is described in `25_agents_ia.md`.

---

## Going further

| Topic | Page |
|---|---|
| Writing conventions and formatter | `10_conventions_de_code.md` |
| Test charter and scenarios | `11_charte_de_test.md` |
| Detailed type system | `01_types_et_coercions.md` |
| Associative arrays (string key → value) | `30_tableaux_associatifs.md` |
| Null value and nullable types | `02_null_et_nullable.md` |
| Operators and precedence | `05_operateurs.md` |
| Connecting an AI to a project | `25_agents_ia.md` |
| Debugging a program (DAP backend `owl-dap`) | `28_debogueur.md` |

For the complete list of native functions, composite types and
constants: `owl categories`, then `owl explain <name>`.
</content>
</invoke>
