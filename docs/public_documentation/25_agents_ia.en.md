# Coding with an AI

The language is designed from the start to be handled by an AI agent as
much as by a human. The principle and the reasons for this choice are
laid out in `00_apercu_du_langage.md` § 14. This page describes the
**how-to**: how to concretely wire an AI onto a project so that it
understands, explores and checks the code without guessing or resorting
to outside sources.

No prior knowledge of AI is needed to follow this page. It is not even
necessary to explain everything to the agent beforehand: it can be left
to **discover on its own** what is available. A single command,
`owl help`, lists all the exploration commands; then by chaining
`owl explain` or `owl list` on a few symbols, an agent grasps the
language's possibilities and its way of doing things within minutes, and
runs its own onboarding without intervention.

---

## 1. Two routes: the command line or the MCP server

There are two ways to give an AI an exact access to the language and its
framework. Both expose the same information; they differ only in the
access mode.

**The command line, directly.** This is the default route, available
immediately, **with no configuration or setup at all**. An agent able to
run shell commands has everything it needs: `owl help` enumerates the
exploration commands (`owl explain <name>`, `owl search <term>`,
`owl examples <name>`, `owl list …`), the checking command (`owl --check`)
and the formatting command (`owl fmt`). There is nothing to install or
declare: the binary answers on its own.

**The MCP server.** The same information, exposed this time through a
structured protocol. It is more comfortable for an agent that prefers to
call declared tools rather than compose shell commands. This route needs a
small configuration declaration, described in the rest of this page.

Neither of the two is mandatory at the other's expense: the command line
requires nothing and works right away, the MCP server is extra comfort for
clients that know how to make use of it.

---

## 2. The built-in server

The `owl` binary embeds an **MCP server** (Model Context Protocol). MCP is
a standard protocol by which an AI assistant connects to an external tool
to ask it questions. Here, the external tool is the language itself: the AI
queries the installed version of the language and its framework, and gets
exact answers drawn from the binary, not from an approximate memory.

The server is launched with the command:

```bash
owl mcp
```

The transport used is `stdio` (standard input/output). In practice, it is
**not** necessary to run this command by hand: the AI client takes care of
it on its own, once it knows the server exists. So it is enough to
**declare** it in the client configuration. That is the subject of the next
section.

---

## 3. Wiring a client

Each MCP-compatible AI client has its own configuration file, but the
content is almost always the same: declare a server named `owl`, transport
`stdio`, running the command `owl mcp`.

**Claude Code** — file `.mcp.json` at the project root:

```json
{
  "mcpServers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

**Cursor** — file `.cursor/mcp.json` (per project) or `~/.cursor/mcp.json`
(global):

```json
{
  "mcpServers": {
    "owl": { "command": "owl", "args": ["mcp"] }
  }
}
```

**Visual Studio Code** (built-in agent) — file `.vscode/mcp.json`. The root
key here is `servers`, not `mcpServers`:

```json
{
  "servers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

**Any other MCP-compatible client** follows the same principle: declare a
server with transport `stdio` that runs the command `owl mcp`. The file
name and the root key vary from one client to another, but the command
launched stays `owl mcp`.

The `command` field is `owl` when the binary is reachable on the `PATH`.
Otherwise, give the full path to the executable.

### Remote or local AI

The server depends on no online service: it runs on the machine where `owl`
is installed. The **model** that connects to it, however, can be remote (an
assistant hosted online) or local (a model run on the same machine). In
both cases, the wiring is identical: it is the MCP client that bridges the
model and the `owl mcp` server. A local model therefore connects in exactly
the same way, as long as the client used knows how to speak MCP.

---

## 4. What the AI can do once wired

The server provides a set of tools that the assistant calls on its own as
needed. They fall into two families.

**Understand and explore the language:**

| Tool | What it returns |
|---|---|
| `owl_explain` | Full page for a symbol: description, signatures, properties, enumerations, scenarios. One name can designate both a type and a function; both are returned. |
| `owl_search` | Cross-cutting search, case-insensitive, across the documentation, the registry (natives, types, enumerations, constants) and the scenarios. To find "what touches a subject" without knowing the exact name. |
| `owl_examples` | Canonical scenarios that demonstrate a symbol's use, code included as is. |
| `owl_where` | Reverse lookup: who uses this symbol. For a property, lists the elements that accept it; for an element type, gives the parent context. |
| `owl_signature` | Short form of a native: only the signatures `Name(arg : type) → return`, without description or example. |
| `owl_list` | Enumerates a family of the catalog: natives, types, constants, keywords, operators, interface elements, events, properties, predefined global variables, compiler limits. The `categories` family gives the index of functional categories (interface, strings, dates, SQL…), which a `cat` filter then lets you target. |

**Check and produce code:**

| Tool | What it returns |
|---|---|
| `owl_check` | Compile-time diagnostic, on an isolated snippet. Returns a JSON `{errors, warnings}` with line, column and message. Does not resolve the other project files and runs nothing. |
| `owl_format` | Reformats a source according to the canonical form (case, accents, indentation) and returns the corrected code. |
| `owl_translate` | Rewrites the language surface in the other language (`fr` or `en`): keywords, natives, types and properties change language; identifiers, strings and comments stay unchanged. |
| `owl_template` | Ready-to-use skeletons, with examples: script, class, test scenario, window. |
| `owl_new` | Minimal frame of an element (window, class, control): declaration and basic events, to get started. |
| `owl_limits` | Registry of the compiler limits (number of types, of methods, of parameters, etc.), to size a project in advance. |

The assistant picks the relevant tool itself: no need to point it out at
each question. These same tools have a direct command-line equivalent
(`owl explain`, `owl search`, `owl examples`, `owl --check`, `owl fmt`…),
which lets an agent without MCP do exactly the same work.

---

## 5. Recommended workflow

An agent can start with an **autonomous onboarding**: `owl help` to discover
the commands, then `owl explain` and `owl list` on the symbols encountered.
In a few calls, it maps the possibilities and the conventions of the
language, without anyone having to describe them. Two guarantees then make
code generation reliable, and are worth using explicitly.

**Deterministic diagnostics.** The command `owl --check` (and the tool
`owl_check` on the server side) produces errors and warnings in JSON format,
identical from one run to the next. An agent gains by systematically
checking a snippet with `owl --check` **before** writing it to disk:
compile errors are caught right away, with their exact position, rather than
discovered later.

**Canonical form restored.** There is only one canonical way to write each
construction. The formatter restores it — case, accents, indentation — via
`owl fmt` (or `owl_format`). An AI therefore does not have to memorize the
exact case of a name or choose between several styles: it produces code that
is approximate on form, and the canonical form is restored mechanically. The
substance stays its responsibility, the form is guaranteed by the tool.

Combining the two — check with `owl --check`, normalize with `owl fmt` —
gives a short and safe loop: generate, check, fix, without ever guessing.

---

## Going further

The motivation for this design choice — why the language is built for AI
agents — is described in `00_apercu_du_langage.md` § 14.

The same information exposed by the server is accessible directly from the
command line, notably `owl explain <name>` and `owl examples <name>`.
