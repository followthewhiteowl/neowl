# Email

The language ships a complete **electronic mail** framework: describe a
message, send it through an **SMTP** sending server, serialize it to a `.eml`
file and read it back, or import a `.msg` message produced by a desktop mail
client. Everything revolves around one central type, `Email`, which carries
both the message **to send** and the message **read back**. Like the rest of
the language, every symbol is **bilingual** FR/EN: the French forms
(`EmailEnvoieMessage`, `Expéditeur`, `Destinataire`…) are equivalent to the
canonical English forms used here and are listed at the end of the page.

## 1. The model: three composite types

Three composite types describe a message and its transport. Their properties
are read and written with the `..` operator.

**`Email` describes the message.** It is the pivot type: it carries the
sender, the recipient lists, the subject, the text and HTML bodies, the
attachments and a set of options (priority, confidentiality, receipts). The
same fields serve both to send and to read an imported message.

```owl
msg is an Email
msg..Sender    = "alice@example.org"
msg..Subject   = "Meeting minutes"
msg..Message   = "Hello, here are the minutes in plain text."
msg..HTML      = "<p>Hello, here are the minutes in <b>HTML</b>.</p>"
Add(msg..Recipient, "bob@example.org")
Add(msg..Cc, "team.lead@example.org")
```

Recipients spread across three lists of strings: `Recipient` (primary
recipients, `To` header), `Cc` (visible copy) and `Bcc` (blind copy).
Priority and confidentiality are set with dedicated constants:

| Property | Constants | Default |
|----------|-----------|---------|
| `Priority` | `emailPriorityLow`, `emailPriorityNormal`, `emailPriorityHigh` | `emailPriorityNormal` |
| `Confidentiality` | `emailNormal`, `emailConfidential`, `emailPersonal`, `emailPrivate` | `emailNormal` |

Two booleans, `AcknowledgeReceipt` and `ReadReceipt` (both `False` by
default), respectively request a delivery receipt and a read confirmation
from the recipient. The `Source` property carries the **raw** message
(headers and MIME body); it is the backbone of the serialization round-trip
described below.

**`emailSMTPSession` describes the sending server.** A session carries the
server address, the port, the authentication credentials and the encryption
options:

```owl
session is an emailSMTPSession
session..ServerAddress = "smtp.example.org"
session..Port          = 587
session..Name          = "account@example.org"
session..Password      = "secret"
```

The port defaults to `25`. `Name` and `Password`, left empty, open an
**unauthenticated** session; filled in, they trigger authentication when the
session is opened.

**`emailAttach` describes an attachment.** Each attachment carries a binary
content (`Content`, a `Buffer`), its MIME type (`ContentType`), its file name
(`Name`) and, for an image referenced from the HTML body, a description and a
content identifier:

```owl
attach is an emailAttach
attach..Name        = "report.pdf"
attach..ContentType = "application/pdf"
attach..Content     = StringToBuffer("binary content of the report")
```

A message's attachments live in the `Email..Attachment` array.

**`emailHeader` describes a custom header**, with a `Name` and a `Value`.
Non-standard headers (for example `X-Origin`) live in the `Email..Header`
array: they are added to the message on send, and on read the unrecognized
headers are deposited there (standard headers stay read into their dedicated
properties).

## 2. Sending a message

Sending happens in three steps: **open** a session to the server, **send**
one or more messages, **close** the session. Each function returns a boolean
(except the close): a failure does not raise a blocking error, it returns
`False` and sets `ErrorOccurred`, to be tested before going further.

- `EmailStartSession(session)` establishes the connection, negotiates the
  optional encryption and authenticates if credentials are provided. Returns
  `True` if the session is open.
- `EmailSendMessage(session, email)` sends an `Email` through an already-open
  session. The raw message is built on the fly from the `Email` fields.
- `EmailCloseSession(session)` closes the connection and releases the
  resources.

`EmailSetTimeOut(session, delay)` sets, before opening, the maximum wait time
for the server in milliseconds: past that delay without a response, the open
fails cleanly instead of blocking indefinitely.

The complete example below composes a two-body message (text and HTML),
attaches a local file with `EmailLoadAttachment`, then sends it:

