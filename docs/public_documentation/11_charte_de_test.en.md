# Testing OWL code

The language provides a built-in framework for writing and running unit
tests: a conventional file structure, fixtures, assertions, mocking of
side-effecting functions, and a **headless mode** that automatically
neutralizes graphical interface popups under `owl test`.

Two principles:

1. **A test is also documentation.** A well-named scenario, with a
   description sentence and clear assertions, serves as a reference for a
   reader discovering the function.
2. **The code and the assertion are the truth, not the comment.** A comment
   that drifts out of sync is annoying; an assertion that drifts out of sync
   breaks the test.

---

## 1. One file per symbol

A test file documents **a single symbol**: a function, a type, a class, a
non-trivial enumeration. The file name echoes the symbol:
`test_string_split.owl`, `test_cFile.owl`, `test_http_request.owl`.

When a function and a prefixed method carry the same semantics (for example a
global form `StringToDate(s)` and a method form `s.ToDate()`), they share
**the same file**. The internal scenarios choose one form or the other
depending on what they illustrate.

The `owl test` runner walks the tree recursively and runs **every** file
named `test_*.owl` it finds there. The sub-folder hierarchy is free.

---

## 2. File header

A header comment, optional but recommended, opens the file and sums up in one
sentence the symbol under test. It is a landmark for the reader who opens the
file without context.

```owl
// Tests of StringSplit: split on a separator, returns an array.
```

---

## 3. Scenarios

Each scenario is a procedure whose name begins with one of the three accepted
prefixes: `scenario_`, `test_`, `should_`. All three are equivalent for the
runner; choosing one or the other is a matter of style, not of meaning.

```owl
// Split a simple CSV line on the comma.
PROCEDURE scenario_csv_simple()
    parts is an array of strings = StringSplit("a,b,c", ",")
    dbgCheckEqual(parts..Occurrence, 3)
    dbgCheckEqual(parts[1], "a")
    dbgCheckEqual(parts[3], "c")
END
```

The comment preceding the procedure is the **description** of the scenario.
A single sentence. It will appear in the generated documentation.

A scenario must:

- Test **a single** named behaviour. If the scenario does two things, then it
  needs to be two.
- Contain at least one assertion (`dbgCheck*`). Without an assertion, it is an
  example, not a test.
- Be readable in isolation. A reader who has seen only the procedure must
  understand what is being tested.

---

## 4. Fixtures

Four procedure names are reserved for fixtures. Their presence is optional; if
present, the runner runs them at the expected moments.

| French form | English form | Execution |
|---|---|---|
| `avantToutTest` | `beforeAllTests` | Once before all the scenarios of the file |
| `apresToutTest` | `afterAllTests` | Once after all of them |
| `avantChaqueTest` | `beforeEachTest` | Before each scenario |
| `apresChaqueTest` | `afterEachTest` | After each scenario |

Both forms are equivalent. Mixing FR and EN in the same file is tolerated but
discouraged.

```owl
// Tests of the .Upper method: conversion to uppercase.

s is a string

PROCEDURE beforeEachTest()
    s = ""
END

PROCEDURE scenario_ascii_simple()
    s = "hello"
    dbgCheckEqual(s.Upper(), "HELLO")
END

PROCEDURE scenario_empty_string()
    dbgCheckEqual(s.Upper(), "")
END
```

---

## 5. Public and private scenarios

By default, a scenario is **public**: it is exposed by the discovery commands
(`owl examples`, `owl explain`) as a reference example.

A scenario that does not deserve to be an example (a test of a defensive or
structural detail) is declared with the `PRIVATE` modifier:

```owl
// Preserve non-ASCII characters when the separator is non-ASCII.
PROCEDURE PRIVATE scenario_unicode_separator()
    parts is an array of strings = StringSplit("a→b→c", "→")
    dbgCheckEqual(parts..Occurrence, 3)
END
```

The runner runs `PRIVATE` scenarios exactly like the others. Only the
documentation extractor ignores them.

---

## 6. Assertions

The assertion functions are provided by the debugging module. Canonical form
with accents (the ASCII form without accents is accepted when writing, the
formatter restores the canon):

