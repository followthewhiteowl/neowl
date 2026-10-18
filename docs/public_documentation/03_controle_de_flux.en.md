# Control flow

OWL provides six control constructs: `IF`, `FOR`, `FOR EACH`,
`WHILE`, `LOOP` and `SWITCH`. All of them end with `END`. Conditions
must be boolean, numeric or `Null` — any other value produces an
explicit runtime error (see `01_types_et_coercions.md` §2).

---

## 1. `IF`

```owl
IF age >= 18 THEN
    Trace("Adult")
ELSE
    Trace("Minor")
END
```

The `ELSE` block is optional. Several branches chain together with
`ELSE IF`:

```owl
IF score >= 16 THEN
    Trace("Excellent")
ELSE IF score >= 10 THEN
    Trace("Passed")
ELSE
    Trace("Failed")
END
```

> **Pitfall — `ELSE IF` is an implicit chaining, not a nesting.**
> The whole cascade `IF ... ELSE IF ... ELSE IF ... ELSE ... END` is
> a single structured block: **a single `END`** closes the complete
> chain, whatever its length. Do not confuse it with a form where
> each `ELSE` would open a new nested `IF` requiring its own `END`.
> The following form is **wrong**:
>
> ```owl
> IF score >= 16 THEN
>     Trace("Excellent")
> ELSE
>     IF score >= 10 THEN           // IF nested in the ELSE
>         Trace("Passed")
>     END                          // END of the inner IF
> END                              // END of the outer IF
> ```
>
> It compiles but it is no longer a cascade: it is a nesting. The
> canonical `ELSE IF ... THEN ...` reads as a single linear switch,
> shorter to write and more readable to share.

**Short single-line form**: when each branch is a single statement,
`END` may be omitted. Handy for guards or binary choices:

```owl
IF sName = "" THEN RESULT
IF i > limit THEN BREAK
IF nScore >= 10 THEN sGrade = "pass" ELSE sGrade = "fail"
```

**`IN` operator**: `value IN (e1, e2, ...)` tests membership in an
enumerated list. More readable than a cascade of `=`/`OR`. The
negation is written `NOT IN`:

```owl
IF sStatus IN ("draft", "validated", "archived") THEN
    Trace("known status")
END

IF sCommand NOT IN ("SELECT", "INSERT", "UPDATE") THEN
    Trace("forbidden command")
END
```

The list cannot be empty (compile error). Mixed types are tolerated;
the comparison follows the equality rules of `05_operateurs.md`.

---

## 2. `FOR` (indexed loop)

```owl
FOR i = 1 TO 5
    Trace(i)
END
```

The bounds are **inclusive**: `1 TO 5` iterates 1, 2, 3, 4, 5. The
counter `i` is automatically declared, typed `integer`, and **local
to the block** — it no longer exists after `END`.

> **Note**: the keyword between the initial bound and the final bound
> may be written `A`, `a`, `À`, `à` or `to` interchangeably — all these
> forms are equivalent and recognized by the scanner. The normative
> form is `TO`. In return, these forms are **reserved** and cannot
> serve as a variable name.

**Step**: add `STEP` after the bounds. The value may be negative to
count down.

```owl
FOR i = 10 TO 0 STEP -2
    Trace(i)              // 10, 8, 6, 4, 2, 0
END
```

When `start > end` without a negative `STEP`, the loop is skipped
without error.

**Reverse traversal**: `DepuisFin` at the end is equivalent to
inverting the bounds with `STEP -1`. `DepuisDébut` (the default) may be
written explicitly for readability.

```owl
FOR i = 1 TO 5 DepuisFin
    Trace(i)                  // 5, 4, 3, 2, 1
END
```

`DepuisFin` and `STEP` are mutually exclusive: use one or the other,
not both.

---

## 3. `FOR EACH` (collection traversal)

Traversal of an ordinal array:

```owl
tFruits is an array of strings = ["apple", "pear", "banana"]
FOR EACH sFruit OF tFruits
    Trace(sFruit)
END
```

`FOR EACH`, `FOR ALL` and `FOR EVERY` are **strict synonyms**. The
choice is a matter of style and natural phrasing (“for each user”,
“for all elements”).

With a **second variable**, you also get the index on an ordinal
array, or the key on an associative array:

