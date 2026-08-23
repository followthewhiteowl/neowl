# Writing NeOwl with a local AI 🦉🤖

[Français](LLM-LOCAL.md) · **English**

Have NeOwl driven by an AI running **on your own machine** — no cloud, no API key. The AI
relies on NeOwl's **MCP** server (`owl mcp`) to discover the language, write code, validate
it with `owl_check` and run it, in a loop.

> **Advice current as of 2026.** The ecosystem moves fast; model and tool names will change.
> The **method** stays valid.

> **What to expect.** On a consumer machine without a dedicated GPU, a local AI **works** but
> is **slow** in agent mode (response time grows with the conversation). For **fast and
> reliable** results, the same setup accepts a **cloud model** (Claude, GPT): you plug in a
> key instead of the local model. **The agent and the MCP are the constant; the model is the
> privacy ↔ power dial.**

---

## How it works

Three pieces:

1. **The server** — it loads the model, runs generation, and **guarantees its tool calls are
   in the right format**. Two choices:
   - **NeOwl's native server** (`owl serve-llm`) — **recommended**: nothing else to install,
     the `owl` binary does it all.
   - **Ollama** — a widespread alternative, useful in particular to exploit a **dedicated GPU**.
2. **The agent** — **OpenCode** (terminal) or **Cline** (in VS Code): an open-source agent
   that executes tools and drives the loop on its own.
3. **The bridge to NeOwl** — the `owl mcp` server.

The AI searches for functions (`owl_search`), reads their signature (`owl_signature`),
writes the code, **validates with `owl_check`**, fixes, runs.

> **Why the MCP is essential.** "OWL" is also the name of a W3C semantic-web standard (the
> *Web Ontology Language*). Without the MCP to anchor it, a model writes… RDF. The MCP gives
> it the real grammar of the NeOwl language.

---

## What do you need? (hardware and model)

Your **available RAM** determines which model you can run (it must fit in memory, alongside
your system). Recommendations for **agentic** use:

| Available RAM | Recommended model | Why |
|---|---|---|
| **≤ ~16 GB** | *(local unreliable)* → **cloud model** | Only a ≤ 7B model fits, and at that size it can't reliably drive tools (it describes what it would do instead of acting). |
| **~24 – 32 GB** | **Qwen2.5-14B-Instruct** (general) | The sweet spot: it drives the loop **and** codes correctly. The default pick. |
| **48 GB and up** | **Qwen2.5-32B-Instruct** (general) | More reliable at driving, better code. Any good *Instruct* model known for tool-calling works too. |

- **The CPU** decides the speed (with no dedicated GPU, everything runs on it).
- **A dedicated GPU** with enough video memory (~12 GB for a 14B) speeds things up a lot.
  NeOwl's native server currently runs **on the CPU**; to exploit a dedicated GPU locally, use
  **Ollama** (see step 2), which uses it automatically.
- **Disk**: a few GB per model.

> RAM guide: a 14B model in Q4 quantization uses ~10-17 GB when running.

### Which *kind* of model?

For tool-calling, the way a model phrases its calls matters — but **the server handles that
for you**:

- With **NeOwl's native server**, the tool-call format is **guaranteed by construction**: any
  model, including a "**Coder**" model, produces executable calls.
- With **Ollama**, prefer a general "**Instruct**" model (e.g. Qwen2.5-**Instruct**, not
  Qwen2.5-**Coder**): a Coder model emits its calls in a format Ollama doesn't always convert,
  and the agent then executes nothing.

Either way, a **general** model usually decides **when** to call a tool better; size (see the
table) remains the first quality factor.

---

## Step 1 — Get a model

You need a model file in **GGUF** format (`Q4_K_M` quantization for a good size/quality
trade-off). Download it as a **single `.gguf` file** from a page that offers it that way, e.g.
**<https://huggingface.co/bartowski/Qwen2.5-14B-Instruct-GGUF>** (*Files* tab →
`Qwen2.5-14B-Instruct-Q4_K_M.gguf`, ~9 GB).

