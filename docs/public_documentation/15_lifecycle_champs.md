# Cycle de vie d'une fenêtre

Le code d'une fenêtre est dispatché en plusieurs endroits qui s'exécutent
à des moments différents : le body, `FinInitialisation`, les
procédures-événements des sous-champs, `AutorisationFermeture`,
`Fermeture`. Cette fiche explique quoi mettre où, et dans quel ordre
ça tire.

---

## 1. Anatomie d'une déclaration

Une fenêtre est déclarée dans **son propre fichier** : un fichier
source ne peut contenir qu'une seule `est une Fenêtre`. Une seconde
déclaration de fenêtre dans le même fichier est refusée à la
compilation.

Le body d'une `est une Fenêtre` n'accepte que certaines constructions :

| Construction | Effet |
|---|---|
| `PROPRIETES ... FIN` | Propriétés statiques de la fenêtre (titre, taille, ...) |
| `CONSTANTE ... FIN` | Constantes hoistées dans la portée |
| `nom est un Type [= expr]` | Variable portée par la fenêtre (cf. §2) |
| `NOM est un Champ <Kind>` | Déclaration d'un sous-champ |
| `PROCEDURE` / `FONCTION` | Handlers d'événement, hooks lifecycle, helpers |

Ce qui n'entre dans aucune de ces catégories est traité différemment :

- Un appel (`Foo()`), un `Trace(...)`, un accès à un sous-champ
  (`LIB_x..Libellé = "y"`) produit un **warning** *« Construction
  inconnue dans le body de fenetre. Skip ligne. »* — la ligne est
  ignorée, la compilation continue.
- Une **affectation nue** comme `n = 5` est interprétée comme une
  tentative de **propriété de fenêtre** et produit une **erreur
  compile** *« Propriete 'n' inconnue pour Fenetre. »*

Pour exécuter du code à l'ouverture, utiliser `FinInitialisation`
(cf. §3).

---

## 2. Variables au top-level du body

Les variables déclarées au top-level d'une fenêtre sont **portées par la
fenêtre**, pas globales au programme. Leur expression d'initialisation
s'évalue **à chaque ouverture** (le top-level *est* le code
d'initialisation). Depuis l'extérieur, on y accède de façon **qualifiée**,
`FEN_X.maVariable`, une fois la fenêtre ouverte — jamais par leur nom nu.

```owl
FEN_Compte est une Fenêtre
    PROPRIETES
        Titre = "Compte"
        Disposition = dispLibre
    FIN

    nbTentatives est un entier = 0

    SAI_Login est un Champ Saisie
        PROPRIETES
            X = 10
            Y = 10
        FIN
        PROCEDURE surValidation()
            nbTentatives = nbTentatives + 1   // nom nu : interne à la fenêtre
        FIN
    FIN
FIN

Ouvre(FEN_Compte)
Trace("tentatives : " + FEN_Compte.nbTentatives)   // qualifié, après ouverture
```

À l'intérieur de la fenêtre (top-level, sous-champs, procédures), la
variable s'utilise par son **nom nu**. Depuis le script, seul l'accès
**qualifié** est valide, et seulement après `Ouvre` : accéder à la
variable alors que la fenêtre n'est pas ouverte **lève une exception** —
la fenêtre doit être ouverte avant tout accès depuis l'extérieur.

Conséquence : ré-ouvrir une fenêtre **ré-initialise** ses variables
top-level (le code d'init re-tourne à chaque ouverture). Pour conserver un
état entre deux ouvertures, le stocker hors de la fenêtre.

### Confiner une variable au flux d'init : `LOCAL`

Par défaut, une variable top-level est **portée par la fenêtre** (visible
de ses sous-champs et procédures par leur nom nu). Pour un temporaire dont
seul le code d'initialisation a besoin — un index de boucle, un
accumulateur — la bascule `LOCAL` la **confine au flux top-level** : elle
n'est alors ni portée par la fenêtre, ni visible des procédures, ni
accessible depuis l'extérieur.

`GLOBAL` et `LOCAL` se posent **seuls sur leur ligne** et basculent le mode
des déclarations qui suivent, jusqu'à la bascule inverse — il n'y a pas de
bloc à refermer. `GLOBAL` rétablit le défaut (variable portée par la
fenêtre). Les deux s'alternent librement.

```owl
FEN_Liste est une Fenêtre
    PROPRIETES
        Titre = "Liste"
    FIN

    nbLignes est un entier = 0        // défaut : portée par la fenêtre

    LOCAL
    i est un entier = 0               // confiné au flux d'init
    TANTQUE i < 10
        nbLignes = nbLignes + 1
        i = i + 1
    FIN

    GLOBAL
    total est un entier = nbLignes    // de nouveau porté par la fenêtre
FIN
```

Ici `nbLignes` et `total` sont accessibles en `FEN_Liste.nbLignes` après
ouverture ; `i`, confiné, ne l'est pas.

---

## 3. Ouverture : `Ouvre(FEN_X)`

Une fenêtre déclarée ne s'affiche pas toute seule. C'est un appel
d'ouverture qui la **monte** (l'instancie et la rend active). Trois
formes existent :

| Appel | Modale | Effet |
|---|---|---|
| `Ouvre(FEN_X)` | Oui | Bloque l'appelant jusqu'à fermeture. Renvoie la valeur passée à `Ferme`. |
| `OuvreFille(FEN_X)` | Non | Ouvre en parallèle et rend la main immédiatement. |
| `OuvreSoeur(FEN_X)` | Non | Idem `OuvreFille`, sans lien de parenté avec la fenêtre courante. |

**Principe à retenir : un champ vit dans une fenêtre montée.** Tant
qu'une fenêtre n'a pas été ouverte, ses sous-champs n'existent pas en
mémoire. Lire ou écrire `SAI_Login..Valeur` avant d'avoir ouvert sa
fenêtre est une **erreur** : il n'y a pas d'instance où chercher la
valeur. On ouvre d'abord, on accède ensuite.

