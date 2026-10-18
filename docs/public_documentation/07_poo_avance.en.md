# Object-oriented programming — advanced concepts

This page extends `06_poo.md` (OOP basics) with the more advanced
mechanisms: abstract classes and methods, calling an ancestor by name,
shared members and methods (`GLOBAL`), class constants, dynamic
introspection with `is`.

Prerequisite: mastery of chapters 1 to 6 of `06_poo.md` (class
declaration, inline methods, visibility, single inheritance,
interfaces).

---

## 1. Abstract classes and methods

A class can be declared **abstract**: it cannot be instantiated
directly, but serves as a base for concrete derived classes.

```owl
cShape is a Class, abstract
PUBLIC
    sName is a string

    PROCEDURE Constructor(name is a string)
        :sName = name
    END

    PROCEDURE ABSTRACT Area() : real    // no END: no body
END
```

An **abstract method** is declared with `PROCEDURE ABSTRACT`,
**without a body and without an `END`** (nothing to close), inside the
class block. It describes a signature that concrete derived classes
**must** provide. A concrete class that inherits from an abstract class
without implementing all its abstract methods produces a compilation
error.

```owl
cCircle is a Class
    Inherits cShape
PUBLIC
    rRadius is a real

    PROCEDURE Constructor(name is a string, radius is a real)
        Constructor cShape(name)
        :rRadius = radius
    END

    PROCEDURE Area() : real
        RESULT 3.14159 * :rRadius * :rRadius
    END
END
```

Attempting to instantiate `cShape` directly → compilation error.

---

## 2. Calling an ancestor by name

`Ancestor.M()` targets the **immediate** parent of a class (cf.
`06_poo.md` § 5). To reach an ancestor located higher in the
inheritance chain, the ancestor class is named directly:
`cAncestor.M()`.

The call is a **direct static dispatch** to the version defined in the
named class — without virtual re-resolution, without possible recursion.

```owl
cAnimal is a Class
PUBLIC
    PROCEDURE Shout()
        Trace("generic sound")
    END
END
```

```owl
cDog is a Class
    Inherits cAnimal
PUBLIC
    PROCEDURE Shout()
        Trace("Woof")
    END
END
```

```owl
cPuppy is a Class
    Inherits cDog
PUBLIC
    PROCEDURE Shout()
        // Ancestor.Shout() would run cDog.Shout() ("Woof").
        // cAnimal.Shout() skips cDog and directly runs
        // the root version ("generic sound").
        cAnimal.Shout()
        Trace("Yip")
    END
END
```

Key differences:

- `Ancestor.M()` targets **the immediate parent**.
- `cAncestor.M()` targets **the version defined in `cAncestor`**,
  skipping all intermediate levels.
- The compiler verifies that `cAncestor` is indeed an ancestor of the
  current class (or exposes `M` as `GLOBAL`). Otherwise, an explicit
  error.

---

## 3. Shared members and methods (`GLOBAL`)

A **shared member** is a field or a method attached to **the class**
rather than to an instance: it exists in a single copy, independently
of the number of instances created. The keyword is `GLOBAL`, for both
fields and methods.

### `GLOBAL` field: one copy per class

A `GLOBAL` block declares fields shared by all instances of the class,
accessible **without an instance**.

```owl
cCounter is a Class
GLOBAL
    nTotal is an integer = 0
PUBLIC
    sName is a string

    PROCEDURE Constructor(name is a string)
        :sName = name
        cCounter.nTotal = cCounter.nTotal + 1
    END
END
```

Each instance creation increments the shared counter. Reading and
writing go through the class name: `cCounter.nTotal`.

### `GLOBAL` method: callable without an instance

An inline method declared with the `GLOBAL` modifier is called directly
on the class, without requiring an instance. It has no access to the
`:` prefix (there is no `this`).

```owl
cMath is a Class
GLOBAL
    rPi is a real = 3.14159
PUBLIC
    PROCEDURE GLOBAL Square(n is an integer) : integer
        RESULT n * n
    END

    PROCEDURE GLOBAL CircleArea(r is a real) : real
        RESULT cMath.rPi * r * r
    END
END
```

```owl
Trace(cMath.Square(7))          // 49
Trace(cMath.CircleArea(2.0))    // 12.56636
```

### Singleton sugar

When a `GLOBAL` member has as its type the class that contains it,
**a unique instance** is created automatically at startup:

