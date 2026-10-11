# Tenseurs et données : le socle de l'apprentissage

## 1. En bref

Un `Tenseur` est le **conteneur numérique différentiable** sur lequel
repose l'apprentissage automatique dans le langage. Là où le
[`Vecteur`](33_vecteurs.md) est une suite de réels à une dimension, le
`Tenseur` prend une **forme quelconque** — scalaire, 1D, 2D (une
matrice), ou N dimensions — et **mémorise le graphe des calculs** qui
l'ont produit. C'est ce graphe qui permet de remonter automatiquement
les gradients, l'ingrédient central de l'entraînement d'un réseau.

Cette fiche décrit deux socles complémentaires : le `Tenseur` en
profondeur (formes, opérations, autograd, pertes), puis les **données**
prêtes pour l'apprentissage (`Dataset`, images étiquetées ou annotées,
chargeurs, lots, découpe, augmentation). L'assemblage de ces briques en
un réseau entraîné est décrit dans [`34_iamodele.md`](34_iamodele.md) ;
la boucle d'entraînement bas niveau et la détection d'objets
entraînable dans [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md).

```owl
t est un Tenseur = Tenseur([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Redimensionne([2, 3])
Trace(t..Valeur[1][1])       // 1
Trace(t..Valeur[2][3])       // 6
```

---

## 2. Le `Tenseur` au-delà du `Vecteur`

Un `Tenseur` se construit à partir d'un **tableau plat de réels** passé
à `Tenseur([...])` : la donnée fournie devient sa valeur, son gradient
démarre à zéro. Un tel tenseur est **scalaire ou 1D**. Pour une forme 2D
ou N-D, enchaîner `.Redimensionne([forme])` : les éléments et leur ordre
de lecture ne changent pas — le premier axe varie le plus lentement, le
dernier le plus vite —, seule leur disposition change. Le produit des
dimensions demandées doit égaler le nombre d'éléments.

```owl
plat est un Tenseur = Tenseur([1.0, 2.0, 3.0, 4.0, 5.0, 6.0])
image est un Tenseur = plat.Redimensionne([2, 3])
Trace(image..Valeur[1][2])       // 2
Trace(image..Valeur[2][1])       // 4
```

Quatre accès exposent le tenseur, tous en double point :

| Propriété | Rend |
|---|---|
| `t..Valeur` / `t..Value` | La donnée, sous forme de réel, `Vecteur` (1D), `Matrice` (2D) ou imbriquée (N-D) |
| `t..Gradient` / `t..Gradient` | Le gradient, de **même forme** que la valeur, à zéro tant qu'aucune rétropropagation n'a eu lieu |
| `t..Forme` / `t..Shape` | La séquence des tailles d'axes, en tableau 1-based |
| `t..Rang` / `t..Rank` | Le nombre d'axes |

```owl
t est un Tenseur = Tenseur([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Redimensionne([2, 3])
Trace(t..Rang)               // 2
Trace(t..Forme[1])           // 2    — 2 lignes
Trace(t..Forme[2])           // 3    — 3 colonnes
```

Le `Vecteur` reste le bon outil pour la géométrie et les statistiques du
quotidien ([`33_vecteurs.md`](33_vecteurs.md)) ; le `Tenseur` prend le
relais dès qu'il faut plusieurs dimensions ou une dérivation
automatique.

---

## 3. Opérations et diffusion