La suite de cette section décrit `Ouvre`. Le multi-instance des formes
non-modales est traité en §5.

`Ouvre(FEN_X)` déclenche l'ouverture modale. La séquence est :

1. Instanciation des **sous-champs** dans l'ordre lexical du body.
2. Application des **propriétés** de chaque sous-champ.
3. Affichage de la fenêtre si `Visible = Vrai` (défaut).
4. Appel de **`FinInitialisation()`** si elle est déclarée.
5. **Boucle d'événements** : les handlers (`surClic`, `surValidation`, ...) sont
   appelés à chaque interaction utilisateur jusqu'à fermeture.

`FinInitialisation` est la **place canon** pour ce qui doit s'exécuter
une fois la fenêtre prête : configuration dynamique des sous-champs,
chargement initial, focus, etc.

```owl
PROCEDURE FinInitialisation()
    LIB_Titre..Libellé = "Bonjour " + sUtilisateurCourant
    SAI_Login..Valeur  = sDernierLogin
FIN
```

Avant l'appel de `FinInitialisation`, les sous-champs existent en
mémoire mais leurs propriétés ne sont que celles déclarées dans
`PROPRIETES`. Toute configuration dynamique va ici.

> Le nom `Initialisation` n'est pas une procédure de cycle de vie : le
> compilateur le rejette et invite à placer le code d'initialisation dans
> `FinInitialisation` ou directement dans le corps de l'élément.

### Sous `owl test` : mode headless

Sous le runner `owl test`, la constante `TestModeHeadless` est activée
par défaut. Dans ce mode, `Ouvre`, `OuvreFille` et `OuvreSoeur`
court-circuitent la boucle d'événements : la fenêtre est montée en
mémoire, `FinInitialisation` s'exécute, puis l'appel rend la main sans
bloquer. Les scénarios de test peuvent donc instancier une fenêtre,
lire/écrire ses champs et appeler `Ferme()` sans déclencher d'UI
graphique. En exécution normale (`owl <fichier.owl>`), la boucle
d'événements est active et `Ouvre` est bloquante comme décrit
ci-dessus.

### Paramètres de fenêtre

Une fenêtre peut déclarer des **paramètres sur sa première ligne**, entre
parenthèses après `Fenêtre`, exactement comme un État :

```owl
FEN_Fiche est une Fenêtre(idClient est un entier, titre est une chaîne)
    PROPRIETES
        Titre = "Fiche client"
        Disposition = dispLibre
    FIN

    // idClient et titre sont disponibles ici, par leur nom nu, dans tout
    // le scope de la fenêtre : top-level, sous-champs, events et procédures.
    Trace("ouverture fiche " + idClient)

    PROCEDURE FinInitialisation()
        LIB_Titre..Libellé = titre
    FIN
FIN
```

L'annotation de type (`est un entier`) est **optionnelle** : `Fenêtre(message,
priorite est un entier)` mélange un paramètre sans type et un paramètre typé.

Les valeurs sont transmises à l'ouverture, après le nom de la fenêtre :

```owl
Ouvre(FEN_Fiche, 42, "Détails")   // idClient = 42, titre = "Détails"
```

L'injection a lieu **avant** l'exécution du code top-level, donc le top-level
et `FinInitialisation` lisent déjà les valeurs reçues. Les trois formes
d'ouverture (`Ouvre`, `OuvreFille`, `OuvreSoeur`) acceptent ces arguments.

Contrats sur le nombre d'arguments :

- **Moins d'arguments que de paramètres** : les paramètres non fournis
  restent à `Null` (`Ouvre(FEN_Fiche)` laisse `idClient` et `titre` à `Null`).
- **Plus d'arguments que de paramètres** : le surplus est ignoré sans erreur.

---

## 4. Fermeture : `Ferme([FEN_X] [, valeur])`

Trois déclencheurs possibles : croix de la fenêtre, raccourci système,
appel explicite `Ferme()` (avec optionnellement une valeur de
retour pour `Ouvre`). `Ferme()` sans argument, ou `Ferme(FEN_X)`,
ferme la **fenêtre courante** — la dernière ouverte (cf. §5 sur la
pile). La séquence est :

1. **`AutorisationFermeture()`** est appelée si déclarée. Elle peut
   **annuler** la fermeture en renvoyant `Faux`. Tout autre retour (ou
   absence de `RENVOYER`) autorise la suite.
2. **`Fermeture()`** est appelée si déclarée. Dernière chance d'observer
   la fenêtre vivante (cleanup, log, sauvegarde). **Ne peut pas annuler**.
3. La fenêtre et ses sous-champs sont détruits.

```owl
PROCEDURE AutorisationFermeture()
    SI bModificationsNonSauvegardees ALORS
        RENVOYER Faux                  // annule la fermeture
    FIN
    // pas de RENVOYER → fermeture autorisée
FIN

PROCEDURE Fermeture()
    Trace("FEN_Compte fermee, " + nbTentatives + " tentatives.")
