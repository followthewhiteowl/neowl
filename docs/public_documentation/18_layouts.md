# Layouts modernes

Le langage propose un modèle de layout sémantique inspiré des
frameworks modernes (Flutter, SwiftUI, Jetpack Compose, CSS
Flexbox/Grid). En mode standard, les enfants d'un conteneur sont
positionnés par règles d'alignement et d'étirement — `X` / `Y`
ne sont pas requis. Un mode pixel-perfect `dispLibre` reste
disponible en opt-in pour les cas qui le justifient (cf. § 6).

Mots-clés concernés : `Disposition`, `Conteneur`, `Étire`, `Alignement`,
`Marge`, `Padding`, `Espacement`, `AlignH`, `AlignV`, `Colonnes`, `Lignes`,
`ColSpan`, `RowSpan`, `LargeurMin/Max`, `HauteurMin/Max`.

## 1. Concept

Tout conteneur (Fenêtre ou `Conteneur`) a une propriété **`Disposition`**
qui décide comment ses enfants directs sont organisés :

| Valeur            | Effet | Équivalent CSS |
|-------------------|-------|----------------|
| `dispVerticale`   | Empile en colonne (par défaut) | `flex-direction: column` |
| `dispHorizontale` | Aligne en ligne | `flex-direction: row` |
| `dispGrille`      | Grille à `Colonnes` × `Lignes` | `display: grid` |
| `dispLibre`       | Pas de layout — `X` / `Y` absolus | `position: absolute` |

**Règle fondamentale** : `X` et `Y` sont **interdits** sur un champ
enfant **sauf** si son parent direct est en `Disposition = dispLibre`.
Le compilateur vérifie cette règle et émet une erreur sinon — l'écriture
des interfaces reste responsive par défaut.

`X` et `Y` sur la **Fenêtre** elle-même restent valides — ils déterminent la
position de la fenêtre à l'écran (`-1` = centrée).

**Important** : si on ne précise pas `Disposition` sur la Fenêtre, le
défaut est **`dispVerticale`**. Les enfants avec `X`/`Y` produiront alors
une **erreur de compilation**. Pour le mode pixel-perfect explicite,
ajouter `Disposition = dispLibre` au PROPRIETES de la Fenêtre.

## 2. Le champ `Conteneur`

`Conteneur` est une boîte de groupage générique qui sert à imbriquer
des layouts. Équivalent moderne du `<div>` HTML ou du `View` React
Native.

```owl
FEN_Form est une Fenêtre
    PROPRIETES
        Disposition = dispVerticale
        Padding     = 12
        Espacement  = 8
    FIN

    LIB_Titre est un Champ Libellé
        PROPRIETES
            Libellé = "Saisir l'utilisateur"
        FIN
    FIN

    SAI_Nom est un Champ Saisie
        PROPRIETES
            Étire = Vrai
        FIN
    FIN

    BAR_Boutons est un Champ Conteneur
        PROPRIETES
            Disposition = dispHorizontale
            AlignH      = alignFin
        FIN

        BTN_OK est un Champ Bouton
            PROPRIETES
                Libellé = "OK"
            FIN
        FIN

        BTN_Annuler est un Champ Bouton
            PROPRIETES
                Libellé = "Annuler"
            FIN
        FIN
    FIN
FIN
```

Trois layouts imbriqués : la Fenêtre verticale, un Conteneur horizontal pour
les boutons. Aucun `X`/`Y`. Lisible, redimensionnable.

## 3. Propriétés conteneur

| Prop | Type | Défaut | Description |
|------|------|--------|-------------|
| `Disposition`  | enum  | `dispVerticale` | Mode de layout |
| `Padding`      | int   | 0 | Marge interne uniforme (px) |
| `PaddingHaut/Bas/Gauche/Droite` | int | hérite `Padding` | Override par côté |
| `Espacement`   | int   | 6 | Gap entre enfants successifs |
| `AlignH`       | enum  | (libre) | Alignement horizontal par défaut des enfants |
| `AlignV`       | enum  | (libre) | Alignement vertical par défaut |
| `Colonnes`     | int   | 1 | Nombre de colonnes (`dispGrille` uniquement) |
| `Lignes`       | int   | calculé | Nombre de lignes (optionnel) |

Valeurs `AlignH/AlignV` : `alignDébut`, `alignCentre`, `alignFin`, `alignÉtire`.

## 4. Propriétés enfant

Tout champ (Libellé/Bouton/Saisie/Table/Conteneur) accepte ces props quand
son parent est en mode layout :

| Prop | Type | Défaut | Description |
|------|------|--------|-------------|
| `Étire`        | bool  | Faux | Prend toute la place restante sur l'axe principal |
| `Marge`        | int   | 0 | Marge externe uniforme |
| `MargeHaut/Bas/Gauche/Droite` | int | hérite `Marge` | Override par côté |
| `Alignement`   | enum  | hérite parent | Override de l'alignement transverse pour ce champ |
| `ColSpan`      | int   | 1 | Fusion de colonnes en `dispGrille` |
| `RowSpan`      | int   | 1 | Fusion de lignes |
| `Largeur`      | int   | (auto) | Largeur figée ; contraint la taille sur l'axe principal en `dispHorizontale` |
| `Hauteur`      | int   | (auto) | Hauteur figée ; contraint la taille sur l'axe principal en `dispVerticale` |
| `LargeurMin/Max` | int | -1 | Bornes de largeur |
| `HauteurMin/Max` | int | -1 | Bornes de hauteur |

