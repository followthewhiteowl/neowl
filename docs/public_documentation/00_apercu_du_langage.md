# OWL — Aperçu du langage

OWL est un langage généraliste à typage explicite, compilé en bytecode et
exécuté par une machine virtuelle. Il est conçu pour être lu, écrit et
maintenu aussi bien par des humains que par des agents IA. La syntaxe est
verbeuse par choix : un programme OWL doit pouvoir se lire comme une
description de ce qu'il fait.

Cet aperçu est volontairement court. Il sert de premier contact. Pour
chaque sujet abordé ici, une fiche dédiée existe.

---

## 1. Premier programme

```owl
Trace("Bonjour, OWL.")
```

Tout fichier `.owl` peut être exécuté directement. Il n'y a ni en-tête de
module à écrire, ni point d'entrée à déclarer pour un script court : le
code écrit au sommet d'un fichier est exécuté de haut en bas.

```bash
owl bonjour.owl
```

Pour expérimenter sans créer de fichier, `owl` lancé seul ouvre une
boucle interactive (REPL).

---

## 2. Variables et types

Une variable est déclarée avec une forme explicite : son nom, le mot
`est`, son type. L'initialisation est facultative à la déclaration ; le
type ne l'est jamais. Il n'y a pas d'inférence : c'est le programmeur
qui dit ce qu'il veut.

> **Noms réservés.** Le langage est insensible à la casse et aux
> accents, donc un mot-clé reste réservé sous toutes ses formes :
> `a`, `A` et `à` valent le même mot-clé. Un nom réservé employé comme
> variable est rejeté à la compilation avec un message explicite. La
> liste complète des mots-clés réservés figure parmi les catalogues du
> langage, à consulter en cas de doute sur un nom.

```owl
nom est une chaîne = "Alice"
age est un entier = 32
taux est un réel = 19.6
montant est un monétaire = 1499.99
```

Les types fondamentaux sont :

| Type | Usage |
|---|---|
| `chaîne` | Texte UTF-8, taille mesurée en octets |
| `entier` | Entier 64 bits ; promotion automatique vers `numérique` en cas de débordement |
| `réel` | Flottant double précision |
| `monétaire` | Décimal exact, 6 chiffres après la virgule |
| `numérique` | Décimal exact à précision illimitée (croît avec la mémoire) |
| `booléen` | `Vrai` ou `Faux` |
| `date` | Date civile sans fuseau |
| `heure` | Heure du jour, précision milliseconde |
| `dateheure` | Combinaison date + heure |
| `durée` | Intervalle de temps |
| `tableau de <T>` | Collection ordonnée, indexée à partir de 1 |
| `JSON` | Document structuré avec accès point |

Les types composés (`httpRequête`, `Cadre`, `Style`, `Chrono`, etc.)
sont décrits chacun dans leur fiche dédiée.

---

## 3. Conversions implicites

Entre types compatibles, OWL convertit sans demander. Une chaîne qui
contient un nombre devient un nombre dans un contexte arithmétique. Un
entier devient une chaîne dans un contexte texte.

L'opérateur `+` est **toujours** une concaténation quand l'un des
opérandes est une chaîne. C'est explicite, pas une promotion :

```owl
total est une chaîne = "42" + 8        // "428"
double est un entier  = 42 + 8         // 50
message est une chaîne = "Score : " + 99   // "Score : 99"
```

Les conversions explicites existent quand le besoin est clair :
`ChaineVersEntier`, `NumériqueVersChaîne`, `Val`, etc.

---

## 4. Contrôle de flux

```owl
SI age >= 18 ALORS
    Trace("Majeur")
SINON
    Trace("Mineur")
FIN

POUR i = 1 A 10
    Trace(i)
FIN

POUR TOUT element DE liste
    Trace(element)
FIN

TANTQUE compteur > 0
    compteur--
FIN

SELON statut
    CAS "actif" : Trace("OK")
    CAS "suspendu", "fermé" : Trace("Inactif")
    AUTRE CAS : Trace("Inconnu")
FIN
```

Le bloc générique `BOUCLE … FIN` existe pour les itérations sans
condition initiale, terminées par `SORTIR`.

---

## 5. Procédures

Une procédure est nommée, retourne ou non une valeur, accepte des
paramètres typés.

```owl
PROCEDURE PrixTTC(ht est un monétaire, taux est un réel) : monétaire
    RENVOYER ht * (1 + taux / 100)
FIN
```

Les procédures sont des valeurs de première classe : on peut les passer
en paramètre, les stocker, les composer.