FIN
```

---

## 5. Plusieurs fenêtres : pile et multi-instance

Les formes non-modales `OuvreFille` et `OuvreSoeur` n'attendent pas la
fermeture : elles rendent la main tout de suite. On peut donc avoir
**plusieurs fenêtres ouvertes en même temps**.

### Pile de fenêtres

Les fenêtres ouvertes s'empilent. La **dernière ouverte est la fenêtre
courante** : c'est elle que visent les accès aux champs et c'est elle
que `Ferme()` ferme. Une fois fermée, la précédente redevient courante.

```owl
OuvreFille(FEN_Liste)     // FEN_Liste courante
OuvreFille(FEN_Fiche)     // FEN_Fiche courante (empilée par-dessus)
Ferme()                   // ferme FEN_Fiche ; FEN_Liste redevient courante
Ferme()                   // ferme FEN_Liste
```

### Plusieurs instances de la même fenêtre

Chaque ouverture crée une **instance indépendante**. Ouvrir deux fois
la même fenêtre donne deux instances distinctes, chacune avec son
propre état : modifier un champ dans l'une n'affecte pas l'autre.

```owl
FEN_Fiche est une Fenêtre
    PROPRIETES
        Titre       = "Fiche client"
        Disposition = dispLibre
    FIN

    SAI_Nom est un Champ Saisie
        PROPRIETES
            X = 10
            Y = 10
        FIN
    FIN
FIN

OuvreSoeur(FEN_Fiche)              // instance A (courante)
SAI_Nom..Valeur = "Durand"
dbgVérifieEgalité(SAI_Nom..Valeur, "Durand", "instance A")

OuvreSoeur(FEN_Fiche)              // instance B, indépendante (courante)
SAI_Nom..Valeur = "Martin"
dbgVérifieEgalité(SAI_Nom..Valeur, "Martin", "instance B")

Ferme()                           // ferme B ; A redevient courante
dbgVérifieEgalité(SAI_Nom..Valeur, "Durand", "instance A intacte")
Ferme()                           // ferme A
```

C'est ce qui permet, par exemple, d'afficher deux fiches client côte à
côte sans qu'elles ne partagent leurs valeurs.

---

## 6. Récapitulatif des hooks Fenêtre

| Procédure | Quand | Annulable | Usage typique |
|---|---|---|---|
| `FinInitialisation` | Après création des sous-champs | — | Config dynamique, chargement initial |
| `AutorisationFermeture` | Avant fermeture | Oui (`RENVOYER Faux`) | Garde « sauvegarder ? » |
| `Fermeture` | Pendant fermeture | Non | Cleanup, log |
| `DemandeMiseAJour` | Sur appel de `DemandeMiseAJourIHM` (cf. §8) | — | Rafraîchir l'affichage |
| `surChangementTaille` | Au redimensionnement | — | Réagir à la nouvelle taille (cf. §6bis) |

---

## 6bis. Contrôle de la taille et de l'état d'une fenêtre

Une fenêtre ouverte se pilote par programme : sa **géométrie** (taille,
position) et son **état d'affichage** (normal, réduit, agrandi).

### Géométrie : propriétés en lecture/écriture

Les propriétés `..Largeur`, `..Hauteur`, `..X` et `..Y` d'une fenêtre
ouverte sont **modifiables au runtime**. Les réécrire redimensionne ou
déplace la fenêtre :

```owl
FEN_Editeur..Largeur = 800     // redimensionne
FEN_Editeur..Hauteur = 600
FEN_Editeur..X = 100           // déplace le coin haut-gauche
FEN_Editeur..Y = 50
```

### `FenTaille` — redimensionner, déplacer, changer d'état

`FenTaille` regroupe ces opérations en une seule fonction, avec quatre
formes :

| Appel | Effet |
|---|---|
| `FenTaille(FEN_X, larg, haut)` | Redimensionne |
| `FenTaille(FEN_X, x, y, larg, haut)` | Déplace puis redimensionne |
| `FenTaille(FEN_X, mode)` | Fixe le mode d'affichage |
| `FenTaille(FEN_X)` | Renvoie le mode d'affichage courant (entier) |

La fenêtre cible peut être omise ou passée en chaîne vide (`""`) pour
viser la **fenêtre active**.

Le `mode` est l'une des constantes :

| Constante | Valeur | Effet |
|---|---|---|
| `TailleNormale` | 0 | Taille normale (ni réduite, ni agrandie) |
| `TailleMini` | 1 | Réduit en icône |
| `TailleMaxi` | 2 | Agrandit au maximum |

```owl
nMode est un entier = FenTaille(FEN_Editeur)   // lit le mode courant
FenTaille(FEN_Editeur, TailleMaxi)             // agrandit
FenTaille(FEN_Editeur, 640, 480)               // revient à une taille fixe
```

### Raccourcis d'état

Trois raccourcis couvrent les changements d'état usuels. La fenêtre est
optionnelle (fenêtre active par défaut) :

| Appel | Équivalent |
|---|---|
| `Maximise([FEN_X])` | `FenTaille(FEN_X, TailleMaxi)` |
| `Iconise([FEN_X])` | `FenTaille(FEN_X, TailleMini)` |
| `Restaure([FEN_X])` | `FenTaille(FEN_X, TailleNormale)` |

### Événement `surChangementTaille`

Une fenêtre peut réagir à son propre redimensionnement en déclarant la
procédure `surChangementTaille(nLargeur, nHauteur)` dans son body. Elle
est déclenchée chaque fois que la taille change réellement — que le
changement vienne d'une réécriture de `..Largeur`/`..Hauteur`, d'un
appel `FenTaille`, ou d'une action de l'utilisateur sur les bords de la
fenêtre. Les nouvelles dimensions sont reçues en paramètres (pixels).

```owl
FEN_Editeur est une Fenêtre
    PROPRIETES
        Titre       = "Éditeur"
        Largeur     = 400
        Hauteur     = 300
        Disposition = dispLibre
    FIN

    PROCEDURE surChangementTaille(nLargeur est un entier, nHauteur est un entier)
        LIB_Dim..Libellé = nLargeur + " x " + nHauteur
    FIN
