# Associative arrays

An associative array stores values under a **string key**, instead of a
numeric position like a classic `array of <T>`. Each key designates at
most one value.

## Declaration

The type of the values is specified after `of`:

```owl
scores is an associative array of integers
```

This type can be a fundamental type (`string`, `integer`, `real`,
`boolean`, `currency`, `numeric`) or a composite type (structure).
Without a specified type, the values are heterogeneous:

```owl
line is an associative array
```

## Usage

The key is given between brackets, for writing as well as reading. An
absent key returns the type's default value. `..Occurrence` gives the
number of entries, and `FOR EACH` traverses the values:

```owl
scores is an associative array of integers

scores["alice"] = 10          // write under a key
scores["bob"]   = 7

Trace(scores["alice"])        // read a key        -> 10
Trace(scores["unknown"])      // absent key         -> 0 (type default)
Trace(scores..Occurrence)     // number of entries  -> 2

FOR EACH value OF scores      // traverse the values
    Trace(value)              // -> 10 then 7
END
```
