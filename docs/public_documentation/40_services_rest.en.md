# REST services

A REST service exposes part of the code as an HTTP API: operations
addressed by a method and a path, served by an embedded server. The
service is **declared** with the `ServiceREST` type, each route is an
`HTTPOperation`, and the `owl serve` command puts the whole thing into
listening mode. Like the rest of the language, the declaration is
**bilingual** FR/EN: the French canonical forms (`OpérationHTTP`,
`Méthode`, `Chemin`…) are equivalent to the English forms and are listed
at the end of this page.

## 1. Declaring a service

A service is written as a declarative block in the composite-type format.
The `BaseUrl` field sets the path prefix common to every operation, as an
array of segments. Each route is a nested `HTTPOperation` block, paired
with an `onRequest` handler.

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : integer
			RESULT id
		END
	END
END
```

The properties of an `HTTPOperation`:

| Property   | Role                                                        |
|------------|-------------------------------------------------------------|
| `Method`   | HTTP verb of the route: `httpGet`, `httpPost`, `httpPut`, `httpDelete`. |
| `Path`     | Path segments, relative to `BaseUrl` (see § 2).             |
| `Responses`| Response codes the operation may produce.                   |
| `StatusCode` | Code returned by default; it must appear in `Responses`.  |
| `ExcludeFromOpenAPI` | Boolean (default `False`). At `True`, the operation is still served but does not appear in the OpenAPI document (see § 11). |
| `Stream`   | Progressive streaming mode: `streamSSE` or `streamBytes`. Absent, the response is produced in one shot (see § 7). |
| `MaxDelay` | Maximum lifetime of a stream, in seconds; `0` (default) = no limit (see § 7). |

The handler is always named `onRequest`. Its parameters are bound
automatically (see § 4) and its return value becomes the body of the
response (see § 6). A service can declare several operations, each with a
distinct name:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ListClients is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients"]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest() : integer
			RESULT 0
		END
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk, httpNotFound]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : integer
			RESULT id
		END
	END
END
```

## 2. Routing: literal paths and parameters

`Path` is a list of segments. A quoted segment is a **literal** (it must
be present as-is in the URL); a bare identifier is a **parameter** (it
captures the value at that position and passes it to the handler by its
name). Here, `clients` is fixed and `id` captures what follows:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : integer
			RESULT id
		END
	END
END
```

A request on `/api/v1/clients/42` resolves this route with `id` equal to
`42`.

**The literal wins over the parameter.** Two operations may share the same
position as long as one places a literal there and the other a parameter:
resolution is deterministic, the most specific segment wins. Below,
`/active` is served by `ListActive`, while `/7` (or any other value) falls
on `ReadClient`:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1", "clients"]
	END

	ListActive is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["active"]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest() : integer
			RESULT 0
		END
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = [id]
			Responses  = [httpOk, httpNotFound]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : integer
			RESULT id
		END
	END
END
```

**Case of a literal segment.** A literal segment has a canonical form —
the one written in `Path`. A request whose case differs on a literal
segment (`/api/v1/Clients/42` where the route declares `clients`) does not
fail: it receives a **308 redirect** to the canonical form. The client
follows the redirect and reaches the right route. Parameter values, on the
other hand, are passed through as-is.

## 3. Capturing a variable path tail

A literal or a parameter captures **a single** segment. To serve a whole
subtree — a file path, a proxy to a nested path — you need to capture
**everything that follows a prefix**, whatever the number of segments.
That is the role of the **greedy segment**: a parameter prefixed with `*`,
in the last position of `Path`.

```owl
FilesService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadFile is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["files", *rest]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(rest is a string) : string
			RESULT rest
		END
	END
END
```

A request on `/api/v1/files/images/logo.png` resolves this route, the
greedy segment `rest` capturing `images/logo.png`.

**The greedy segment captures one or more segments.** It requires at least
one segment after the prefix: a bare `/api/v1/files` — with nothing after
`files` — **does not match** this route. To serve the bare path too,
declare a distinct operation on `["files"]`.

**The parameter type decides the received shape.** The same greedy segment
reads in two ways, depending on the declared type of the parameter in
`onRequest`:

| Declared type         | Value received for `/files/images/logo.png`       |
|-----------------------|---------------------------------------------------|
| `string`              | the tail joined by `/`: `"images/logo.png"`       |
| `array of string`     | the separated segments: `["images", "logo.png"]`  |

