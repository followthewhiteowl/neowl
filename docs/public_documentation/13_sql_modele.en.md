# The SQL model

A minimal, secure-by-construction SQL layer is available, with
**eight backends** reachable through the same API:

- `sqlSQLite` — embedded SQLite engine (file or `:memory:`)
- `sqlDuckDB` — embedded analytical engine (OLAP, Parquet, JSON)
- `sqlPostgres` — PostgreSQL server via native protocol
- `sqlMySQL` — MySQL / MariaDB server via native protocol
- `sqlMSSQL` — Microsoft SQL Server via native TDS
- `sqlHFSQL` — client/server SQL database, embedded driver (zero install)
- `sqlHFSQLClassic` — local file-based database, serverless (driver and engine embedded)
- `sqlODBC` — any DBMS through a third-party ODBC driver

The API boils down to three types and three functions. No ORM, no
schema generator, no query DSL: you write SQL, while execution and
binding safety are handled for you.

Version, authentication and TLS details per backend are in
`14_sql_connecteurs.md`.

---

## 1. The three types

| Type | Role |
|---|---|
| `sqlConnection` | Connection descriptor (provider, source, credentials…) + internal handle |
| `sqlQuery` | SQL text with `{name}` placeholders + `Parameters` associative array |
| `sqlResponse` | Execution result: success, returned rows, affected rows |

Every property is available in French and in English, both forms being
interchangeable line by line.

### `sqlConnection`

| Field FR | Field EN | Type | Note |
|---|---|---|---|
| `Provider` | `Provider` | `integer` | `sqlSQLite`, `sqlDuckDB`, `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL`, `sqlHFSQLClassic`, `sqlODBC` |
| `Source` | `Source` | `string` | SQLite/DuckDB file, or full ODBC connection string |
| `Serveur` | `Server` | `string` | Host of network backends (`host` or `host:port`) |
| `Port` | `Port` | `integer` | Explicit override (0 = backend default port) |
| `BaseDeDonnées` | `Database` | `string` | Database name |
| `Utilisateur` | `User` | `string` | Login |
| `MotDePasse` | `Password` | `string` | Password |
| `Pilote` | `Driver` | `string` | Registered ODBC driver name (`sqlODBC` only) |
| `Analyse` | `Analysis` | `string` | Analysis (schema) file of a local file-based database (`sqlHFSQLClassic` only) |
| `Répertoire` | `Directory` | `string` | Folder of the data files of a local file-based database (`sqlHFSQLClassic` only) |
| `ModeTLS` | `TLSMode` | `integer` | `tlsDisabled`, `tlsAcceptAll`, `tlsVerifyCA`, `tlsVerifyFull` (default) |
| `CertificatCA` | `CACertificate` | `string` | Path to a PEM file, internal CA to use as root |
| `TimeoutCnx` | `ConnectTimeout` | `integer` | Open timeout **in seconds** (0 = 5 s by default), honored by `sqlODBC`, `sqlHFSQL`, `sqlHFSQLClassic` and `sqlMSSQL` |

### `sqlQuery`

| Field FR | Field EN | Type |
|---|---|---|
| `Code` | `Code` | `string` — SQL with `{name}` placeholders |
| `Paramètres` | `Parameters` | `associative array` |

### `sqlResponse`

| Field FR | Field EN | Type |
|---|---|---|
| `Succès` | `Success` | `boolean` |
| `Lignes` | `Rows` | `array` of `associative array` |
| `Occurrence` | `Count` | `integer` — number of returned rows |
| `LignesAffectées` | `AffectedRows` | `integer` — number of rows created, modified or deleted |

Each field describes **the query that was just sent**, never the history of the
connection. A read therefore reports `AffectedRows` as 0, and a write reports
`Rows` empty and `Count` as 0. These values are identical regardless of the
connector: the same program yields the same figures on SQLite as on PostgreSQL.

The two measures stay **independent**: `Rows` and `Count` describe what the
query *returned*, `AffectedRows` what it *wrote*. A query that does both — a
write with a returning clause, `RETURNING` or `OUTPUT` — fills both on
`sqlSQLite`, `sqlDuckDB` and `sqlPostgres`: neither measure falls back to 0 on
the grounds that the other is set.

