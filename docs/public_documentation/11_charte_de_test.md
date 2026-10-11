# Tester du code OWL

Le langage fournit un cadre intégré pour écrire et exécuter des tests
unitaires : structure de fichier conventionnée, fixtures, assertions,
mock des fonctions à effet de bord, et un **mode headless** qui
neutralise automatiquement les popups d'interface graphique sous
`owl test`.

Deux principes :

1. **Un test est aussi une documentation.** Un scénario bien nommé,
   avec une phrase de description et des assertions claires, sert de
   référence à un lecteur qui découvre la fonction.
2. **Le code et l'assertion sont la vérité, pas le commentaire.** Un
   commentaire qui se désynchronise est gênant ; une assertion qui se
   désynchronise casse le test.

---

## 1. Un fichier par symbole

Un fichier de test documente **un seul symbole** : une fonction, un
type, une classe, une énumération non triviale. Le nom du fichier
reprend le symbole : `test_chaine_decoupe.owl`, `test_cFichier.owl`,
`test_http_requete.owl`.

Quand une fonction et une méthode préfixée portent la même sémantique
(par exemple une forme globale `ChaineVersDate(s)` et une forme
méthode `s.VersDate()`), elles partagent **le même fichier**. Les
scénarios internes choisissent l'une ou l'autre forme selon ce
qu'ils illustrent.

Le runner `owl test` parcourt récursivement l'arborescence et exécute
**tout** fichier nommé `test_*.owl` qu'il y trouve. La hiérarchie de
sous-dossiers est libre.

---

## 2. En-tête du fichier

Un commentaire d'en-tête, optionnel mais recommandé, ouvre le fichier
et résume en une phrase le symbole testé. Il sert de repère pour le
lecteur qui ouvre le fichier sans contexte.

```owl
// Tests de ChaineDecoupe : découpe selon un séparateur, retour tableau.
```

---

## 3. Scénarios

Chaque scénario est une procédure dont le nom commence par l'un des
trois préfixes acceptés : `scenario_`, `test_`, `should_`. Les trois
sont équivalents pour le runner ; choisir l'un ou l'autre est une
question de style, pas de sens.

```owl
// Découpe une ligne CSV simple sur la virgule.
PROCEDURE scenario_csv_simple()
    parts est un tableau de chaînes = ChaineDecoupe("a,b,c", ",")
    dbgVérifieEgalité(parts..Occurrence, 3)
    dbgVérifieEgalité(parts[1], "a")
    dbgVérifieEgalité(parts[3], "c")
FIN
```

Le commentaire qui précède la procédure est la **description** du
scénario. Une seule phrase. Elle apparaîtra dans la documentation
générée.

Un scénario doit :

- Tester **un seul** comportement nommé. Si le scénario fait deux
  choses, c'est qu'il en faut deux.
- Contenir au moins une assertion (`dbgVerifie*`). Sans assertion,
  c'est un exemple, pas un test.
- Être lisible isolément. Un lecteur qui n'a vu que la procédure doit
  comprendre ce qui est testé.

---

## 4. Fixtures

Quatre noms de procédure sont réservés pour les fixtures. Leur
présence est facultative ; si présentes, le runner les exécute aux
moments attendus.

| Forme française | Forme anglaise | Exécution |
|---|---|---|
| `avantToutTest` | `beforeAllTests` | Une fois avant l'ensemble des scénarios du fichier |
| `apresToutTest` | `afterAllTests` | Une fois après l'ensemble |
| `avantChaqueTest` | `beforeEachTest` | Avant chaque scénario |
| `apresChaqueTest` | `afterEachTest` | Après chaque scénario |

Les deux formes sont équivalentes. Mélanger FR et EN dans le même
fichier est toléré mais déconseillé.

```owl
// Tests de ChaineMajuscule : conversion en majuscules.

s est une chaîne

PROCEDURE avantChaqueTest()
    s = ""
FIN

PROCEDURE scenario_ascii_simple()
    s = "bonjour"
    dbgVérifieEgalité(s.Majuscule(), "BONJOUR")
FIN

PROCEDURE scenario_chaine_vide()
    dbgVérifieEgalité(s.Majuscule(), "")
FIN
```