The `string` form is natural for rebuilding a file path; the
`array of string` form serves when each segment must be handled
separately:

```owl
FilesService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadFile is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["files", *rest]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(rest is an array of string) : integer
			RESULT Count(rest)
		END
	END
END
```

**One greedy segment only, in the last position.** A route may carry only
one greedy segment, and it must be the **last** segment of `Path`. A
greedy segment placed elsewhere (`["a", *rest, "b"]`) or in duplicate
(`["a", *x, *y]`) is a **compilation error**. The rule keeps route
resolution deterministic: the greedy segment, voracious by nature, cannot
be followed by anything else without ambiguity.

## 4. Binding parameters

The parameters of `onRequest` are bound **by name**, from three sources:

- the **path** — a parameter of `Path` (`id`) feeds the handler parameter
  with the same name;
- the **query** — a name declared in `Query` feeds the parameter with the
  same name from the query string (`?sort=city`);
- the **body** — a parameter whose type is a `Structure` receives the body
  of the request, deserialized into that structure.

Binding is lenient on types: a textual value from the path or the query is
**coerced** to the declared type of the parameter (`"123"` to an integer,
for example).

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	stClient is a Structure
		name is a string
	END

	UpdateClient is an HTTPOperation
		PROPERTIES
			Method     = httpPut
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer, client is a stClient) : integer
			RESULT id
		END
	END
END
```

Here `id` comes from the path and `client` — of structured type —
receives the body. The `stClient` type is declared **inside** the
`ServiceREST` block, alongside the operations: a service file contains only
the service block, and the types it uses live inside it.

## 5. Receiving an uploaded file

A file-upload form arrives as `multipart/form-data`: a body split into
several parts, each carrying a name, some containing a file with its
metadata (original name, type, size), others a plain text field. An
operation receives these files through the `UploadedFile` type, bound like
the other parameters — **by name**.

**Two declarations, inseparable.** Receiving an upload requires two things
together:

- the `ContentType = mimeTypeMultipartForm` property on the
  `HTTPOperation`, which announces that the operation consumes a multipart
  form;
- at least one `onRequest` parameter of type `UploadedFile`, which
  receives one of the file parts.

The two go hand in hand. Declaring `ContentType = mimeTypeMultipartForm`
without any `UploadedFile`, or the reverse — an `UploadedFile` parameter
without the property — is **rejected at compilation**. Likewise, an
`UploadedFile` cannot coexist with a structured parameter receiving a body
(see § 4): a multipart form and an `application/json` body are two mutually
exclusive request contents, and their coexistence is a **compilation
error**.

```owl
DocumentsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	Upload is an HTTPOperation
		PROPERTIES
			Method      = httpPost
			Path        = ["documents"]
			ContentType = mimeTypeMultipartForm
			Responses   = [httpCreated]
			StatusCode  = httpCreated
		END
		PROCEDURE onRequest(title is a string, scan is an UploadedFile) : string
			RESULT title + " : " + scan..FileName + " (" + scan..MimeType + ")"
		END
	END
END
```

Here the text part `title` and the file part `scan` are both bound by
their name: each parameter of `onRequest` matches the part of the form
that carries the same name. Text fields are ordinary sibling parameters
(string, integer…), coerced like those of the query.

**Metadata before content.** An `UploadedFile` exposes three properties,
readable with the `..` operator, **without having to read the file
content**:

| Property      | Role                                                    |
|---------------|---------------------------------------------------------|
| `..FileName`  | Original name of the file, as sent by the client.       |
| `..Size`      | Size of the file in bytes.                              |
| `..MimeType`  | Content type declared for this part.                    |

The original name is data supplied by the client: it serves for display or
logging, never as a destination path. The received content is kept
separately and does not borrow that name.

**Several files under the same name.** When a form sends several files on
the same field name, the parameter is declared `array of UploadedFile`: it
receives all the matching parts, in arrival order.

```owl
DocumentsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	UploadBatch is an HTTPOperation
		PROPERTIES
			Method      = httpPost
			Path        = ["documents", "batch"]
			ContentType = mimeTypeMultipartForm
			Responses   = [httpCreated]
			StatusCode  = httpCreated
		END
		PROCEDURE onRequest(files is an array of UploadedFile) : integer
			total is an integer = 0
			FOR EACH f OF files
				total = total + f..Size
			END
			RESULT total
		END
	END