FIN
```

> L'alias EN est `onResize`. Les noms EN des fonctions et constantes
> suivent la même règle bilingue : `WinSize`, `Maximize`, `Iconize`,
> `Restore`, `normalSize`, `minSize`, `maxSize`.

---

## 6ter. Lire la géométrie réelle d'un champ

Un champ expose sa géométrie en lecture par quatre propriétés :
`..X`, `..Y` (position) et `..Largeur`, `..Hauteur` (taille). Ces
lectures rendent la **géométrie réelle** du champ une fois la fenêtre
montée — ce que le champ occupe effectivement à l'écran, pas ce que le
fichier déclare. Trois points guident leur interprétation, et l'intuition
courante est souvent la mauvaise.

### Repère de `..X` / `..Y` : la zone client de la fenêtre

`..X` et `..Y` sont exprimés **par rapport au coin haut-gauche de la zone
client de la fenêtre**, jamais par rapport au parent qui porte le champ.
Deux cas font diverger les deux repères — ce sont eux qui surprennent :

- **Champ dans un conteneur** : un champ posé en tête d'un conteneur
  lui-même décalé de 100 pixels rend `..X = 100`, pas `0`. Le décalage du
  conteneur est inclus.
- **Champ dans un volet d'onglet** : le champ est en plus décalé par la
  position de l'onglet, sa barre de languettes et le cadre du volet. Là
  encore, `..X` / `..Y` comptent depuis la fenêtre, pas depuis le volet.

### `..Largeur` / `..Hauteur` : la taille du rendu réel

`..Largeur` et `..Hauteur` rendent la taille effectivement occupée, qui
peut **différer de la valeur déclarée** dans le fichier :

- un champ étiré (`Alignement = alignÉtire`, ou `Étire = Vrai`) prend la
  taille que la disposition lui accorde, même sans `Largeur` déclarée ;
- en disposition automatique, la taille finale résulte du calcul de
  layout, pas d'une valeur écrite.

En disposition libre (`dispLibre`), la géométrie déclarée coïncide avec la
géométrie réelle : ces lectures rendent alors les valeurs posées.

### Lecture avant l'ouverture : une absence de valeur

Un champ n'existe en mémoire qu'une fois sa fenêtre montée (cf. §3). Lire
sa géométrie **avant l'ouverture** rend `Null` — une **absence de valeur**,
et non un `0` qui aurait l'allure d'une mesure. Ce contrat permet de
distinguer « pas encore mesurable » d'une position réelle à l'origine.

Dès `FinInitialisation`, en revanche, la géométrie rendue est déjà la
géométrie finale : la lecture y est fiable, sans attendre l'affichage.

### Exemple

```owl
// FEN_Fiche.owl — la fenêtre seule (un fichier ne contient qu'une fenêtre)

FEN_Fiche est une Fenêtre
    PROPRIETES
        Titre       = "Fiche"
        Largeur     = 400
        Hauteur     = 300
        Disposition = dispVerticale
        Espacement  = 4
    FIN

    BTN_Etire est un Champ Bouton
        PROPRIETES
            Libellé    = "Enregistrer"
            Hauteur    = 30
            Alignement = alignÉtire       // occupe toute la largeur, aucune Largeur déclarée
        FIN
    FIN

    CONT_Boite est un Champ Conteneur
        PROPRIETES
            Largeur     = 300
            Hauteur     = 100
            Disposition = dispVerticale
            Alignement  = alignFin        // collé à droite d'une fenêtre de 400
        FIN

        LIB_Dedans est un Champ Libellé
            PROPRIETES
                Libellé = "Dans le conteneur"
            FIN
        FIN
    FIN
FIN
```

```owl
// demo.owl — script voisin qui ouvre la fenêtre et lit sa géométrie

// Avant ouverture : le champ n'est pas monté -> absence de valeur.
Trace(BTN_Etire..Largeur = Null)          // Vrai

OuvreSoeur(FEN_Fiche)

Trace(BTN_Etire..Largeur)                 // 400 : rendu réel, aucune Largeur déclarée
Trace(LIB_Dedans..X)                      // 100 : relatif à la fenêtre, pas au conteneur (0)

Ferme(FEN_Fiche)
```

---

## 6quater. Écrire la géométrie d'un champ

Les quatre propriétés `..X`, `..Y`, `..Largeur`, `..Hauteur` s'écrivent
aussi, pas seulement se lisent. Ce que l'écriture produit dépend du **mode
de disposition** du conteneur qui porte le champ. La règle tient en une
phrase : **une taille écrite est tenue, une position écrite en disposition
automatique est refusée.**

### Taille en disposition automatique : une contrainte persistante

En disposition automatique (`dispVerticale`, `dispHorizontale`), écrire
`..Largeur` ou `..Hauteur` sur un champ pose une **contrainte réelle** sur
sa taille. La dimension demandée est appliquée immédiatement, et surtout
elle **survit aux recalculs de disposition** — un redimensionnement de la
fenêtre, l'ajout ou le retrait d'un champ voisin ne la remettent pas à la
valeur calculée. La taille écrite gagne sur l'étirement ou l'alignement de
l'axe concerné : elle fige la dimension.

Fixer la taille d'un champ décale ses voisins dans la pile : agrandir la
hauteur d'un champ en disposition verticale repousse d'autant celui qui le
suit.

### Position en disposition automatique : une écriture refusée

En disposition automatique, la position d'un champ est **entièrement
dérivée** par le moteur : c'est lui qui décide où chaque champ se pose sur
l'axe de la pile comme sur l'axe transverse. Écrire `..X` ou `..Y` sur un
tel champ est donc **refusé par une erreur explicite** : le champ **ne
bouge pas**, et l'écriture pose une erreur récupérable via `ErreurDétectée`
/ `ErreurInfo`. La relecture rend toujours la position que la disposition
impose.

Un déplacement qui se déferait de lui-même au recalcul suivant serait le
pire des retours — l'utilisateur croirait avoir réussi. Le refus est donc
franc. Pour positionner un champ à la main, passer son conteneur en
disposition libre (voir ci-dessous).

### Disposition libre : taille et position persistent

En disposition libre (`dispLibre`), aucun moteur ne calcule la géométrie :
les quatre propriétés s'écrivent et **persistent** toutes les quatre. C'est
le mode à choisir quand le placement au pixel près est le besoin.

### Exemple

```owl
// FEN_Panneau.owl — la fenêtre seule, en disposition verticale.

