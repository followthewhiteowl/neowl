# Initialisation littérale `{ ... }`

Forme compacte pour initialiser un type composé (`Cadre`, `Police`,
`Style`, `Coin`, `Trait`) sans déclarer de variable temporaire ni
assigner les sous-propriétés une par une.

## Syntaxe

```owl
maVar est un T = {
    field1 = expression
    field2 = expression
    ...
}
```

Les séparateurs entre fields acceptés : **virgule** ou **newline**.
Mixage OK.

## Sources de type cible

Le littéral `{ ... }` **doit** avoir un type cible explicite — aucune
inférence n'est faite. Trois sources sont reconnues :

| Source | Exemple | Statut |
|---|---|---|
| Déclaration de variable | `var est un Cadre = { ... }` | ✅ |
| Affectation de variable typée | `var = { ... }` (déclarée séparément) | ✅ (remplacement) |
| Argument de fonction native | `f({ ... })` | ✅ |
| Affectation de propriété typée durée | `t..Intervalle = { ... }` | ✅ (voir « Durée par composantes ») |
| Affectation de champ de structure | `cfg.delai = { ... }` | ✅ pour un champ typé durée |
| Prop DSL dans `PROPRIETES` | `Cadre = { ... }` | ❌ non supporté — utiliser une variable séparée |
| Argument de procédure utilisateur | `f({ ... })` | ❌ non supporté |

## Récursivité

Les sous-objets (`Coin`, `Trait`, `Police`, `Cadre` dans `Style`)
acceptent leur propre littéral `{ ... }` :

```owl
monCadre est un Cadre = {
    Couleur = BleuFoncé
    CoinHautGauche = {
        Largeur = 8
        Hauteur = 4
    }
    TraitBas = {
        Couleur   = RougeFoncé
        Epaisseur = 3
        Type      = TraitTiret
    }
}
```

Le sous-type est déduit du nom du field cible (`Cadre..CoinHautGauche`
→ `Coin`, `Cadre..TraitBas` → `Trait`, `Style..Police` → `Police`,
etc.).

## Expressions complexes

Les valeurs de fields acceptent **toutes** les expressions OWL : appels
de fonctions, opérateurs, références à variables, accès chaînés.

```owl
baseEp est un entier = 2

monCadre est un Cadre = {
    Couleur   = RVB(180, 30, 30)             // appel fonction
    Epaisseur = baseEp + 1                   // arithmétique
    Bords     = BordHaut + BordBas           // combinable enum
}
```

## Durée par composantes

Le type `durée` accepte une forme littérale `{ ... }` particulière : ce
n'est pas un remplissage de champs mais une **construction par
composantes**. Les noms acceptés sont exactement les composantes
lisibles d'une durée :

| Composante | Poids |
|---|---|
| `Jour` | 86 400 000 ms |
| `Heure` | 3 600 000 ms |
| `Minute` | 60 000 ms |
| `Seconde` | 1 000 ms |
| `Milliseconde` | 1 ms |

```owl
d est une durée = { Minute = 2, Seconde = 30 }   // 150 000 ms
e est une durée = { Heure = 1, Minute = 30 }      // 5 400 000 ms
vide est une durée = { }                          // 0 ms
```

La valeur produite est la somme pondérée des composantes en
millisecondes. Un entier reste accepté en parallèle (`d est une durée =
5000` → 5 000 ms).

**Règles** :

- Les totaux d'accès en lecture (`EnSecondes`, `EnMinutes`…) ne sont
  **pas** des composantes de construction et sont rejetés.
- `Centième` n'existe pas : le grain le plus fin est `Milliseconde`.
- Une composante ne peut apparaître qu'une fois (`{ Seconde = 1, Seconde
  = 2 }` est rejeté à la compilation).
- Une composante inconnue est signalée à la compilation avec la liste
  des composantes valides.

### Affectation de propriété typée durée

À la différence des types composés, le littéral durée est aussi accepté
en **affectation de propriété** quand la propriété attend une durée
(par exemple `..Intervalle` d'un `Timer`) :

```owl
t est un Timer
t..Intervalle = { Minute = 2, Seconde = 30 }   // 150 000 ms
t..Intervalle = 5000                            // entier coercé → 5 000 ms
```

Idem pour un champ de structure typé `durée` :

```owl
ConfigTimer est une Structure
    delai est une durée
FIN

cfg est un ConfigTimer
cfg.delai = { Seconde = 3 }   // 3 000 ms
```

## Disambiguation `{ ... }` (3 contextes)

Le langage utilise déjà `{ }` à deux endroits. Le littéral d'instance
se distingue par le contexte syntaxique :

| Contexte | Forme | Sens |
|---|---|---|
| **LHS d'un statement** | `{a, b} = ...` | Indirection : assignation par nom dynamique |
| **RHS après `.`** | `obj.{expr}` | Field name dynamique |
| **RHS après `=` / `(` / `,`** | `... = { Couleur = X }` | **Init littéral d'instance** |

## Points d'attention

- **Réaffectation `var = { ... }`** : sur une variable déjà déclarée,
  le littéral **remplace** entièrement l'instance — les champs non cités
  reviennent à leur valeur par défaut (ce n'est pas une fusion).
- **Init dans `PROPRIETES`** (DSL Fenêtre / État) : passer par une
  variable séparée — déclarer `c est un Cadre = { ... }` puis
  `champ..Cadre = c`.
- **Argument de procédure utilisateur** : `maProc({ ... })` exige une
  déclaration préalable de la variable.
- **Forme préfixée par le type** (`Cadre { Couleur = X }`, style
  C / JS) : la forme avec `=` (déclaration ou affectation) est la
  seule reconnue.
- **Récursivité automatique des sous-types** : disponible sur les
  types composés du framework (`Cadre`, `Style`, `Police`, `Coin`,
  `Trait`). Pour un type utilisateur, déclarer chaque sous-objet
  explicitement.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Système de types et coercions | `01_types_et_coercions.md` |
| Cycle de vie des champs | `15_lifecycle_champs.md` |
| Layouts modernes (DSL `PROPRIETES`) | `18_layouts.md` |
| Conventions de code | `10_conventions_de_code.md` |