END
```

**Size cap.** The server applies a global limit on the size of the request
body. An upload that exceeds this limit is rejected **before** reaching the
operation: `onRequest` is not called for an over-large body, and the
service stays protected without extra code.

A query parameter becomes **optional** by giving it a default value. As
with any procedure of the language, an optional parameter is declared
**last**: no required parameter follows a defaulted one. Below, `sort` is
declared in `Query` and carries the default value `"name"`:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	stClient is a Structure
		name is a string
	END

	UpdateClient is an HTTPOperation
		PROPERTIES
			Method     = httpPut
			Path       = ["clients", id]
			Query      = [sort]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer, client is a stClient, sort is a string = "name") : integer
			RESULT id
		END
	END
END
```

## 6. Responding

The value passed to `RESULT` **is the body** of the response. Its type
determines the content type:

- a **string** is returned as-is, in `text/plain`;
- a **structure** (or any other non-textual value) is serialized
  automatically to **JSON**, in `application/json`.

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	stClient is a Structure
		name is a string
		city is a string
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : stClient
			client is a stClient
			client.name = "Ada"
			client.city = "Lyon"
			RESULT client
		END
	END
END
```

This handler responds `{"name":"Ada","city":"Lyon"}` in
`application/json`.

**The exchange context.** Inside the handler body, `MySelf` carries two
sub-objects, addressed with the property operator `..`: `MySelf..Response`
groups what the handler sets on output (`StatusCode`, `Body`,
`ContentType`) and `MySelf..Request` what it reads from the incoming
request (`Method`, `Path`, `Identity`).

**Setting the status code.** `MySelf..Response..StatusCode` sets the
returned status — for example `httpCreated` for a creation. The chosen code
must appear in `Responses`:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	stNewClient is a Structure
		name is a string
	END

	CreateClient is an HTTPOperation
		PROPERTIES
			Method     = httpPost
			Path       = ["clients"]
			Responses  = [httpCreated]
			StatusCode = httpCreated
		END
		PROCEDURE onRequest(client is a stNewClient) : string
			MySelf..Response..StatusCode = httpCreated
			RESULT "created: " + client.name
		END
	END
END
```

**Explicit control of the body.** `MySelf..Response..Body` and
`MySelf..Response..ContentType` take over the automatic deduction: `Body`
replaces the returned value, `ContentType` forces the type header. Useful
to produce content already formatted:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	Ping is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["ping"]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest() : string
			MySelf..Response..ContentType = "application/json"
			MySelf..Response..Body        = `{"state":"ok"}`
			RESULT ""
		END
	END
