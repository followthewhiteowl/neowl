# OWL — Pièges courants

Les conventions qui ne se devinent pas. Un programme OWL est prévisible à
la lecture, mais quelques règles cadrent l'écriture dès le premier fichier.
Voici celles qui surprennent le plus souvent.

---

## Littéral décimal nu = réel

Un nombre à virgule écrit tel quel produit un réel (flottant). Pour une
valeur exacte (montant monétaire, calcul numérique sans dérive), typer la
variable en `numérique` **ou** préfixer le littéral par `0n`.

```owl
nReel est un réel = 154.49
nExact est un numérique = 0n154.49
```

## Précision des réels : `numérique` pour l'exact

Le type `réel` est un flottant binaire (IEEE-754). Beaucoup de valeurs
décimales n'ont pas de représentation binaire finie : elles sont stockées
au plus proche, avec une infime erreur. La somme `0.1 + 0.2` ne vaut donc
pas exactement `0.3`, et la comparaison d'égalité stricte échoue :

```owl
nA est un réel = 0.1
nB est un réel = 0.2
Trace(nA + nB)                              // 0.30000000000000004
SI (nA + nB) = 0.3 ALORS Trace("égaux") FIN // ne s'affiche jamais
```

Pour tout calcul qui doit être exact — montants, comptage, comparaisons
d'égalité — utiliser `numérique`. La même opération y est exacte et la
comparaison vraie :

```owl
mA est un numérique = 0n0.1
mB est un numérique = 0n0.2
Trace(mA + mB)                                 // 0.3
SI (mA + mB) = 0n0.3 ALORS Trace("égaux") FIN  // affiche "égaux"
```

Réserver le `réel` à la mesure, au calcul scientifique et à la géométrie,
où une marge d'erreur infime est acceptable et la vitesse prime.

## Le modèle de nombre en bref

Quatre types numériques, chacun avec un rôle précis :

- **`entier`** : entier signé, borné à environ ±9,2×10¹⁸.
- **`numérique`** : décimal exact à précision *illimitée* — elle croît
  avec la mémoire disponible. Le type pivot du calcul exact.
- **`monétaire`** : décimal à précision *fixe* (6 décimales), large
  magnitude. Pensé pour les montants.
- **`réel`** : flottant binaire (voir la section précédente).

Deux mécanismes gouvernent le passage d'un type à l'autre :

**Promotion automatique — l'agrandissement sûr.** Un calcul sur `entier`
qui dépasse la borne ne déborde jamais en silence : le résultat devient
automatiquement un `numérique`. La valeur est préservée, exacte.

```owl
nGros est un entier = 9223372036854775807    // borne haute de l'entier
nProm est un numérique = nGros + 1           // promu en numérique exact : 9223372036854775808
```

**L'exception au rétrécissement — le mur.** Le chemin inverse n'est pas
toujours possible. Affecter un `numérique` dont la *magnitude* ne tient
pas dans une cible à taille fixe (`entier`, `monétaire`) lève une
exception, au lieu d'écrire en silence une valeur tronquée :

```owl
nÉnorme est un numérique = 0n100000000000000000000   // 10^20
nCible  est un entier    = nÉnorme                    // exception : magnitude hors capacité
```

La perte des seules *décimales* (la magnitude, elle, rentre) reste
silencieuse : c'est une conversion souple assumée — un `entier` qui reçoit
`3.75` retient `3`. Pour jeter la partie décimale de façon volontaire et
visible dans le code, passer par une fonction explicite (`NumériqueTronque`,
`Arrondi`) plutôt que de compter sur la conversion implicite.

## `numérique` et écriture en base de données

Un `numérique` n'est jamais écrit silencieusement dans une base : s'il ne
peut pas être converti pour la colonne cible, l'écriture est rejetée — pas
de valeur tronquée posée en douce. En revanche, le respect de la précision
*déclarée* d'une colonne dépend du moteur : certains moteurs stockent de
façon souple et acceptent des valeurs au-delà de la précision annoncée.
C'est la responsabilité du moteur de base et du développeur, pas une
garantie uniforme du langage.

## Préfixer les variables

