# AWS services: `AWSSession`, `AWSRequest` and dedicated clients

## 1. In brief

Two layers let you reach AWS services from OWL:

- **The generic call `AWSRequest`** — usable with **any** service (S3,
  IAM, DynamoDB, EC2, STS, API Gateway…). You pass it a **session** of
  credentials, a **service**, an **action** and a **body**; it signs the
  request (SigV4), sends it and returns the response decoded as `JSON`.
- **Dedicated clients** — `AwsS3Client`, `AwsBedrockClient`,
  `AwsLambdaClient` — which offer a more direct object form for the most
  common operations of their service.

Every request starts from the same credentials sheet, `AWSSession`: a
region and two keys.

```owl
sess is an AWSSession
sess..Region    = "us-east-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

body is a JSON = StringToJSON("{}")
response is a JSON = AWSRequest(sess, awsIAM, awsIAMListUsers, body)

Trace(response.ListUsersResponse.ListUsersResult.IsTruncated)
```

The response is a `JSON` document walked with the single dot
(`response.Path.To.The.Field`), exactly like any other `JSON`.

---

## 2. The session: `AWSSession`

`AWSSession` is a composite type that gathers the call credentials. Its
properties are read and written with the double dot `..`:

| Property | Role | Required |
|---|---|---|
| `..Region` | Targeted AWS region (e.g. `us-east-1`). | yes |
| `..AccessKey` | Access key identifier. | yes |
| `..SecretKey` | Secret access key. | yes |
| `..Token` | Session token for temporary credentials. | no |
| `..Endpoint` | Custom entry point. | no |

A session declared without initialization starts with all its properties
empty; you fill them in before the first call.

```owl
sess is an AWSSession
sess..Region    = "eu-west-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
```

### Temporary credentials: `..Token`

Credentials obtained temporarily (assumed role, time-limited session)
come with a **session token** in addition to the two keys. It is set on
`..Token` and then goes out automatically with every signed request.

```owl
sess is an AWSSession
sess..Region    = "us-east-1"
sess..AccessKey = "ASIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
sess..Token     = "AQoDYXdzEJr...EXAMPLETOKEN"
```

### Custom entry point: `..Endpoint`

By default, the entry point is derived automatically from the service and
the region. Setting it on `..Endpoint` forces a precise address — handy to
target a self-hosted compatible service or a local test environment.

```owl
sess is an AWSSession
sess..Region   = "us-east-1"
sess..Endpoint = "http://localhost:4566"
```

---

## 3. The generic call: `AWSRequest`

`AWSRequest(session, service, action, body)` covers the whole set of
services. Its four arguments:

| Argument | Type | Role |
|---|---|---|
| `session` | `AWSSession` | Credentials and region. |
| `service` | `∈ awsService` | The targeted service, chosen from the `awsService` enumeration. |
| `action` | symbol or string | The operation to invoke. |
| `body` | `JSON` | The request body. |

It returns the **response decoded as `JSON`**.

### The service: constrained to `awsService`

The second argument is chosen from the `awsService` enumeration (`awsS3`,
`awsIAM`, `awsDynamoDB`, `awsEC2`, `awsSTS`, `awsBedrockRuntime`…). Since
it is an enumeration value and not a free string, a typo on the service is
reported at compile time, not at the moment of the network call.

### The action: a symbol checked early, or a free string

The action is expressed in two ways:

- **By symbol** of the form `aws<Service><Action>` — for example
  `awsIAMListUsers`, `awsS3ListBuckets`, `awsDynamoDBGetItem`. This symbol
  is **resolved at compile time**: an unknown action is caught early, and
  the call embeds the exact framing of the operation along the way
  (method, path, protocol).
- **By free string** as a fallback — for example `"ListUsers"`. Handy for
  an operation that would not yet have a dedicated symbol; the name is
  taken as is.

```owl
sess is an AWSSession
sess..Region    = "us-east-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

body is a JSON = StringToJSON(`{"TableName":"Clients","Key":{"id":{"S":"42"}}}`)

// Action by symbol (checked at compile time)…
bySymbol is a JSON = AWSRequest(sess, awsDynamoDB, awsDynamoDBGetItem, body)

// … or by free string, taken as is.
byString is a JSON = AWSRequest(sess, awsDynamoDB, "GetItem", body)
```

### The body and the response

The body is a `JSON`: the native adapts it to the protocol expected by the
service (JSON body, form, or structured document depending on the case).
The response comes back **decoded as `JSON`**, whatever the original shape
returned by the service. The **lists** in the response are exposed as
`JSON` **arrays**, indexed from `1`.

```owl
sess is an AWSSession
sess..Region    = "us-east-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

body is a JSON = StringToJSON("{}")
response is a JSON = AWSRequest(sess, awsSTS, awsSTSGetCallerIdentity, body)

account is a string = response.GetCallerIdentityResponse.GetCallerIdentityResult.Account
Trace(account)
```

---

## 4. The dedicated clients

For the most-used services, an **object client** offers a more direct form
than the generic call. The client is declared, receives a session via
`..Session`, then exposes its operations as method calls (`.Method(...)`).

### Object storage: `AwsS3Client`

`AwsS3Client` covers file operations on S3 in path style (`/bucket/key`):

