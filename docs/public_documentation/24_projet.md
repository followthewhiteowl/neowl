# Le fichier projet.owl

Un projet OWL est délimité par un fichier nommé `projet.owl` posé à la
racine du projet. Ce fichier joue trois rôles complémentaires : il
**marque la racine** du projet (à partir de lui, les sous-dossiers sont
inclus dans le projet), il **déclare les métadonnées** valables pour tout
le projet, et le **code de son bloc est le programme** — le point de
démarrage du projet (voir § 4bis). La règle de frontière et les
deux niveaux d'organisation (script, projet) sont décrits dans
`00_apercu_du_langage.md` § 11 ; cette fiche détaille le contenu du
fichier lui-même.

Le nom `projet.owl` est **réservé** : on ne le lance jamais comme un
script isolé. `owl`, `owl <dossier>` et `owl projet.owl` lancent tous le
**projet** (métadonnées, inclusion des sous-dossiers, ordre d'exécution). Un
`projet.owl` doit déclarer un bloc `est un Projet` avec au moins le champ
`Langues` ; le reste des métadonnées et le code de démarrage sont optionnels.

## 1. Forme de la déclaration

Les métadonnées s'écrivent dans un bloc déclaratif au format canon des
types composés :

```owl
MonApplication est un Projet
	PROPRIETES
		Langues     = [langueFrançais]
		Description = "Gestion de stock"
		Auteur      = "Scumm Solution"
		Licence     = "MIT"
	FIN
FIN
```

L'identifiant placé avant `est un Projet` (ici `MonApplication`) devient
une **variable globale du projet** : il est lisible depuis n'importe quel
fichier du projet (voir § 5). Le catalogue de champs est **figé** et
couvre plusieurs familles de métadonnées : l'identité du projet
(`Langues`, seul champ obligatoire, plus `Description`, `Auteur`,
`Licence`), la présentation (`Thème`, `FenêtreDémarrage`), les formats
d'affichage par défaut (`FormatDate`, `FormatHeure`, `FormatDurée`,
`FormatNombre`, `FormatMonétaire`) et un garde-fou de nommage
(`ActiveAvertissementConventionDeNommage`). Chacun est détaillé plus bas.
Un champ hors catalogue est rejeté à la compilation. La liste vivante des
champs et de leurs types s'obtient avec `owl explain Projet`.

## 2. Le champ `Langues` (obligatoire)

`Langues` déclare les langues du projet sous forme d'un **tableau
ordonné**, toujours entre crochets `[ ]`, dont chaque entrée est une
constante `langueXxx` ou un tag littéral (voir plus bas). Dès
qu'un bloc `Projet` est déclaré, ce champ est **obligatoire** : au moins
une langue doit être présente.

La **première langue de la liste est la langue par défaut** (langue
source du projet) : c'est elle que le runtime considère comme la langue
d'écriture des chaînes littérales.

La forme liste est la **seule** acceptée, y compris pour une seule
langue :

```owl
MonApplication est un Projet
	PROPRIETES
		Langues = [langueFrançais]
	FIN
FIN
```

Plusieurs langues se déclarent dans le même tableau, l'ordre étant
signifiant :

```owl
MonApplication est un Projet
	PROPRIETES
		Langues = [langueFrançais, langueAnglais, langueAllemand]
	FIN
FIN
```

Ici `langueFrançais` est la langue par défaut, `langueAnglais` et
`langueAllemand` sont des langues additionnelles. La forme scalaire nue
(`Langues = langueFrançais`, sans crochets) est refusée à la
compilation.

Chaque entrée de la liste accepte deux formes équivalentes : une
constante de langue officielle `langueXxx` (par exemple
`langueFrançais`, `langueAnglais`, `langueAllemand`, `langueEspagnol`…)
**ou** un tag littéral non vide. Le tag peut suivre la convention
standard région/langue (`"en-GB"`, `"fr-CH"`) ou désigner une variante
custom ou un patois (`"fr-jura"`, `"sindarin"`). La liste complète des
constantes est consultable via `owl list constants` (filtrer sur
`langue`).

La casse des tags est **normalisée** en forme canonique : la sous-balise
de langue en minuscules, la sous-balise de région en majuscules
(`"fr-fr"` devient `"fr-FR"`). Une constante et un littéral qui
désignent la même valeur sont donc interchangeables.

Sont refusés à la compilation : une **liste vide**, un **doublon**
(deux entrées qui se normalisent vers le même tag) et toute valeur qui
**n'est pas constante** (appel de fonction, variable) — la métadonnée
doit rester lisible sans exécuter le code. La première langue de la
liste est la **langue source** du projet.

La gestion des chaînes traduites elle-même est décrite dans
`21_internationalisation.md`.

## 3. Champs optionnels