Un nom qui entre en collision avec un type ou un mot-clé est rejeté à la
compilation : `a` heurte le mot-clé `À`, `liste` heurte le type `Liste`.
Un nom d'une seule lettre n'est pas rejeté en soi — `n` ou `x` passent
sans problème ; seules les collisions avec un mot réservé posent souci.
La convention est de préfixer selon le type — `sNom`, `nAge`, `oCompte`,
`tabNoms` — ce qui lève l'ambiguïté et rend le code lisible.

## Mots réservés du langage

Certains noms sont verrouillés par le langage et ne peuvent pas servir
de nom de variable, de paramètre, ni de champ. La liste ci-dessous est
exhaustive. Toutes les formes sont accent-insensibles et
casse-insensibles : `À`, `a`, `À` valent le même mot-clé, `SI`, `si`,
`Si` aussi. Chaque entrée FR a son équivalent EN également réservé.

**Contrôle de flux** : `SI`, `ALORS`, `SINON`, `FIN`, `POUR`, `TOUT`,
`DE`, `DANS`, `TANTQUE`, `BOUCLE`, `FAIRE`, `SELON`, `CAS`, `AUTRE`,
`SORTIR`, `CONTINUER`, `RENVOYER`, `RETOUR`, `AVEC`, `À` (séparateur
de bornes), `PAS` (négation et incrément de boucle). Équivalents EN :
`IF`, `THEN`, `ELSE`, `END`, `FOR`, `EACH`/`ALL`/`EVERY`, `OF`, `IN`,
`WHILE`, `LOOP`, `DO`, `SWITCH`, `CASE`, `DEFAULT`, `BREAK`,
`CONTINUE`, `RETURN`, `WITH`, `TO`, `STEP`, `NOT`.

**Exceptions** : `QUAND`, `EXCEPTION` (EN : `WHEN`).

**Logique et arithmétique** : `ET`, `OU`, `_ET_`, `_OU_` (formes
court-circuit). EN : `AND`, `OR`, `_AND_`, `_OR_`.

**Scalaires primitifs** : `chaîne`, `entier`, `réel`, `monétaire`,
`numérique`, `caractère`, `booléen`, `buffer`, `octet`, `octets`.

**Composites structurels** : `tableau`, `structure`, `énumération`,
`combinaison`, `liste`, `pile`, `file`, `associatif`.

**Modificateurs de type** : `fixe`, `dynamique`, `par`, `sans`,
`signe`.

**Déclarateurs OOP / procédure** : `classe`, `interface`, `procedure`,
`fonction`.

**Portée et namespace** : `local`, `global`, `globale`. `global` et
`local` sont des **bascules de portée** posées seules sur leur ligne au
top-level d'un conteneur ou d'un script (voir `15_lifecycle_champs.md` §2,
`16_lifecycle_etats.md` et `24_projet.md`) ; `globale` est le
modificateur de méthode statique d'une classe. Les trois restent des
mots-clés réservés : aucun ne peut servir de nom de variable.

**Référence d'instance** : `objet`, `ancêtre`, `MoiMême` (alias EN
`MySelf`). `MoiMême` désigne, dans un handler d'événement ou un
top-level de sous-champ, le champ courant — voir
`15_lifecycle_champs.md` §7bis et `16_lifecycle_etats.md` §10bis.

Un usage interdit produit une erreur explicite à la compilation :

```
Le mot 'chaîne' est un mot-clé réservé du langage et ne peut pas être
utilisé comme nom de variable.
```

À l'inverse, les noms de types valeur (`date`, `heure`, `dateheure`,
`durée`, `json`, `uuid`) et les types issus du registre (`httpRequête`,
`chrono`, `image`, `couleur`, …) restent **libres** comme noms
d'identifiant. La règle complète figure au chapitre conventions.

> **Piège typique — collision silencieuse au runtime.** Quelques
> exemples qui surprennent les nouveaux venus :
>
> - `signe est un entier` est rejeté (`signe` est un modificateur de
>   type, employé dans `entier sans signe sur 4 octets`).
> - `À est un réel` est rejeté (`À` est le séparateur de bornes des
>   boucles `POUR i = 1 À 10`).
> - `pas est un entier` est rejeté (`PAS` est l'incrément de boucle
>   et la négation booléenne).
> - `liste est un tableau de chaînes` est rejeté (`liste` est un
>   composite structurel ; préférer `tabNoms` avec préfixe).
>
> Le réflexe : préfixer les variables par leur type (`sNom`, `nAge`,
> `tabFruits`) supprime tout risque de collision et reste la
> convention OWL recommandée (voir `12_prefixes_de_nommage.md`).

