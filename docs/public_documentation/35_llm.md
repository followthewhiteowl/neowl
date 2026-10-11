# Modèles de langage : `LLM`

## 1. En bref

Un `LLM` charge un **modèle de langage** depuis un fichier au format
GGUF, puis **génère du texte** à partir d'une amorce. Deux gestes
suffisent : **charger** le modèle avec `.Charge`, **générer** avec
`.Génère`. Les réglages d'un appel de génération vivent dans une feuille
séparée, `OptionsGénération`.

```owl
assistant est un LLM
SI assistant.Charge("modeles/mon-modele.gguf") ALORS
	options est un OptionsGénération
	options..Température = 0
	options..MaxTokens = 64

	réponse est une chaîne = assistant.Génère("Il était une fois", options)
	Trace(réponse)
FIN
```

Pour **dialoguer** avec un modèle de conversation plutôt que compléter du
texte brut, `.Chat` prend l'historique d'une conversation (rôles + contenus)
et rend une réponse structurée — voir la section 7.

Le modèle lui-même n'est pas fourni : `LLM` charge un fichier GGUF que
l'on dépose à côté du programme. `LLM` complète la famille
d'apprentissage automatique aux côtés d'`IAModèle`, qui construit et
entraîne des réseaux de neurones (fiche [`34_iamodele.md`](34_iamodele.md)) :
les deux appartiennent à la catégorie **Intelligence artificielle**.

---

## 2. Charger un modèle

`.Charge(chemin)` lit un fichier GGUF et prépare l'inférence. Elle rend
`Vrai` en cas de succès, `Faux` sinon — et dans ce cas `ErreurDétectée`
est posé, sans plantage. Un chemin absent, vide ou pointant vers un
fichier illisible échoue proprement.

```owl
assistant est un LLM
ErreurRAZ()
ok est un booléen = assistant.Charge("modeles/mon-modele.gguf")
SI PAS ok ALORS
	Trace("chargement impossible : " + ErreurInfo())
FIN
```

Recharger un modèle sur le même `LLM` remplace le précédent : les
ressources du modèle abandonné sont libérées automatiquement.

La forme anglaise est `.Load`.

---

## 3. Générer du texte

`.Génère(amorce, options)` produit du texte à partir d'une amorce
(le *prompt*) et d'une feuille `OptionsGénération`. Elle rend le texte
généré sous forme de chaîne.

```owl
options est un OptionsGénération
options..MaxTokens = 128

texte est une chaîne = assistant.Génère("Résume la photosynthèse.", options)
Trace(texte)
```

La génération s'arrête au premier des deux événements suivants : le
modèle émet un jeton de fin de séquence, ou le nombre maximum de jetons
(`..MaxTokens`) est atteint.

Générer sur un `LLM` qui n'a jamais été chargé est un échec récupérable,
pas un plantage : `ErreurDétectée` est posé et la chaîne rendue est vide.

La forme anglaise est `.Generate`.

---

## 4. Régler la génération : `OptionsGénération`

`OptionsGénération` regroupe les réglages d'un appel de génération, avec
des valeurs par défaut sensées.

| Propriété | Rôle | Défaut |
|---|---|---|
| `..Température` | Contrôle le hasard : `0` choisit systématiquement le jeton le plus probable (sortie reproductible) ; plus la valeur monte, plus la sortie est variée. | `0.8` |
| `..TopK` | Nombre de jetons candidats les plus probables gardés avant tirage. | `40` |
| `..TopP` | Seuil de masse de probabilité cumulée (échantillonnage noyau). | `0.95` |
| `..MaxTokens` | Nombre maximum de jetons générés avant arrêt. | `512` |
| `..Graine` | Graine du tirage aléatoire : une même graine rend une sortie reproductible pour une température non nulle (sans effet en mode déterministe). | aléatoire |
| `..ContrainteDeSortie` | Contraint la forme de la sortie (JSON garanti, grammaire GBNF). Voir la section 6. | `contrainteLibre` |

```owl
options est un OptionsGénération
options..Température = 0.7
options..TopK = 20
options..TopP = 0.9
options..MaxTokens = 256
options..Graine = 42
```

