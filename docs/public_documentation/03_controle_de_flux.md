# Contrôle de flux

OWL fournit six constructions de contrôle : `SI`, `POUR`, `POUR TOUT`,
`TANTQUE`, `BOUCLE` et `SELON`. Toutes terminent par `FIN`. Les
conditions doivent être booléennes, numériques ou `Null` — toute autre
valeur produit une erreur d'exécution explicite (voir
`01_types_et_coercions.md` §2).

---

## 1. `SI`

```owl
SI age >= 18 ALORS
    Trace("Majeur")
SINON
    Trace("Mineur")
FIN
```

Le bloc `SINON` est optionnel. Plusieurs branches s'enchaînent avec
`SINON SI` :

```owl
SI score >= 16 ALORS
    Trace("Excellent")
SINON SI score >= 10 ALORS
    Trace("Reçu")
SINON
    Trace("Recalé")
FIN
```

> **Piège — `SINON SI` est un chaînage implicite, pas une imbrication.**
> Toute la cascade `SI ... SINON SI ... SINON SI ... SINON ... FIN` est
> un seul bloc structuré : **un seul `FIN`** ferme la chaîne complète,
> quelle que soit sa longueur. Ne pas confondre avec une écriture où
> chaque `SINON` ouvrirait un nouveau `SI` imbriqué qui exigerait son
> propre `FIN`. La forme suivante est **erronée** :
>
> ```owl
> SI score >= 16 ALORS
>     Trace("Excellent")
> SINON
>     SI score >= 10 ALORS         // SI imbriqué dans le SINON
>         Trace("Reçu")
>     FIN                          // FIN du SI interne
> FIN                              // FIN du SI externe
> ```
>
> Elle compile mais ce n'est plus une cascade : c'est une imbrication.
> Le canon `SINON SI ... ALORS ...` se lit comme un seul aiguillage
> linéaire, plus court à écrire et plus lisible à diffuser.

**Forme courte sur une ligne** : quand chaque branche est une seule
instruction, on peut omettre `FIN`. Pratique pour les gardes ou les
choix binaires :

```owl
SI sNom = "" ALORS RENVOYER
SI i > limite ALORS SORTIR
SI nNote >= 10 ALORS sMention = "admis" SINON sMention = "ajourné"
```

**Opérateur `DANS`** : `valeur DANS (e1, e2, ...)` teste l'appartenance
à une liste énumérée. Plus lisible qu'une cascade de `=`/`OU`. La
négation s'écrit `PAS DANS` :

```owl
SI sStatut DANS ("brouillon", "validé", "archivé") ALORS
    Trace("statut connu")
FIN

SI sCommande PAS DANS ("SELECT", "INSERT", "UPDATE") ALORS
    Trace("commande interdite")
FIN
```

La liste ne peut pas être vide (erreur compile). Les types mixtes sont
tolérés ; la comparaison suit les règles d'égalité de `05_operateurs.md`.

---

## 2. `POUR` (boucle indicée)

```owl
POUR i = 1 A 5
    Trace(i)
FIN
```

Les bornes sont **inclusives** : `1 A 5` itère 1, 2, 3, 4, 5. Le
compteur `i` est automatiquement déclaré, typé `entier`, et **local au
bloc** — il n'existe plus après `FIN`.

> **Note** : le mot-clé entre la borne initiale et la borne finale
> s'écrit indifféremment `A`, `a`, `À`, `à` ou `to` — toutes ces formes
> sont équivalentes et reconnues par le scanner. La forme normative est
> `A`. En contrepartie, ces formes sont **réservées** et ne peuvent pas
> servir de nom de variable.

**Pas de boucle** : ajouter `PAS` après les bornes. La valeur peut être
négative pour descendre.

```owl
POUR i = 10 A 0 PAS -2
    Trace(i)              // 10, 8, 6, 4, 2, 0
FIN
```

Quand `start > end` sans `PAS` négatif, la boucle est sautée sans
erreur.

