# Internationalisation

OWL traite le multilangue comme un **type natif du langage**, pas
comme une bibliothèque externe ni un format de fichier séparé. Les
traductions vivent **dans le fichier `.owl`**, à côté de la chaîne
qu'elles décrivent. Aucun fichier `.po`/`.ts`/`.json` à synchroniser.

Trois piliers structurent l'API :

| Pilier | Surface |
|---|---|
| Deux types first-class | `chaîneMultilingue`, `langue` |
| Indexation BCP 47 (RFC 5646) | enum `langueXxx` (34 constantes) ou tag string libre |
| Coerce auto bidirectionnel | `chaîne` ⇄ `chaîneMultilingue` selon le contexte |

> **Rationale architectural** : décision d'architecture dédiée (type multilingue first-class, BCP 47).

## Types

### `chaîneMultilingue`

Dictionnaire interne tag → chaîne. Indexable par constante
`langueXxx` (`m[langueFrançais]`) ou par tag BCP 47 libre
(`m["fr-CH"]`).

```owl
m est une chaîneMultilingue
```

Init par défaut : dictionnaire vide.

### `langue`

Alias scalaire : référence typée vers une langue, stockée sous forme
de tag BCP 47 (`"fr-FR"`, `"en-US"`, `"zh-CN"`...). Comparable comme
une chaîne, passable en paramètre, retournable d'une fonction.

```owl
lng    est une langue
lngFR  est une langue = langueFrançais       // tag "fr-FR"
lngCH  est une langue = "fr-CH"              // tag libre accepté
```

Init par défaut : chaîne vide.

### Enum `langueXxx`

34 constantes — 30 langues principales + 4 variantes régionales
courantes.

**Convention "défaut sans suffixe = variante principale"** :

| Constante | Tag BCP 47 |
|---|---|
| `langueFrançais` | `"fr-FR"` |
| `langueAnglais` | `"en-US"` |
| `langueAllemand` | `"de-DE"` |
| `languePortugais` | `"pt-PT"` |
| `langueChinoisSimplifié` | `"zh-CN"` |

Les variantes régionales portent leur suffixe explicite :
`langueFrançaisCanada` (`"fr-CA"`),
`langueAnglaisRoyaumeUni` (`"en-GB"`),
`languePortugaisBrésil` (`"pt-BR"`),
`langueEspagnolMexique` (`"es-MX"`).

Liste complète via `owl list constants | grep langue`.

## Syntaxe

### Littéral inline `{ ... }`

Forme compacte pour initialiser une `chaîneMultilingue`. Les
séparateurs entre entrées sont **virgule** ou **newline** (mixage
OK).

```owl
m est une chaîneMultilingue = {
    langueFrançais: "Bonjour",
    langueAnglais:  "Hello",
    langueAllemand: "Hallo"
}
```

Le littéral **doit** avoir un type cible explicite : déclaration
typée, DSL `PROPRIETES` sur une propriété typée `chaîneMultilingue`, ou
argument de fonction native qui attend `chaîneMultilingue`. Pas
d'inférence depuis l'expression — c'est du typage par contexte
cible, cohérent avec le pilier "pas d'inférence implicite" du
langage.

```owl
// Refusé : pas de type cible.
Info({langueFrançais: "Bonjour"})       // erreur compile

// OK : type cible explicite.
m est une chaîneMultilingue = {langueFrançais: "Bonjour"}
Info(m)
```

### Valeurs sur plusieurs lignes

La valeur d'une entrée est une expression chaîne quelconque : les deux
formes multilignes du langage s'y emploient directement.

Bloc entre crochets — contenu brut, sans interpolation. Le `]` de
fermeture porte la ponctuation du littéral (`],` entre deux entrées,
`]}` pour la dernière).

```owl
m est une chaîneMultilingue = {
    langueFrançais: [
Ligne 1
Ligne 2
    ],
    langueAnglais: "Hello"
}
```

