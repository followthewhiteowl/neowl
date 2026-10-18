# Field groups

A **Group** is a named collection of UI controls that lets a single
property be applied to all its members in one statement (broadcast).
Useful to enable/disable, hide, or reset a set of related controls
without looping over them manually.

## Declaration

A Group is declared inside a `Window`, next to its controls. The value is
a literal list of controls (or empty):

```owl
FEN_Input is a Window
    PROPERTIES
        Title       = "New article"
        Layout      = dispVertical
    END

    SAI_Name     is a Control Input
        PROPERTIES
            Placeholder = "Name"
        END
    END

    SAI_Price    is a Control Input
        PROPERTIES
            Placeholder = "Price"
        END
    END

    SAI_Quantity is a Control Input
        PROPERTIES
            Placeholder = "Quantity"
        END
    END

    GR_Inputs is a Group = [SAI_Name, SAI_Price, SAI_Quantity]
END
```

An empty Group is valid:

```owl
GR_ToModify is a Group
```

Conventional prefix: `GR_` (analogous to `BTN_`, `SAI_`, `LBL_`).

## Broadcasting a property

Writing `..Prop` on a Group applies the value to **each member that
exposes that property**. Members that do not expose it are silently
ignored.

```owl
// All the controls become grayed
GR_Inputs..Grayed = True

// All become invisible
GR_Inputs..Visible = False
```

**Heterogeneous Group**: no problem. If a Group mixes Buttons, Inputs
and Labels, writing `..Value = "foo"` only affects the Inputs (which
expose `Value`). No error, no warning — the other members stay
unchanged.

```owl
GR_Mixed is a Group = [BTN_OK, SAI_Name, LBL_Info]

GR_Mixed..Visible = False      // all 3 become invisible
GR_Mixed..Value   = "value"    // only SAI_Name is affected
```

## Reading membership

`myControl..Group` returns an `array of Group` listing the Groups that
contain this control. Useful to traverse, test, or dynamically manipulate
membership.

```owl
tabGroups is an array of Group = SAI_Name..Group
IF tabGroups..Occurrence > 0 THEN
    Trace("SAI_Name belongs to " + tabGroups..Occurrence + " group(s)")
END
```

A control can belong to several Groups at once:

```owl
GR_All       is a Group = [BTN_OK, BTN_Cancel]
GR_Essential is a Group = [BTN_OK]

// BTN_OK appears in both Groups
nGroupCount is an integer = BTN_OK..Group..Occurrence  // 2
```

## Writing membership

`myControl..Group = [GR_X, GR_Y]` **recomposes** the control's
membership. The control is first removed from all existing Groups, then
added to the Groups mentioned in the list.

```owl
// Initially BTN_Target is in no Group
BTN_Target..Group = [GR_First]      // added to GR_First

BTN_Target..Group = [GR_Second]     // removed from GR_First,
                                    // added to GR_Second

BTN_Target..Group = []              // removed from all Groups
```

## Manual manipulation

A Group behaves like an `array of Control`: the standard array functions
apply.

| Action | Syntax |
|---|---|
| Count the members | `GR_X..Occurrence` |
| Test if empty | `GR_X..Empty` |
| Add a member | `Add(GR_X, myControl)` |
| Remove a member | `Delete(GR_X, nIndex)` |
| Clear entirely | `DeleteAll(GR_X)` |
| Find a member | `nIndex is an integer = Seek(GR_X, myControl)` |
| Iterate | `FOR EACH field OF GR_X DO ... END` |

```owl
// Remove a button from a Group by search
nIdx is an integer = Seek(GR_Buttons, BTN_Obsolete)
IF nIdx > 0 THEN
    Delete(GR_Buttons, nIdx)
END
```

## Most useful broadcast properties

| Property | Effect on compatible members |
|---|---|
| `Visible`  | Shows / hides |
| `Grayed`   | Enables / disables (with a grayed appearance) |
| `Editable` | Allows / forbids user input (Inputs) |
| `Value`    | Initializes / clears the content (Inputs) |

Any property declared on a control type can be broadcast. Members that do
not expose it are silently ignored (cf. § Broadcasting).

## Points of attention

- **References to controls.** A Group stores references: if a control is
  destroyed (window closing), its references in the Groups become
  invalid. A Group outlives the window that declares it only if an
  explicit reference to it is kept.
- **Non-recursive broadcast over containers.** A Group containing a
  Container broadcasts the property to the Container itself, not to its
  children. To reach the children, add them explicitly to the Group.
- **Reading a property over a whole Group.** Writing `GR_X..Prop = v`
  broadcasts to all members; on reading, a Group has no aggregated
  "single value". To read, iterate or index (`GR_X[1]..Value`).

---

## Further reading

| Topic | Page |
|---|---|
| Control lifecycle | `15_lifecycle_champs.md` |
| Code conventions | `10_conventions_de_code.md` |
| Naming prefixes (`GR_`, `BTN_`, `SAI_`…) | `12_prefixes_de_nommage.md` |

For the list of properties exposed by a control type:
`owl explain <Type>`.
