# NeOwl 🦉

**Français** · [English](README.en.md)

**Un langage de programmation orienté productivité, all inclusive.**

L'atout de NeOwl tient autant à son langage qu'à son **framework intégré**. Des
centaines de fonctions natives sont embarquées dans le compilateur — HTTP,
JSON, SQL, dates, expressions régulières, fichiers, interfaces graphiques, PDF,
tests, et bien d'autres — sans aucune dépendance externe à installer ou faire cohabiter. L'objectif : écrire son code, livrer de la valeur pas assembler sa plomberie.

Le langage est fortement typé, explicite et prévisible : un programme se lit et
se comprend sans deviner ni recourir systématiquement à un débogueur. 
Cette clarté le rend lisible autant par un humain que par un agent IA.

> **Version alpha.** Le langage évolue encore. Les signalements de bugs, idées
> et retours sont précieux à ce stade (voir la section Communauté).

---

## Installation

**Prérequis :** Windows 10 ou plus récent (64 bits).

**Fortement recommandé :** Visual Studio Code, pour profiter de l'extension OWL
(coloration syntaxique, autocomplétion, diagnostics en direct, designers visuels).

1. Télécharger la dernière version depuis la
   [page des releases](https://github.com/followthewhiteowl/neowl/releases/latest) (Windows).
2. L'exécuter — installation par utilisateur, sans droits administrateur.
3. Ouvrir un nouveau terminal et lancer `owl help` pour vérifier qu'il se lance bien.

L'installateur place le compilateur, l'ajoute au `PATH` et, en option, installe
l'extension Visual Studio Code. Un désinstalleur est disponible dans la liste
des applications installées.

---

## Démarrage rapide

Dans un **nouveau** terminal, après l'installation :

```
owl template hello --output hello.owl   # génère un premier script complet (UTF-8)
owl hello.owl                        # l'exécute
```

> L'option `--output` écrit le fichier en UTF-8. Sous PowerShell, évitez la
> redirection `>`, qui réencoderait le fichier.

Ouvrez sinon `hello.owl` dans VS Code (extension OWL installée) : **F9**
l'exécute, **F1** ouvre la documentation hors ligne sur le mot sous le curseur.

---

## Premiers pas (sans éditeur de code)

Tout part de l'aide en ligne de commande :

```
owl help
```

Elle recense les commandes pour explorer le langage, exécuter du code, formater
et tester. Pour une console interactive, lancer `owl` sans argument.

![Aide en ligne de commande : owl help](images/cli-help.png)

Vous pouvez aussi écrire un fichier à la main — `hello.owl` :

```owl
sNom est une chaîne = "le monde"
Trace(`Bonjour, [%sNom%] !`)
```

---

## Éditeur

Visual Studio Code est l'éditeur pris en charge par défaut. **L'installation de
l'extension OWL est fortement recommandée** pour une expérience d'édition
confortable et intégrée : coloration syntaxique, autocomplétion, informations du
compilateur en direct, diagnostics à la frappe, formatage à l'enregistrement et
designers visuels.

![Édition d'un fichier OWL dans Visual Studio Code](images/vscode-edition.jpg)

Raccourcis fournis par l'extension :

| Touche | Action |
|---|---|
| `F9` | GO sur le fichier courant |
| `Ctrl+F9` | GO sur le projet |
| `Shift+F9` | Déboguer le fichier courant |
| `F5` | Déboguer le projet |
| `F11` | Ouvrir le designer visuel (fenêtres et états) |
| `F1` | Ouvrir la documentation sur le mot sous le curseur |
| `Ctrl+Alt+i` | Ouvrir le catalogue d'icônes |

Toutes les commandes du framework sont aussi regroupées dans la **palette de
commandes** de VS Code : `Ctrl+Shift+P`, puis tapez `OWL` pour les lister
(exécution, débogage, designers, documentation, catalogue d'icônes…), chacune
avec son raccourci — rien à mémoriser.

![Commandes OWL dans la palette de VS Code](images/palette-commandes.png)

**Designer de fenêtre** — une fenêtre se déclare dans son propre fichier `.owl`,
à partir d'un squelette minimal :

![Déclaration minimale d'une fenêtre](images/fenetre-code.png)

Dès lors, deux chemins équivalents : continuer au clavier, ou ouvrir le
**designer visuel** (`F11`) pour composer l'interface à la souris.

![Designer visuel de fenêtre](images/designer-fenetre.jpg)

Le designer et le code sont **synchronisés dans les deux sens** : ce que vous
dessinez s'écrit dans le `.owl`, et ce que vous éditez dans le code se reflète
aussitôt dans le designer.

> **Mode de disposition.** La propriété `Disposition` pilote l'agencement des
> enfants : `dispLibre` place chaque élément librement en X/Y (au pixel près) ;
> `dispVerticale`, `dispHorizontale` et `dispGrille` délèguent au contraire
> l'agencement à un conteneur (placement automatique, sans coordonnées).

**Designer d'état** — mettez en page vos documents imprimables (factures,
rapports) par blocs :

![Designer visuel d'état](images/designer-etat.jpg)

**Catalogue d'icônes intégré** — des milliers d'icônes embarquées, explorables
et insérables directement depuis l'éditeur (ou en ligne de commande avec
`owl icons`) :

![Catalogue d'icônes intégré](images/catalogue-icones.png)

---

## Organisation du code

Owl lie les fichiers `.owl` automatiquement, sans imports à déclarer. Aucun import/include à gérer. Le périmètre d'un fichier se détermine en trois niveaux :

- **Script** — le mode par défaut : un ou plusieurs fichiers `.owl` à plat.
  `owl mon_fichier.owl` exécute le fichier visé.
- **Point d'entrée** — un fichier `main.owl` contenant une procédure `main()`
  désigne, par convention, le point d'entrée d'un ensemble de fichiers ; `owl`
  sans argument le lance.
- **Projet** — un fichier `projet.owl` ouvre une organisation plus complète,
  avec découverte de tous les sous-dossiers.

---

## Explorer le langage

La documentation est issue du cœur du langage et reste interrogeable hors ligne,
dans la version exactement installée :

| Commande | Rôle |
|---|---|
| `owl primer` | L'essentiel en cinq minutes |
| `owl learn` | Les fiches fondamentales (`owl learn types`, …) |
| `owl gotchas` | Les pièges qui ne se devinent pas |
| `owl cheatsheet` | L'aide-mémoire de syntaxe |
| `owl explain <symbole>` | La fiche complète d'une fonction, d'un type, d'une constante |
| `owl search <mot>` | Recherche transversale (documentation, catalogue, exemples) |
| `owl list` | Le catalogue des natives, types et constantes |
| `owl examples <symbole>` | Des exemples démontrant un symbole |

Par exemple, `owl explain ChaîneVersDate` affiche la fiche complète — signatures,
description et exemples exécutables :

![owl explain ChaîneVersDate](images/cli-explain.png)

La même documentation est aussi disponible **hors ligne dans un navigateur**
(touche `F1` dans VS Code, ou depuis le menu Démarrer), avec recherche bilingue
FR / EN :

![Documentation OWL hors ligne](images/doc-offline.png)

---

## Brancher un agent IA

NeOwl embarque un serveur **MCP** (Model Context Protocol). Un agent IA y
découvre le langage et le framework en quelques secondes, dans la version
installée, pour **comprendre, explorer et diagnostiquer** le code — sans
ressource externe et sans rien deviner.

Le serveur correspond à la commande `owl mcp` (transport `stdio`). Il suffit de
le déclarer dans la configuration de l'agent ; celui-ci se charge de le lancer.

**Claude Code** — fichier `.mcp.json` à la racine du projet :

```json
{
  "mcpServers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

Équivalent en ligne de commande : `claude mcp add --transport stdio owl -- owl mcp`

**Cursor** — `.cursor/mcp.json` (projet) ou `~/.cursor/mcp.json` (global) :

```json
{
  "mcpServers": {
    "owl": { "command": "owl", "args": ["mcp"] }
  }
}
```

**Windsurf** — `~/.codeium/windsurf/mcp_config.json` : même format (`mcpServers`).

**Claude Desktop** — `claude_desktop_config.json` : même format (`mcpServers`).

**Visual Studio Code** (agent intégré) — `.vscode/mcp.json`, avec la clé
`servers` :

```json
{
  "servers": {
    "owl": { "command": "owl", "args": ["mcp"] }
  }
}
```

Pour tout autre client compatible MCP : déclarer un serveur de transport `stdio`
exécutant la commande `owl mcp`.

---

## Tests

```
owl test
```

NeOwl découvre les fichiers `test_*.owl` et les exécute (répertoire courant, et tous les sous répertoires si présence d'un fichier projet.owl à la racine). Tous les fichiers qui respectent cette nomenclature sont considérés comme des fichiers de test. Un rapport d'execution s'affiche automatiquement à la fin.

---

## Communauté

- Canal FR : <https://t.me/neowl_fr>
- Canal EN : <https://t.me/neowl_en>
- Groupe de discussion : <https://t.me/+RuC4ayexheJkYTlk>
