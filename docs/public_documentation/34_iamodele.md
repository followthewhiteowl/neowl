# Réseaux de neurones : `IAModèle`

## 1. En bref

Un `IAModèle` est un **réseau de neurones** que l'on construit en
empilant des couches, que l'on **entraîne** sur des données, puis que
l'on interroge pour **prédire**. Il vise les tâches d'apprentissage
supervisé classiques — classification, régression — sur des données
numériques.

L'API tient en quatre gestes : **construire** les couches, **entraîner**
le modèle, **inférer** une sortie, **sauver / charger** le modèle sur
disque. Les valeurs qui circulent — entrées, sorties, poids — sont des
`Tenseur` (voir [`33_vecteurs.md`](33_vecteurs.md) pour le socle
numérique).

```owl
modèle est un IAModèle
modèle.AjouteCoucheDense(4, 8)
modèle.AjouteCoucheReLU()
modèle.AjouteCoucheDense(8, 3)
Trace(modèle..NbCouches)      // 3
```

Le périmètre actuel est **explicite** : `IAModèle` gère aujourd'hui les
**réseaux denses** (perceptrons multicouches). Ce qu'il ne gère pas
encore est nommé clairement en [section 9](#9-hors-périmètre-actuel) —
un cas hors-périmètre lève une erreur qui désigne le manque, jamais un
plantage obscur.

---

## 2. Construire un réseau

Un modèle se construit en **empilant des couches** par appels fluents,
dans l'ordre où le signal les traverse. Deux familles de couches
existent aujourd'hui.

**Les couches denses** (`AjouteCoucheDense(entrées, sorties)`) relient
chaque entrée à chaque sortie. Elles portent les **poids** appris. Le
nombre de `sorties` d'une couche doit correspondre au nombre d'`entrées`
de la couche dense suivante.

**Les couches d'activation** introduisent la non-linéarité, sans quoi un
empilement de couches denses se réduirait à une seule. Trois activations
sont disponibles : `AjouteCoucheReLU`, `AjouteCoucheSigmoïde`,
`AjouteCoucheTanh`.

```owl
modèle est un IAModèle
modèle.AjouteCoucheDense(4, 8)
modèle.AjouteCoucheReLU()
modèle.AjouteCoucheDense(8, 3)
```

| Couche | Rôle |
|---|---|
| `.AjouteCoucheDense(e, s)` / `.AddDenseLayer` | Couche dense, `e` entrées vers `s` sorties |
| `.AjouteCoucheReLU()` / `.AddReLULayer` | Activation ReLU (annule les valeurs négatives) |
| `.AjouteCoucheSigmoïde()` / `.AddSigmoidLayer` | Activation sigmoïde (ramène dans `]0, 1[`) |
| `.AjouteCoucheTanh()` / `.AddTanhLayer` | Activation tangente hyperbolique (ramène dans `]-1, 1[`) |

### Initialisation des poids

Les poids d'une couche dense sont tirés **au hasard à sa création**,
selon une stratégie qui conditionne la vitesse d'apprentissage. Un
troisième argument optionnel choisit cette stratégie ; par défaut,
`initHe` s'applique.

| Stratégie | Convient à |
|---|---|
| `initHe` (défaut) | Couches suivies d'une activation ReLU |
| `initXavier` | Couches suivies d'une activation sigmoïde ou tangente hyperbolique |

```owl
modèle est un IAModèle
modèle.AjouteCoucheDense(4, 8, initHe)       // explicite, mais c'est le défaut
modèle.AjouteCoucheReLU()
modèle.AjouteCoucheDense(8, 3, initXavier)
```

### Construire des branches parallèles

Toutes les architectures ne sont pas une simple pile. Le signal peut se
**diviser** en plusieurs chemins parallèles à partir d'un même point, puis
ces chemins **convergent** en une seule sortie. Un tel bloc se délimite par
quatre marqueurs, insérés dans la construction comme des couches.

| Marqueur | Rôle |
|---|---|
| `.DébutBranches()` / `.BeginBranches()` | Capture l'activation courante — le **point de divergence** — et ouvre la première branche |
| `.BrancheSuivante()` / `.NextBranch()` | Clôt la branche courante et en ouvre une nouvelle, repartant du **même** point de divergence |
| `.FusionneParAddition()` / `.MergeByAdd()` | Clôt le bloc et **additionne** les sorties des branches, élément par élément |
| `.FusionneParConcaténation()` / `.MergeByConcat()` | Clôt le bloc et met les sorties **bout à bout** sur l'axe des canaux |

Les couches ajoutées entre deux marqueurs alimentent la branche courante.
Une branche **vide** — aucune couche — laisse passer l'entrée telle quelle :
elle vaut l'**identité**. Un raccourci identité s'exprime donc par une branche
qui porte des couches et une branche vide, fusionnées par addition. Les
branches fusionnent dans leur ordre de déclaration.

Les préconditions de fusion diffèrent selon le mode :

- **Fusion par addition** : les sorties de toutes les branches doivent avoir
  exactement la **même forme** — mêmes dimensions spatiales *et* même nombre
  de canaux —, la somme se faisant case à case.
- **Fusion par concaténation** : les dimensions spatiales doivent coïncider
  (même taille de lot, même hauteur, même largeur), mais le **nombre de
  canaux peut différer** — la sortie en porte la somme. Des dimensions
  spatiales incompatibles lèvent une erreur plutôt que de produire un
  résultat incohérent.

Un **bloc résiduel à projection** additionne un chemin principal et un
raccourci qui adapte le nombre de canaux. Le chemin principal transforme
l'entrée (convolution 3×3 suivie d'une normalisation de lot) ; le raccourci
la reprojette (convolution 1×1 + normalisation de lot) pour que les deux
sorties partagent la même forme avant l'addition.