Chaîne entre backticks — multiligne également, avec interpolation
`[%expression%]` active.

```owl
nom est une chaîne = "Alice"
m est une chaîneMultilingue = {
    langueFrançais: `Bonjour [%nom%],
bienvenue.`,
    langueAnglais:  `Hello [%nom%],
welcome.`
}
```

Dans les deux cas, un saut de ligne du source vaut `LF`. Détail des
formes de chaîne dans `00_apercu_du_langage.md`, section « Chaînes ».

### Indexation lecture/écriture

Lecture et écriture par constante `langueXxx` ou par tag BCP 47
libre. Les deux formes sont équivalentes en interne.

```owl
m est une chaîneMultilingue = {langueFrançais: "Bonjour"}

// Lecture
sFR est une chaîne = m[langueFrançais]      // "Bonjour"
sFR2 est une chaîne = m["fr-FR"]            // "Bonjour" (même chose)

// Écriture
m[langueAnglais] = "Hello"
m["zh-CN"]       = "你好"                    // tag libre
m["fr-jura"]     = "Bondjoû"                 // patois local — accepté
```

Les tags libres permettent de stocker un patois local, une variante
régionale fine ou un identifiant custom sans modifier l'enum.

L'indexation par tag est **insensible à la casse** : les tags sont
traités en forme canonique (langue en minuscules, région en
majuscules). `m["en-gb"]`, `m["EN-GB"]` et `m[langueAnglaisRoyaumeUni]`
désignent donc la même entrée `"en-GB"`.

## Coerce auto bidirectionnel

OWL coerce automatiquement entre `chaîne` et `chaîneMultilingue`
selon le contexte d'usage. Le dev ne pose jamais d'appel de
conversion manuel.

### `chaîneMultilingue → chaîne` à l'usage

Toute fonction qui attend une `chaîne` et reçoit une
`chaîneMultilingue` résout via la **langue courante runtime**
(`Langue()`). Couvre `Info`, `Trace`, la concaténation, l'affichage
dans les champs UI, etc.

```owl
m est une chaîneMultilingue = {
    langueFrançais: "Bonjour",
    langueAnglais:  "Hello"
}
Info(m)                          // affiche "Bonjour" si Langue() = "fr-FR"
Langue(langueAnglais)
Info(m)                          // affiche "Hello"

s est une chaîne = "Salut " + m  // concat : utilise la langue courante
```

### `chaîne → chaîneMultilingue` à l'affectation

Affecter une `chaîne` à une variable typée `chaîneMultilingue`
remplit **uniquement la langue source** du projet (cf.
`LangueSource()`). Économie mémoire — le fallback gère le reste.

```owl
m est une chaîneMultilingue = "Bonjour"
// équivaut à : m = {langueFrançais: "Bonjour"}  en projet source fr-FR

Info(m[langueFrançais])          // "Bonjour"
Info(m[langueAnglais])           // "Bonjour" (fallback langue source)
```

## Fallback langue source

Lecture sur clé absente → retombe sur la **langue source du projet**
(`LangueSource()`, par défaut `"fr-FR"`). Si la langue source est
elle-même absente du dictionnaire, retourne une chaîne vide.

```owl
m est une chaîneMultilingue = {
    langueFrançais: "Bonjour",
    langueAnglais:  "Hello"
}

Info(m[langueAllemand])          // "Bonjour" — fallback fr-FR
Info(m["fr-CH"])                 // "Bonjour" — fallback fr-FR
```

Pas de cascade hiérarchique BCP 47 : un lookup miss `"fr-CH"` ne
tente **pas** `"fr"` intermédiaire avant le fallback. Comportement
simple et prévisible.

## Fonctions runtime

| Fonction | Effet |
|---|---|
| `Langue()` | Retourne le tag BCP 47 courant |
| `Langue(<langue>)` | Pose la langue courante, retourne l'ancienne valeur |
| `LangueSource()` | Retourne la langue source du projet |
| `LangueSystème()` | Détecte la langue de l'OS, retourne un tag BCP 47 |
| `LangueSensÉcriture(<langue>)` | Retourne `sensGaucheDroite` ou `sensDroiteGauche` |
| `ListeLangues()` | Retourne le tableau des langues actives du projet |