> **If you run into several files** named `...-00001-of-00003.gguf`, `...-00002-of-00003.gguf`,
> etc.: that's not a choice to make — it's **one model split into parts**. By far the simplest
> is to grab a repo that provides it as **one file** (like the one above). Otherwise you must
> **merge** the parts into a single `.gguf` — command `llama-gguf-split --merge first-part.gguf
> output.gguf`.

*(Ollama can also fetch the model for you with `ollama pull` — see the Ollama option in step 2.)*

---

## Step 2 — Run the model

Pick **one** of the two options. The first is the simplest; the second is useful if you have a
dedicated GPU or already use Ollama.

### Option A — NeOwl's native server (recommended)

The `owl` binary runs the model itself. One command is enough:

```
owl serve-llm --model C:\path\to\qwen2.5-14b-instruct-q4_k_m.gguf --port 11434 --ctx-size 16384
```

This exposes an OpenAI-compatible API on `http://127.0.0.1:11434/v1`, ready for the agent.

- `--ctx-size 16384` sets the **context window**. An agent's prompts are large (they contain
  the description of all the tools): 16384 is a good starting point. Larger = more memory used.
- The tool-call format is **guaranteed**: nothing to configure on the model side, and even a
  Coder model works.
- **CPU only** for now: on a machine with no GPU in use, generation is slow (see "Speed"
  below). For speed, look at GPU via Ollama, or a cloud model.

Leave this command running in its terminal; the agent connects to it.

### Option B — Ollama (for a dedicated GPU, or if you already use it)

**Ollama** is a widespread local server. It **automatically uses a recognized dedicated GPU**,
which speeds up generation a lot. Download it from **<https://ollama.com/download>** (Windows
installer, no admin rights).

Get the model — either `ollama pull qwen2.5:14b`, or by importing the `.gguf` from step 1.
Then create a derived model with an enlarged context window (Ollama caps it otherwise, which
makes agent tasks fail). Create a **`Modelfile`** (no extension):

```
FROM qwen2.5:14b
PARAMETER num_ctx 16384
```

*(For a hand-imported `.gguf`, replace the first line with `FROM C:\path\to\model.gguf`.)* Then:

```
ollama create qwen-owl -f Modelfile
```

This creates the **`qwen-owl`** model (16k context), to use everywhere afterwards.

> ⚠️ **GGUF-import trap under Ollama.** A Modelfile reduced to `FROM …gguf` + `num_ctx` can
> **lose the tool-calling *template***: the AI then prints its calls as text (`{ "name": … }`)
> instead of executing them. `ollama pull qwen2.5:14b` includes it out of the box; if you
> import a `.gguf`, also add the model's tool `TEMPLATE` block to the Modelfile. *(NeOwl's
> native server does not have this trap.)*

---

## Step 3 — Install the agent

Two options; pick by comfort.

### OpenCode (terminal)

OpenCode is an agent you use in the terminal. It installs with **npm**, the package manager
bundled with **Node.js**.

**Prerequisite — Node.js.** If you don't have Node.js (check with `node --version` in a
terminal): install it from **<https://nodejs.org/>** (**LTS** button, Windows `.msi`
installer; it also installs `npm`). Open **a new** terminal after installing.

**Install OpenCode**:

```
npm install -g opencode-ai
```

Check: `opencode --version`.

### Cline (in VS Code, no Node)

If you'd rather stay in the editor and **not install Node**: install the **Cline** extension
from the VS Code marketplace. ⚠️ There are look-alikes: pick the one by publisher
**saoudrizwan** (id `saoudrizwan.claude-dev`), the most-installed one. Configuration is
described below (*VS Code option*).

---

## Step 4 — Connect OpenCode to the server + the owl MCP

