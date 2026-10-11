# Types et conversions

OWL a un typage **explicite à la déclaration** et **souple au casting**.
Toute variable est déclarée avec son type ; mais la
valeur réelle qu'elle contient est convertie au moment où une opération
l'exige, pas à l'affectation. Cette combinaison donne :

- de la sécurité à la lecture (on sait ce qu'une variable est censée
  contenir),
- de la souplesse à l'écriture (pas besoin de `ChaineVersEntier(...)`
  partout).

---

## 1. Types fondamentaux

| Type | Description |
|---|---|
| `chaîne` | Texte UTF-8, taille mesurée en octets |
| `entier` | Entier 64 bits signé, promu en `numérique` en cas de débordement |
| `réel` | Flottant double précision (8 octets) |
| `monétaire` | Décimal exact, 6 chiffres après la virgule |
| `numérique` | Décimal exact à précision illimitée (croît avec la mémoire) |
| `booléen` | `Vrai` ou `Faux` |
| `date`, `heure`, `dateheure` | Calendrier civil, heure du jour, combinaison |
| `durée` | Intervalle, en millisecondes |
| `buffer` | Séquence d'octets bruts |
| `tableau de <T>` | Collection ordonnée, index base 1 |
| `Vecteur` | Séquence contiguë de réels pour le calcul numérique (distinct du `tableau`) |
| `JSON` | Document structuré avec accès point |

OWL n'a **pas** de types entiers dimensionnés : les formes du genre
`entier sur 2 octets`, `entier sans signe`, `réel sur 4 octets` sont
**rejetées à la compilation**, tout comme `octet` employé comme type
déclaré. Le seul type entier est `entier` (64 bits, promu en `numérique`
en cas de débordement).

