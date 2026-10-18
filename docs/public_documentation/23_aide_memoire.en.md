# OWL — Cheat sheet

The common constructs in their canonical form. Each example compiles
as-is and follows the formatter's indentation (one tab per level).
Variable names are prefixed by their type (`sName`, `nX`, `tabNames`,
`oObject`) to avoid any collision with a type or a keyword.

---

## Variable

```owl
sName is a string = "Alice"
```

## Condition

Block form, for several statements:

```owl
nX is an integer = 5
IF nX > 0 THEN
	Trace("positive")
ELSE
	Trace("negative")
END
```

Inline form, for a single statement (no `END`):

```owl
nX is an integer = 5
IF nX > 0 THEN Trace("positive")
```

## Counter

```owl
FOR i = 1 TO 10
	Trace(i)
END
```

## Collection traversal

```owl
tabNames is an array of strings
FOR EACH sElem OF tabNames
	Trace(sElem)
END
```

## Procedure

```owl
PROCEDURE Calc(nVal is an integer) : integer
	nResult is an integer = nVal * 2
	RESULT nResult
END
```

## Class

```owl
cType is a Class
PUBLIC
	sField is a string

	PROCEDURE Constructor(name is a string)
		:sField = name
	END
END
```

## Array

```owl
tabNames is an array of strings
tabNames.Add("x")
nOcc is an integer = tabNames..Occurrence
```

## JSON

```owl
jDoc is a JSON = StringToJSON("{}")
sValue is a string = "test"
jDoc.key = sValue
```

## Exception

```owl
WHEN EXCEPTION IN
	Trace("try")
DO
	Trace("catch")
END
```

## Byte and buffer

```owl
bData is a buffer = "ABC"
nByte is an integer = bData[[1]]                // reads byte 1 → 65
bData[[1]] = 200                                // writes byte 1 (strict 0–255)
nClamp is an integer = IntegerToByte(300, overflowClamp)   // 255
nXor is an integer = 0xFF || 0x0F               // exclusive OR → 240
```

---

Debugging a program (breakpoints, step-by-step, inspection):
[`28_debogueur.md`](28_debogueur.md).
