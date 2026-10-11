# Programmation orientée objet

On définit des **classes**, on crée des **instances**, on relie les
classes par **héritage** ou par des **interfaces**.

Cette fiche couvre le modèle objet : déclaration, constructeur,
méthodes, visibilité, héritage, polymorphisme, interfaces, classes
abstraites, membres et méthodes partagés, constantes de classe et
introspection.

---

## 1. Une classe, un fichier

Un fichier `.owl` qui contient une déclaration de classe ne contient
que :

- la déclaration de la classe avec son corps complet (champs,
  constructeur, destructeur, méthodes — toutes déclarées **inline**),
- des commentaires.

Pas de code de niveau supérieur, pas de seconde classe, pas de
fenêtre ni d'état dans le même fichier. Toute violation produit une
erreur de compilation pointant la ligne fautive.

> **Méthodes déclarées inline.** Les méthodes d'une classe sont
> déclarées **dans le corps de la classe**, entre le `est une Classe`
> et le `FIN` final. Toute déclaration hors du corps de classe
> produit une erreur de compilation.

---

## 2. Déclarer une classe

Une classe se déclare par un bloc `est une Classe … FIN`. Les
membres (champs **et** méthodes) sont regroupés par mode d'accès
(`PUBLIC` par défaut, `PRIVÉ`, `PROTÉGÉ`). Une convention courante
préfixe le nom d'une classe par `c` minuscule pour la distinguer
d'une instance ou d'un type primitif.

```owl
cCompte est une Classe
PUBLIC
    sProprietaire est une chaîne
    rSolde est un réel
FIN
```

On instancie une classe en déclarant une variable de son type :

```owl
oCompte est un cCompte("Alice")
oCompte.rSolde = 100
```

---

## 3. Méthodes : déclarées inline

Toutes les méthodes (ordinaires, constructeur, destructeur,
partagées) se déclarent **à l'intérieur du corps de la classe**,
entre `est une Classe` et le `FIN` final. La méthode est rattachée
directement à la classe par sa position dans le bloc — pas besoin de
préfixer son nom par celui de la classe.

```owl
cCompte est une Classe
PUBLIC
    sProprietaire est une chaîne
    rSolde est un réel

    PROCEDURE Constructeur(nom est une chaîne)
        :sProprietaire = nom
        :rSolde = 0
    FIN

    PROCEDURE Destructeur()
        Trace("Fermeture du compte de " + :sProprietaire)
    FIN

    PROCEDURE Crediter(rMontant est un réel)
        :rSolde += rMontant
    FIN

    PROCEDURE Solde() : réel
        RENVOYER :rSolde
    FIN
FIN
```

Le préfixe `:` désigne le membre de l'instance courante — l'équivalent
du `this.` ou `self.` d'autres langages, en plus court. On l'utilise
pour lire et écrire un champ (`:rSolde += rMontant`) ou pour appeler
une autre méthode de la même classe (`:Recalculer()`).

L'appel est sans surprise :

```owl
oCompte est un cCompte("Alice")
oCompte.Crediter(50)
Trace(oCompte.Solde())
```

### Constructeur et destructeur

**Constructeur** : `PROCEDURE Constructeur(...)` déclarée inline. Il
s'exécute à chaque création d'instance. Une classe sans constructeur
explicite est instanciable sans arguments (un constructeur trivial
implicite est fourni).

**Instanciation sans parenthèses (constructeur trivial).** Quand le
constructeur ne requiert aucun argument (pas de constructeur explicite,
ou constructeur sans paramètre, ou paramètres tous à valeur par défaut),
les deux formes sont équivalentes :

```owl
f est un cFichier()        // forme explicite
f est un cFichier          // forme courte — auto-instanciation
```

Si le constructeur exige un ou plusieurs arguments, la forme courte
est rejetée à la compilation avec un message explicite : *"Constructeur
de `<nom>` requiert N argument(s) : utiliser `<nom>(...)` pour
instancier."*. Cela ferme un piège classique où une variable typée
classe restait silencieusement à `Null`. Pour déclarer une instance
volontairement nullable, utiliser le marqueur `<nullable>` :
`o est un cFoo <nullable>`.

**Destructeur** : `PROCEDURE Destructeur()` déclarée inline, sans
paramètre. Le destructeur reste optionnel. Il est déclenché
explicitement par le mot-clé `Libérer` :

```owl
cFichier est une Classe
PUBLIC
    nHandle est un entier = 0

    PROCEDURE Constructeur()
        :nHandle = 1       // ouverture simulée
        Trace("fichier ouvert")
    FIN

    PROCEDURE Destructeur()
        Trace("fichier fermé")
        :nHandle = 0
    FIN
FIN
```

