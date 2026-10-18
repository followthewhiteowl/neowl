# SQL connectors — versions and specifics

Eight native connectors and one generic connector:

| Connector | For |
|---|---|
| `sqlSQLite` | SQLite — local file or `:memory:` |
| `sqlDuckDB` | DuckDB — local file or `:memory:` (OLAP, Parquet) |
| `sqlPostgres` | PostgreSQL |
| `sqlMySQL` | MySQL / MariaDB |
| `sqlMSSQL` | Microsoft SQL Server / Azure SQL |
| `sqlHFSQL` | Client/server SQL server, embedded driver |
| `sqlHFSQLClassic` | Local file-based database, serverless |
| `sqlODBC` | Any DBMS with a third-party ODBC driver |

This page lists the specifics worth knowing before coding against a
real server: supported versions, authentication modes, TLS, error
codes. For the semantics of the constants themselves:
`owl explain sqlPostgres`, `owl explain tlsVerifyFull`, etc.

---

## Which field for which connector

The `sqlConnection` descriptor exposes a common set of fields, but each
field concerns only a subset of the connectors. This matrix indicates,
for each field, where it applies — enough to remove any doubt before
filling in a connection.

| Field | Connectors concerned |
|---|---|
| `Provider` | all (chooses the connector) |
| `Source` | expert mode: `sqlSQLite`/`sqlDuckDB` (file path), `sqlODBC`, `sqlHFSQL`, `sqlHFSQLClassic` (raw connection string) |
| `Server` | network: `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL` (Client/Server), `sqlODBC` |
| `Port` | network: `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL` |
| `Database` | network: `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL`, `sqlODBC` |
| `User` / `Password` | authentication: `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL`, `sqlODBC` |
| `TLSMode` / `CACertificate` | network only: `sqlPostgres`, `sqlMySQL`, `sqlMSSQL` |
| `Driver` | `sqlODBC` (name of the installed ODBC driver) |
| `Analysis` / `Directory` | `sqlHFSQLClassic` only |

A field outside its column is not an error: it is ignored, and
`Open()` proceeds exactly as if it had not been set. So that an ignored
field does not cost an hour of searching, a warning is still emitted on
the error output, naming the discarded field and listing those the
chosen connector takes into account:

```
[OWL] WARNING SQLConnectionOpen: cnx..Source is set but ignored by the
sqlPostgres connector. Fields handled by this connector: Server, Port,
Database, User, Password, TLSMode, CACertificate.
```

Classic mistake targeted by this warning: describing a network server
in `Source` (`"127.0.0.1:5432"`) where the connector expects `Server`.
The expert `Source` mode of `sqlODBC` / `sqlHFSQL` / `sqlHFSQLClassic`
(raw connection string, see below) is a normal use of `Source` and
triggers no warning. Neither does `TLSMode`, since it always has a
value; its special case for `sqlHFSQL` is covered in the dedicated
section.

---

## TLS — network connectors (PostgreSQL / MySQL / MSSQL)

Four modes exposed through `cnx..TLSMode`: `tlsDisabled` /
`tlsAcceptAll` / `tlsVerifyCA` / `tlsVerifyFull` (production default).
Details of each mode in `owl explain`.

For an internal CA (enterprise, AD CS, local OpenSSL) not present in
the default trust bundle:

```owl
cnx..TLSMode        = tlsVerifyCA
cnx..CACertificate  = "C:/certs/ca-internal.pem"
```

The provided CA **replaces** the default bundle — the trust choice is
explicit. Field supported by the three network connectors.

`sqlHFSQL` and `sqlODBC` do not expose `TLSMode` on the connection
string side (see their sections).

---

## PostgreSQL

| PG version | Auth plugins | Status |
|---|---|---|
| **PG 8.0 → 9.6** | MD5 password | ✅ |
| **PG 10 → 17** | SCRAM-SHA-256 (default), MD5 (compat) | ✅ |
| Pre-8.0 (≤ 2004) | trust / clear | ❌ |

Lower bound: **PG 8.0 (2005)**. SCRAM requires PG 10+; MD5 strongly
discouraged from PG 10 onward.

---

## MySQL / MariaDB

