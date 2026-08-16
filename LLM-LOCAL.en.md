# Writing NeOwl with a local AI 🦉🤖

[Français](LLM-LOCAL.md) · **English**

Have NeOwl driven by an AI running **on your own machine** — no cloud, no API key. The AI
relies on NeOwl's **MCP** server (`owl mcp`) to discover the language, write code, validate
it with `owl_check` and run it, in a loop.

> **Advice current as of August 2026.** The ecosystem moves fast; model and tool names will
> change. The **method** stays valid.

> **What to expect.** On a consumer machine without a dedicated GPU, a local AI **works** but
> is **slow** in agent mode (response time grows with the conversation). For **fast and
> reliable** results, the same setup accepts a **cloud model** (Claude, GPT): you plug in a
> key instead of the local model. **The agent and the MCP are the constant; the model is the
> privacy ↔ power dial.**

---

## The most important point: the right kind of model

To drive tools (the MCP), a model must **emit its calls in a precise format** that the
server recognizes and turns into an executable call. Counterintuitive consequence:

> **A "Coder" model is NOT suitable for agentic use.** These models emit their tool calls in
> a format agents can't execute → the agent executes nothing and the loop stops at step one.
> A Coder model is still great at writing code **one-shot** (when you give it all the
> context), but not for driving tools.
>
> **For agentic use, use a general "Instruct" model** (e.g. Qwen2.5-**Instruct**, not
> Qwen2.5-**Coder**). At equivalent OWL code quality, only the general model can drive the
> loop.

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
- **A dedicated GPU** with enough video memory speeds things up a lot (see below). An
  integrated GPU (Arc, laptop Radeon) shares your RAM: modest gain.
- **Disk**: a few GB per model.

> RAM guide: a 14B model in Q4 quantization uses ~10-17 GB when running.

---

## How it works

Three pieces:

1. **The server** — **Ollama**. It loads the model *and* translates its tool calls into an
   executable format.
2. **The agent** — **OpenCode** (terminal) or **Cline** (in VS Code): an open-source agent
   that executes tools and drives the loop on its own.
3. **The bridge to NeOwl** — the `owl mcp` server.

The AI searches for functions (`owl_search`), reads their signature (`owl_signature`),
writes the code, **validates with `owl_check`**, fixes, runs.

> **Why the MCP is essential.** "OWL" is also the name of a W3C semantic-web standard (the
> *Web Ontology Language*). Without the MCP to anchor it, a model writes… RDF. The MCP gives
> it the real grammar of the NeOwl language.

---

## Step 1 — Install Ollama and get the model

**Ollama** is the local server that runs the model. Download it from
**<https://ollama.com/download>** (Windows installer, no admin rights; it then runs in the
background). The latest version is fine.

Then get the **general** model (see the table above). Two methods:

**A. Command line (simplest)** — in a terminal:

```
ollama pull qwen2.5:14b
```

**B. From a browser (if a proxy blocks the command, or to pick it manually)** — download the
model as a **single `.gguf` file** (`Q4_K_M` format) from a page that offers it that way, e.g.
**<https://huggingface.co/bartowski/Qwen2.5-14B-Instruct-GGUF>** (*Files* tab →
`Qwen2.5-14B-Instruct-Q4_K_M.gguf`, ~9 GB). Then import it into Ollama with a *Modelfile*
(see the next box, replacing the first line with
`FROM C:\path\to\qwen2.5-14b-instruct-q4_k_m.gguf`).

> **If you run into several files** named `...-00001-of-00003.gguf`, `...-00002-of-00003.gguf`,
> etc.: that's not a choice to make — it's **one model split into parts**. Simplest is to grab a
> repo that provides it as **one file** (like the one above). Otherwise download **all** the
> parts into the same folder and point `FROM` at the **first** one (`...-00001-of-...`): Ollama
> reassembles the rest automatically.

### Enlarge the context window (recommended)

An agent's prompts are large: they contain the description of **all** the tools. But Ollama
limits context to 4096 tokens by default, which is quickly exceeded and makes the task fail.
To enlarge that window, create a derived model:

1. Create a text file named **`Modelfile`** (no extension) containing these two lines:

   ```
   FROM qwen2.5:14b
   PARAMETER num_ctx 16384
   ```

   *Line 1 means "start from the `qwen2.5:14b` model"; line 2 sets the context window to
   16384 tokens.*

2. In the terminal, from that file's folder, run:

   ```
   ollama create qwen-owl -f Modelfile
   ```

   This creates a new model named **`qwen-owl`** — identical, but with a 16k context.

3. From now on, use **`qwen-owl`** wherever you select the model.

---

## Step 2 — Install the agent

Two options; pick by comfort.

### Option A — OpenCode (terminal)