**Parcours inverse** : `DepuisFin` à la fin équivaut à inverser les
bornes avec `PAS -1`. `DepuisDébut` (par défaut) peut être écrit
explicitement pour la lisibilité.

```owl
POUR i = 1 A 5 DepuisFin
    Trace(i)                  // 5, 4, 3, 2, 1
FIN
```

`DepuisFin` et `PAS` sont mutuellement exclusifs : utiliser l'un ou
l'autre, pas les deux.

---

## 3. `POUR TOUT` (parcours de collection)

Parcours d'un tableau ordinal :

```owl
tFruits est un tableau de chaînes = ["pomme", "poire", "banane"]
POUR TOUT sFruit DE tFruits
    Trace(sFruit)
FIN
```

`POUR TOUT`, `POUR TOUS` et `POUR CHAQUE` sont des **synonymes
stricts**. Le choix relève du style et de la formulation naturelle
(« pour chaque utilisateur », « pour tous les éléments »).

Avec une **deuxième variable**, on récupère en plus l'index sur un
tableau ordinal, ou la clé sur un tableau associatif :

```owl
POUR TOUT sFruit, nIndex DE tFruits
    Trace(nIndex + ". " + sFruit)         // 1. pomme  /  2. poire  /  3. banane
FIN

tabUtilisateurs est un tableau associatif de chaînes
tabUtilisateurs["alice"]  = "admin"
tabUtilisateurs["bob"]    = "lecteur"
tabUtilisateurs["carole"] = "admin"

POUR TOUT sRôle, sLogin DE tabUtilisateurs
    SI sRôle = "admin" ALORS Trace(sLogin)
FIN
// Affiche : alice  /  carole
```

**Parcours inverse** : `DepuisFin` après l'itérable. Sur tableau
ordinal comme associatif.

```owl
POUR TOUT sFruit DE tFruits DepuisFin
    Trace(sFruit)             // banane, poire, pomme
FIN
```

**Filtre inline `AVEC`** : équivaut à un `SI NON cond ALORS CONTINUER`
injecté en tête de corps. Plus lisible quand on filtre puis traite.

```owl
tabTemp est un tableau de entiers = [12, 25, 18, 30, 22, 8]
POUR TOUT t DE tabTemp AVEC t > 20
    Trace("chaude:", t)       // 25, 30, 22
FIN
```

**Suppression pendant l'itération** : `Supprime(t, ÉlémentCourant)` est
sûr. La boucle visite chaque élément exactement une fois, même si le
courant est retiré pendant le corps.

```owl
// Garde seulement les administrateurs.
POUR TOUT sRôle, sLogin DE tabUtilisateurs
    SI sRôle <> "admin" ALORS Supprime(tabUtilisateurs, ÉlémentCourant)
FIN
```

### Parcours d'une chaîne

Deux variantes spécifiques évitent de passer par un tableau
intermédiaire.

**Découpage par séparateur** — `POUR TOUTE chaîne` :

```owl
POUR TOUTE chaîne sCouleur DE "rouge,vert,bleu" SEPAREE par ","
    Trace(sCouleur)           // rouge, vert, bleu
FIN
```

Forme étendue à 3 variables : sous-chaîne, position de début dans la
source (1-based), et numéro d'itération (1-based) :

```owl
POUR TOUTE chaîne sc, nPos, nIdx DE "A.B.C" SEPAREE par "."
    Trace(nIdx + ": " + sc + "@" + nPos)   // 1: A@1  /  2: B@3  /  3: C@5
FIN
```

**Occurrences d'un motif** — `POUR TOUTE Position` :

```owl
POUR TOUTE Position p DE ";" DANS "a;b;c;d"
    Trace(p)                  // 2, 4, 6
FIN
```

### Parcours d'un document JSON

`POUR TOUT` distingue à l'exécution un objet JSON d'un tableau JSON, et
adapte le sens des variables d'itération en conséquence.

**Objet JSON** — la forme à deux variables expose les vraies paires
clé/valeur des membres, dans l'ordre du document. La première variable
reçoit la valeur du membre, la seconde son nom, exactement comme sur un
tableau associatif.

