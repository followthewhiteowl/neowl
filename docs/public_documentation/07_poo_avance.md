# Programmation orientée objet — concepts avancés

Cette fiche prolonge `06_poo.md` (bases de la POO) avec les mécanismes
plus avancés : classes et méthodes abstraites, appel d'un ancêtre par
nom, membres et méthodes partagés (`GLOBAL` / `GLOBALE`), constantes
de classe, introspection dynamique avec `est`.

Pré-requis : maîtrise des chapitres 1 à 6 de `06_poo.md` (déclaration
de classe, méthodes inline, visibilité, héritage simple, interfaces).

---

## 1. Classes et méthodes abstraites

Une classe peut être déclarée **abstraite** : elle ne peut pas être
instanciée directement, mais sert de base à des classes dérivées
concrètes.

```owl
cFigure est une Classe, abstraite
PUBLIC
    sNom est une chaîne

    PROCEDURE Constructeur(nom est une chaîne)
        :sNom = nom
    FIN

    PROCEDURE ABSTRAITE Aire() : réel    // pas de FIN : aucun corps
FIN
```

Une **méthode abstraite** se déclare avec `PROCEDURE ABSTRAITE` (EN :
`PROCEDURE ABSTRACT`), **sans corps et sans `FIN`** (rien à fermer),
à l'intérieur du bloc classe. Elle décrit une signature que les
classes dérivées concrètes **doivent** fournir. Une classe concrète
qui hérite d'une classe abstraite sans implémenter toutes ses
méthodes abstraites produit une erreur de compilation.

```owl
cFigureCercle est une Classe
    Hérite de cFigure
PUBLIC
    rRayon est un réel

    PROCEDURE Constructeur(nom est une chaîne, rayon est un réel)
        Constructeur cFigure(nom)
        :rRayon = rayon
    FIN

    PROCEDURE Aire() : réel
        RENVOYER 3.14159 * :rRayon * :rRayon
    FIN
FIN
```

Tentative d'instancier `cFigure` directement → erreur de compilation.

---

## 2. Appeler un ancêtre par nom

`Ancêtre.M()` cible le parent **immédiat** d'une classe (cf.
`06_poo.md` § 5). Pour atteindre un ancêtre situé plus haut dans la
chaîne d'héritage, on nomme la classe ancêtre directement :
`cAncestor.M()`.

L'appel est un **dispatch statique direct** vers la version définie
dans la classe nommée — sans re-résolution virtuelle, sans récursion
possible.

```owl
cAnimal est une Classe
PUBLIC
    PROCEDURE Crier()
        Trace("bruit générique")
    FIN
FIN
```

```owl
cChien est une Classe
    Hérite de cAnimal
PUBLIC
    PROCEDURE Crier()
        Trace("Wouf")
    FIN
FIN
```

```owl
cChiot est une Classe
    Hérite de cChien
PUBLIC
    PROCEDURE Crier()
        // Ancêtre.Crier() exécuterait cChien.Crier() ("Wouf").
        // cAnimal.Crier() saute cChien et exécute directement
        // la version racine ("bruit générique").
        cAnimal.Crier()
        Trace("Wif")
    FIN
FIN
```

Différences à retenir :

- `Ancêtre.M()` cible **le parent immédiat**.
- `cAncestor.M()` cible **la version définie dans `cAncestor`**, en
  sautant tous les niveaux intermédiaires.
- Le compilateur vérifie que `cAncestor` est bien un ancêtre de la
  classe courante (ou expose `M` en `GLOBALE`). Sinon, erreur
  explicite.

---

## 3. Membres et méthodes partagés (`GLOBAL`, `GLOBALE`)

Un **membre partagé** est un champ ou une méthode attaché à **la
classe** plutôt qu'à une instance : il existe en un seul exemplaire,
indépendamment du nombre d'instances créées. Les mots-clés sont
`GLOBAL` pour les champs et `GLOBALE` pour les méthodes.

### Champ `GLOBAL` : un exemplaire par classe

Un bloc `GLOBAL` déclare des champs partagés par toutes les instances
de la classe, accessibles **sans instance**.

```owl
cCompteur est une Classe
GLOBAL
    nTotal est un entier = 0
PUBLIC
    sNom est une chaîne

    PROCEDURE Constructeur(nom est une chaîne)
        :sNom = nom
        cCompteur.nTotal = cCompteur.nTotal + 1
    FIN
FIN
```

Chaque création d'instance incrémente le compteur partagé. La lecture
et l'écriture passent par le nom de la classe :
`cCompteur.nTotal`.

### Méthode `GLOBALE` : appelable sans instance

Une méthode inline déclarée avec le modificateur `GLOBALE` (EN :
`GLOBAL`) s'appelle directement sur la classe, sans nécessiter
d'instance. Elle n'a pas accès au préfixe `:` (il n'y a pas de
`this`).

```owl
cMath est une Classe
GLOBAL
    rPi est un réel = 3.14159
PUBLIC
    PROCEDURE GLOBALE Carre(n est un entier) : entier
        RENVOYER n * n
    FIN

    PROCEDURE GLOBALE AireCercle(r est un réel) : réel
        RENVOYER cMath.rPi * r * r
    FIN
FIN
```

```owl
Trace(cMath.Carre(7))           // 49
Trace(cMath.AireCercle(2.0))    // 12.56636
```

### Sucre singleton

Lorsqu'un membre `GLOBAL` a pour type la classe qui le contient,
**une instance unique** est créée automatiquement au démarrage :

