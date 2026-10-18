# Exceptions

An exception interrupts the normal flow of a procedure to signal a
condition that the calling code must handle. OWL provides two
complementary forms to catch them, a block dedicated to the guaranteed
cleanup of resources, a function to raise them, and a small toolkit to
inspect or propagate the current exception.

The two main forms share the same capture mechanism but address two
different needs: protecting **a specific block** (specific form) or
**the whole remainder of a procedure** (general form).

---

## 1. Specific form: protecting a block

```owl
WHEN EXCEPTION IN
    n is an integer = EvaluateExpression(sFormula)
    nResult is an integer = 100 / n
DO
    Trace("Cannot compute: " + ExceptionInfo(errMessage))
END
```

The `IN … DO` block is the protected zone. If an exception is raised
there, execution jumps immediately to the `DO` block, which decides
what to do — often: trace, fall back to a default value, or propagate.

> Beware: not all natives raise an exception on error. Many (file
> reading, JSON parsing, SQL queries…) return a neutral value (empty
> string, Null…) and set `ErrorOccurred` to True. The `DO` block would
> not fire for that kind of failure. For these cases, use the
> soft-error pattern:
>
> ```owl
> content is a string = fLoadText("config.txt")
> IF ErrorOccurred THEN
>     Trace("Cannot read: " + ErrorInfo())
> ELSE
>     Trace("OK, " + Length(content) + " characters read")
> END
> ```
>
> Exceptions are reserved for genuinely blocking errors (division by
> zero, Null dereference, allocation failure…) or for errors explicitly
> promoted via `ExceptionThrow` or `ErrorPropagate()`.

When no exception is raised, the `DO` block is **not** executed, and
the program continues after the `END`. To distinguish the two outcomes
explicitly, add an `ELSE` block that runs only on success:

```owl
WHEN EXCEPTION IN
    bDone is a boolean = ProcessFile(sPath)
DO
    Trace("Failure on " + sPath)
ELSE
    Trace("Success on " + sPath)
END
```

After the `END`, the program resumes its normal course — whether an
exception was caught or not. The specific form is the tool of choice
when you want to continue the procedure after handling.

---

## 2. `FINALLY`: guaranteed cleanup

A `FINALLY` block placed before the `END` runs **always**, whatever
the outcome of the protected block:

```owl
f is an integer
WHEN EXCEPTION IN
    f = fOpen("data.txt", foRead)
    content is a string = fReadLine(f)
DO
    Trace("Cannot read: " + ExceptionInfo(errMessage))
FINALLY
    fClose(f)              // runs whether we read or crashed
END
```

This is the mechanism to use as soon as a resource must be released
reliably: an open file, a held lock, a started transaction, a
connection to free. Without `FINALLY`, that cleanup would have to be
duplicated manually in the `DO` block **and** after the `END`, which is
a classic source of leaks.

### Syntax and order of clauses

`FINALLY` is optional and comes **after** any `DO` and `ELSE`. The
order of clauses is strict:

```
WHEN EXCEPTION IN <try>
    [DO      <handler>]
    [ELSE    <else>]
    [FINALLY <cleanup>]
END
```

All clauses are individually optional, but the `IN` block is useless
without at least one of them. Only one `FINALLY` is allowed per block.

### Four paths, a single cleanup

Whatever the execution path, the `FINALLY` block runs:

| Path | Trigger | Order |
|---|---|---|
| Success | no exception raised | `IN` → (`ELSE` if present) → `FINALLY` |
| Caught | exception raised and caught by `DO` | `IN` (partial) → `DO` → `FINALLY` |
| Passes through | exception raised with no `DO` to catch it | `IN` (partial) → `FINALLY` → propagation |
| Re-raised | `DO` re-raises via `EXCEPTION` | `IN` (partial) → `DO` (partial) → `FINALLY` → propagation |

On the last two paths, the `FINALLY` runs **before** the exception
rises to the parent scope: the cleanup happens, then the exception
continues on its way.

### Cleanup must not interrupt the flow

The `FINALLY` block cannot contain any statement that alters the flow
of the procedure: these forms are rejected at compile time.

| Forbidden in `FINALLY` | Why |
|---|---|
| `RESULT`, `RETURN` | would silently overwrite the return of the `IN` or `DO` |
| `BREAK`, `CONTINUE` | would mask the procedure's loop control |
| `EXCEPTION` (re-raise) | would mask the exception being propagated |
| `ExceptionThrow` | likewise, and would prevent the original diagnostic |

Compile-time diagnostic: *“'<keyword>' is not allowed inside a
'FINALLY' block. Cleanup must not interrupt the flow.”*

This discipline guarantees that the protected block **always** decides
the result of the procedure: `FINALLY` cleans up, full stop. If the
cleanup itself may fail (e.g. closing a network connection that times
out), wrap the risky call in a nested `WHEN EXCEPTION IN` sub-block
inside the `FINALLY`.

### Restricted to the specific form

`FINALLY` only makes sense with the `WHEN EXCEPTION IN` form. The
compiler rejects it in the general form (see section 3): that one
already ends the procedure after its handler, so there is nothing to
clean up between the handler and any hypothetical following code.

