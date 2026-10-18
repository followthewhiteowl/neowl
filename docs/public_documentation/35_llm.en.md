# Language models: `LLM`

## 1. In brief

An `LLM` loads a **language model** from a file in GGUF format, then
**generates text** from a prompt. Two gestures are enough: **load** the
model with `.Load`, **generate** with `.Generate`. The settings of a
generation call live in a separate sheet, `GenerationOptions`.

```owl
assistant is an LLM
IF assistant.Load("models/my-model.gguf") THEN
	options is a GenerationOptions
	options..Temperature = 0
	options..MaxTokens = 64

	response is a string = assistant.Generate("Once upon a time", options)
	Trace(response)
END
```

To **chat** with a conversation model rather than complete raw text,
`.Chat` takes the history of a conversation (roles + contents) and
returns a structured response — see section 7.

The model itself is not bundled: `LLM` loads a GGUF file placed next to
the program. `LLM` rounds out the machine-learning family alongside
`AIModel`, which builds and trains neural networks (guide
[`34_iamodele.md`](34_iamodele.md)): both belong to the **Artificial
intelligence** category.

---

## 2. Loading a model

`.Load(path)` reads a GGUF file and prepares inference. It returns `True`
on success, `False` otherwise — and in that case `ErrorOccurred` is set,
with no crash. A missing path, an empty one, or one pointing to an
unreadable file fails cleanly.

```owl
assistant is an LLM
ErrorReset()
ok is a boolean = assistant.Load("models/my-model.gguf")
IF NOT ok THEN
	Trace("cannot load: " + ErrorInfo())
END
```

Reloading a model on the same `LLM` replaces the previous one: the
resources of the discarded model are freed automatically.

The French form is `.Charge`.

---

## 3. Generating text

`.Generate(prompt, options)` produces text from a prompt and a
`GenerationOptions` sheet. It returns the generated text as a string.

```owl
options is a GenerationOptions
options..MaxTokens = 128

text is a string = assistant.Generate("Summarize photosynthesis.", options)
Trace(text)
```

Generation stops at the first of the two following events: the model
emits an end-of-sequence token, or the maximum number of tokens
(`..MaxTokens`) is reached.

Generating on an `LLM` that was never loaded is a recoverable failure,
not a crash: `ErrorOccurred` is set and the returned string is empty.

The French form is `.Génère`.

---

## 4. Tuning generation: `GenerationOptions`

`GenerationOptions` groups the settings of a generation call, with
sensible default values.

| Property | Role | Default |
|---|---|---|
| `..Temperature` | Controls randomness: `0` always picks the most probable token (reproducible output); the higher the value, the more varied the output. | `0.8` |
| `..TopK` | Number of most-probable candidate tokens kept before sampling. | `40` |
| `..TopP` | Cumulative probability mass threshold (nucleus sampling). | `0.95` |
| `..MaxTokens` | Maximum number of tokens generated before stopping. | `512` |
| `..Seed` | Seed of the random draw: the same seed yields reproducible output for a non-zero temperature (no effect in deterministic mode). | random |
| `..OutputConstraint` | Constrains the shape of the output (guaranteed JSON, GBNF grammar). See section 6. | `constraintFree` |

```owl
options is a GenerationOptions
options..Temperature = 0.7
options..TopK = 20
options..TopP = 0.9
options..MaxTokens = 256
options..Seed = 42
```

The French forms are `..Température`, `..TopK`, `..TopP`, `..MaxTokens`
and `..Graine`.

### Reproducible generation

A temperature of `0` disables the random draw: the model always picks
the most probable token. Two calls to `.Generate` with the same prompt
and the same model then return exactly the same text — handy to lock a
behavior down without depending on randomness.

```owl
options is a GenerationOptions
options..Temperature = 0

first is a string = assistant.Generate("Hello", options)
second is a string = assistant.Generate("Hello", options)
// first and second are identical
```

---

## 5. Following generation token by token: `..OnToken`

To react as generation unfolds — display the text as it comes, update an
interface — `GenerationOptions` accepts a **streaming callback**
`..OnToken`. The referenced procedure is called back **once per generated
token**, with two parameters: the chunk of text for the token, and a
boolean `done` that is `True` on the **last** chunk (`False` during the
stream).

```owl
PROCEDURE DisplayAsItComes(chunk is a string, done is a boolean)
	Trace(chunk)
	IF done THEN
		Trace("--- generation finished ---")
	END
END

options is a GenerationOptions
options..OnToken = DisplayAsItComes

text is a string = assistant.Generate("Tell a short story.", options)
```