> **The guarantee stops at these three connectors.** On `sqlMySQL` (MariaDB),
> `sqlMSSQL` and `sqlODBC`, a write with a returning clause does return its rows
> in `Rows` and `Count`, but `AffectedRows` is 0 there: the protocol of these
> connectors cannot tell this form apart from a read. Counting the returned rows
> is the reliable workaround there, since the clause returns exactly one row per
> written row.

---

## 2. The three functions

| Function FR | Function EN | Method prefix |
|---|---|---|
| `SQLConnexionOuvre(cnx)` | `SQLConnectionOpen(cnx)` | `cnx.Ouvre()` / `cnx.Open()` |
| `SQLConnexionFerme(cnx)` | `SQLConnectionClose(cnx)` | `cnx.Ferme()` / `cnx.Close()` |
| `SQLEnvoie(cnx, req)` | `SQLSend(cnx, req)` | — |

The prefix methods and the global functions are equivalent — picking
one or the other is a matter of style.

---

## 3. Idiomatic skeleton

On SQLite (local file):

```owl
cnx is a sqlConnection
cnx..Provider = sqlSQLite
cnx..Source   = "./data.db"
cnx.Open()

req is a sqlQuery
req..Code = "SELECT * FROM customers WHERE country = {p}"
req..Parameters["p"] = "FR"

rep is a sqlResponse = SQLSend(cnx, req)
FOR EACH line OF rep..Rows
    Trace(line["name"])
END

cnx.Close()
```

On PostgreSQL (network, TLS by default):

```owl
cnx is a sqlConnection
cnx..Provider = sqlPostgres
cnx..Server   = "db.example.com:5432"
cnx..Database = "production"
cnx..User     = "app"
cnx..Password = "..."
// cnx..TLSMode = tlsVerifyFull  // default, redundant
cnx.Open()
```

On a client/server server (`sqlHFSQL`):

```owl
cnx is a sqlConnection
cnx..Provider = sqlHFSQL
cnx..Server   = "base.local:4900"
cnx..Database = "invoice"
cnx..User     = "admin"
cnx..Password = "..."
cnx..TLSMode  = tlsDisabled  // the server handles its own encryption
cnx.Open()
```

The examples use SQLite for compactness; the mechanics are identical on
the other connectors, only the `Provider` field changes.

---

## 4. SQL injection — neutralized by construction

**SQL injection** is the flaw where an attacker slips SQL code into a
value that the server will execute instead of treating as data. A
classic on forms:

```owl
// ❌ Flaw — the entered string becomes code
req..Code = "SELECT * FROM customers WHERE name = '" + userInput + "'"
```

If `userInput` is `"'; DROP TABLE customers; --"`, the sent query runs
the `DROP`. The table is gone.

The **named parameters** pattern is mandatory: you never write the
value in the SQL text, you pass it through `req..Parameters["name"]`.
The engine prepares the query (the SQL is frozen), then binds the value
as **data** — never as code.

```owl
// ✅ Secure — userInput cannot escape into SQL
req..Code = "SELECT * FROM customers WHERE name = {name}"
req..Parameters["name"] = userInput
```

Even if `userInput` carries all the malice in the world, the server
treats it as a text literal. The table stays intact.

The `{name}` placeholder is turned into the backend's canonical
positional placeholder (`:name` SQLite/DuckDB, `$N` Postgres, `?`
MySQL/MSSQL/ODBC) at pre-processing. Names are free as long as they are
valid identifiers.

---

## 5. Iterating over rows

The result of a `SELECT` is a standard OWL `array`; you iterate with
`FOR EACH … OF …`:

```owl
FOR EACH line OF rep..Rows
    Trace(line["id"], line["name"])
END
```

Each `line` is an `associative array` indexed by column name. Types are
preserved on read (see §7).

For direct indexed access, rows are 1-based like any OWL array:

```owl
first is an associative array = rep..Rows[1]
Trace(first["name"])
```

---

## 6. Reusing a `sqlQuery`

A query is reusable. You change the parameters and call `SQLSend`
again:

```owl
ins is a sqlQuery
ins..Code = "INSERT INTO customers (name, email) VALUES ({n}, {e})"

FOR EACH c OF customersToImport
    ins..Parameters["n"] = c.Name
    ins..Parameters["e"] = c.Email
    SQLSend(cnx, ins)
END
```

The prepared statement is internal to `SQLSend` (prepare + bind + step
+ finalize in one go). No cursor object to manage.

---

## 7. OWL ↔ SQL type mapping