---

## 3. General form: a net for the rest of the procedure

```owl
PROCEDURE ProcessBatch(t is an array of strings)
    WHEN EXCEPTION
        Trace("Batch interrupted: " + ExceptionInfo(errMessage))
        RESULT
    END
    FOR EACH s OF t
        ProcessOne(s)
    END
END
```

`WHEN EXCEPTION … END` without `IN` declares a handler that covers
**all the rest** of the current procedure. Any exception raised after
this point — directly or from a called function — is captured.

**The procedure ends after the handler runs.** The code written after
the handler's `END` does not run when the handler has fired; it runs
only on the normal flow, that is, when no exception is raised.

```owl
PROCEDURE ProcEcho(s is a string) : string
    WHEN EXCEPTION
        RESULT "[error] " + ExceptionInfo(errMessage)
    END
    // Normal flow: the handler is NOT called.
    RESULT "echo:" + s
END
```

This automatic termination avoids the class of bugs where an exception
would be caught then silently re-raised by the following code (for
instance because it depends on a variable that was not initialised
correctly).

If code must run after the handler, two options:

- write it **before** the handler declaration;
- use the specific form `WHEN EXCEPTION IN … DO`, which naturally
  resumes the flow after the block.

### Only one general handler active at a time

A procedure may declare several successive general handlers. From the
line following a handler's `END`, **only the last declared one** is
active. The next declaration replaces the previous one for the rest of
the procedure.

```owl
PROCEDURE Step()
    WHEN EXCEPTION
        Trace("preparation: error")
        RESULT
    END
    Prepare()      // error here -> handler 1

    WHEN EXCEPTION
        Trace("execution: error")
        RESULT
    END
    Execute()      // error here -> handler 2 (not handler 1)
END
```

---

## 4. Raising an exception

`ExceptionThrow` raises an exception with an optional code and message:

```owl
ExceptionThrow("File not found")
ExceptionThrow(404, "Resource missing")
ExceptionThrow(2)
```

An optional third argument carries additional information distinct from
the message — typically a machine-readable context (key/value, short
JSON) intended for a handler that wants to make a conditional decision:

```owl
ExceptionThrow(404, "Resource missing", "url=/api/users/42")
```

The handler retrieves it via `ExceptionInfo(errInfo)`. Without a third
argument, `errInfo` returns an empty string.

The parameters are independent: you can supply a message alone, a code
alone, the code and the message, or all three together. Without an
argument, the exception carries a generic message.

The exception climbs the call stack up to the first handler that can
process it. If no handler captures it, execution stops with an error
message including the call trace.

---

## 5. Inspecting the current exception

From a handler, `ExceptionInfo` returns the details of the exception
just caught. Without an argument, it returns the message. With an
`errXxx` constant, it returns the corresponding field:

```owl
WHEN EXCEPTION IN
    ExceptionThrow(42, "boom")
DO
    Trace(ExceptionInfo())                  // "boom"
    Trace(ExceptionInfo(errCode))           // 42
    Trace(ExceptionInfo(errMessage))        // "boom"
    Trace(ExceptionInfo(errLine))           // line number of the throw
    Trace(ExceptionInfo(errStack))          // formatted call trace
END
```

The available constants:

| Constant | Content |
|---|---|
| `errMessage` | Full message of the exception (string) |
| `errCode` | Numeric code of the exception (integer) |
| `errLine` | Source line where the exception was raised (integer) |
| `errElement` | Name of the source file where the raise happened (string) |
| `errProcess` | Name of the current procedure at the moment of the raise (string) |
| `errDuring` | Name of the function that actually triggered the exception (string) |
| `errStack` | Formatted call trace (multi-line string) |
| `errSummary` | First line of the message, truncated to 80 characters (string) |
| `errInfo` | Additional information passed as third argument to `ExceptionThrow`, otherwise an empty string |
| `errDateTime` | Unix millisecond timestamp of the raise (integer) |
| `errFullDetails` | All the information concatenated (string) |
| `errSystemCode` | System error code if the origin provided one, 0 otherwise (integer) |
| `errSystemMessage` | System error message if the origin provided one, empty otherwise (string) |
| `errNumberSubError` | Number of sub-errors (integer, currently always 0) |

### Scope of the exception context

The context read by `ExceptionInfo` follows the current handler
strictly:

- **Inside a handler**: `ExceptionInfo` describes the exception caught
  by that specific handler (message, code, line, etc.).
- **Outside any handler**: the context is empty. `ExceptionInfo()` and
  `ExceptionInfo(errMessage)` return an empty string,
  `ExceptionInfo(errCode)` returns `0`, and so on. No residual data
  from the last processed exception persists after the handler's `END`.
  This is intentional: a logging helper can run indifferently in both
  contexts without raising an error or reading stale values.
- **Nested handlers**: when a handler contains another, the inner
  context applies as long as the inner handler runs. On leaving the
  inner handler, the enclosing handler's context is restored: the reads
  that follow, still inside the outer handler, find the outer exception
  again, not the now-closed one of the inner block.

