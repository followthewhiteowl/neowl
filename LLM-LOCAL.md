# Écrire du NeOwl avec une IA locale 🦉🤖

**Français** · [English](LLM-LOCAL.en.md)

Faire écrire du NeOwl par une IA qui tourne **sur votre propre machine** — sans cloud,
sans clé API, sans qu'une seule ligne de votre code ne sorte de l'ordinateur. L'IA
s'appuie sur le serveur **MCP** de NeOwl (`owl mcp`) pour connaître le langage
*exactement* dans la version installée, au lieu de le deviner.

> **Conseils à jour en août 2026.** Le paysage des modèles et des outils bouge très
> vite : les modèles conseillés plus bas seront à réévaluer dans quelques mois. La
> **méthode**, elle, reste valable — seuls les noms de modèles changeront.

> **À quoi s'attendre, franchement.** Sur une machine grand public (32 Go de RAM, sans
> carte graphique dédiée), une IA locale est un **assistant** efficace : elle explore le
> langage, écrit un brouillon, le valide et le corrige. Elle n'égale pas encore un modèle
> cloud pour la pleine autonomie « je décris, elle livre toute seule ». Bien réglée, elle
> fait quand même gagner un temps réel — et tout reste privé.

---

## Comment ça marche

Trois briques à assembler une seule fois :