```owl
oConfig est un JSON = ChaîneVersJSON(`{"nom":"owl","langue":"fr"}`)
POUR TOUT sValeur, sClé DE oConfig
    Trace(sClé + " = " + sValeur)   // nom = owl  /  langue = fr
FIN
```

La forme mono-variable itère les valeurs des membres (ni les clés, ni un
index).

```owl
oScores est un JSON = ChaîneVersJSON(`{"a":10,"b":20,"c":30}`)
nSomme est un entier = 0
POUR TOUT nValeur DE oScores
    nSomme = nSomme + nValeur       // 60
FIN
```

**Tableau JSON** — le parcours porte sur les éléments. La forme
mono-variable donne chaque élément ; la deuxième variable reçoit l'index
positionnel (1-based), comme sur un tableau ordinal.

```owl
tMesures est un JSON = ChaîneVersJSON(`[10, 20, 30]`)
POUR TOUT nElem DE tMesures
    Trace(nElem)                    // 10, 20, 30
FIN

POUR TOUT nElem, nIndex DE tMesures
    Trace(nIndex + ": " + nElem)    // 1: 10  /  2: 20  /  3: 30
FIN
```

---

## 4. `TANTQUE`

```owl
n est un entier = 5
TANTQUE n > 0
    Trace(n)
    n--
FIN
```

La condition est vérifiée **avant** chaque itération. Si elle est
fausse au départ, le corps n'est jamais exécuté.

---

## 5. `BOUCLE`

Sans argument, boucle inconditionnelle — la sortie se fait depuis
l'intérieur du corps avec `SORTIR`. Utile pour le parsing, la lecture
stream, ou les machines à états.

```owl
n est un entier = 0
BOUCLE
    n = n + 1
    Trace(n)
    SI n >= 3 ALORS SORTIR
FIN
```

**Compteur explicite** : `BOUCLE (N)` exécute le corps exactement `N`
fois. `N` est une expression évaluée **une seule fois** au démarrage.
`N ≤ 0` produit zéro itération, sans erreur.

```owl
BOUCLE (3)
    Trace("ping")             // ping  /  ping  /  ping
FIN
```

`SORTIR` et `CONTINUER` fonctionnent comme dans toute boucle. Sur
`CONTINUER`, le tour est tout de même décompté.

---

## 6. `SELON`

Aiguillage par filtrage. `SELON` évalue son expression **une seule
fois**, puis cherche le premier `CAS` qui matche.

```owl
SELON statut
    CAS "actif"             : Trace("OK")
    CAS "suspendu", "fermé" : Trace("Inactif")
    AUTRE CAS               : Trace("Inconnu")
FIN
```

Un `CAS` accepte plusieurs patterns séparés par virgule. Les patterns
peuvent prendre cinq formes :

**Valeur simple** — égalité directe :
`CAS 1`, `CAS "actif", "valide"`.

**Plage** — bornes inclusives :
`CAS 1 A 10` (compris entre 1 et 10), `CAS A 8` (≤ 8), `CAS 11 A`
(≥ 11). Une plage `min > max` ne matche jamais, sans erreur.

**Comparateur préfixe** — sur la valeur testée :
`CAS > 100`, `CAS <= 0`, `CAS <> 42`. Sur les chaînes, les opérateurs
souples sont disponibles : `CAS [= "Bonjour"` (commence par),
`CAS [=] "erreur"` (contient), `CAS ~= "hello"` (égalité sans casse),
etc. Liste complète dans `05_operateurs.md`.

**Sandwich** — borne min + placeholder `*` + borne max :
`CAS 10 < * < 20` (strict des deux côtés),
`CAS 10 <= * <= 20` (inclus). Les deux opérateurs peuvent être mixés.

**Idiome `SELON Vrai`** — pour des conditions complexes :

```owl
SELON Vrai
    CAS score >= 16              : Trace("Excellent")
    CAS score >= 10 ET réussi    : Trace("Reçu")
    AUTRE CAS                    : Trace("Recalé")
FIN
```

