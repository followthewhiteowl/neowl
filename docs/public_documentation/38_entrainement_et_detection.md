# Entraînement bas niveau, évaluation et détection

## 1. En bref

Le type [`IAModèle`](34_iamodele.md) offre un entraînement clé en main
(`.Entraîne` avec optimiseur, planification du taux, validation et arrêt
anticipé). Cette fiche descend d'un cran : elle montre comment **écrire
soi-même la boucle d'entraînement** à partir des tenseurs et de
l'autograd ([`37_tenseurs_et_donnees.md`](37_tenseurs_et_donnees.md)),
comment **mesurer** la qualité d'un modèle, et comment assembler une
**détection d'objets entraînable de bout en bout** — la tâche que
`IAModèle` sait inférer, ici apprise à partir de zéro.

Trois briques structurent le propos : la boucle d'apprentissage
explicite, les métriques d'évaluation, puis le pipeline complet de
détection.

---

## 2. Une boucle d'entraînement explicite

Écrire la boucle à la main donne le contrôle total sur ce qui est
appris. Le motif est toujours le même à chaque pas : **prédire**,
**mesurer la perte**, **rétropropager**, **avancer les paramètres**.
L'exemple ci-dessous apprend une relation linéaire `y = 2x` par un seul
poids scalaire, en observant la perte fondre.

```owl
// Régression linéaire 1D par autograd explicite : apprendre y = 2x.
InitHasard(3)
entrées est un Tenseur = Tenseur([1.0, 2.0, 3.0, 4.0])
cibles est un Tenseur = Tenseur([2.0, 4.0, 6.0, 8.0])
w est un Tenseur = Tenseur([0.0])
taux est un réel = 0.05
perte0 est un réel = 0.0
perteF est un réel = 0.0
POUR ép = 1 À 200
	préd est un Tenseur = entrées * w              // diffusion : w appliqué à chaque entrée
	perte est un Tenseur = préd.ErreurQuadratique(cibles)
	perte.Rétropropage()
	SI ép = 1 ALORS
		perte0 = perte..Valeur[1]
	FIN
	perteF = perte..Valeur[1]
	w      = w.DescenteGradient(taux)
FIN
Trace("perte initiale " + Arrondi(perte0, 4))       // 30
Trace("perte finale " + Arrondi(perteF, 6))         // 0
Trace("w appris " + Arrondi(w..Valeur[1], 3))       // 2
```