```owl
cConfig est une Classe
GLOBAL
    // Pas de parenthèses : syntaxe singleton, instance unique créée
    // automatiquement au démarrage (différent de `est un cConfig()`
    // qui appellerait explicitement le constructeur).
    m_Inst est cConfig
PUBLIC
    sNom est une chaîne = ""
    nVersion est un entier = 0
FIN
```

```owl
cConfig.m_Inst.sNom = "production"
cConfig.m_Inst.nVersion = 42
```

Deux accès à `cConfig.m_Inst` désignent **le même objet**. Une seule
limite : le constructeur de la classe doit être appelable sans
argument (zéro paramètre, ou pas de constructeur déclaré).

### Membres `GLOBAL` non hérités

Les membres et méthodes `GLOBAL` / `GLOBALE` ne sont **pas hérités**
par les classes dérivées. Chaque classe a son propre espace statique.
Si `cBase` déclare `GLOBAL nCompteur`, alors `cBase.nCompteur` existe
mais `cDerivee.nCompteur` n'existe pas.

---

## 4. Constantes de classe

Une classe peut déclarer des constantes — valeurs immuables liées au
type, accessibles sans instance. Deux formes au choix :

```owl
cMath est une Classe
    CONSTANT PI = 3.14159
FIN
```

```owl
cConfig est une Classe
    CONSTANT
        K = 5
        J = 10
        Nom = "Application"
    FIN
FIN
```

L'accès se fait via le nom de la classe :

```owl
Trace(cMath.PI)
Trace(cConfig.Nom)
```

Les constantes de classe sont en lecture seule. Toute tentative
d'écriture produit une erreur. Elles sont publiques par défaut et
constituent l'endroit idoine pour des seuils, des codes, ou des
chaînes de référence partagées.

---

## 5. Introspection : `est`

L'opérateur `est <NomClasse>` teste dynamiquement le type d'une
instance. Il retourne `Vrai` si la valeur est une instance de la
classe **ou** d'une de ses descendantes ; `Faux` sinon.

```owl
oChat est un cChat("Felix", Vrai)

SI oChat est cChat ALORS
    Trace("type exact")
FIN

SI oChat est cAnimal ALORS
    Trace("via héritage")          // Vrai aussi
FIN

SI oChat est cChien ALORS
    Trace("frère")                 // Faux, classes sœurs
FIN
```

L'opérateur respecte la chaîne d'héritage : une instance d'une classe
dérivée satisfait toujours le test pour ses ancêtres.

### Test sur interface

`est` accepte aussi un nom d'interface à droite. Il retourne `Vrai`
si la classe de l'instance (ou un de ses ancêtres) **implémente**
l'interface ; `Faux` sinon.

```owl
iAffichable est une Interface
    PROCEDURE Afficher()
FIN
```

```owl
cProduit est une Classe
    implemente iAffichable
PUBLIC
    sNom est une chaîne

    PROCEDURE Constructeur(nom est une chaîne)
        :sNom = nom
    FIN

    PROCEDURE Afficher()
        Trace("produit : " + :sNom)
    FIN
FIN
```

```owl
p est un cProduit("clavier")

SI p est iAffichable ALORS
    p.Afficher()           // exécuté : Vrai
FIN
```

Comme pour le test sur classe, l'héritage est pris en compte : si
`cParent implemente iX` et que `cEnfant Hérite de cParent`, alors
toute instance de `cEnfant` satisfait `obj est iX`. Le test sur une
valeur non-instance (entier, chaîne…) retourne `Faux` sans erreur.

---

## 6. Récapitulatif visuel

Un fichier de classe complet, mettant en œuvre la plupart des
mécanismes décrits dans `06_poo.md` et dans cette fiche :

```owl
cCompteurNomme est une Classe
    Hérite de cCompteur
    implemente iAffichable
GLOBAL
    sPrefixeGlobal est une chaîne = "compteur-"
PUBLIC
    sLibelle est une chaîne
PROTÉGÉ
    nDerniereLecture est un entier
    CONSTANT SEUIL_MAX = 1000

PUBLIC
    PROCEDURE Constructeur(sNomInit est une chaîne)
        Constructeur cCompteur(sNomInit)
        :sLibelle = cCompteurNomme.sPrefixeGlobal + sNomInit
        :nDerniereLecture = 0
    FIN

    PROCEDURE Destructeur()
        Trace("Fermeture : " + :sLibelle)
    FIN

    PROCEDURE Afficher()
        Trace(:sLibelle + " = " + :nDerniereLecture)
    FIN

    PROCEDURE Lire() : entier
        // Étend la version parente : appel via Ancêtre puis log local.
        :nDerniereLecture = Ancêtre.Lire()
        Trace("lecture sur " + :sLibelle)
        RENVOYER :nDerniereLecture
    FIN

    PROCEDURE Reinitialiser()
        // Saute Ancêtre direct, appelle la version racine par nom.
        cCompteur.Reinitialiser()
    FIN

    PROCEDURE GLOBALE SeuilAtteint(n est un entier) : booléen
        RENVOYER n >= cCompteurNomme.SEUIL_MAX
    FIN
FIN
```

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Bases de la POO | `06_poo.md` |
| Aperçu général et top-level | `00_apercu_du_langage.md` |
| Système de types et coercions | `01_types_et_coercions.md` |
| Préfixes de nommage (`c`, `i`, `s`, `n`…) | `12_prefixes_de_nommage.md` |

Pour la liste exhaustive des classes natives et de leurs méthodes :
`owl categories`, puis `owl explain <nom>`.