| Version | Auth plugin | Status |
|---|---|---|
| **MySQL 5.0 → 5.7** | `mysql_native_password` | ✅ |
| **MySQL 8.0 → 8.4** | `caching_sha2_password` (fast path) | ✅ |
| **MariaDB 5.x → 11.x** | `mysql_native_password` | ✅ |
| MariaDB with `ed25519` | `client_ed25519` | ❌ |
| MariaDB with `gssapi_client` | GSSAPI | ❌ |

Lower bound: **MySQL 5.0 (2005)**.

**Slow path `caching_sha2`**: refused by default (sends the password in
clear to the server, even over TLS). Typical error case: a new user
never authenticated → empty server cache. Solution: authenticate this
user once through the official `mysql` client, the cache fills up, then
OWL works.

---

## MSSQL (Microsoft SQL Server)

| Version | TDS protocol | Status |
|---|---|---|
| **SQL Server 2012 → 2022** | TDS 7.4 | ✅ main target |
| **SQL Server 2008 R2 / 2008** | TDS 7.3 | ⚠ compatible, manual validation |
| ≤ SQL Server 2005 | TDS 7.2 / 7.1 | ❌ |
| Azure SQL Database | TDS 7.4 + Force Encryption | ✅ with `tlsVerifyFull` |
| Azure Synapse SQL | TDS 7.4 (subset) | ✅ standard queries |

**Authentication**: `user / password` only. Windows Integrated (NTLM /
Kerberos / SSPI) and Azure Active Directory are not supported.

**Specific TLS**: Azure SQL and any SQL Server with "Force Encryption
ON" require `tlsVerifyFull` (or `tlsVerifyCA + CACertificate` for an
internal CA). `tlsDisabled` only works if Force Encryption is OFF on
the server side.

**Named parameters**: routed through `sp_executesql` RPC (injection-safe,
see `13_sql_modele.md §4`). `NVARCHAR(MAX)` / `VARBINARY(MAX)` PARTLEN,
MARS and bulk insert are not covered.

---

## Client/server database server (`sqlHFSQL`)

| OS | Status |
|---|---|
| Windows x86_64 | ✅ |
| Linux x86_64 | ✅ |
| macOS | ❌ |

**Transport encryption**: specific to the server engine, **not standard
PKI/X.509 TLS**. The encryption level is configured on the server side
and negotiated automatically at connection time.

Consequence in code: `cnx..TLSMode` has **no effect** on this
connector. As a safety measure (to avoid a false impression of X.509
TLS), a runtime warning is emitted as long as the value differs from
`tlsDisabled`. To silence it, set explicitly:

```owl
cnx..Provider = sqlHFSQL
cnx..TLSMode  = tlsDisabled    // explicit signal: encryption is on the server side
```

**Authentication**: account configured on the server side.

---

## Local file-based database (`sqlHFSQLClassic`)

Access to a database stored in **local files** (data files and their
indexes), **with no server** to install or contact. The driver and the
engine are embedded in OWL: zero installation on the workstation.

| OS | Status |
|---|---|
| Windows x86_64 | ✅ |
| Linux x86_64 | ✅ |
| macOS | ❌ |

**Two mandatory fields**:

- `cnx..Analysis` — path to the **analysis file** (the database
  schema). Without this file, the data cannot be queried: this is a
  hard limit of the connector (the schema cannot be inferred from the
  data files alone).
- `cnx..Directory` — path to the **folder containing the data files**.

```owl
cnx is a sqlConnection
cnx..Provider  = sqlHFSQLClassic
cnx..Analysis  = "C:/Databases/Billing/Billing.wdd"
cnx..Directory = "C:/Databases/Billing/Data"

IF cnx.Open() THEN
    req is a sqlQuery
    req..Code = "SELECT ClientID, Name FROM Client ORDER BY Name"
    rep is a sqlResponse = SQLSend(cnx, req)
    FOR EACH row OF rep..Rows
        Trace(row["Name"])
    END
    cnx.Close()
ELSE
    Trace(ErrorInfo())
END
```

**Encryption / TLS**: purely local access, **no network transport**.
`cnx..TLSMode` makes no sense here and **no warning is emitted**
(unlike the Client/Server mode).

**Configuration errors**: if `Analysis` or `Directory` is empty,
`cnx.Open()` returns `False` and `ErrorInfo()` names the missing field.

**Expert mode**: a set `cnx..Source` short-circuits the assembly and
takes priority (raw connection string, as for `sqlODBC`).

---

## ODBC (Oracle, Access, Firebird, …)