```owl
modèle est un IAModèle
modèle.DébutBranches()
    modèle.AjouteCoucheConvolution2D(3, 8, 3, 1, 1)   // chemin principal : F(x)
    modèle.AjouteCoucheNormalisationLot(8)
modèle.BrancheSuivante()
    modèle.AjouteCoucheConvolution2D(3, 8, 1, 1, 0)   // raccourci : projection 1×1
    modèle.AjouteCoucheNormalisationLot(8)
modèle.FusionneParAddition()
```

Les arguments d'`AjouteCoucheConvolution2D` sont, dans l'ordre : nombre de
canaux d'entrée, nombre de canaux de sortie, taille du noyau, pas et
remplissage. Ici les deux branches produisent 8 canaux à dimensions
spatiales identiques, ce que la fusion par addition exige.

Un **module à deux branches** applique deux convolutions de tailles de noyau
différentes à la même entrée, puis place leurs sorties côte à côte. La
concaténation autorise des nombres de canaux distincts — ici 4 et 6 —, la
sortie en portant la somme (10).

```owl
modèle est un IAModèle
modèle.DébutBranches()
    modèle.AjouteCoucheConvolution2D(3, 4, 1, 1, 0)   // branche à noyau 1×1
modèle.BrancheSuivante()
    modèle.AjouteCoucheConvolution2D(3, 6, 3, 1, 1)   // branche à noyau 3×3
modèle.FusionneParConcaténation()
```

`.Infère` traverse un bloc de branches sans manipulation particulière :
rejouer chaque branche depuis le point de divergence, puis fusionner selon le
marqueur de clôture. Appeler `.BrancheSuivante`, `.FusionneParAddition` ou
`.FusionneParConcaténation` **sans** `.DébutBranches` ouvert au préalable lève
une erreur.

---

## 3. Inférer

`.Infère(entrée)` fait passer une entrée à travers toutes les couches,
dans l'ordre d'empilement, et rend la sortie du réseau. La forme de la
sortie suit celle de l'entrée.

**Un échantillon unique** est un `Tenseur` à une dimension (1D) dont la
taille est le nombre d'entrées de la première couche. La sortie est un
`Tenseur` 1D de la taille de la dernière couche.

