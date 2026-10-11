# Couleurs

Une couleur se déclare de plusieurs façons interopérables et se manipule
comme une valeur ordinaire. Le type composé `Couleur` (forme anglaise
`Color`) porte les composantes ainsi que le canal de transparence
(`Opacité` / `Opacity`).

## Les quatre formes de déclaration

Les quatre formes ci-dessous produisent toutes une valeur affectable à une
propriété couleur (`CouleurFond`, `Couleur`, `CouleurJauge`…).

### 1. Constante nommée

```owl
c est une Couleur = Rouge
```

Un catalogue de couleurs prédéfinies est disponible : `Blanc` / `White`,
`Noir` / `Black`, `Rouge` / `Red`, `Vert` / `Green`, `Bleu` / `Blue`,
`JauneOr` / `Gold`, les déclinaisons `…Clair` / `Light…`, `…Foncé` /
`Dark…`, `…Pastel` / `Pastel…`, ainsi que `Transparent`. Le catalogue
comprend également des constantes sentinelles (`CouleurDéfaut` / `DefaultColor`,
`iSystème` / `iSystem`) décrites dans « Couleurs sentinelles » ci-dessous.

### 2. `RVB(rouge, vert, bleu)` / `RGB(red, green, blue)`

```owl
c est une Couleur = RVB(255, 128, 0)
```

Chaque composante va de 0 à 255. La forme anglaise `RGB` est équivalente.

### 3. Entier compact

```owl
c est une Couleur = 33023
```

La valeur entière encode les trois composantes : `rouge + vert * 256 +
bleu * 65536`. L'exemple ci-dessus est donc identique à `RVB(255, 128, 0)`
(`255 + 128 * 256 = 33023`).

### 4. Littéral inline `{ ... }`

```owl
c est une Couleur = { Rouge = 255, Vert = 128, Bleu = 0, Opacité = 200 }
```

Seule cette forme permet de fixer l'opacité au moment de la déclaration.
Les champs absents prennent leur valeur par défaut (`Opacité` vaut 255,
soit opaque). Cf. `17_init_litterale.md` pour la syntaxe générale du
littéral d'instance.

## Couleurs sentinelles (repli par défaut)

Au-delà des couleurs à valeur fixe, le catalogue expose des constantes
**sentinelles** : elles ne portent pas de composantes RVB figées mais
indiquent un mode de rendu. Le rendu final dépend alors de la propriété à
laquelle la sentinelle est affectée.

| Constante (FR) | Forme EN | Rôle |
|---|---|---|
| `CouleurDéfaut` | `DefaultColor` | La propriété reprend son propre défaut de rendu |
| `iSystème` | `iSystem` | La propriété reprend la couleur du système / thème |
| `Transparent` | `Transparent` | Aucun fond peint : ce qui est derrière transparaît |

### Repli par propriété, pas couleur unique partagée

`CouleurDéfaut` signifie « rends cette propriété avec **son** défaut », et
non « telle couleur précise ». Le repli est donc calculé **par propriété** :
deux propriétés différentes affectées à `CouleurDéfaut` ne prennent pas la
même couleur, chacune retombe sur le défaut qui lui est propre.

```owl
LIB_Carte..CouleurFond = CouleurDéfaut   // fond par défaut du contrôle
LIB_Carte..Couleur     = CouleurDéfaut   // couleur de texte par défaut
```

Dans cet exemple, `CouleurFond` (le fond) et `Couleur` (le texte) ne
deviennent **pas** identiques : le fond reprend le fond par défaut du
contrôle, le texte reprend la couleur de texte par défaut. Il n'existe pas
de couleur unique commune vers laquelle `CouleurDéfaut` ferait converger
toutes les propriétés.

`iSystème` suit le même principe de repli par propriété, en déléguant le
choix au système ou au thème actif plutôt qu'au défaut du contrôle.

## Le type `Couleur`