```owl
FOR EACH sFruit, nIndex OF tFruits
    Trace(nIndex + ". " + sFruit)         // 1. apple  /  2. pear  /  3. banana
END

tabUsers is an associative array of strings
tabUsers["alice"]  = "admin"
tabUsers["bob"]    = "reader"
tabUsers["carol"]  = "admin"

FOR EACH sRole, sLogin OF tabUsers
    IF sRole = "admin" THEN Trace(sLogin)
END
// Prints: alice  /  carol
```

**Reverse traversal**: `DepuisFin` after the iterable. On an ordinal
array as well as an associative one.

```owl
FOR EACH sFruit OF tFruits DepuisFin
    Trace(sFruit)             // banana, pear, apple
END
```

**Inline `WITH` filter**: equivalent to a `IF NOT cond THEN CONTINUE`
injected at the top of the body. More readable when you filter then
process.

```owl
tabTemp is an array of integers = [12, 25, 18, 30, 22, 8]
FOR EACH t OF tabTemp WITH t > 20
    Trace("hot:", t)          // 25, 30, 22
END
```

**Deletion during iteration**: `Delete(t, CurrentElement)` is safe.
The loop visits each element exactly once, even if the current one is
removed during the body.

```owl
// Keep only the administrators.
FOR EACH sRole, sLogin OF tabUsers
    IF sRole <> "admin" THEN Delete(tabUsers, CurrentElement)
END
```

### Traversing a string

Two specific variants avoid going through an intermediate array.

**Split by separator** — `FOR EACH string`:

```owl
FOR EACH string sColor OF "red,green,blue" SEPARATED BY ","
    Trace(sColor)             // red, green, blue
END
```

Extended form with 3 variables: substring, start position in the
source (1-based), and iteration number (1-based):

```owl
FOR EACH string sc, nPos, nIdx OF "A.B.C" SEPARATED BY "."
    Trace(nIdx + ": " + sc + "@" + nPos)   // 1: A@1  /  2: B@3  /  3: C@5
END
```

**Occurrences of a pattern** — `FOR EACH Position`:

```owl
FOR EACH Position p OF ";" IN "a;b;c;d"
    Trace(p)                  // 2, 4, 6
END
```

### Traversing a JSON document

`FOR EACH` distinguishes at runtime a JSON object from a JSON array,
and adapts the meaning of the iteration variables accordingly.

**JSON object** — the two-variable form exposes the real key/value
pairs of the members, in document order. The first variable receives
the member's value, the second its name, exactly as on an associative
array.

```owl
oConfig is a JSON = StringToJSON(`{"name":"owl","lang":"en"}`)
FOR EACH sValue, sKey OF oConfig
    Trace(sKey + " = " + sValue)   // name = owl  /  lang = en
END
```

The single-variable form iterates the members' values (neither the
keys nor an index).

```owl
oScores is a JSON = StringToJSON(`{"a":10,"b":20,"c":30}`)
nSum is an integer = 0
FOR EACH nValue OF oScores
    nSum = nSum + nValue            // 60
END
```

**JSON array** — the traversal is over the elements. The
single-variable form gives each element; the second variable receives
the positional index (1-based), as on an ordinal array.

```owl
tMeasures is a JSON = StringToJSON(`[10, 20, 30]`)
FOR EACH nElem OF tMeasures
    Trace(nElem)                    // 10, 20, 30
END

FOR EACH nElem, nIndex OF tMeasures
    Trace(nIndex + ": " + nElem)    // 1: 10  /  2: 20  /  3: 30
END
```

---

## 4. `WHILE`

```owl
n is an integer = 5
WHILE n > 0
    Trace(n)
    n--
END
```

The condition is checked **before** each iteration. If it is false at
the start, the body is never executed.

---

## 5. `LOOP`

Without an argument, an unconditional loop — the exit is done from
inside the body with `BREAK`. Useful for parsing, stream reading, or
state machines.

```owl
n is an integer = 0
LOOP
    n = n + 1
    Trace(n)
    IF n >= 3 THEN BREAK
END
```

**Explicit counter**: `LOOP (N)` runs the body exactly `N` times. `N`
is an expression evaluated **only once** at startup. `N ≤ 0` produces
zero iterations, without error.

