# Logging

The language ships a **leveled application log**: the `Logger` type and its
`Logger*` family of functions. The design follows current practice (structured
logs, verbosity levels, a clean split between *producing* an entry and *shaping*
it). The core stays minimal — four levels, a decomposed record — and **two
extension points** let it fit any collection chain (files, platform agents, cloud
systems) **without extra dependencies**: a **formatting** procedure decides the
emitted shape, a **destination** decides where it goes out.

Like the rest of the language, every symbol is **bilingual** EN/FR: the English
canonical forms (`LoggerInfo`, `Level`, `Formatter`…) are equivalent to the
French forms and are listed at the end of this page.

## 1. Default log and instances

Two ways to write a log, identical in behavior.

**The default log**, declaring nothing: the functions `LoggerDebug`, `LoggerInfo`,
`LoggerWarn` and `LoggerError` emit directly. It is the shortest path, suited to a
program or service that holds a single log. `LoggerSetting` configures that
implicit log:

```owl
LoggerSetting(lvlInfo)                   // threshold: Info and above
LoggerInfo("Service started")
LoggerDebug("Below the threshold, not emitted")
LoggerError("Database connection refused")
```

**A `Logger` instance**, declared and configured through properties (`..`), when a
program holds several distinct logs (one per component, say). The methods
`.Debug`, `.Info`, `.Warn`, `.Error` emit on the instance:

```owl
log is a Logger
log..Level = lvlDebug                    // this log is more verbose
log.Info("Loading configuration")
log.Debug("3 sections read")
```

The type and the static functions carry the **same behavior**: what follows holds
for both.

## 2. The four levels

A level ranks the importance of an entry. From least to most severe:

| Level | When to use it | Example |
|-------|----------------|---------|
| `lvlDebug` | Development diagnostics: values, intermediate states, paths taken. The most verbose. | "empty cache for key `u-42`" |
| `lvlInfo` | Normal expected events, the "it works" thread. | "service started", "order 1007 handled" |
| `lvlWarn` | Abnormal but **non-blocking** situation: fallback to a default, a retry, a deprecated use. | "2nd attempt to connect to the broker" |
| `lvlError` | **Failure** of an operation: caught exception, request that did not complete. | "payment refused: insufficient funds" |

**The threshold filters at emission.** The `..Level` property (or the
`LoggerSetting` argument) sets a **severity floor**: an entry is emitted if its
level is **at least as severe** as the threshold, otherwise it is **dropped at no
cost**. The severity order is `lvlDebug < lvlInfo < lvlWarn < lvlError`. Setting a
level does *not* "allow" the *less* severe ones: with `..Level = lvlInfo`, `Info`,
`Warn` and `Error` pass (Info and more severe), while `Debug` (less severe) stays
silent. Lowering the threshold opens more entries, raising it restricts them; in
production one typically stays at `lvlInfo` and drops to `lvlDebug` for the time
of an investigation.

## 3. The record and the formatter

When an entry crosses the threshold, the log builds a **decomposed record**, the
`LogRecord` type, whose fields are read with `..`:

| Property | Content |
|----------|---------|
| `..Timestamp` | Date-time of the entry. |
| `..Level` | The entry's level, as an enumeration value (`lvlInfo`…) — useful to compare. |
| `..LevelText` | The level as **text** (`DEBUG` / `INFO` / `WARN` / `ERROR`) — to use in the formatted output, where `..Level` would only give an internal value. |
| `..Message` | The message passed to the call. |

By default, the entry comes out in a simple, neutral text form. To decide the
emitted shape **yourself**, set a formatting procedure on the `..Formatter`
property: it receives the record and returns the string to emit. The log keeps
the emission; the procedure decides only the shape.

```owl
// A key=value (logfmt) format, readable and easy to index.
PROCEDURE formatLogfmt(rec is a LogRecord)
	RETURN `ts=[%rec..Timestamp%] level=[%rec..LevelText%] msg="[%rec..Message%]"`
END

log is a Logger
log..Formatter = formatLogfmt
log.Info("Order handled")
```

This single point is what lets you match the shape expected by any collector,
with nothing to install: just write the procedure that produces the wanted form.

## 4. Destination and emitter

The **where** is separate from the **how**. Two properties, depending on the need:

- `..Destination` — a built-in target. By default the standard output; a file path
  may be given. This is the common, recommended case: the program writes to the
  standard output (or a file), and a **platform agent** (container collector,
  logging daemon) handles the shipping.