Chaque itération réaffecte `w` par la feuille rendue par
`.DescenteGradient` : le nouveau tenseur repart avec un gradient à zéro,
détaché du graphe précédent, prêt pour le pas suivant. La même mécanique
s'étend à un réseau entier — plusieurs tenseurs de poids, une passe
avant composée de convolutions et d'activations — comme dans la
détection en [section 4](#4-détection-entraînable-de-bout-en-bout).

---

## 3. Évaluer un modèle

Une fois un modèle entraîné, plusieurs **métriques** en jugent la
qualité. Toutes sont un simple comptage, sans gradient : ce sont des
mesures d'évaluation, pas des pertes. Les prédictions sont soit un
`Tenseur` 2D `lot × classes` (la classe retenue par ligne étant celle du
plus grand score), soit un `Tenseur` 1D d'indices ; les cibles sont un
`Tenseur` 1D d'indices attendus, **basés sur 0**.

| Métrique | Native | Rend |
|---|---|---|
| Exactitude | `Exactitude(préd, cibles)` | Fraction d'exemples correctement classés, dans `[0, 1]` |
| Matrice de confusion | `MatriceDeConfusion(préd, cibles)` | `Matrice` `classes × classes` : réel en ligne, prédit en colonne |
| Précision par classe | `Précision(préd, cibles)` | `Vecteur` — vrais positifs / prédits positifs |
| Rappel par classe | `Rappel(préd, cibles)` | `Vecteur` — vrais positifs / réels positifs |
| Score F1 par classe | `F1(préd, cibles)` | `Vecteur` — moyenne harmonique de précision et rappel |

```owl
// 4 exemples, 3 classes. Scores (logits) 2D lot x classes.
scores est un Tenseur = Tenseur([3.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 3.0]).Redimensionne([4, 3])
cibles est un Tenseur = Tenseur([0.0, 1.0, 2.0, 2.0])
Trace("Exactitude " + Arrondi(Exactitude(scores, cibles), 3))       // 0.75
conf est une Matrice = MatriceDeConfusion(scores, cibles)
Trace("conf[3][2] (réel 2 prédit 1) " + conf[3][2])                 // 1
précisions est un Vecteur = Précision(scores, cibles)
rappels est un Vecteur = Rappel(scores, cibles)
scoresF1 est un Vecteur = F1(scores, cibles)
Trace("Précision classe 0 " + Arrondi(précisions[1], 3))            // 1
Trace("Rappel macro " + Arrondi(rappels.Moyenne(), 3))              // 0.833
Trace("F1 macro " + Arrondi(scoresF1.Moyenne(), 3))                 // 0.778
```

`Précision`, `Rappel` et `F1` rendent un `Vecteur` indexé par classe
(position 1-based = classe 0-based + 1). La **moyenne macro** — moyenne
non pondérée sur les classes — s'obtient d'un `.Moyenne()` sur ce
vecteur. Une classe sans instance vaut `0.0`, jamais une valeur
indéfinie. La `MatriceDeConfusion` se lit en position 1-based : la
cellule `[i][j]` compte les exemples de classe réelle `i - 1` prédits en
classe `j - 1`.

---

## 4. Détection entraînable de bout en bout

Détecter des objets, c'est trouver **où** ils sont et **de quelle
classe**. Un détecteur à grille divise l'image en cellules et fait
prédire à chacune une boîte, une **présence d'objet** (objectness) et
une classe. L'entraîner demande trois pièces : fabriquer les cibles par
cellule, composer une perte qui ne pénalise que les bonnes cellules,
puis mesurer la qualité par la précision moyenne (mAP).

### Fabriquer les cibles par cellule

`AssocieCiblesDétection(boîtes, hauteurGrille, largeurGrille, nbClasses)`
transforme des boîtes annotées en un `Tenseur` de cibles `[H×W,
4 + 1 + K]`, rangé en ordre ligne-major. Chaque boîte est assignée à la
**cellule responsable** — celle qui contient son centre. Le centre y est
stocké en coordonnées **relatives à la cellule**, ce qui rend la
position apprenable par une tête à poids partagés ; largeur et hauteur
restent en fraction de l'image entière.

```owl
// Une boîte classe 0, centre (0.7,0.3), taille 0.4x0.3, sur grille 2x2, K=1 classe.
boîtes est un tableau = [[0.0, 0.7, 0.3, 0.4, 0.3]]
cible est un Tenseur = AssocieCiblesDétection(boîtes, 2, 2, 1)
// cellule responsable : col=plancher(0.7*2)=1, ligne=plancher(0.3*2)=0
// index = ligne*W+col = 0*2+1 = 1 (0-based) -> ligne 2 (1-based)
obj est un Tenseur = cible.TrancheCanaux(5, 1)
Trace(obj..Valeur[2][1])         // 1    — cellule responsable
Trace(obj..Valeur[1][1])         // 0    — cellule de fond
boîteCible est un Tenseur = cible.TrancheCanaux(1, 4)
Trace(Arrondi(boîteCible..Valeur[2][1], 3))       // 0.4   — cx relatif : 0.7*2 - 1
Trace(Arrondi(boîteCible..Valeur[2][2], 3))       // 0.6   — cy relatif : 0.3*2 - 0
```

Le canal d'objectness (canal 5) sert **aussi de masque** : il vaut 1 sur
la cellule responsable, 0 partout ailleurs.

### Composer une perte masquée

La perte additionne deux termes : l'objectness sur **toutes** les
cellules (entropie croisée binaire) et la boîte sur la **seule** cellule
responsable (L1). Le masque, diffusé sur les quatre canaux de boîte
([diffusion](37_tenseurs_et_donnees.md#la-diffusion-broadcasting)),
annule la contribution des cellules de fond.

```owl
boîtes est un tableau = [[0.0, 0.7, 0.3, 0.4, 0.3]]
cible est un Tenseur = AssocieCiblesDétection(boîtes, 2, 2, 1)
cibleBoîte est un Tenseur = cible.TrancheCanaux(1, 4)
cibleObj est un Tenseur = cible.TrancheCanaux(5, 1)
masque est un Tenseur = cible.TrancheCanaux(5, 1)

// Prédiction fictive à zéro (avant tout apprentissage).
prédObj est un Tenseur = Tenseur([0.0, 0.0, 0.0, 0.0]).Redimensionne([4, 1])
prédBoîte est un Tenseur = Tenseur([0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]).Redimensionne([4, 4])

perte est un Tenseur = prédObj.EntropieCroiséeBinaire(cibleObj) + (prédBoîte * masque).L1(cibleBoîte)
perte.Rétropropage()
Trace("perte " + Arrondi(perte..Valeur[1], 4))       // 0.7994
```

### Mesurer par la précision moyenne (mAP)

`PrécisionMoyenne(détections, vérités, seuilIoU)` calcule la précision
moyenne d'un détecteur. Les détections sont des `[classe, score, x, y,
largeur, hauteur]`, les vérités des `[classe, x, y, largeur, hauteur]`,
boîtes en coin dans le même espace. Chaque détection est appariée de
façon gloutonne à la meilleure vérité libre dont le recouvrement (IoU)
atteint le seuil.

```owl
détections est un tableau = [[0.0, 0.9, 0.10, 0.10, 0.20, 0.20], [0.0, 0.3, 0.60, 0.60, 0.20, 0.20]]
vérités est un tableau = [[0.0, 0.11, 0.11, 0.20, 0.20]]
Trace("mAP " + Arrondi(PrécisionMoyenne(détections, vérités, 0.5), 3))       // 1

recouvre est un réel = IoU([10.0, 10.0, 50.0, 50.0], [10.0, 10.0, 50.0, 50.0])
Trace("IoU " + recouvre)          // 1    — boîtes identiques
gardés est un tableau = SuppressionNonMax([[10.0, 10.0, 50.0, 50.0], [12.0, 12.0, 48.0, 48.0]], [0.9, 0.7], 0.5)
Trace("NMS gardés " + Taille(gardés))       // 1    — la 2e boîte, quasi confondue, est écartée
```

`IoU` et `SuppressionNonMax` sont les briques réutilisables du filtrage
de détections (voir aussi [`34_iamodele.md`](34_iamodele.md#5-détecter-des-objets)
pour l'inférence d'un détecteur importé).

### Un exemple complet

Un détecteur entraîné **de bout en bout** — génération d'un jeu
synthétique, passe avant à deux convolutions, boucle d'entraînement,
mAP qui monte au fil des époques, puis rendu des boîtes prédites —
accompagne le dossier de démonstration `detection_entrainement`. Il
n'exige aucun fichier externe et se termine en quelques secondes.

---

## 5. Le cap : entraîner ailleurs, exécuter ici

L'apprentissage automatique du langage suit une ligne claire : **importer
et exécuter** de vrais modèles localement, et **entraîner léger** sur
processeur. Un modèle lourd s'entraîne là où la puissance est (matériel
dédié), puis s'importe au format ONNX pour être exécuté sans dépendance
externe ([`34_iamodele.md`](34_iamodele.md)). Sur processeur, le langage
couvre l'entraînement de réseaux de taille modeste, l'apprentissage par
transfert (réutiliser un corps figé, réapprendre une tête), et la
détection légère montrée ici.

Le calcul s'exécute sur processeur, sans accélération matérielle dédiée :
un petit réseau s'entraîne en quelques secondes ; un grand réseau reste
plus lent qu'un moteur accéléré — contrainte à intégrer au
dimensionnement d'une tâche.

---

## 6. Hors-périmètre actuel

Ces usages **ne sont pas encore couverts**. Ils sont nommés pour qu'un
besoin non traité se comprenne comme un palier à venir, pas comme un
défaut silencieux :

- **Segmentation d'images** — délimiter les objets au pixel près n'est
  pas pris en charge ; la détection rend des boîtes, pas des masques.
- **Génération d'images** — le langage entraîne et interroge des réseaux
  de reconnaissance, il ne synthétise pas d'images.
- **Entraîner un modèle de langage** — le chargement et la génération de
  texte existent par un type dédié, `LLM` ([`35_llm.md`](35_llm.md)),
  mais l'entraînement d'un tel modèle est hors périmètre.
- **Accélération matérielle (GPU)** — tout le calcul s'exécute sur
  processeur.

---

## 7. Aliases anglais

| FR canon | EN canon |
|---|---|
| `Exactitude` | `Accuracy` |
| `MatriceDeConfusion` | `ConfusionMatrix` |
| `Précision` / `Rappel` / `F1` | `Precision` / `Recall` / `F1` |
| `PrécisionMoyenne` | `MeanAveragePrecision` |
| `AssocieCiblesDétection` | `AssignDetectionTargets` |
| `IoU` | `IoU` |
| `SuppressionNonMax` | `NonMaxSuppression` |
| `.Rétropropage` / `.DescenteGradient` | `.Backpropagate` / `.GradientDescent` |
| `.Moyenne` | `.Average` |

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Le socle `Tenseur`, autograd, pertes et données | [`37_tenseurs_et_donnees.md`](37_tenseurs_et_donnees.md) |
| Construire, entraîner et inférer un réseau (`IAModèle`) | [`34_iamodele.md`](34_iamodele.md) |
| Détecter des objets avec un modèle importé | [`34_iamodele.md`](34_iamodele.md#5-détecter-des-objets) |
| Charger un modèle de langage et générer du texte (`LLM`) | [`35_llm.md`](35_llm.md) |
| Gestion des exceptions (`QUAND EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