OpenCode is an agent you use in the terminal. It installs with **npm**, the package manager
bundled with **Node.js**.

**Prerequisite — Node.js.** If you don't have Node.js (check with `node --version` in a
terminal): install it from **<https://nodejs.org/>** (**LTS** button, Windows `.msi`
installer; it also installs `npm`). The current **LTS** version is fine. Open **a new**
terminal after installing.

**Install OpenCode** — the latest version is fine:

```
npm install -g opencode-ai
```

Check: `opencode --version`.

### Option B — Cline (in VS Code, no Node)

If you'd rather stay in the editor and **not install Node**: install the **Cline** extension
from the VS Code marketplace (nothing else to install). Configuration is described below
(*VS Code option*).

---

## Step 3 — Connect OpenCode to Ollama + the owl MCP

In your project folder, create an **`opencode.json`** file:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "ollama": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "Ollama",
      "options": { "baseURL": "http://127.0.0.1:11434/v1", "apiKey": "ollama" },
      "models": { "qwen-owl": { "name": "Qwen2.5 14B (owl)" } }
    }
  },
  "mcp": {
    "owl": { "type": "local", "command": ["owl", "mcp"], "enabled": true }
  }
}
```

Check the MCP is recognized: `opencode mcp list` should show **`owl ✓ connected`**.

---

## Step 4 — Run

Interactive:

```
opencode
```

…or one-shot (`--auto` auto-approves tool calls):

```
opencode run "Write an OWL program in extractor.owl that ... . Use owl_search / owl_signature to find the natives, and owl_check to validate." -m ollama/qwen-owl --auto
```

OpenCode executes the owl tools and drives the loop, printing every step
(search → write → validate → run).

---

## VS Code option (Cline)

Same setup (Ollama + a **general** model + the owl MCP), with **Cline** as the agent:

1. Install the **[Cline](https://cline.bot)** extension from the VS Code marketplace.
2. Set the model: *API Provider* → **OpenAI Compatible** → *Base URL*
   `http://127.0.0.1:11434/v1` → *Model* `qwen-owl` (API key: any value).
3. Add the owl MCP in Cline's MCP settings: server `owl`, command `owl`, argument `mcp`.
4. **Enable tool auto-approval** (the *Auto-approve* setting).

> ⚠️ **Setting not to miss.** Without auto-approval, the agent **stops and waits for manual
> confirmation at every tool call**: the loop doesn't advance on its own. Enable it for
> autonomous operation.

> **Same model rule** as with OpenCode: a **general instruct** model, never a **Coder** —
> otherwise the tool calls come out as text and are not executed.

---

## Speed: what to know

On **CPU only**, a 14B model in agent mode is **slow**: as the conversation accumulates tool
results, each step takes longer (often several minutes). Fine for "launch and let it run,"
painful interactively.

Two ways to get **fast and reliable**, with the **same** agent and the **same** `owl mcp`:
- **A cloud model**: add a provider (Anthropic, OpenAI…) with your API key and select that
  model — fast and reliable.
- **A dedicated GPU** (see below).

---

## Speeding up with a GPU (optional)

A GPU isn't required, but a **dedicated card** with enough **video memory** (~12 GB for a
14B) speeds things up a lot; Ollama uses it automatically if recognized. An **integrated
GPU** (Arc, laptop Radeon) shares your RAM: modest gain (Ollama disables it by default;
`OLLAMA_IGPU_ENABLE=1` to enable it).

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| The AI prints the tool call as text (`{"name":...}`) then stops | The model emits the wrong format — usually a **Coder** model | Use a **general instruct** model |
| Tools never execute, even with a good model | Server that doesn't parse the calls | Use **Ollama** (it translates calls into an executable format) |
| The agent waits for confirmation at every step | Auto-approval disabled | Enable *Auto-approve* (Cline) or `--auto` (OpenCode) |
| The task fails with a context-size overflow | Window too small (4096 by default) | Create the 16k model (see Step 1) |
| `opencode mcp list` doesn't show owl | MCP misconfigured, or `owl` not on the PATH | Check `opencode.json` and the NeOwl install |
| Endless responses | Model too big for the CPU + large context | Wait, or GPU, or cloud model |
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

1. **Ollama** (`ollama pull qwen2.5:14b`, or import a `.gguf` from Hugging Face) — the server
   that parses tools.
2. A **general instruct** model (never Coder) — the only kind that drives an agent.
3. **OpenCode** (terminal, via Node) or **Cline** (VS Code, no Node) — the agent, pointed at
   Ollama + the owl MCP, with auto-approval enabled.
4. Local = **private but slow** on CPU; **cloud** (same setup + an API key) = fast and
   reliable. The `owl` MCP works with both.

Questions, feedback? Join the **community** (links at the bottom of the [README](README.en.md)).
