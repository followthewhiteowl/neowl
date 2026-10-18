# Vecteurs : le socle du calcul numérique

## 1. Le vecteur, en bref

Un `Vecteur` est une **suite de réels** rangés côte à côte, taillée pour
le calcul : géométrie, statistiques, traitement de données numériques.
Chaque élément est un `réel` (flottant double précision) ; le stockage
est **contigu** et la taille est **fixée à la construction**.

Le `Vecteur` est un **type à part entière**, distinct du
`tableau de réels`. Les deux se déclarent avec des crochets et
s'indexent en base 1, mais ils ne se mélangent pas : le tableau est une
collection ordonnée polyvalente ; le vecteur est un objet numérique qui
sait s'additionner, se normaliser, mesurer sa norme. Ce chapitre décrit
cette surface de calcul et le **contrat numérique** qui la gouverne
(voir la [section 7](#7-le-contrat-numérique)).

```owl
v est un Vecteur = [1.0, 2.0, 3.0]
Trace(v..Taille)          // 3
Trace(v[1])               // 1
```

---

## 2. Construire et accéder

Un `Vecteur` se construit par un **littéral** : une suite de valeurs
entre crochets, qui fixe aussitôt sa taille. Un élément entier est
**coercé en réel** à la construction, exactement comme une variable
`réel` affectée depuis un entier.

```owl
v est un Vecteur = [1.0, 2.0, 3.0]
vEntiers est un Vecteur = [1, 2, 3]        // chaque entier coercé en réel
r est un réel = vEntiers[1]                // 1.0
```

L'**indexation** `v[i]` lit et écrit un élément, en **base 1**. Écrire à
une position ne touche pas les autres.

```owl
v est un Vecteur = [10.0, 20.0, 30.0]
v[2] = 99.0
Trace(v[1])               // 10   — intact
Trace(v[2])               // 99   — écrit
```

Un indice **hors des bornes** lève une exception récupérable, jamais une
lecture silencieuse d'une valeur voisine :

```owl
v est un Vecteur = [1.0, 2.0, 3.0]
QUAND EXCEPTION DANS
    x est un réel = v[10]
FAIRE
    Trace("indice hors bornes")
FIN
```

Deux attributs structurels se lisent avec le double point `..`, en O(1) :

| Propriété | Rôle |
|---|---|
| `v..Taille` / `v..Length` | Nombre d'éléments |
| `v..Vide` / `v..Empty` | `Vrai` si le vecteur ne contient aucun élément |

```owl
v est un Vecteur = [1.0, 2.0, 3.0]
Trace(v..Taille)          // 3
Trace(v..Vide)            // Faux

vide est un Vecteur = []
Trace(vide..Taille)       // 0
Trace(vide..Vide)         // Vrai
```

---

## 3. Convertir depuis et vers un tableau

Le passage entre `tableau de réels` et `Vecteur` est **toujours
explicite** : aucune conversion implicite n'a lieu en cours d'expression
(voir la [section 7](#7-le-contrat-numérique)). Deux natives symétriques
assurent l'aller-retour.

| Native | Rôle |
|---|---|
| `TableauVersVecteur(tableau)` / `ArrayToVector` | Tableau de réels → Vecteur de même taille |
| `VecteurVersTableau(vecteur)` / `VectorToArray` | Vecteur → tableau de réels de même taille |

```owl
t est un tableau de réels = [1.0, 2.0, 3.0]
v est un Vecteur = TableauVersVecteur(t)
Trace(v..Taille)          // 3

sortie est un tableau de réels = VecteurVersTableau(v)
Trace(sortie[1])          // 1
```

La coercion `[...]` → `Vecteur` n'opère qu'**à la déclaration** d'une
variable typée `Vecteur`, là où la cible est explicite. Un tableau déjà
construit passe par `TableauVersVecteur` — pas de raccourci.

---

## 4. Arithmétique

Les opérateurs `+ - * /` s'appliquent à **deux vecteurs de même taille**
et opèrent **élément par élément**. Le résultat est un **nouveau
vecteur** : les opérandes ne sont jamais modifiés (immuabilité).

```owl
v1 est un Vecteur = [1.0, 2.0, 3.0]
v2 est un Vecteur = [4.0, 5.0, 6.0]

somme est un Vecteur = v1 + v2
Trace(somme[1])           // 5    — 1.0 + 4.0
Trace(v1[1])              // 1    — v1 intact
```

Combiné à un **scalaire réel**, l'opérateur diffuse l'opération sur
chaque élément (broadcast). `+` et `*` sont commutatifs avec le
scalaire.

```owl
v est un Vecteur = [1.0, 2.0, 3.0]
Trace((v * 2.0)[2])       // 4
Trace((2.0 * v)[2])       // 4    — commutatif
Trace((v / 2.0)[3])       // 1.5
```

La **négation unaire** `-v` inverse le signe de chaque élément :

```owl
v est un Vecteur = [1.0, -2.0, 3.0]
Trace((-v)[1])            // -1
Trace((-v)[2])            // 2
```

Chaque opérateur a une **méthode fluente** équivalente —
`.Ajoute` / `.Soustrait` / `.Multiplie` / `.Divise` (formes anglaises
`.Add` / `.Subtract` / `.Multiply` / `.Divide`). Elles produisent le
même nouveau vecteur, elles aussi sans muter l'opérande.

```owl
v1 est un Vecteur = [1.0, 2.0, 3.0]
v2 est un Vecteur = [4.0, 5.0, 6.0]
Trace(v1.Ajoute(v2)[1])       // 5    — identique à (v1 + v2)[1]
Trace(v1.Multiplie(v2)[3])    // 18
```

Deux vecteurs de **tailles différentes** en opération binaire lèvent une
exception.

---

## 5. Calculs vectoriels

Les opérations d'algèbre linéaire se déclinent sur **deux formes** au
choix, pour le même calcul :

| Forme | Exemple | Quand la préférer |
|---|---|---|
| **Native préfixée** | `VecteurMagnitude(v)` | Appel autonome, repérable par préfixe |
| **Méthode fluente** | `v.Magnitude()` | Enchaînement à partir d'un vecteur |

Les parenthèses signalent un **calcul** (par opposition au `..` d'un
attribut structurel, [section 2](#2-construire-et-accéder)).

| Calcul | Native / méthode | Résultat |
|---|---|---|
| Produit scalaire | `VecteurProduitScalaire(v1, v2)` / `v1.ProduitScalaire(v2)` | réel |
| Norme euclidienne | `VecteurMagnitude(v)` / `v.Magnitude()` | réel |
| Normalisation | `VecteurNormalise(v)` / `v.Normalise()` | Vecteur de norme 1 |
| Distance euclidienne | `VecteurDistance(v1, v2)` / `v1.Distance(v2)` | réel |
| Similarité cosinus | `VecteurSimilaritéCosinus(v1, v2)` / `v1.SimilaritéCosinus(v2)` | réel entre -1 et 1 |

Formes anglaises : `VectorDotProduct`, `VectorMagnitude`,
`VectorNormalize`, `VectorDistance`, `VectorCosineSimilarity`.

```owl
p est un Vecteur = [3.0, 4.0]
Trace(VecteurMagnitude(p))            // 5    — √(3² + 4²)
Trace(p.Magnitude())                  // 5    — même calcul, forme fluente

q est un Vecteur = [1.0, 0.0]
Trace(p.ProduitScalaire(q))           // 3

u est un Vecteur = [3.0, 0.0]
Trace(VecteurNormalise(u)[1])         // 1    — direction conservée, norme 1

origine est un Vecteur = [0.0, 0.0]
Trace(VecteurDistance(origine, p))    // 5
```

Les calculs qui divisent par la norme — `Normalise`,
`SimilaritéCosinus` — lèvent une exception sur un **vecteur nul**
(magnitude 0), qui n'a pas de direction définie. Les opérations binaires
exigent deux vecteurs de **même taille**.

---

## 6. Réductions

Une **réduction** condense un vecteur en une seule valeur. Elles
s'expriment en méthode fluente.

| Réduction | Rôle | Résultat |
|---|---|---|
| `v.Somme()` / `v.Sum()` | Somme des éléments | réel |
| `v.Moyenne()` / `v.Average()` | Moyenne arithmétique | réel |
| `v.Min()` | Plus petite valeur | réel |
| `v.Max()` | Plus grande valeur | réel |
| `v.ArgMax()` | **Indice** (base 1) du maximum | entier |

```owl
v est un Vecteur = [3.0, 1.0, 2.0]
Trace(v.Somme())          // 6
Trace(v.Moyenne())        // 2
Trace(v.Min())            // 1
Trace(v.Max())            // 3
Trace(v.ArgMax())         // 1    — l'indice de 3.0, pas la valeur
```

`ArgMax` renvoie l'**indice** du maximum, pas sa valeur, et la
**première** position en cas d'ex æquo.

Sur un **vecteur vide**, les réductions se comportent ainsi :

- `Somme()` et `Moyenne()` renvoient **0** ;
- `Min()`, `Max()` et `ArgMax()` **lèvent une exception** : le résultat
  n'a pas de valeur définie, aucune valeur arbitraire n'est inventée.

```owl
vide est un Vecteur = []
Trace(vide.Somme())       // 0
QUAND EXCEPTION DANS
    m est un réel = vide.Min()
FAIRE
    Trace("Min indéfini sur un vecteur vide")
FIN
```

---

## 7. Le contrat numérique

### Un élément est un `réel` IEEE

Chaque élément d'un `Vecteur` se comporte **exactement** comme un
scalaire `réel` : flottant double précision, avec l'imprécision normale
du calcul flottant. La comparaison `=` est stricte ; un résultat
irrationnel (une `Magnitude`, une `Normalise`) se compare **avec
tolérance**, jamais par égalité exacte.

Une différence assumée avec le flottant brut : **diviser par zéro lève
une exception récupérable**, plutôt que de produire `Inf` ou `NaN`. Le
`Vecteur` mure la division par zéro comme le fait déjà le scalaire
`réel`.

```owl
v est un Vecteur = [4.0, 5.0, 6.0]
avecZero est un Vecteur = [2.0, 0.0, 3.0]
QUAND EXCEPTION DANS
    resultat est un Vecteur = v / avecZero
FAIRE
    Trace("division par un élément nul")
FIN
```

### Choisir le bon type de nombre

Le `Vecteur` est fait pour la **géométrie, les statistiques et le calcul
numérique**, où le flottant IEEE est l'outil adapté et où son
imprécision est normale et attendue.

Pour un tout autre besoin — l'**argent** ou le **comptage exact**, où la
moindre erreur d'arrondi est inacceptable — ce sont les types `monétaire`
et `numérique` (décimaux exacts) qui conviennent, hors du `Vecteur`.
Chaque domaine son type : IEEE pour le calcul, décimal exact pour la
valeur comptable. Le détail des types de nombre vit dans
[`01_types_et_coercions.md`](01_types_et_coercions.md).

### Pas de mélange `tableau` ↔ `Vecteur` en expression

Un opérande `tableau` — variable typée ou littéral nu — est **refusé**
dans l'arithmétique et les opérations d'un `Vecteur`. Accepter `v + t`
ou `v + [1.0, 2.0]` en pleine expression reviendrait à **deviner** si
l'on manipule un tableau ou un vecteur ; OWL ne devine pas les types.

```owl
v est un Vecteur = [1.0, 2.0, 3.0]
t est un tableau de réels = [1.0, 2.0, 3.0]

// v + t          → exception : opérande tableau refusé
somme est un Vecteur = v + TableauVersVecteur(t)   // conversion explicite
```

Seuls `Vecteur op Vecteur` et `Vecteur op scalaire` sont légaux. Pour
combiner un tableau, le convertir explicitement
([section 3](#3-convertir-depuis-et-vers-un-tableau)) ou le déclarer
d'emblée en `Vecteur`.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Types de nombre, `réel` / `monétaire` / `numérique`, coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Opérateurs et leur sémantique | [`05_operateurs.md`](05_operateurs.md) |
| Gestion des exceptions (`QUAND EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
| Conventions de code | [`10_conventions_de_code.md`](10_conventions_de_code.md) |
