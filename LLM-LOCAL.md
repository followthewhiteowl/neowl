# Écrire du NeOwl avec une IA locale 🦉🤖

**Français** · [English](LLM-LOCAL.en.md)

Faire piloter NeOwl par une IA qui tourne **sur votre propre machine** — sans cloud, sans
clé API. L'IA s'appuie sur le serveur **MCP** de NeOwl (`owl mcp`) pour découvrir le
langage, écrire du code, le valider avec `owl_check` et l'exécuter, en boucle.

> **Conseils à jour en août 2026.** L'écosystème évolue vite ; les noms de modèles et
> d'outils changeront. La **méthode**, elle, reste valable.

> **À quoi s'attendre.** Sur une machine grand public sans carte graphique dédiée, une IA
> locale **fonctionne** mais reste **lente** en mode agent (le temps de réponse grandit avec
> la conversation). Pour des résultats **rapides et fiables**, le même montage accepte un
> **modèle cloud** (Claude, GPT) : vous branchez une clé à la place du modèle local.
> **L'agent et le MCP sont la constante ; le modèle est le curseur privé ↔ puissance.**

---

## Le point le plus important : le bon type de modèle

Pour piloter des outils (le MCP), un modèle doit **émettre ses appels dans un format précis**
que le serveur reconnaît et transforme en appel exécutable. Conséquence contre-intuitive :

> **Un modèle « Coder » ne convient PAS pour l'agentique.** Ces modèles émettent leurs
> appels d'outils dans un format que les agents ne savent pas exécuter → l'agent n'exécute
> rien et la boucle s'arrête à la première étape. Un modèle Coder reste excellent pour
> écrire du code **en une fois** (quand vous lui fournissez tout le contexte), mais pas pour
> piloter des outils.
>
> **Pour l'agentique, utilisez un modèle « Instruct » généraliste** (ex. Qwen2.5-**Instruct**,
> pas Qwen2.5-**Coder**). À qualité de code équivalente sur NeOwl, seul le généraliste sait
> piloter la boucle.

---

## De quoi avez-vous besoin ? (matériel et modèle)

La **RAM disponible** détermine quel modèle vous pouvez faire tourner (il doit tenir en
mémoire, à côté de votre système). Recommandations pour l'usage **agentique** :

| RAM disponible | Modèle conseillé | Pourquoi |
|---|---|---|
| **≤ ~16 Go** | *(local peu fiable)* → **modèle cloud** | Seul un modèle ≤ 7B tient, et à cette taille il n'arrive pas à piloter les outils de façon fiable (il décrit ce qu'il ferait au lieu d'agir). |
| **~24 – 32 Go** | **Qwen2.5-14B-Instruct** (généraliste) | Le point d'équilibre : il pilote la boucle **et** code correctement. Le choix par défaut. |
| **48 Go et plus** | **Qwen2.5-32B-Instruct** (généraliste) | Plus fiable pour piloter, meilleur code. Tout autre bon modèle *Instruct* réputé pour l'appel d'outils convient aussi. |

- **Le processeur** décide de la vitesse (sans GPU dédié, tout repose sur lui).
- **Un GPU dédié** avec assez de mémoire vidéo accélère fortement (voir plus bas). Un GPU
  intégré (Arc, Radeon de portable) partage la RAM : gain modeste.
- **Disque** : quelques Go par modèle.

> Repère RAM : un modèle 14B en quantification Q4 occupe ~10-17 Go en fonctionnement.

---

## Comment ça marche

Trois briques :

1. **Le serveur** — **Ollama**. Il charge le modèle *et* traduit ses appels d'outils en
   format exécutable.
2. **L'agent** — **OpenCode** (terminal) ou **Cline** (dans VS Code) : un agent open-source
   qui exécute les outils et enchaîne la boucle tout seul.
3. **Le pont vers NeOwl** — le serveur MCP `owl mcp`.

L'IA cherche les fonctions (`owl_search`), lit leur signature (`owl_signature`), écrit le
code, le **valide avec `owl_check`**, corrige, exécute.

> **Pourquoi le MCP est indispensable.** « OWL » est aussi le nom d'un standard du web
> sémantique (le *Web Ontology Language* du W3C). Sans le MCP pour l'ancrer, un modèle
> écrit… du RDF. Le MCP lui donne la vraie grammaire du langage NeOwl.

---

## Étape 1 — Installer Ollama et récupérer le modèle

**Ollama** est le serveur local qui fait tourner le modèle. Téléchargez-le sur
**<https://ollama.com/download>** (installateur Windows, sans droits administrateur ; il
tourne ensuite en tâche de fond). La dernière version convient.

Puis récupérez le modèle **généraliste** (voir le tableau ci-dessus). Deux méthodes :

**A. En ligne de commande (le plus simple)** — dans un terminal :

```
ollama pull qwen2.5:14b
```