`sqlODBC` opens access to any DBMS with an installed ODBC driver.
Escape hatch for DBMS not covered by a native connector.

> For the embedded client/server server, use `sqlHFSQL` rather than
> going through `sqlODBC` (which would require the user to install the
> driver separately).

| OS | Status |
|---|---|
| Windows | ✅ (native driver manager since Win 2000) |
| Linux | ✅ |
| macOS | ❌ |

**Validated drivers (Windows)**: ODBC Driver 17/18 for SQL Server,
Oracle Instant Client 12c+, IBM DB2, Microsoft Access (`.mdb`/`.accdb`),
Sybase ASE, Firebird. Any ODBC 3.x driver is accepted.

**Two API modes**:

```owl
// Mode 1 — full connection string
cnx..Provider = sqlODBC
cnx..Source   = "DRIVER={Oracle in OraClient21Home1};DBQ=prod_db;UID=app;PWD=..."

// Mode 2 — structured fields (OWL assembles internally)
cnx..Provider = sqlODBC
cnx..Driver   = "Oracle in OraClient21Home1"
cnx..Server   = "prod_db"
cnx..User     = "app"
cnx..Password = "..."
```

If `Source` is set, it takes priority (expert mode).

**Authentication & TLS**: entirely delegated to the driver. `TLSMode`
and `CACertificate` have no effect — each third-party driver exposes
its own parameters in its connection string (`Encrypt=yes`,
`SslMode=verify-full`, etc.). Consult the documentation of the driver
concerned.

**Limitations**: BLOB/CLOB materialized in memory (no streaming), only
the first result set of a multi-cursor stored procedure is read.

### ODBC diagnostics

On failure, `ErrorInfo()` returns a message in the format:

```
SQLConnectionOpen (ODBC): SQLDriverConnect: [SQLSTATE] native_code <message>
```

The `SQLSTATE` (5 characters, ISO standard) helps diagnose. Useful
codes:

| Code | Cause |
|---|---|
| `IM002` | Data source name not found — driver misnamed or not installed |
| `IM003` | Specified driver could not be loaded — 32/64 bitness mismatch |
| `28000` | Invalid authorization specification — credentials |
| `08001` | Client unable to establish connection — network / firewall |

---

## Oracle

No native connector. Two options:

- **`Provider = sqlODBC`** + Oracle Instant Client ODBC driver
  installed on the machine.
- Migrate the scope concerned to PostgreSQL or MSSQL.

---

## Diagnosing a failing connection

`Open()` returns `False` and **nothing is displayed**: this is the
convention of all operations that can fail without being programming
faults (file access, network call, connection). The full detail is not
lost for all that — it is placed in `ErrorInfo()`, and that is the only
thing to read when a connection refuses to open:

```owl
IF NOT cnx.Open() THEN
    Trace(ErrorInfo())
END
```

The message names the operation, the target contacted, and the native
code returned by the server or the driver, which is generally enough to
identify the cause:

```
SQLConnectionOpen (Postgres): tcp connect: connect: 127.0.0.1:5432
unreachable (timeout 5000ms) (code 1).
```

> **Pitfall to avoid**: substituting a custom message for the
> diagnostic, for example `Error("connection failed")` in the failure
> branch. The generic message replaces the only usable information.
> Display `ErrorInfo()`, or include it in the custom message.

When the target is local (`127.0.0.1`, `::1`, `localhost`) and a TLS
mode is required, the detail adds the most frequent lead: a local
development server is very often configured without encrypted
transport, whereas the default `tlsVerifyFull` imposes it. In this
specific case, set `cnx..TLSMode = tlsDisabled` — and only for this
local server.

The field to query separately is available through the error
information constants (`ErrorInfo(errMessage)` for the message alone),
useful to test the cause in validation code.

Concrete connection examples are available for the main DBMS
(PostgreSQL, MySQL, `sqlHFSQL`) and serve as a starting point to
reproduce a problem — edit the credentials before running.

---

## Further reading

| Topic | Page |
|---|---|
| SQL model (connection, query, result set) | `13_sql_modele.md` |
| Language overview | `00_apercu_du_langage.md` |
| Code conventions | `10_conventions_de_code.md` |

For the semantics of a given constant: `owl explain <constant>`
(e.g.: `sqlPostgres`, `tlsVerifyFull`).