Un **octet** n'est donc pas un type : c'est une **valeur logique 0–255**
portée par un `entier`. Pour stocker une séquence d'octets bruts, utiliser
un `buffer` ; pour lire ou écrire un octet précis, l'opérateur
`buffer[[i]]` et les natives décrites en
[section 6](#6-travailler-au-niveau-de-loctet).

---

## 2. Conversions implicites

OWL convertit silencieusement entre types compatibles, **au moment où
l'opération s'effectue**, pas à la déclaration.

### Chaîne et nombre

L'opérateur `+` est **concaténation** dès qu'au moins un opérande est
une chaîne, quel que soit l'ordre :

```owl
Trace("x" + 5)            // "x5"
Trace(5 + "x")            // "5x"
Trace("12" + 30)          // "1230"   — concat, pas addition
```

L'**addition numérique** demande deux opérandes numériques. Si la
chaîne contient un nombre valide, elle est parsée à l'usage :

```owl
nResultat est un entier = "10" + 5    // 15
                                       // OWL parse "10" → 10 en contexte arithmétique
                                       // (le LHS attend un entier)
```

### Booléens

Dans un contexte booléen (`SI`, `TANTQUE`), trois types sont
acceptés : `booléen`, nombre (entier, réel, monétaire, numérique) et
`Null`. Toute autre valeur — chaîne, tableau, structure, instance —
produit une erreur d'exécution explicite.

Règles de coercion :

- `0`, `0.0`, `Faux`, `Null` valent faux.
- Tout autre nombre vaut vrai (y compris négatif).

```owl
SI 1 ALORS …              // exécute
SI 0 ALORS …              // ignore
SI 3.5 ALORS …            // exécute
SI Null ALORS …           // ignore

SI "abc" ALORS …          // erreur : chaîne en condition
SI t ALORS …              // erreur : tableau en condition
```

Pour tester une chaîne non vide ou un tableau non vide, expliciter :

```owl
SI Taille(s) > 0 ALORS …
SI t..Occurrence > 0 ALORS …
```

### Nombres entre eux

`entier`, `réel`, `monétaire`, `numérique` s'additionnent et se
multiplient sans préoccupation. La précision suit le plus large
opérande. Sur les bornes de boucle, un réel est tronqué à l'entier.

```owl
moPrix est un monétaire = 10
moPrix = moPrix + 0.5     // 10.5
```

---

## 3. Quand la conversion a lieu

Pour les cibles `entier`, `réel` et `booléen`, OWL convertit la valeur
**à l'affectation**. La variable contient désormais une valeur du type
déclaré, vérifiable via `TypeVar`.

```owl
nT est un entier = 3.7
Trace(nT, " type=", TypeVar(nT))      // 3  type=8 (entier) — tronc à l'init

n est un entier = "abc"
Trace(n)                               // 0  — parse échoué, défaut silencieux
```

La conversion vers `entier` tronque (n'arrondit pas) un réel. La
conversion vers `entier` depuis une chaîne non parseable renvoie `0`
sans erreur ; vérifier le contenu en amont si l'on veut détecter
l'échec.

Pour les autres cibles, le comportement actuel est de **conserver le
type natif de la valeur**, la conversion étant assurée à l'usage par les
opérations qui en ont besoin (`Trace`, `Taille`, arithmétique). Ce point
est en cours de stabilisation.

Pour forcer une conversion explicite, passer par les fonctions dédiées
(section 5).

---

## 4. Promotions automatiques

Quelques promotions arrivent automatiquement, sans cast :

- **Débordement d'entier** : un calcul sur `entier` qui dépasse 2⁶³−1
  promeut le résultat en `numérique` (précision arbitraire).
  ```owl
  nGros est un entier = 9223372036854775807
  nGros = nGros + 1
  Trace(TypeVar(nGros))         // 14 (numérique)
  ```
- **Mixte entier + réel** : le résultat est `réel`.
- **Mixte entier + monétaire** : le résultat est `monétaire`.

Ces promotions sont sûres : elles ne perdent jamais de précision, et
elles sont **automatiques**. Elles s'opposent au débordement contrôlé
d'une valeur dans une plage ou un octet
([section 6](#6-travailler-au-niveau-de-loctet)), qui est au contraire
**explicite et choisi** : sans mode de débordement, une valeur hors plage
lève une erreur ; le repli (cyclique, clamp…) ne s'applique que si on le
demande. Élargir un nombre est sûr et silencieux ; le rétrécir dans un
octet est un acte délibéré.

---

## 5. Conversions explicites

Quand on veut une conversion **sans ambiguïté** au moment voulu, OWL
fournit des natives dédiées :

| De → vers | Fonction |
|---|---|
| Chaîne → entier | `ChaineVersEntier(s)`, `Val(s)` |
| Chaîne → réel | `ChaineVersReel(s)`, `Val(s)` |
| Chaîne → numérique | `ChaineVersNumerique(s)` |
| Chaîne → date / heure | `ChaineVersDate`, `ChaineVersHeure` |
| Chaîne → JSON | `ChaineVersJSON(s)` |
| Entier / réel / numérique → chaîne | `NumériqueVersChaîne(n)` |
| Date / heure / dateheure → chaîne | `DateVersChaine`, `HeureVersChaine`, `DateHeureVersChaine` |
| Numérique / réel → entier | `PartieEntière(x)` (partie entière, arrondi vers `-∞`) |
| Buffer ↔ chaîne | `ChaineVersBuffer`, `BufferVersChaine` |

Pour la liste à jour : `owl explain` sur chaque famille.

Le passage `chaîne` ↔ octets aux frontières d'entrée/sortie (fichiers,
buffers, URLs) et les encodages disponibles sont détaillés dans
[`26_encodage_et_io.md`](26_encodage_et_io.md).

---

## 6. Travailler au niveau de l'octet

Pour les formats binaires, les protocoles réseau ou le chiffrement, OWL
expose les octets sans détour : un `buffer` est une séquence d'octets
bruts, et chaque octet est une valeur entière 0–255. Cette section couvre
l'accès octet par octet ; le buffer comme conteneur binaire (marshalling
de scalaires, pré-dimensionnement, I/O fichier) est traité dans
[`27_buffers.md`](27_buffers.md).

### Accès direct : `buffer[[i]]`

L'opérateur `buffer[[i]]` lit ou écrit **un octet** à la position `i`
(index base 1). La lecture renvoie un **entier** 0–255, sans passer par
l'encodage du texte ; l'écriture pose exactement un octet.

```owl
b est un buffer = "Hé"          // "é" occupe 2 octets en UTF-8
Trace(b[[1]])                    // 72  ('H')
Trace(b[[2]])                    // 195 (1er octet de 'é')
Trace(b[[3]])                    // 169 (2e octet de 'é')

b[[1]] = 200                     // pose l'octet 200 en position 1
Trace(b[[1]])                    // 200
```

L'écriture est **stricte** : une valeur hors `[0, 255]` lève une
exception (aucune troncature silencieuse). Un index hors du buffer pose
une erreur récupérable (`ErreurDétectée`).

```owl
b est un buffer = "ABC"
QUAND EXCEPTION DANS
    b[[1]] = 300                 // hors [0,255] → exception
FAIRE
    Trace("valeur d'octet invalide")
FIN
```

### Natives octet et plage

Quatre natives complètent l'opérateur, chacune avec une forme fluent et
un alias anglais :

| Native | Rôle |
|---|---|
| `EntierDansPlage(v, min, max, mode)` | Contraint `v` à `[min, max]` inclusif |
| `EntierVersOctet(v, mode)` | Cas particulier : contraint `v` à `[0, 255]` |
| `BufferLitOctet(buf, pos)` | Lit l'octet en `pos` → entier 0–255 |
| `BufferÉcritOctet(buf, pos, v, mode)` | Écrit l'octet `v` en `pos` |

```owl
nClamp est un entier = EntierDansPlage(300, 0, 255, débordementBorne)
Trace(nClamp)                    // 255 (ramené à la borne haute)

nOctet est un entier = (300).VersOctet(débordementCyclique)
Trace(nOctet)                    // 44
```

### Modes de débordement

Une valeur hors de la plage demandée doit être traitée explicitement. Le
mode passé en dernier argument décide du comportement ; **sans mode, le
défaut lève une erreur**.

| Mode | Comportement hors plage |
|---|---|
| `débordementErreur` | Défaut — lève une exception |
| `débordementCyclique` | Repli modulo : la valeur « tourne » dans la plage |
| `débordementBorne` | Ramène à la borne la plus proche (clamp) |
| `débordementOctetFaible` | Garde les 8 bits faibles (octet uniquement) |

`débordementOctetFaible` est réservé au travail sur octet : il est
**refusé à la compilation** dans `EntierDansPlage` (plage arbitraire).

```owl
// Compteur 8 bits qui repart à zéro après 255
nSuivant est un entier = EntierVersOctet(255 + 1, débordementCyclique)
Trace(nSuivant)                  // 0

// Compteur 16 bits, même principe sur une plage plus large
nSeize est un entier = EntierDansPlage(70000, 0, 65535, débordementCyclique)
Trace(nSeize)                    // 4464

// 8 bits faibles d'une valeur négative
nFaible est un entier = EntierVersOctet(-1, débordementOctetFaible)
Trace(nFaible)                   // 255
```

### Opérations binaires

Sur des entiers traités comme des octets, les opérateurs bit à bit
s'appliquent directement :

| Opérateur | Effet |
|---|---|
| `&` | ET binaire |
| `\|` | OU binaire |
| `\|\|` | OU exclusif (XOR) |
| `bitDécaleGauche(x, n)` | Décale `x` de `n` bits vers la gauche |
| `bitDécaleDroite(x, n)` | Décale `x` de `n` bits vers la droite |

```owl
Trace(0xF0 & 0x0F)               // 0   (aucun bit commun)
Trace(0xF0 | 0x0F)               // 255 (0xFF)
Trace(0xFF || 0x0F)              // 240 (0xF0 — XOR)
Trace(bitDécaleGauche(1, 4))     // 16
Trace(bitDécaleDroite(256, 4))   // 16
```

### Exemples

Lire l'en-tête d'un mini format binaire `[version][longueur][données…]` :

```owl
paquet est un buffer = "XXXXX"
paquet[[1]] = 2                  // version
paquet[[2]] = 3                  // longueur des données
paquet[[3]] = 170                // 0xAA
paquet[[4]] = 187                // 0xBB
paquet[[5]] = 204                // 0xCC

nVersion est un entier = paquet[[1]]
nLongueur est un entier = paquet[[2]]
Trace("version ", nVersion, ", ", nLongueur, " octets de données")
```

Chiffrement symétrique simple : le XOR appliqué deux fois avec la même
clé restitue la valeur d'origine.

```owl
nClair est un entier = 0x41      // 'A'
nCle est un entier = 0x5A
nChiffre est un entier = nClair || nCle      // 27
nRendu est un entier = nChiffre || nCle      // 65 — retour à 'A'
Trace(nClair, " → ", nChiffre, " → ", nRendu)
```

---

## 7. Pièges courants

- **Concat vs addition** : `"12" + 30 = "1230"`, pas `42`. Pour
  additionner, utiliser `Val("12") + 30` ou assigner à une variable
  typée entier (qui parse alors la chaîne).
- **Parse silencieux vers entier** : `n est un entier = "abc"` met `0`
  sans erreur. Contrôler la chaîne en amont si l'on veut détecter
  l'échec.
- **Réel littéral sans zéro initial** : `.5` n'est **pas** un littéral
  réel valide — il produit une valeur inattendue. Toujours écrire
  `0.5`.
- **Comparer un entier et une chaîne** : `5 = "5"` retourne `Vrai`
  (coercion). Pour distinguer le type, comparer aussi `TypeVar`.
- **Condition non booléenne** : `SI` et `TANTQUE` n'acceptent que
  booléen, nombre ou `Null`. Une chaîne ou un tableau passé en
  condition produit une erreur d'exécution explicite ; expliciter avec
  `Taille(s) > 0` ou `t..Occurrence > 0`.
- **Opérateurs `*`, `-`, `/`, `mod`, `^` avec chaîne** : rejet runtime
  explicite (`Les opérandes doivent être des nombres`). La coercion
  implicite chaîne→nombre n'opère qu'à l'affectation vers une cible
  typée numérique (cf. §3), pas dans une expression arithmétique sans
  cible typée. Pour multiplier ou soustraire deux valeurs dont l'une
  est en chaîne, convertir explicitement avec `Val(s)` ou
  `ChaineVersEntier(s)`.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Null et types nullables | `02_null_et_nullable.md` |
| Conventions de code | `10_conventions_de_code.md` |
| Opérateurs et précédence | `05_operateurs.md` |
| Calcul numérique et vecteurs | `33_vecteurs.md` |