1. **Le moteur** — [`llama.cpp`](https://github.com/ggml-org/llama.cpp), qui charge un
   modèle et l'expose en local (comme une petite API privée).
2. **Le modèle** — un fichier de quelques Go, téléchargé une fois pour toutes.
3. **Le pont vers NeOwl** — le serveur MCP `owl mcp`, qui apprend le langage à l'IA
   (fonctions natives, types, validation) sans ressource externe.

Une fois branchée, l'IA **pilote NeOwl toute seule** : elle cherche les bonnes fonctions
(`owl_search`), lit leur signature (`owl_signature`), écrit le code, le **valide avec
`owl_check`**, corrige les erreurs que le compilateur lui pointe, puis l'exécute. Le MCP
est le maillon qui transforme un brouillon approximatif en code qui compile réellement.

> **Pourquoi le MCP est indispensable.** « OWL » est aussi le nom d'un standard du web
> sémantique (le *Web Ontology Language* du W3C). Sans le MCP pour l'ancrer, un modèle
> croit connaître OWL et écrit… du RDF, ou une syntaxe inventée. Le MCP le remet sur les
> rails et lui donne la vraie grammaire.

---

## De quoi avez-vous besoin ? (le matériel)

- **La RAM décide quel modèle vous pouvez charger** — c'est le facteur n°1 (voir le
  tableau de l'étape 2). Comptez la RAM *libre*, système et applications déduits.
- **Le processeur décide de la vitesse.** Sans carte graphique dédiée, tout repose sur le
  CPU : plus il a de cœurs et de bande passante mémoire, plus l'IA répond vite.
- **Un GPU n'est pas obligatoire** — tout fonctionne sur le processeur. Mais si vous avez
  une **carte graphique dédiée** avec assez de mémoire vidéo (VRAM), l'accélération est
  spectaculaire : voir *Accélérer avec un GPU* plus bas. Un **GPU intégré** (Intel Arc,
  Radeon de portable) partage la RAM système : gain plus modeste.
- **Disque** : quelques Go par modèle.

En clair : une machine de développeur récente avec **32 Go de RAM** est le point d'entrée
confortable. En dessous de ~24 Go, l'IA locale reste possible mais frustrante.

---

## Étape 1 — Installer le moteur

Téléchargez llama.cpp pour Windows depuis sa
[page des releases](https://github.com/ggml-org/llama.cpp/releases), variante
**`llama-<version>-bin-win-cpu-x64.zip`** (processeur seul — le choix sûr, il marche
partout). Décompressez-la dans un dossier, par exemple `C:\ai\llama\`.

> **Version testée : `b10405`.** Toutes nos mesures ci-dessous ont été faites avec ce
> build : [lien direct](https://github.com/ggml-org/llama.cpp/releases/tag/b10405),
> fichier `llama-b10405-bin-win-cpu-x64.zip`. Une version plus récente marche aussi ; si
> l'appel d'outils déraille, revenez à celle-ci.

> **GPU ?** Gardez cette variante `cpu` pour valider que tout marche. Si vous avez une
> carte graphique dédiée, passez ensuite à *Accélérer avec un GPU* (plus bas) pour une
> tout autre vitesse.

---

## Étape 2 — Choisir et télécharger un modèle

Un modèle est un gros fichier `.gguf`. **Votre RAM commande le choix** — le modèle doit
tenir en mémoire à côté de votre système.

| Votre RAM | Modèle conseillé | En un mot |
|---|---|---|
| **≤ 16 Go** | *(aucun fiable)* | Trop juste : seuls de très petits modèles tiennent, et ils **inventent**. Préférez un modèle cloud, ou ajoutez de la RAM. |
| **24 – 32 Go** | **Qwen2.5-Coder-14B** (Q4) | Le meilleur compromis : code le plus propre, **pilote le MCP tout seul**. Le choix par défaut. |
| **32 Go, priorité vitesse** | DeepSeek-Coder-V2-Lite (Q4) | ~5× plus rapide, mais moins autonome (à assister, voir plus bas). |
| **48 Go et plus** | Qwen3-Coder-30B-A3B (Q4) | Rapide *et* de qualité, sans contrainte mémoire. |

**Ce qu'on a mesuré** (août 2026, machine 32 Go, processeur seul) :

| Modèle testé | RAM utilisée | Vitesse | Erreurs au 1er essai\* | Pilote le MCP seul ? |
|---|---|---|---|---|
| Qwen2.5-Coder-**7B** | ~6 Go | rapide | *invente tout* | — |
| **Qwen2.5-Coder-14B** | ~17 Go | 5,4 t/s | **2** | ✅ oui |
| DeepSeek-Coder-V2-Lite | ~16 Go | **28 t/s** | 8 | ❌ non |
| Qwen3-Coder-30B-A3B | ~22 Go | 8 t/s | 8 | ✅ mais ne tient pas sur 32 Go |
| Devstral-Small-24B | ~22 Go | 3,3 t/s | 10 | non concluant |

\* Erreurs de syntaxe au premier jet, que le MCP (`owl_check`) aide ensuite à corriger.
`t/s` = *tokens* par seconde ; à titre de repère, ~4 t/s ≈ la vitesse de lecture d'un
humain.

Deux enseignements : la **vitesse** vient de l'architecture « MoE » (DeepSeek, 30B) plus
que de la taille ; et **plus gros ≠ meilleur code** (le 14B fait moins d'erreurs que le
30B). Le 14B est le seul qui, sur 32 Go, code proprement *et* pilote le MCP tout seul.

Récupérez le fichier `.gguf` (format `Q4_K_M`, bon compromis taille/qualité) depuis
[Hugging Face](https://huggingface.co). Placez-le par exemple dans `C:\ai\modeles\`.

> **Vérifiez la taille du fichier téléchargé** (elle est indiquée sur la page du modèle).
> Un fichier interrompu ou trop gros = modèle corrompu, qui se charge mais génère du
> charabia.

---

## Étape 3 — Lancer le modèle

Dans un terminal :

```
C:\ai\llama\llama-server.exe -m C:\ai\modeles\Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf --jinja -c 16384 --host 127.0.0.1 --port 8080 -ngl 0 -t 8
```

Les deux réglages qui comptent vraiment :

- **`--jinja`** — *indispensable*. Sans lui, l'IA « pense » appeler les outils mais rien
  ne s'exécute : elle tourne en boucle sans jamais agir.
- **`-ngl 0`** — tout en mémoire vive (processeur). Mettre des couches sur un GPU intégré
  à petite mémoire provoque une erreur `out of device memory`.

`-c 16384` fixe la taille de contexte, `-t 8` le nombre de cœurs. Laissez le terminal
ouvert : le modèle tourne tant qu'il est là. Pour vérifier, ouvrez
<http://127.0.0.1:8080> dans un navigateur.

---

## Accélérer avec un GPU (optionnel)

Un GPU n'est pas nécessaire, mais si vous avez une **carte graphique dédiée** avec assez
de **mémoire vidéo (VRAM)**, elle rend l'IA plusieurs fois plus rapide. Repère utile : à
partir de ~**8 Go de VRAM** ça vaut le coup ; comptez ~**12 Go** pour charger entièrement
le 14B conseillé.

1. Prenez la bonne variante de llama.cpp (étape 1) : **`...-cuda-...`** pour une carte
   **NVIDIA** (le plus rapide), **`...-vulkan-...`** pour **AMD ou Intel** (universel).
2. Ajoutez **`-ngl 999`** à la commande de l'étape 3 pour envoyer toutes les couches du
   modèle sur le GPU :

```
llama-server.exe -m ...\Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf --jinja -c 16384 --host 127.0.0.1 --port 8080 -ngl 999
```

Si le modèle ne tient pas entièrement dans la VRAM (erreur `out of device memory`),
baissez le nombre : `-ngl 20`, `-ngl 10`… jusqu'à ce qu'il démarre. Les couches restantes
tournent alors sur le processeur (mode mixte, déjà plus rapide que tout-CPU).

> **GPU intégré (Arc / Radeon de portable).** Utilisez la variante `vulkan`, mais
> attendez-vous à un gain modeste : l'iGPU partage la RAM et n'a pas de VRAM rapide dédiée.
> Si l'option existe, augmentez d'abord la mémoire allouée au GPU dans le BIOS.

---

## Étape 4 — Brancher l'IA sur NeOwl

### Avec Visual Studio Code (le plus simple)

1. Installez une extension d'agent qui accepte un modèle local *et* le MCP — par exemple
   **Cline**, **Continue** ou **Roo Code**.
2. Dans ses réglages, pointez le modèle sur votre serveur local, endpoint
   **`http://127.0.0.1:8080/v1`** (API compatible OpenAI ; le nom du modèle importe peu).
3. Déclarez le serveur MCP de NeOwl — fichier `.vscode/mcp.json` à la racine du projet :

```json
{
  "servers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

4. Passez l'extension en **mode Agent**, puis **activez les outils `owl_*`** dans son
   sélecteur d'outils. Vérifiez dans ses logs un message du type `Discovered N tools`.

> **Le piège n°1 des débutants.** Si l'IA fait une recherche dans vos fichiers au lieu
> d'interroger le MCP, c'est que les outils `owl_*` ne lui sont pas transmis : vérifiez
> le **mode Agent** et l'activation des outils. Sans ça, elle devine — et se trompe.

### Sans Visual Studio Code

Deux options :

- **Un agent de bureau compatible MCP** (Claude Desktop, Cursor, Windsurf…) : même
  configuration MCP que dans la section *Brancher un agent IA* du README, avec le modèle
  pointé sur votre serveur local.
- **En manuel, avec le CLI** : ouvrez un chat local (LM Studio, Jan…) et servez-vous
  vous-même des commandes de documentation de NeOwl pour guider et vérifier l'IA :
  `owl search <mot>`, `owl explain <fonction>`, `owl --check fichier.owl`. C'est plus
  artisanal, mais ça marche sans rien installer de plus.

---

## Un prompt de départ (à copier-coller)

Peu importe ce que vous construisez, donnez ce **prompt système** à votre agent : il
l'empêche de dériver (RDF, syntaxe inventée) et l'oblige à passer par le MCP. Adaptez
seulement la toute dernière ligne à votre besoin.

```
Tu écris du code dans le langage NeOwl (fichiers .owl).

IMPORTANT : NeOwl est un LANGAGE DE PROGRAMMATION procédural (variables typées, boucles,
fonctions natives). Ce n'est PAS le "Web Ontology Language" du W3C — n'écris jamais de
RDF, de triplets ni de préfixes d'ontologie.

Tu ne connais PAS la syntaxe de NeOwl à l'avance. Procède dans cet ordre :

1. LES BASES D'ABORD. Assimile les fondamentaux AVANT de coder : l'aperçu du langage, le
   contrôle de flux, les conventions, et surtout les PIÈGES COURANTS. (Ces fiches te sont
   fournies ci-dessous par l'utilisateur, ou lisibles via les ressources
   owl://public_doc/ — dont 22_pieges_courants — si ton client les expose.)
2. LES FONCTIONS ENSUITE, via les outils MCP owl_ :
   - owl_search <mot> et owl_list : trouver les natives et types utiles ;
   - owl_signature / owl_explain : vérifier la forme exacte d'une fonction ;
   - owl_examples : voir un usage réel.
3. ÉCRIS le code.
4. VALIDE avec owl_check et corrige jusqu'à ZÉRO erreur avant de répondre.
5. Si tu peux, EXÉCUTE le programme et vérifie le résultat.

N'invente JAMAIS un nom de fonction ni une tournure de syntaxe : dans le doute, appuie-toi
sur les fiches de base ou appelle owl_search.

Ma demande : <décris ici ce que tu veux construire>
```

> **Astuce — donnez-lui les bases dès le départ.** Pour une meilleure entrée en matière,
> collez au début de la conversation les fiches fondamentales : `owl primer` (l'essentiel
> en 5 min), `owl cheatsheet` (la syntaxe) et `owl gotchas` (les pièges qui ne se devinent
> pas). En prime, les signatures des fonctions clés (via `owl explain <fonction>`). Le
> modèle part alors sur de bonnes fondations et devine beaucoup moins — c'est le mode
> « assisté », le plus fiable en local.

---

## Bien s'en servir : nos conseils

- **Donnez-lui un coup de pouce.** Sur une machine 32 Go, la pleine autonomie
  « je décris, elle livre » n'est pas encore fiable. Le mode qui marche vraiment :
  laissez-la découvrir le langage via le MCP, **et** rappelez-lui d'utiliser `owl_search`
  / `owl_signature` **avant** d'écrire, et `owl_check` **après** (voir le prompt ci-dessus).
- **Exigez la validation.** Une consigne efficace se termine par : « valide toujours ton
  code avec `owl_check` et corrige jusqu'à zéro erreur avant de me répondre ».
- **Un seul programme par dossier de travail.** NeOwl charge le « projet voisin » : deux
  fichiers `.owl` côte à côte peuvent mélanger leurs erreurs.
- **Ne lancez pas de gros téléchargement pendant que l'IA travaille** — la vitesse peut
  chuter de moitié (l'inférence et le téléchargement se disputent la mémoire).
- **Petit modèle = béquilles ; bon modèle = autonomie.** Si un modèle invente des noms de
  fonctions, ce n'est pas votre faute : il est trop petit. Montez en gamme (voir le
  tableau) plutôt que de vous épuiser à corriger.

---

## En résumé

1. `llama.cpp` (variante `cpu`, build testé `b10405`) + un modèle `.gguf` adapté à votre RAM.
2. `llama-server ... --jinja -ngl 0` — le `--jinja` est vital.
3. Une extension VS Code (Cline / Continue) pointée sur `http://127.0.0.1:8080/v1`, **plus**
   le MCP `owl` en mode Agent, avec le prompt de départ ci-dessus.
4. Sur 32 Go : **Qwen2.5-Coder-14B** par défaut. Assistez le modèle (MCP + `owl_check`),
   ne visez pas la pleine autonomie tant que vous n'avez pas une grosse machine ou un
   modèle cloud.

Des questions, un retour ? Rejoignez la **communauté** (liens en bas du [README](README.md)).