**B. Depuis un navigateur (si un proxy bloque la commande, ou pour choisir à la main)** —
téléchargez le modèle en **un seul fichier `.gguf`** (format `Q4_K_M`) depuis une page qui le
propose ainsi, par exemple **<https://huggingface.co/bartowski/Qwen2.5-14B-Instruct-GGUF>**
(onglet *Files* → `Qwen2.5-14B-Instruct-Q4_K_M.gguf`, ~9 Go). Puis importez-le dans Ollama avec
un *Modelfile* (voir l'encadré suivant, en remplaçant la première ligne par
`FROM C:\chemin\vers\qwen2.5-14b-instruct-q4_k_m.gguf`).

> **Si vous tombez sur plusieurs fichiers** nommés `...-00001-of-00003.gguf`,
> `...-00002-of-00003.gguf`, etc. : ce n'est pas un choix à faire — c'est **un seul modèle
> découpé en tranches**. Le plus simple, et de loin, est de prendre un dépôt qui le fournit
> **en un seul fichier** (comme celui ci-dessus). Sinon, il faut **fusionner** les tranches en un
> seul `.gguf` avant l'import — commande `llama-gguf-split --merge première-tranche.gguf
> sortie.gguf` (fournie avec llama.cpp) — car la version actuelle d'Ollama **ne sait pas**
> réassembler les tranches toute seule.

> ⚠️ **Le piège le plus vicieux de l'import GGUF.** Un Modelfile réduit à `FROM …gguf` +
> `num_ctx` **perd le *template* d'appel d'outils** : l'IA imprime alors ses appels en texte
> (`{ "name": … }`) au lieu de les **exécuter**, et l'agent ne fait rien. La méthode A
> (`ollama pull qwen2.5:14b`) l'inclut d'office — c'est pourquoi elle est préférable. Si vous
> devez vraiment importer un GGUF, ajoutez **aussi** le bloc `TEMPLATE` d'outils du modèle au
> Modelfile.

### Agrandir la fenêtre de contexte (recommandé)

Les prompts d'un agent sont volumineux : ils contiennent la description de **tous** les
outils. Or Ollama limite le contexte à 4096 tokens par défaut, ce qui est vite dépassé et
fait échouer la tâche. Pour agrandir cette fenêtre, créez un modèle dérivé :

1. Créez un fichier texte nommé **`Modelfile`** (sans extension) contenant ces deux lignes :

   ```
   FROM qwen2.5:14b
   PARAMETER num_ctx 16384
   ```

   *La 1ère ligne signifie « pars du modèle `qwen2.5:14b` » ; la 2e fixe la fenêtre de
   contexte à 16384 tokens.*

2. Dans le terminal, placé dans le dossier de ce fichier, lancez :

   ```
   ollama create qwen-owl -f Modelfile
   ```

   Cela crée un nouveau modèle nommé **`qwen-owl`** — identique, mais avec 16k de contexte.

3. Utilisez désormais **`qwen-owl`** partout où vous choisissez le modèle.

---

## Étape 2 — Installer l'agent

Deux options ; choisissez selon votre confort.

### Option A — OpenCode (terminal)

OpenCode est un agent qui s'utilise dans le terminal. Il s'installe avec **npm**, le
gestionnaire de paquets fourni avec **Node.js**.

**Prérequis — Node.js.** Si vous n'avez pas Node.js (vérifiez avec `node --version` dans un
terminal) : installez-le depuis **<https://nodejs.org/>** (bouton **LTS**, installateur
Windows `.msi` ; il installe aussi `npm`). La version **LTS** courante convient. Ouvrez
**un nouveau** terminal après l'installation.

**Installer OpenCode** — la dernière version convient :

```
npm install -g opencode-ai
```

Vérifiez : `opencode --version`.

### Option B — Cline (dans VS Code, sans Node)

Si vous préférez rester dans l'éditeur et **ne pas installer Node** : installez l'extension
**Cline** depuis le marketplace de VS Code (rien d'autre à installer). La configuration est
décrite plus bas (*Option VS Code*).

---

## Étape 3 — Brancher OpenCode sur Ollama + le MCP owl

Dans le dossier de votre projet, créez un fichier **`opencode.json`** :

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

Vérifiez que le MCP est reconnu : `opencode mcp list` doit afficher **`owl ✓ connected`**.

---

## Étape 4 — Lancer

En mode interactif :

```
opencode
```

…ou en une commande (`--auto` approuve automatiquement les appels d'outils) :

```
opencode run "Ecris un programme OWL dans extractor.owl qui ... . Utilise owl_search / owl_signature pour trouver les natives, et owl_check pour valider." -m ollama/qwen-owl --auto
```

OpenCode exécute les outils owl et enchaîne la boucle, en affichant chaque étape
(recherche → écriture → validation → exécution).

---

## Option VS Code (Cline)

Même montage (Ollama + modèle **généraliste** + MCP owl), avec **Cline** comme agent :

1. Installez l'extension **[Cline](https://cline.bot)** depuis le marketplace VS Code. ⚠️ Il
   existe des imitations : prenez bien celle de l'éditeur **saoudrizwan** (identifiant
   `saoudrizwan.claude-dev`), la plus installée.
2. Réglez le modèle : *API Provider* → **OpenAI Compatible** → *Base URL*
   `http://127.0.0.1:11434/v1` → *Model* `qwen-owl` (clé API : une valeur quelconque).
