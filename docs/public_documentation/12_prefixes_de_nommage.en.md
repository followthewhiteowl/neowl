# Naming prefixes

This page details the name prefixes used to indicate **scope**,
**type** and **nature** of an entity. These prefixes are a
convention, not a compiler requirement: code compiles and runs
without them. They exist for two reasons:

1. **Fast reading.** A prefixed name conveys its properties without
   having to trace back to the declaration.
2. **Consistent writing discipline.** When several people write in
   the same project, shared prefixes avoid stylistic discrepancies.

Each team may enforce or relax its own charter; the tables below
describe the one used as reference in the documentation and the
examples.

---

## 1. Scope prefixes

The scope prefix comes **before** the type prefix. It indicates where
the variable is visible.

| Scope | Prefix |
|---|---|
| Local variable (procedure, block) | none |
| Global variable of the project, a window, a report | `g` |
| Class instance member | `m_` |
| Class global member (static equivalent) | `mg_` |
| Instance property | `p_` |
| Global property | `pg_` |

```owl
// in projet.owl, at the project top level
gbTestMode is a boolean = False        // project global, boolean

PROCEDURE ProcessOrder()
    nLine is an integer = 0            // local, integer
    FOR i = 1 TO 10
        nLine += i
    END
END

cAccount is a class
PUBLIC
    m_balance is a currency
    mg_counter is an integer
END
```

---

## 2. Type prefixes for variables

The prefixes below are glued **directly** to the name (no
underscore), in lowercase. The name that follows starts with an
uppercase letter (camelCase).

| Type | Prefix | Example |
|---|---|---|
| `string` | `s` | `sName`, `sEmailAddress` |
| `integer` | `n` | `nAge`, `nLineCount` |
| `real` | `r` | `rRate`, `rDistance` |
| `currency` | `mo` | `moAmount`, `moPriceExclTax` |
| `numeric` | `x` | `xTotal`, `xCoefficient` |
| `boolean` | `b` | `bActive`, `bVisible` |
| `date` | `d` | `dBirth`, `dDueDate` |
| `time` | `h` | `hOpening`, `hDeparture` |
| `datetime` | `dh` | `dhCreation`, `dhLastModified` |
| `duration` | `du` | `duTimeout`, `duVisit` |
| `array` | `tab` | `tabClients`, `tabLines` |
| `buffer` | `buf` | `bufImage`, `bufContent` |

> Note: `s` for `string` works the same as a mnemonic in both
> languages.

---

## 3. Prefixes for user-defined types and their instances

A two-part convention distinguishes **the type** (the definition)
and **the instance** (the variable that holds a value of that type):

| Category | Definition (the type) | Instance (the variable) | Example |
|---|---|---|---|
| Enumeration | `E` + PascalCase | `e` + PascalCase | `EStatus` type, `eCurrentState` instance |
| Combination | `CO` + PascalCase | `co` + PascalCase | `COPermissions` type, `coUserRights` instance |
| Structure | `ST` + PascalCase | `st` + PascalCase | `STAddress` type, `stDelivery` instance |
| Class | `c` + PascalCase | `o` + PascalCase | `cAccount` type, `oCurrentAccount` instance |

```owl
EStatus is an enumeration
    active
    suspended
    closed
END

STAddress is a structure
    sStreet is a string
    sPostalCode is a string
    sCity is a string
END

cAccount is a class
PUBLIC
    m_balance is a currency
END

PROCEDURE ProcessAccount()
    eCurrentStatus is an EStatus = active
    stAddress is an STAddress
    oAccount is a cAccount
END
```

Note on classes: the lowercase `c` denotes the **type** (the class
itself), the `o` (for *object*) denotes an **instance**. This
distinction is not symmetric with structures/enumerations (where the
type is in UPPERCASE and the instance in lowercase); it is a
deliberate convention.

---

## 4. Interface control prefixes

The controls of a window or a report carry an **UPPERCASE** prefix
followed by an underscore, then the control name in PascalCase. This
prefix identifies the control type at a glance in the code and in the
designer.

### Main containers

| Type | Prefix | Example |
|---|---|---|
| Window | `FEN_` | `FEN_Login` |
| Report | `ETAT_` | `ETAT_Invoice` |

### Controls (usable in Window and Report)