```owl
f est un cFichier()
// ... travail sur f ...
Libérer f
// Sortie : "fichier ouvert" / "fichier fermé"
```

Après `Libérer f`, la variable `f` est invalide ; toute réutilisation
produit une erreur d'exécution. Une instance non libérée explicitement
est réclamée plus tard par le ramasse-miettes ; l'exécution du
destructeur n'est alors pas garantie dans un délai déterministe — à
réserver aux objets sans ressource externe à fermer.

### Modificateurs de méthode

Une méthode inline peut porter des modificateurs explicites entre
`PROCEDURE` et son nom : `PUBLIC`, `PRIVÉ` / `PRIVATE`, `PROTÉGÉ` /
`PROTECTED`, `GLOBALE` / `GLOBAL`, `ABSTRAITE` / `ABSTRACT`.

```owl
cCompte est une Classe
PUBLIC
    rSolde est un réel

    PROCEDURE PUBLIC Solde() : réel
        RENVOYER :rSolde
    FIN

    PROCEDURE PRIVÉ Recalculer()
        // détail interne
    FIN
FIN
```

En l'absence de modificateur explicite, **la méthode hérite la
visibilité du bloc d'accès courant** — exactement comme les champs.
Dans l'exemple ci-dessous, `Crediter` et `Solde` sont publiques (bloc
`PUBLIC`), tandis que `Recalculer` est privée (bloc `PRIVÉ`) :

```owl
cCompte est une Classe
PUBLIC
    rSolde est un réel

    PROCEDURE Crediter(montant est un réel)
        :rSolde += montant
        :Recalculer()
    FIN

    PROCEDURE Solde() : réel
        RENVOYER :rSolde
    FIN

PRIVÉ
    PROCEDURE Recalculer()
        // détail interne
    FIN
FIN
```

### Multi-arité : paramètres par défaut

La surcharge n'est pas supportée : deux méthodes du même nom dans la
même classe sont interdites au moment de la compilation. Pour fournir
plusieurs façons d'appeler la même action, on utilise des **paramètres
par défaut** :

```owl
cPersonne est une Classe
PUBLIC
    sNom est une chaîne
    nAge est un entier

    // Appelable : cPersonne(), cPersonne("Alice"), cPersonne("Alice", 30)
    PROCEDURE Constructeur(nom est une chaîne = "inconnu",
                           age est un entier = 0)
        :sNom = nom
        :nAge = age
    FIN
FIN
```

### Accès contrôlé via méthodes nommées

Une expression comme `obj.Champ = val` agit uniquement sur un **champ
déclaré** ; elle ne déclenche pas l'appel d'une méthode cachée. Pour
exposer un accès contrôlé, on déclare deux méthodes inline nommées de
façon explicite :

```owl
cCompte est une Classe
PUBLIC
    rSolde est un réel

    PROCEDURE Solde() : réel
        RENVOYER :rSolde
    FIN

    PROCEDURE DefinirSolde(rNouveau est un réel)
        SI rNouveau < 0 ALORS
            ExceptionDéclenche(1, "Solde négatif refusé")
        FIN
        :rSolde = rNouveau
    FIN
FIN
```

Pour une paire getter/setter, choisir deux noms distincts (`Solde()`
/ `DefinirSolde(v)`, `Largeur()` / `LargeurChange(v)`, etc.).

### Chaînage de méthodes sur une instance

Le mot-clé `objet` désigne l'**instance courante** à l'intérieur d'une
méthode. Il est utilisable comme valeur (passage en argument, retour
de méthode, affectation à une variable typée classe) mais **pas comme
préfixe d'accès aux membres** : pour lire ou écrire un champ, le
préfixe `:` reste la seule forme valide (`:sNom`, `:rSolde`).

Hors d'une méthode, référencer `objet` est une erreur de compilation
`'objet' : utilisable uniquement à l'intérieur d'une méthode de classe`.
Les alias EN `this` et `object` sont reconnus au runtime ; la forme
canonique conservée par le formatter reste en minuscules.

Cas d'usage canonique : une méthode qui retourne `objet` permet le
chaînage style *builder* sur une classe utilisateur.