```owl
cConfig is a Class
GLOBAL
    // No parentheses: singleton syntax, unique instance created
    // automatically at startup (different from `is a cConfig()`
    // which would explicitly call the constructor).
    m_Inst is cConfig
PUBLIC
    sName is a string = ""
    nVersion is an integer = 0
END
```

```owl
cConfig.m_Inst.sName = "production"
cConfig.m_Inst.nVersion = 42
```

Two accesses to `cConfig.m_Inst` designate **the same object**. A
single limitation: the class constructor must be callable without an
argument (zero parameters, or no declared constructor).

### `GLOBAL` members not inherited

`GLOBAL` members and methods are **not inherited** by derived classes.
Each class has its own static space. If `cBase` declares
`GLOBAL nCounter`, then `cBase.nCounter` exists but `cDerived.nCounter`
does not.

---

## 4. Class constants

A class can declare constants — immutable values tied to the type,
accessible without an instance. Two forms to choose from:

```owl
cMath is a Class
    CONSTANT PI = 3.14159
END
```

```owl
cConfig is a Class
    CONSTANT
        K = 5
        J = 10
        Name = "Application"
    END
END
```

Access goes through the class name:

```owl
Trace(cMath.PI)
Trace(cConfig.Name)
```

Class constants are read-only. Any write attempt produces an error.
They are public by default and are the ideal place for thresholds,
codes, or shared reference strings.

---

## 5. Introspection: `is`

The `is <ClassName>` operator dynamically tests the type of an
instance. It returns `True` if the value is an instance of the class
**or** of one of its descendants; `False` otherwise.

```owl
oCat is a cCat("Felix", True)

IF oCat is cCat THEN
    Trace("exact type")
END

IF oCat is cAnimal THEN
    Trace("via inheritance")       // True as well
END

IF oCat is cDog THEN
    Trace("sibling")               // False, sibling classes
END
```

The operator respects the inheritance chain: an instance of a derived
class always satisfies the test for its ancestors.

### Test on an interface

`is` also accepts an interface name on the right. It returns `True` if
the class of the instance (or one of its ancestors) **implements** the
interface; `False` otherwise.

```owl
iDisplayable is an Interface
    PROCEDURE Display()
END
```

```owl
cProduct is a Class
    implements iDisplayable
PUBLIC
    sName is a string

    PROCEDURE Constructor(name is a string)
        :sName = name
    END

    PROCEDURE Display()
        Trace("product: " + :sName)
    END
END
```

```owl
p is a cProduct("keyboard")

IF p is iDisplayable THEN
    p.Display()            // executed: True
END
```

As with the test on a class, inheritance is taken into account: if
`cParent implements iX` and `cChild Inherits cParent`, then any
instance of `cChild` satisfies `obj is iX`. The test on a non-instance
value (integer, string…) returns `False` without error.

---

## 6. Visual recap

A complete class file, putting to work most of the mechanisms described
in `06_poo.md` and in this page:

```owl
cNamedCounter is a Class
    Inherits cCounter
    implements iDisplayable
GLOBAL
    sGlobalPrefix is a string = "counter-"
PUBLIC
    sLabel is a string
PROTECTED
    nLastRead is an integer
    CONSTANT MAX_THRESHOLD = 1000

PUBLIC
    PROCEDURE Constructor(sNameInit is a string)
        Constructor cCounter(sNameInit)
        :sLabel = cNamedCounter.sGlobalPrefix + sNameInit
        :nLastRead = 0
    END

    PROCEDURE Destructor()
        Trace("Closing: " + :sLabel)
    END

    PROCEDURE Display()
        Trace(:sLabel + " = " + :nLastRead)
    END

    PROCEDURE Read() : integer
        // Extends the parent version: call via Ancestor then local log.
        :nLastRead = Ancestor.Read()
        Trace("read on " + :sLabel)
        RESULT :nLastRead
    END

    PROCEDURE Reset()
        // Skips the direct Ancestor, calls the root version by name.
        cCounter.Reset()
    END

    PROCEDURE GLOBAL ThresholdReached(n is an integer) : boolean
        RESULT n >= cNamedCounter.MAX_THRESHOLD
    END
END
```

---

## See also

| Topic | Page |
|---|---|
| OOP basics | `06_poo.md` |
| General overview and top-level | `00_apercu_du_langage.md` |
| Type system and coercions | `01_types_et_coercions.md` |
| Naming prefixes (`c`, `i`, `s`, `n`…) | `12_prefixes_de_nommage.md` |

For the exhaustive list of native classes and their methods:
`owl categories`, then `owl explain <name>`.
