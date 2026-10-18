# Object-oriented programming

**Classes** are defined, **instances** are created, and classes are
related to one another through **inheritance** or through **interfaces**.

This page covers the object model: declaration, constructor, methods,
visibility, inheritance, polymorphism, interfaces, abstract classes,
shared members and methods, class constants and introspection.

---

## 1. One class, one file

A `.owl` file that contains a class declaration contains only:

- the class declaration with its complete body (fields, constructor,
  destructor, methods — all declared **inline**),
- comments.

No top-level code, no second class, no window or state in the same
file. Any violation produces a compilation error pointing to the
offending line.

> **Methods declared inline.** The methods of a class are declared
> **inside the class body**, between the `is a Class` and the final
> `END`. Any declaration outside the class body produces a compilation
> error.

---

## 2. Declaring a class

A class is declared with an `is a Class … END` block. Members (fields
**and** methods) are grouped by access mode (`PUBLIC` by default,
`PRIVATE`, `PROTECTED`). A common convention prefixes a class name with
a lowercase `c` to distinguish it from an instance or a primitive type.

```owl
cAccount is a Class
PUBLIC
    sOwner is a string
    rBalance is a real
END
```

A class is instantiated by declaring a variable of its type:

```owl
oAccount is a cAccount("Alice")
oAccount.rBalance = 100
```

---

## 3. Methods: declared inline

All methods (ordinary, constructor, destructor, shared) are declared
**inside the class body**, between `is a Class` and the final `END`.
A method is attached directly to the class by its position in the
block — no need to prefix its name with the class name.

```owl
cAccount is a Class
PUBLIC
    sOwner is a string
    rBalance is a real

    PROCEDURE Constructor(name is a string)
        :sOwner = name
        :rBalance = 0
    END

    PROCEDURE Destructor()
        Trace("Closing the account of " + :sOwner)
    END

    PROCEDURE Credit(rAmount is a real)
        :rBalance += rAmount
    END

    PROCEDURE Balance() : real
        RESULT :rBalance
    END
END
```

The `:` prefix designates the member of the current instance — the
equivalent of `this.` or `self.` in other languages, but shorter. It
is used to read and write a field (`:rBalance += rAmount`) or to call
another method of the same class (`:Recompute()`).

The call is unsurprising:

```owl
oAccount is a cAccount("Alice")
oAccount.Credit(50)
Trace(oAccount.Balance())
```

### Constructor and destructor

**Constructor**: `PROCEDURE Constructor(...)` declared inline. It runs
on every instance creation. A class without an explicit constructor is
instantiable without arguments (a trivial implicit constructor is
provided).

**Instantiation without parentheses (trivial constructor).** When the
constructor requires no argument (no explicit constructor, or a
constructor with no parameter, or parameters all with a default value),
the two forms are equivalent:

```owl
f is a cFile()        // explicit form
f is a cFile          // short form — auto-instantiation
```

If the constructor requires one or more arguments, the short form is
rejected at compilation with an explicit message stating that the
constructor of `<name>` requires N argument(s) and that `<name>(...)`
must be used to instantiate. This closes a classic trap where a
class-typed variable silently stayed `Null`. To declare an instance
intentionally nullable, use the `<nullable>` marker:
`o is a cFoo <nullable>`.

**Destructor**: `PROCEDURE Destructor()` declared inline, with no
parameter. The destructor remains optional. It is triggered explicitly
by the `Free` keyword:

```owl
cFile is a Class
PUBLIC
    nHandle is an integer = 0

    PROCEDURE Constructor()
        :nHandle = 1       // simulated open
        Trace("file opened")
    END

    PROCEDURE Destructor()
        Trace("file closed")
        :nHandle = 0
    END
END
```

```owl
f is a cFile()
// ... work on f ...
Free f
// Output: "file opened" / "file closed"
```

After `Free f`, the variable `f` is invalid; any reuse produces a
runtime error. An instance not freed explicitly is reclaimed later by
the garbage collector; the execution of the destructor is then not
guaranteed within a deterministic delay — to be reserved for objects
with no external resource to close.

### Method modifiers

An inline method may carry explicit modifiers between `PROCEDURE` and
its name: `PUBLIC`, `PRIVATE`, `PROTECTED`, `GLOBALE`, `ABSTRACT`.

```owl
cAccount is a Class
PUBLIC
    rBalance is a real

    PROCEDURE PUBLIC Balance() : real
        RESULT :rBalance
    END

    PROCEDURE PRIVATE Recompute()
        // internal detail
    END
END
```

Without an explicit modifier, **the method inherits the visibility of
the current access block** — exactly like fields. In the example below,
`Credit` and `Balance` are public (the `PUBLIC` block), while
`Recompute` is private (the `PRIVATE` block):