```owl
PROCEDURE Doubler(n est un entier) : entier
    RENVOYER n * 2
FIN

f est une Procédure = Doubler
Trace(f(7))   // 14
```

Plusieurs valeurs de retour sont possibles. Les variables réceptrices
doivent être déclarées en amont :

```owl
PROCEDURE MinMax(t est un tableau de entiers)
    mini est un entier = t[1]
    maxi est un entier = t[1]
    POUR TOUT v DE t
        SI v < mini ALORS
            mini = v
        FIN
        SI v > maxi ALORS
            maxi = v
        FIN
    FIN
    RENVOYER (mini, maxi)
FIN

bas est un entier = 0
haut est un entier = 0
(bas, haut) = MinMax(scores)
```

### Surcharger une fonction native

Une procédure globale peut porter le même nom qu'une fonction native. Elle la
**masque** alors : un appel non préfixé exécute la procédure, pas la native. Le
préfixe **`OWL.`** (point simple) rappelle la fonction native d'origine, même
lorsqu'une procédure homonyme la masque.

Cela ouvre le motif « enrichir puis déléguer » : ajouter un comportement, puis
passer la main à la fonction d'origine.

```owl
PROCEDURE Taille(x est une chaîne) : entier
    Trace("appel intercepté")
    RENVOYER OWL.Taille(x)   // délègue à la fonction native d'origine
FIN

Trace(Taille("abc"))   // affiche "appel intercepté" puis 3
```

Sans le préfixe, la procédure s'appellerait elle-même indéfiniment. `OWL.` ne
vise qu'une fonction native connue : `OWL.<nom inconnu>` est refusé à la
compilation.

---

## 6. Chaînes

Une chaîne s'écrit sous quatre formes selon le contenu et le besoin
d'interpolation. Toutes les formes sont **raw** par défaut : aucun
échappement de type `\n` ou `\t`. Les deux seules formes qui activent
l'interpolation `[%expression%]` sont les backticks et le préfixe `%`.

**Guillemets doubles** `"…"` : forme historique, raw. Pour insérer un
guillemet littéral, le doubler (`""`). **L'interpolation `[%…%]`
n'est PAS active** — la séquence reste littérale dans la chaîne.

```owl
nom est une chaîne = "Alice"
modele est une chaîne = "Bonjour [%nom%], il fait beau"   // "[%nom%]" reste littéral
citation est une chaîne = "Il a dit ""salut""."
```

**Crochets** `[ … ]` : pour le multi-ligne sans échappement, contenu
brut, pas d'interpolation.

```owl
modele est une chaîne = [
    Ligne 1
    Ligne 2 avec "guillemets" et \ littéraux
]
```

Le bloc se termine sur la première ligne dont le premier caractère non
blanc est un `]`, **et** dont le premier caractère non blanc qui suit ce
`]` est soit une fin de ligne, soit une ponctuation de structure : `,`
`)` `}` `]`. Ce qui vient après cette ponctuation est libre — un bloc
peut donc servir de premier argument à un appel, ou de valeur dans un
littéral.

```owl
message est une chaîne = ChaîneConstruit([
Bonjour %1,
Vous avez %2 messages.
], "Alice", 3)
```

Toute autre ligne commençant par `]` reste du contenu, y compris
`] suivi de texte`. Un modèle qui contient du JSON ou des crochets
fermants en début de ligne est ainsi préservé.

```owl
extrait est une chaîne = [
] cette ligne fait partie du texte
]
```

**Backticks** `` ` … ` `` : raw + **interpolation `[%var%]` active
automatiquement**, multi-ligne supportée nativement, et — contrairement
à `[ … ]` — un `]` interne ne ferme rien. Idéal pour du JSON, du SQL,
du HTML, ou pour interpoler dans un message lisible.

```owl
config est une chaîne = `{"nom":"Alice","tags":["admin","dev"]}`
salut est une chaîne = `Bonjour [%nom%], bienvenue.`
```

**Fins de ligne des formes multilignes.** Dans un bloc `[ … ]` comme
entre backticks, un saut de ligne vaut toujours **`LF`**, quel que soit
l'enregistrement du fichier source (à la manière Unix ou à la manière
Windows). Ce qui est lu à l'écran est ce que le programme voit. Une
paire Windows dans une chaîne s'écrit donc explicitement, avec `CRLF` ;
pour l'octet 13 seul, `Caract(13)`.