FEN_Panneau est une Fenêtre
    PROPRIETES
        Titre       = "Panneau"
        Largeur     = 300
        Hauteur     = 200
        Disposition = dispVerticale
        Espacement  = 4
    FIN

    LIB_Ligne est un Champ Libellé
        PROPRIETES
            Libellé    = "Ligne"
            Hauteur    = 30
            Alignement = alignÉtire
        FIN
    FIN
FIN
```

```owl
// demo.owl — script voisin qui écrit la géométrie.

OuvreSoeur(FEN_Panneau)

// Écrire une taille : contrainte appliquée et persistante.
LIB_Ligne..Hauteur = 60
Trace(LIB_Ligne..Hauteur)              // 60 : la contrainte tient

// Écrire une position en disposition automatique : refusé.
LIB_Ligne..X = 40
SI ErreurDétectée ALORS
    Trace("position refusée : " + ErreurInfo())
FIN
Trace(LIB_Ligne..X)                    // 0 : la pile impose la position, le champ n'a pas bougé

Ferme(FEN_Panneau)
```

Pour que le `LIB_Ligne..X = 40` prenne effet, déclarer `FEN_Panneau` en
`Disposition = dispLibre` : la position comme la taille s'y écrivent alors
librement et persistent.

---

## 7. Événements par type de champ

Les événements ci-dessous se déclarent comme des `PROCEDURE` à
l'intérieur du body du sous-champ. Le nom doit matcher exactement (à
la casse près) ; un nom non reconnu compile sans erreur mais ne sera
jamais déclenché.

**Bouton, Libellé**
- `surClic()` — clic gauche
- `surDoubleClic()` — double clic gauche

**Saisie**
- `surEntrée()` / `surSortie()` — prise / perte de focus
- `surValidation()` — Entrée ou Tab
- `surAChaqueModification()` — à chaque caractère tapé ou collé

**Combo**
- `surEntrée()` / `surSortie()`
- `surOuverture()` / `surFermeture()` — du dropdown (pas de la fenêtre)
- `surSélection()` — la valeur sélectionnée change
- `surAChaqueModification()` — combo éditable, à chaque frappe

**Liste**
- `surEntrée()` / `surSortie()`
- `surSélection()`

**Table**
- `surEntrée()` / `surSortie()` — prise / perte de focus du champ
- `surClic()` / `surDoubleClic()`
- `surChangementSelection()`, `surEntréeLigne()`, `surSortieLigne()`
- `surAffichageLigne(nIndice)` — déclenché au rendu de chaque ligne
  (`nIndice` 1-based), pour personnaliser l'apparence ligne par ligne

**Sélecteur, Interrupteur**
- `surEntrée()` / `surSortie()`
- `surClic()` / `surDoubleClic()`
- `surAChaqueModification()`

**ZoneRépétée**
- `surEntrée()` / `surSortie()`
- `surClic()` / `surDoubleClic()`
- `surAffichageLigne(nIndice)`, `surSélectionLigne(nIndice)`

**Image** — palette mouse-tracking complète :
- `surClic()` / `surDoubleClic()`
- `surEntréeSurvol()` / `surSortieSurvol()` — encadrent la session souris
- `surSurvol(nX, nY)` — mouvement continu dans la zone
- `surBoutonGauche(nX, nY)` / `surRelâcheBoutonGauche(nX, nY)`
- `surBoutonDroit(nX, nY)` / `surRelâcheBoutonDroit(nX, nY)`
- `surDoubleClicDroit(nX, nY)`
- `surMolette(nMouvement, nX, nY)`
- `surMenuContextuel(nX, nY)`
- `surDéplacement(nX, nY)`, `surChangementTaille(nLargeur, nHauteur)`

**Conteneur** : aucun événement exposé.

---

## 7bis. `MoiMême` — référence au champ courant

À l'intérieur d'un handler d'événement, le mot-clé `MoiMême` (alias
EN `MySelf`) désigne le champ qui exécute le code à cet instant. La
même procédure peut donc être partagée entre plusieurs champs et
distinguer celui qui a déclenché l'événement.

```owl
LIB_Rouge est un Champ Libellé
    PROPRIETES
        Libellé = "Rouge"
        Couleur = RVB(255, 0, 0)
    FIN
    PROCEDURE surClic()
        Trace("Cliqué : " + MoiMême..Libellé)
    FIN