**Pas de fall-through** : le premier `CAS` qui matche exécute son
bloc, puis on sort de `SELON`. `AUTRE CAS` est optionnel et doit
venir en dernier.

Les deux-points après le pattern sont **optionnels** : `CAS 1 : ...`
et `CAS 1 ...` sont équivalents.

Les valeurs des `CAS` sont comparées avec les règles de coercion
habituelles (cf. `05_operateurs.md` §2 et
`01_types_et_coercions.md` §2).

---

## 7. `SORTIR` et `CONTINUER`

`SORTIR` quitte la boucle courante. `CONTINUER` saute à l'itération
suivante (incrément du compteur pour `POUR`, ré-évaluation de la
condition pour `TANTQUE`, retour en tête pour `BOUCLE`).

```owl
// Ignorer les pairs
POUR i = 1 A 5
    SI i modulo 2 = 0 ALORS CONTINUER
    Trace(i)                          // 1, 3, 5
FIN
```

Les deux sont des **instructions de boucle uniquement** : utilisés hors
d'une boucle, ils produisent une erreur de compilation. `SORTIR` ne
permet **pas** de sortir d'une procédure — pour ça, utiliser `RETOUR`
ou `RENVOYER` (cf §8).

### Sortie simple — boucle directement englobante

Par défaut, `SORTIR` quitte la boucle la plus proche :

```owl
POUR i = 1 A 2
    POUR j = 1 A 5
        SI j > 2 ALORS SORTIR        // sort du POUR j seulement
        Trace(i + " " + j)
    FIN
FIN
// Sortie : 1 1, 1 2, 2 1, 2 2
```

### Sortie multi-niveau — `SORTIR(N)`

