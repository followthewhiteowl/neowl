# Buffers: the binary container

## 1. The buffer in brief

A `buffer` is a **sequence of raw bytes**: the container for everything
binary (file formats, network protocols, exchanges with external code).
Where a `string` carries text and an `integer` carries a number, the
`buffer` carries bytes with no interpretation.

A useful mental-model point for low-level interop: **when a precise size
in bytes is needed, it is the buffer that carries it**, never the text
type nor the numeric type. A buffer is pre-sized to receive exactly _n_
bytes (see [section 3](#3-pre-sizing-a-buffer)), without reintroducing a
fixed-size integer or string into the language: the type stays simple to
read, and the size constraint lives in the byte container.

Two facets of the buffer are documented elsewhere and are not repeated
here:

- **byte-by-byte access** (the `buffer[[i]]` operator, reading and
  writing a byte, overflow modes, bitwise operations) is detailed in
  [`01_types_et_coercions.md`](01_types_et_coercions.md), section
  "Working at the byte level";
- **text ⇄ bytes conversion** and the choice of an encoding
  (`StringToBuffer` / `BufferToString`) are detailed in
  [`26_encodage_et_io.md`](26_encodage_et_io.md).

This chapter covers the level above: the buffer as a tool for binary
marshalling and input/output.

---

## 2. Packing scalars (marshalling)

To write a number as fixed-size bytes, OWL provides dedicated natives:

| Native | Role |
|---|---|
| `IntegerToBuffer(value, byteCount, endianness?)` | Integer → bytes (1, 2, 4 or 8), two's complement |
| `BufferToInteger(buffer, position, byteCount, endianness?, signed?)` | Bytes → integer |
| `RealToBuffer(value, byteCount, endianness?)` | Real → IEEE 754 bytes (4 or 8) |
| `BufferToReal(buffer, position, byteCount, endianness?)` | Bytes → real |

The `endianness` parameter (enumeration) sets the **byte order**:
`littleEndian` places the low-order byte first (the default),
`bigEndian` places the high-order byte first. This order matters as soon
as data is exchanged with the outside: two systems that disagree on
endianness read the same byte sequence as two different values. It must
therefore be stated explicitly according to the format expected by the
other end.

Round-trip of an integer through a buffer, in both orders:

```owl
buf is a buffer = IntegerToBuffer(1000, 4)       // little-endian by default
Trace(Length(buf))                               // 4
Trace(buf[[1]])                                  // 232  — 0xE8, low byte first
n is an integer = BufferToInteger(buf, 1, 4)
Trace(n)                                          // 1000

bufBig is a buffer = IntegerToBuffer(1000, 4, bigEndian)
Trace(bufBig[[1]])                               // 0    — high byte first
Trace(BufferToInteger(bufBig, 1, 4, bigEndian))  // 1000
```

The same principle applies to reals, in IEEE 754 format (4 bytes for
single precision, 8 for double):

```owl
bufR is a buffer = RealToBuffer(1.5, 8)
Trace(Length(bufR))                              // 8
Trace(BufferToReal(bufR, 1, 8))                  // 1.5
```

An unsupported size (other than 1/2/4/8 for an integer, or 4/8 for a
real), a value that does not fit in the requested size, or a position
outside the buffer's bounds raise an exception.

---

## 3. Pre-sizing a buffer

`EmptyBuffer(byteCount)` reserves a buffer of _n_ bytes, all initialized
to zero. It is the receiving buffer prepared **before** the operation
that will fill it — typically the response of an external call whose
maximum size is known.

```owl
buf is a buffer = EmptyBuffer(16)
Trace(Length(buf))                               // 16
Trace(buf[[1]])                                  // 0
Trace(buf[[16]])                                 // 0
```

`EmptyBuffer(0)` returns a valid empty buffer; a negative byte count
raises an exception. This is where the trade-off mentioned in
[section 1](#1-the-buffer-in-brief) materializes: the fixed size lives on
the buffer, not on the scalars.

---

## 4. Binary files

Reading or writing a buffer on disk is done with three natives symmetric
to the text versions:

| Native | Role |
|---|---|
| `fSaveBuffer(path, buffer)` | Writes the buffer to the file (overwrites) |
| `fLoadBuffer(path, offset?, length?)` | Loads all or part of the file into a buffer |
| `fAddBuffer(path, buffer)` | Appends the buffer to the end of the file |

No encoding is involved: these are raw bytes, written and read back as
is. The path follows the usual project resolution; a relative path is
resolved against the current project.

```owl
path is a string = "_paquet.bin"
data is a buffer = IntegerToBuffer(305419896, 4, bigEndian)
fSaveBuffer(path, data)

reread is a buffer = fLoadBuffer(path)
Trace(Length(reread))                            // 4
Trace(BufferToInteger(reread, 1, 4, bigEndian))  // 305419896
fDelete(path)
```

---

## 5. The buffer as a result

The buffer is not only a container filled by hand: several families of
natives **produce** a buffer as output. The buffer is the common exchange
format of these binary operations (their details live in their own
documentation):

- **serialization**: `Serialize` turns a structure into a buffer;
- **encryption**: `EncryptStandard` produces an encrypted buffer;
- **compression**: `Compress` returns a compressed buffer;
- **zip archives**: they exchange buffers — in-memory extraction
  (`zipExtractFile`) and adding a buffer (`zipAddFile`), by selecting the
  "in memory" mode via the `zipInMemory` constant; see the zip
  documentation.

Once obtained, this buffer is handled like any other: it is saved to
disk, transmitted, its bytes are read. Refer to the documentation of each
native for the parameters and the exact format.

---

## Further reading

| Topic | Page |
|---|---|
| Byte-by-byte access, `buffer[[i]]`, overflow, bitwise | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Text ⇄ bytes conversion, choosing an encoding | [`26_encodage_et_io.md`](26_encodage_et_io.md) |
| Code conventions | [`10_conventions_de_code.md`](10_conventions_de_code.md) |