```owl
cBuilder est une Classe
PUBLIC
    sNom est une chaîne
    nAge est un entier

    PROCEDURE FixeNom(sValeur est une chaîne) : cBuilder
        :sNom = sValeur
        RENVOYER objet
    FIN

    PROCEDURE FixeAge(nValeur est un entier) : cBuilder
        :nAge = nValeur
        RENVOYER objet
    FIN

    PROCEDURE Affiche()
        Trace(:sNom + " (" + :nAge + " ans)")
    FIN
FIN

oB est un cBuilder()
oB.FixeNom("Alice").FixeAge(42).Affiche()
```

Le chaînage reste également disponible sur les méthodes prefix natives
qui le documentent explicitement (par exemple les méthodes de tableau
`.Ajoute(...).Trie()...` — voir `owl explain Ajoute`).

---

## 4. Visibilité : PUBLIC, PRIVÉ, PROTÉGÉ

Les membres d'une classe sont groupés sous un mode d'accès, vérifié à
la compilation :

| Mode | Forme EN | Visibilité |
|---|---|---|
| `PUBLIC` (défaut) | `PUBLIC` | partout |
| `PRIVÉ` | `PRIVATE` | classe propriétaire uniquement |
| `PROTÉGÉ` | `PROTECTED` | classe + classes dérivées |

```owl
cCompte est une Classe
PUBLIC
    sProprietaire est une chaîne
PRIVÉ
    rSolde est un réel
PROTÉGÉ
    sNumeroInterne est une chaîne
FIN
```

Tout accès qui sort du périmètre autorisé produit une erreur de
compilation explicite, mentionnant la ligne, le champ et la classe
concernés. Aucun coût n'est payé au runtime — la vérification se fait
à 100 % avant exécution.

La visibilité s'applique aux **champs et aux méthodes** : une méthode
déclarée dans un bloc `PRIVÉ` n'est appelable que depuis la classe
propriétaire ; une méthode dans un bloc `PROTÉGÉ` n'est appelable que
depuis la classe et ses dérivées. Une méthode peut aussi forcer sa
visibilité par un modificateur explicite (`PROCEDURE PRIVÉ M()`) qui
prime sur le bloc d'accès courant.

**Note sur l'accès indirect.** Le check de visibilité s'applique
uniquement aux accès statiques `obj.NomDuChamp`. Un accès indirect
par expression dynamique `obj.{sNomDuChamp}` (où `sNomDuChamp` est
une variable contenant le nom à la volée) **contourne le check de
compilation**, car le compilateur ne peut pas connaître la cible
avant exécution. Ce comportement est intentionnel et limité au cas
de l'évaluation dynamique : pour conserver la garantie d'encapsulation,
préférer l'accès statique nominal.

**Instance ou partagé : un axe distinct de la visibilité.** Les blocs
`LOCAL` et `GLOBAL` choisissent *où vit* un membre, indépendamment de
sa visibilité. `LOCAL` (forme EN : `LOCAL`) déclare des **membres
d'instance** — un exemplaire par objet, c'est le mode par défaut.
`GLOBAL` déclare des **membres partagés** par toutes les instances
(voir `07_poo_avance.md`). Les deux axes se combinent : un bloc `LOCAL`
placé après un bloc `GLOBAL` revient aux membres d'instance sans
changer la visibilité courante, exactement comme `GLOBAL` ne modifie
pas la visibilité.

---

## 5. Héritage

Une classe peut hériter d'une autre via la clause `Hérite de` (forme
EN : `Inherits`). L'héritage est simple : une classe a au plus un
parent direct.

```owl
cAnimal est une Classe
PUBLIC
    sNom est une chaîne

    PROCEDURE Constructeur(nom est une chaîne)
        :sNom = nom
    FIN

    PROCEDURE Presenter()
        Trace("Animal : " + :sNom)
    FIN
FIN
```

```owl
cChat est une Classe
    Hérite de cAnimal
PUBLIC
    bInterieur est un booléen

    PROCEDURE Constructeur(nom est une chaîne, interieur est un booléen)
        Constructeur cAnimal(nom)
        :bInterieur = interieur
    FIN
FIN
```

Le constructeur de la classe parente s'appelle par le mot-clé
`Constructeur cParent(args)` — il doit être la **première
instruction** du constructeur enfant lorsque le parent attend des
arguments.

Les membres et méthodes hérités sont accessibles directement sur
l'instance dérivée :

```owl
oChat est un cChat("Felix", Vrai)
Trace(oChat.sNom)            // hérité de cAnimal
oChat.Presenter()            // méthode héritée
```

### Redéfinition (polymorphisme)

Une méthode héritée est redéfinie en redéclarant simplement la même
signature dans la classe dérivée. Aucun mot-clé `virtuel` n'est
nécessaire — toute méthode est redéfinissable par défaut.

