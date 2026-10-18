# Conventions de code OWL

Cette fiche fixe les conventions d'écriture du code OWL : casse, accents,
bilinguisme, nommage, présentation, commentaires. Elle est la référence
opposable au formatter et aux agents qui produisent du code.

Principe directeur : **le formatter est juge final.** Toute forme tolérée
à l'écriture est restaurée à sa forme canon au prochain `owl fmt`. Les
conventions ci-dessous décrivent ce que le formatter écrit, pas ce qu'il
accepte en entrée — il accepte beaucoup plus.

---

## 1. Casse et accents

Pour les **identifiants** (variables, paramètres, procédures
utilisateur, types user-defined), le scanner est :

- **insensible à la casse** : `prix`, `Prix`, `PRIX` désignent la même
  entité.
- **insensible aux accents** : `cle`, `clé`, `Clé`, `CLE` désignent la
  même entité.

Cette tolérance n'autorise pas le laxisme : à la sauvegarde, le
formatter normalise les **symboles connus** (mots-clés, types builtin,
fonctions natives, constantes du registry) vers leur forme canonique
exacte, accents compris.

Les identifiants **inventés** par l'utilisateur (variables, paramètres,
champs, constantes, noms de procédures et de classes) sont également
canonisés par le formatter : chaque usage est ramené à la **graphie de
déclaration**. La graphie de déclaration est celle écrite à l'endroit où
l'identifiant est introduit (déclaration de variable, signature de
procédure, déclaration de champ ou de constante). Le formatter résout :

- **Intra-fichier** : la déclaration est cherchée dans le fichier courant.
  Pour une variable ou un paramètre local à une procédure, la résolution
  est bornée au scope de la procédure (un homonyme dans une autre
  procédure conserve sa propre graphie).
- **Cross-fichier (projet)** : si le fichier appartient à un projet
  (`projet.owl` ancêtre, cf. fiche modèle de projet), la déclaration est
  cherchée dans tous les fichiers du projet. Une procédure ou un champ
  déclaré dans un fichier voisin est canonisé à sa graphie de déclaration
  dans les fichiers qui l'utilisent. En cas de déclarations multiples
  avec des casses divergentes, le formatter s'abstient (verbatim).

Choisir sciemment la graphie de déclaration est donc important : c'est
elle qui apparaît à chaque usage après formatage.

| Catégorie | Forme canon |
|---|---|
| Mot-clé de contrôle (SI, POUR, BOUCLE, FIN, …) | MAJUSCULES |
| Type builtin (`chaîne`, `monétaire`, `réel`, `dateheure`, …) | minuscules, accents inclus quand canon |
| Fonction native (`ChaineDecoupe`, `HTTPRequête`, `MockCrée`, …) | PascalCase, accents inclus |
| Constante de registry (`httpGet`, `colTypeTexte`, `errMessage`, …) | préfixe minuscule + suffixe descriptif |
| Mot-clé de structure (`Structure`, `Classe`, `Enumération`, `Combinaison`) | Initiale majuscule |
| Modificateur (`PUBLIC`, `PRIVE`, `PROTEGE`) | MAJUSCULES |

---

## 2. Bilinguisme FR / EN

Tout mot-clé, type builtin, fonction native, constante et propriété
existe sous une forme française canonique et une forme anglaise
canonique. Les deux sont strictement équivalentes pour le compilateur.
Pour la lisibilité :

- **Préférer une langue principale par fichier.** Mélanger `SI` et
  `END` dans le même bloc est techniquement accepté mais visuellement
  désagréable.
- **Le formatter respecte la langue de la ligne**, pas du fichier :
  une ligne écrite en FR reste en FR, une ligne EN reste en EN. Pas de
  conversion automatique d'une langue vers l'autre.
- **Pour la doc générée et les exemples publics**, la langue FR est la
  référence. Les scénarios de test sont attendus en FR sauf
  raison explicite.

---

## 3. Nommage

OWL adopte un système de préfixes pour les variables et les champs
d'interface, hérité d'une convention métier éprouvée. Le détail complet
des préfixes (portée, type, champ) est dans la fiche
`12_prefixes_de_nommage.md`. Les principes synthétiques :

### 3.1 Variables

Une variable porte un **préfixe de portée** (en minuscule, facultatif) suivi
d'un **préfixe de type** (en minuscule, facultatif), puis du nom en PascalCase.

```owl
sNom est une chaîne                  // locale, chaîne
nAge est un entier                   // locale, entier
gbModeTest est un booléen            // globale projet, booléen
m_solde est un monétaire             // membre d'instance, monétaire
```