Pour sortir de N boucles imbriquées d'un coup, passer le nombre de
niveaux à remonter entre parenthèses (`N` doit être un entier littéral
entre 1 et la profondeur d'imbrication courante) :

```owl
POUR x = 1 A 15
    POUR y = 1 A 25
        POUR z = 1 A 50
            SI conditionCritique ALORS
                SORTIR(2)            // sort des boucles z ET y
                                     // → continue avec next x
            FIN
        FIN
    FIN
FIN
```

`SORTIR(1)` est équivalent à `SORTIR` simple. `SORTIR(N)` avec N >
profondeur des boucles englobantes produit une erreur de compilation.

### `SORTIR SELON` — quitter un `SELON` sans quitter la boucle

```owl
SELON quantité
    CAS 1
        SI utilisateur = "Admin" ALORS SORTIR SELON
        Trace("Promo : 2 produits pour le prix d'un")
    AUTRE CAS : Trace("Standard")
FIN
```

`SORTIR SELON` placé hors d'un `SELON` produit une erreur de
compilation. Un `SORTIR` simple à l'intérieur d'un `SELON` placé dans
une boucle quitte **la boucle**, pas le `SELON`.

---

## 8. `RETOUR` et `RENVOYER` — sortie de procédure

Une procédure se termine soit en arrivant à la fin de son corps, soit
explicitement via `RETOUR` ou `RENVOYER`. Les deux mots-clés sont
équivalents fonctionnellement (synonymes en sortie immédiate avec
valeur) mais **ne peuvent pas être mélangés** dans la même procédure.

### Forme avec valeur

```owl
PROCEDURE Carré(n est un entier) : entier
    RENVOYER n * n
FIN
```

Ou équivalent :

```owl
PROCEDURE Carré(n est un entier) : entier
    RETOUR n * n
FIN
```

### Forme sans valeur (procédure-action)

```owl
PROCEDURE Saluer(nom est une chaîne)
    SI nom = "" ALORS RENVOYER
    Trace("Bonjour " + nom)
FIN
```

### Plusieurs valeurs de retour

```owl
PROCEDURE MinMax(t est un tableau de entiers) : (entier, entier)
    RENVOYER (Min(t), Max(t))
FIN

(petit, grand) = MinMax(monTab)
```

### Règle d'exclusion — pas de mélange `RETOUR` / `RENVOYER`

Une procédure ne peut utiliser qu'un seul des deux mots-clés. Le
compilateur enforce cette règle :

```owl
PROCEDURE Mauvais(x est un entier) : entier
    SI x > 0 ALORS
        RENVOYER x * 2          // ← premier kind verrouillé : RENVOYER
    FIN
    RETOUR 0                    // ← ERREUR compile : mélange interdit
FIN
```

Diagnostic compile-time : « *Mélange interdit 'RENVOYER' / 'RETOUR'
dans le même traitement. Premier RENVOYER vu ligne X. Choisir un seul
keyword pour la procédure.* »

**Astuce** : `RENVOYER` est la forme préférée pour les fonctions qui
retournent une valeur, `RETOUR` pour les procédures-actions qui sortent
sans valeur. La discipline aide à lire le code.

---

## 9. Aliases anglais

Toutes les constructions ont un équivalent anglais. Le langage est
strictement bilingue : un fichier peut mélanger les deux formes sans
conséquence.

| Français | Anglais |
|---|---|
| `SI ... ALORS ... SINON ... FIN` | `IF ... THEN ... ELSE ... END` |
| `POUR i = 1 A 5 PAS 1 ... FIN` | `FOR i = 1 to 5 STEP 1 ... END` |
| `POUR TOUT`, `POUR TOUS`, `POUR CHAQUE` | `FOR EACH`, `FOR ALL`, `FOR EVERY` |
| `... DE ...` | `... OF ...` |
| `TANTQUE ... FIN` | `WHILE ... END` |
| `BOUCLE ... FIN`, `BOUCLE (N) ... FIN` | `LOOP ... END`, `LOOP (N) ... END` |
| `SELON ... CAS ... AUTRE CAS ... FIN` | `SWITCH ... CASE ... DEFAULT ... END` |
| `SORTIR`, `CONTINUER`, `SORTIR SELON` | `BREAK`, `CONTINUE`, `EXIT SWITCH` |
| `DANS`, `AVEC` | `IN`, `WITH` |

Pour le style à adopter dans un projet, voir `10_conventions_de_code.md`.

---

## 10. Pièges courants

- **Chaîne ou tableau en condition** : `SI s ALORS` est rejeté à
  l'exécution. Tester explicitement : `SI Taille(s) > 0 ALORS`,
  `SI t..Occurrence > 0 ALORS` (double point pour les propriétés).
- **Bornes inversées sans `PAS` négatif** : `POUR i = 5 A 1` boucle
  zéro fois. Pas d'erreur, pas de warning. Vérifier l'intention.
- **`DepuisFin` et `PAS` combinés** : refus à la compilation. Choisir
  l'un ou l'autre — `DepuisFin` impose déjà un pas implicite de -1.
- **Compteur de `POUR` hors du bloc** : `i` n'existe plus après `FIN`.
  Si on veut conserver la valeur finale, déclarer une variable externe
  et l'assigner dans le corps.
- **`SORTIR` depuis un `SELON`** : sort de la boucle englobante, pas
  du `SELON`. Pour quitter uniquement le `SELON`, utiliser `SORTIR
  SELON` (le « fall-through » n'existe pas, mais on peut court-circuiter
  un `CAS` en plusieurs branches).
- **Borne réelle dans `POUR`** : les bornes sont attendues entières.
  Un réel est tronqué à l'entier. Pour itérer sur des réels (rare),
  utiliser `TANTQUE`.
- **Liste vide pour `DANS`** : `v DANS ()` est refusé à la compilation.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Types et conversions (conditions booléennes) | `01_types_et_coercions.md` |
| Opérateurs et précédence | `05_operateurs.md` |
| Procédures, paramètres, retour | `04_procedures.md` (à venir) |
| Exceptions et propagation | `08_exceptions.md` |
| Conventions de code | `10_conventions_de_code.md` |