FIN
```

`MoiMême` résout aussi dans le **top-level** d'un sous-champ (le code
de niveau 0 dans le body du champ, hors `PROCEDURE`), où il référence
le champ courant — `MoiMême..Libellé = "..."` y est strictement
équivalent à `LIB_X..Libellé = "..."`. Idem dans `FinInitialisation`
de la fenêtre, où `MoiMême` est la fenêtre elle-même.

Hors handler et hors top-level (par exemple, appel direct depuis le
script principal), `MoiMême` n'a pas de champ courant et déclenche
l'erreur runtime *« MoiMême n'a pas de champ courant »* ; l'erreur
est récupérable via `ErreurDetectee` / `ErreurInfo`.

`MoiMême` s'hérite le long de la chaîne d'appel : une procédure
appelée depuis un handler voit le même `MoiMême` que son appelant.
Pratique pour factoriser un traitement entre plusieurs champs.

---

## 7ter. Déclencher un événement par code : `DéclencheÉvénement`

`DéclencheÉvénement` (anglais `TriggerEvent`) exécute par code le handler d'un
événement d'un champ, sans passer par une interaction. Utile quand la logique
vit dans le handler lui-même et qu'elle doit être ré-exécutée depuis du code
(orchestration, tests).

```owl
DéclencheÉvénement(BTN_Valider, evtClic)          // forme objet
DéclencheÉvénement("BTN_Valider", evtClic)         // forme chaîne (nom résolu à l'exécution)
DéclencheÉvénement(SAI_Age, evtToucheAppuyée, 65)  // événement à paramètre
```

- **Cible** : le champ en objet (`BTN_Valider`) ou son nom en chaîne (indirection).
- **`evtXxx`** : une constante d'événement. Le jeu `evt*` couvre les événements des
  champs (`evtClic`, `evtDoubleClic`, `evtEntrée`, `evtSortie`, `evtAChaqueModification`,
  `evtToucheAppuyée`…) et suit le catalogue d'événements : chaque événement de champ a
  sa constante.
- **Paramètres** : passés au handler si l'événement en attend un (par exemple le code de
  touche pour `evtToucheAppuyée`).

Trois contrôles sont faits **à la compilation** :

1. cible inconnue (objet non déclaré, ou chaîne littérale ne désignant aucun champ) ;
2. événement absent du type de champ visé (par exemple un événement de table sur un
   bouton) ;
3. paramètres invalides (nombre ou type), vérifiés d'après la signature portée par la
   constante `evtXxx` — donc contrôlés même en forme chaîne.

`DéclencheÉvénement` ne sert qu'à déclencher un événement. Pour partager une logique
sans passer par un événement, extraire une procédure nommée et l'appeler directement.

---

## 7quater. Le champ Table : déclaration et mise en forme

### Déclarer une Table et ses colonnes

Un champ Table regroupe des colonnes déclarées comme sous-blocs `Colonne`,
imbriqués dans le bloc de la Table. Chaque colonne porte ses propriétés
courantes : `Libellé` (le texte d'en-tête), `Type` (la nature des cellules,
via l'énumération `colTypes`) et `Largeur` (en pixels).

```owl
TABLE_Repertoire est un Champ Table
    PROPRIETES
        Étire = Vrai
    FIN

    COL_Nom est une Colonne
        PROPRIETES
            Libellé = "Nom"
            Type    = colTypeTexte
            Largeur = 220
        FIN
    FIN

    COL_Age est une Colonne
        PROPRIETES
            Libellé = "Âge"
            Type    = colTypeNumerique
            Largeur = 100
        FIN
    FIN
FIN
```

Une ligne s'ajoute avec `TableAjouteLigne`, une valeur par colonne dans
l'ordre de déclaration. Une cellule se lit par le nom de sa colonne :

```owl
TableAjouteLigne(TABLE_Repertoire, "Ada", 36)
TableAjouteLigne(TABLE_Repertoire, "Grace", 41)

sNom  est une chaîne = TABLE_Repertoire[1].COL_Nom     // "Ada"
nAge  est un entier  = TABLE_Repertoire[2].COL_Age     // 41
nCount est un entier = TableOccurrence(TABLE_Repertoire) // 2
```

`TableInsèreLigne` insère à une position précise (indice 1-based) au lieu
d'ajouter en fin : `TableInsèreLigne(TABLE_Repertoire, 1, "Alan", 24)` place la
ligne en tête et décale les suivantes.

Le point simple désigne le nom de la colonne (`TABLE_X.COL_A`) ; les
propriétés d'affichage s'écrivent avec le double point (voir plus bas).
La fiche `owl explain Colonne` détaille les propriétés d'une colonne.

### Mise en forme conditionnelle

Le style d'une Table (`TABLE_X..Style`) et celui d'une colonne
(`COL_X..Style`) décrivent une apparence fixe, décidée à la déclaration.
Pour faire ressortir une donnée en fonction de sa valeur — un solde
négatif, un stock bas, une échéance dépassée — une Table expose deux
propriétés qui s'écrivent à l'exécution, ligne par ligne ou cellule par
cellule.

### Les deux propriétés

| propriété | alias EN | effet |
|---|---|---|
| `..Couleur` | `..Color` | couleur du **texte** |
| `..CouleurFond` | `..BackgroundColor` | couleur du **fond** |

Elles acceptent tout ce qui vaut une couleur : constante nommée, `RVB(r, v, b)`
ou entier compact (cf. chapitre *Couleurs*). Elles se relisent : une cellule
qu'aucune écriture n'a visée rend `Null`.

### Les quatre écritures

```owl
TABLE_Synthese[nLigne][nColonne]..Couleur = RVB(255, 255, 255)  // une cellule
TABLE_Synthese[nLigne, nColonne]..Couleur = RVB(255, 255, 255)  // une cellule (forme compacte)
TABLE_Synthese.COL_Stock[nLigne]..Couleur = RVB(255, 255, 255)  // une cellule
TABLE_Synthese[nLigne]..Couleur           = RVB(255, 255, 255)  // la ligne entière
```

- **Cellule, la ligne d'abord** — `TABLE_X[ligne][colonne]` : deux indices
  numériques, la colonne étant désignée par son rang (1 pour la première
  colonne déclarée).
- **Cellule, forme compacte** — `TABLE_X[ligne, colonne]` : les deux
  coordonnées dans les mêmes crochets, séparées par une virgule. La colonne se
  désigne par son rang **ou** par son nom (`TABLE_X[ligne, COL_A]`). Équivalente
  à la forme à deux crochets.
- **Cellule, la colonne d'abord** — `TABLE_X.COL_A[ligne]` : la colonne est
  désignée par son nom, ce qui évite de compter les rangs et survit à une
  réorganisation des colonnes. Ces formes visent la même cellule :
  ce qui est écrit par l'une se relit par les autres.
- **Ligne entière** — `TABLE_X[ligne]` : un seul indice. Toutes les colonnes
  de la ligne sont teintées.

Dans les quatre formes, la propriété s'écrit avec le **double point** :
`[1][2]..Couleur`. Le point simple y désigne un nom de colonne
(`TABLE_X.COL_A[1]`), jamais une propriété.

### Les indices sont des expressions

C'est le point qui rend la fonctionnalité utilisable : `ligne` et `colonne`
ne sont pas obligatoirement des littéraux. Une variable de boucle, un
paramètre de procédure, le résultat d'un appel conviennent. La mise en forme
s'écrit donc dans une boucle qui parcourt les lignes et décide de la couleur
à partir de la donnée affichée.

```owl
PROCEDURE AppliqueMiseEnForme()
    nLigne est un entier
    nSolde est un entier

    POUR nLigne = 1 À TableOccurrence(TABLE_Synthese)
        nSolde = TABLE_Synthese[nLigne].COL_Tresorerie

        SI nSolde < 0 ALORS
            TABLE_Synthese[nLigne][2]..Couleur     = RVB(255, 255, 255)
            TABLE_Synthese[nLigne][2]..CouleurFond = RVB(200, 30, 30)
        FIN
    FIN
