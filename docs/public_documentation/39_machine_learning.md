# Apprentissage automatique sur données tabulaires

## 1. En bref

Là où le [`Tenseur`](37_tenseurs_et_donnees.md) et l'[`IAModèle`](34_iamodele.md)
couvrent les réseaux de neurones, une seconde famille traite les **données
tabulaires** — des lignes d'observations décrites par des colonnes de
variables numériques. Elle rassemble les modèles classiques de
l'apprentissage automatique : arbres de décision, ensembles d'arbres,
régressions et regroupement.

Tous ces modèles partagent un même **patron estimateur** : un type préfixé
`ML`, réglé par ses propriétés, entraîné par `.Entraîne`, interrogé par
`.Prédit`. Cette fiche décrit ce patron, passe en revue les six modèles,
puis montre comment préparer les données et évaluer un modèle sur des
exemples qu'il n'a jamais vus.

```owl
// Un classifieur, du réglage à la prédiction, en quatre lignes.
X est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5]]
y est un Tenseur = Tenseur([[0.0], [0.0], [1.0], [1.0]])
arbre est un MLArbreDécision
arbre..Objectif = objClassification
arbre.Entraîne(X, y)
Trace("exactitude " + Exactitude(arbre.Prédit(X), y))       // 1
```

---

## 2. Le patron estimateur

Chaque modèle suit le même cycle de vie, quelle que soit la tâche.

1. **Déclarer** le modèle par son type `ML…`.
2. **Régler** ses propriétés en double point (`modèle..Propriété = …`) :
   nombre d'arbres, graine, profondeur maximale — chacune a un défaut
   raisonnable, à ajuster au besoin.
3. **Entraîner** avec `.Entraîne(X, y)`, où `X` est une `Matrice`
   d'observations (une ligne par exemple, une colonne par variable) et `y`
   un `Tenseur` colonne de cibles (une valeur par ligne de `X`).
4. **Prédire** avec `.Prédit(X)`, qui rend un `Tenseur` de prédictions.

`.Entraîne` et `.Prédit` sont des **méthodes** (point simple) ; les réglages
et les résultats appris sont des **propriétés** (double point). Un modèle
non supervisé comme `MLKMeans` s'entraîne sur `X` seul, sans cible.

### L'objectif explicite

Trois modèles savent traiter **deux tâches distinctes** — prédire une classe
(classification) ou une valeur continue (régression). Pour ceux-là,
l'objectif se règle explicitement avant l'entraînement, il n'est jamais
deviné :

```owl
// Le même type d'arbre, sur les deux objectifs.
Xc est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5]]
yc est un Tenseur = Tenseur([[0.0], [0.0], [1.0], [1.0]])
classif est un MLArbreDécision
classif..Objectif = objClassification
classif.Entraîne(Xc, yc)
Trace("classe prédite " + classif.Prédit(Xc)..Valeur[3])       // 1

Xr est une Matrice = [[1.0], [2.0], [3.0], [4.0], [5.0], [6.0]]
yr est un Tenseur = Tenseur([[10.0], [20.0], [30.0], [40.0], [50.0], [60.0]])
régress est un MLArbreDécision
régress..Objectif = objRégression
régress.Entraîne(Xr, yr)
Trace("R² régression " + Arrondi(CoefficientDétermination(régress.Prédit(Xr), yr), 3))       // 1
```

L'objectif prend l'une des deux valeurs `objClassification` ou
`objRégression`. Il doit être réglé avant `.Entraîne` : un modèle
multi-tâches sans objectif ne s'entraîne pas.

---

## 3. Les six modèles

| Modèle | Tâches | Réglages principaux |
|---|---|---|
| `MLArbreDécision` | classification, régression | `..Objectif`, `..ProfondeurMax`, `..MinÉchantillonsFeuille` |
| `MLForêtAléatoire` | classification, régression | `..Objectif`, `..NbArbres`, `..Graine` |
| `MLGradientBoosting` | classification, régression | `..Objectif`, `..NbArbres`, `..TauxApprentissage` |
| `MLRégressionLinéaire` | régression | `..Coefficients`, `..Ordonnée` (appris) |
| `MLRégressionLogistique` | classification | `..TauxApprentissage`, `..NbItérations` |
| `MLKMeans` | regroupement (non supervisé) | `..NbClusters`, `..Graine`, `..Centroïdes` (appris) |

Les trois premiers portent un `..Objectif` explicite. Les deux régressions
sont mono-tâche — l'une continue, l'autre binaire — et n'ont donc pas
d'objectif à régler. `MLKMeans` est non supervisé : il s'entraîne sans
cible.

