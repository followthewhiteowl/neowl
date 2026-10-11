# Coder avec une IA

Le langage est conçu dès le départ pour être manipulé par un agent IA
autant que par un humain. Le principe et les raisons de ce choix sont
exposés dans `00_apercu_du_langage.md` § 14. Cette fiche décrit le
**mode d'emploi** : comment brancher concrètement une IA sur un projet
pour qu'elle comprenne, explore et vérifie le code sans deviner ni
recourir à des sources extérieures.

Aucune connaissance préalable de l'IA n'est nécessaire pour suivre
cette fiche. Il n'est même pas nécessaire de tout expliquer à l'agent
au préalable : on peut le laisser **découvrir seul** ce qui est
disponible. Une seule commande, `owl help`, liste toutes les commandes
d'exploration ; en enchaînant ensuite `owl explain` ou `owl list` sur
quelques symboles, un agent saisit en quelques minutes les
possibilités du langage et sa façon de faire, puis mène sa propre prise
en main sans intervention.

---

## 1. Deux voies : la ligne de commande ou le serveur MCP

Il existe deux façons de donner à une IA un accès exact au langage et à
son framework. Les deux exposent la même information ; elles diffèrent
seulement par le mode d'accès.

**La ligne de commande, directement.** C'est la voie par défaut,
disponible immédiatement, **sans aucune configuration ni réglage**. Un
agent capable de lancer des commandes shell dispose de tout ce qu'il
faut : `owl help` lui énumère les commandes d'exploration
(`owl explain <nom>`, `owl search <terme>`, `owl examples <nom>`,
`owl list …`), de vérification (`owl --check`) et de mise en forme
(`owl fmt`). Il n'y a rien à installer ni à déclarer : le binaire
répond de lui-même.

**Le serveur MCP.** La même information, exposée cette fois via un
protocole structuré. C'est plus confortable pour un agent qui préfère
appeler des outils déclarés plutôt que de composer des commandes
shell. Cette voie demande une petite déclaration de configuration,
décrite dans le reste de cette fiche.

Aucune des deux n'est obligatoire au détriment de l'autre : la ligne de
commande ne réclame rien et fonctionne tout de suite, le serveur MCP
est un confort supplémentaire pour les clients qui savent en tirer
parti.

---

## 2. Le serveur intégré

Le binaire `owl` embarque un **serveur MCP** (Model Context Protocol).
MCP est un protocole standard par lequel un assistant IA se connecte à
un outil extérieur pour lui poser des questions. Ici, l'outil extérieur
est le langage lui-même : l'IA interroge la version installée du
langage et de son framework, et obtient des réponses exactes tirées du
binaire, pas d'une mémoire approximative.

Le serveur se lance par la commande :

```bash
owl mcp
```

Le transport utilisé est `stdio` (entrée/sortie standard). En pratique,
il n'est **pas** nécessaire de lancer cette commande à la main : le
client IA s'en charge tout seul, une fois qu'il sait que le serveur
existe. Il suffit donc de le **déclarer** dans la configuration du
client. C'est l'objet de la section suivante.

---

## 3. Brancher un client

Chaque client IA compatible MCP a son propre fichier de configuration,
mais le contenu est presque toujours le même : déclarer un serveur
nommé `owl`, de transport `stdio`, exécutant la commande `owl mcp`.

**Claude Code** — fichier `.mcp.json` à la racine du projet :