Hormis `Langues`, tous les champs du catalogue sont facultatifs. Ils
peuvent être renseignés dans n'importe quel ordre, ou omis.

### 3.1 Identité (chaînes)

| Champ         | Rôle                                   |
|---------------|----------------------------------------|
| `Description` | Description courte du projet.          |
| `Auteur`      | Auteur ou éditeur du projet.           |
| `Licence`     | Licence du projet.                     |

```owl
MonApplication est un Projet
	PROPRIETES
		Langues     = [langueFrançais]
		Description = "Gestion de stock"
	FIN
FIN
```

### 3.2 Présentation

Deux champs pointent vers des instances déclarées dans un fichier
compagnon (référence typée, pas une chaîne) :

| Champ              | Rôle                                                   |
|--------------------|--------------------------------------------------------|
| `Thème`            | Thème de styles appliqué au projet (instance `est un Thème`). |
| `FenêtreDémarrage` | Fenêtre ouverte au lancement (instance `est une Fenêtre`). |

Non renseignés, ces champs valent « aucun » : pas de thème, et un projet
sans fenêtre de démarrage n'ouvre aucune interface automatiquement.

### 3.3 Formats d'affichage par défaut

Ces champs fixent un format hérité par les champs qui n'en portent pas un
qui leur soit propre :

| Champ             | Rôle                                              |
|-------------------|---------------------------------------------------|
| `FormatDate`      | Gabarit de date par défaut (`JJ`, `MM`, `AAAA`…). |
| `FormatHeure`     | Gabarit d'heure par défaut (`HH`, `MI`, `SS`…).   |
| `FormatDurée`     | Gabarit de durée par défaut (`JJ`, `HH`, `MI`…).  |
| `FormatNombre`    | Format numérique inline (séparateurs, décimales). |
| `FormatMonétaire` | Format monétaire inline (symbole, décimales).     |

### 3.4 Garde-fou de nommage

| Champ                                     | Rôle                                        |
|-------------------------------------------|---------------------------------------------|
| `ActiveAvertissementConventionDeNommage`  | Active la vérification des préfixes de champs. |

À `Vrai`, chaque champ d'interface dont le nom ne suit pas le préfixe
canonique de son type déclenche un avertissement non bloquant. La charte
complète des préfixes est décrite dans `12_prefixes_de_nommage.md`.

### 3.5 Exemple des champs avancés

Un `projet.owl` qui ouvre une fenêtre au lancement et active la
vérification de nommage :

```owl
// projet.owl
MonApplication est un Projet
	PROPRIETES
		Langues          = [langueFrançais]
		Description      = "Gestion de stock"

		// ouvre cette fenêtre au démarrage (instance déclarée ailleurs)
		FenêtreDémarrage = FEN_Accueil

		// signale les champs mal préfixés (avertissement non bloquant)
		ActiveAvertissementConventionDeNommage = Vrai
	FIN
FIN
```

`FEN_Accueil` référence une instance `est une Fenêtre` déclarée dans un
fichier compagnon du projet ; la référence s'écrit **sans guillemets**.
La graphie bilingue de chaque champ (forme anglaise incluse) est donnée
par `owl explain Projet`.

## 4. Accès en cascade

L'identifiant du projet est une globale accessible depuis tout fichier
du projet, avec l'opérateur de propriété `..` :

```owl
// FEN_Accueil.owl, ailleurs dans le projet
PROCEDURE FinInitialisation()
	Trace(MonApplication..Description)
	Trace(MonApplication..Auteur)
	Trace(MonApplication..Licence)
FIN
```

Les champs optionnels non renseignés valent la chaîne vide. `Langues`
renvoie la liste ordonnée des constantes déclarées. Ces valeurs sont
disponibles **très tôt** — établies avant tout code exécutable — parce
que ce sont des expressions constantes matérialisées à la compilation.

## 4bis. Le programme et l'ordre d'exécution

Le code écrit **dans le bloc** `est un Projet` de `projet.owl` **est le
programme** : c'est le point de démarrage. Il n'existe **pas de procédure
`Main()` appelée automatiquement** ; `Main` est un nom de procédure ordinaire,
sans statut particulier. Le démarrage, c'est ce code — et rien ne s'écrit en
dehors du bloc.

```owl
// projet.owl — les métadonnées ET le programme sont dans le même bloc
MonApplication est un Projet
	PROPRIETES
		Langues     = [langueFrançais]
		Description = "Gestion de stock"
	FIN

	// le code du bloc est le programme, exécuté en dernier
	Trace("Démarrage de " + MonApplication..Description)
	Ouvre(FEN_Accueil)

	PROCEDURE Fermeture()
		Trace("Fermeture du projet")
	FIN
FIN
```

L'exécution d'un projet suit un ordre déterministe :