```owl
LOOP (3)
    Trace("ping")             // ping  /  ping  /  ping
END
```

`BREAK` and `CONTINUE` work as in any loop. On `CONTINUE`, the turn is
still counted.

---

## 6. `SWITCH`

Dispatch by filtering. `SWITCH` evaluates its expression **only
once**, then looks for the first `CASE` that matches.

```owl
SWITCH status
    CASE "active"             : Trace("OK")
    CASE "suspended", "closed" : Trace("Inactive")
    OTHER CASE                : Trace("Unknown")
END
```

A `CASE` accepts several patterns separated by commas. The patterns
can take five forms:

**Simple value** — direct equality:
`CASE 1`, `CASE "active", "valid"`.

**Range** — inclusive bounds:
`CASE 1 TO 10` (between 1 and 10), `CASE TO 8` (≤ 8), `CASE 11 TO`
(≥ 11). A range `min > max` never matches, without error.

**Prefix comparator** — on the tested value:
`CASE > 100`, `CASE <= 0`, `CASE <> 42`. On strings, the lenient
operators are available: `CASE [= "Hello"` (starts with),
`CASE [=] "error"` (contains), `CASE ~= "hello"` (case-insensitive
equality), etc. Complete list in `05_operateurs.md`.

**Sandwich** — min bound + placeholder `*` + max bound:
`CASE 10 < * < 20` (strict on both sides),
`CASE 10 <= * <= 20` (inclusive). The two operators may be mixed.

**`SWITCH True` idiom** — for complex conditions:

```owl
SWITCH True
    CASE score >= 16              : Trace("Excellent")
    CASE score >= 10 AND passed   : Trace("Passed")
    OTHER CASE                    : Trace("Failed")
END
```

**No fall-through**: the first `CASE` that matches runs its block,
then exits the `SWITCH`. `OTHER CASE` is optional and must come last.

The colon after the pattern is **optional**: `CASE 1 : ...` and
`CASE 1 ...` are equivalent.

The `CASE` values are compared with the usual coercion rules (cf.
`05_operateurs.md` §2 and `01_types_et_coercions.md` §2).

---

## 7. `BREAK` and `CONTINUE`

`BREAK` leaves the current loop. `CONTINUE` skips to the next
iteration (counter increment for `FOR`, re-evaluation of the condition
for `WHILE`, back to the top for `LOOP`).

```owl
// Skip the even ones
FOR i = 1 TO 5
    IF i modulo 2 = 0 THEN CONTINUE
    Trace(i)                          // 1, 3, 5
END
```

Both are **loop-only statements**: used outside a loop, they produce a
compile error. `BREAK` does **not** allow exiting a procedure — for
that, use `RETURN` or `RESULT` (cf §8).

### Simple exit — directly enclosing loop

By default, `BREAK` leaves the nearest loop:

```owl
FOR i = 1 TO 2
    FOR j = 1 TO 5
        IF j > 2 THEN BREAK          // exits the FOR j only
        Trace(i + " " + j)
    END
END
// Output: 1 1, 1 2, 2 1, 2 2
```

### Multi-level exit — `BREAK(N)`

To exit N nested loops at once, pass the number of levels to climb
between parentheses (`N` must be an integer literal between 1 and the
current nesting depth):

```owl
FOR x = 1 TO 15
    FOR y = 1 TO 25
        FOR z = 1 TO 50
            IF criticalCondition THEN
                BREAK(2)             // exits loops z AND y
                                     // → continues with next x
            END
        END
    END
END
```

`BREAK(1)` is equivalent to a plain `BREAK`. `BREAK(N)` with N >
depth of the enclosing loops produces a compile error.

### `EXIT SWITCH` — leaving a `SWITCH` without leaving the loop

```owl
SWITCH quantity
    CASE 1
        IF user = "Admin" THEN EXIT SWITCH
        Trace("Promo: 2 products for the price of one")
    OTHER CASE : Trace("Standard")
END
```

`EXIT SWITCH` placed outside a `SWITCH` produces a compile error. A
plain `BREAK` inside a `SWITCH` placed in a loop leaves **the loop**,
not the `SWITCH`.

---

## 8. `RETURN` and `RESULT` — leaving a procedure