```owl
cAccount is a Class
PUBLIC
    rBalance is a real

    PROCEDURE Credit(amount is a real)
        :rBalance += amount
        :Recompute()
    END

    PROCEDURE Balance() : real
        RESULT :rBalance
    END

PRIVATE
    PROCEDURE Recompute()
        // internal detail
    END
END
```

### Multi-arity: default parameters

Overloading is not supported: two methods of the same name in the same
class are forbidden at compile time. To offer several ways to call the
same action, **default parameters** are used:

```owl
cPerson is a Class
PUBLIC
    sName is a string
    nAge is an integer

    // Callable: cPerson(), cPerson("Alice"), cPerson("Alice", 30)
    PROCEDURE Constructor(name is a string = "unknown",
                          age is an integer = 0)
        :sName = name
        :nAge = age
    END
END
```

### Controlled access through named methods

An expression such as `obj.Field = val` acts only on a **declared
field**; it does not trigger the call of a hidden method. To expose
controlled access, two inline methods with explicit names are declared:

```owl
cAccount is a Class
PUBLIC
    rBalance is a real

    PROCEDURE Balance() : real
        RESULT :rBalance
    END

    PROCEDURE SetBalance(rNew is a real)
        IF rNew < 0 THEN
            ExceptionThrow(1, "Negative balance refused")
        END
        :rBalance = rNew
    END
END
```

For a getter/setter pair, choose two distinct names (`Balance()` /
`SetBalance(v)`, `Width()` / `WidthChange(v)`, etc.).

### Method chaining on an instance

The `object` keyword designates the **current instance** inside a
method. It is usable as a value (passing as argument, method return,
assignment to a class-typed variable) but **not as a member-access
prefix**: to read or write a field, the `:` prefix remains the only
valid form (`:sName`, `:rBalance`).

Outside a method, referencing `object` is a compilation error stating
that `object` is usable only inside a class method. The EN aliases
`this` and `object` are recognized at runtime; the canonical form kept
by the formatter stays in lowercase.

Canonical use case: a method that returns `object` enables *builder*-style
chaining on a user class.

```owl
cBuilder is a Class
PUBLIC
    sName is a string
    nAge is an integer

    PROCEDURE SetName(sValue is a string) : cBuilder
        :sName = sValue
        RESULT object
    END

    PROCEDURE SetAge(nValue is an integer) : cBuilder
        :nAge = nValue
        RESULT object
    END

    PROCEDURE Show()
        Trace(:sName + " (" + :nAge + " years)")
    END
END

oB is a cBuilder()
oB.SetName("Alice").SetAge(42).Show()
```

Chaining is also available on the native prefix methods that document
it explicitly (for example the array methods `.Add(...).Sort()...` —
see `owl explain Add`).

---

## 4. Visibility: PUBLIC, PRIVATE, PROTECTED

The members of a class are grouped under an access mode, checked at
compilation:

| Mode | FR form | Visibility |
|---|---|---|
| `PUBLIC` (default) | `PUBLIC` | everywhere |
| `PRIVATE` | `PRIVÉ` | owning class only |
| `PROTECTED` | `PROTÉGÉ` | class + derived classes |

```owl
cAccount is a Class
PUBLIC
    sOwner is a string
PRIVATE
    rBalance is a real
PROTECTED
    sInternalNumber is a string
END
```

Any access that leaves the authorized scope produces an explicit
compilation error, mentioning the line, the field and the class
concerned. No cost is paid at runtime — the check happens 100 % before
execution.

Visibility applies to **fields and methods**: a method declared in a
`PRIVATE` block is callable only from the owning class; a method in a
`PROTECTED` block is callable only from the class and its derivatives.
A method can also force its visibility with an explicit modifier
(`PROCEDURE PRIVATE M()`) that takes precedence over the current access
block.

**Note on indirect access.** The visibility check applies only to
static accesses `obj.FieldName`. An indirect access through a dynamic
expression `obj.{sFieldName}` (where `sFieldName` is a variable holding
the name on the fly) **bypasses the compilation check**, because the
compiler cannot know the target before execution. This behavior is
intentional and limited to the dynamic-evaluation case: to keep the
encapsulation guarantee, prefer nominal static access.

**Instance or shared: an axis distinct from visibility.** The `LOCAL`
and `GLOBAL` blocks choose *where* a member lives, independently of its
visibility. `LOCAL` declares **instance members** — one copy per object,
the default mode. `GLOBAL` declares **members shared** by all instances
(see `07_poo_avance.md`). The two axes combine: a `LOCAL` block placed
after a `GLOBAL` block reverts to instance members without changing the
current visibility, exactly as `GLOBAL` does not alter visibility.

---

## 5. Inheritance

A class can inherit from another through the `Inherits` clause. Inheritance
is single: a class has at most one direct parent.

