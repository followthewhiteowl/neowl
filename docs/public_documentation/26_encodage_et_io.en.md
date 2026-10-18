# Encoding and input/output

## 1. The idea in two minutes

Text has no encoding. That is the starting point, and it avoids the old
"ANSI or Unicode?" headache as well as broken accents (mojibake).

- An OWL `string` is **neutral**: it is text, with no encoding. Because
  it is text, `Length` counts characters — `Length("café")` is 4.
- A `buffer` is a sequence of raw **bytes**.
- The encoding (UTF-8, Latin-1, Windows-1252…) is a property of the
  **bytes**, not of the text. It is set **at the boundaries**, at the
  moment a `string` becomes a `buffer` or a file — and never anywhere
  else. That is the whole point.

OWL never guesses the encoding of incoming data: it must be stated. In
return, the behavior is identical on Windows, Linux and macOS, without
depending on a machine's regional setting.

---

## 2. Converting at the boundaries

Two natives bridge text and bytes, each parameterized by an encoding:

- `StringToBuffer(s, encoding)`: encodes a `string` into a `buffer` of
  bytes.
- `BufferToString(buf, encoding)`: decodes a `buffer` of bytes into a
  `string`.

Encoding then decoding with the **same** encoding restores the original
text. This is where, and only where, the bytes appear:

```owl
// the same text takes a variable number of bytes depending on the encoding
buf is a buffer = StringToBuffer("café", encWindows1252)
Trace(Length(buf))                          // 4  — 'é' fits in 1 byte here
Trace(Length(StringToBuffer("café", encUTF8)))   // 5  — 'é' takes 2 bytes in UTF-8

sRendered is a string = BufferToString(buf, encWindows1252)
Trace(sRendered)                            // café
Trace(sRendered = "café")                   // 1 (True)
```

Same principle on the file side with `fLoadText` / `fSaveText` (and
`fAddText`), which accept the same encoding argument:

```owl
path is a string = "_demo.txt"
fSaveText(path, "déjà vu", encLatin1)
Trace(fLoadText(path, encLatin1))           // déjà vu
```

The encoding is **optional** and defaults to `encUTF8`: without an
argument, the internal bytes pass through unchanged.

A few common encodings: `encUTF8` (default), `encLatin1`,
`encWindows1252`, `encUTF16LE`. The full and current list is given by the
documentation of `owlEncoding`.

A character absent from the target encoding is replaced, on writing, by
byte `63` (the `?` character):

```owl
// '€' does not exist in Latin-1
buf is a buffer = StringToBuffer("euro €", encLatin1)
Trace(BufferToString(buf, encLatin1))       // euro ?
```

---

## 3. Format is not charset

Base64, URL percent-encoding and hexadecimal are **format** encodings:
they re-encode already-determined bytes, unrelated to the charset of a
text. They live in distinct natives — `StringToBase64`, `URLEncode`,
`StringToHex` — and do not mix with the choice of an `owlEncoding`. See
the documentation of `URLEncode` and `StringToBase64`.

Base64 comes in two alphabets, chosen by the second parameter of
`StringToBase64` and of `Base64ToString`:

- `encodeBASE64` (default) — standard alphabet, with `+`, `/` and the
  `=` padding character;
- `encodeBASE64URL` — URL-safe alphabet, with `-` and `_`, and no
  padding. This is the form to use for a token carried in a URL or a
  header.

```owl
bytes is a buffer = SecureRandomBytes(32)
token is a string = StringToBase64(bytes, encodeBASE64URL)
reread is a buffer = Base64ToString(token, encodeBASE64URL)
Trace(reread = bytes)                        // 1 (True) — round-trip without loss
```

The two alphabets do not mix: submitting a `+` to the `encodeBASE64URL`
decoder, or a `-` to the standard decoder, produces an error and an empty
string rather than an approximate result. An invalid length, a character
outside the alphabet or misplaced padding are rejected the same way; the
error is read with `ErrorOccurred`.

---

## 4. Line endings

In a `.owl` file, **a line ending is a line ending**. The file can be
saved the Unix way (LF) or the Windows way (CRLF), and may even mix the
two: it is accepted without a warning, and the value of a multiline
string is the same in every case. What is read on screen is what the
program sees. The rule holds for both multiline forms, backticks and
brackets.

```owl
excerpt is a string = `line 1
line 2`
Trace(Length(excerpt))                       // 13 — regardless of how the file is saved
Trace(excerpt = "line 1" + LF + "line 2")    // 1 (True)
```

**The data is never touched up.** This rule concerns only the program
text, read once at compile time. Entered text, a file that is read, a
network response, a database row keep their exact bytes, carriage returns
included:

```owl
path is a string = "_demo_eol.txt"
content is a string = "line 1" + CRLF + "line 2"
fSaveText(path, content)
reread is a string = fLoadText(path)
Trace(Length(reread))                        // 14 — the carriage return is intact
Trace(reread = content)                      // 1 (True)
```

When a format or a protocol requires a specific line ending, it is
written out, in the form of a constant:

| Constant | Value |
|---|---|
| `LF` | line feed alone — OWL's line ending |
| `CRLF` | carriage return followed by a line feed (CR+LF) |

For the carriage-return byte alone, without a line feed, there is no
constant: it is written `Charact(13)`.

Converting an already-built text in a single operation is done with
`Replace(text, LF, CRLF)`.

---

## Further reading

| Topic | Reference |
|---|---|
| Type system, `string`, working at the byte level | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| The buffer as a binary container (marshalling, I/O) | [`27_buffers.md`](27_buffers.md) |
| Multilingual and language tags | [`21_internationalisation.md`](21_internationalisation.md) |
| Code conventions | [`10_conventions_de_code.md`](10_conventions_de_code.md) |

For the exact signature of a native or the values of an enum, refer to
the reference documentation of the symbol.