The reference mapping below concerns SQLite. The other backends have
their specifics (BIT vs INTEGER, native DATETIME, BUFFER vs VARBINARY,
Unicode NVARCHAR…) — see `14_sql_connecteurs.md` for each backend.

### Bind (OWL → SQLite)

| OWL type | SQLite | Note |
|---|---|---|
| `integer` | `INTEGER` | int64 |
| `real` | `REAL` | double |
| `string` | `TEXT` | UTF-8 |
| `boolean` | `INTEGER` | 0 or 1 |
| `Buffer` | `BLOB` | binary bytes |
| `Null` | `NULL` | direct |
| `Date`/`Time`/`DateTime` | ❌ | convert manually via `.ToString()` |
| `array`, others | ❌ | explicit error |

### Read (SQLite → OWL)

| SQLite | OWL type returned |
|---|---|
| `INTEGER` | `integer` |
| `REAL` | `real` |
| `TEXT` | `string` (UTF-8) |
| `BLOB` | `Buffer` |
| `NULL` | `Null` |

---

## 8. Beyond `SELECT`

`SQLSend` accepts any SQL the target backend understands: `INSERT`,
`UPDATE`, `DELETE`, `CREATE TABLE`, `CREATE VIEW`, `BEGIN` / `COMMIT` /
`ROLLBACK`, trigger declarations, stored-procedure calls… No
transactional wrapper is provided: transaction management stays the
responsibility of the application SQL. If an error occurs between a
`BEGIN` and its `COMMIT`/`ROLLBACK`, the transaction stays pending until
the connection is closed.

---

## 9. Errors

`SQLSend` always returns a `sqlResponse` — never an exception (except a
fatal error such as OOM). On failure:

- `rep..Success = False`
- `ErrorOccurred = True` (global variable)
- `ErrorInfo()` returns an explanatory message (including the backend's
  native code)

```owl
rep is a sqlResponse = SQLSend(cnx, req)
IF NOT rep..Success THEN
    Trace("Failure: ", ErrorInfo())
END
```

`cnx.Open()` and `cnx.Close()` return a `boolean`: `True` on success,
`False` otherwise (with `ErrorInfo()` filled in).

`cnx.Close()` is **idempotent**: closing an already-closed connection is
a silent no-op.

---

## 10. Points of attention

Common to all connectors:

- **Datetime types.** `Date` / `Time` / `DateTime` are converted
  manually to an ISO `string` (`"YYYY-MM-DD HH:MM:SS"`) before binding.
  Datetime SQL columns are read as ISO `string`.
- **Eager read.** `rep..Rows` loads the entire result into memory. For
  very large results, paginate with `LIMIT` on the SQL side.
- **Freeing the memory of a `sqlResponse`.** A large response (a direct
  consequence of the eager read) occupies memory as long as a variable
  references it. In a short scope, the memory is reclaimed
  automatically on scope exit, with nothing special to do. However, when
  a large response is kept in a long scope — a global variable, a loop
  that chains queries — and is no longer useful before the natural end
  of that scope, reassigning the variable releases the reference: the
  garbage collector then reclaims the former content. Two equivalent
  forms: overwrite the variable with the next response, or assign it to
  `Null`.

  ```owl
  rep is a sqlResponse = SQLSend(cnx, req)
  FOR EACH line OF rep..Rows
      Trace(line["name"])
  END
  rep = Null   // releases the reference: the former result becomes collectable
  ```

Per-connector specifics: see `14_sql_connecteurs.md`.

---

## 11. Quick reference

```owl
// Setup
cnx is a sqlConnection
cnx..Provider = sqlSQLite
cnx..Source   = "./db.sqlite"
cnx.Open()

// One query, several executions
req is a sqlQuery
req..Code = "SELECT * FROM t WHERE x = {x}"
req..Parameters["x"] = 42

rep is a sqlResponse = SQLSend(cnx, req)

// Read
FOR EACH line OF rep..Rows
    Trace(line["col"])
END

// Status
IF rep..Success THEN
    Trace(rep..Count, " rows")
ELSE
    Trace(ErrorInfo())
END

cnx.Close()
```

---

## Going further

| Topic | Sheet |
|---|---|
| Versions, auth, TLS per connector | `14_sql_connecteurs.md` |
| General conventions | `10_conventions_de_code.md` |
| Test charter | `11_charte_de_test.md` |
| Naming prefixes | `12_prefixes_de_nommage.md` |

Connection, query and iteration examples are available for each of the
supported connectors.