```owl
session is an emailSMTPSession
session..ServerAddress = "smtp.example.org"
session..Port          = 587
session..Name          = "account@example.org"
session..Password      = "secret"
EmailSetTimeOut(session, 5000)

msg is an Email
msg..Sender   = "account@example.org"
msg..Subject  = "Monthly report"
msg..Message  = "Hello, this month's report is attached."
msg..HTML     = "<p>Hello,</p><p>This month's report is attached.</p>"
msg..Priority = emailPriorityHigh
Add(msg..Recipient, "bob@example.org")
Add(msg..Cc, "archive@example.org")
EmailLoadAttachment(msg, "reports/monthly.pdf")

IF EmailStartSession(session) THEN
	IF EmailSendMessage(session, msg) THEN
		Trace("message accepted by the sending server")
	ELSE
		Trace("sending failed")
	END
	EmailCloseSession(session)
ELSE
	Trace("cannot connect to the sending server")
END
```

`EmailLoadAttachment(email, path)` reads the designated file, deduces its
MIME type from the extension and adds an `emailAttach` to the message; it
returns `False` if the file is not found, adding nothing.

`EmailSendMessage` accepts an optional third argument: a set of **build
options** for the message, combinable with the `+` operator (see the next
section and the serialization).

## 3. Encrypting the transport (TLS)

The session's `Option` property chooses the connection mode. Three values are
available:

| Constant | Connection | Standard port |
|----------|-----------|---------------|
| `emailOptionDefault` | In clear, no encryption. | `25` |
| `emailOptionSecureTLS` | Starts in clear then switches to TLS (STARTTLS). | `587` |
| `optionSSL` | Encrypted from the start (direct TLS). | `465` |

The `CACertificate` property receives the path of a PEM certificate-authority
file. Left empty, server certificate validation relies on the standard public
authorities; filled in, it adds a **private** trusted authority — the case of
an internal sending server whose certificate is signed by an in-house
authority.

```owl
session is an emailSMTPSession
session..ServerAddress = "smtp.internal.local"
session..Port          = 465
session..Option        = optionSSL
session..CACertificate = "/etc/owl/ca-internal.pem"
session..Name          = "service@internal.local"
session..Password      = "secret"

IF EmailStartSession(session) THEN
	Trace("encrypted session opened")
	EmailCloseSession(session)
END
```

Server certificate verification is **strict**: a certification chain that
does not trace back to a trusted authority makes the open fail. For an
internal server, pointing to its private authority through `CACertificate` is
the way to go — never disable verification.

## 4. Saving and reading a message back (`.eml`)

An `Email` serializes to a standard raw message, the one of a `.eml` file.
Two functions do the round-trip, through the `Source` property:

- `EmailBuildSource(email)` builds the raw message (headers and MIME body)
  from the `Email` fields and deposits it in `email..Source`.
- `EmailImportSource(email)` does the reverse: it parses the raw message
  present in `email..Source` and fills its fields (sender, recipients,
  subject, text and HTML bodies, attachments).

Combined with the file functions `fSaveText` and `fLoadText`, they write and
read a `.eml` file back:

```owl
msg is an Email
msg..Sender  = "alice@example.org"
msg..Subject = "Minutes"
msg..Message = "Here are the minutes of the meeting."
Add(msg..Recipient, "bob@example.org")

// Build the raw message, then write it to disk.
IF EmailBuildSource(msg) THEN
	fSaveText("archive/minutes.eml", msg..Source)
END

// Later: read the file back and repopulate an Email.
reread is an Email
reread..Source = fLoadText("archive/minutes.eml")
IF EmailImportSource(reread) THEN
	Trace("subject read back: " + reread..Subject)
END
```

The produced body depends on the message: text only (`text/plain`), text and
HTML (`multipart/alternative`) or a message with attachments
(`multipart/mixed`, parts encoded in base64). The parsing on the way back
restores each part, including the binary content of the attachments.

`EmailBuildSource` accepts an optional second argument of build options,
combinable with the `+` operator:

| Constant | Effect |
|----------|--------|
| `emailOptionEncodeHeader` | Encodes headers containing non-ASCII characters (encoded-words). Without the option, an accented subject stays in clear. |
| `emailOptionHeaderBcc` | Makes blind-copy recipients appear in a dedicated header of the message (hidden by default). |

```owl
msg is an Email
msg..Sender  = "alice@example.org"
msg..Subject = "Budget meeting"
msg..Message = "Please confirm your attendance."
Add(msg..Recipient, "bob@example.org")

EmailBuildSource(msg, emailOptionEncodeHeader)
```