```owl
WHEN EXCEPTION IN
    ExceptionThrow(7, "outer")
DO
    Trace(ExceptionInfo(errCode))       // 7

    WHEN EXCEPTION IN
        ExceptionThrow(3, "inner")
    DO
    END

    Trace(ExceptionInfo(errCode))       // 7 — the outer context is restored
END
Trace(ExceptionInfo(errCode))           // 0 — outside a handler, empty context
```

---

## 6. Propagating to the upper level

A handler may decide that it does not know how to process the exception
and let it rise. Two equivalent forms:

```owl
WHEN EXCEPTION IN
    Action()
DO
    IF ExceptionInfo(errCode) = 99 THEN
        Trace("local handling")
    ELSE
        EXCEPTION              // re-raises the current exception
    END
END
```

The `EXCEPTION` keyword alone, inside a handler, re-raises the current
exception with its message, code and context intact. The same thing can
be written as a function:

```owl
ExceptionPropagate()
```

The two forms are strictly equivalent. The keyword is more readable
when chaining a conditional decision; the function can be handy to pass
it as an argument to a helper.

---

## 7. Forbidden inside a loop

The general form `WHEN EXCEPTION` (without `IN`) cannot be declared
inside a `FOR`, `FOR EACH`, `WHILE` or `LOOP` loop. The compiler
rejects it:

```owl
FOR i = 1 TO 10
    WHEN EXCEPTION           // compile ERROR
        Trace("oops")
    END
    Process(i)
END
```

Compile-time diagnostic: *“'WHEN EXCEPTION' (general form) not allowed
inside a loop. Use 'WHEN EXCEPTION IN ... DO ... END' to protect a
specific block.”*

The reason is semantic: the general form ends the procedure after its
handler. Placed in a loop, its behaviour would be misleading (a single
iteration catches, the rest goes through the code following the
handler's `END`, which is never meant to be reached).

Two replacements depending on the intent:

- To protect **each iteration** independently, use the specific form:

  ```owl
  FOR EACH s OF list
      WHEN EXCEPTION IN
          Process(s)
      DO
          Trace("Failure on " + s)
      END
  END
  ```

- To protect **the whole procedure**, declare the handler **before**
  the loop:

  ```owl
  PROCEDURE ProcessAll(list is an array of strings)
      WHEN EXCEPTION
          Trace("Batch interrupted")
          RESULT
      END
      FOR EACH s OF list
          Process(s)
      END
  END
  ```

---

## 8. Choosing between the two forms

| Need | Suitable form |
|---|---|
| Protect a specific line or block, continue afterwards | `WHEN EXCEPTION IN … DO … END` |
| Protect the whole rest of a procedure, end on error | `WHEN EXCEPTION … END` |
| Release a resource whatever the outcome | `WHEN EXCEPTION IN … FINALLY … END` |
| Protect each iteration of a loop | Specific form inside the loop |
| Global net for a procedure that contains a loop | General form, declared before the loop |

---

## 9. Common pitfalls

- **Code after the `END` of a general handler**: this code runs only on
  the normal flow (no exception raised). If you want it to run after an
  error has been handled too, write the logic in the handler itself or
  use the specific form.
- **Catching an exception then forgetting it**: an empty handler
  (`WHEN EXCEPTION … END` that does nothing) silently swallows all
  errors. Reserve this for cases where the absence of a reaction is a
  conscious, documented choice.
- **`EXCEPTION` outside a handler**: the keyword alone is only
  recognised inside the body of a `WHEN EXCEPTION` or `DO`. Elsewhere,
  the compiler rejects it.
- **Risky cleanup in `FINALLY`**: closing a resource can itself fail. If
  that is likely (network, remote IO), wrap the call in a
  `WHEN EXCEPTION IN` sub-block inside the `FINALLY` rather than letting
  a second exception mask the first.

---

## 10. English aliases

| French | English |
|---|---|
| `QUAND EXCEPTION … FIN` | `WHEN EXCEPTION … END` |
| `QUAND EXCEPTION DANS … FAIRE … [SINON …] [ENFIN …] FIN` | `WHEN EXCEPTION IN … DO … [ELSE …] [FINALLY …] END` |
| `ENFIN` | `FINALLY` |
| `EXCEPTION` (re-raise keyword) | `EXCEPTION` |
| `ExceptionDéclenche` | `ExceptionThrow` |
| `ExceptionInfo` | `ExceptionInfo` |
| `ExceptionPropage` | `ExceptionPropagate` |

---

## Going further

| Topic | Page |
|---|---|
| General language overview | `00_apercu_du_langage.md` |
| Control flow (`SI`, `POUR`, `TANTQUE`…) | `03_controle_de_flux.md` |
| Code conventions | `10_conventions_de_code.md` |
| Test charter (assertions, scenarios) | `11_charte_de_test.md` |

For the list of available `errXxx` constants: `owl explain
ExceptionInfo`. For the details of an Exception\* function:
`owl explain ExceptionDéclenche`, `owl explain ExceptionPropage`, etc.