### `Langue()` — getter et setter

Sans argument, retourne la langue courante. Avec un argument
(constante `langueXxx` ou tag libre), pose la langue et retourne
l'ancienne valeur — pratique pour un switch temporaire.

```owl
cur est une chaîne = Langue()                // "fr-FR" par défaut
ancien est une chaîne = Langue(langueAnglais)
// ... section en anglais ...
Langue(ancien)                                // restaure la langue précédente
```

### `LangueSystème()` — détection OS

Détecte la langue de l'OS en cross-platform (Windows : `GetUserDefaultLocaleName`,
macOS / POSIX : `LANG`/`LC_ALL`). Retombe sur `"en-US"` si la
détection échoue (variable absente, locale C).

```owl
Langue(LangueSystème())          // aligne la langue runtime sur l'OS
```

### `LangueSensÉcriture()` et l'enum `sensÉcriture`

Retourne le sens d'écriture d'une langue : `sensDroiteGauche` (RTL)
pour les langues arabe, hébreu, persan, ourdou ;
`sensGaucheDroite` (LTR) sinon. L'enum compte une 3e valeur
`sensDéfaut` qui signifie "hérite du sens d'écriture de la langue
courante" — utile sur les propriétés `..SensÉcriture` des champs UI.

```owl
sens est un sensÉcriture = LangueSensÉcriture(langueArabe)
SI sens = sensDroiteGauche ALORS
    Trace("Langue de droite à gauche")
FIN
```

### `LangueSource()` et `ListeLangues()`

`LangueSource()` retourne la langue dans laquelle les textes ont été
saisis à l'origine. `ListeLangues()` retourne les langues actives du
projet. Les deux sont des **paramètres projet** ; une future fiche de
configuration projet rendra ces valeurs paramétrables.

## Champs UI multilingues

Plusieurs propriétés textuelles des champs UI sont **nativement
typées `chaîneMultilingue`** : titres de fenêtres, libellés et bulles
de boutons/libellés/saisies/combos/sélecteurs/interrupteurs/zones de
répétition/tables, libellés de colonnes, libellés d'états.

Le dev peut donc taper directement le dictionnaire dans
`PROPRIETES` :

```owl
FEN_Article est une Fenêtre
    PROPRIETES
        Titre = {
            langueFrançais: "Article",
            langueAnglais:  "Item",
            langueAllemand: "Artikel"
        }
    FIN

    BTN_Valider est un Champ Bouton
        PROPRIETES
            Libellé = {
                langueFrançais: "Valider",
                langueAnglais:  "Confirm"
            }
            Bulle = "Cliquez pour valider"     // chaîne simple OK aussi
        FIN
    FIN
FIN
```

La forme chaîne simple (`Bulle = "Cliquez..."`) reste acceptée : le
coerce auto remplit la langue source.

## Exemples pédagogiques

### Message d'erreur internationalisé

```owl
PROCEDURE ValiderEmail(sEmail est une chaîne)
    msgErreur est une chaîneMultilingue = {
        langueFrançais: "Adresse email invalide.",
        langueAnglais:  "Invalid email address.",
        langueAllemand: "Ungültige E-Mail-Adresse."
    }
    SI PAS Correspond(sEmail, "[a-z]+@[a-z]+\\.[a-z]+") ALORS
        Erreur(msgErreur)                     // affiché dans la langue courante
    FIN
FIN
```

### Switch langue runtime

```owl
PROCEDURE AfficherEnDeuxLangues()
    salutation est une chaîneMultilingue = {
        langueFrançais: "Bonjour le monde",
        langueAnglais:  "Hello world"
    }

    Langue(langueFrançais)
    Info(salutation)                          // "Bonjour le monde"

    Langue(langueAnglais)
    Info(salutation)                          // "Hello world"
FIN
```