- `..Emitter` — an emission procedure, when you want to **push** each entry
  yourself to a remote system. It receives the formatted line and sends it with
  the language's **own** functions (`HTTPSend`, `AWSRequest`…). No dedicated
  library per system: the framework plugs into itself.

```owl
log is a Logger
log..Destination = "service.log"         // a file instead of the standard output
log.Info("Started")
```

In the vast majority of cases, a structured format on the standard output is
enough — the model the three integrations below follow.

## 5. Three standard integrations

Same principle everywhere: **adapt the formatter** (and, if needed, the emitter),
never add a library. The business code itself always calls `log.Info(...)` the
same way.

### EFK (Elasticsearch / Fluentd / Kibana) — JSON on the standard output

The collector ingests JSON line by line. The formatter produces a JSON object, the
destination stays the standard output: the collection agent does the rest.

```owl
PROCEDURE formatJSON(rec is a LogRecord)
	RETURN `{"@timestamp":"[%rec..Timestamp%]","level":"[%rec..LevelText%]","message":"[%rec..Message%]"}`
END

LoggerSetting(lvlInfo, formatJSON)       // default log: threshold + formatter
LoggerInfo("Request handled")
LoggerError("Database unreachable")
```

### Grafana / Loki — logfmt on the standard output

Loki indexes labels and stores the line. The `logfmt` (`key=value`) format is read
natively in Grafana; the agent (Promtail, Alloy) attaches the labels from the
container. On the program side, only the formatter changes:

```owl
PROCEDURE formatLogfmt(rec is a LogRecord)
	RETURN `level=[%rec..LevelText%] ts=[%rec..Timestamp%] msg="[%rec..Message%]"`
END

LoggerSetting(lvlInfo, formatLogfmt)
LoggerInfo("Batch imported")
```

### Containerized AWS Lambda (Docker) — JSON captured by CloudWatch

In a Lambda function packaged as a Docker image, **everything written to the
standard output is captured automatically to CloudWatch Logs** — no SDK or network
call to make from the code. It is therefore enough to emit structured JSON on the
standard output: CloudWatch Logs Insights can then filter on the fields (`level`,
`requestId`…).

```owl
// The formatter enriches the entry with a request id, handy to correlate the
// lines of a single invocation in CloudWatch.
PROCEDURE formatLambda(rec is a LogRecord)
	RETURN `{"time":"[%rec..Timestamp%]","level":"[%rec..LevelText%]","msg":"[%rec..Message%]","requestId":"[%gRequestId%]"}`
END

gRequestId is a string = ""

// Called at the start of each handler invocation.
PROCEDURE handleEvent(requestId is a string)
	gRequestId = requestId
	LoggerInfo("Invocation received")
	// ... processing ...
	LoggerInfo("Invocation done")
END

LoggerSetting(lvlInfo, formatLambda)
```

If a case requires **pushing** the entries to an API (rather than letting the
platform collect the standard output), that is the role of `..Emitter`: a procedure
relaying each line through `AWSRequest` or `HTTPSend`. The "standard output
captured by the platform" model remains, however, the simplest and most robust,
and is the one recommended in a container.

## French aliases

**Type and record**

| EN canon | FR canon |
|----------|----------|
| `Logger` | `Logger` |
| `LogRecord` | `LogEnreg` |

**Functions**

| EN canon | FR canon |
|----------|----------|
| `LoggerDebug` | `LoggerDebug` |
| `LoggerInfo` | `LoggerInfo` |
| `LoggerWarn` | `LoggerAvert` |
| `LoggerError` | `LoggerErreur` |
| `LoggerSetting` | `LoggerParamètre` |

**Methods**

| EN canon | FR canon |
|----------|----------|
| `.Debug` | `.Debug` |
| `.Info` | `.Info` |
| `.Warn` | `.Avert` |
| `.Error` | `.Erreur` |

**Properties**

| EN canon | FR canon |
|----------|----------|
| `..Level` | `..Niveau` |
| `..LevelText` | `..NiveauTexte` |
| `..Formatter` | `..Formateur` |
| `..Destination` | `..Destination` |
| `..Emitter` | `..Émetteur` |
| `..Timestamp` | `..Horodatage` |
| `..Message` | `..Message` |

**Levels (`logLevel` / `logNiveau`)**

| EN canon | FR canon |
|----------|----------|
| `lvlDebug` | `nivDebug` |
| `lvlInfo` | `nivInfo` |
| `lvlWarn` | `nivAvert` |
| `lvlError` | `nivErreur` |

See also: [AWS](36_aws.en.md), [Messaging](41_messagerie.en.md), [REST
services](40_services_rest.en.md).