Les formes anglaises sont `..Temperature`, `..TopK`, `..TopP`,
`..MaxTokens` et `..Seed`.

### Génération reproductible

Une température à `0` désactive le tirage aléatoire : le modèle choisit
toujours le jeton le plus probable. Deux appels de `.Génère` avec la même
amorce et le même modèle rendent alors exactement le même texte — utile
pour verrouiller un comportement sans dépendre du hasard.

```owl
options est un OptionsGénération
options..Température = 0

premier est une chaîne = assistant.Génère("Bonjour", options)
second est une chaîne = assistant.Génère("Bonjour", options)
// premier et second sont identiques
```

---

## 5. Suivre la génération jeton par jeton : `..SurToken`

Pour réagir au fil de la génération — afficher le texte au fur et à
mesure, mettre à jour une interface — `OptionsGénération` accepte un
**callback de flux** `..SurToken`. La procédure référencée est rappelée
**une fois par jeton généré**, avec deux paramètres : le morceau de texte
du jeton, et un booléen `terminé` valant `Vrai` sur le **dernier**
morceau (`Faux` pendant le flux).

```owl
PROCEDURE AfficheAuFil(morceau est une chaîne, terminé est un booléen)
	Trace(morceau)
	SI terminé ALORS
		Trace("--- génération terminée ---")
	FIN
FIN

options est un OptionsGénération
options..SurToken = AfficheAuFil

texte est une chaîne = assistant.Génère("Raconte une histoire courte.", options)
```

Le contrat du callback :

- **Deux paramètres** : `morceau` (chaîne, le bout de texte du jeton) et
  `terminé` (booléen, `Vrai` sur le dernier morceau).
- **Rappelé une fois par jeton**, de façon synchrone pendant l'appel à
  `.Génère` (ou `.Chat`).
- **Fin de flux** : signalée par `terminé = Vrai` sur le dernier morceau,
  connue dans le callback sans attendre le retour de la fonction. Si
  aucun jeton n'est produit (fin immédiate), le callback reçoit un unique
  appel avec `morceau = ""` et `terminé = Vrai`.
- **Valeur de retour ignorée.**

Le callback se référence **au nom nu** (sans parenthèses) et peut être
défini plus bas dans le fichier. La concaténation des morceaux reçus par
`..SurToken` correspond exactement au texte final rendu.

La forme anglaise est `..OnToken`. Le même callback s'applique à `.Génère`
et à `.Chat` (section 7).

---

## 6. Sortie contrainte : `..ContrainteDeSortie`

Demander du JSON à un modèle en le formulant dans l'amorce reste
fragile : la réponse peut envelopper le JSON dans du texte ou le
malformer. Une seule propriété offre une garantie forte à la place —
`..ContrainteDeSortie` **contraint** la génération de sorte que seuls les
jetons gardant la sortie conforme sont produits.

La propriété est une chaîne à trois modes :

| Valeur | Effet |
|---|---|
| `contrainteLibre` (défaut) | Aucune contrainte : génération libre. |
| `contrainteJSON` | Sortie **JSON structurellement valide**, garantie par une grammaire intégrée. |
| toute autre chaîne | Traitée comme une **grammaire GBNF** libre à laquelle la sortie doit se conformer. |

Sa forme anglaise est `..OutputConstraint` ; les constantes portent les
formes `constraintFree` et `constraintJSON`.

### JSON valide, ou erreur claire — avec `contrainteJSON`

`..ContrainteDeSortie = contrainteJSON` applique une grammaire JSON
intégrée : seuls les jetons gardant la sortie structurellement valide
sont émis.

```owl
options est un OptionsGénération
options..Température        = 0
options..MaxTokens          = 128
options..ContrainteDeSortie = contrainteJSON

texte est une chaîne = assistant.Génère("Donne un objet avec un nom et un âge.", options)
// texte est un JSON valide
doc est un JSON = ChaîneVersJSON(texte)
```

Une grammaire contraint les **jetons**, mais ne peut pas **forcer la
complétion** : si `..MaxTokens` tombe au milieu d'un objet, la structure
reste inachevée. La sortie tronquée n'est **jamais rendue en silence** —
`.Génère` signale alors `ErreurDétectée` avec un message explicite
(augmenter `..MaxTokens`) et rend le texte partiel, inspectable. Une
sortie JSON complète ne signale aucune erreur.

