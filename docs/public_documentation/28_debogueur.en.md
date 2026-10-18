# Debugger

## 1. The OWL debugger, in brief

OWL ships a debugger in its toolchain, as a standalone binary —
`owl-dap` — delivered alongside `owl.exe`. This binary implements the
**DAP** (Debug Adapter Protocol), a standard protocol for exchanges
between a debugger and an editor.

A direct consequence: the DAP protocol makes the debugger **universal**
— `owl-dap` plugs into any DAP-compatible client, which drives the
session (set a breakpoint, step, inspect) without the debugger itself
knowing the editor. The debugger is `owl-dap`; the editor is only a
front end.

To date, the actively supported editor is **Visual Studio Code**, whose
matching plugin provides the ready-to-use integration (see the box in §4
and the OWL extension documentation). The protocol does not, however,
exclude any other DAP-compatible client.

---

## 2. What the debugger allows

The debugger suspends execution and hands over control to observe and
drive the program. Concretely, `owl-dap` offers:

- **Breakpoints**: suspend execution at a given line.
- **Full stepping**, once the session is suspended:
  - **resume** execution until the next breakpoint;
  - move to the next line **without entering** calls (step over);
  - **step into** a procedure call, or **step out** back to the caller;
  - **pause** a running execution.
- **Call stack**: walk up the chain of calls that led to the breakpoint.
- **Variable inspection**: read the **local** variables of the current
  procedure **and** the **global** variables.
- **Expression evaluation**: evaluate an expression in the suspended
  context — from the Watch panel, by hovering over a variable in the
  code (hover), or in the debug console.

These capabilities are independent of the client: they are part of the
debugger, not of the editor that displays it.

---

## 3. A breakpoint only stops on an executed line

A breakpoint suspends execution only if it is set on a line that is
**actually executed**; on a blank line, a comment or a procedure header,
it causes no pause. The practical rule: set it on an **instruction line**
of the path taken (below, the second one).

```owl
sName is a string = "the world"
sMessage is a string = "Hello, " + sName   // useful breakpoint here
Trace(sMessage)
```

---

## 4. Connecting a DAP client

Since the debugger is driven by the client, the procedure to follow
depends on the editor. The principle stays constant: open a `.owl` file,
set a breakpoint on an executed line, start debugging. The client then
talks to `owl-dap`.

> **Example with a DAP client — Visual Studio Code**
>
> With the OWL extension installed, there is no configuration to write:
> the plugin provides a default debug configuration (no configuration
> file to write by hand). Set a breakpoint in the gutter of a `.owl`
> file, then launch debugging of the active file; its neighbors in the
> same project are loaded with it.
>
> Installation and usage details — keyboard shortcuts, diagnostic
> channel to trace the exchanges with the debugger — belong to the
> plugin documentation, which should be consulted. This chapter
> describes the framework debugger, not a particular editor.

---

## Further reading

| Topic | Reference |
|---|---|
| Common language pitfalls | [`22_pieges_courants.md`](22_pieges_courants.md) |
| Control flow and procedures | [`03_controle_de_flux.md`](03_controle_de_flux.md) |
| Built-in tests | [`11_charte_de_test.md`](11_charte_de_test.md) |

For editor-specific details, refer to the documentation of the DAP
client in use.