Le type expose huit propriétés, toutes entières. La forme anglaise de
chaque nom est indiquée pour rappel (les deux formes sont acceptées).

| Propriété (FR) | Forme EN | Plage | Rôle |
|---|---|---|---|
| `Rouge` | `Red` | 0–255 | Composante rouge |
| `Vert` | `Green` | 0–255 | Composante verte |
| `Bleu` | `Blue` | 0–255 | Composante bleue |
| `Teinte` | `Hue` | 0–240 | Teinte |
| `Saturation` | `Saturation` | 0–240 | Saturation |
| `Luminosité` | `Luminance` | 0–240 | Luminosité |
| `Opacité` | `Opacity` | 0–255 | Canal alpha : 0 = transparent, 255 = opaque |
| `Couleur` | `Color` | entier | Valeur entière compacte (propriété par défaut) |

`Opacité` est indépendante de `Couleur` (qui n'encode que les trois
composantes visibles). Affecter un entier ou une constante nommée à une
variable `Couleur` décompose automatiquement les composantes ; l'opacité
reste alors à 255.

### Mutation

Les propriétés se lisent et s'écrivent après déclaration :

```owl
c est une Couleur = RVB(255, 128, 0)
c..Opacité = 128           // devient semi-transparente
c..Rouge   = 200           // ajuste une composante
```

## Fond translucide (glassmorphisme)

Poser une `Couleur` d'`Opacité` inférieure à 255 sur une propriété de fond
rend ce fond translucide : ce qui se trouve derrière transparaît. C'est le
principe du glassmorphisme (surface « verre dépoli »).

```owl
FEN_Glass est une Fenêtre
    PROPRIETES
        Titre       = "Fond translucide"
        Largeur     = 360
        Hauteur     = 220
        Disposition = dispLibre
        CouleurFond = Rouge
    FIN

    // Blanc à 50 % d'opacité : le fond rouge transparaît → rose.
    verreClair est une Couleur = { Rouge = 255, Vert = 255, Bleu = 255, Opacité = 128 }

    LIB_Carte est un Champ Libellé
        PROPRIETES
            X       = 40
            Y       = 40
            Largeur = 280
            Hauteur = 120
            Libellé = "Carte translucide"
        FIN

        LIB_Carte..CouleurFond = verreClair
    FIN
FIN
```

Un fond opaque (opacité 255) reste rendu à l'identique : la transparence
n'entre en jeu qu'en deçà de 255.

## Limites actuelles

Deux limites sont à connaître pour éviter les mauvaises surprises.

### Le littéral `{ ... }` exige une variable nommée

Le littéral inline de couleur n'est reconnu qu'en **déclaration typée**
(`c est une Couleur = { ... }`). Il n'est **pas** accepté directement dans
une affectation de propriété couleur, ni imbriqué dans un autre littéral :

```owl
// ✗ non supporté
LIB_Carte..CouleurFond = { Rouge = 255, Opacité = 128 }
monStyle est un Style = { CadreExtérieur = { CouleurFond = { Rouge = 255, Opacité = 128 } } }

// ✓ contournement : passer par une variable Couleur nommée
verre est une Couleur = { Rouge = 255, Opacité = 128 }
LIB_Carte..CouleurFond = verre
```

### Périmètre de l'opacité des fonds

L'opacité d'un fond translucide est appliquée sur : la fenêtre, un
conteneur (via son Style), un contrôle générique, les briques de saisie et
de libellé, et les dégradés.

Elle n'est **pas encore** prise en compte sur : les lignes de table
(paires / impaires), la ligne sélectionnée, et les coches / puces. Sur ces
surfaces, une couleur translucide est traitée comme opaque.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Initialisation littérale `{ ... }` | `17_init_litterale.md` |
| Cycle de vie des champs | `15_lifecycle_champs.md` |
| Système de types et coercions | `01_types_et_coercions.md` |
