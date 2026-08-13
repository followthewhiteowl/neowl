# Writing NeOwl with a local AI 🦉🤖

[Français](LLM-LOCAL.md) · **English**

Have NeOwl code written by an AI running **on your own machine** — no cloud, no API key,
without a single line of your code leaving the computer. The AI relies on NeOwl's **MCP**
server (`owl mcp`) to know the language *exactly* as installed, instead of guessing.

> **Advice current as of August 2026.** The model and tooling landscape moves fast: the
> models recommended below will need revisiting in a few months. The **method** stays
> valid — only the model names will change.

> **What to honestly expect.** On a consumer machine (32 GB of RAM, no dedicated GPU), a
> local AI is a capable **assistant**: it explores the language, writes a draft, validates
> and fixes it. It doesn't yet match a cloud model for full "I describe it, it ships on its
> own" autonomy. Tuned well, it still saves real time — and everything stays private.

---

## How it works

Three pieces, assembled once:

1. **The engine** — [`llama.cpp`](https://github.com/ggml-org/llama.cpp), which loads a
   model and serves it locally (like a small private API).
2. **The model** — a file of a few GB, downloaded once and for all.
3. **The bridge to NeOwl** — the `owl mcp` server, which teaches the AI the language
   (native functions, types, validation) with no external resource.

Once connected, the AI **drives NeOwl on its own**: it searches for the right functions
(`owl_search`), reads their signature (`owl_signature`), writes the code, **validates it
with `owl_check`**, fixes the errors the compiler points out, then runs it. The MCP is the
link that turns a rough draft into code that actually compiles.

> **Why the MCP is essential.** "OWL" is also the name of a semantic-web standard (the
> W3C *Web Ontology Language*). Without the MCP to anchor it, a model believes it knows OWL
> and writes… RDF, or invented syntax. The MCP puts it back on track with the real grammar.

---

## What do you need? (hardware)

- **RAM decides which model you can load** — this is factor number one (see the table in
  step 2). Count *free* RAM, once the system and apps are accounted for.
- **The CPU decides the speed.** With no dedicated graphics card, everything runs on the
  CPU: the more cores and memory bandwidth it has, the faster the AI replies.
- **A GPU is not required** — everything runs on the CPU. But if you have a **dedicated
  graphics card** with enough video memory (VRAM), the speedup is dramatic: see *Speeding
  up with a GPU* below. An **integrated GPU** (Intel Arc, laptop Radeon) shares your
  system RAM: a more modest gain.
- **Disk**: a few GB per model.

Bottom line: a recent developer machine with **32 GB of RAM** is the comfortable entry
point. Below ~24 GB, a local AI is still possible but frustrating.

---

## Step 1 — Install the engine

Download llama.cpp for Windows from its
[releases page](https://github.com/ggml-org/llama.cpp/releases), the
**`llama-<version>-bin-win-cpu-x64.zip`** variant (CPU only — the safe choice, it works
everywhere). Unzip it into a folder, for example `C:\ai\llama\`.

> **Version we tested: `b10405`.** All the figures below were measured with this build:
> [direct link](https://github.com/ggml-org/llama.cpp/releases/tag/b10405), file
> `llama-b10405-bin-win-cpu-x64.zip`. A newer version works too; if tool-calling misbehaves,
> fall back to this one.

> **GPU?** Keep this `cpu` variant to confirm everything works. If you have a dedicated
> graphics card, then move to *Speeding up with a GPU* (below) for a whole other speed.

---

## Step 2 — Choose and download a model

A model is a large `.gguf` file. **Your RAM drives the choice** — the model must fit in
memory alongside your system.

| Your RAM | Recommended model | In a word |
|---|---|---|
| **≤ 16 GB** | *(none reliable)* | Too tight: only very small models fit, and they **make things up**. Prefer a cloud model, or add RAM. |
| **24 – 32 GB** | **Qwen2.5-Coder-14B** (Q4) | Best all-rounder: cleanest code, **drives the MCP by itself**. The default pick. |
| **32 GB, speed first** | DeepSeek-Coder-V2-Lite (Q4) | ~5× faster, but less autonomous (needs assisting, see below). |
| **48 GB and up** | Qwen3-Coder-30B-A3B (Q4) | Fast *and* high quality, with no memory pressure. |

**What we measured** (August 2026, 32 GB machine, CPU only):

| Model tested | RAM used | Speed | Errors on first try\* | Drives the MCP alone? |
|---|---|---|---|---|
| Qwen2.5-Coder-**7B** | ~6 GB | fast | *makes it all up* | — |
| **Qwen2.5-Coder-14B** | ~17 GB | 5.4 t/s | **2** | ✅ yes |
| DeepSeek-Coder-V2-Lite | ~16 GB | **28 t/s** | 8 | ❌ no |
| Qwen3-Coder-30B-A3B | ~22 GB | 8 t/s | 8 | ✅ but won't fit in 32 GB |
| Devstral-Small-24B | ~22 GB | 3.3 t/s | 10 | inconclusive |

\* Syntax errors on the first draft, which the MCP (`owl_check`) then helps fix. `t/s` =
*tokens* per second; for reference, ~4 t/s ≈ a human's reading speed.

Two takeaways: **speed** comes from the "MoE" architecture (DeepSeek, 30B) more than from
size; and **bigger ≠ better code** (the 14B makes fewer errors than the 30B). The 14B is
the only one that, on 32 GB, writes clean code *and* drives the MCP on its own.

Grab the `.gguf` file (`Q4_K_M` format, a good size/quality trade-off) from
[Hugging Face](https://huggingface.co). Put it in, say, `C:\ai\models\`.

> **Check the downloaded file's size** (shown on the model's page). An interrupted or
> oversized file means a corrupt model — it loads but generates gibberish.

---

## Step 3 — Launch the model

In a terminal:

```
C:\ai\llama\llama-server.exe -m C:\ai\models\Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf --jinja -c 16384 --host 127.0.0.1 --port 8080 -ngl 0 -t 8
```

The two settings that truly matter:

- **`--jinja`** — *essential*. Without it, the AI "thinks" about calling tools but nothing
  runs: it loops forever without ever acting.
- **`-ngl 0`** — everything in RAM (CPU). Putting layers on a small integrated GPU causes
  an `out of device memory` error.

`-c 16384` sets the context size, `-t 8` the number of cores. Keep the terminal open: the
model runs as long as it's there. To check, open <http://127.0.0.1:8080> in a browser.

---

## Speeding up with a GPU (optional)

A GPU isn't required, but if you have a **dedicated graphics card** with enough **video
memory (VRAM)**, it makes the AI several times faster. Rule of thumb: from ~**8 GB of
VRAM** it's worth it; count on ~**12 GB** to fully load the recommended 14B.

1. Get the right llama.cpp variant (step 1): **`...-cuda-...`** for an **NVIDIA** card
   (fastest), **`...-vulkan-...`** for **AMD or Intel** (universal).
2. Add **`-ngl 999`** to the step 3 command to push all the model's layers onto the GPU:

```
llama-server.exe -m ...\Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf --jinja -c 16384 --host 127.0.0.1 --port 8080 -ngl 999
```

If the model doesn't fully fit in VRAM (`out of device memory` error), lower the number:
`-ngl 20`, `-ngl 10`… until it starts. The remaining layers then run on the CPU (mixed
mode, already faster than all-CPU).

> **Integrated GPU (Arc / laptop Radeon).** Use the `vulkan` variant, but expect a modest
> gain: the iGPU shares your RAM and has no fast dedicated VRAM. If the option exists,
> first raise the memory allocated to the GPU in the BIOS.

---

## Step 4 — Connect the AI to NeOwl

### With Visual Studio Code (the easiest)

1. Install an agent extension that accepts a local model *and* MCP — for example
   **Cline**, **Continue** or **Roo Code**.
2. In its settings, point the model at your local server, endpoint
   **`http://127.0.0.1:8080/v1`** (OpenAI-compatible API; the model name doesn't matter).
3. Declare NeOwl's MCP server — a `.vscode/mcp.json` file at the project root:

```json
{
  "servers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

4. Switch the extension to **Agent mode**, then **enable the `owl_*` tools** in its tool
   picker. Check its logs for a message like `Discovered N tools`.

> **The #1 beginner trap.** If the AI searches your files instead of querying the MCP, its
> `owl_*` tools aren't being passed to it: check **Agent mode** and that the tools are
> enabled. Otherwise it guesses — and gets it wrong.

### Without Visual Studio Code

Two options:

- **A desktop MCP-capable agent** (Claude Desktop, Cursor, Windsurf…): same MCP
  configuration as in the *Connecting an AI agent* section of the README, with the model
  pointed at your local server.
- **Manually, with the CLI**: open a local chat (LM Studio, Jan…) and use NeOwl's own
  documentation commands to guide and check the AI yourself: `owl search <word>`,
  `owl explain <function>`, `owl --check file.owl`. More hands-on, but it works with
  nothing extra to install.

---

## A starter prompt (copy-paste)

Whatever you're building, give your agent this **system prompt**: it keeps it from drifting
(RDF, invented syntax) and forces it through the MCP. Only adapt the very last line to your
need.

```
You write code in the NeOwl language (.owl files).

IMPORTANT: NeOwl is a procedural PROGRAMMING LANGUAGE (typed variables, loops, native
functions). It is NOT the W3C "Web Ontology Language" — never write RDF, triples or
ontology prefixes.

You do NOT know NeOwl's syntax in advance. Proceed in this order:

1. BASICS FIRST. Absorb the fundamentals BEFORE coding: the language overview, control
   flow, the conventions, and above all the COMMON PITFALLS. (These sheets are provided
   below by the user, or readable via the owl://public_doc/ resources — including
   22_pieges_courants — if your client exposes them.)
2. FUNCTIONS NEXT, via the owl_ MCP tools:
   - owl_search <word> and owl_list: find the useful natives and types;
   - owl_signature / owl_explain: check a function's exact form;
   - owl_examples: see real usage.
3. WRITE the code.
4. VALIDATE with owl_check and fix until ZERO errors before answering.
5. If you can, RUN the program and check the result.

NEVER invent a function name or a syntax form: when in doubt, lean on the basics sheets or
call owl_search.

My request: <describe here what you want to build>
```

> **Tip — give it the basics up front.** For a better start, paste the fundamental sheets
> at the beginning of the conversation: `owl primer` (the 5-minute essentials),
> `owl cheatsheet` (the syntax) and `owl gotchas` (the pitfalls you can't guess). As a
> bonus, the key function signatures (via `owl explain <function>`). The model then starts
> on solid ground and guesses far less — that's the "assisted" mode, the most reliable one
> locally.

---

## Getting the best out of it: our tips

- **Give it a head start.** On a 32 GB machine, full "describe it and it ships" autonomy
  isn't reliable yet. What actually works: let it discover the language through the MCP,
  **and** remind it to use `owl_search` / `owl_signature` **before** writing, and
  `owl_check` **after** (see the prompt above).
- **Demand validation.** An effective prompt ends with: "always validate your code with
  `owl_check` and fix it until zero errors before answering."
- **One program per working folder.** NeOwl loads the "neighbouring project": two `.owl`
  files side by side can mix up their errors.
- **Don't start a big download while the AI works** — speed can drop by half (inference and
  the download compete for memory).
- **Small model = crutches; good model = autonomy.** If a model invents function names,
  it's not your fault: it's too small. Move up a tier (see the table) rather than wearing
  yourself out fixing it.

---

## In short

1. `llama.cpp` (`cpu` variant, tested build `b10405`) + a `.gguf` model that fits your RAM.
2. `llama-server ... --jinja -ngl 0` — the `--jinja` is vital.
3. A VS Code extension (Cline / Continue) pointed at `http://127.0.0.1:8080/v1`, **plus**
   the `owl` MCP in Agent mode, with the starter prompt above.
4. On 32 GB: **Qwen2.5-Coder-14B** by default. Assist the model (MCP + `owl_check`); don't
   aim for full autonomy until you have a big machine or a cloud model.

Questions, feedback? Join the **community** (links at the bottom of the [README](README.en.md)).