### Arbre de décision

Un arbre découpe récursivement l'espace des variables par des seuils.
Lisible et sans mise à l'échelle préalable, il sert de brique de base aux
deux ensembles qui suivent.

```owl
X est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y est un Tenseur = Tenseur([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
arbre est un MLArbreDécision
arbre..Objectif = objClassification
arbre..ProfondeurMax = 3
arbre.Entraîne(X, y)
Trace("exactitude " + Exactitude(arbre.Prédit(X), y))       // 1
```

### Forêt aléatoire

Une forêt entraîne de nombreux arbres sur des tirages aléatoires des
données et des variables, puis agrège leurs votes. Plus robuste qu'un arbre
seul. La graine fixe la reproductibilité du tirage.

```owl
X est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y est un Tenseur = Tenseur([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
forêt est une MLForêtAléatoire
forêt..Objectif = objClassification
forêt..NbArbres = 50
forêt..Graine = 13
forêt.Entraîne(X, y)
Trace("exactitude " + Exactitude(forêt.Prédit(X), y))       // 1
```

### Gradient boosting

Le boosting empile des arbres peu profonds, chacun corrigeant les erreurs
résiduelles des précédents. Le taux d'apprentissage pondère la contribution
de chaque arbre.

```owl
X est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y est un Tenseur = Tenseur([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
boost est un MLGradientBoosting
boost..Objectif = objClassification
boost..TauxApprentissage = 0.1
boost.Entraîne(X, y)
Trace("exactitude " + Exactitude(boost.Prédit(X), y))       // 1
```

### Régression linéaire

La régression linéaire ajuste une combinaison pondérée des variables plus un
biais. Après entraînement, les poids appris se lisent en `..Coefficients`
(un par variable) et le biais en `..Ordonnée`.

```owl
X est une Matrice = [[1.0], [2.0], [3.0], [4.0]]
y est un Tenseur = Tenseur([[7.0], [9.0], [11.0], [13.0]])
lin est une MLRégressionLinéaire
lin.Entraîne(X, y)
Trace("coefficient " + Arrondi(lin..Coefficients[1], 2))       // 2
Trace("ordonnée " + Arrondi(lin..Ordonnée, 2))                 // 5
Trace("R² " + Arrondi(CoefficientDétermination(lin.Prédit(X), y), 3))       // 1
```

### Régression logistique

Malgré son nom, la régression logistique est un **classifieur** : elle
apprend une frontière linéaire entre classes par descente de gradient.

```owl
X est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
y est un Tenseur = Tenseur([[0.0], [0.0], [1.0], [1.0], [0.0], [1.0]])
logit est une MLRégressionLogistique
logit.Entraîne(X, y)
Trace("exactitude " + Exactitude(logit.Prédit(X), y))       // 1
```

### KMeans

`MLKMeans` regroupe les lignes par proximité, **sans étiquette** : il
s'entraîne sur `X` seul. `.Prédit` rend le numéro de cluster de chaque ligne
(basé sur 0), et les centres appris se lisent en `..Centroïdes`.

```owl
X est une Matrice = [[1.0, 2.0], [1.5, 1.8], [8.0, 8.0], [9.0, 8.5], [1.2, 2.1], [8.5, 9.0]]
km est un MLKMeans
km..NbClusters = 2
km..Graine = 13
km.Entraîne(X)
aff est un Tenseur = km.Prédit(X)
Trace("cluster de la ligne 1 " + aff..Valeur[1])       // 0
centres est une Matrice = km..Centroïdes
Trace("nombre de centres " + centres..Lignes)          // 2
```

---

## 4. Préparer les données

### Charger un CSV numérique

`CSVVersMatrice(csv, séparateur, ignoreEntête)` convertit un contenu CSV
tout-numérique — une chaîne déjà en mémoire, pas un chemin — en `Matrice`.

```owl
csv est une chaîne = "1.0;2.0" + CRLF + "3.0;4.0" + CRLF + "5.0;6.0"
X est une Matrice = CSVVersMatrice(csv, ";", Faux)
Trace("dimensions " + X..Lignes + " × " + X..Colonnes)       // 3 × 2
```

### Mettre les variables à la même échelle

Les modèles fondés sur des distances (dont `MLKMeans`) sont sensibles aux
amplitudes des variables. Deux mises à l'échelle, colonne par colonne :