```owl
cAnimal is a Class
PUBLIC
    sName is a string

    PROCEDURE Constructor(name is a string)
        :sName = name
    END

    PROCEDURE Present()
        Trace("Animal: " + :sName)
    END
END
```

```owl
cCat is a Class
    Inherits cAnimal
PUBLIC
    bIndoor is a boolean

    PROCEDURE Constructor(name is a string, indoor is a boolean)
        Constructor cAnimal(name)
        :bIndoor = indoor
    END
END
```

The constructor of the parent class is called with the keyword
`Constructor cParent(args)` — it must be the **first statement** of the
child constructor when the parent expects arguments.

Inherited members and methods are accessible directly on the derived
instance:

```owl
oCat is a cCat("Felix", True)
Trace(oCat.sName)            // inherited from cAnimal
oCat.Present()               // inherited method
```

### Redefinition (polymorphism)

An inherited method is redefined simply by re-declaring the same
signature in the derived class. No `virtual` keyword is needed — every
method is redefinable by default.

```owl
cCat is a Class
    Inherits cAnimal
PUBLIC
    PROCEDURE Present()
        Trace("Cat: " + :sName)
    END
END
```

The call `oCat.Present()` runs the `cCat` version. Resolution is
dynamic: if `oCat` is passed to a procedure that expects a `cAnimal`,
it is always the `cCat.Present()` version that is called.

### Calling the parent version

The `Ancestor` keyword explicitly calls the version defined in the
direct parent of a class:

```owl
cCat is a Class
    Inherits cAnimal
PUBLIC
    PROCEDURE Present()
        Ancestor.Present()        // runs cAnimal.Present()
        Trace("  (and it meows)")
    END
END
```

Useful for extending the parent's behavior instead of replacing it
entirely.

> To call an ancestor located higher in the chain (skipping an
> intermediate level), the syntax `cAncestor.M()` is available.
> See `07_poo_avance.md` § 2.

---

## 6. Interfaces

An **interface** declares a contract: the list of methods a class must
provide to satisfy it. It has neither fields nor implementation, only
signatures.

```owl
iDisplayable is an Interface
    PROCEDURE Display()
END
```

A class declares that it satisfies a contract with the `implements`
clause:

```owl
cProduct is a Class
    implements iDisplayable
PUBLIC
    sName is a string
    rPrice is a real

    PROCEDURE Constructor(name is a string, price is a real)
        :sName = name
        :rPrice = price
    END

    PROCEDURE Display()
        Trace(:sName + " : " + :rPrice)
    END
END
```

Conformance is **checked at compilation**. If a method of the contract
is missing, or if a signature diverges (number of parameters, types,
return type), it is an explicit compilation error. No cost is paid at
runtime.

### Contract inheritance

If `cParent implements iX` and `cChild Inherits cParent`, then `cChild`
satisfies `iX` automatically (through method inheritance). No need to
re-declare `implements iX` in the child class — the compiler walks the
inheritance chain to verify it.

A class can implement several interfaces by listing them separated by
commas (`implements iA, iB`).

---

## 7. Bilingualism

Like every symbol of the language, the OOP keywords exist under a
canonical French form and a canonical English form. The two are
interchangeable line by line.

| Canonical FR form | Canonical EN form |
|---|---|
| `Classe` | `Class` |
| `Interface` | `Interface` |
| `Hérite de` | `Inherits` |
| `implemente` | `implements` |
| `abstraite` | `abstract` |
| `Constructeur` | `Constructor` |
| `Destructeur` | `Destructor` |
| `Ancêtre` | `Ancestor` |
| `est` (introspection) | `is` |
| `PUBLIC` / `PRIVÉ` / `PROTÉGÉ` | `PUBLIC` / `PRIVATE` / `PROTECTED` |
| `LOCAL` (instance member) | `LOCAL` |
| `GLOBAL` (member) | `GLOBAL` |
| `GLOBALE` (method) | `GLOBALE` |
| `CONSTANT` | `CONSTANT` |
| `FIN` | `END` |

The scanner also tolerates the absence of accents and case variants
when writing; the formatter restores the canonical form on save.

---

## See also

| Topic | Page |
|---|---|
| General overview and top-level | `00_apercu_du_langage.md` |
| Type system and coercions | `01_types_et_coercions.md` |
| Null value and dereferencing | `02_null_et_nullable.md` |
| OOP — advanced concepts (abstract, GLOBAL, CONSTANT, `is`, named ancestor) | `07_poo_avance.md` |
| Writing conventions | `10_conventions_de_code.md` |
| Naming prefixes (`c`, `i`, `s`, `n`…) | `12_prefixes_de_nommage.md` |
| Test charter and scenarios | `11_charte_de_test.md` |

For the exhaustive list of native classes and their methods:
`owl categories`, then `owl explain <name>`.