> **Important — les mots réservés bloquent aussi les noms de
> procédure, méthode, fonction et champ de classe**, pas seulement
> les variables. Une `PROCEDURE Etat()` (mot-clé du sous-bloc d'État)
> ou `PROCEDURE Signe()` (modificateur `signe` du type `entier`)
> sont rejetées au même titre qu'une variable `signe est un entier`.
> L'erreur émise pointe directement sur le token de déclaration de
> la procédure — pas de cascade trompeuse.

## `owl test` charge tout le projet

`owl test` charge **et exécute** le top-level de tout le projet, pas
seulement les fichiers `test_*`. Un script qui produit un effet de bord au
niveau top-level s'exécutera pendant la campagne de tests. Garder les
effets de bord dans des procédures appelées explicitement.

## Tester une fenêtre : activer les vrais champs

Pendant `owl test`, les fenêtres ne montent pas leurs champs par défaut :
les tests sont rapides et n'ouvrent aucune fenêtre à l'écran. Une lecture
comme `SAI_X..Valeur` ou un événement `surAChaqueModification` ne reflète
alors pas le champ réel. Pour écrire un test qui exerce une saisie, un clic
ou un focus comme un utilisateur, activer les champs réels en tête de test :

```owl
TestModeHeadless(Faux)             // active les vrais champs pour ce test

OuvreSoeur(FEN_Saisie)             // ouvre sans bloquer, champs prêts
TestSimuleSaisie(SAI_Nom, "Léa")   // frappe réelle, caractère par caractère
dbgVérifieEgalité(SAI_Nom..Valeur, "Léa", "saisie lue depuis le champ")
Ferme(FEN_Saisie)
```

Trois points à retenir :

- `TestModeHeadless(Faux)` ne vaut que pour le test où il est écrit ; le
  test suivant repart en mode rapide.
- Utiliser `OuvreSoeur` (non bloquant), pas `Ouvre` (modal) qui figerait le
  test en attente de fermeture.
- `TestSimuleSaisie` et `TestSimuleSouris` rejouent l'entrée réelle :
  chaque frappe déclenche `surAChaqueModification`, un clic prend le focus.
  `Temporise(n)` laisse alors tourner les minuteurs et événements en
  attente.

À réserver aux tests qui ciblent vraiment l'interface : laisser le mode
rapide pour tout le reste.

## `OU` / `ET` évaluent les deux côtés

Les formes pleines `OU`, `ET` évaluent **les deux opérandes**, même si
le premier suffit à déterminer le résultat. Pour protéger un accès
quand un objet peut être `Null`, utiliser les formes court-circuit
`_OU_`, `_ET_` :

```owl
SI oCompte = Null OU oCompte.solde > 0 ALORS …    // crash si Null
SI oCompte = Null _OU_ oCompte.solde > 0 ALORS …  // safe
```

Doctrine complète dans `05_operateurs.md` §3.

## `ErreurDétectée` : lire juste après l'appel

`ErreurDétectée` reflète l'état de la dernière opération native.
Chaque appel suivant remet le flag à zéro (RAZ automatique). La
lecture doit donc venir **immédiatement** après l'appel à tester :

```owl
sRes est une chaîne = ChaineVersDate("blabla")
SI ErreurDétectée ALORS
    Trace("oups")    // ✓
FIN
```

Pas après deux `Trace` enchaînés : le 1er consomme la sentinelle de
fraîcheur, le 2e relit la valeur déjà remise à zéro. Pour réutiliser
le flag plus loin, le capturer dans une variable booléenne aussitôt
après l'appel :

```owl
sRes est une chaîne = NativeQuiPeutEchouer(arg)
bEcheck est un booléen = ErreurDétectée      // capture immédiate
Trace("résultat :", sRes, " — erreur :", bEcheck)
```

## `.` membre, `..` propriété

Un seul point appelle une méthode ou accède à un membre
(`oObjet.Methode()`, `jDoc.champ`). Deux points lisent une propriété
(`tabNoms..Occurrence`, `SAI_X..Valeur`). Confondre les deux produit une
erreur de compilation.

## `Trace(monétaire)` affiche la forme brute

`Trace` (et la conversion implicite vers chaîne) imprime un `monétaire`
sous sa **forme brute**, sans zéros de fin. `150.00 - 49.50` produit
`100.5`, pas `100.50`. C'est volontaire : la valeur stockée est exacte,
seule la représentation textuelle est compactée.

Pour un affichage 2 décimales typique comptable, passer par
`NumériqueVersChaîne` après cast en `réel` (ou en `numérique`) :

```owl
mSolde est un monétaire = 150.00
mDelta est un monétaire =  49.50
mNet   est un monétaire = mSolde - mDelta

Trace(mNet)                              // 100.5  (brut)

rNet est un réel = mNet
Trace(NumériqueVersChaîne(rNet, ".2f"))  // 100.50
```

Le cast intermédiaire vers `réel` est nécessaire : `NumériqueVersChaîne`
appliqué directement à un `monétaire` traite la valeur en unités
internes et produit un résultat surdimensionné.

## Tout est typé explicitement

Pas d'inférence de type, pas de `Variant`. Chaque variable annonce son
type à la déclaration. Cette contrainte est volontaire : elle rend chaque
fichier lisible sans deviner, pour un humain comme pour un agent.

## `ChaîneVersDate` sans masque — formes acceptées

Sans masque explicite, `ChaîneVersDate` auto-détecte cinq formes :

- `AAAAMMJJ`     — forme compacte (`"20261225"`)
- `JJ/MM/AAAA`   — européenne avec barres obliques (`"25/12/2026"`)
- `JJ-MM-AAAA`   — européenne avec tirets (`"25-12-2026"`)
- `AAAA-MM-JJ`   — ISO 8601 date-only (`"2026-12-25"`)
- `AAAA/MM/JJ`   — variante ISO avec barres obliques (`"2026/12/25"`)

La forme ISO `AAAA-MM-JJ` est recommandée pour les nouveaux scripts :
non ambiguë, internationale, alignée avec les formats d'échange standard
(logs, JSON, CSV).

Toute autre forme (dateheure, RFC-3339, format régional alternatif)
exige un masque explicite ou une constante dédiée :

```owl
dh est une dateheure = ChaîneVersDate("2019-08-25T17:15:00+02:00", maskDateInternet)
d  est une date      = ChaîneVersDate("12 mai 2026", "JJ MMMM AAAA")
```

Une chaîne qui ne correspond à aucune forme acceptée retourne une
chaîne vide et positionne `ErreurDétectée` à `Vrai`.

## Tester une native bloquante ou destructive : `MockCrée` couvre tout

`MockCrée` intercepte **n'importe quelle native** par pattern matching.
Pas besoin de chercher un système dédié par cas d'usage — un seul
mécanisme couvre les natives qui bloquent (`ConsoleLit` qui attend
stdin, `Avertissement` qui ouvre une boîte de dialogue) comme celles
qui ont des effets de bord destructifs (`VersPressePapier` qui écrase
le presse-papiers de l'utilisateur, `HTTPRequête` qui émet un appel
réseau réel).

Cas typique : tester un script qui lit l'entrée standard sans bloquer
la suite de tests.

```owl
// Retour fixe
mLit est un MockNative = MockCrée(ConsoleLit)
mLit.Quand().Retourne("Roger").Active()

// Séquence multi-lectures (chaque appel consomme l'élément suivant)
mEntree est un MockNative = MockCrée(ConsoleLit)
mEntree.Quand().RetourneSéquence(["entrée 1", "entrée 2", "fin"]).Active()
```

Même pattern pour neutraliser un appel réseau, une boîte de dialogue
Qt, ou une écriture clipboard : `MockCrée(HTTPRequête)`,
`MockCrée(Avertissement)`, `MockCrée(VersPressePapier)`. Une seule
infrastructure, déclarative et chaînable.

## `PROCEDURE Helper()` hors du bloc Fenêtre — erreur compile

Dans un fichier-fenêtre (`FEN_X.owl`), les helpers `PROCEDURE` qui
adressent les sous-champs doivent vivre **à l'intérieur du bloc
`est une Fenêtre`**, pas après son `FIN`. Le canon est strict :

```owl
// ✗ Rejeté à la compilation
FEN_Compteur est une Fenêtre
    PROPRIETES Titre = "Compteur" FIN
    LIB_Valeur est un Libellé FIN
FIN

PROCEDURE Rafraichit()                       // hors du bloc → erreur
    LIB_Valeur..Libellé = "..."
FIN
```

```owl
// ✓ Canon
FEN_Compteur est une Fenêtre
    PROPRIETES Titre = "Compteur" FIN

    PROCEDURE Rafraichit()                   // dans le bloc → ok
        LIB_Valeur..Libellé = "..."
    FIN

    LIB_Valeur est un Libellé FIN
FIN
```

La règle est posée par le canon strict OWL : un fichier-fenêtre
contient une fenêtre, ses sous-champs et ses helpers de contexte —
rien d'autre. Le câblage complet d'une application GUI multi-fichier
est documenté dans `15_lifecycle_champs.md` §9.

## Adresser un champ depuis un fichier voisin — impossible

Les sous-champs d'une fenêtre (`LIB_X`, `SAI_Y`, `BTN_Z`) sont
visibles **à l'intérieur du bloc `est une Fenêtre`** qui les déclare.
Un fichier voisin ne peut pas y accéder par leur nom :

```owl
// FEN_Détail.owl
FEN_Détail est une Fenêtre
    LIB_Total est un Libellé FIN
FIN
```

```owl
// autre_fichier.owl — ne compile pas
LIB_Total..Libellé = "..."           // LIB_Total inconnu ici
```

Pour partager une information entre fichiers, deux voies canon :

1. **État partagé via `projet.owl`** : une variable globale au projet,
   lue/écrite depuis le bloc Fenêtre qui en a besoin.
2. **Classe métier** (POO) : isole la logique sans connaître les
   champs, et reste réutilisable entre fenêtres.

L'exemple bout-en-bout figure dans `15_lifecycle_champs.md` §9.

## Pas de boucle longue dans `FinInitialisation`

`FinInitialisation` s'exécute **avant** l'entrée de la fenêtre dans sa
boucle d'événements (étape 4 sur 5 du cycle d'ouverture, voir
`15_lifecycle_champs.md`). Une boucle longue placée là bloque tout :
la fenêtre ne se peint jamais, rien ne s'affiche. `Temporise` ne sauve
pas ce cas — il ne maintient l'affichage que depuis la boucle
d'événements déjà vivante.

