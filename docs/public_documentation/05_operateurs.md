# Opérateurs (guide)

Cette fiche liste les opérateurs OWL classés par famille, et termine
par un tableau de précédence. Tous les exemples sont vérifiés sur le
binaire.

---

## 1. Arithmétique

| Opérateur | Effet | Exemple |
|---|---|---|
| `+` | Addition (ou concaténation si l'un des opérandes est une chaîne, cf. section 4) | `7 + 3 → 10` |
| `-` | Soustraction, ou unaire | `7 - 3 → 4`, `-x` |
| `*` | Multiplication | `7 * 3 → 21` |
| `/` | Division (toujours réelle, jamais entière) | `7 / 3 → 2.333…` |
| `%` ou `modulo` | Reste de la division entière | `7 % 3 → 1`, `7 modulo 3 → 1` |
| `^` | Puissance, associativité gauche | `2 ^ 3 ^ 2 → 64` (= `(2^3)^2`) |

**Promotions automatiques** : un débordement d'`entier` promeut vers
`numérique` ; un mixte avec `réel` produit un `réel`. Voir
`01_types_et_coercions.md` pour le détail.

**Convention unaire moins + puissance** : `-2 ^ 2` vaut `4`, pas `-4`.
Le moins unaire se lie plus fort que `^`, donc l'expression se lit
`(-2)^2`. C'est la convention des tableurs (Excel donne le même
résultat). Elle diffère de la convention mathématique manuscrite et de
plusieurs langages de programmation où l'unaire est plus faible que la
puissance. En cas de doute, parenthéser.

---

## 2. Comparaison

