# Packaging an application

Once a project is ready, the `owl-build` tool turns it into a
**standalone native binary**: a single executable, distributable as is,
that needs nothing installed on the target machine. This sheet describes
what `owl-build` produces, its prerequisites and its options. The content
of the `projet.owl` file itself — metadata, languages, startup code — is
detailed in `24_projet.md` and not repeated here.

## 1. What `owl-build` produces

`owl-build` takes an OWL project as input and writes a native executable
that embeds the program and everything it needs to run. The resulting
binary launches directly, with no interpreter or library to deploy
alongside it.

## 2. No external tool required

`owl-build` carries **its own build toolchain**: there is no compiler,
runtime or dependency to install beforehand.

On **first launch**, `owl-build` extracts its internal payload into a
temporary folder; depending on the machine and the antivirus, this step
takes from a few seconds to several minutes, and its progress is shown in
the console. **Subsequent launches** reuse this extraction and start
immediately.

This extraction can be **paid up front** with the `--prepare` option:
`owl-build --prepare` warms up the cache for the host platform, then
stops without packaging a project. A first real build then starts
immediately. The option is useful for a deployment that prepares the
machine ahead of time. It honors `--target` to warm up another target,
and does nothing (without error) on a binary that embeds no payload.

## 3. Prerequisites on the project side

`owl-build` expects a real **project**: a folder containing a `projet.owl`
file that declares an `is a Project` block. This block carries the
metadata (at least the `Languages` field) and **its body is the
program** — the application's start point. The companion files of the
folder provide their definitions (windows, classes, collections…); the
code of `projet.owl` orchestrates them.

The full project model — metadata fields, execution order, cascade
access — is described in `24_projet.md`.

## 4. Command line

```
owl-build [PROJECT_FOLDER] [-o NAME] [--target TRIPLE]
```

| Argument / option | Role |
|-------------------|------|
| `PROJECT_FOLDER`  | Folder of the project to package. Required; use `.` for the current folder. |
| `-o NAME`         | Name of the produced executable. By default, derived from the project. |
| `--target TRIPLE` | Target platform. By default, the host platform. |
| `--prepare`       | Warms up the extraction cache for the target, then stops without packaging a project. The next build for that target starts immediately. |

### Choosing the target

The `--target` option designates the platform to produce the binary for.
Without it, the target is the **host platform** — the one `owl-build`
runs on.

`owl-build` can produce for `windows-x86_64` as well as for
`linux-x86_64`. When both targets are embedded, a single `owl-build`
produces a Windows executable just as well as a standalone static Linux
binary, regardless of the platform it runs on: producing a Linux binary
from Windows (or the other way around) then requires no extra tool.

```
owl-build mon_projet --target linux-x86_64
```

The actually available targets remain those embedded in the `owl-build`
binary used; a given distribution may offer more or limit itself to its
platform.

## 5. Location of the result

The binary is written under the project folder, in an `exe` tree:

```
<project>/exe/<target>/<name>[.exe]
```

The `<target>` segment reflects the value of `--target`, that is the
target platform. The `.exe` suffix appears only for targets that require
it.

## 6. Build parameters

As of today, the packaging parameters come down to the **command-line
options** described above: input folder, output name, target.

There is not yet a build manifest declared in the project itself. This
capability — describing the packaging parameters within `projet.owl` — is
planned and will be documented here once it is opened.

## 7. Minimal example

A project reduced to the essentials: a `projet.owl` that declares its
metadata and shows a message at startup.

```owl
// project.owl
MyApplication is a Project
	PROPERTIES
		Languages   = [languageFrench]
		Description = "Address book"
	END

	Trace("Starting " + MyApplication..Description)
END
```

From the folder that contains this file, packaging is launched by
explicitly designating the current folder with `.`:

```bash
owl-build .
```

Launched without a folder, `owl-build` shows its help and stops without
packaging anything: the folder to process is always indicated, `.`
included.

Or by explicitly naming the folder and the produced executable:

```bash
owl-build mon_projet -o carnet
```

The standalone binary then appears under `mon_projet/exe/<target>/`,
ready to be distributed and run without installing anything else.

## 8. Packaging a server (ServiceREST)

When a project declares an `is a ServiceREST` block, `owl-build`
recognizes it and produces a **server executable**: on launch, the binary
sets up the service, opens a port and answers requests until it stops. It
is not a program that runs once and then exits, but a process that stays
listening.

No extra option is required at packaging: the presence of a `ServiceREST`
in the project boundary is enough to produce this type of binary. A
project without a service keeps being packaged as an ordinary program.

The declaration of the `ServiceREST` block, its `OperationHTTP`
operations and the `onRequest` handler is described in
`40_services_rest.md`.

### Launching the packaged server

The produced executable is configured **at launch**, by command line:

```
<exe> [--port N] [--bind ADDR]
```

| Option      | Role |
|-------------|------|
| `--port N`  | Listening port. `8080` by default. |
| `--bind ADDR`| Listening address. `127.0.0.1` by default. |
| `-h`, `--help` | Shows the help and stops. |

By default, the server listens only on `127.0.0.1`: it is reachable only
from the local machine. To open it to the network, explicitly indicate
`--bind 0.0.0.0`.

```bash
# Local listening on port 9000
monserveur --port 9000

# Network opening on the default port
monserveur --bind 0.0.0.0
```

An invalid port (non-numeric or outside the `1`–`65535` range) triggers
an error message and an immediate stop. The server stops with `Ctrl-C`.

As the same binary deploys as is on several machines, the address and the
port are chosen at runtime and not at packaging.

## 9. Current limitations (alpha)

Two limits to know before distributing a binary produced by `owl-build`:

- **Unsigned binary.** The produced executables carry no publisher
  signature; Windows SmartScreen may show an "unknown publisher" warning
  on first launch. Signing is planned.
- **Recoverable source code.** The project's `.owl` code is embedded in
  the binary in **compressed** form: it does not appear in clear text in
  the executable, but remains recoverable through a determined analysis.
  Avoid, for now, packaging code whose confidentiality is critical. A
  strengthening of the protection is under study.