3. Ajoutez le MCP owl dans les réglages MCP de Cline : serveur `owl`, commande `owl`,
   argument `mcp`.
4. **Activez l'auto-approbation des outils** (réglage *Auto-approve*).

> ⚠️ **Réglage à ne pas oublier.** Sans l'auto-approbation, l'agent **s'arrête et attend une
> validation manuelle à chaque appel d'outil** : la boucle n'avance pas toute seule.
> Activez-la pour un fonctionnement autonome.

> **Même règle de modèle** qu'avec OpenCode : un **généraliste-instruct**, jamais un
> **Coder** — sinon les appels d'outils partent en texte et ne sont pas exécutés.

---

## La vitesse : ce qu'il faut savoir

Sur **processeur seul**, un modèle 14B en mode agent est **lent** : à mesure que la
conversation accumule les résultats d'outils, chaque étape prend de plus en plus de temps
(souvent plusieurs minutes). C'est utilisable en « lance et laisse tourner », pénible en
interactif.

Deux façons d'obtenir du **rapide et fiable**, avec le **même** agent et le **même**
`owl mcp` :
- **Un modèle cloud** : ajoutez un provider (Anthropic, OpenAI…) avec votre clé API et
  sélectionnez ce modèle — rapide et fiable.
- **Un GPU dédié** (voir ci-dessous).

---

## Accélérer avec un GPU (optionnel)

Un GPU n'est pas obligatoire, mais une **carte dédiée** avec assez de **mémoire vidéo**
(~12 Go pour un 14B) accélère énormément ; Ollama l'utilise automatiquement s'il est
reconnu. Un **GPU intégré** (Arc, Radeon de portable) partage la RAM : gain modeste (Ollama
le désactive par défaut ; `OLLAMA_IGPU_ENABLE=1` pour l'activer).

---

## Dépannage

| Symptôme | Cause probable | Correctif |
|---|---|---|
| L'IA affiche l'appel d'outil en texte (`{"name":...}`) puis s'arrête | Le modèle émet le mauvais format — le plus souvent un modèle **Coder** | Utiliser un **généraliste-instruct** |
| Les outils ne s'exécutent jamais, même avec un bon modèle | Serveur qui ne parse pas les appels | Utiliser **Ollama** (il traduit les appels en format exécutable) |
| L'agent attend une validation à chaque étape | Auto-approbation désactivée | Activer *Auto-approve* (Cline) ou `--auto` (OpenCode) |
| La tâche échoue avec un dépassement de contexte | Fenêtre trop petite (4096 par défaut) | Créer le modèle 16k (voir Étape 1) |
| `opencode mcp list` ne montre pas owl | MCP mal déclaré ou `owl` absent du PATH | Vérifier `opencode.json` et l'installation de NeOwl |
| Réponses interminables | Modèle trop gros pour le CPU + gros contexte | Patienter, ou GPU, ou modèle cloud |
| Le modèle écrit du RDF / des `prefix` | Confusion avec le *Web Ontology Language* | Imposer l'usage du MCP owl (voir le prompt ci-dessous) |

---

## Un prompt de départ (à copier-coller)

```
Tu écris du code dans le langage NeOwl (.owl). C'est un LANGAGE DE PROGRAMMATION procédural,
PAS le "Web Ontology Language" du W3C — jamais de RDF ni d'ontologie.

Tu ne connais pas sa syntaxe a l'avance. Procède dans cet ordre :
1. LES BASES d'abord : imprègne-toi de l'apercu du langage et surtout des PIEGES COURANTS
   (l'utilisateur peut te les fournir via `owl primer`, `owl cheatsheet`, `owl gotchas`).
2. Les FONCTIONS via les outils MCP : owl_search / owl_list, puis owl_signature / owl_explain.
3. Ecris le code, VALIDE avec owl_check jusqu'a ZERO erreur, puis exécute.
N'invente jamais un nom de fonction : dans le doute, owl_search.

Ma demande : <...>
```

> **Astuce.** Pour un meilleur départ, fournissez d'abord au modèle les fiches de base —
> sortie de `owl primer`, `owl cheatsheet` et `owl gotchas` : il part sur de bonnes bases et
> commet beaucoup moins d'erreurs de syntaxe.

---

## En résumé

1. **Ollama** (`ollama pull qwen2.5:14b`, ou import d'un `.gguf` de Hugging Face) — le
   serveur qui parse les outils.
2. Un modèle **généraliste-instruct** (jamais Coder) — le seul qui pilote un agent.
3. **OpenCode** (terminal, via Node) ou **Cline** (VS Code, sans Node) — l'agent, pointé sur
   Ollama + le MCP owl, avec l'auto-approbation activée.
4. Local = **privé mais lent** sur CPU ; **cloud** (même montage + une clé API) = rapide et
   fiable. Le MCP `owl` fonctionne avec les deux.

Questions, retours ? Rejoignez la **communauté** (liens en bas du [README](README.md)).