Pour les indices de boucle (`i`, `j`, `k`) ou les variables de très
courte vie, le préfixe est facultatif.

### 3.2 Procédures

Convention : **PascalCase**, sans préfixe.

```owl
PROCEDURE CalculerTotal(tabPanier est un tableau de monétaires) : monétaire
PROCEDURE EnvoyerCourriel(sDestinataire est une chaîne)
```

Pour les procédures de scénario de test, le préfixe `scenario_`,
`test_` ou `should_` est obligatoire (voir `11_charte_de_test.md`).

### 3.3 Types user-defined

Par convention on distingue le **type** de l'**instance** par la casse du préfixe :

| Catégorie | Type | Instance |
|---|---|---|
| Énumération | `EStatut` | `eStatutCourant` |
| Combinaison | `COPermissions` | `coDroitsUtilisateur` |
| Structure | `STAdresse` | `stLivraison` |
| Classe | `cCompte` | `oCompteCourant` |

Détail et exemples complets dans `12_prefixes_de_nommage.md`.

### 3.4 Constantes

Les constantes scalaires user-defined adoptent les MAJUSCULES avec
souligné séparateur :

```owl
TAUX_TVA est un réel = 20.0
MAX_TENTATIVES est un entier = 3
```

Pour des familles de valeurs, préférer une Énumération nommée plutôt
qu'une série de constantes plates.

### 3.5 Mots réservés du langage

Certains noms sont verrouillés au niveau scanner et ne peuvent pas
servir de nom de variable, de paramètre, ni de champ (structure ou
classe). La liste est volontairement finie et stable : elle correspond
aux types et aux modificateurs qui font partie du noyau du langage.

**Scalaires primitifs** :

- `chaîne` (alias EN : `string`)
- `entier` (`integer`)
- `réel` (`real`)
- `monétaire` (`currency`)
- `numérique` (`numeric`)
- `caractère` (`character`)
- `booléen` (`boolean`)
- `buffer`
- `octet` (`byte`)
- `octets` (`bytes`)

**Composites structurels** :

- `tableau` (`array`)
- `structure`
- `énumération` (`enumeration`)
- `combinaison` (`combination`)
- `liste` (`list`)
- `pile` (`stack`)
- `file` (`queue`)
- `associatif` (`associative`)

**Modificateurs** :

- `fixe` (`fixed`)
- `dynamique` (`dynamic`)
- `par` (`by`)
- `sans` (`unsigned`)
- `signe`

**Déclarateurs** :

- `classe` (`class`)
- `interface`
- `procedure`
- `fonction` (`function`)

Tenter d'utiliser l'un de ces noms en position de variable produit une
erreur de compilation explicite :

```owl
chaîne est une chaîne = "Bonjour"   // refusé : mot réservé
```

```
Le mot 'chaîne' est un mot-clé réservé du langage et ne peut pas être
utilisé comme nom de variable.
```

**Noms libres** comme identifiants, malgré une apparence proche d'un
type :

- Types valeur datetime : `date`, `heure`, `dateheure`, `durée`
- Types valeur génériques : `json`, `uuid`
- Tous les types du registre : `httpRequête`, `chrono`, `image`,
  `couleur`, …
- Tous les types user-defined (Structures, Classes, Énumérations,
  Combinaisons déclarées dans le projet).

Ces noms restent disponibles comme variables, paramètres ou champs
(`durée est une durée`, `date est une date`, `httpRequête est une
chaîne`), même si le contexte permet de distinguer la déclaration
d'usage du nom comme type. Cette tolérance reflète la pratique courante
et n'introduit aucune ambiguïté : la grammaire du langage tranche.

### 3.6 Champs de fenêtre, état, autres champs

Les noms de champs UI portent un préfixe MAJUSCULES suivi d'un souligné
qui dénote le type de champ : `FEN_` pour la fenêtre elle-même, `LIB_`
pour libellé, `BTN_` pour bouton, `SAI_` pour saisie, `TABLE_` pour
table, etc. La liste complète des champs supportés est dans
`12_prefixes_de_nommage.md`.

```owl
FEN_Login est une Fenêtre
    LIB_Titre est un Champ Libellé
    SAI_Utilisateur est un Champ Saisie
    SAI_MotDePasse est un Champ Saisie
    BTN_Valider est un Champ Bouton
FIN
```

---

## 4. Indentation et présentation

Pour **le compilateur**, OWL est indentation-indépendant : la structure
du code est portée par les mots-clés (`FIN`, `END`, etc.), pas par
l'indentation. Tout code compile, quelle que soit la disposition.

Pour **le formatter**, la présentation est figée et imposée : à la
sauvegarde, le code est ré-indenté selon la profondeur des blocs. La
forme canon utilisée par le formatter :