```owl
échantillon est un Tenseur = Tenseur([1.0, 0.5, -1.0, 2.0])
sortie est un Tenseur = modèle.Infère(échantillon)
prédiction est un Vecteur = sortie..Valeur
Trace(prédiction..Taille)      // 3
```

**Un lot d'échantillons** est un `Tenseur` à deux dimensions (2D),
`B` lignes de `entrées` colonnes. La sortie est un `Tenseur` 2D
`B × sorties` : la dimension de lot traverse le réseau sans être
altérée. Traiter un lot en un seul appel est plus efficace qu'une boucle
d'appels échantillon par échantillon.

```owl
lot est un Tenseur = Tenseur([[1.0, 0.5, -1.0, 2.0], [0.0, 1.0, 1.0, -1.0]])
sorties est un Tenseur = modèle.Infère(lot)      // 2 lignes en sortie
```

`.Infère` est un calcul **pur** : deux appels avec la même entrée
rendent exactement la même sortie. Les poids ne changent qu'à
l'entraînement.

---

## 4. Entraîner

L'entraînement ajuste les poids pour que les prédictions se rapprochent
des cibles attendues. Il se pilote par un objet `OptionsEntraînement`,
puis un seul appel `.Entraîne(entrées, cibles, options)`.

Les **entrées** et les **cibles** sont deux `Tenseur` 2D alignés :
`N` échantillons en lignes. Chaque ligne des cibles est la sortie
attendue pour la ligne d'entrées de même rang — en encodage un-parmi-n
pour la classification, en valeur brute pour la régression.

`.Entraîne` **modifie le modèle en place** : les poids appris restent
dans le modèle, il n'y a pas de valeur à réaffecter.

```owl
entrées est un Tenseur = Tenseur([[1.0, 0.5, -1.0, 2.0], [0.0, 1.0, 1.0, -1.0]])
cibles est un Tenseur = Tenseur([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])

options est un OptionsEntraînement
options..Époques           = 50
options..TailleLot         = 32
options..TauxApprentissage = 0.01
options..Optimiseur        = optimAdam
options..Perte             = perteEntropieCroisée

modèle.Entraîne(entrées, cibles, options)
```

### Les réglages d'`OptionsEntraînement`

Les valeurs par défaut permettent un premier entraînement sans tout régler.

| Réglage | Rôle | Défaut |
|---|---|---|
| `..Époques` / `..Epochs` | Nombre de passes complètes sur les données | `1` |
| `..TailleLot` / `..BatchSize` | Taille des mini-lots | `32` |
| `..TauxApprentissage` / `..LearningRate` | Pas de mise à jour des poids | `0.01` |
| `..Optimiseur` / `..Optimizer` | Stratégie d'optimisation | `optimSGD` |
| `..Perte` / `..Loss` | Fonction de perte | `perteEntropieCroisée` |
| `..SurÉpoque` / `..OnEpoch` | Procédure de suivi rappelée à chaque époque | *(aucune)* |

**Les optimiseurs** déterminent comment les poids reculent à chaque
mini-lot :

| Optimiseur | Comportement |
|---|---|
| `optimSGD` | Descente de gradient stochastique : pas fixe proportionnel au gradient |
| `optimAdam` | Pas adapté par poids ; converge souvent plus vite sans réglage fin du taux |

**Les fonctions de perte** mesurent l'écart entre sortie et cible :

| Perte | Usage |
|---|---|
| `perteEntropieCroisée` / `lossCrossEntropy` | Classification multi-classe (cible un-parmi-n) |
| `perteErreurQuadratique` / `lossQuadraticError` | Régression (valeur continue) |

### Suivre la progression

`..SurÉpoque` reçoit une **procédure** rappelée à la fin de chaque
époque, avec le numéro d'époque courant, le nombre total d'époques et la
perte moyenne de l'époque écoulée. De quoi alimenter une jauge ou une
trace sans dérouler la boucle à la main. Sans affectation, l'entraînement
ne rappelle rien.