| Native | Effet |
|---|---|
| `MatriceStandardiseColonnes(m)` | Centre-réduit chaque colonne : moyenne 0, écart-type 1 |
| `MatriceNormaliseColonnes(m)` | Ramène chaque colonne sur l'intervalle `[0, 1]` (min-max) |

```owl
csv est une chaîne = "1.0;2.0" + CRLF + "3.0;4.0" + CRLF + "5.0;6.0"
X est une Matrice = CSVVersMatrice(csv, ";", Faux)
Xstd est une Matrice = MatriceStandardiseColonnes(X)
Trace("standardisé [1][1] " + Arrondi(Xstd[1][1], 3))       // -1.225
Xnorm est une Matrice = MatriceNormaliseColonnes(X)
Trace("normalisé [1][1] " + Arrondi(Xnorm[1][1], 3))        // 0
Trace("normalisé [3][1] " + Arrondi(Xnorm[3][1], 3))        // 1
```

Chaque native rend une **nouvelle** `Matrice` ; la source n'est pas
modifiée. Une colonne constante sort à `0.0` partout, jamais indéfinie.

### Découper en entraînement et test

Évaluer honnêtement un modèle suppose de le juger sur des lignes **tenues à
part**, jamais vues à l'entraînement. `MatriceSousLignes(m, indices)` extrait
les lignes d'une `Matrice` par leurs indices (à partir de 1), dans l'ordre du
tableau et avec répétition permise : de quoi matérialiser un sous-ensemble
directement, sans boucle à la main.

```owl
X est une Matrice = [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0], [7.0, 8.0]]
Xtrain est une Matrice = MatriceSousLignes(X, [1, 2, 3])
Xtest est une Matrice = MatriceSousLignes(X, [4])
Trace("lignes d'entraînement " + Xtrain..Lignes)       // 3
Trace("test [1][2] " + Xtest[1][2])                    // 8
```

La même native matérialise le sous-ensemble de test. Pour brasser l'ordre
avant de découper, mélanger les indices avec une graine fixe (`InitHasard`)
garde la découpe reproductible.

Une cible se range en `Matrice` colonne avec `TableauVersMatriceColonne` : un
tableau plat de réels devient une `Matrice` `N × 1`, prête à passer en
`Tenseur`.

```owl
cibles est un tableau = [0.0, 1.0, 1.0, 0.0]
y est une Matrice = TableauVersMatriceColonne(cibles)
Trace("dimensions " + y..Lignes + " × " + y..Colonnes)       // 4 × 1
Trace("y[2][1] " + y[2][1])                                  // 1
```

---

## 5. Évaluer un modèle

Une prédiction se juge par des **métriques**, sans gradient : de simples
mesures de qualité. Les métriques de classification comparent des indices de
classe (basés sur 0) ; celles de régression comparent des valeurs continues.

| Métrique | Native | Tâche |
|---|---|---|
| Exactitude | `Exactitude(préd, cibles)` | Classification — fraction correctement classée, dans `[0, 1]` |
| Précision par classe | `Précision(préd, cibles)` | Classification — `Vecteur`, vrais positifs / prédits positifs |
| Rappel par classe | `Rappel(préd, cibles)` | Classification — `Vecteur`, vrais positifs / réels positifs |
| Score F1 par classe | `F1(préd, cibles)` | Classification — `Vecteur`, moyenne harmonique des deux |
| Coefficient de détermination | `CoefficientDétermination(préd, cibles)` | Régression — R², part de variance expliquée |
| Erreur quadratique moyenne | `ErreurQuadratiqueMoyenne(préd, cibles)` | Régression — moyenne des carrés des écarts |
| Erreur absolue moyenne | `ErreurAbsolueMoyenne(préd, cibles)` | Régression — moyenne des écarts absolus |

```owl
// Classification : 4 exemples, 2 classes.
préd est un Tenseur = Tenseur([0.0, 0.0, 1.0, 1.0])
cibles est un Tenseur = Tenseur([0.0, 1.0, 1.0, 1.0])
Trace("exactitude " + Arrondi(Exactitude(préd, cibles), 3))       // 0.75
précisions est un Vecteur = Précision(préd, cibles)
rappels est un Vecteur = Rappel(préd, cibles)
f1s est un Vecteur = F1(préd, cibles)
Trace("précision classe 1 " + Arrondi(précisions[2], 3))       // 1
Trace("rappel classe 1 " + Arrondi(rappels[2], 3))            // 0.667
Trace("F1 macro " + Arrondi(f1s.Moyenne(), 3))                // 0.733
```