### Schéma précis avec une grammaire GBNF

Pour contraindre la sortie plus finement — un format exact, un ensemble
de réponses fermé — `..ContrainteDeSortie` accepte n'importe quelle
grammaire GBNF (toute chaîne autre que `contrainteJSON`, reconnaissable à
son opérateur `::=`). Le contenu n'est interprété qu'au moment de générer.

```owl
options est un OptionsGénération
options..ContrainteDeSortie = `root ::= "oui" | "non"`

réponse est une chaîne = assistant.Génère("Le ciel est-il bleu ?", options)
// réponse vaut "oui" ou "non", jamais autre chose
```

Le garde-fou anti-troncature ne s'applique qu'au mode `contrainteJSON` :
la complétion d'une grammaire arbitraire ne peut pas être vérifiée
automatiquement, c'est à l'amorce et à `..MaxTokens` de laisser à la
structure la place de se compléter.

Une grammaire GBNF malformée n'entraîne pas de plantage : `.Génère`
signale un échec récupérable (`ErreurDétectée` posé) et rend une chaîne
vide, exactement comme une génération sur un modèle non chargé.

La contrainte a un coût par jeton (filtrage du vocabulaire à chaque pas),
sans effet observable sur la forme du résultat au-delà de la conformité
garantie.

---

## 7. Conversation multi-tours : `.Chat`

`.Génère` fait de la **complétion brute** : on lui donne une amorce, elle
la prolonge. Pour dialoguer avec un modèle « instruct/chat », on utilise
`.Chat`, qui distingue les **rôles** (système, utilisateur, assistant) et
applique le **gabarit de dialogue** propre au modèle avant de générer.

`.Chat(messages, options)` prend l'**historique complet** de la
conversation — un tableau de `MessageLLM` — et rend une `RéponseLLM`. Le
dialogue est **sans état** : rien n'est conservé entre deux appels, on
repasse l'historique à chaque fois.

```owl
messages est un tableau de MessageLLM

système est un MessageLLM = {Rôle = rôleSystème, Contenu = "Tu es un assistant concis."}
messages.Ajoute(système)

question est un MessageLLM = {Rôle = rôleUtilisateur, Contenu = "Explique la photosynthèse en une phrase."}
messages.Ajoute(question)

options est un OptionsGénération
options..Température = 0
options..MaxTokens = 128

réponse est une RéponseLLM = assistant.Chat(messages, options)
Trace(réponse..Contenu)
```

Toutes les options de génération (sections 4 à 6) s'appliquent
identiquement : `..Température`, `..MaxTokens`, `..SurToken`,
`..ContrainteDeSortie`, etc.

### Le message : `MessageLLM`

Un `MessageLLM` porte deux propriétés :

| Propriété | Rôle |
|---|---|
| `..Rôle` | Le rôle de l'auteur du message. |
| `..Contenu` | Le texte du message. |

Il se construit **au littéral d'accolade nommé**, en deux lignes : une
instance nommée, puis un ajout au tableau.

```owl
m est un MessageLLM = {Rôle = rôleUtilisateur, Contenu = "Bonjour"}
messages.Ajoute(m)
```

Le rôle se choisit parmi quatre constantes suggérées, mais toute chaîne
libre est également acceptée (rôle personnalisé, évolution du standard) :

| Constante | Usage |
|---|---|
| `rôleSystème` | Instructions de cadrage de l'assistant. |
| `rôleUtilisateur` | Le message adressé à l'assistant. |
| `rôleAssistant` | Une réponse du modèle, dans l'historique. |
| `rôleOutil` | Le résultat d'un appel d'outil, dans l'historique. |

Les formes anglaises sont le type `LLMMessage`, les propriétés
`..Role` / `..Content` et les constantes `roleSystem`, `roleUser`,
`roleAssistant`, `roleTool`.

### La réponse : `RéponseLLM`

`.Chat` rend une `RéponseLLM` en lecture seule :