```owl
cChat est une Classe
    Hérite de cAnimal
PUBLIC
    PROCEDURE Presenter()
        Trace("Chat : " + :sNom)
    FIN
FIN
```

L'appel `oChat.Presenter()` exécute la version `cChat`. La résolution
est dynamique : si `oChat` est passé à une procédure qui attend un
`cAnimal`, c'est toujours la version `cChat.Presenter()` qui est
appelée.

### Appeler la version parente

Le mot-clé `Ancêtre` (forme EN : `Ancestor`) appelle explicitement la
version définie dans le parent direct d'une classe :

```owl
cChat est une Classe
    Hérite de cAnimal
PUBLIC
    PROCEDURE Presenter()
        Ancêtre.Presenter()       // exécute cAnimal.Presenter()
        Trace("  (et il miaule)")
    FIN
FIN
```

Utile pour étendre le comportement du parent au lieu de le remplacer
entièrement.

> Pour appeler un ancêtre situé plus haut dans la chaîne (sauter un
> niveau intermédiaire), la syntaxe `cAncestor.M()` est disponible.
> Voir `07_poo_avance.md` § 2.

---

## 6. Interfaces

Une **interface** déclare un contrat : la liste des méthodes qu'une
classe doit fournir pour la satisfaire. Elle n'a ni champs ni
implémentation, seulement des signatures.

```owl
iAffichable est une Interface
    PROCEDURE Afficher()
FIN
```

Une classe déclare qu'elle satisfait un contrat avec la clause
`implemente` (EN : `implements`) :

```owl
cProduit est une Classe
    implemente iAffichable
PUBLIC
    sNom est une chaîne
    rPrix est un réel

    PROCEDURE Constructeur(nom est une chaîne, prix est un réel)
        :sNom = nom
        :rPrix = prix
    FIN

    PROCEDURE Afficher()
        Trace(:sNom + " : " + :rPrix)
    FIN
FIN
```

La conformité est **vérifiée à la compilation**. Si une méthode du
contrat manque, ou si une signature diverge (nombre de paramètres,
types, type de retour), c'est une erreur de compilation explicite.
Aucun coût n'est payé à l'exécution.

### Héritage du contrat

Si `cParent implemente iX` et que `cEnfant Hérite de cParent`, alors
`cEnfant` satisfait automatiquement `iX` (par héritage des méthodes).
Pas besoin de re-déclarer `implemente iX` dans la classe enfant — le
compilateur remonte la chaîne d'héritage pour le vérifier.

Une classe peut implémenter plusieurs interfaces en les listant
séparées par des virgules (`implemente iA, iB`).

---

## 7. Bilinguisme

Comme tout symbole du langage, les mots-clés de la POO existent sous
une forme française canonique et une forme anglaise canonique. Les
deux sont interchangeables ligne par ligne.

| Forme FR canon | Forme EN canon |
|---|---|
| `Classe` | `Class` |
| `Interface` | `Interface` |
| `Hérite de` | `Inherits` |
| `implemente` | `implements` |
| `abstraite` | `abstract` |
| `Constructeur` | `Constructor` |
| `Destructeur` | `Destructor` |
| `Ancêtre` | `Ancestor` |
| `est` (introspection) | `is` |
| `PUBLIC` / `PRIVÉ` / `PROTÉGÉ` | `PUBLIC` / `PRIVATE` / `PROTECTED` |
| `LOCAL` (membre d'instance) | `LOCAL` |
| `GLOBAL` (membre) | `GLOBAL` |
| `GLOBALE` (méthode) | `GLOBALE` |
| `CONSTANT` | `CONSTANT` |
| `FIN` | `END` |

Le scanner tolère également l'absence d'accents et les variantes de
casse à l'écriture ; le formatter restaure la forme canon à la
sauvegarde.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général et top-level | `00_apercu_du_langage.md` |
| Système de types et coercions | `01_types_et_coercions.md` |
| Valeur Null et déréférencement | `02_null_et_nullable.md` |
| POO — concepts avancés (abstraites, GLOBAL, CONSTANT, `est`, ancêtre nommé) | `07_poo_avance.md` |
| Conventions d'écriture | `10_conventions_de_code.md` |
| Préfixes de nommage (`c`, `i`, `s`, `n`…) | `12_prefixes_de_nommage.md` |
| Charte de test et scénarios | `11_charte_de_test.md` |

Pour la liste exhaustive des classes natives et de leurs méthodes :
`owl categories`, puis `owl explain <nom>`.