FIN
```

Poser une mise en forme ne modifie pas le contenu de la cellule :
`TABLE_X[ligne].COL_A` continue de rendre la valeur affichée.

### Ce qui n'est pas une désignation de cellule

`TABLE_X[ligne].COL_A` désigne la **valeur** de la cellule, pas la cellule
elle-même : lui accoler une propriété (`TABLE_X[ligne].COL_A..Couleur`) est
refusé à la compilation, avec un message qui rappelle les deux écritures
ci-dessus. Un troisième niveau d'indice (`TABLE_X[l][c][x]`) est refusé pour
la même raison.

---

## 7quinquies. Champ Saisie : masque de saisie

Un `Champ Saisie` peut contraindre la frappe caractère par caractère via
sa propriété `MasqueSaisie` (`InputMask`). Le masque est un gabarit à
**positions fixes** : chaque position du texte correspond à une position
du masque et n'accepte que ce que cette position autorise.

Trois caractères sont des **métacaractères** (des emplacements à remplir) :

| Métacaractère | Accepte |
|---|---|
| `9` | Un chiffre (`0`–`9`) |
| `A` | Une lettre |
| `C` | Une lettre **ou** un chiffre |

**Tout autre caractère est un littéral** : il s'affiche tel quel, à
position fixe, et l'utilisateur ne peut pas le modifier. Le curseur saute
par-dessus lors de la frappe. Un masque composé uniquement de littéraux
n'a donc aucun emplacement de saisie ; `AFFF` par exemple vaut une lettre
(`A`) suivie du littéral `FFF`.

Exemples :

```owl
SAI_Date..MasqueSaisie = "99/99/9999"        // date brute : 8 chiffres, / fixes
SAI_Initiales..MasqueSaisie = "AAA"          // exactement 3 lettres
SAI_CodeArticle..MasqueSaisie = "CCCCCC"     // 6 caractères alphanumériques
SAI_Telephone..MasqueSaisie = "+99 999 99 99 99"  // + et espaces littéraux
```

Le masque contraint la **saisie**. Pour piloter uniquement le *format
affiché* d'un champ Date sans imposer une frappe position par position,
utiliser plutôt `MasqueAffichage` (`DisplayMask`).

---

## 8. `DemandeMiseAJour` — hors flow

Cet événement n'a pas de place fixe dans le cycle : il se déclenche à la
demande, par l'appel de `DemandeMiseAJourIHM` (`RequestUpdateUI`). À
implémenter dans une fenêtre dont l'affichage dépend de sources externes
(fichier, HTTP, base) susceptibles de changer.

```owl
FEN_Tableau est une Fenêtre
    PROCEDURE DemandeMiseAJour()
        LIB_Solde..Libellé = SoldeFormaté()      // relit la source de données
    FIN
FIN
```

```owl
Ouvre(FEN_Tableau)
EnregistreVente(nMontant)
DemandeMiseAJourIHM()          // rafraîchit l'affichage
```

`DemandeMiseAJourIHM` s'appelle **sans paramètre** et vise la **fenêtre
courante**. L'exécution est **synchrone** : l'événement `DemandeMiseAJour`
s'exécute entièrement avant le retour de l'appel. Sans fenêtre ouverte, ou
si la fenêtre courante ne déclare pas cet événement, l'appel est sans effet
et sans erreur.

---

## 9. Câblage d'une application multi-fichier

Une application GUI réelle s'écrit rarement dans un seul fichier. Le
canon OWL fixe une règle simple : **un fichier source ne contient qu'une
seule fenêtre**, et ses helpers de rendu ou de logique vivent **dans le
bloc `est une Fenêtre`**, pas après `FIN`. Le code d'exécution
(`Ouvre`, état partagé) vit dans un fichier voisin.

Voici la mise en page minimale pour un projet GUI multi-fichier — un
petit compteur avec moteur métier dans une classe dédiée :

```
mon_projet/
    projet.owl          ← frontière + programme (état global + démarrage)
    FEN_Compteur.owl    ← fenêtre + helpers DANS le bloc
    cMoteur.owl         ← logique métier (classe POO)
```

```owl
// projet.owl — frontière, état global et point de démarrage.
// Le code de niveau supérieur est le programme : il s'exécute en dernier,
// après l'init des collections (la classe cMoteur ci-dessous est une simple
// déclaration, elle ne s'exécute pas au démarrage).

oMoteur est un cMoteur                       // global au projet

Ouvre(FEN_Compteur)
```

```owl
// cMoteur.owl — logique métier isolée, sans dépendance UI