Le canon : `FinInitialisation` prépare l'état puis **rend la main**.
Toute boucle longue (animation, traitement itératif) se lance depuis
un handler d'événement (`surClic`, `surEntrée`, ...), qui s'exécute
lui dans la boucle vivante.

## Un handler occupé ne se ré-entre pas

Un handler qui ne rend pas la main (boucle avec `Temporise`) ne sera
pas re-déclenché par de nouveaux événements sur son propre champ : les
frappes alimentent bien le contenu du champ, mais l'événement de
modification n'est pas re-émis tant que le handler est occupé. Pour
lire une saisie en continu pendant une boucle, lire directement la
propriété du champ depuis la boucle (polling de `SAI_X..Valeur`),
pas par événement.

## `dCopie(source, destination, …)` — la source vient en premier

L'ordre des arguments de `dCopie` est **source en premier,
destination ensuite**. Beaucoup d'API courantes (memcpy, file
copy, etc.) inversent : destination puis source. Inverser ici
ne déclenche **aucune erreur** — le rendu sort vide
silencieusement, ce qui peut coûter du temps à diagnostiquer.

```owl
imgSrc  est une Image = dCréeImage(64, 64, Rouge)
imgDest est une Image = dCréeImage(128, 128, Blanc)

dCopie(imgSrc, imgDest, 0, 0, 64, 64, 32, 32, 64, 64)    // OK : copie src vers dest
dCopie(imgDest, imgSrc, 0, 0, 64, 64, 32, 32, 64, 64)    // PIÈGE : ordre inversé, rendu vide
```

Le réflexe : relire l'ordre en mnémo « **source d'abord, comme une
lecture de gauche à droite** ». `owl explain dCopie` confirme la
signature à tout moment.