- **Une tabulation par niveau d'imbrication.** Pas d'espaces de
  remplacement.
- **Une ligne par instruction**, sauf cas particulier. Le séparateur
  `;` permet d'enchaîner explicitement deux instructions sur la même
  ligne logique (`sTest est une chaîne ; sTest = "Bonjour"`) ; le
  formatter respecte ce choix et ne le décompose pas. À réserver aux
  cas où la concision améliore la lecture, sinon préférer le saut de
  ligne.
- **Espace autour des opérateurs binaires** : `a + b`, pas `a+b`.
- **Pas d'espace entre le nom d'une fonction et sa parenthèse
  d'appel** : `Trace("…")`, pas `Trace ("…")`.
- **Pas d'espace à l'intérieur des parenthèses d'appel** : `f(x)`,
  pas `f( x )`.
- **Un seul `FIN` ferme un bloc à la fois** ; on ne factorise pas
  `SI … FIN FIN` sur une seule ligne.

L'utilisateur qui écrit en désordre voit son fichier remis en forme au
prochain `owl fmt`. Pas la peine de viser la perfection de présentation
à l'écriture : le formatter s'en charge.

---

## 5. Commentaires

Deux formes :

```owl
// Commentaire jusqu'à la fin de la ligne.

/* Commentaire de bloc, peut s'étendre
   sur plusieurs lignes et imbriqué :
   /* commentaire interne valide */
   le bloc continue ici. */
```

Le commentaire de bloc supporte l'imbrication, ce qui est utile pour
commenter temporairement du code qui contient déjà des commentaires de
bloc.

Convention rédactionnelle :