The callback contract:

- **Two parameters**: `chunk` (string, the bit of text for the token) and
  `done` (boolean, `True` on the last chunk).
- **Called once per token**, synchronously during the call to `.Generate`
  (or `.Chat`).
- **End of stream**: signaled by `done = True` on the last chunk, known
  inside the callback without waiting for the function to return. If no
  token is produced (immediate end), the callback gets a single call with
  `chunk = ""` and `done = True`.
- **Return value ignored.**

The callback is referenced **by its bare name** (no parentheses) and may
be defined further down the file. Concatenating the chunks received by
`..OnToken` matches exactly the final returned text.

The French form is `..SurToken`. The same callback applies to `.Generate`
and to `.Chat` (section 7).

---

## 6. Constrained output: `..OutputConstraint`

Asking a model for JSON by phrasing it in the prompt remains fragile: the
answer may wrap the JSON in text or malform it. A single property offers a
strong guarantee instead — `..OutputConstraint` **constrains** generation
so that only the tokens keeping the output compliant are produced.

The property is a string with three modes:

| Value | Effect |
|---|---|
| `constraintFree` (default) | No constraint: free generation. |
| `constraintJSON` | **Structurally valid JSON** output, guaranteed by a built-in grammar. |
| any other string | Treated as a free **GBNF grammar** the output must conform to. |

Its French form is `..ContrainteDeSortie`; the constants carry the forms
`contrainteLibre` and `contrainteJSON`.

### Valid JSON, or a clear error — with `constraintJSON`

`..OutputConstraint = constraintJSON` applies a built-in JSON grammar:
only the tokens keeping the output structurally valid are emitted.

```owl
options is a GenerationOptions
options..Temperature      = 0
options..MaxTokens        = 128
options..OutputConstraint = constraintJSON

text is a string = assistant.Generate("Give an object with a name and an age.", options)
// text is valid JSON
doc is a JSON = StringToJSON(text)
```

A grammar constrains the **tokens** but cannot **force completion**: if
`..MaxTokens` falls in the middle of an object, the structure stays
unfinished. The truncated output is **never returned silently** —
`.Generate` then signals `ErrorOccurred` with an explicit message
(increase `..MaxTokens`) and returns the partial, inspectable text. A
complete JSON output signals no error.

### Precise schema with a GBNF grammar

To constrain the output more finely — an exact format, a closed set of
answers — `..OutputConstraint` accepts any GBNF grammar (any string other
than `constraintJSON`, recognizable by its `::=` operator). The content is
only interpreted at generation time.

```owl
options is a GenerationOptions
options..OutputConstraint = `root ::= "yes" | "no"`

answer is a string = assistant.Generate("Is the sky blue?", options)
// answer is "yes" or "no", never anything else
```

The anti-truncation guard only applies to the `constraintJSON` mode: the
completion of an arbitrary grammar cannot be checked automatically, so it
is up to the prompt and `..MaxTokens` to leave the structure room to
complete.

A malformed GBNF grammar does not cause a crash: `.Generate` signals a
recoverable failure (`ErrorOccurred` set) and returns an empty string,
exactly like a generation on an unloaded model.

The constraint has a per-token cost (filtering the vocabulary at each
step), with no observable effect on the shape of the result beyond the
guaranteed compliance.

---

## 7. Multi-turn conversation: `.Chat`

`.Generate` does **raw completion**: give it a prompt, it extends it. To
chat with an "instruct/chat" model, use `.Chat`, which distinguishes the
**roles** (system, user, assistant) and applies the model's own **dialog
template** before generating.

`.Chat(messages, options)` takes the **full history** of the conversation
— an array of `LLMMessage` — and returns an `LLMResponse`. The dialog is
**stateless**: nothing is kept between two calls, the history is passed
again each time.

```owl
messages is an array of LLMMessage

system is an LLMMessage = {Role = roleSystem, Content = "You are a concise assistant."}
messages.Add(system)

question is an LLMMessage = {Role = roleUser, Content = "Explain photosynthesis in one sentence."}
messages.Add(question)

options is a GenerationOptions
options..Temperature = 0
options..MaxTokens = 128

response is an LLMResponse = assistant.Chat(messages, options)
Trace(response..Content)
```

All the generation options (sections 4 to 6) apply identically:
`..Temperature`, `..MaxTokens`, `..OnToken`, `..OutputConstraint`, etc.