In your project folder, create an **`opencode.json`** file. Both servers expose the same
address (`http://127.0.0.1:11434/v1`), so the configuration is identical:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "local": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "Local model",
      "options": { "baseURL": "http://127.0.0.1:11434/v1", "apiKey": "local" },
      "models": { "owl": { "name": "NeOwl local model" } }
    }
  },
  "mcp": {
    "owl": { "type": "local", "command": ["owl", "mcp"], "enabled": true }
  }
}
```

Check the MCP is recognized: `opencode mcp list` should show **`owl ✓ connected`**.

---

## Step 5 — Run

Interactive:

```
opencode
```

…or one-shot (`--auto` auto-approves tool calls):

```
opencode run "Write an OWL program in extractor.owl that ... . Use owl_search / owl_signature to find the natives, and owl_check to validate." -m local/owl --auto
```

OpenCode executes the owl tools and drives the loop, printing every step
(search → write → validate → run).

---

## VS Code option (Cline)

Same setup (a server — native or Ollama — + the owl MCP), with **Cline** as the agent:

1. Install the **[Cline](https://cline.bot)** extension (publisher **saoudrizwan**, see Step 3).
2. Set the model: *API Provider* → **OpenAI Compatible** → *Base URL*
   `http://127.0.0.1:11434/v1` → *Model* your model (API key: any value).
3. Add the owl MCP in Cline's MCP settings: server `owl`, command `owl`, argument `mcp`.
4. **Enable tool auto-approval** (the *Auto-approve* setting).

> ⚠️ **Setting not to miss.** Without auto-approval, the agent **stops and waits for manual
> confirmation at every tool call**: the loop doesn't advance on its own. Enable it for
> autonomous operation.

---

## Speed: what to know

On **CPU only**, a 14B model in agent mode is **slow**: as the conversation accumulates tool
results, each step takes longer (often several minutes). Fine for "launch and let it run,"
painful interactively.

Three ways to go faster, with the **same** agent and the **same** `owl mcp`:
- **A cloud model**: add a provider (Anthropic, OpenAI…) with your API key and select that
  model — **fast and reliable**, whatever your machine.
- **A dedicated GPU**, via **Ollama** (option B), which uses it automatically.
- **A smaller model**, if your task allows it.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| The AI prints the tool call as text (`{"name":...}`) then stops | The server doesn't guarantee the format — **Coder** model under **Ollama** | Use **NeOwl's native server** (it forces the format), or an **Instruct** model under Ollama |
| The agent waits for confirmation at every step | Auto-approval disabled | Enable *Auto-approve* (Cline) or `--auto` (OpenCode) |
| The task fails with a context-size overflow | Window too small | `--ctx-size 16384` (native server) or `PARAMETER num_ctx 16384` (Ollama) |
| `opencode mcp list` doesn't show owl | MCP misconfigured, or `owl` not on the PATH | Check `opencode.json` and the NeOwl install |
| Endless responses | Model too big for the CPU + large context | Wait, or GPU (Ollama), or cloud model |
| The model writes RDF / `prefix` | Confusion with the *Web Ontology Language* | Force MCP usage (see the prompt below) |

---

## A starter prompt (copy-paste)

```
You write code in the NeOwl language (.owl). It is a procedural PROGRAMMING LANGUAGE,
NOT the W3C "Web Ontology Language" — never write RDF or ontology.

You do not know its syntax in advance. Proceed in this order:
1. BASICS first: absorb the language overview and above all the COMMON PITFALLS (the user
   can provide them via `owl primer`, `owl cheatsheet`, `owl gotchas`).
2. FUNCTIONS via the MCP tools: owl_search / owl_list, then owl_signature / owl_explain.
3. Write the code, VALIDATE with owl_check until ZERO errors, then run it.
Never invent a function name: when in doubt, owl_search.

My request: <...>
```

> **Tip.** For a better start, first give the model the base sheets — the output of
> `owl primer`, `owl cheatsheet` and `owl gotchas`: it starts on solid ground and makes far
> fewer syntax errors.

---

## In short

1. **A model** in `.gguf` format (single file, `Q4_K_M`) — chosen to fit your RAM.
2. **A server** to run it:
   - **`owl serve-llm --model x.gguf --ctx-size 16384`** — NeOwl's **native** server,
     recommended: one binary, and the tool-call format is guaranteed (even a Coder model works).
   - or **Ollama**, in particular to exploit a **dedicated GPU**.
3. **An agent** — **OpenCode** (terminal, via Node) or **Cline** (VS Code, no Node) — pointed
   at the server + the owl MCP, with auto-approval enabled.
4. Local = **private**; for **fast and reliable** on any machine, the same setup accepts a
   **cloud model** (an API key). The `owl` MCP works with all of them.

Questions, feedback? Join the **community** (links at the bottom of the [README](README.en.md)).
