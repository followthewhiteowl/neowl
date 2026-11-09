# La journalisation

Le langage intègre un **journal applicatif à niveaux** : le type `Logger` et sa
famille de fonctions `Logger*`. Le principe suit les pratiques actuelles
(journaux structurés, niveaux de verbosité, séparation nette entre *produire*
une entrée et *la mettre en forme*). Le cœur reste minimal — quatre niveaux, un
enregistrement décomposé — et **deux points d'extension** permettent de l'adapter
à n'importe quelle chaîne de collecte (fichiers, agents de plateforme, systèmes
cloud) **sans dépendance supplémentaire** : une procédure de **formatage** décide
de la forme émise, une **destination** décide d'où elle sort.

Comme le reste du langage, chaque symbole est **bilingue** FR/EN : les formes
anglaises canoniques (`LoggerInfo`, `Level`, `Formatter`…) sont équivalentes aux
formes françaises et sont listées en fin de fiche.

## 1. Journal par défaut et instances

Deux manières d'écrire un journal, identiques dans leur comportement.

**Le journal par défaut**, sans rien déclarer : les fonctions `LoggerDebug`,
`LoggerInfo`, `LoggerAvert` et `LoggerErreur` émettent directement. C'est la voie
la plus courte, adaptée à un programme ou un service qui n'a qu'un journal.
`LoggerParamètre` règle ce journal implicite :

```owl
LoggerParamètre(nivInfo)                 // seuil : Info et au-dessus
LoggerInfo("Service démarré")
LoggerDebug("Sous le seuil, non émis")
LoggerErreur("Connexion à la base refusée")
```

**Une instance `Logger`**, déclarée et configurée par propriétés (`..`), quand un
programme tient plusieurs journaux distincts (un par composant, par exemple). Les
méthodes `.Debug`, `.Info`, `.Avert`, `.Erreur` émettent sur l'instance :

```owl
journal est un Logger
journal..Niveau = nivDebug               // ce journal-ci est plus verbeux
journal.Info("Chargement de la configuration")
journal.Debug("3 sections lues")
```

Le type et les fonctions statiques portent le **même comportement** : ce qui
suit vaut pour les deux.

## 2. Les quatre niveaux

Un niveau classe l'importance d'une entrée. Du moins au plus grave :

| Niveau | Quand l'employer | Exemple |
|--------|------------------|---------|
| `nivDebug` | Diagnostic de développement : valeurs, états intermédiaires, chemins pris. Le plus verbeux. | « cache vide pour la clé `u-42` » |
| `nivInfo` | Événements normaux attendus, le fil du « ça fonctionne ». | « service démarré », « commande 1007 traitée » |
| `nivAvert` | Situation anormale mais **non bloquante** : repli sur un défaut, nouvelle tentative, usage déprécié. | « 2ᵉ tentative de connexion au broker » |
| `nivErreur` | **Échec** d'une opération : exception rattrapée, requête qui n'aboutit pas. | « paiement refusé : fonds insuffisants » |

**Le seuil filtre à l'émission.** La propriété `..Niveau` (ou l'argument de
`LoggerParamètre`) fixe un **plancher de gravité** : une entrée est émise si son
niveau est **au moins aussi grave** que le seuil, sinon elle est **ignorée sans
coût**. L'ordre de gravité est `nivDebug < nivInfo < nivAvert < nivErreur`.
Fixer un niveau n'« autorise » donc pas les niveaux *moins* graves : avec
`..Niveau = nivInfo`, ce sont `Info`, `Avert` et `Erreur` qui passent (Info et
plus grave), tandis que `Debug` (moins grave) reste muet. Abaisser le seuil ouvre
davantage d'entrées, le relever les restreint ; en production on reste
typiquement à `nivInfo`, on descend à `nivDebug` le temps d'une investigation.

## 3. L'enregistrement et le formateur

Quand une entrée franchit le seuil, le journal construit un **enregistrement
décomposé**, le type `LogEnreg`, dont les champs se lisent par `..` :

| Propriété | Contenu |
|-----------|---------|
| `..Horodatage` | Date-heure de l'entrée. |
| `..Niveau` | Le niveau de l'entrée, comme valeur d'énumération (`nivInfo`…) — utile pour comparer. |
| `..NiveauTexte` | Le niveau en **texte** (`DEBUG` / `INFO` / `WARN` / `ERROR`) — à employer dans la sortie formatée, là où `..Niveau` ne donnerait qu'une valeur interne. |
| `..Message` | Le message passé à l'appel. |

Par défaut, l'entrée sort sous une forme texte simple et neutre. Pour décider
**soi-même** de la forme émise, on pose une procédure de formatage sur la
propriété `..Formateur` : elle reçoit l'enregistrement et renvoie la chaîne à
émettre. Le journal garde l'émission ; la procédure ne décide que de la forme.

```owl
// Un format « clé=valeur » (logfmt), lisible et facile à indexer.
PROCEDURE formatLogfmt(enr est un LogEnreg)
	RENVOYER `ts=[%enr..Horodatage%] niveau=[%enr..NiveauTexte%] msg="[%enr..Message%]"`
FIN

journal est un Logger
journal..Formateur = formatLogfmt
journal.Info("Commande traitée")
```

C'est ce point unique qui permet d'épouser le format attendu par n'importe quel
collecteur, sans rien installer : il suffit d'écrire la procédure qui produit la
forme voulue.

## 4. Destination et émetteur

Le **où** est séparé du **comment**. Deux propriétés, selon le besoin :