| Control type | Prefix | Example |
|---|---|---|
| Label | `LIB_` | `LIB_Title`, `LIB_ClientName` |
| Button | `BTN_` | `BTN_Validate`, `BTN_Cancel` |
| Input | `SAI_` | `SAI_User`, `SAI_Password` |
| Image | `IMG_` | `IMG_Logo`, `IMG_Photo` |
| List | `LISTE_` | `LISTE_Products` |
| ComboBox | `COMBO_` | `COMBO_Country` |
| RadioGroup | `SEL_` | `SEL_Salutation` |
| CheckBox | `INT_` | `INT_Newsletter` |
| Container | `CONT_` | `CONT_Form` |
| Table | `TABLE_` | `TABLE_Orders` |
| Table column | `COL_` | `COL_Reference`, `COL_Quantity` |
| Looper | `ZR_` | `ZR_InvoiceLines` |
| ProgressBar | `JAU_` | `JAU_Loading`, `JAU_Progress` |

The blocks of a report (`DocumentHeader`, `PageHeader`, `Body`,
`PageFooter`, `DocumentFooter`, `Iteration`) carry no prefix: they are
fixed language structures, not arbitrarily named controls.

```owl
FEN_Login is a Window
    LIB_Title is a Control Label
    SAI_User is a Control Input
    SAI_Password is a Control Input
    BTN_Validate is a Control Button
    BTN_Cancel is a Control Button
END
```

### Automatic check (project opt-in)

By default, control prefixes stay a convention: nothing is enforced.
A project can enable a check by setting the
`EnableNamingConventionWarning` property (bilingual
`ActiveAvertissementConventionDeNommage`) to `True` in the
`is a Project` block of its `projet.owl`:

```owl
MyApplication is a Project
    PROPERTIES
        Languages   = [languageFrench]
        Description = "Order Management"
        EnableNamingConventionWarning = True
    END
END
```

When set to `True`, the compiler flags every interface control whose
name does not start with the canonical prefix of its type — `BTN_`
for a Button, `SAI_` for an Input, `COL_` for a Column, etc. The
signal is a **non-blocking warning**: the code compiles and runs
normally, the warning serves as a reminder to standardize naming.

```owl
FEN_Login is a Window
    Validate is a Control Button    // warned: expected prefix 'BTN_'
    BTN_Cancel is a Control Button  // compliant, no warning
END
```

When not set or set to `False`, the property leaves the check
disabled.

This property is part of the metadata catalog of the `is a Project`
block, described in detail in `24_projet.md`; the reference page for
the type and its fields opens with `owl explain Projet`.

The scope of this warning is deliberately narrow. It covers
**only** the names of the interface controls listed in the table of
section 4. Therefore the following are **never** affected:

- **Plain variables.** The type prefixes described in section 2
  (`n` for an integer, `s` for a string, `r` for a real...) stay a
  purely indicative convention: a mis-prefixed variable triggers no
  warning, whatever the value of the property.
- **Containers with no canonical prefix** (Window, Tab), which have
  no expected prefix to check.

```owl
MyApplication is a Project
    PROPERTIES
        Languages = [languageFrench]
        EnableNamingConventionWarning = True
    END
END

age is an integer          // no warning: plain variable
MyWindow is a Window        // no warning: container with no canonical prefix
    Validate is a Control Button // warned: this is a control, expected prefix 'BTN_'
END
```

---

## 5. Composing prefixes

When several prefixes apply, they compose **in the order: scope +
type + name**. All prefixes stay lowercase, glued to the name.

```owl
gbTestMode        // global (g) + boolean (b) + TestMode
gsConfigPath      // global (g) + string (s) + ConfigPath
gtabUsers         // global (g) + array (tab) + Users
m_nCounter        // instance member (m_) + integer (n) + Counter
mg_sServiceName   // class global member (mg_) + string (s) + ServiceName
```

The rule reads left → right: "**scope** of a **type** called
**Name**".

---

## 6. When not to prefix

- **Short procedure parameters.** `i`, `j`, `k` as loop indices, `it`
  for the element of a `FOR EACH`, stay accepted without a prefix.
- **Context that makes the prefix redundant.** In a procedure
  `ComputeVAT(rate)`, the `rate` parameter need not be called
  `rRate`: the call context and the signature already reveal the
  type.
- **Procedures and native functions.** Prefix conventions apply to
  **variables**, not to calls.
- **Mixing within a single file.** If a file uses prefixes, they
  apply to all variables; otherwise to none. A half-prefixed file is
  harder to read than a file with no prefixes at all.

---

## Further reading

| Topic | Page |
|---|---|
| General conventions (case, accents, indentation, comments) | `10_conventions_de_code.md` |
| Test charter | `11_charte_de_test.md` |
| Language overview | `00_apercu_du_langage.md` |