`EmailImportSource` decodes these encoded-words on the way back: the accented
subject is restored identically.

## 5. Importing an Outlook message (`.msg`)

`EmailImportMSG(email, path)` reads a `.msg` file — the message format saved
by a desktop mail client such as Outlook — and fills an `Email` from it:
sender, primary and copied recipients, subject, text and HTML bodies,
attachments. The read is **read-only**: the import does not modify the
original file.

```owl
received is an Email
IF EmailImportMSG(received, "inbox/message.msg") THEN
	Trace("from: " + received..Sender)
	Trace("subject: " + received..Subject)
	FOR EACH attach OF received..Attachment
		Trace("attachment: " + attach..Name)
	END
ELSE
	Trace("import failed: file missing or unrecognized format")
END
```

The function returns `False` and sets `ErrorOccurred` if the file is not
found or is not a valid message container, without crashing.

## French aliases

**Types**

| EN canon | FR canon |
|----------|----------|
| `Email` | `Email` |
| `emailSMTPSession` | `emailSessionSMTP` |
| `emailAttach` | `emailAttache` |
| `emailHeader` | `emailEntête` |

**`Email` properties**

| EN canon | FR canon |
|----------|----------|
| `Sender` | `Expéditeur` |
| `SenderAddress` | `AdresseExpéditeur` |
| `ErrorReturnAddress` | `AdresseRetourErreur` |
| `Recipient` | `Destinataire` |
| `Cc` | `Cc` |
| `Bcc` | `Cci` |
| `Subject` | `Sujet` |
| `Message` | `Message` |
| `HTML` | `HTML` |
| `Attachment` | `Attache` |
| `Category` | `Catégorie` |
| `Priority` | `Priorité` |
| `Confidentiality` | `Confidentialité` |
| `AcknowledgeReceipt` | `AccuséRéception` |
| `ReadReceipt` | `ConfirmationLecture` |
| `MessageID` | `MessageID` |
| `Reference` | `Référence` |
| `Source` | `Source` |
| `Header` | `Entête` |

**`emailSMTPSession` properties**

| EN canon | FR canon |
|----------|----------|
| `ServerAddress` | `AdresseServeur` |
| `Name` | `Nom` |
| `Password` | `MotDePasse` |
| `Port` | `Port` |
| `Option` | `Option` |
| `CACertificate` | `CertificatCA` |

**`emailAttach` properties**

| EN canon | FR canon |
|----------|----------|
| `Content` | `Contenu` |
| `ContentType` | `ContentType` |
| `Name` | `Nom` |
| `ContentDescription` | `ContentDescription` |
| `Identifier` | `Identifiant` |

**`emailHeader` properties**

| EN canon | FR canon |
|----------|----------|
| `Name` | `Nom` |
| `Value` | `Valeur` |

**Functions**

| EN canon | FR canon |
|----------|----------|
| `EmailStartSession` | `EmailOuvreSession` |
| `EmailSendMessage` | `EmailEnvoieMessage` |
| `EmailCloseSession` | `EmailFermeSession` |
| `EmailSetTimeOut` | `EmailChangeTimeOut` |
| `EmailLoadAttachment` | `EmailChargeFichierAttaché` |
| `EmailBuildSource` | `EmailConstruitSource` |
| `EmailImportSource` | `EmailImporteSource` |
| `EmailImportMSG` | `EmailImporteMSG` |

**Constants**

| EN canon | FR canon |
|----------|----------|
| `emailOptionDefault` | `emailOptionDéfaut` |
| `emailOptionSecureTLS` | `emailOptionSécuriséTLS` |
| `optionSSL` | `optionSSL` |
| `emailOptionEncodeHeader` | `emailOptionEncodeEntête` |
| `emailOptionHeaderBcc` | `emailOptionEntêteCCi` |
| `emailPriorityLow` | `emailPrioritéBasse` |
| `emailPriorityNormal` | `emailPrioritéNormale` |
| `emailPriorityHigh` | `emailPrioritéHaute` |
| `emailNormal` | `emailNormal` |
| `emailConfidential` | `emailConfidentiel` |
| `emailPersonal` | `emailPersonnel` |
| `emailPrivate` | `emailPrivé` |

See also: [Messaging](41_messagerie.md), [REST services](40_services_rest.md).