| Opérateur | Effet |
|---|---|
| `=` | Égalité (en contexte de test, pas d'assignation) |
| `<>` | Inégalité |
| `<` | Strictement inférieur |
| `<=` | Inférieur ou égal |
| `>` | Strictement supérieur |
| `>=` | Supérieur ou égal |

`!=` n'est **pas** reconnu — utiliser `<>`. Le résultat d'une comparaison
est un booléen (`Vrai`/`Faux`).

```owl
SI age >= 18 ALORS …
SI statut <> "actif" ALORS …
```

**Appartenance à une liste** — `DANS` et `PAS DANS` :

```owl
SI sStatut DANS ("brouillon", "validé", "archivé") ALORS …
SI sCommande PAS DANS ("SELECT", "INSERT", "UPDATE") ALORS …
```

Sucre pour une cascade d'égalités. La liste ne peut pas être vide
(erreur compile). Les deux variantes `DANS` (full-eval) et `_DANS_`
(court-circuit) suivent la même doctrine que `ET` / `_ET_` — voir §3.

---

## 3. Logiques

| Opérateur | Effet | Évaluation |
|---|---|---|
| `ET` / `AND` | Conjonction full-eval | Les deux opérandes évalués |
| `OU` / `OR` | Disjonction full-eval | Les deux opérandes évalués |
| `PAS` / `NOT` | Négation unaire | — |
| `_ET_` | Conjonction court-circuit | Si gauche faux, droite ignorée |
| `_OU_` | Disjonction court-circuit | Si gauche vrai, droite ignorée |

Les versions court-circuit (`_ET_`, `_OU_`) sont indispensables quand
le second opérande peut lever une erreur ou coûter cher :

```owl
SI EstNul(oCompte) _OU_ oCompte.solde > 0 ALORS …
```

`ET` est plus prioritaire que `OU` : `Vrai OU Faux ET Faux` s'évalue
`Vrai OU (Faux ET Faux) → Vrai`.

### Doctrine `X` / `_X_`

OWL propose cinq paires d'opérateurs où la forme nominale fait
**full-eval / ré-évaluation** et la forme à underscores fait
**court-circuit / évaluation unique** :

| Forme nominale | Forme `_X_` | Contexte |
|---|---|---|
| `ET`, `OU` | `_ET_`, `_OU_` | Expressions booléennes |
| `DANS` | `_DANS_` | Test d'appartenance (§2) |
| `A` | `_A_` | Borne dans `POUR i = a A b` (cf. `03_controle_de_flux.md` §2) |
| `PAS` (step) | `_PAS_` | Pas dans `POUR ... PAS s` (cf. `03_controle_de_flux.md` §2) |

La distinction est **sémantique**, pas cosmétique : les deux formes
produisent des bytecodes différents et peuvent donner des résultats
différents dès qu'un opérande porte un effet de bord, ou qu'une borne
varie entre deux tours. La forme nominale est la forme écrite ;
basculer en `_X_` est un choix explicite quand on veut éviter la
ré-évaluation.

Sur les types admis en condition (booléen, nombre, `Null`), cf.
`01_types_et_coercions.md` §2. Une chaîne ou un tableau en condition
produit une erreur d'exécution explicite.

---

## 4. Chaînes

L'opérateur `+` concatène dès qu'au moins un opérande est une chaîne.
Le résultat est toujours une chaîne, même si l'autre opérande est un
nombre :

```owl
Trace("score : " + 99)        // "score : 99"
Trace(5 + "x")                // "5x"
```

OWL fournit trois opérateurs **fixes** dédiés à la comparaison
positionnelle, déclinés en trois variantes de tolérance :

| Position | Strict | Casse-insensible | Casse + accent insensibles |
|---|---|---|---|
| Commence par | `[=` | `[~` | `[~~` |
| Termine par | `=]` | `~]` | `~~]` |
| Contient | `[=]` | `[~]` | `[~~]` |

```owl
"alpha" [= "alp"      // Vrai
"ALPHA" [~ "alp"      // Vrai  (casse ignorée)
"héllo" [~ "hel"      // Faux  (accents encore distincts)
"Héllo" [~~ "hello"   // Vrai  (casse + accents ignorés)
"alpha" [=] "ph"      // Vrai  (contient)
"alpha" =] "ha"       // Vrai  (termine par)
```

Pour des comparaisons plus complexes (motifs, expressions), utiliser
les natives `CommencePar`, `Position`, `Contient`, ou les expressions
régulières via le module regex.

---

## 5. Affectation et compositions

`=` sert à la fois d'affectation et de comparaison ; le contexte
syntaxique tranche. En instruction, c'est une affectation ; dans une
expression de condition, c'est une comparaison.

Opérateurs composés :

| Opérateur | Équivalent |
|---|---|
| `+=` | `x = x + y` |
| `-=` | `x = x - y` |
| `*=` | `x = x * y` |
| `/=` | `x = x / y` |
| `%=` | `x = x modulo y` |

Incréments/décréments **postfixe uniquement** :

```owl
n est un entier = 5
n++           // n vaut 6
n--           // n vaut 5
```

`++n` (préfixe) n'est pas reconnu. `n++` s'utilise comme instruction,
pas comme expression — `m = n++` n'est pas accepté.

---

## 6. Opérations bit-à-bit

OWL fournit les opérateurs binaires en plus des fonctions nommées.
Les deux formes sont équivalentes et interchangeables.

| Opérateur | Fonction | Effet |
|---|---|---|
| `a & b` | `ETBinaire(a, b)` | ET bit-à-bit |
| `a \| b` | `OUBinaire(a, b)` | OU bit-à-bit |
| `a \|\| b` | `OUExclusifBinaire(a, b)` | XOR bit-à-bit |
| `~a` | `NONBinaire(a)` | Complément à 1 (unaire) |
| `a bitDécaleGauche n` | `bitDécaleGauche(a, n)` | Décalage à gauche |
| `a bitDécaleDroite n` | `bitDécaleDroite(a, n)` | Décalage à droite |

```owl
masque   est un entier = drapeaux & 0x0F        // = ETBinaire(drapeaux, 0x0F)
position est un entier = 1 bitDécaleGauche 4    // = 16
inverse  est un entier = ~drapeaux              // complément à 1
```

### Accès à un bit individuel

Pour lire ou modifier un bit précis d'un entier, OWL expose quatre
méthodes préfixes. La position est en base 1, bit de poids faible
d'abord (`pos=1` → poids 2⁰, `pos=4` → poids 2³). Une position hors
de `[1..64]` retourne la valeur inchangée pour les setters et `0` pour
`BitLit`. Style **immutable** : aucune mutation de la variable, la
méthode renvoie une nouvelle valeur.

| Méthode | Alias EN | Effet |
|---|---|---|
| `n.BitLit(pos)` | `BitRead` | Renvoie `0` ou `1` |
| `n.BitActive(pos)` | `BitEnable` | Renvoie `n` avec le bit allumé |
| `n.BitDesactive(pos)` | `BitDisable` | Renvoie `n` avec le bit éteint |
| `n.BitInverse(pos)` | `BitFlip` | Renvoie `n` avec le bit basculé |

```owl
n est un entier = 10                  // 1010
Trace(n.BitLit(2))                    // 1
n = n.BitActive(1)                    // 1011 = 11
n = n.BitDesactive(4)                 // 0011 = 3
```

---

## 7. Précédence

De la plus basse à la plus haute. Un opérateur de haute précédence se
lie plus fort, donc s'évalue d'abord.

| Niveau | Opérateurs | Associativité |
|---|---|---|
| Affectation | `=`, `+=`, `-=`, `*=`, `/=`, `%=` | Droite |
| OU logique | `OU`, `_OU_` | Gauche |
| ET logique | `ET`, `_ET_` | Gauche |
| OU binaire | `\|` | Gauche |
| XOR binaire | `\|\|` | Gauche |
| ET binaire | `&` | Gauche |
| Égalité / appartenance | `=`, `<>`, `DANS`, `_DANS_` | Gauche |
| Comparaison | `<`, `<=`, `>`, `>=`, `[=`, `=]`, `[=]` (et variantes flex) | Gauche |
| Décalage binaire | `bitDécaleGauche`, `bitDécaleDroite` | Gauche |
| Addition | `+`, `-` | Gauche |
| Multiplication | `*`, `/`, `%`, `modulo` | Gauche |
| Puissance | `^` | Gauche |
| Unaire | `-`, `PAS`, `~` | — |
| Appel / accès | `.`, `()`, `[]`, `[[ ]]` | Gauche |

En cas de doute, parenthéser. L'effet est zéro côté performance, le
gain de clarté est immédiat.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Types et conversions | `01_types_et_coercions.md` |
| Null et types nullables | `02_null_et_nullable.md` |
| Contrôle de flux (`A` / `_A_`, `PAS` step) | `03_controle_de_flux.md` |
| Accès par nom calculé (`{ nom }`, `{ nom }..prop`) | `31_indirection.md` |
| Conventions de code | `10_conventions_de_code.md` |