1. **Recensement** — OWL relève ce que déclare chaque fichier du projet
   (fenêtres, états, classes, collections, thèmes, projet). Les fichiers qui
   ne déclarent aucun de ces éléments (de simples scripts) sont laissés de
   côté. Recenser n'est pas exécuter : à ce stade, rien ne tourne encore.
2. **Métadonnées et langue source** — les valeurs du bloc `est un Projet`
   sont établies avant tout code exécutable, et restent lisibles en
   cascade depuis n'importe quel fichier.
3. **Collections** — le code au sommet de chaque collection s'exécute
   **d'abord** : ce sont les services que consomme l'application.
4. **`projet.owl`** — son code s'exécute **en dernier**, une fois les
   services en place. C'est lui qui démarre l'application.
5. **`Fermeture()` / `Closing()`** — appelée à la fermeture du projet pour
   libérer les ressources, si `projet.owl` la définit.

Cet ordre — dépendances d'abord, orchestrateur en dernier — garantit que
le programme démarre dans un environnement déjà initialisé.

### Globale du projet lue avant le démarrage : valeur par défaut

Une variable déclarée au top-level du bloc `est un Projet` est une globale du
projet, **résolvable depuis n'importe quel fichier**. Sa *déclaration* (nom et
type) est établie très tôt, mais son *initialisation* (`= …`) n'est jouée que par
le code du bloc `projet.owl`, exécuté **en dernier**. Un fichier qui s'exécute
**avant** ce code — le sommet d'une collection, par exemple — lit donc la globale
à sa **valeur par défaut de type** : chaîne vide, `0`, `0.0`, `Faux`, ou `Null`
pour un type nullable ou composite. La valeur d'initialisation n'apparaît qu'une
fois le code de `projet.owl` exécuté.

```owl
// projet.owl
MonApplication est un Projet
	PROPRIETES
		Langues = [langueFrançais]
	FIN

	seuilAlerte est un entier = 80     // globale du projet, initialisée ici (en dernier)
FIN
```

```owl
// services_stock.owl — collection, exécutée AVANT le code de projet.owl
PROCEDURE surInitialisation()
	Trace(seuilAlerte)     // affiche 0 : l'initialisation à 80 n'a pas encore eu lieu
FIN
```

Les **métadonnées** du bloc `est un Projet` (§ 4) échappent à cette règle : ce
sont des expressions constantes, disponibles dès le départ en accès cascade.

### Confiner une variable au flux de démarrage : `LOCAL`

Par défaut, une variable déclarée au top-level du bloc `est un Projet` est
une **globale du projet**, lisible depuis n'importe quel fichier. Pour un
temporaire dont seul le code de démarrage a besoin — un index de boucle, un
accumulateur — la bascule `LOCAL` la **confine au flux top-level** : elle
n'est alors ni globale du projet, ni visible des procédures, ni accessible
depuis les autres fichiers.

`GLOBAL` et `LOCAL` se posent **seuls sur leur ligne** et basculent le mode
des déclarations qui suivent, jusqu'à la bascule inverse — il n'y a pas de
bloc à refermer. `GLOBAL` rétablit le défaut (globale du projet). Les deux
s'alternent librement.

```owl
MonApplication est un Projet
	PROPRIETES
		Langues = [langueFrançais]
	FIN

	nbServices est un entier = 0     // défaut : globale du projet

	LOCAL
	i est un entier = 0              // confiné au flux de démarrage
	TANTQUE i < 3
		nbServices = nbServices + 1
		i = i + 1
	FIN

	GLOBAL
	Trace("services démarrés : " + nbServices)
FIN
```

Ici `nbServices` est lisible depuis les autres fichiers du projet ; `i`,
confiné, ne l'est pas.

## 5. Graphie bilingue

Comme tout symbole du langage, le marqueur de projet existe sous deux
graphies canoniques équivalentes : `projet.owl` en français et
`project.owl` en anglais. Le type déclaré suit la même règle :
`Projet` / `Project`. Un projet choisit une graphie et s'y tient ; les
deux formes ne cohabitent pas dans un même dossier.

```owl
MyApplication est un Project
	PROPRIETES
		Languages   = languageFrench
		Description = "Inventory management"
	FIN
FIN
```

## 6. Créer un projet

La commande `owl new` génère un marqueur `projet.owl` dans le dossier
courant :

```bash
owl new projet MonApplication
owl new project MyApplication   # graphie anglaise
```

Le nom est optionnel : par défaut, il reprend le nom du dossier courant.
Le fichier généré contient un bloc minimal avec le champ `Langues`
renseigné. La commande refuse d'écraser un marqueur existant.

## 7. À venir

Le fichier `projet.owl` a vocation à accueillir d'autres métadonnées de
projet, notamment la notion de configurations. Ces extensions seront
documentées ici au fur et à mesure de leur ouverture.