- Pas de commentaire qui paraphrase le code (`// incrémente i`).
- Un commentaire explique le **pourquoi**, pas le **quoi**.
- Pas de commentaires d'historique projet dans le code utilisateur
  (« Phase n », « ajouté pour issue #42 », « lot interne »). L'historique vit
  dans le système de versionnement, pas dans le source.
- Pour les scénarios de test, le commentaire qui précède la procédure
  est la **description publique** : règle décrite dans
  `11_charte_de_test.md`.

---

## 6. Continuation de ligne

Une ligne logique trop longue peut être coupée en plusieurs lignes
physiques avec le marqueur `...` placé en fin de ligne. La ligne
suivante est alors traitée comme la suite directe de la précédente.

```owl
moResultat est un monétaire
moResultat = moMontantHT * (1 + rTauxTVA / 100)...
             - moRemise...
             - moFraisExpedition
```

Le formatter aligne automatiquement les lignes de continuation sous le
premier opérande de la valeur affectée, afin que la suite du calcul reste
lisible. Lorsque l'en-tête n'est pas une affectation simple (déclaration
et initialisation combinées, appel réparti sur plusieurs lignes), les
lignes suivantes reçoivent un retrait d'un niveau plutôt qu'un alignement.

L'opérateur de liaison (`+`, `-`, `*` …) peut être placé indifféremment
**avant** le `...` ou en début de la ligne suivante : aucune des deux
formes n'est imposée. Choisir une convention par fichier pour la
lisibilité.

La continuation n'est ni un raccourci ni une fonctionnalité
d'élégance : elle existe pour les lignes qui deviennent
matériellement trop larges. Pour le reste, préférer une variable
intermédiaire bien nommée à une ligne fragmentée.

---

## 7. Forme globale ou forme méthode

De nombreuses fonctions natives existent sous deux formes équivalentes :

```owl
parts est un tableau de chaînes = ChaineDecoupe(ligne, ",")
parts est un tableau de chaînes = ligne.Decoupe(",")
```

Convention :

- **Forme méthode** quand le sujet est clairement le receveur et que
  l'opération se lit naturellement comme « le receveur fait quelque
  chose ».
- **Forme globale** quand plusieurs arguments sont également importants
  ou quand l'expression de l'appel se chaîne mal.
- **Forme méthode en chaîne** quand plusieurs transformations se
  succèdent sur le même objet : `s.SansEspace().Majuscule().Inverse()`.

---

## 8. Organisation des fichiers

Certaines conventions sont **imposées par le langage** (le runtime
s'en sert pour la découverte et le lancement). D'autres sont des
**suggestions** d'organisation qui aident la lecture mais que rien
n'oblige à suivre.

### 8.1 Imposé par le langage

- **`projet.owl`** à la racine du projet pour le mode exécution. C'est
  le marqueur de racine du projet **et** le point de démarrage : le code
  de son bloc est le programme, exécuté en dernier (après l'init des
  collections). Sans `projet.owl` en remontant, `owl` seul
  (sans fichier nommé) ouvre la REPL au lieu de lancer un projet.
- **Le code de niveau supérieur est le démarrage.** Aucune procédure
  `Main()` n'est appelée automatiquement : `Main` est un nom de procédure
  ordinaire sans statut spécial.
- **Procédure `Fermeture()` (ou `Closing()`)** comme point de sortie
  automatiquement appelé en fin d'exécution du projet, pour libérer les
  ressources (définie dans `projet.owl`).
- **Préfixe `test_`** sur les fichiers exécutés par `owl test`. Un
  fichier sans ce préfixe n'est pas considéré comme un test, même s'il
  contient des assertions.

Ces éléments sont des **contrats** avec le runtime. Y déroger casse la
mécanique d'exécution.

### 8.2 Suggestions d'organisation

- **Découper par sujet, pas par couche.** Un fichier `utilisateurs.owl`
  qui regroupe les structures et procédures liées aux utilisateurs est
  préférable à `structures.owl` + `procedures_utilisateur.owl`.
- **Une classe par fichier** quand la classe est non triviale, nommé
  d'après la classe : `cCompte.owl`. Le compilateur s'en fiche
  totalement ; c'est purement une aide à la navigation.
- **Une fenêtre ou un état par fichier**, nommé d'après le champ qu'il
  définit : `FEN_Login.owl`, `ETAT_Facture.owl`. Là encore, pour la
  lecture humaine.
- **Tests par nature** : scénarios, régression, vérification, formatage.
  Voir `11_charte_de_test.md`.

OWL parcourt récursivement le projet et concatène **tous** les fichiers
qui ne commencent pas par `test_`. La disposition en sous-dossiers est
donc un outil de lecture pour les humains et les agents, pas un
mécanisme de modularisation du langage.

---

## 9. Marqueurs de déclaration

Un **marqueur de déclaration** est une annotation placée après le type
d'une déclaration, entre chevrons (`<...>`), qui ajuste le traitement de
cette déclaration par le compilateur sans changer le type lui-même.

### 9.1 `<utile>` / `<useful>` — variable volontairement inutilisée

Le compilateur émet un avertissement non bloquant quand une variable ou
un paramètre est déclaré mais jamais lu :

```
Variable 'x' est déclarée mais jamais utilisée.
```

C'est un rappel utile pour détecter le code mort, mais certaines
déclarations sont inutilisées **à dessein** : un paramètre imposé par
une signature commune, une variable réservée pour une évolution
proche, un compagnon documentaire. Pour ces cas, le marqueur `<utile>`
(forme anglaise `<useful>`) annote la déclaration comme délibérément
inutilisée et éteint l'avertissement pour elle seule :

```owl
n est un entier <utile>          // aucun avertissement, même jamais lu
m est un entier                  // averti : déclaré mais jamais utilisé
```

Le marqueur se pose aux mêmes endroits qu'une déclaration ordinaire :

- **Variables locales** d'une procédure.
- **Variables globales** d'un fichier ou d'un projet.
- **Paramètres de procédure**, après le type du paramètre :

```owl
PROCEDURE SurClic(sSource est une chaîne <utile>)
    // sSource n'est pas exploité ici, mais la signature l'impose
    Trace("clic")
FIN
```

Une forme à valeur libre permet de consigner la raison directement
dans la déclaration ; la chaîne est purement documentaire :

```owl
nCodeRetour est un entier <utile="réservé pour la future gestion d'erreur">
```

Le marqueur est **insensible à la casse** (`<UTILE>`, `<Useful>` sont
acceptés) et disponible dans les deux langues (`<utile>` en français,
`<useful>` en anglais), comme tout symbole du langage.

Une déclaration **initialisée** (`n est un entier = 3`) n'est jamais
concernée par l'avertissement, marqueur ou non : l'avertissement ne
vise que les déclarations non lues et non initialisées.

### 9.2 Autres marqueurs

Le marqueur `<nullable>` autorise une variable typée à valoir `Null`
distinctement de la valeur par défaut de son type ; il partage cette
sémantique avec le suffixe `?`. Les deux sont décrits dans les fiches
`02_null_et_nullable.md` et `06_poo.md`.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Charte de test | `11_charte_de_test.md` |
| Préfixes de nommage (détail complet) | `12_prefixes_de_nommage.md` |
| Système de types | `01_types_et_coercions.md` |
| Fins de ligne, encodage et entrées/sorties | `26_encodage_et_io.md` |

Pour vérifier qu'un fichier respecte les conventions canon :
`owl fmt --check <fichier>` (diff sans écriture).