### The message: `LLMMessage`

An `LLMMessage` carries two properties:

| Property | Role |
|---|---|
| `..Role` | The role of the message author. |
| `..Content` | The text of the message. |

It is built **with the named brace literal**, in two lines: a named
instance, then an append to the array.

```owl
m is an LLMMessage = {Role = roleUser, Content = "Hello"}
messages.Add(m)
```

The role is chosen among four suggested constants, but any free string is
also accepted (custom role, evolving standard):

| Constant | Use |
|---|---|
| `roleSystem` | Framing instructions for the assistant. |
| `roleUser` | The message addressed to the assistant. |
| `roleAssistant` | A model answer, in the history. |
| `roleTool` | The result of a tool call, in the history. |

The French forms are the type `MessageLLM`, the properties `..Rôle` /
`..Contenu`, and the constants `rôleSystème`, `rôleUtilisateur`,
`rôleAssistant`, `rôleOutil`.

### The response: `LLMResponse`

`.Chat` returns a read-only `LLMResponse`:

| Property | Role |
|---|---|
| `..Content` | The assistant's response text. |
| `..Reason` | Why generation stopped: `stopNatural` (end token emitted) or `stopMaxTokens` (`..MaxTokens` cap reached, response cut off). |
| `..InputTokens` | Number of tokens consumed by the prompt. |
| `..OutputTokens` | Number of tokens generated in the response. |

The token counters help budget the context window and tell when to prune
a history that has grown too long.

```owl
response is an LLMResponse = assistant.Chat(messages, options)
IF response..Reason = stopMaxTokens THEN
	Trace("response cut off at " + response..OutputTokens + " tokens")
END
```

The French forms are the type `RéponseLLM`, the properties `..Contenu` /
`..Raison` / `..JetonsEntrée` / `..JetonsSortie`, and the constants
`stopNaturel`, `stopMaxTokens`.

### Continuing the dialog

Since the history is passed again on each call, a conversation is
extended by appending the assistant's response and then the user's next
message. Each message is built on a named instance (two lines) before
being added to the array.

```owl
echo is an LLMMessage = {Role = roleAssistant, Content = response..Content}
messages.Add(echo)

followUp is an LLMMessage = {Role = roleUser, Content = "And for a six-year-old?"}
messages.Add(followUp)

more is an LLMResponse = assistant.Chat(messages, options)
```

> Note: building an `LLMMessage` by brace literal goes through a named
> instance (`m is an LLMMessage = {...}`), then `.Add(m)`. The declared
> type of the variable is what carries the shape of the fields.

### Constrained output and truncation

`..OutputConstraint = constraintJSON` shapes `response..Content` exactly
as for `.Generate`: the output is valid JSON, or an error is signaled —
never broken JSON returned silently. When the `..MaxTokens` cap cuts a
JSON structure, `.Chat` sets `ErrorOccurred` **and** `response..Reason`
reflects `stopMaxTokens`.

### Without a loaded model

Calling `.Chat` on an `LLM` that was never loaded is a recoverable
failure: `ErrorOccurred` is set and the returned `LLMResponse` is neutral
(empty `..Content`, counters at zero), with no crash.

### Independence of calls

`.Chat` and `.Generate` on the same instance are **independent**: each
call starts from a clean context. A `.Generate` inserted between two
identical `.Chat` calls (same messages, deterministic settings) does not
change the result of the second `.Chat`.

---

## 8. Performance and footprint

Inference runs on the processor (CPU), without dedicated hardware
acceleration. The required speed and memory depend directly on the size of
the loaded model: a model of several billion parameters demands several
gigabytes of memory and generates more slowly than a small model. Sizing
the model is therefore a choice to make according to the task and the
target machine.

---

## 9. Out of current scope

These uses are **not supported yet**:

- **Training or fine-tuning a language model** — `LLM` loads and queries
  an existing model; it does not train or fine-tune it.
- **Hardware acceleration (GPU)** — generation runs on the processor
  only.
- **Formats other than GGUF** — only the GGUF format is supported for
  loading.

---

## Going further

| Topic | Guide |
|---|---|
| Building and training a neural network (`AIModel`) | [`34_iamodele.md`](34_iamodele.md) |
| Exception handling and `ErrorOccurred` | [`08_exceptions.md`](08_exceptions.md) |
| Wiring an AI agent onto the language | [`25_agents_ia.md`](25_agents_ia.md) |