Les opérateurs `+` et `*` entre deux tenseurs opèrent **élément par
élément** et **enchaînent le graphe** de calcul, condition de l'autograd
([section 4](#4-lautograd-des-gradients-sans-les-écrire)).

```owl
u est un Tenseur = Tenseur([1.0, 2.0, 3.0])
v est un Tenseur = Tenseur([10.0, 20.0, 30.0])
Trace((u + v)..Valeur[1])        // 11
Trace((u * v)..Valeur[3])        // 90
```

### La diffusion (broadcasting)

Quand un axe vaut `1` d'un côté et davantage de l'autre, la valeur est
**diffusée** le long de cet axe : une colonne s'applique à toutes les
colonnes, un scalaire à tous les éléments. La diffusion évite de
recopier la donnée à la main et sert notamment à masquer une sortie.

```owl
mat est un Tenseur = Tenseur([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Redimensionne([2, 3])
col est un Tenseur = Tenseur([10.0, 100.0]).Redimensionne([2, 1])
prod est un Tenseur = mat * col       // chaque ligne multipliée par son scalaire
Trace(prod..Valeur[1][3])        // 30   — ligne 1 × 10
Trace(prod..Valeur[2][1])        // 400  — ligne 2 × 100
```

### Réductions et réagencements

| Opération | Native / méthode | Effet |
|---|---|---|
| Somme le long d'un axe | `TenseurSomme(t, axe)` / `t.Somme(axe)` | Réduit l'axe (1-based), abaisse le rang de 1 |
| Moyenne le long d'un axe | `TenseurMoyenne(t, axe)` / `t.Moyenne(axe)` | Réduit l'axe, abaisse le rang de 1 |
| Réordonner tous les axes | `TenseurPermute(t, ordre)` / `t.Permute(ordre)` | Permutation complète des axes |
| Échanger deux axes | `TenseurTranspose(t, a, b)` / `t.Transpose(a, b)` | Transposition généralisée |
| Extraire des canaux | `TenseurTrancheCanaux(t, début, longueur)` / `t.TrancheCanaux(début, longueur)` | Plage contiguë sur l'axe canal (2e axe), `début` 1-based |

```owl
m est un Tenseur = Tenseur([1.0, 2.0, 3.0, 4.0, 5.0, 6.0]).Redimensionne([2, 3])
colonnes est un Tenseur = m.Somme(1)          // réduit les lignes -> 3 colonnes
Trace(colonnes..Valeur[1])       // 5    — 1 + 4
Trace(colonnes..Valeur[3])       // 9    — 3 + 6
lignes est un Tenseur = m.Moyenne(2)          // réduit les colonnes -> 2 lignes
Trace(lignes..Valeur[1])         // 2    — (1 + 2 + 3) / 3
Trace(lignes..Valeur[2])         // 5    — (4 + 5 + 6) / 3
```

`TrancheCanaux` sert à découper une sortie multi-canaux en groupes — par
exemple séparer la tête d'un détecteur en boîte, présence d'objet et
classe (voir [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md)).
Toutes ces opérations sont différentiables : la rétropropagation renvoie
chaque gradient à sa position d'origine.

---

## 4. L'autograd : des gradients sans les écrire

Chaque tenseur produit par une opération se souvient de ses parents. Un
appel `.Rétropropage()` sur un tenseur scalaire parcourt ce graphe **à
rebours** et remplit le `..Gradient` de chaque tenseur intervenu, par la
règle de la chaîne. Aucune dérivée n'est écrite à la main.

```owl
x est un Tenseur = Tenseur([1.0, 2.0, 3.0])
perte est un Tenseur = (x * x).Somme(1)       // somme des carrés
perte.Rétropropage()
Trace(x..Gradient[1])            // 2    — d(x²)/dx = 2x
Trace(x..Gradient[2])            // 4
Trace(x..Gradient[3])            // 6
```

Une fois les gradients remplis, `.DescenteGradient(taux)` applique **un
pas d'optimisation** à un tenseur de paramètres : il rend une nouvelle
feuille égale à `valeur - taux × gradient`, dont le gradient repart à
zéro, détachée du graphe précédent. C'est le geste élémentaire de tout
entraînement.

```owl
w est un Tenseur = Tenseur([1.0, 2.0, 3.0])
perte est un Tenseur = (w * w).Somme(1)
perte.Rétropropage()
wMaj est un Tenseur = w.DescenteGradient(0.1)      // w - 0.1 × 2w
Trace(Arrondi(wMaj..Valeur[1], 4))       // 0.8
Trace(Arrondi(wMaj..Valeur[2], 4))       // 1.6
Trace(Arrondi(wMaj..Valeur[3], 4))       // 2.4
```

---

## 5. Fonctions de perte

Une **perte** mesure l'écart entre une prédiction et une cible et rend
un `Tenseur` scalaire, point de départ de la rétropropagation. La cible
est une vérité fixe : elle ne reçoit jamais de gradient.

| Perte | Native / méthode | Usage |
|---|---|---|
| Erreur quadratique (MSE) | `TenseurErreurQuadratique(p, c)` / `p.ErreurQuadratique(c)` | Régression : moyenne des carrés des écarts |
| Entropie croisée | `TenseurEntropieCroisée(p, c)` / `p.EntropieCroisée(c)` | Classification : logits contre cible un-parmi-n, softmax fusionnée |
| Entropie croisée binaire (BCE) | `TenseurEntropieCroiséeBinaire(p, c)` / `p.EntropieCroiséeBinaire(c)` | Score binaire de présence : logit contre cible dans `[0, 1]`, sigmoïde fusionnée |
| Erreur absolue (L1) | `TenseurL1(p, c)` / `p.L1(c)` | Régression robuste aux valeurs aberrantes, adaptée à une boîte englobante |

```owl
préd est un Tenseur = Tenseur([2.0, 0.0, 0.0])
cible est un Tenseur = Tenseur([1.0, 0.0, 0.0])
Trace(Arrondi(préd.ErreurQuadratique(cible)..Valeur[1], 4))       // 0.3333
Trace(Arrondi(préd.L1(cible)..Valeur[1], 4))                      // 0.3333
```

L'entropie croisée reçoit des **logits** (scores bruts, avant softmax)
et une cible un-parmi-n ; sa variante binaire reçoit un logit et une
cible entre 0 et 1. La fusion interne de la softmax (ou de la sigmoïde)
avec la perte évite tout débordement numérique.

```owl
logits est un Tenseur = Tenseur([2.0, 0.0, 0.0])
cible est un Tenseur = Tenseur([1.0, 0.0, 0.0])
Trace(Arrondi(logits.EntropieCroisée(cible)..Valeur[1], 4))       // 0.2395

logit est un Tenseur = Tenseur([2.0])
présence est un Tenseur = Tenseur([1.0])
Trace(Arrondi(logit.EntropieCroiséeBinaire(présence)..Valeur[1], 4))    // 0.1269
```

Une boucle d'entraînement complète, qui enchaîne prédiction, perte,
rétropropagation et pas de descente, est déroulée dans
[`38_entrainement_et_detection.md`](38_entrainement_et_detection.md).

---

## 6. Les données : `Dataset` et images

Un `Dataset` est une **collection ordonnée d'exemples** prête pour
l'apprentissage. Il se déclare avec le type de ses éléments —
`Dataset de ImageÉtiquetée` pour la classification, `Dataset de
ImageAnnotée` pour la détection — et se remplit par un chargeur.

Une **`ImageÉtiquetée`** porte un lien disque `..Chemin`, une
`..Étiquette` (nom de classe gravé à l'import) et une `..Image`
**matérialisée paresseusement** : le fichier n'est lu qu'au premier
accès à `..Image`, pas à l'import. Une **`ImageAnnotée`** porte à la
place des `..Boîtes` (tableau de `[classe, cx, cy, largeur, hauteur]`
normalisé dans `[0, 1]`), pour la détection d'objets.

| Chargeur | Source |
|---|---|
| `ChargeImagesÉtiquetéesDepuisDossier(racine)` | Arborescence « un dossier = une classe », classes triées par ordre alphabétique |
| `ChargeImagesÉtiquetéesDepuisCSV(fichier)` | Manifeste `chemin;étiquette`, une ligne par exemple, ordre préservé |
| `ChargeImagesAnnotéesDepuisYOLO(dossier)` | Images + fichiers `.txt` de boîtes (`classe cx cy largeur hauteur`) |

```owl
racine est une chaîne = fRepTemp() + "\doc_ia_ds"
fRepCree(racine + "\chat")
fRepCree(racine + "\chien")
dSauveImage(dCréeImage(4, 4, RVB(0, 200, 0)), racine + "\chat\c1.png")
dSauveImage(dCréeImage(4, 4, RVB(0, 200, 0)), racine + "\chat\c2.png")
dSauveImage(dCréeImage(4, 4, RVB(200, 200, 0)), racine + "\chien\d1.png")

jeu est un Dataset de ImageÉtiquetée = ChargeImagesÉtiquetéesDepuisDossier(racine)
Trace(jeu..Taille)               // 3
Trace(jeu..NbClasses)            // 2
Trace(jeu..Classes[1])           // chat   — ordre alphabétique
ex est un ImageÉtiquetée = jeu[1]
Trace(ex..Étiquette)             // chat
fRepSupprime(racine, frRécursif)
```

Un `Dataset` expose `..Taille`, `..NbClasses`, `..Classes` (tableau
trié) et `..Source` ; l'indexation `jeu[i]` rend l'exemple de rang `i`.

---

## 7. Du disque au tenseur

Le pont entre une image et le réseau est `ImageVersTenseur`, qui rend un
`Tenseur` **canaux-d'abord** `C × H × W` : les valeurs de pixel sont
normalisées dans `[0, 1]` (divisées par 255) sur 3 canaux RVB, le canal
alpha éventuel étant ignoré. L'image source n'est pas modifiée.

```owl
img est une Image = dCréeImage(2, 2, RVB(255, 0, 0))
t est un Tenseur = ImageVersTenseur(img)
Trace(t..Valeur[1][1][1])        // 1    — canal rouge saturé
Trace(t..Valeur[2][1][1])        // 0    — canal vert
Trace(t..Valeur[3][1][1])        // 0    — canal bleu
```

Pour l'entraînement, un `Dataset` fournit les données **par lots**, sans
matérialiser tout le jeu d'un coup :

| Méthode | Rend |
|---|---|
| `jeu.NombreLots(taille)` / `.BatchCount` | Nombre de lots pour couvrir tous les exemples (le dernier peut être partiel) |
| `jeu.LotEntrées(taille, i)` / `.BatchInputs` | Le i-ème lot d'entrées, `Tenseur` `B × C × H × W` |
| `jeu.LotCibles(taille, i)` / `.BatchTargets` | Le i-ème lot de cibles, indices de classe |
| `jeu.SousEnsemble(début, fin)` / `.Subset` | Tranche `[début, fin[` en fractions, en un nouveau `Dataset` |

```owl
racine est une chaîne = fRepTemp() + "\doc_ia_lots"
fRepCree(racine + "\a")
fRepCree(racine + "\b")
POUR n = 1 À 3
	dSauveImage(dCréeImage(4, 4, RVB(0, 200, 0)), racine + "\a\a" + n + ".png")
	dSauveImage(dCréeImage(4, 4, RVB(200, 0, 0)), racine + "\b\b" + n + ".png")
FIN
jeu est un Dataset de ImageÉtiquetée = ChargeImagesÉtiquetéesDepuisDossier(racine)
Trace(jeu..Taille)               // 6
Trace(jeu.NombreLots(4))         // 2    — 6 exemples / lot de 4 = 2 lots
X est un Tenseur = jeu.LotEntrées(4, 1)
Y est un Tenseur = jeu.LotCibles(4, 1)
Trace(Y..Valeur[1])              // 0    — indice de classe du 1er exemple
sous est un Dataset de ImageÉtiquetée = jeu.SousEnsemble(0.0, 0.5)
Trace(sous..Taille)              // 3    — première moitié
fRepSupprime(racine, frRécursif)
```

`.SousEnsemble` sert un **partage train / test déterministe** : deux
tranches disjointes du même jeu. Pour brasser l'ordre avant de découper,
`.Mélange()` réordonne le jeu ; le tirage suit la graine (`InitHasard`),
donc reste reproductible.

### Augmentation à la volée

Deux méthodes, **opt-in** et appliquées au vol lors de la construction
de chaque lot, diversifient un jeu d'apprentissage sans grossir le
disque :

| Méthode | Effet |
|---|---|
| `jeu.RetournementHorizontalAléatoire()` / `.RandomHorizontalFlip` | Miroir horizontal, probabilité 0,5 par image |
| `jeu.RecadrageAléatoire(marge)` / `.RandomCrop` | Zéro-remplissage de `marge` px puis recadrage aléatoire à la taille d'origine |

```owl
InitHasard(1)
racine est une chaîne = fRepTemp() + "\doc_ia_aug"
fRepCree(racine + "\a")
POUR n = 1 À 4
	dSauveImage(dCréeImage(8, 8, RVB(0, 200, 0)), racine + "\a\a" + n + ".png")
FIN
jeu est un Dataset de ImageÉtiquetée = ChargeImagesÉtiquetéesDepuisDossier(racine)
jeu.Mélange()
jeu.RetournementHorizontalAléatoire()
jeu.RecadrageAléatoire(4)
X est un Tenseur = jeu.LotEntrées(4, 1)          // lot augmenté à la volée
Trace(jeu..Taille)               // 4
fRepSupprime(racine, frRécursif)
```

L'augmentation ne s'applique qu'au jeu d'apprentissage ; le jeu
d'évaluation reste tel quel, pour une mesure honnête.

Un exemple complet qui charge un `Dataset` d'images depuis un dossier,
le découpe, l'augmente et entraîne un réseau convolutif dessus accompagne
le dossier de démonstration `reseau_cifar`.

---

## 8. Aliases anglais

| FR canon | EN canon |
|---|---|
| `Tenseur` | `Tensor` |
| `..Valeur` / `..Gradient` | `..Value` / `..Gradient` |
| `..Forme` / `..Rang` | `..Shape` / `..Rank` |
| `.Redimensionne` | `.Reshape` (`TensorReshape`) |
| `.Somme` / `.Moyenne` | `.Sum` / `.Mean` (`TensorSum` / `TensorMean`) |
| `.Permute` / `.Transpose` | `.Permute` / `.Transpose` (`TensorPermute` / `TensorTranspose`) |
| `.TrancheCanaux` | `.ChannelSlice` (`TensorChannelSlice`) |
| `.Rétropropage` | `.Backpropagate` |
| `.DescenteGradient` | `.GradientDescent` (`TensorGradientDescent`) |
| `.ErreurQuadratique` | `.MeanSquaredError` (`TensorMeanSquaredError`) |
| `.EntropieCroisée` | `.CrossEntropy` (`TensorCrossEntropy`) |
| `.EntropieCroiséeBinaire` | `.BinaryCrossEntropy` (`TensorBinaryCrossEntropy`) |
| `.L1` | `.L1` (`TensorL1`) |
| `Dataset` | `Dataset` |
| `ImageÉtiquetée` / `ImageAnnotée` | `LabeledImage` / `AnnotatedImage` |
| `ImageVersTenseur` | `ImageToTensor` |
| `ChargeImagesÉtiquetéesDepuisDossier` | `LoadLabeledImagesFromFolder` |
| `ChargeImagesÉtiquetéesDepuisCSV` | `LoadLabeledImagesFromCSV` |
| `ChargeImagesAnnotéesDepuisYOLO` | `LoadAnnotatedImagesFromYOLO` |
| `..Taille` / `..NbClasses` / `..Classes` (Dataset) | `..Count` / `..ClassCount` / `..Classes` |
| `.NombreLots` / `.LotEntrées` / `.LotCibles` | `.BatchCount` / `.BatchInputs` / `.BatchTargets` |
| `.SousEnsemble` / `.Mélange` | `.Subset` / `.Shuffle` |
| `.RetournementHorizontalAléatoire` / `.RecadrageAléatoire` | `.RandomHorizontalFlip` / `.RandomCrop` |

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Le `Vecteur`, socle numérique 1D | [`33_vecteurs.md`](33_vecteurs.md) |
| Construire, entraîner et interroger un réseau (`IAModèle`) | [`34_iamodele.md`](34_iamodele.md) |
| Boucle d'entraînement bas niveau, évaluation et détection | [`38_entrainement_et_detection.md`](38_entrainement_et_detection.md) |
| Types de nombre et coercions | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Gestion des exceptions (`QUAND EXCEPTION`) | [`08_exceptions.md`](08_exceptions.md) |
