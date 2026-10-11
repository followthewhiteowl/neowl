# Null et types nullables

OWL distingue explicitement deux concepts :

- **La valeur par défaut d'un type** : `""` pour une chaîne, `0` pour un
  entier, `Faux` pour un booléen, etc. C'est ce que contient une
  variable typée fraîchement déclarée sans initialisation.
- **L'absence de valeur** : représentée par le mot-clé `Null`. Une
  variable ordinaire ne peut **pas** contenir `Null` ; seules les
  variables marquées explicitement nullables le peuvent.

Cette séparation rend les programmes prévisibles : `n est un entier`
garantit que `n` contient un entier, jamais `Null`. Pour autoriser
l'absence, il faut le déclarer.

---

## 1. La valeur `Null`

`Null` est un mot-clé du langage. La comparaison `=` ne reconnaît
l'égalité qu'entre deux `Null` :

```owl
Trace(Null = 0)        // Faux
Trace(Null = "")       // Faux
Trace(Null = Faux)     // Faux
Trace(Null = Null)     // Vrai
```

Pour tester si une variable contient `Null`, deux formes équivalentes :

```owl
SI EstNul(v) ALORS …      // forme explicite, préférée
SI v = Null ALORS …       // forme directe
```

`EstNul(v)` est plus lisible et signale clairement l'intention. Sur une
variable non-nullable les deux renvoient toujours `Faux` (cf. section
suivante).

---

## 2. Variables non-nullables (par défaut)

Sans marqueur particulier, une variable typée **ne peut pas** contenir
`Null`. À la déclaration sans initialisation, elle reçoit la **valeur
par défaut de son type** :

| Type | Valeur par défaut |
|---|---|
| `chaîne` | `""` |
| `entier`, `réel`, `monétaire`, `numérique`, `durée` | `0` |
| `booléen` | `Faux` |
| `date`, `heure`, `dateheure` | instant courant |
| `buffer` | buffer vide |
| `tableau de <T>` | tableau vide |
| Structure user-defined | clone du template, champs en valeur par défaut |

```owl
sNom est une chaîne
Trace("[", sNom, "]")        // []
Trace(EstNul(sNom))           // Faux

nAge est un entier
Trace(nAge)                   // 0
```

**Affecter `Null` après coup** sur une variable non-nullable est
accepté : c'est un sucre qui **reset à la valeur par défaut**. La
variable n'est jamais réellement `Null` après.

```owl
sNom est une chaîne = "Alice"
sNom = Null
Trace("[", sNom, "]")         // []   — reset à ""
Trace(EstNul(sNom))           // Faux
```

---

## 3. Variables nullables avec `?`

Pour qu'une variable puisse représenter l'absence de valeur de façon
distincte de sa valeur par défaut, on l'annote avec `?` après le type :

```owl
sCommentaire est une chaîne?
Trace(EstNul(sCommentaire))   // Vrai   — non initialisée = Null

sCommentaire = "RAS"
Trace(EstNul(sCommentaire))   // Faux

sCommentaire = Null
Trace(EstNul(sCommentaire))   // Vrai

sCommentaire = ""             // chaîne vide, distincte de Null
Trace(EstNul(sCommentaire))   // Faux
```

Cas d'usage canonique : un champ de formulaire optionnel où l'on veut
distinguer « pas saisi » de « saisi puis effacé ».

```owl
sNote est une chaîne?

SI EstNul(sNote) ALORS
    // jamais saisi
SINON SI sNote = "" ALORS
    // saisi puis effacé
SINON
    // saisie réelle
FIN
```

Sans le `?`, les trois cas seraient indiscernables : la variable
vaudrait `""` partout.

---

## 4. Containers et structures

Un tableau typé ne contient que des valeurs du type — jamais `Null`. Les
cellules créées par extension reçoivent la valeur par défaut :

```owl
tabNoms est un tableau de 3 chaînes
Trace("[", tabNoms[1], "]")   // []  — pas Null

tabScores est un tableau d'entiers
tabScores[5] = 100
Trace(tabScores[2])           // 0  — extension auto en valeur par défaut
```

Pour des éléments nullables, typer la collection en `tableau de chaînes?`.

Les champs scalaires d'une structure user-defined suivent la même règle :
valeur par défaut au moment du clone, sauf champ explicitement annoté
nullable.

```owl
STPersonne est une Structure
    sNom est une chaîne
    sNoteOptionnelle est une chaîne?
FIN

stClient est un STPersonne
Trace(EstNul(stClient.sNom))           // Faux  — "" par défaut
Trace(EstNul(stClient.sNoteOptionnelle)) // Vrai
```

---

## 5. Récapitulatif

| Situation | Résultat |
|---|---|
| Variable non-nullable sans init | Valeur par défaut du type |
| Variable non-nullable `= Null` (init ou après) | Reset à la valeur par défaut |
| Variable nullable (`?`) sans init | `Null` |
| Variable nullable `= Null` | `Null` |
| `Null = Null` | `Vrai` |
| `Null = <valeur typée>` | `Faux` |
| `EstNul(v)` sur valeur par défaut typée | `Faux` |

---

## 6. Erreurs courantes

- **Confondre chaîne vide et `Null`** : `""` est une valeur, distincte de
  `Null`. Utiliser `s = ""` pour tester la première, `EstNul(s)` pour la
  seconde.
- **Initialiser à la valeur par défaut sans raison** : `nAge est un entier
  = 0` redouble inutilement le défaut. Écrire `nAge est un entier`.
- **Supposer qu'un champ de structure peut « manquer »** : non, sauf si
  annoté `?`. Sinon il contient toujours la valeur par défaut du type.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Système de types détaillé | `01_types_et_coercions.md` |
| Conventions de code | `10_conventions_de_code.md` |