| Method | Role |
|---|---|
| `.Upload(bucket, key, buffer, contentType)` | Puts an object (binary body). |
| `.Download(bucket, key)` | Fetches an object, returned as a `buffer`. |
| `.List(bucket, prefix)` | Lists the objects under a prefix, returned as `JSON`. |
| `.Delete(bucket, key)` | Deletes an object. |

```owl
sess is an AWSSession
sess..Region    = "eu-west-3"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

s3 is an AwsS3Client
s3..Session = sess

content is a buffer = "test content"
s3.Upload("my-bucket", "folder/file.txt", content, "text/plain")

objects is a JSON = s3.List("my-bucket", "folder/")
Trace(objects.Contents[1].Key)

bytes is a buffer = s3.Download("my-bucket", "folder/file.txt")
s3.Delete("my-bucket", "folder/file.txt")
```

The body put and the body fetched are `buffer`s: the bytes are transmitted
and restored without alteration, including for non-textual binary content.

### Inference: `AwsBedrockClient`

`AwsBedrockClient` invokes an inference model. The session is set on
`..Session`, then `.Invoke(model, body)` sends the supplied `JSON` body
and returns the model's response decoded as `JSON`.

```owl
sess is an AWSSession
sess..Region    = "eu-central-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

body is a JSON
body.anthropic_version = "bedrock-2023-05-31"
body.max_tokens = 100

bedrock is an AwsBedrockClient
bedrock..Session = sess

response is a JSON = bedrock.Invoke("eu.anthropic.claude-sonnet-4-6", body)
Trace(response.completion)
```

### Running functions: `AwsLambdaClient`

`AwsLambdaClient` invokes a remote function. `.Invoke(functionName,
payload)` passes the `JSON` payload as is and returns the response decoded
as `JSON`.

```owl
sess is an AWSSession
sess..Region    = "eu-central-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

payload is a JSON
payload.action = "ping"
payload.value = 42

lambda is an AwsLambdaClient
lambda..Session = sess

response is a JSON = lambda.Invoke("my-function", payload)
Trace(response.statusCode)
```

The clients share the same signature mechanism as the generic call: each
operation goes out signed, with the fingerprint of its body.

---

## 5. Responses and errors

Whatever the path (generic or dedicated client), the response is a `JSON`
walked with the single dot. Two forms of failure are to be distinguished.

### Error returned by the service

When the service rejects the request (refused credentials, missing
resource, invalid parameter…), it answers with an **error document**, also
decoded as `JSON`. That response then carries an `Error` node containing
`Code` (the short identifier of the error) and `Message` (the label). Just
inspect the response to detect it:

```owl
sess is an AWSSession
sess..Region    = "us-east-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

body is a JSON = StringToJSON("{}")
response is a JSON = AWSRequest(sess, awsIAM, awsIAMListUsers, body)

IF response.Error.Code <> "" THEN
	Trace("AWS failure: " + response.Error.Code + " — " + response.Error.Message)
END
```

### Local failure before sending

When the call cannot be built (a required path value is missing from the
body, for instance), the failure is **local**: no request goes out, and
`ErrorOccurred` is set. Test it right after the call, before any other
instruction.

```owl
sess is an AWSSession
sess..Region    = "us-east-1"
sess..AccessKey = "AKIAIOSFODNN7EXAMPLE"
sess..SecretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

body is a JSON = StringToJSON("{}")

ErrorReset()
response is a JSON = AWSRequest(sess, awsAPIGateway, awsAPIGatewayGetRestApi, body)
IF ErrorOccurred THEN
	Trace("request not built: " + ErrorInfo())
END
```

---

## 6. Out of current scope

- **No automatic pagination.** When a list exceeds one page, the response
  carries a **continuation token**; continuing the walk consists of
  passing that token back in the body of the next call. There is no
  paginator that unrolls the pages on its own.
- **No implicit date conversion.** A date or time value expected by a
  service is passed **as a string, as is**; it is not converted
  automatically from a date type.
- **Body fingerprint: nothing to do.** Some writes require a checksum of
  the body. It is **computed and set automatically** wherever the service
  requires it: no manual handling is needed.

---

## 7. Going further

| Topic | Guide |
|---|---|
| Handling `buffer`s (S3 binary bodies) | [`27_buffers.md`](27_buffers.md) |
| Encoding and input/output | [`26_encodage_et_io.md`](26_encodage_et_io.md) |
| Exception handling and `ErrorOccurred` | [`08_exceptions.md`](08_exceptions.md) |
| Querying a local language model (`LLM`) | [`35_llm.md`](35_llm.md) |

---

## French aliases

| EN canon | FR canon |
|---|---|
| `AWSRequest` | `AWSRequête` |
| `AWSSession` | `AWSSession` |
| `..Region` | `..Région` |
| `..AccessKey` | `..CléAccès` |
| `..SecretKey` | `..CléSecrète` |
| `..Token` | `..Jeton` |
| `..Endpoint` | `..Endpoint` |
| `.Download` | `.Télécharge` |
| `.List` | `.Liste` |
| `.Delete` | `.Supprime` |
| `.Upload` | `.Upload` |
| `.Invoke` | `.Invoque` |