```json
{
  "mcpServers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

**Cursor** — fichier `.cursor/mcp.json` (par projet) ou `~/.cursor/mcp.json`
(global) :

```json
{
  "mcpServers": {
    "owl": { "command": "owl", "args": ["mcp"] }
  }
}
```

**Visual Studio Code** (agent intégré) — fichier `.vscode/mcp.json`. La clé
racine est ici `servers`, et non `mcpServers` :

```json
{
  "servers": {
    "owl": { "type": "stdio", "command": "owl", "args": ["mcp"] }
  }
}
```

**Tout autre client compatible MCP** suit le même principe : déclarer un
serveur de transport `stdio` qui exécute la commande `owl mcp`. Le nom de
fichier et la clé racine varient d'un client à l'autre, mais la commande
lancée reste `owl mcp`.

Le champ `command` vaut `owl` lorsque le binaire est accessible dans le
`PATH`. Sinon, indiquer le chemin complet vers l'exécutable.

### IA distante ou locale

Le serveur ne dépend d'aucun service en ligne : il tourne sur la machine où
`owl` est installé. Le **modèle** qui s'y connecte, lui, peut être distant
(un assistant hébergé en ligne) ou local (un modèle exécuté sur la même
machine). Dans les deux cas, le branchement est identique : c'est le client
MCP qui fait le pont entre le modèle et le serveur `owl mcp`. Un modèle
local se raccorde donc exactement de la même façon, du moment que le client
utilisé sait parler MCP.

---

## 4. Ce que l'IA peut faire une fois branchée

Le serveur met à disposition un jeu d'outils que l'assistant appelle de
lui-même selon ses besoins. Ils se répartissent en deux familles.

**Comprendre et explorer le langage :**

| Outil | Ce qu'il retourne |
|---|---|
| `owl_explain` | Fiche complète d'un symbole : description, signatures, propriétés, énumérations, scénarios. Un même nom peut désigner à la fois un type et une fonction ; les deux sont retournés. |
| `owl_search` | Recherche transversale, insensible à la casse, dans la documentation, le registry (natives, types, énumérations, constantes) et les scénarios. Pour trouver « ce qui touche à un sujet » sans connaître le nom exact. |
| `owl_examples` | Scénarios canoniques qui démontrent l'usage d'un symbole, code inclus tel quel. |
| `owl_where` | Recherche inverse : qui utilise ce symbole. Pour une propriété, liste les éléments qui l'acceptent ; pour un type d'élément, indique le contexte parent. |
| `owl_signature` | Forme courte d'une native : uniquement les signatures `Nom(arg : type) → retour`, sans description ni exemple. |
| `owl_list` | Énumère une famille du catalogue : natives, types, constantes, mots-clés, opérateurs, éléments d'interface, événements, propriétés, variables globales prédéfinies, limites du compilateur. La famille `categories` donne l'index des catégories fonctionnelles (interface, chaînes, dates, SQL…), qu'un filtre `cat` permet ensuite de cibler. |

**Vérifier et produire du code :**

| Outil | Ce qu'il retourne |
|---|---|
| `owl_check` | Diagnostic à la compilation, sur un extrait isolé. Retourne un JSON `{errors, warnings}` avec ligne, colonne et message. Ne résout pas les autres fichiers du projet et n'exécute rien. |
| `owl_format` | Reformate un source selon la forme canonique (casse, accents, indentation) et retourne le code corrigé. |
| `owl_translate` | Réécrit la surface du langage dans l'autre langue (`fr` ou `en`) : mots-clés, natives, types et propriétés changent de langue ; identifiants, chaînes et commentaires restent inchangés. |
| `owl_template` | Squelettes prêts à l'emploi, avec exemples : script, classe, scénario de test, fenêtre. |
| `owl_new` | Ossature minimale d'un élément (fenêtre, classe, champ) : déclaration et événements de base, pour démarrer. |
| `owl_limits` | Registre des limites du compilateur (nombre de types, de méthodes, de paramètres, etc.), pour dimensionner un projet à l'avance. |

L'assistant choisit lui-même l'outil pertinent : nul besoin de le lui
indiquer à chaque question. Ces mêmes outils ont un équivalent direct en
ligne de commande (`owl explain`, `owl search`, `owl examples`,
`owl --check`, `owl fmt`…), ce qui permet à un agent sans MCP de faire
exactement le même travail.

---

## 5. Workflow recommandé

Un agent peut commencer par une **prise en main autonome** : `owl help`
pour découvrir les commandes, puis `owl explain` et `owl list` sur les
symboles rencontrés. En quelques appels, il cerne les possibilités et
les conventions du langage, sans qu'on ait à les lui décrire. Deux
garanties rendent ensuite la génération de code fiable, et méritent
d'être exploitées explicitement.

**Diagnostics déterministes.** La commande `owl --check` (et l'outil
`owl_check` côté serveur) produit des erreurs et avertissements au format
JSON, identiques d'une exécution à l'autre. Un agent gagne à vérifier
systématiquement un extrait avec `owl --check` **avant** de l'écrire sur
disque : les erreurs de compilation sont attrapées tout de suite, avec leur
position exacte, plutôt que découvertes plus tard.

**Forme canonique restaurée.** Il n'existe qu'une seule façon canonique
d'écrire chaque construction. Le formatter la rétablit — casse, accents,
indentation — via `owl fmt` (ou `owl_format`). Une IA n'a donc pas à
mémoriser la casse exacte d'un nom ni à choisir entre plusieurs styles :
elle produit du code approximatif sur la forme, et la forme canonique est
restaurée mécaniquement. Le fond reste sous sa responsabilité, la forme est
garantie par l'outil.

La combinaison des deux — vérifier avec `owl --check`, normaliser avec
`owl fmt` — donne une boucle courte et sûre : générer, vérifier,
corriger, sans jamais deviner.

---

## Pour aller plus loin

La motivation de ce choix de conception — pourquoi le langage est pensé
pour les agents IA — est décrite dans `00_apercu_du_langage.md` § 14.

Les mêmes informations que celles exposées par le serveur sont accessibles
directement en ligne de commande, notamment `owl explain <nom>` et
`owl examples <nom>`.
