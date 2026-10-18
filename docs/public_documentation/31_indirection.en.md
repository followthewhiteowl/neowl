# Indirection

Indirection designates an element — a global variable, a control, a window —
by a **name computed at run time** rather than written literally. The name, a
string expression, is placed between braces: `{ expression }`. The form works
for reading as well as writing. The examples in this chapter are verified
against the binary; those that manipulate a window or a control assume the
matching element is declared in a project.

## Principle

A brace containing a string resolves the global variable bearing that name:

```owl
score is an integer = 42

Trace({"score"})            // 42   — read by name
{"score"} = 100             // write by name
Trace(score)                // 100
```

The name can come from a variable, and therefore be decided at run time:

```owl
score is an integer = 42
name is a string = "score"

Trace({name})               // 42   — computed name
```

A name that matches no known element raises a run-time error, which a
`WHEN EXCEPTION` block can catch:

```owl
WHEN EXCEPTION IN
    Trace({"missing"})      // name not found
DO
    Trace("name not found")
END
```

## Indirection on a structure member

Prefixed by an instance followed by a dot, the brace resolves a member by its
name. Reading and writing follow the same rules:

```owl
Pers is a Structure
    name is a string
    age is an integer
END

p is a Pers
p.{"name"} = "Jeff"         // write the member by name
Trace(p.{"name"})           // Jeff
```

An absent member raises an error, just like a variable that cannot be found.

## Property chaining

The brace chains with `..` to read or write a property of the resolved
element. This is the way to reach a window whose name is computed:

```owl
sWindowName is a string = "FEN_Demo"
Open(FEN_Demo)

n is an integer = {sWindowName, indWindow}..Width
```

## Typed scope

A brace admits an optional second term, a **scope** constant, which directs
the resolution towards a precise family of elements:

| Constant | Resolution |
|---|---|
| `indWindow` / `indFenêtre` | a window designated by its name |
| `indControl` / `indChamp` | a control designated by its name |
| `indVariable` | a global variable |

```owl
sControl is a string = "LIB_D"
Trace({sControl, indControl}..Caption)  // caption of the resolved control
Trace({"score", indVariable})           // value of the global variable
```

Without a scope, `{ name }` resolves a global variable; the explicit scope
lifts the ambiguity and validates the expected family.

## Reach

Resolving a variable targets the **global variables**; the optional second
term is limited to the scope constants in the table above.

## Further reading

| Topic | Reference |
|---|---|
| Operators and access (`.`, `..`) | [`05_operateurs.md`](05_operateurs.md) |
| Types and structures | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Null and absent values | [`02_null_et_nullable.md`](02_null_et_nullable.md) |