cMoteur est une Classe
    nValeur est un entier = 0

    PROCEDURE Incrémenter()
        objet.nValeur = objet.nValeur + 1
    FIN

    PROCEDURE Valeur() : entier
        RENVOYER objet.nValeur
    FIN
FIN
```

```owl
// FEN_Compteur.owl — fenêtre seule, helpers DANS le bloc

FEN_Compteur est une Fenêtre
    PROPRIETES
        Titre       = "Compteur"
        Largeur     = 240
        Hauteur     = 120
        Disposition = dispVerticale
        Padding     = 12
        Espacement  = 8
    FIN

    // Helper DANS le bloc. Adresse les sous-champs LIB_Valeur et
    // BTN_Plus déclarés plus bas — l'ordre de déclaration au sein
    // du bloc n'a pas d'importance (helpers comme champs).
    PROCEDURE RafraichitAffichage()
        LIB_Valeur..Libellé = "Valeur : " + oMoteur.Valeur()
    FIN

    LIB_Valeur est un Champ Libellé
        PROPRIETES
            Libellé = "Valeur : 0"
            Étire   = Vrai
        FIN
    FIN

    BTN_Plus est un Champ Bouton
        PROPRIETES
            Libellé = "+1"
            Largeur = 80
        FIN
        PROCEDURE surClic()
            oMoteur.Incrémenter()
            RafraichitAffichage()
        FIN
    FIN

    PROCEDURE FinInitialisation()
        RafraichitAffichage()
    FIN
FIN
```

Quelques règles à garder en tête :

- **`projet.owl` marque la racine du projet**, inclut les sous-dossiers **et
  porte le programme** : le code de son bloc est le point de démarrage,
  exécuté en dernier (après l'init des collections). Sans lui, un dossier
  multi-fichier n'est pas vu comme un projet (cf.
  `00_apercu_du_langage.md` §11).
- **L'état global vit dans `projet.owl`** (ou un autre fichier non-classe
  du projet). Les variables top-level de `projet.owl` sont **globales au
  projet** : visibles depuis n'importe quel bloc Fenêtre.
- **Les helpers PROCEDURE qui adressent des sous-champs vivent DANS le
  bloc `est une Fenêtre`**. Mettre une `PROCEDURE Helper()` après le
  `FIN` de la fenêtre, dans le but de manipuler `LIB_Valeur` ou
  `BTN_Plus`, est rejeté à la compilation : le fichier-fenêtre est
  réservé à la déclaration et à ses helpers de contexte.
- **Pas de helpers cross-fichier qui adressent les champs d'une autre
  fenêtre.** Pour partager du code de calcul ou de logique entre
  plusieurs fenêtres, passer par une **classe** (le `cMoteur` de
  l'exemple) — la classe ne connaît pas les champs et reste réutilisable.
- **Refraîchir l'affichage = réassigner la propriété**. Après un
  calcul, recopier le résultat dans le champ (`LIB_Valeur..Libellé =
  …`, `IMG_Vue..Image = …`). La fenêtre se redessine sur la
  réassignation.

### Mode GO local : ouvrir un fichier-fenêtre direct

Pour une démo ou un test rapide d'une fenêtre **qui n'a pas besoin
d'état global externe**, exécuter directement le fichier-fenêtre suffit :

```
owl FEN_Compteur.owl
```

OWL monte la fenêtre et entre dans la boucle d'événements sans passer
par `projet.owl`. Pratique pour itérer sur le visuel d'une fenêtre isolée.
Dès qu'il faut de l'état partagé ou plusieurs fenêtres orchestrées,
revenir au `projet.owl` du projet.

---

## 10. Pièges courants

- **Code de body au top-level**. `Trace(...)`, un appel de procédure ou
  `LIB_Titre..Libellé = "x"` directement dans le body sont rejetés avec
  warning et la ligne est sautée. Une affectation nue (`n = 5`) déclenche
  l'erreur plus tranchante *« Propriete inconnue pour Fenetre »*. Tout
  code d'exécution va dans `FinInitialisation` ou un handler d'événement.
- **`AutorisationFermeture` sans `RENVOYER`**. Le défaut est *autorisée*.
  Pour annuler, écrire explicitement `RENVOYER Faux`.
- **`Fermeture` n'annule pas**. Un `RENVOYER Faux` dedans n'a aucun
  effet sur la fermeture en cours.
- **Variable de fenêtre persistante**. Comme c'est une globale, sa
  valeur survit à la fermeture. Réinitialiser dans `FinInitialisation`
  si on veut un état frais à chaque ouverture.
- **Nom d'événement mal orthographié**. `surCLic`, `SurClic` ou
  `surClick` compilent mais ne se déclenchent pas — la procédure
  n'est jamais appelée. Vérifier la casse exacte (`surClic`,
  `surDoubleClic`, ...).
- **`surFermeture` ≠ `Fermeture`**. `surFermeture` est l'événement
  Combo (fermeture du dropdown), pas le hook de fenêtre. Pour le
  cleanup d'une fenêtre, c'est `Fermeture`.
- **Accéder à un champ sans ouvrir sa fenêtre**. Un champ n'existe que
  dans une fenêtre montée. `SAI_Nom..Valeur` avant tout `Ouvre`/
  `OuvreFille`/`OuvreSoeur` de sa fenêtre échoue : il faut ouvrir
  d'abord.
- **Confondre deux instances**. Deux ouvertures de la même fenêtre =
  deux états séparés. Les accès portent toujours sur l'instance
  courante (la dernière ouverte) ; un `Ferme()` fait redevenir la
  précédente courante.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Contrôle de flux (`SI`, `POUR`, ...) | `03_controle_de_flux.md` |
| Cycle de vie d'un État | `16_lifecycle_etats.md` |
| Couleurs (`RVB`, constantes nommées) | `29_couleurs.md` |
| Conventions de code | `10_conventions_de_code.md` |