### Lookup explicite ponctuel

Pour récupérer une traduction précise sans toucher à la langue
courante :

```owl
m est une chaîneMultilingue = {
    langueFrançais: "Annuler",
    langueAnglais:  "Cancel"
}

sEN est une chaîne = m[langueAnglais]         // "Cancel" — toujours
sFR est une chaîne = m[langueFrançais]        // "Annuler" — toujours
```

### Aligner OWL sur l'OS au lancement

```owl
PROCEDURE Principale()
    Langue(LangueSystème())                   // aligne le runtime sur l'OS

    SI LangueSensÉcriture(Langue()) = sensDroiteGauche ALORS
        FEN_Principale..SensÉcriture = sensDroiteGauche
    FIN

    Ouvre(FEN_Principale)
FIN
```

## Limites connues

Le multilangue OWL couvre les besoins courants. Quelques
restrictions explicites :

- **Pas de cascade hiérarchique BCP 47.** Un lookup miss `"fr-CH"`
  retombe directement sur la langue source, pas sur `"fr"`
  intermédiaire. Le dev qui veut une cascade peut la coder en OWL.
- **Pas de pluralisation native.** Le pluriel se gère en OWL
  standard : `SI n = 1 ALORS msgUn SINON msgPlusieurs FIN`.
- **`LangueSource()` et `ListeLangues()` non configurables.** Valeurs
  par défaut à `"fr-FR"` et `[langueFrançais]` ; une fiche de
  configuration projet rendra ces valeurs paramétrables.
- **Réaffectation littérale dict = remplacement.** Le littéral
  `m = {langueFrançais: "..."}` remplace **intégralement** la valeur,
  y compris après une déclaration séparée : les langues absentes du
  littéral sont écrasées. Pour ajouter ou mettre à jour une langue
  **sans écraser** les autres, utiliser l'indexation
  `m[langueFrançais] = "..."` champ par champ. La forme scalaire
  `m = "texte"` reste distincte : elle pose la langue source et
  préserve les autres traductions.

## Énum `langueXxx` — récap

Trente langues principales sans suffixe (variante "défaut" du
tableau ci-dessous) plus quatre variantes régionales explicites.

| Catégorie | Constantes |
|---|---|
| Romanes | `langueFrançais`, `langueEspagnol`, `langueItalien`, `languePortugais`, `langueRoumain` |
| Germaniques | `langueAnglais`, `langueAllemand`, `langueNéerlandais`, `langueSuédois`, `langueDanois`, `langueNorvégien` |
| Slaves | `langueRusse`, `langueUkrainien`, `languePolonais`, `langueTchèque`, `langueSlovaque`, `langueBulgare` |
| Autres européennes | `langueGrec`, `langueFinnois`, `langueTurc` |
| Asiatiques | `langueChinoisSimplifié`, `langueChinoisTraditionnel`, `langueJaponais`, `langueCoréen`, `langueHindi`, `langueThaï`, `langueVietnamien`, `langueIndonésien` |
| Sémitiques (RTL) | `langueArabe`, `langueHébreu` |
| Variantes régionales | `langueFrançaisCanada`, `langueAnglaisRoyaumeUni`, `languePortugaisBrésil`, `langueEspagnolMexique` |

`owl explain langueXxx --json` retourne la liste exhaustive avec
les tags BCP 47 associés.

---

## Pour aller plus loin

| Sujet | Référence |
|---|---|
| Rationale architectural multilangue | décision d'architecture dédiée |
| Enum comme type de variable | décision d'architecture dédiée |
| Initialisation littérale `{ ... }` | `17_init_litterale.md` |
| Système de types et coercions | `01_types_et_coercions.md` |
| Conventions de code | `10_conventions_de_code.md` |

Pour la signature exacte d'une fonction ou les valeurs d'un enum :
`owl explain <symbole>` (option `--json` pour la sortie machine).