- `..Destination` — une cible intégrée. Par défaut la sortie standard ; on peut
  donner un chemin de fichier. C'est le cas courant et recommandé : le programme
  écrit sur la sortie standard (ou un fichier), un **agent de la plateforme**
  (collecteur de conteneur, démon de journalisation) se charge d'acheminer.
- `..Émetteur` — une procédure d'émission, quand on veut **pousser soi-même**
  chaque entrée vers un système distant. Elle reçoit la ligne formatée et
  l'envoie avec les fonctions **du langage** (`HTTPEnvoie`, `AWSRequête`…). Aucune
  bibliothèque dédiée par système : le framework se branche sur lui-même.

```owl
journal est un Logger
journal..Destination = "service.log"     // un fichier au lieu de la sortie standard
journal.Info("Démarré")
```

Dans la grande majorité des cas, un format structuré sur la sortie standard
suffit — c'est le modèle que suivent les trois intégrations ci-dessous.

## 5. Trois intégrations standards

Même principe partout : **adapter le formateur** (et, si besoin, l'émetteur),
jamais ajouter de bibliothèque. Le code métier, lui, appelle toujours
`journal.Info(...)` de la même façon.

### EFK (Elasticsearch / Fluentd / Kibana) — JSON sur la sortie standard

Le collecteur ingère du JSON ligne par ligne. Le formateur produit un objet JSON,
la destination reste la sortie standard : l'agent de collecte fait le reste.

```owl
PROCEDURE formatJSON(enr est un LogEnreg)
	RENVOYER `{"@timestamp":"[%enr..Horodatage%]","level":"[%enr..NiveauTexte%]","message":"[%enr..Message%]"}`
FIN

LoggerParamètre(nivInfo, formatJSON)      // journal par défaut : seuil + formateur
LoggerInfo("Requête traitée")
LoggerErreur("Base injoignable")
```

### Grafana / Loki — logfmt sur la sortie standard

Loki indexe des étiquettes et stocke la ligne. Le format `logfmt` (`clé=valeur`)
se parcourt nativement dans Grafana ; l'agent (Promtail, Alloy) attache les
étiquettes depuis le conteneur. Côté programme, seul le formateur change :

```owl
PROCEDURE formatLogfmt(enr est un LogEnreg)
	RENVOYER `level=[%enr..NiveauTexte%] ts=[%enr..Horodatage%] msg="[%enr..Message%]"`
FIN

LoggerParamètre(nivInfo, formatLogfmt)
LoggerInfo("Lot importé")
```

### AWS Lambda conteneurisée (Docker) — JSON capté par CloudWatch

Dans une fonction Lambda packagée en image Docker, **tout ce qui sort sur la
sortie standard est capté automatiquement vers CloudWatch Logs** — aucun SDK ni
appel réseau à faire depuis le code. Il suffit donc d'émettre du JSON structuré
sur la sortie standard : CloudWatch Logs Insights sait ensuite filtrer sur les
champs (`level`, `requestId`…).

```owl
// Le formateur enrichit l'entrée d'un identifiant de requête, utile pour
// corréler les lignes d'une même invocation dans CloudWatch.
PROCEDURE formatLambda(enr est un LogEnreg)
	RENVOYER `{"time":"[%enr..Horodatage%]","level":"[%enr..NiveauTexte%]","msg":"[%enr..Message%]","requestId":"[%gIdRequete%]"}`
FIN

gIdRequete est une chaîne = ""

// Appelé au début de chaque invocation du handler.
PROCEDURE traiteEvenement(idRequete est une chaîne)
	gIdRequete = idRequete
	LoggerInfo("Invocation reçue")
	// ... traitement ...
	LoggerInfo("Invocation terminée")
FIN

LoggerParamètre(nivInfo, formatLambda)
```

Si un cas impose de **pousser** les entrées vers une API (plutôt que de laisser
la plateforme collecter la sortie standard), c'est le rôle de `..Émetteur` : une
procédure qui relaie chaque ligne via `AWSRequête` ou `HTTPEnvoie`. Le modèle
« sortie standard captée par la plateforme » reste toutefois le plus simple et le
plus robuste, et c'est celui recommandé en conteneur.

## Aliases anglais

**Type et enregistrement**

| FR canon | EN canon |
|----------|----------|
| `Logger` | `Logger` |
| `LogEnreg` | `LogRecord` |

**Fonctions**

| FR canon | EN canon |
|----------|----------|
| `LoggerDebug` | `LoggerDebug` |
| `LoggerInfo` | `LoggerInfo` |
| `LoggerAvert` | `LoggerWarn` |
| `LoggerErreur` | `LoggerError` |
| `LoggerParamètre` | `LoggerSetting` |

**Méthodes**

| FR canon | EN canon |
|----------|----------|
| `.Debug` | `.Debug` |
| `.Info` | `.Info` |
| `.Avert` | `.Warn` |
| `.Erreur` | `.Error` |

**Propriétés**

| FR canon | EN canon |
|----------|----------|
| `..Niveau` | `..Level` |
| `..NiveauTexte` | `..LevelText` |
| `..Formateur` | `..Formatter` |
| `..Destination` | `..Destination` |
| `..Émetteur` | `..Emitter` |
| `..Horodatage` | `..Timestamp` |
| `..Message` | `..Message` |

**Niveaux (`logNiveau` / `logLevel`)**

| FR canon | EN canon |
|----------|----------|
| `nivDebug` | `lvlDebug` |
| `nivInfo` | `lvlInfo` |
| `nivAvert` | `lvlWarn` |
| `nivErreur` | `lvlError` |

Voir aussi : [AWS](36_aws.md), [La messagerie](41_messagerie.md), [Les services
REST](40_services_rest.md).