```owl
extrait est une chaîne = `ligne 1
ligne 2`
Trace(extrait = "ligne 1" + LF + "ligne 2")     // 1 (Vrai)
windows est une chaîne = "ligne 1" + CRLF + "ligne 2"
Trace(Taille(windows))                          // 16
```

La règle ne concerne que le texte du programme : les données ne sont
jamais retouchées, un texte saisi, un fichier lu ou une réponse réseau
gardent leurs octets exacts. Détail dans `26_encodage_et_io.md`,
section « Fins de ligne ».

**Préfixe `%`** sur guillemets doubles, `%"…"` : strictement
équivalent à `"…"` côté caractères acceptés, **mais active
l'interpolation `[%expression%]`**. À utiliser quand on veut une
chaîne courte et interpolée sans passer aux backticks.

```owl
salut est une chaîne = %"Bonjour [%nom%], bienvenue."
```

L'interpolation accepte un identifiant ou un accès membre
(`[%personne.nom%]`), pas un appel de fonction. Pour insérer le
résultat d'un appel, le stocker dans une variable au préalable.

> **Règle simple.** Pour interpoler, utiliser **backticks** ou le
> **préfixe `%"…"`**. Les guillemets nus `"…"` ne font jamais
> d'interpolation : c'est un choix de conception explicite (cf.
> `docs/VISION.md`, principe AI-first — aucun comportement de
> chaîne dépendant d'une configuration de projet). C'est une
> divergence assumée par rapport à la convention historique des
> langages métier dont OWL s'inspire.

L'indexation et le tranchage utilisent la double crochet, base 1, avec
le séparateur `À` (forme canon, accent-insensible donc `A` ou `À` au
runtime, `SUR` pour spécifier une longueur) :

```owl
s est une chaîne = "abcdefgh"
Trace(s[[3]])         // "c"     — caractère à l'index 3
Trace(s[[2 À 4]])     // "bcd"   — sous-chaîne du 2e au 4e inclus
Trace(s[[5 À]])       // "efgh"  — depuis l'index 5 jusqu'à la fin
Trace(s[[À 4]])       // "abcd"  — depuis le début jusqu'à l'index 4
Trace(s[[2 SUR 3]])   // "bcd"   — 3 caractères depuis l'index 2
```

Les méthodes de chaîne se chaînent naturellement :

```owl
propre est une chaîne = "  Bonjour  ".SansEspace().Majuscule()
```

---

## 7. Tableaux et JSON

Les tableaux sont indexés à partir de 1. La taille est dynamique sauf
contrainte explicite. Les méthodes de manipulation se chaînent.

```owl
panier est un tableau de chaînes
panier.Ajoute("pomme").Ajoute("poire").Ajoute("banane")

POUR TOUT fruit DE panier
    Trace(fruit)
FIN
```

Le type `JSON` permet la manipulation de documents structurés sans
modèle préalable. Une chaîne JSON se parse explicitement :

```owl
doc est un JSON = ChaineVersJSON("{""utilisateur"": {""nom"": ""Alice"", ""age"": 32}}")
Trace(doc.utilisateur.nom)   // "Alice"
doc.utilisateur.age = 33
```

---

## 8. Exceptions

Le bloc `QUAND EXCEPTION` capture les erreurs levées dans son corps. Le
bloc optionnel `SINON` est exécuté uniquement quand aucune erreur ne
s'est produite.

```owl
QUAND EXCEPTION DANS
    ExceptionDéclenche(7, "ressource indisponible")
FAIRE
    Trace("Échec : " + ExceptionInfo(errMessage))
SINON
    Trace("Tout s'est bien passé")
FIN
```

Les exceptions portent un contexte structuré : message, ligne, procédure
appelante, pile. La fonction `ExceptionInfo` y accède champ par champ.

Toutes les erreurs ne sont pas des exceptions. Certaines natives (lecture
de fichier, parsing JSON, requêtes SQL…) signalent l'échec en retournant
une valeur neutre (chaîne vide, Null…) et en positionnant la variable
globale `ErreurDétectée`. C'est une *soft-error* — non bloquante, à
tester explicitement avec `Si ErreurDétectée Alors …`. Le détail est
fourni par `ErreurInfo()`. Voir le chapitre dédié pour la distinction
soft-error vs exception.

---

## 9. Classes, structures, énumérations

OWL supporte la programmation orientée objet et les types nommés. Les
membres et les méthodes d'une classe sont déclarés **dans le corps** de
la classe, à l'intérieur d'un bloc de visibilité (`PUBLIC` ou `PRIVE`).
Le constructeur est une procédure nommée `Constructeur`. Le préfixe `:`
désigne le membre de l'instance courante.

```owl
Personne est une Structure
    nom est une chaîne
    age est un entier
FIN

Statut est une Enumération
    actif
    suspendu
    fermé
FIN

cCompte est une Classe
PUBLIC
    proprietaire est une chaîne
    solde est un monétaire

    PROCEDURE Constructeur(nom est une chaîne)
        :proprietaire = nom
        :solde = 0
    FIN

    PROCEDURE Crediter(montant est un monétaire)
        :solde += montant
    FIN
FIN
```

Une instance se crée en appelant le type comme un constructeur :

```owl
oCompte est un cCompte("Alice")
oCompte.Crediter(150.00)
```

Les classes supportent l'héritage simple et l'implémentation
d'interfaces. Le détail figure dans la fiche dédiée à l'objet.

### Accès aux membres : `.` et `..`

OWL distingue deux opérateurs d'accès, et la distinction est vérifiée à
la compilation :

- **un point** `.` appelle une **méthode** ou lit un **membre** (champ
  d'un objet, d'une structure, ou d'un document JSON) :
  `oCompte.Crediter(...)`, `personne.nom`, `doc.utilisateur`.
- **deux points** `..` lisent ou écrivent une **propriété** —
  caractéristique prédéfinie d'un objet ou d'un conteneur :
  `tableau..Occurrence`, `SAI_Nom..Valeur`.

Utiliser le mauvais opérateur produit une erreur de compilation explicite.

---

## 10. Bilinguisme et tolérance d'écriture

Tout symbole du langage existe sous deux formes équivalentes : française
canonique et anglaise canonique. Les deux peuvent se mélanger dans un
même fichier, ligne par ligne.

```owl
SI x > 0 THEN
    Trace("positif")
END
```

Au-delà du bilinguisme, le scanner est insensible à la casse et aux
accents pour les identifiants : `chaine`, `Chaîne` et `CHAINE` désignent
le même type. Cette tolérance n'est pas une invitation au laxisme : à
la sauvegarde, le formatter restaure la forme canonique exacte. La
tolérance sert à l'écriture rapide ; le canon sert à la lecture et au
diff.

---

## 11. Organisation d'un projet

OWL distingue **deux niveaux** d'organisation, déterminés par remontée
des dossiers parents depuis le fichier ciblé :

1. **Script** — aucun marqueur. Le contexte se limite au **dossier
   courant** : les fichiers voisins du même répertoire se voient
   mutuellement, les sous-dossiers sont ignorés. Liberté maximale, esprit
   « langage de script ». Le fichier à exécuter doit être désigné
   explicitement : `owl mon_fichier.owl` exécute son code de niveau
   supérieur et charge ses voisins de dossier. Il n'existe **aucune entrée
   par défaut** : `owl` seul (ou `owl <dossier>`) sans `projet.owl` produit
   une erreur — il faut nommer un fichier, ou créer un `projet.owl`.
2. **Projet** — un fichier `projet.owl` marque la **racine** du projet et
   **étend le périmètre à tous les sous-dossiers** : le projet regroupe tous
   les fichiers situés dans le dossier de `projet.owl` et dans les
   sous-dossiers. Il
   désigne aussi le **point de démarrage** : `owl`, `owl <dossier>` ou
   `owl projet.owl` lancent le projet. Le fichier `projet.owl` est
   **réservé** (nom protégé, casse-insensible) : il ne s'exécute jamais
   comme un script isolé. Sa forme (marqueur de frontière et métadonnées :
   langues, description, auteur, licence) et son rôle de programme sont
   décrits dans la fiche `24_projet.md`.

Pour tout fichier, le premier `projet.owl` rencontré en remontant définit
la racine. Sans `projet.owl` ancêtre, le périmètre retombe sur le mode
script local.

Les outils `owl run`, `owl --check`, `owl fmt` et `owl test` résolvent
le périmètre de chaque fichier passé en argument via cette règle de
frontière ; ils n'analysent jamais le fichier de manière isolée.

**Ordre d'exécution d'un projet.** OWL procède en étapes déterministes :

1. **Recensement** — OWL relève ce que déclare chaque fichier du projet :
   fenêtres, états, classes, collections, thèmes, et le projet lui-même. Les
   fichiers qui ne déclarent aucun de ces éléments (de simples scripts) sont
   laissés de côté — voir « Fichiers de script » plus bas. Recenser n'est pas
   exécuter : à ce stade, rien ne tourne encore.
2. **Métadonnées et langue source** — les valeurs du bloc `est un Projet`
   sont établies très tôt, avant tout code exécutable, et restent lisibles
   en cascade (`MonApp..Version`) depuis n'importe quel fichier.
3. **Collections** — le code au sommet de chaque collection s'exécute
   **avant** le programme principal : ce sont les services dont dépend
   l'application (données partagées, procédures utilitaires). Une fenêtre ou
   un état, lui, ne s'initialise qu'à son ouverture, pas au démarrage.
4. **`projet.owl`** — son code s'exécute **en dernier**. Le code au sommet de
   `projet.owl` **est le programme principal** : il démarre une fois les
   services en place.
5. **`Fermeture()` / `Closing()`** — à la fermeture du projet, si `projet.owl`
   définit cette procédure, elle est appelée pour libérer les ressources.

Il n'y a **pas de procédure `Main()` appelée automatiquement** : le code
de niveau supérieur est le démarrage.

**Fichiers de script.** Dans un projet, un fichier fait partie du projet dès
lors qu'il déclare une fenêtre, un état, une classe, une collection, un thème
ou le projet. Un fichier qui contient seulement du code libre (un fichier sans
type particulier : ni fenêtre, ni état, ni classe, ni collection) — des
variables, des procédures ou des instructions écrites directement, sans les
regrouper dans l'un de ces éléments — est un **script** : il reste privé,
invisible au reste du
projet, et ne s'exécute que si on le lance soi-même (`owl mon_script.owl`). Le
code destiné à être partagé (procédures, constantes, types, données communes) se
met donc dans une **collection**, faite pour ça. Un script qu'on lance voit le
reste du projet — les collections (déjà initialisées) et les types des fenêtres
et états — mais pas les autres scripts voisins. Hors projet, dans un simple
dossier sans `projet.owl`, cette règle ne s'applique pas : tous les fichiers du
dossier se voient et leur code s'exécute.

`owl test` parcourt récursivement l'arborescence ciblée pour **trouver**
tous les fichiers `test_*.owl`, et exécute **chaque test dans le contexte
de sa propre frontière** (pas un méga-projet global) : un test situé sous
un `projet.owl` voit l'arbre du projet ; un test isolé voit seulement son
dossier. La répartition des tests par nature reste libre.

Note importante : l'éditeur (VSCode) ouvre un **workspace** qui peut
contenir plusieurs projets et scripts indépendants. Les outils résolvent
le projet de **chaque fichier** par la règle de frontière ci-dessus, jamais
le workspace comme un projet unique. Pour qu'un dossier multi-fichiers
fonctionne avec ses sous-dossiers, ajouter un `projet.owl` (avec son bloc
`est un Projet`) à sa racine suffit.

**Un fichier qui déclare une classe ne contient que cette classe et des
commentaires** — pas de code de niveau supérieur, pas d'autre classe.
Le code exécutable va dans `projet.owl` ou dans une Collection.
Toute violation produit une erreur de compilation. Voir `06_poo.md` § 1.

```owl
// projet.owl — le corps du bloc est le programme
MonApp est un Projet
    PROPRIETES
        Langues = [langueFrançais]
    FIN

    configActive est une chaîne = "production"
    Trace("Démarrage en mode " + configActive)

    PROCEDURE Fermeture()
        Trace("Au revoir")
    FIN
FIN
```

`projet.owl` déclare toujours un bloc `est un Projet` ; c'est le corps de ce
bloc qui constitue le programme (point de démarrage). Rien n'est accepté hors
du bloc. Une fenêtre est déclarée ailleurs dans le projet ; le pattern
recommandé est que `projet.owl` *lance* les fenêtres via `Ouvre(...)` plutôt
que d'être lui-même une fenêtre — ce qui garde le démarrage distinct des vues.

**Portée : elle dépend de l'élément qui entoure la déclaration.** La règle se
lit au niveau du bloc qui contient la déclaration :

- Dans `projet.owl` et dans une **collection**, ce qui est déclaré au sommet
  (variable, constante, type, procédure) est **visible partout dans le projet**.
- Dans une **fenêtre** ou un **état**, les variables déclarées au sommet sont
  **privées** : elles n'existent que pour ce fichier. Seuls les **types** qu'on
  y déclare (structure, énumération) restent accessibles de l'extérieur, en les
  préfixant du nom de la fenêtre : `FEN_Login.EStatut`, ou
  `FEN_Login.EStatut.actif` pour une valeur. La vue garde ainsi son état pour
  elle, tout en partageant ses types.
- Un **fichier de script** garde tout pour lui : rien de ce qu'il déclare n'est
  visible depuis le projet (voir « Fichiers de script » ci-dessus).

Dans un fichier-fenêtre (`FEN_X.owl`), les procédures de rendu ou de logique
vivent **à l'intérieur du bloc `est une Fenêtre`**, pas après son `FIN`. Le
fichier-fenêtre sert à déclarer la fenêtre et ses procédures liées ; écrire une
procédure au sommet du fichier, en dehors du bloc, pour accéder aux champs
déclarés dans la fenêtre, est refusé à la compilation. Le câblage complet d'une
application graphique répartie sur plusieurs fichiers (état dans `projet.owl`,
procédures dans le bloc de la fenêtre, logique partagée dans une classe) est
détaillé dans `15_lifecycle_champs.md` §9.

Dans un répertoire sans `projet.owl`, lancer `owl` sans nommer de fichier
ouvre la REPL.

---

## 12. Découvrir le langage

Le binaire `owl` est sa propre documentation.

```bash
owl primer                      # ce document
owl learn <sujet>               # une fiche fondamentale
owl explain <symbole>           # détail d'un type, d'une fonction, d'une constante
owl examples <symbole>          # scénarios concrets associés
owl search <mot-cle>            # recherche transversale
owl spec                        # grammaire complète
```

Aucune connexion réseau n'est requise. Aucun fichier externe à
télécharger. La doc accompagne le binaire.

---

## 13. Tests intégrés

OWL traite le test comme un citoyen de première classe. Tout fichier
nommé `test_*.owl` est exécuté par `owl test`. Le format canon des
scénarios, les fixtures et la convention de nommage sont détaillés dans
la fiche `11_charte_de_test.md`.

```owl
PROCEDURE scenario_addition_basique()
    dbgVerifieEgalite(2 + 2, 4, "addition entière")
FIN
```

Les scénarios sont à la fois des tests exécutables et des exemples de
référence. C'est par eux que la documentation des fonctions natives est
illustrée.

---

## 14. Pensé pour les agents IA

OWL est explicitement conçu pour qu'un agent IA puisse lire, écrire et
vérifier du code sans avoir à deviner. La conception combine des
contraintes strictes (qui éliminent l'ambiguïté) et des tolérances
ciblées (qui rendent l'écriture rapide).

**Strict — ce qui est interdit :**

- Pas d'inférence de type. Toute variable est déclarée avec son type.
- Pas de type variant ni d'objets dynamiques au sens où une valeur
  changerait de structure à l'exécution.
- Pas d'état caché : pas de méta-programmation invisible, pas d'effet
  d'import, pas de macro qui réécrit le code.
- Diagnostics déterministes : `owl --check` produit des erreurs JSON
  structurées, identiques d'une exécution à l'autre.

**Souple — ce qui est facilité :**

- Une seule façon canonique d'écrire chaque concept, restaurée par le
  formatter à la sauvegarde.
- Bilinguisme FR/EN, tolérance d'accents et de casse pour l'écriture.
- Coercions automatiques entre types numériques et entre numérique et
  texte dans les contextes sans ambiguïté.
- Introspection native disponible à l'exécution : `TypeVar(x)`,
  `VariableExiste(x)`, `ExceptionInfo`, etc.
- Découverte du langage via le binaire, sans dépendance externe.

Un agent qui rencontre OWL pour la première fois peut, en lisant ce
document et en utilisant `owl explain` et `owl examples`, écrire son
premier programme correct sans recourir à des sources tierces.

Le mode d'emploi concret — brancher une IA sur un projet via le serveur
MCP intégré — est décrit dans `25_agents_ia.md`.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Conventions d'écriture et formatter | `10_conventions_de_code.md` |
| Charte de test et scénarios | `11_charte_de_test.md` |
| Système de types détaillé | `01_types_et_coercions.md` |
| Tableaux associatifs (clé chaîne → valeur) | `30_tableaux_associatifs.md` |
| Valeur nulle et types nullables | `02_null_et_nullable.md` |
| Opérateurs et précédence | `05_operateurs.md` |
| Brancher une IA sur un projet | `25_agents_ia.md` |
| Déboguer un programme (backend DAP `owl-dap`) | `28_debogueur.md` |

Pour la liste complète des fonctions natives, types composés et
constantes : `owl categories`, puis `owl explain <nom>`.