Une `Hauteur` (ou `Largeur`) figée sur un enfant fixe sa taille le long de
l'axe principal du parent : en `dispVerticale`, `Hauteur` verrouille la
hauteur du champ ; en `dispHorizontale`, `Largeur` verrouille sa largeur.
Sur l'axe transverse, la taille reste régie par l'alignement (`alignÉtire`
remplit toute la dimension transverse, les autres valeurs prennent la taille
naturelle). Combiner `Hauteur`/`Largeur` figée avec `HauteurMin/Max` /
`LargeurMin/Max` pour borner une taille sans la figer complètement.

Ces propriétés sont des **valeurs déclarées**. Une fois la fenêtre montée,
`..Largeur` / `..Hauteur` / `..X` / `..Y` d'un champ se lisent au runtime et
rendent sa **géométrie réelle**, qui peut différer de ce qui est déclaré
(champ étiré, taille calculée par la disposition). La lecture de cette
géométrie réelle est détaillée dans `15_lifecycle_champs.md` §6ter, et son
**écriture** (taille contrainte-persistante, position refusée en
disposition automatique) dans `15_lifecycle_champs.md` §6quater.

## 5. Distribution de l'espace sur l'axe principal

Par défaut, chaque enfant prend sa taille naturelle (ou la taille figée
imposée par `Hauteur`/`Largeur`) et les enfants **s'empilent au début** de
l'axe principal :

- en `dispVerticale`, les enfants s'empilent **en haut** ; l'excédent
  vertical reste vide en bas ;
- en `dispHorizontale`, les enfants s'empilent **à gauche** ; l'excédent
  horizontal reste vide à droite.

Pour consommer cet excédent, marquer un enfant `Étire = Vrai` : ce champ
absorbe alors tout l'espace restant sur l'axe principal (un `Conteneur`
`Étire = Vrai` s'étend jusqu'à remplir la zone). Plusieurs enfants
`Étire = Vrai` se partagent l'excédent à parts égales.

`alignÉtire` ne joue **pas** le même rôle : c'est un alignement **transverse**.
Il fait remplir la dimension perpendiculaire à l'axe principal (la largeur en
`dispVerticale`, la hauteur en `dispHorizontale`) **sans** consommer d'espace
le long de l'axe principal. Pour qu'un champ prenne la place restante dans le
sens du flux, utiliser `Étire = Vrai` ; pour qu'il occupe toute la largeur (ou
hauteur) transverse, utiliser `alignÉtire`.

```owl
FEN_Empilement est une Fenêtre
    PROPRIETES
        Disposition = dispVerticale
    FIN

    LIB_Haut est un Champ Libellé
        PROPRIETES
            Libellé = "En haut, taille naturelle"
        FIN
    FIN

    SAI_Milieu est un Champ Saisie
        PROPRIETES
            Alignement = alignÉtire
        FIN
    FIN
FIN
```

`LIB_Haut` et `SAI_Milieu` se collent en haut de la fenêtre ; `SAI_Milieu`
occupe toute la largeur grâce à `alignÉtire`, mais l'espace vertical
excédentaire reste vide en bas (aucun enfant n'est `Étire = Vrai`).

## 6. Quelques cas d'usage

### Liste extensible (élément qui prend la place restante)

```owl
FEN_Liste est une Fenêtre
    PROPRIETES
        Disposition = dispVerticale
    FIN

    BAR_Filtre est un Champ Conteneur
        PROPRIETES
            Disposition = dispHorizontale
        FIN
    FIN

    TBL_Lignes est un Champ Table
        PROPRIETES
            Étire = Vrai
        FIN
    FIN
FIN
```

`Étire = Vrai` sur `TBL_Lignes` consomme l'espace restant entre la
barre de filtre (taille fixe) et le bas de la fenêtre.

### Grille (calculette)

```owl
FEN_Calc est une Fenêtre
    PROPRIETES
        Disposition = dispGrille
        Colonnes    = 4
        Espacement  = 4
    FIN

    BTN_0 est un Champ Bouton
        PROPRIETES
            Libellé = "0"
            ColSpan = 2
        FIN
    FIN

    BTN_Dot est un Champ Bouton
        PROPRIETES
            Libellé = "."
        FIN
    FIN

    BTN_Eq est un Champ Bouton
        PROPRIETES
            Libellé = "="
        FIN
    FIN
FIN
```

Les autres boutons (1-9, opérateurs) suivent la même structure ; le
moteur place chaque champ dans la cellule libre suivante en
respectant `ColSpan`.

### Mode pixel-perfect (opt-in)

Quand l'usage le justifie (badge décoratif, écran de garde fait
exactement au pixel, champ flottant) :

```owl
FEN_Carte est une Fenêtre
    PROPRIETES
        Disposition = dispLibre
        Largeur     = 300
        Hauteur     = 200
    FIN

    LIB_Badge est un Champ Libellé
        PROPRIETES
            X       = 240
            Y       = 10
            Libellé = "NEW"
        FIN
    FIN
FIN
```

`dispLibre` est un opt-in explicite et **ne devrait pas** être le
défaut.

## 7. Cas particulier : États

Les États (rapports paginés A4 / A5) **ne suivent pas** ce modèle de
layout. Ils conservent un système `X` / `Y` absolu organisé en bandes
(EnTêteDocument, HautDePage, Corps, BasDePage, FinDeDocument) qui
correspond mieux aux contraintes de l'éditique PDF.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Cycle de vie des champs | `15_lifecycle_champs.md` |
| Cycle de vie des États (bandes, modèle `X`/`Y`) | `16_lifecycle_etats.md` |
| Groupes de champs (broadcast d'une propriété) | `20_groupes_de_champs.md` |
| Texte riche (Markdown étendu pour libellés) | `19_texte_riche.md` |
| Conventions de code | `10_conventions_de_code.md` |