| Function | Checks |
|---|---|
| `dbgCheckTrue(c, message?)` | `c` is true |
| `dbgCheckFalse(c, message?)` | `c` is false |
| `dbgCheckNull(v, message?)` | `v` is null |
| `dbgCheckNotNull(v, message?)` | `v` is not null |
| `dbgCheckEqual(a, b, message?)` | Strict equality |
| `dbgCheckEqualReal(a, b, epsilon, message?)` | Equality of reals within epsilon |
| `dbgCheckDifferent(a, b, message?)` | `a` and `b` are different |

`dbgTestSummary()` prints the overall summary of the file's assertions.

The final message is optional but recommended: it appears in the report on
failure and helps locate the problem without re-reading the file.

---

## 7. Testing side-effecting functions

Some native functions have an observable effect on the outside world — a
network call, a file read, a read of the system clock. Testing these
functions without precaution makes scenarios:

- **slow** (a real HTTP call costs hundreds of milliseconds),
- **fragile** (the test passes or fails depending on connectivity, the time,
  the machine),
- **non-reproducible** (the remote target may change its response).

The language provides a universal mock system that intercepts any native.
Simple rule: **every side-effecting function must be mocked in a test**.

```owl
// Tests of HTTPRequest: running an HTTP request and reading the response.

PROCEDURE scenario_get_simple()
    mockHTTP is a MockNative = MockCreate(HTTPRequest)
    MockWhen(mockHTTP)
    MockWith(mockHTTP, MockAny)
    MockReturns(mockHTTP, "simulated response")
    MockActivate(mockHTTP)

    response is a string = HTTPRequest("http://example.invalid/")
    dbgCheckEqual(response, "simulated response")

    MockDeactivate(mockHTTP)
    MockVerifyCount(mockHTTP)
END
```

The Mock system covers response sequences, verification of precise arguments,
counting the number of calls and negation. See `owl explain MockCreate` for
the full signature.

---

## 8. Headless mode: testing without a graphical interface

When a test exercises code that opens a window, displays a message or waits
for a user click, making the real graphical interface appear on each run
breaks automation.

The language offers a **headless mode**: a switch that neutralizes the
graphical interface functions. Popups (`Info`, `Error`, `Warning`) do not
show; window openings (`Open`, `OpenChild`) run the initialization code then
close without an event loop; confirmation dialogs (`Confirm`, `YesNo`) return
their default value.

### Automatic activation under `owl test`

Under `owl test`, headless mode is **enabled by default** at the start of each
test file. The author of a test has nothing to do: `Info(...)` or
`Warning(...)` in the code under test do not block the flow of the scenario.

### Explicit control

The `TestHeadlessMode` native lets you read and change the current state:

```owl
TestHeadlessMode()           // returns the current state (boolean)
TestHeadlessMode(True)       // enables headless mode
TestHeadlessMode(False)      // disables it — the real interface shows
```

Default state:

| Context | `TestHeadlessMode()` at start |
|---|---|
| `owl test` | `True` (reset for each file) |
| `owl` (application mode) | `False` (normal interface) |

### Typical use cases

**Checking that an interface function was called** — combine headless mode
(already active under `owl test`) and `MockNative`:

```owl
// Tests of Info: display of a modal information box.

PROCEDURE scenario_info_called_twice()
    mockInfo is a MockNative = MockCreate(Info)
    MockActivate(mockInfo)

    Info("first message")
    Info("second message")

    MockVerifyCount(mockInfo, 2)
    MockDeactivate(mockInfo)
END
```

**Running the real interface occasionally** — useful to visually validate a
complex behaviour during development:

```owl
PROCEDURE scenario_real_dialog()
    TestHeadlessMode(False)       // enable the interface for this scenario
    Info("this popup really shows")
    TestHeadlessMode(True)        // restore headless mode
END
```

### Limits

- Headless mode **does not record** the history of calls. To check that a
  function was called with a given argument, use `MockNative`.
- Headless mode applies only to interface natives. Application code
  (computations, data access, business logic) runs normally.

---

## 9. Simulating user actions

Headless mode neutralizes popups, but you also need to be able to **trigger**
the behaviours a real user would have caused: a click on a button, an entry in
an input, and so on. Without that, the `PROCEDURE Click()`,
`PROCEDURE Modification()`, `PROCEDURE Validation()` blocks would stay
orphaned, never covered by the tests.