---

## 5. Scénarios publics et privés

Par défaut, un scénario est **public** : il est exposé par les
commandes de découverte (`owl examples`, `owl explain`) comme exemple
de référence.

Un scénario qui ne mérite pas d'être un exemple (test d'un détail
défensif ou structurel) est déclaré avec le modificateur `PRIVE` :

```owl
// Préserve les caractères non-ASCII quand le séparateur est non-ASCII.
PROCEDURE PRIVE scenario_separateur_unicode()
    parts est un tableau de chaînes = ChaineDecoupe("a→b→c", "→")
    dbgVérifieEgalité(parts..Occurrence, 3)
FIN
```

Le runner exécute les scénarios `PRIVE` exactement comme les autres.
Seul l'extracteur de documentation les ignore.

---

## 6. Assertions

Les fonctions d'assertion sont fournies par le module de débogage.
Forme canonique avec accents (la forme ASCII sans accents est
acceptée à l'écriture, le formatter restaure le canon) :

| Fonction | Vérifie |
|---|---|
| `dbgVérifieVrai(c, message?)` | `c` est vrai |
| `dbgVérifieFaux(c, message?)` | `c` est faux |
| `dbgVérifieNull(v, message?)` | `v` est nul |
| `dbgVérifieNonNull(v, message?)` | `v` n'est pas nul |
| `dbgVérifieEgalité(a, b, message?)` | Égalité stricte |
| `dbgVérifieÉgalitéRéel(a, b, epsilon, message?)` | Égalité de réels à epsilon près |
| `dbgVérifieDifférence(a, b, message?)` | `a` et `b` sont différents |

`dbgBilanTest()` affiche le bilan global des assertions du fichier.

Le message final est facultatif mais recommandé : il apparaît dans le
rapport en cas d'échec et aide à localiser le problème sans relire le
fichier.

---

## 7. Tester les fonctions à effet de bord

Certaines fonctions natives ont un effet observable sur le monde
extérieur — appel réseau, lecture de fichier, lecture de l'horloge
système. Tester ces fonctions sans précaution rend les scénarios :

- **lents** (un appel HTTP réel coûte des centaines de millisecondes),
- **fragiles** (le test passe ou échoue selon la connectivité,
  l'heure, la machine),
- **non reproductibles** (la cible distante peut changer de réponse).

Le langage fournit un système de mock universel qui intercepte
n'importe quelle native. Règle simple : **toute fonction à effet de
bord doit être mockée dans un test**.

```owl
// Tests de HTTPRequête : exécution d'une requête HTTP et lecture de la réponse.

PROCEDURE scenario_get_simple()
    mockHTTP est un MockNative = MockCrée(HTTPRequête)
    MockQuand(mockHTTP)
    MockAvec(mockHTTP, MockTout)
    MockRetourne(mockHTTP, "réponse simulée")
    MockActive(mockHTTP)

    reponse est une chaîne = HTTPRequête("http://exemple.invalide/")
    dbgVérifieEgalité(reponse, "réponse simulée")

    MockDésactive(mockHTTP)
    MockVérifieAppelé(mockHTTP)
FIN
```

Le système Mock couvre les séquences de réponses, la vérification
d'arguments précis, le comptage du nombre d'appels et la négation.
Voir `owl explain MockCrée` pour la signature complète.

---

## 8. Mode headless : tester sans interface graphique

Quand un test exerce du code qui ouvre une fenêtre, affiche un
message ou attend un clic utilisateur, faire apparaître la véritable
interface graphique à chaque exécution casse l'automatisation.

Le langage propose un **mode headless** : un commutateur qui
neutralise les fonctions d'interface graphique. Les popups
(`Info`, `Erreur`, `Avertissement`) ne s'affichent pas ;
les ouvertures de fenêtre (`Ouvre`, `OuvreFille`) exécutent le code
d'initialisation puis se referment sans event loop ; les dialogues
de confirmation (`Confirmer`, `OuiNon`) retournent leur valeur par
défaut.

### Activation automatique sous `owl test`

Sous `owl test`, le mode headless est **activé par défaut** au
démarrage de chaque fichier de test. L'auteur d'un test n'a rien à
faire : `Info(...)` ou `Avertissement(...)` dans le code testé ne bloquent
pas le déroulement du scénario.

### Contrôle explicite

La native `TestModeHeadless` permet de lire et de modifier l'état
courant :

```owl
TestModeHeadless()           // retourne l'état courant (booléen)
TestModeHeadless(Vrai)       // active le mode headless
TestModeHeadless(Faux)       // désactive — l'interface réelle s'affiche
```

État par défaut :

| Contexte | `TestModeHeadless()` au démarrage |
|---|---|
| `owl test` | `Vrai` (réinitialisé à chaque fichier) |
| `owl` (mode application) | `Faux` (interface normale) |

### Cas d'usage typiques

**Vérifier qu'une fonction d'interface a été appelée** — combiner
mode headless (déjà actif sous `owl test`) et `MockNative` :

```owl
// Tests de Info : affichage d'une boîte d'information modale.

PROCEDURE scenario_info_appelee_deux_fois()
    mockInfo est un MockNative = MockCrée(Info)
    MockActive(mockInfo)

    Info("premier message")
    Info("second message")

    MockVérifieAppelé(mockInfo, 2)
    MockDésactive(mockInfo)
FIN
```

**Exécuter ponctuellement la vraie interface** — utile pour valider
visuellement un comportement complexe en cours de développement :

```owl
PROCEDURE scenario_dialog_reel()
    TestModeHeadless(Faux)        // active l'interface pour ce scénario
    Info("ce popup s'affiche réellement")
    TestModeHeadless(Vrai)        // restaure le mode headless
FIN
```

### Limites

- Le mode headless **n'enregistre pas** l'historique des appels. Pour
  vérifier qu'une fonction a été appelée avec tel argument, utiliser
  `MockNative`.
- Le mode headless ne s'applique qu'aux natives d'interface. Le code
  applicatif (calculs, accès aux données, logique métier) s'exécute
  normalement.

---

## 9. Simuler les actions utilisateur

Le mode headless neutralise les popups, mais il faut aussi pouvoir
**déclencher** les comportements qu'un utilisateur réel aurait
provoqués : un clic sur un bouton, une saisie dans un champ, etc.
Sans cela, les blocs `PROCEDURE Clic()`, `PROCEDURE Modification()`,
`PROCEDURE Validation()` resteraient orphelins, jamais couverts par
les tests.

Le langage fournit la famille `TestSimule*` pour cela. Chaque native
prend le champ ciblé en premier argument et déclenche le callback
correspondant directement sur l'objet, sans passer par la couche
graphique. Le résultat est **déterministe** : qu'on soit en mode
headless ou en mode interface réelle, le callback s'exécute de la
même façon.

### `TestSimuleSouris`

Déclenche le callback `Clic` ou `DoubleClic` d'un champ.

```owl
TestSimuleSouris(<champ>, <action>)
```

`<action>` est une valeur de l'énumération `ActionSouris` :

| Valeur | Callback déclenché |
|---|---|
| `ClicGauche` | `Clic` |
| `ClicDroit` | `Clic` |
| `DoubleClicGauche` | `DoubleClic` |
| `DoubleClicDroit` | `DoubleClic` |

Si le champ ne définit pas le callback attendu, l'appel est un no-op
silencieux : pas d'erreur, pas d'effet.

```owl
// Tests de Bouton.Clic : déclenchement du callback Clic d'un bouton.

drapeau est un booléen = Faux

FEN_Demo est une Fenêtre
    PROPRIETES
        Disposition = dispLibre
    FIN
    BTN_OK est un Champ Bouton
        PROCEDURE Clic()
            drapeau = Vrai
        FIN
    FIN
FIN

PROCEDURE scenario_clic_simule()
    dbgVérifieFaux(drapeau, "drapeau Faux au départ")
    TestSimuleSouris(BTN_OK, ClicGauche)
    dbgVérifieVrai(drapeau, "callback Clic déclenché")
FIN
```

### `TestSimuleSaisie`

Écrit un texte dans un champ de saisie **et** déclenche son callback
`Modification`.

```owl
TestSimuleSaisie(<champ>, <texte>)
```

```owl
PROCEDURE scenario_saisie_simulee()
    valeurModifiée est une chaîne = ""

    FEN_Demo est une Fenêtre
        PROPRIETES
            Disposition = dispLibre
        FIN
        SAI_Nom est un Champ Saisie
            PROCEDURE Modification()
                valeurModifiée = SAI_Nom..Valeur
            FIN
        FIN
    FIN

    TestSimuleSaisie(SAI_Nom, "Marc")
    dbgVérifieEgalité(SAI_Nom..Valeur, "Marc")
    dbgVérifieEgalité(valeurModifiée, "Marc")
FIN
```

### `TestSimuleMenu`

Déclenche la sélection d'une option d'un menu principal, en désignant
l'option par son nom. Le callback `surSélection` de l'option ciblée
s'exécute comme si l'utilisateur l'avait choisie dans le menu.

```owl
TestSimuleMenu(<fenêtre>, <nom de l'option>)
```

Le premier argument est la fenêtre dont on pilote le menu principal, le
second le nom de l'`OptionMenu` à déclencher. La recherche parcourt tout
l'arbre du menu, y compris les sous-menus imbriqués : le nom seul suffit,
sans qualifier le chemin.

```owl
PROCEDURE scenario_menu_simule()
    Ouvre(FEN_Demo)
    TestSimuleMenu(FEN_Demo, "OPT_Nouveau")
    dbgVérifieEgalité(compteurNouveau, 1, "surSélection déclenché une fois")
    Ferme(FEN_Demo)
FIN
```

Contrairement à la souris et à la saisie, une cible introuvable est une
**erreur explicite**, pas un no-op : un nom d'option absent de l'arbre,
ou une fenêtre sans menu principal affecté, positionnent
`ErreurDétectée`.

### Différence avec `MockNative`

`MockNative` intercepte un appel pour le **remplacer** ou pour
**vérifier** qu'il a eu lieu. `TestSimule*` **déclenche** un
comportement applicatif comme l'aurait fait un utilisateur.

Les deux sont complémentaires :

| Besoin | Outil |
|---|---|
| Vérifier qu'`Info` a été appelée 2 fois | `MockNative` |
| Forcer `Confirmer` à retourner `Oui` | `MockNative` |
| Déclencher le callback `Clic` d'un bouton | `TestSimuleSouris` |
| Saisir du texte dans un champ + déclencher `Modification` | `TestSimuleSaisie` |
| Déclencher la sélection d'une option de menu | `TestSimuleMenu` |
| Vérifier l'état d'un contrôle après simulation | accès direct aux propriétés (`BTN_OK..Visible`, `SAI_Nom..Valeur`) |

### Limites actuelles

- La famille couvre la souris, la saisie et les options de menu. Les
  raccourcis clavier globaux et les gestes ne sont pas simulés.
- L'argument de position `(X, Y)` n'est pas exposé : le callback est
  déclenché sans coordonnées spécifiques.
- Modificateurs (`Shift`, `Ctrl`, `Alt`) non exposés.
- `TestSimule*` ne simule **pas** un événement matériel au niveau
  du système graphique : si un test doit vérifier la chaîne d'événements
  complète jusqu'au pilote d'affichage, ce n'est pas le bon outil.
  Pour la couverture des callbacks applicatifs, c'est exactement ce
  qu'il faut.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Conventions de code | `10_conventions_de_code.md` |

Pour la liste des scénarios publics d'un symbole donné :
`owl examples <symbole>`.