| Propriété | Rôle |
|---|---|
| `..Contenu` | Le texte de réponse de l'assistant. |
| `..Raison` | Pourquoi la génération s'est arrêtée : `stopNaturel` (jeton de fin émis) ou `stopMaxTokens` (plafond `..MaxTokens` atteint, réponse coupée). |
| `..JetonsEntrée` | Nombre de jetons consommés par l'invite. |
| `..JetonsSortie` | Nombre de jetons générés dans la réponse. |

Les compteurs de jetons permettent de budgéter la fenêtre de contexte et
de savoir quand élaguer un historique devenu trop long.

```owl
réponse est une RéponseLLM = assistant.Chat(messages, options)
SI réponse..Raison = stopMaxTokens ALORS
	Trace("réponse coupée à " + réponse..JetonsSortie + " jetons")
FIN
```

Les formes anglaises sont le type `LLMResponse`, les propriétés
`..Content` / `..Reason` / `..InputTokens` / `..OutputTokens`, et les
constantes `stopNatural`, `stopMaxTokens`.

### Poursuivre le dialogue

Comme l'historique est repassé à chaque appel, on prolonge une
conversation en y ajoutant la réponse de l'assistant puis le message
suivant de l'utilisateur. Chaque message se construit sur une instance
nommée (deux lignes) avant d'être ajouté au tableau.

```owl
écho est un MessageLLM = {Rôle = rôleAssistant, Contenu = réponse..Contenu}
messages.Ajoute(écho)

relance est un MessageLLM = {Rôle = rôleUtilisateur, Contenu = "Et pour un enfant de six ans ?"}
messages.Ajoute(relance)

suite est une RéponseLLM = assistant.Chat(messages, options)
```

> Note : la construction d'un `MessageLLM` par littéral d'accolade passe
> par une instance nommée (`m est un MessageLLM = {...}`), puis
> `.Ajoute(m)`. C'est le type déclaré de la variable qui porte la forme
> des champs.

### Sortie contrainte et troncature

`..ContrainteDeSortie = contrainteJSON` façonne `réponse..Contenu`
exactement comme pour `.Génère` : la sortie est un JSON valide, ou une
erreur est signalée — jamais de JSON cassé rendu en silence. Lorsque le
plafond `..MaxTokens` coupe une structure JSON, `.Chat` pose
`ErreurDétectée` **et** `réponse..Raison` reflète `stopMaxTokens`.

### Sans modèle chargé

Appeler `.Chat` sur un `LLM` qui n'a jamais été chargé est un échec
récupérable : `ErreurDétectée` est posé et la `RéponseLLM` rendue est
neutre (`..Contenu` vide, compteurs à zéro), sans plantage.

### Indépendance des appels

`.Chat` et `.Génère` sur la même instance sont **indépendants** : chaque
appel repart d'un contexte propre. Un `.Génère` intercalé entre deux
`.Chat` identiques (mêmes messages, réglages déterministes) ne change pas
le résultat du second `.Chat`.

---

## 8. Performance et empreinte

L'inférence s'exécute sur processeur (CPU), sans accélération matérielle
dédiée. La vitesse et la mémoire nécessaires dépendent directement de la
taille du modèle chargé : un modèle de plusieurs milliards de paramètres
demande plusieurs gigaoctets de mémoire et génère plus lentement qu'un
petit modèle. Le dimensionnement du modèle est donc un choix à faire en
fonction de la tâche et de la machine cible.

---

## 9. Hors-périmètre actuel

Ces usages **ne sont pas encore supportés** :

- **Entraîner ou ajuster un modèle de langage** — `LLM` charge et
  interroge un modèle existant ; il ne l'entraîne pas et ne l'affine pas.
- **Accélération matérielle (GPU)** — la génération s'exécute sur
  processeur uniquement.
- **Formats autres que GGUF** — seul le format GGUF est pris en charge
  pour le chargement.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Construire et entraîner un réseau de neurones (`IAModèle`) | [`34_iamodele.md`](34_iamodele.md) |
| Gestion des exceptions et `ErreurDétectée` | [`08_exceptions.md`](08_exceptions.md) |
| Brancher un agent IA sur le langage | [`25_agents_ia.md`](25_agents_ia.md) |