The language provides the `TestSimulate*` family for this. Each native takes
the targeted control as its first argument and triggers the matching callback
directly on the object, without going through the graphical layer. The result
is **deterministic**: whether in headless mode or in real-interface mode, the
callback runs the same way.

### `TestSimulateMouse`

Triggers the `Click` or `DoubleClick` callback of a control.

```owl
TestSimulateMouse(<control>, <action>)
```

`<action>` is a value of the `mouseAction` enumeration:

| Value | Callback triggered |
|---|---|
| `LeftClick` | `Click` |
| `RightClick` | `Click` |
| `LeftDoubleClick` | `DoubleClick` |
| `RightDoubleClick` | `DoubleClick` |

If the control does not define the expected callback, the call is a silent
no-op: no error, no effect.

```owl
// Tests of Button.Click: triggering a button's Click callback.

flag is a boolean = False

FEN_Demo is a Window
    PROPERTIES
        Layout = dispFree
    END
    BTN_OK is a Control Button
        PROCEDURE Click()
            flag = True
        END
    END
END

PROCEDURE scenario_simulated_click()
    dbgCheckFalse(flag, "flag False at start")
    TestSimulateMouse(BTN_OK, LeftClick)
    dbgCheckTrue(flag, "Click callback triggered")
END
```

### `TestSimulateInput`

Writes a text into an input control **and** triggers its `Modification`
callback.

```owl
TestSimulateInput(<control>, <text>)
```

```owl
PROCEDURE scenario_simulated_input()
    modifiedValue is a string = ""

    FEN_Demo is a Window
        PROPERTIES
            Layout = dispFree
        END
        SAI_Name is a Control Input
            PROCEDURE Modification()
                modifiedValue = SAI_Name..Value
            END
        END
    END

    TestSimulateInput(SAI_Name, "Marc")
    dbgCheckEqual(SAI_Name..Value, "Marc")
    dbgCheckEqual(modifiedValue, "Marc")
END
```

### `TestSimulateMenu`

Triggers the selection of an option in a main menu, designating the option by
its name. The `onSelection` callback of the targeted option runs as if the
user had chosen it in the menu.

```owl
TestSimulateMenu(<window>, <option name>)
```

The first argument is the window whose main menu is driven, the second the
name of the `MenuOption` to trigger. The search walks the whole menu tree,
including nested submenus: the name alone is enough, with no need to qualify
the path.

```owl
PROCEDURE scenario_simulated_menu()
    Open(FEN_Demo)
    TestSimulateMenu(FEN_Demo, "OPT_New")
    dbgCheckEqual(counterNew, 1, "onSelection triggered once")
    Close(FEN_Demo)
END
```

Unlike the mouse and the input, a target that cannot be found is an
**explicit error**, not a no-op: an option name absent from the tree, or a
window with no main menu assigned, sets `ErrorOccurred`.

### Difference with `MockNative`

`MockNative` intercepts a call to **replace** it or to **verify** that it took
place. `TestSimulate*` **triggers** an application behaviour as a user would
have done.

The two are complementary:

| Need | Tool |
|---|---|
| Check that `Info` was called 2 times | `MockNative` |
| Force `Confirm` to return `Yes` | `MockNative` |
| Trigger a button's `Click` callback | `TestSimulateMouse` |
| Enter text in a control + trigger `Modification` | `TestSimulateInput` |
| Trigger the selection of a menu option | `TestSimulateMenu` |
| Check the state of a control after simulation | direct property access (`BTN_OK..Visible`, `SAI_Name..Value`) |

### Current limits

- The family covers the mouse, the input and the menu options. Global
  keyboard shortcuts and gestures are not simulated.
- The position argument `(X, Y)` is not exposed: the callback is triggered
  without specific coordinates.
- Modifiers (`Shift`, `Ctrl`, `Alt`) are not exposed.
- `TestSimulate*` does **not** simulate a hardware event at the level of the
  graphical system: if a test needs to check the full event chain down to the
  display driver, this is not the right tool. For covering application
  callbacks, it is exactly what is needed.

---

## Further reading

| Topic | Reference |
|---|---|
| General overview of the language | `00_apercu_du_langage.md` |
| Coding conventions | `10_conventions_de_code.md` |

For the list of public scenarios of a given symbol: `owl examples <symbol>`.