```owl
PROCEDURE SuiviÉpoque(numéro est un entier, total est un entier, perte est un réel)
    Trace("Époque " + numéro + " / " + total + " — perte " + perte)
FIN

options..SurÉpoque = SuiviÉpoque      // le nom nu, sans parenthèses
```

### Apprentissage par transfert

Réentraîner un réseau à partir de zéro coûte cher en données et en temps.
Quand un modèle a déjà appris des représentations utiles sur une tâche
voisine, il est plus économique de **réutiliser son corps** et de ne
réapprendre que la partie qui décide — la **tête**. C'est l'apprentissage
par transfert : garder les couches qui extraient les caractéristiques,
remplacer la tête d'origine par une tête adaptée à ses propres catégories,
puis n'ajuster que cette dernière.

Quatre gestes suffisent, dans cet ordre.

**Figer le corps.** `.Gèle()` / `.Freeze()` marque non entraînables
**toutes les couches présentes** au moment de l'appel : l'inférence les
traverse normalement, mais l'entraînement saute leur mise à jour. Leurs
poids restent donc strictement inchangés. `.Dégèle()` / `.Unfreeze()`
réactive l'entraînement sur l'ensemble des couches. Le gel concerne
l'entraînement, pas la structure.

**Retirer la tête d'origine.** `.RetireDernièreCouche()` /
`.RemoveLastLayer()` enlève la **dernière** couche empilée : `..NbCouches`
diminue de un, et l'inférence produit désormais la sortie de la couche qui
précède. Un appel répété retire les couches une à une, dans l'ordre inverse
de l'ajout. Retirer la couche d'un modèle vide, ou retirer un marqueur de
bloc à branches (couper au milieu d'un bloc serait ambigu), lève une erreur
et laisse le modèle intact.

**Greffer une nouvelle tête.** Toute couche ajoutée **après** le gel est
entraînable — le gel ne s'applique qu'aux couches déjà présentes. Une
nouvelle couche dense dimensionnée sur ses propres catégories devient donc
la tête à apprendre.

**Ajuster.** Un appel `.Entraîne` ne modifie plus que la nouvelle tête : le
corps figé reste au bit près ce qu'il était. L'entraînement converge plus
vite qu'un apprentissage complet, puisqu'il n'ajuste qu'une fraction des
poids.

```owl
// Un corps pré-entraîné et sa tête d'origine à 5 catégories.
modèle est un IAModèle
modèle.AjouteCoucheDense(4, 8)
modèle.AjouteCoucheReLU()
modèle.AjouteCoucheDense(8, 5)      // tête d'origine

modèle.Gèle()                       // figer corps + tête d'origine

modèle.RetireDernièreCouche()       // retirer la tête d'origine
modèle.AjouteCoucheDense(8, 2)      // nouvelle tête : 2 catégories, entraînable

// Fine-tune : seul le corps gelé reste inchangé, la nouvelle tête apprend.
modèle.Entraîne(entrées, cibles, options)
```

Le corps gelé sert d'extracteur de caractéristiques figé ; seule la tête
greffée après le gel s'adapte aux nouvelles données. `.RetireDernièreCouche`
et `.Gèle` s'utilisent aussi hors transfert : le premier pour corriger une
pile mal construite, le second pour figer une partie d'un réseau pendant une
phase d'entraînement.

---

## 5. Détecter des objets

Certaines tâches ne demandent pas *quelle* catégorie une image représente,
mais *quels objets* s'y trouvent et *où*. Un modèle de détection importé au
format ONNX (`.ChargeONNX`, voir [section 9](#9-hors-périmètre-actuel) pour
le périmètre de l'import) localise plusieurs objets dans une image en un seul
geste : `.Détecte`.

Là où `.Infère` rend un tenseur de sorties brut, `.Détecte` enchaîne toute la
chaîne d'un détecteur : redimensionner l'image à l'entrée du modèle, inférer,
décoder les prédictions, écarter celles sous un seuil de confiance, supprimer
les boîtes redondantes qui se recouvrent (suppression non-maximale, par
classe), puis remettre les boîtes à l'échelle de l'image d'origine. Le
résultat est une liste d'objets directement exploitables.

### `.Détecte`

`.Détecte(image, seuilScore, seuilIoU)` prend une `Image` et deux seuils, et
rend un **tableau de `Détection`** trié par score décroissant.

| Argument | Rôle |
|---|---|
| `image` | L'`Image` à analyser (chargée par `dChargeImage`). Elle est redimensionnée à l'entrée du modèle en interne ; les boîtes rendues restent exprimées dans les pixels d'origine. |
| `seuilScore` | Confiance minimale, dans `[0, 1]` : toute détection en dessous est écartée. Plus il est élevé, moins de boîtes, mais plus sûres. |
| `seuilIoU` | Seuil de recouvrement pour la suppression des doublons : deux boîtes de même classe dont le recouvrement dépasse ce seuil sont ramenées à la mieux notée. |

| Forme fluente | Forme préfixée |
|---|---|
| `modèle.Détecte(image, seuilScore, seuilIoU)` / `.Detect` | *(forme méthode uniquement)* |

### Le type `Détection`

Chaque élément du tableau rendu est une `Détection`, lue par double point.

| Propriété | Rend |
|---|---|
| `..Boîte` / `..Box` | Tableau de 4 réels `[x, y, largeur, hauteur]`, en pixels de l'image d'origine : coin haut-gauche, puis taille. |
| `..Classe` / `..Class` | Index de la classe prédite — un entier **basé sur 0**. |
| `..Score` / `..Score` | Confiance de la détection, un réel dans `[0, 1]`. |

La classe est un **index**, pas un libellé : le modèle ne connaît que des
numéros. Associer chaque index à un nom lisible relève de l'appelant — un
simple tableau de noms suffit. Cet index est basé sur 0 alors qu'un tableau
OWL est basé sur 1, d'où l'accès `noms[classe + 1]`.

### Exemple de bout en bout

Charger une image et un modèle de détection, détecter, puis parcourir les
objets trouvés :

```owl
noms est un tableau de chaînes = ["personne", "vélo", "voiture", "bus", "chien"]

image est une Image = dChargeImage("photo.jpg")

modèle est un IAModèle
modèle.ChargeONNX("detecteur.onnx")

détections est un tableau de Détection = modèle.Détecte(image, 0.25, 0.45)
Trace(Taille(détections) + " objet(s) trouvé(s)")

POUR TOUT d DE détections
    nomClasse est une chaîne = noms[d..Classe + 1]
    posX est un réel = d..Boîte[1]
    posY est un réel = d..Boîte[2]
    Trace(nomClasse + " (" + Arrondi(d..Score, 2) + ") en [" + posX + ", " + posY + "]")
FIN
```

Dessiner les boîtes sur l'image se fait ensuite avec les primitives de dessin
`dRectangle` (le contour) et `dTexte` (l'étiquette). Un exemple complet et
annoté — chargement du modèle, dessin des boîtes, sauvegarde de l'image —
accompagne le dossier de démonstration `detection_yolo`.

### Primitives : `SuppressionNonMax` et `IoU`

`.Détecte` applique déjà tout le filtrage. Deux natives exposent les briques
sous-jacentes pour un pipeline sur mesure — filtrer les sorties d'un modèle
décodées à la main, ou fusionner des détections de plusieurs sources.

**`IoU(boîteA, boîteB)`** / `IoU` mesure le recouvrement de deux boîtes,
chacune donnée par un tableau de 4 réels `[x, y, largeur, hauteur]`. Le
résultat est un réel dans `[0, 1]` : `0` quand les boîtes sont disjointes, `1`
quand elles coïncident, une valeur intermédiaire selon l'aire commune rapportée
à l'aire totale.

**`SuppressionNonMax(boîtes, scores, seuilIoU)`** / `NonMaxSuppression`
supprime les doublons qui se chevauchent. Elle parcourt les boîtes de la mieux
notée à la moins bien notée et conserve une boîte tant que son recouvrement
(IoU) avec chaque boîte déjà retenue reste sous `seuilIoU`. Elle rend un
tableau des **indices** (basés sur 1) des boîtes conservées, triés par score
décroissant ; une liste vide rend un tableau vide, une seule boîte est toujours
conservée.

```owl
boîtes est un tableau de tableaux = [[10.0, 10.0, 50.0, 50.0], [12.0, 12.0, 48.0, 48.0], [200.0, 80.0, 40.0, 90.0]]
scores est un tableau de réels = [0.9, 0.75, 0.6]

gardés est un tableau d'entiers = SuppressionNonMax(boîtes, scores, 0.5)
// gardés vaut [1, 3] : la 2e boîte, quasi confondue avec la 1re, est écartée.

recouvrement est un réel = IoU(boîtes[1], boîtes[2])   // proche de 1 : fort chevauchement
```

---

## 6. Sauver et charger

Un modèle entraîné se **sérialise en JSON** sur disque, puis se recharge
à l'identique — sans ré-entraînement. Les réels sont écrits en pleine
précision : un modèle sauvé puis rechargé redonne exactement les mêmes
poids, donc la même sortie de `.Infère`.

```owl
modèle.Sauve("modele.json")
```

`.Charge` remplit un modèle **de même architecture** avec les poids lus.
Le modèle receveur se construit d'abord avec les mêmes couches, puis
reçoit les poids du fichier.

```owl
frais est un IAModèle
frais.AjouteCoucheDense(4, 8)
frais.AjouteCoucheReLU()
frais.AjouteCoucheDense(8, 3)
frais.Charge("modele.json")
```

| Forme fluente | Forme préfixée | Rôle |
|---|---|---|
| `modèle.Sauve(chemin)` / `.Save` | `IAModèleSauve(modèle, chemin)` / `AIModelSave` | Écrit le modèle en JSON |
| `modèle.Charge(chemin)` / `.Load` | `IAModèleCharge(modèle, chemin)` / `AIModelLoad` | Recharge les poids dans un modèle |

Les deux formes sont équivalentes. Elles rendent `Vrai` en cas de
succès, `Faux` avec `ErreurDétectée` si le chemin est invalide ou le
fichier illisible.

---

## 7. Introspection

Trois accès exposent la structure interne, utiles pour vérifier un
modèle ou reproduire un calcul à la main.

| Accès | Rend |
|---|---|
| `modèle..NbCouches` / `..LayerCount` | Nombre total de couches empilées (denses + activations) |
| `modèle.Poids(n)` / `.Weights(n)` | `Tenseur` 2D des poids de la n-ième couche **dense** (base 1) |
| `modèle.Biais(n)` / `.Bias(n)` | `Tenseur` 1D des biais de la n-ième couche **dense** (base 1) |

L'indice `n` de `.Poids` / `.Biais` ne compte que les couches **denses**,
en ignorant les activations : `.Poids(1)` désigne la première couche
dense, quelle que soit sa position dans la pile.

```owl
poids est un Tenseur = modèle.Poids(1)
biais est un Tenseur = modèle.Biais(1)
```

---

## 8. Matrice de capacités

`IAModèle` est un socle qui **grandit par paliers**. Ce tableau fixe le
contrat public : ce qui fonctionne aujourd'hui, et ce qui viendra.
Chaque palier ajoute sa ligne à jour ; aucune date n'est promise.

| Capacité | Réseaux denses (aujourd'hui) | Vision / classification | Modèles de langage |
|---|---|---|---|
| Construire les couches | oui | oui | non (modèle externe) |
| Entraîner à partir de données | oui | à venir | non |
| Inférer | oui | oui (modèle importé) | oui (génération de texte) |
| Sauver / charger (format OWL) | oui | à venir | — |
| Charger un modèle externe | non | oui (ONNX) | oui (fichier GGUF) |
| Ajuster un modèle chargé | — | à venir | non |

Aujourd'hui, la colonne **réseaux denses** est complète : construire,
entraîner, inférer, sauver et charger fonctionnent de bout en bout. La
colonne **modèles de langage** est couverte par un type distinct, `LLM`,
qui charge un modèle de langage au format GGUF et génère du texte — voir
la fiche dédiée [`35_llm.md`](35_llm.md). La colonne **vision /
classification** progresse par deux fronts. Côté **import**, un réseau
convolutif préentraîné se charge depuis un fichier ONNX (`.ChargeONNX`) et
s'interroge aussitôt par `.Infère` ou, pour la localisation d'objets, par
`.Détecte` ([section 5](#5-détecter-des-objets)). Côté **entraînement
natif**, les couches convolutives (convolution 2D, pooling, normalisation de
lot, aplatissement) sont livrées comme briques ; l'assemblage d'un
classifieur d'images entraîné de bout en bout, sa fiche dédiée et la
persistance des couches convolutives au format OWL constituent le palier en
cours.

### Performance

Le calcul s'exécute sur processeur (CPU), sans accélération matérielle
dédiée. Les réseaux denses de taille modeste s'entraînent en quelques
secondes ; un réseau plus grand reste plus lent qu'un moteur accéléré —
contrainte réelle à prendre en compte dans le dimensionnement d'une
tâche.

---

## 9. Hors-périmètre actuel

Ces usages **ne sont pas encore supportés**. Ils sont nommés ici pour
qu'un besoin non couvert se comprenne comme un palier à venir, pas comme
un défaut :

- **Classifieur d'images de bout en bout** — les couches convolutives
  (convolution 2D, pooling, normalisation de lot, aplatissement) sont
  désormais disponibles comme briques ; en revanche l'assemblage complet
  d'un classifieur d'images entraîné et documenté, et la persistance
  (`Sauve`/`Charge`) des couches convolutives, restent un palier en cours.
- **Entraîner à partir d'un modèle importé** — un réseau chargé depuis un
  fichier ONNX (`.ChargeONNX`, [section 5](#5-détecter-des-objets)) sert à
  inférer et à détecter, mais son entraînement ou son ajustement natif n'est
  pas pris en charge : il est importé pour être utilisé, pas prolongé. La
  persistance au format OWL (`Sauve`/`Charge`, [section 6](#6-sauver-et-charger))
  reste réservée aux modèles construits dans le langage.
- **Modèles de langage dans `IAModèle`** — `IAModèle` ne construit ni
  n'entraîne un grand modèle de langage. Le chargement et la génération
  de texte par un modèle de langage existent, mais via un type dédié,
  `LLM` (fiche [`35_llm.md`](35_llm.md)), et non par `IAModèle`.

Un modèle dense reste toutefois **complet pour ce qu'il fait** :
construire, entraîner sur des données réelles, inférer et persister — le
cycle complet, illustré en [fin de fiche](#pour-aller-plus-loin).

---

## Pour aller plus loin

Un cas d'école complet : un réseau dense qui apprend à lire des chiffres
manuscrits (images 28 × 28). Sur un jeu réduit, il s'entraîne en quelques
secondes pour environ 90 % de justesse ; sur le jeu complet, ce même réseau
atteint environ 96,8 %. Le cycle **construire → entraîner → inférer**
s'écrit en une poignée de lignes.

| Sujet | Fiche |
|---|---|
| Charger un modèle de langage et générer du texte (`LLM`) | [`35_llm.md`](35_llm.md) |
| Le socle numérique `Tenseur` / `Vecteur` | [`33_vecteurs.md`](33_vecteurs.md) |
| Types de nombre et coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Gestion des exceptions (`QUAND EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
| Brancher un agent IA sur le langage | [`25_agents_ia.md`](25_agents_ia.md) |