END
```

Be careful not to confuse `..` and `.`: `MySelf..Response..StatusCode`
uses the double dot (property of the exchange context), while accessing a
structure field uses the single dot (`client.name`).

**Serving a binary file for download.** A body can be a `buffer` — a
sequence of raw bytes. A `buffer` body is transmitted **as-is, byte for
byte, without re-encoding**; a `string` body, on the other hand, is text.
That difference is what makes it possible to serve a PDF, an image or any
other binary **without corrupting it**: the content sent to the client is
exactly that of the buffer, without translation. Two forms produce this
result. The first explicitly sets the body and its type;
`Content-Length` then equals the size of the buffer:

```owl
DocumentsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadReport is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["reports", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : string
			content is a buffer = fLoadBuffer("reports/" + id + ".pdf")
			MySelf..Response..ContentType = "application/pdf"
			MySelf..Response..Body        = content
			RESULT ""
		END
	END
END
```

The second returns the buffer directly with `RESULT`: the result is the
same intact binary body. In the absence of a `..ContentType` set, a
`buffer` body returned this way takes the type `application/octet-stream`
— the generic type for binary. To announce a PDF (or any other precise
format), set `..ContentType` before the `RESULT`:

```owl
DocumentsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	DownloadReport is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["reports", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : buffer
			content is a buffer = fLoadBuffer("reports/" + id + ".pdf")
			MySelf..Response..ContentType = "application/pdf"
			RESULT content
		END
	END
END
```

`fLoadBuffer` reads the file into a buffer here; any byte source works the
same way (content generated in memory, data received from another service).

**Setting response headers.** `MySelf..Response..Headers` is an
associative array indexed by the **name** of the header. An assignment
adds — or replaces — a header of the response: `Cache-Control`, a CORS
header, a business header:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : string
			MySelf..Response..Headers["Cache-Control"] = "no-store"
			MySelf..Response..Headers["X-Version"]     = "1.0"
			RESULT "ok"
		END
	END
END
```

Key points:

- **The name is plural: `..Headers`.** The singular `..Header` does not
  exist and causes a compilation error.
- **Header names are case-insensitive**, in accordance with HTTP:
  `Cache-Control` and `cache-control` designate the same header.
- **`Content-Type` stays driven by `..ContentType`.** Setting a
  `Content-Type` via `..Headers` is ignored in favor of the dedicated
  channel, which avoids any duplicate.
- **Values are validated**: a malformed header name or value (a newline
  injection, for example) is rejected and the header is not emitted. The
  service stays protected.

**Reading the headers of the incoming request.** Symmetric to the response
side, `MySelf..Request..Headers` is an associative array indexed by the
**name** of the header, in **read-only**: it gives access to the headers
sent by the client (or injected by an intermediary). The most common case
is deployment behind a **reverse proxy** (nginx, Apache, HAProxy…), which
sets `X-Forwarded-For` (the real client address), `X-Forwarded-Proto` (the
original scheme), or any tracing header specific to the gateway:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : string
			clientIp is a string = MySelf..Request..Headers["X-Forwarded-For"]
			IF clientIp = "" THEN
				clientIp = "unknown"
			END
			RESULT clientIp
		END
	END
END
```

Key points:

- **Read-only.** `..Request..Headers` is for consulting the request; the
  headers of the response are set via `..Response..Headers`.
- **Names are case-insensitive**, in accordance with HTTP:
  `..Headers["x-forwarded-for"]` and `..Headers["X-Forwarded-For"]`
  designate the same header.
- **A missing header returns the empty string** — hence the fallback on a
  default value in the example.
- **A repeated header** in the request is presented once, its values
  **joined by `, `**.

**A handler that fails produces a clean 500.** If the handler code raises
an error, the request receives a `500` response and the server
**continues** to run: a faulty route does not bring down the service. The
error handler `onError` (see § 9) lets you customize this response.

## 7. Streaming a response

Up to here, `onRequest` computes a value and returns it: the body of the
response is produced **in one shot**, at the end of the handler. Some
operations need the opposite — to keep the connection open and **emit the
content in chunks, over time**: a live event stream, the progressive
export of a large dataset, an output generated as it goes. That is the role
of **streaming**.

Two elements activate it, always together:

- the `Stream` property on the `HTTPOperation`, which declares the
  streaming mode;
- the `StreamWrite` native, called in `onRequest` to emit each chunk.

**Choosing the mode with `Stream`.** Absent, the operation responds
normally (§ 6). Present, it takes one of two values:

| Value        | Streaming                                                     |
|--------------|---------------------------------------------------------------|
| `streamSSE`  | Events in *Server-Sent Events* format (each chunk is framed with the prefix `data: `, each line of a multi-line chunk being re-prefixed), for an event stream read by a web client. |
| `streamBytes`| Raw bytes transmitted continuously, without framing, for binary or text content produced on the fly. |

**Emitting with `StreamWrite`.** `StreamWrite(chunk)` sends a chunk to the
client **and continues execution** — that is the essential difference from
`RESULT`, which ends the handler. The `onRequest` handler stays **alive for
the whole duration of the stream**: it is called **once**, loops and emits
as many chunks as needed; the **end of the procedure marks the end of the
stream**. A streaming operation therefore does not use `RESULT`: it writes,
then ends.

**The chunk accepts any type.** Like the value of a `RESULT` (§ 6), the
argument of `StreamWrite` can be of any type. A **string** (or a
**buffer**) is emitted **as-is**, byte for byte; any **other value** —
structure, array, integer — is **serialized automatically to minified
JSON** (a single line, suited to an SSE event). This is the most common
use of SSE: emitting a structured event. Here, each turn sends a structure,
passed to the client as a JSON object:

```owl
NotificationsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	stEvent is a Structure
		number is an integer
		message is a string
	END

	Broadcast is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["notifications", from]
			Stream     = streamSSE
			MaxDelay   = 300
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(from is an integer)
			FOR i = from TO from + 3
				evt is a stEvent
				evt.number  = i
				evt.message = "up to date"
				StreamWrite(evt)
			END
		END
	END
END
```

A request on `/api/v1/notifications/5` then receives one event per loop
turn, each structure serialized to JSON:

```
data: {"number":5,"message":"up to date"}

data: {"number":6,"message":"up to date"}
```

Each `StreamWrite` goes out immediately on the open connection; the client
receives the events at the pace they are emitted, without waiting for the
end of the handler. To emit plain text rather than an object, pass a string
directly: `StreamWrite("event " + i)` produces `data: event 5`. A
**multi-line** chunk is framed line by line — each internal line receives
its own `data: ` prefix, in accordance with the SSE format.

**The status is frozen from the first chunk.** Sending the first
`StreamWrite` puts the headers and the status code **on the wire**: they
can no longer change afterwards. The status-control pattern described in
§ 6 — setting `MySelf..Response..StatusCode` then `RESULT` — therefore
holds only **before** the first chunk. An error that occurs **before** the
first `StreamWrite` produces a clean `500`, like a normal operation; an
error **during the stream** can no longer rewrite the status: it closes the
connection abruptly, the content already received staying truncated.

**Bounding the duration with `MaxDelay`.** `MaxDelay` sets a maximum
lifetime of the stream, **in seconds**. Past this delay, the server closes
the transport, even if the handler has not finished. The value `0` — the
default — means **no limit**: the stream lives as long as the handler emits
and the client listens. Setting a bound protects the service from
connections that linger.

**A client disconnection raises an exception.** If the client closes the
connection during the stream, the next call to `StreamWrite` **raises an
exception**. Without handling, it unwinds the handler and the framework
releases the stream — nothing to write for the common case. The only case
to handle is that of a **resource to release** (an open file, an external
connection): wrapping the emission in a `WHEN EXCEPTION IN … DO` is enough
to guarantee cleanup, whether the stream ends normally or is cut off by the
client leaving.

Two constants expose the code of these exceptions, to compare against
`ExceptionInfo(errCode)` to distinguish the cases:

| Constant               | Cause                                                              |
|------------------------|--------------------------------------------------------------------|
| `httpStreamDisconnected` | The client disconnected during the stream (or `MaxDelay` reached). |
| `httpStreamOutOfContext` | `StreamWrite` called outside an open stream (a programming error). |

The handler can thus catch only the disconnection — the normal case to
handle — and let any other incident propagate:

```owl
ExportService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	ExportLines is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["export"]
			Stream     = streamBytes
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest()
			WHEN EXCEPTION IN
				FOR i = 1 TO 1000
					StreamWrite("line " + i + RC)
				END
			DO
				IF ExceptionInfo(errCode) = httpStreamDisconnected THEN
					Trace("client gone: releasing resources")
				ELSE
					// Other incident: do not swallow it.
					ExceptionPropagate()
				END
			END
		END
	END
END
```

Comparing `ExceptionInfo(errCode)` against `httpStreamDisconnected` targets
precisely the client leaving; any other exception (including a
`StreamWrite` out of context, signaled by `httpStreamOutOfContext`) comes
back intact rather than being masked by the cleanup block.

## 8. Authenticating requests

A service can require that each request be **authenticated** before
reaching its operations. The service declares an authentication **scheme**
in its `PROPERTIES`, and provides a handler in charge of validating the
identity. The `httpAuthBearer` scheme expects a bearer token in the
`Authorization` header of the request.

**Declaring the scheme and the handler.** The `Authentication` property
chooses the scheme; the `onAuthentication` handler, declared **at the
service level** (next to the operations, outside any `HTTPOperation`),
receives the credential already extracted according to the scheme. For
`httpAuthBearer`, it is the token that follows `Bearer ` in the
`Authorization` header. The handler returns `True` to accept the request,
`False` to reject it:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl        = ["api", "v1"]
		Authentication = httpAuthBearer
	END

	stIdentity is a Structure
		name is a string
	END

	PROCEDURE onAuthentication(token is a string) : boolean
		IF token = "secret" THEN
			id is a stIdentity
			id.name = "Ada"
			MySelf..Request..Identity = id
			RESULT True
		END
		RESULT False
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : string
			ident is a stIdentity = MySelf..Request..Identity
			RESULT ident.name
		END
	END
END
```

**Depositing and re-reading the identity.** On acceptance, the handler
places the application identity in `MySelf..Request..Identity` — a value of
the application's choice (here a structure). Each operation then re-reads
it in `onRequest` via `MySelf..Request..Identity`, without redoing the
validation.

**Rejecting cleanly.** When `onAuthentication` returns `False`, the request
is rejected **before** `onRequest`: no operation is executed. The handler
can set the status of the refusal via `MySelf..Response..StatusCode`, for
example `httpUnauthorized`:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl        = ["api", "v1"]
		Authentication = httpAuthBearer
	END

	PROCEDURE onAuthentication(token is a string) : boolean
		IF token = "" THEN
			MySelf..Response..StatusCode = httpUnauthorized
			RESULT False
		END
		RESULT token = "secret"
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients"]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest() : string
			RESULT "ok"
		END
	END
END
```

If the handler sets no status, the refusal receives a **`401` by default**:
a refusal without an explicit code stays safe.

**Exempting an operation.** An operation can opt out of the scheme by
declaring `Authentication = httpAuthNone` in its `PROPERTIES`. It then
requires no authentication — useful for a login route or an open health
check:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl        = ["api", "v1"]
		Authentication = httpAuthBearer
	END

	PROCEDURE onAuthentication(token is a string) : boolean
		RESULT token = "secret"
	END

	Health is an HTTPOperation
		PROPERTIES
			Method         = httpGet
			Path           = ["health"]
			Authentication = httpAuthNone
			Responses      = [httpOk]
			StatusCode     = httpOk
		END
		PROCEDURE onRequest() : string
			RESULT "ok"
		END
	END
END
```

**The scheme must have its handler.** Declaring `Authentication` on a
service without providing `onAuthentication` produces a **compilation
warning**: without a handler, the scheme is not applied and the service
answers every request. The handler is required for the protection to take
effect.

**Authorizing beyond authenticating.** Authentication answers "*who* makes
the request?"; authorization answers "does this requester have the
**right** to perform this action?". The `onAuthorization` handler, also
declared **at the service level**, runs **after** `onAuthentication` and
**before** the operation. It receives no argument: it **reads** the
identity already deposited (`MySelf..Request..Identity`), as needed the
method and the path (`MySelf..Request..Method`, `MySelf..Request..Path`),
then returns `True` (authorized) or `False` (forbidden):

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl        = ["api", "v1"]
		Authentication = httpAuthBearer
	END

	stIdentity is a Structure
		role is a string
	END

	PROCEDURE onAuthentication(token is a string) : boolean
		IF token = "" THEN
			RESULT False
		END
		id is a stIdentity
		IF token = "admin" THEN
			id.role = "admin"
		ELSE
			id.role = "guest"
		END
		MySelf..Request..Identity = id
		RESULT True
	END

	PROCEDURE onAuthorization() : boolean
		ident is a stIdentity = MySelf..Request..Identity
		IF ident.role = "admin" THEN
			RESULT True
		END
		MySelf..Response..StatusCode = httpForbidden
		RESULT False
	END

	DeleteClient is an HTTPOperation
		PROPERTIES
			Method     = httpDelete
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : string
			RESULT "deleted"
		END
	END
END
```

What `onAuthorization` guarantees:

- **Safe rejection by default.** When it returns `False`, the request is
  rejected before the operation. Without a code set, the refusal receives a
  **`403` by default**; the handler can set another status via
  `MySelf..Response..StatusCode`.
- **It runs only if declared**, and **after** a successful
  authentication: a request rejected by `onAuthentication` (401) never
  reaches authorization.
- **An operation exempted from authentication** (`Authentication =
  httpAuthNone`) is also exempted from authorization: a public route goes
  through neither one nor the other.

## 9. Lifecycle and error handling

Beyond individual requests, a service can hook code onto its **lifecycle**
— a startup, a shutdown — and onto the **error path** when an operation
fails. These handlers are declared at the service level, next to the
operations.

**Initializing and releasing once.** Two handlers frame the life of the
service:

- `EndInitialization` runs **once**, at startup, before the service accepts
  the first request. This is the place to open a connection, prepare a
  cache, load a configuration.
- `Closing` runs **once**, at shutdown, to release what has been set up
  (close a connection, write a final log).

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	PROCEDURE EndInitialization()
		Trace("service ready")
	END

	PROCEDURE Closing()
		Trace("service stopped")
	END

	Health is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["health"]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest() : string
			RESULT "ok"
		END
	END
END
```

**Watch the naming.** These two handlers are called `EndInitialization`
and `Closing` — the same names as the lifecycle events elsewhere in the
language, reused here as-is. They are **not** `onInitialization` or
`onClosing`: only the handlers tied to a request carry the `on` prefix
(`onRequest`, `onAuthentication`, `onAuthorization`, `onError`).

If `EndInitialization` fails, the startup is **aborted**: a service that
cannot initialize does not go into listening mode, rather than serving in
an incomplete state.

**Customizing the error response.** When an operation raises an error, the
default response is a `500` with an empty body. The `onError` handler lets
you take over this response. It **receives no argument** today; it consults
the current request (`MySelf..Request`) and **writes** the response to
return (`MySelf..Response..Body`, `..StatusCode`, `..ContentType`). The
detail of the error that triggered it is read with `ExceptionInfo()` — the
message without an argument, or a precise field with an `errXxx` constant —
as in any error handling:

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	PROCEDURE onError()
		MySelf..Response..ContentType = "application/json"
		MySelf..Response..Body        = `{"error":"internal"}`
	END

	ReadClient is an HTTPOperation
		PROPERTIES
			Method     = httpGet
			Path       = ["clients", id]
			Responses  = [httpOk]
			StatusCode = httpOk
		END
		PROCEDURE onRequest(id is an integer) : string
			RESULT "ok"
		END
	END
END
```

`onError` is **optional**: without it, a faulty operation produces the
clean `500` described in § 6. If it is present, the final response takes
over what it set, and in the absence of an explicit code stays a `500`. The
service continues to run in all cases.

## 10. Starting the server

The `owl serve` command compiles a file, mounts the services it declares
and puts the server into listening mode:

```bash
owl serve MyAPI.owl
```

By default, the server listens on `127.0.0.1:8080` (reachable from the
local machine only). Two options adjust the listening:

| Option        | Effect                                             | Default      |
|---------------|----------------------------------------------------|--------------|
| `--port N`    | Listening port.                                    | `8080`       |
| `--bind addr` | Listening address. `0.0.0.0` exposes to the network. | `127.0.0.1`  |

```bash
owl serve MyAPI.owl --port 9000 --bind 0.0.0.0
```

The server runs until a keyboard interrupt (`Ctrl-C`), which stops it
cleanly.

Beyond `owl serve`, a service is also **packaged as a standalone server
executable**: a single binary that puts the service into listening mode
with no interpreter to deploy. See [Packaging an
application](43_packaging.md), section "Packaging a server (ServiceREST)".

## 11. Describing the service in OpenAPI

A service already carries its full description: methods, paths, parameters,
response codes, bodies. The `owl openapi` command **derives** an [OpenAPI
3](https://www.openapis.org/) document from it — the standard format for
describing an HTTP API, readable by client tools, code generators and
interactive explorers:

```bash
owl openapi MyAPI.owl
```

The document is written as JSON on standard output, ready to redirect to a
file or to pass to another tool:

```bash
owl openapi MyAPI.owl > openapi.json
```

For a service exposing a single read operation, the output looks like this
(faithful excerpt):

```json
{
  "openapi":"3.0.3",
  "info":{
    "title":"ClientsService",
    "version":"1.0.0"
  },
  "paths":{
    "/api/v1/clients/{id}":{
      "get":{
        "operationId":"ReadClient",
        "parameters":[
          {
            "name":"id",
            "in":"path",
            "required":true,
            "schema":{"type":"integer"}
          }
        ],
        "responses":{
          "200":{
            "description":"200 OK",
            "content":{
              "application/json":{
                "schema":{"type":"string"}
              }
            }
          },
          "404":{
            "description":"404 Not Found",
            "content":{
              "application/problem+json":{
                "schema":{"type":"object"}
              }
            }
          },
          "500":{
            "description":"500 Internal Server Error",
            "content":{
              "application/problem+json":{
                "schema":{"type":"object"}
              }
            }
          }
        }
      }
    }
  },
  "components":{}
}
```

Each operation becomes an entry of `paths`: the name of the operation gives
the `operationId`, the path and query parameters become `parameters`, and
each code of `Responses` produces a described response. The titles take the
name of the service.

**A `500` is always described.** Any operation can fail and produce a `500`
(see § 6): the document therefore systematically adds a `500` response — in
`application/problem+json` — to each operation that does not already declare
one in `Responses`. The contract thus reflects the real behavior of the
service, where any route can return an internal error.

**The content type follows the streaming mode.** For an operation without
`Stream`, the success response is described in `application/json`. For a
streaming operation (§ 7), the document reflects the media actually served:
`text/event-stream` for `streamSSE`, `application/octet-stream` (binary
schema) for `streamBytes`. The error responses stay, for their part, in
`application/problem+json` whatever the mode.

**Excluding an operation from the document.** Every route serves by default
as the source of its own description. The `ExcludeFromOpenAPI` property on
an `HTTPOperation` removes the operation from the generated document
**without changing its routing**: at `True`, the route is still served
normally, but it appears neither in `paths` nor elsewhere in the contract.
Useful to hide an internal route — a health check, an administration route,
documentation plumbing — from a public contract. The default is `False`
(the operation is documented):

```owl
ClientsService is a ServiceREST
	PROPERTIES
		BaseUrl = ["api", "v1"]
	END

	Health is an HTTPOperation
		PROPERTIES
			Method           = httpGet
			Path             = ["health"]
			ExcludeFromOpenAPI = True
			Responses        = [httpOk]
			StatusCode       = httpOk
		END
		PROCEDURE onRequest() : string
			RESULT "ok"
		END
	END
END
```

Here the route `/api/v1/health` responds normally, but stays absent from
the `openapi.json`.

**The document is derived, never a second source.** There is nothing to
keep up to date in parallel: the description comes from the service itself.
Modifying an operation then regenerating is enough to obtain an up-to-date
document; editing the JSON by hand makes no sense, the next generation
would overwrite it.

**Served online.** A server started by `owl serve` exposes the same
document on the route `GET /openapi.json`. A client can therefore fetch the
description directly from the running service, without an intermediate
file:

```bash
curl http://localhost:8080/openapi.json
```

## 12. Full cycle

The `owl new` command **prints** a service skeleton on standard output;
redirect it to a file to edit it:

```bash
owl new service MyAPI > MyAPI.owl
```

The `MyAPI.owl` file then contains a minimal `ServiceREST` with a read
operation. All that is left is to add or adapt the operations, then start
the server:

```bash
owl serve MyAPI.owl
```

The service is then queryable:

```bash
curl http://localhost:8080/api/v1/clients/42
```

## French aliases

| EN canon        | FR canon         |
|-----------------|------------------|
| `ServiceREST`   | `ServiceREST`    |
| `HTTPOperation` | `OpérationHTTP`  |
| `onRequest`     | `surRequête`     |
| `MySelf`        | `MoiMême`        |
| `Authentication` | `Authentification` |
| `onAuthentication` | `surAuthentification` |
| `onAuthorization` | `surAutorisation` |
| `onError`       | `surErreur`      |
| `EndInitialization` | `FinInitialisation` |
| `Closing`       | `Fermeture`      |
| `httpAuthBearer` | `httpAuthBearer` |
| `httpAuthNone`  | `httpAuthAucune` |
| `Response` (context) | `Réponse` |
| `Request` (context) | `Requête`   |
| `BaseUrl`       | `UrlBase`        |
| `Method`        | `Méthode`        |
| `Path`          | `Chemin`         |
| `Identity`      | `Identité`       |
| `Query` (binding list) | `Requête` |
| `Responses`     | `Réponses`       |
| `StatusCode`    | `CodeÉtat`       |
| `Body`          | `Corps`          |
| `ContentType`   | `TypeContenu`    |
| `Headers`       | `Entêtes`        |
| `ExcludeFromOpenAPI` | `ExclureDeOpenAPI` |
| `Stream`        | `Flux`           |
| `MaxDelay`      | `DélaiMax`       |
| `StreamWrite`   | `FluxÉcrit`      |
| `httpStream`    | `httpFlux`       |
| `httpStreamDisconnected` | `httpFluxDéconnecté` |
| `httpStreamOutOfContext` | `httpFluxHorsContexte` |
| `streamSSE`     | `fluxSSE`        |
| `streamBytes`   | `fluxOctets`     |
| `UploadedFile`  | `FichierTéléversé` |
| `FileName`      | `NomFichier`     |
| `Size`          | `Taille`         |
| `MimeType`      | `TypeMime`       |
| `mimeTypeMultipartForm` | `typeMimeFormulaireMultiPartie` |

See also: [The project.owl file](24_projet.md), [Encoding and
input/output](26_encodage_et_io.md).