A procedure ends either by reaching the end of its body, or explicitly
via `RETURN` or `RESULT`. The two keywords are functionally equivalent
(synonyms for an immediate exit with a value) but **cannot be mixed**
in the same procedure.

### Form with a value

```owl
PROCEDURE Square(n is an integer) : integer
    RESULT n * n
END
```

Or equivalently:

```owl
PROCEDURE Square(n is an integer) : integer
    RETURN n * n
END
```

### Form without a value (action procedure)

```owl
PROCEDURE Greet(name is a string)
    IF name = "" THEN RESULT
    Trace("Hello " + name)
END
```

### Multiple return values

```owl
PROCEDURE MinMax(t is an array of integers) : (integer, integer)
    RESULT (Min(t), Max(t))
END

(low, high) = MinMax(myArray)
```

### Exclusion rule — no mixing of `RETURN` / `RESULT`

A procedure may use only one of the two keywords. The compiler
enforces this rule:

```owl
PROCEDURE Bad(x is an integer) : integer
    IF x > 0 THEN
        RESULT x * 2            // ← first kind locked: RESULT
    END
    RETURN 0                    // ← compile ERROR: mixing forbidden
END
```

Compile-time diagnostic: *“Forbidden mix of 'RESULT' / 'RETURN' in the
same procedure. First 'RESULT' seen at line X. Choose a single keyword
for the procedure.”*

**Tip**: `RESULT` is the preferred form for functions that return a
value, `RETURN` for action procedures that exit without a value. The
discipline helps reading the code.

---

## 9. English aliases

All constructs have a French and an English equivalent. The language
is strictly bilingual: a file may mix the two forms without
consequence.

| French | English |
|---|---|
| `SI ... ALORS ... SINON ... FIN` | `IF ... THEN ... ELSE ... END` |
| `POUR i = 1 A 5 PAS 1 ... FIN` | `FOR i = 1 to 5 STEP 1 ... END` |
| `POUR TOUT`, `POUR TOUS`, `POUR CHAQUE` | `FOR EACH`, `FOR ALL`, `FOR EVERY` |
| `... DE ...` | `... OF ...` |
| `TANTQUE ... FIN` | `WHILE ... END` |
| `BOUCLE ... FIN`, `BOUCLE (N) ... FIN` | `LOOP ... END`, `LOOP (N) ... END` |
| `SELON ... CAS ... AUTRE CAS ... FIN` | `SWITCH ... CASE ... DEFAULT ... END` |
| `SORTIR`, `CONTINUER`, `SORTIR SELON` | `BREAK`, `CONTINUE`, `EXIT SWITCH` |
| `DANS`, `AVEC` | `IN`, `WITH` |

For the style to adopt in a project, see `10_conventions_de_code.md`.

---

## 10. Common pitfalls

- **String or array in a condition**: `IF s THEN` is rejected at
  runtime. Test explicitly: `IF Length(s) > 0 THEN`,
  `IF t..Occurrence > 0 THEN` (double dot for properties).
- **Inverted bounds without a negative `STEP`**: `FOR i = 5 TO 1`
  loops zero times. No error, no warning. Check the intent.
- **`DepuisFin` and `STEP` combined**: rejected at compilation. Choose
  one or the other — `DepuisFin` already imposes an implicit step of
  -1.
- **`FOR` counter outside the block**: `i` no longer exists after
  `END`. To keep the final value, declare an external variable and
  assign it in the body.
- **`BREAK` from a `SWITCH`**: leaves the enclosing loop, not the
  `SWITCH`. To leave only the `SWITCH`, use `EXIT SWITCH` (there is no
  “fall-through”, but a `CASE` can be short-circuited across several
  branches).
- **Real bound in `FOR`**: the bounds are expected to be integers. A
  real is truncated to the integer. To iterate over reals (rare), use
  `WHILE`.
- **Empty list for `IN`**: `v IN ()` is rejected at compilation.

---

## Going further

| Topic | Page |
|---|---|
| General language overview | `00_apercu_du_langage.md` |
| Types and conversions (boolean conditions) | `01_types_et_coercions.md` |
| Operators and precedence | `05_operateurs.md` |
| Procedures, parameters, return | `04_procedures.md` (upcoming) |
| Exceptions and propagation | `08_exceptions.md` |
| Code conventions | `10_conventions_de_code.md` |