`Précision`, `Rappel` et `F1` rendent un `Vecteur` indexé par classe
(position 1-based = classe 0-based + 1). La **moyenne macro** — moyenne non
pondérée sur les classes — s'obtient d'un `.Moyenne()` sur ce vecteur.

```owl
// Régression : prédictions parfaites -> R² = 1, erreurs nulles.
préd est un Tenseur = Tenseur([10.0, 20.0, 30.0])
cibles est un Tenseur = Tenseur([10.0, 20.0, 30.0])
Trace("R² " + Arrondi(CoefficientDétermination(préd, cibles), 3))       // 1
Trace("EQM " + Arrondi(ErreurQuadratiqueMoyenne(préd, cibles), 3))      // 0
Trace("EAM " + Arrondi(ErreurAbsolueMoyenne(préd, cibles), 3))          // 0
```

---

## 6. Le workflow complet

Bout à bout, un pipeline tabulaire enchaîne : fabrication ou chargement des
données, mise à l'échelle des variables, découpe train/test, entraînement de
plusieurs modèles sur le train, puis mesure sur le test tenu à part. Comparer
les modèles sur les mêmes lignes de test révèle celui qui généralise le
mieux.

Un exemple complet — jeu de données fabriqué à la volée, standardisation,
découpe, comparaison des six modèles sur les trois tâches (classification,
régression, regroupement) — accompagne le dossier de démonstration
`ml_tabulaire`. Il ne dépend d'aucun fichier externe et se termine en une
fraction de seconde.

---

## 7. Hors-périmètre actuel

Ces usages **ne sont pas encore couverts**. Ils sont nommés pour qu'un
besoin non traité se comprenne comme un palier à venir, pas comme un défaut
silencieux :

- **Validation croisée** — l'évaluation repose sur une découpe train/test
  composée à la main ; il n'y a pas de validation croisée en k blocs
  automatisée.
- **Réduction de dimension (PCA)** — l'analyse en composantes principales et
  les autres projections ne sont pas fournies.
- **Plus proches voisins et machines à vecteurs de support** — les modèles
  de type kNN et SVM ne font pas partie de cette famille.

---

## 8. Aliases anglais

| FR canon | EN canon |
|---|---|
| `MLArbreDécision` | `MLDecisionTree` |
| `MLForêtAléatoire` | `MLRandomForest` |
| `MLGradientBoosting` | `MLGradientBoosting` |
| `MLRégressionLinéaire` | `MLLinearRegression` |
| `MLRégressionLogistique` | `MLLogisticRegression` |
| `MLKMeans` | `MLKMeans` |
| `.Entraîne` / `.Prédit` | `.Train` / `.Predict` |
| `..Objectif` | `..Objective` |
| `objClassification` / `objRégression` | `objClassification` / `objRegression` |
| `..NbArbres` / `..Graine` | `..NumTrees` / `..Seed` |
| `..ProfondeurMax` | `..MaxDepth` |
| `..TauxApprentissage` / `..NbItérations` | `..LearningRate` / `..NumIterations` |
| `..Coefficients` / `..Ordonnée` | `..Coefficients` / `..Intercept` |
| `..NbClusters` / `..Centroïdes` | `..NumClusters` / `..Centroids` |
| `CSVVersMatrice` | `CSVToMatrix` |
| `MatriceStandardiseColonnes` | `MatrixStandardizeColumns` |
| `MatriceNormaliseColonnes` | `MatrixNormalizeColumns` |
| `MatriceSousLignes` | `MatrixSubRows` |
| `TableauVersMatriceColonne` | `ArrayToColumnMatrix` |
| `Exactitude` | `Accuracy` |
| `Précision` / `Rappel` / `F1` | `Precision` / `Recall` / `F1` |
| `CoefficientDétermination` | `RSquared` |
| `ErreurQuadratiqueMoyenne` | `MeanSquaredError` |
| `ErreurAbsolueMoyenne` | `MeanAbsoluteError` |

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Le socle `Tenseur`, autograd, pertes et données | [`37_tenseurs_et_donnees.md`](37_tenseurs_et_donnees.md) |
| Construire, entraîner et interroger un réseau (`IAModèle`) | [`34_iamodele.md`](34_iamodele.md) |
| Boucle d'entraînement bas niveau, évaluation et détection | [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md) |
| La `Matrice` et le `Vecteur`, socles numériques | [`33_vecteurs.md`](33_vecteurs.md) |
| Types de nombre et coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
