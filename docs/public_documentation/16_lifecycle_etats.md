# Cycle de vie d'un État

Un État produit un **rapport mis en page** — typiquement un PDF.
Contrairement à une Fenêtre, il n'a pas de boucle d'événements
utilisateur : son code s'exécute selon une séquence fixe pilotée par
`iImprimeEtat`, ponctuée de hooks et de sous-blocs où le dev greffe
sa logique.

---

## 1. Anatomie d'une déclaration

```owl
ETAT_Facture est un État
    PROPRIETES
        Format      = formatA4
        Orientation = orientPortrait
        MargeHaut   = 20
        MargeBas    = 20
        MargeGauche = 20
        MargeDroite = 20
    FIN

    intro est un EnTêteDocument
        PROPRIETES
            Hauteur = 40
        FIN
        Trace("entete du document")
    FIN

    ligne est un Corps
        Trace("rendu d'une ligne")
    FIN

    PROCEDURE Lecture()
        // logique métier (boucle d'impression, calculs dérivés, etc.)
    FIN
FIN
```

Le préfixe canon est `ETAT_` (avec le T). Un fichier peut contenir
**plusieurs États**, mais **pas mélanger État et Fenêtre** dans le même
fichier (erreur compile).

### Constructions acceptées dans le body

| Construction | Effet |
|---|---|
| `PROPRIETES ... FIN` | Propriétés racine de l'État (format, marges, ...) |
| `nom est un Type [= expr]` | Variable portée par l'État (init à la 1re impression) |
| `<NOM> est un <TypeBloc>` | Déclaration d'un sous-bloc (cf. §2) |
| `PROCEDURE Filtre/Lecture/Fermeture` | Hook lifecycle (cf. §5) |
| `PROCEDURE` / `FONCTION` quelconque | Helper utilisateur |
| Tout autre statement (`Trace`, appel, `SI`, `POUR`, ...) | Compilé dans l'**event implicite `Ouverture`**, exécuté **une fois**, à la 1re impression |

C'est une différence majeure avec le body de Fenêtre : ici les
instructions au top-level **sont acceptées**, et regroupées dans un
event `Ouverture` implicite.

### Confiner une variable au flux d'init : `LOCAL`

Par défaut, une variable top-level est **portée par l'État** (visible de
ses sous-blocs et procédures par leur nom nu). Pour un temporaire dont
seul le code d'initialisation a besoin — un index de boucle, un
accumulateur — la bascule `LOCAL` la **confine au flux top-level** : elle
n'est alors ni portée par l'État, ni visible des procédures, ni accessible
depuis l'extérieur.

`GLOBAL` et `LOCAL` se posent **seuls sur leur ligne** et basculent le mode
des déclarations qui suivent, jusqu'à la bascule inverse — il n'y a pas de
bloc à refermer. `GLOBAL` rétablit le défaut (variable portée par l'État).
Les deux s'alternent librement.

```owl
ETAT_Ventes est un État
    nbLignes est un entier = 0        // défaut : portée par l'État

    LOCAL
    i est un entier = 0               // confiné au flux d'init
    TANTQUE i < 10
        nbLignes = nbLignes + 1
        i = i + 1
    FIN

    ligne est un Corps
        Trace("lignes : " + nbLignes)
    FIN
FIN
```

Ici `nbLignes` est accessible dans tout l'État ; `i`, confiné, ne l'est pas.

### Contrainte : `Corps` obligatoire

Un État sans bloc `Corps` est refusé à la compilation. C'est le seul
sous-bloc obligatoire ; les cinq autres sont optionnels.

### Contrainte : pas d'ouverture de fenêtre depuis un État

`Ouvre`, `OuvreFille` et `OuvreSoeur` sont **refusés à la compilation**
dans tout code lexicalement contenu dans un `est un État ... FIN` —
body top-level, sous-blocs (`Corps`, `EnTêteDocument`, ...), procédures
réservées (`Lecture`, `Filtre`, `Fermeture`) et helpers utilisateur. Un
État produit un rapport, il n'a pas de boucle d'événements UI ; ces
trois appels n'ont pas de sens dans ce contexte.

---

## 2. Les 6 sous-blocs canoniques

| Bloc | Genre | Rôle |
|---|---|---|
| `EnTêteDocument` | un | Une fois en début de document — titre, période, logo |
| `HautDePage` | un | Une fois par page — numéro de page, titre court |
| `Corps` | un | Contenu principal — **obligatoire** |
| `BasDePage` | un | Une fois par page — date d'impression, footer |
| `FinDeDocument` | une | Une fois en fin de document — totaux, signatures |
| `Itération` | une | Bloc libre, déclenché uniquement par `iImprimeBloc(…)` |

> Le genre grammatical (`un`/`une`) suit l'usage français. Le compilateur
> accepte les deux articles ; choisis celui qui sonne juste.

Chaque sous-bloc peut contenir :
- Un bloc `PROPRIETES ... FIN` (cf. §9)
- Du **code OWL** dans son body, exécuté à chaque rendu du bloc
- Des **contrôles WYSIWYG** (libellés, images) déclarés comme dans une
  Fenêtre, en positionnement absolu

```owl
totaux est une FinDeDocument
    PROPRIETES
        Hauteur = 30
    FIN
    nMontantTotal = CalculeTotal()
    LBL_Total..Libellé = "Total : " + nMontantTotal
FIN
```

> **À noter** : une propriété déclarée comme une **expression dans `PROPRIETES`**
> (ex. `Libellé = "Total : " + n`) se **ré-évalue à chaque rendu du bloc**, avec
> la valeur courante des variables qu'elle référence. Un même bloc imprimé N fois
> (boucle `iImprimeBloc`, rendu automatique du Corps) affiche donc N valeurs
> distinctes si l'expression en dépend. Une propriété **littérale** (`Libellé =
> "Fixe"`) reste identique à chaque rendu. L'affectation impérative par **code
> dans le body** (`LBL_Total..Libellé = …`, comme ci-dessus) demeure disponible
> et **prime** sur l'expression `PROPRIETES` : quand les deux coexistent sur le
> même contrôle, la valeur affectée par code l'emporte.

---

## 3. Imprimer un État

L'impression demande **trois étapes** :

```owl
iDestination(iPDFGénérique, "/tmp/facture.pdf")
bOK est un booléen = iImprimeEtat(ETAT_Facture)
iFinImprime()
```

- `iDestination` ouvre la sortie PDF.
- `iImprimeEtat(ETAT_X [, p1, p2, ...])` lance la séquence (cf. §4).
  Retourne `Vrai` si l'impression a réussi.
- `iFinImprime()` finalise le PDF.

Appeler `iImprimeEtat` sans `iDestination` préalable produit une erreur
runtime.

---

## 4. Séquence d'exécution

L'ordre réel d'exécution dans `iImprimeEtat`, pour un rapport
mono-page sans `iSautePage` :

```
1.  Injection des paramètres (si est un État(p1, p2, ...))
2.  Application des PROPRIETES (Format, Orientation, marges)
3.  Event Ouverture (= code top-level du body, hors déclarations) — 1re impression uniquement
4.  Initialisation de chaque contrôle WYSIWYG des sous-blocs
5.  EnTêteDocument
6.  HautDePage
7.  PROCEDURE Lecture       (1 fois)
8.  PROCEDURE Filtre        (1 fois)
9.  Corps                   (1 fois, en plus des iImprimeBloc dans Lecture)
10. FinDeDocument
11. BasDePage
12. PROCEDURE Fermeture
```

Quatre points à noter :

- **`Lecture` est appelé avant `Filtre`**, dans l'ordre indiqué.
- **Les hooks `Lecture` et `Filtre` sont des slots 1×**. Aucune boucle
  automatique sur une source de données — c'est au dev de produire
  l'itération (cf. §6).
- **`Corps` est rendu 1 fois automatiquement** après `Filtre`,
  **en plus** des appels manuels à `iImprimeBloc("Corps")` faits depuis
  `Lecture`. Si tu boucles N fois sur Corps dans Lecture, tu obtiens
  **N+1** rendus.
- **`BasDePage` est imprimé à chaque fin de page**. Avec `iSautePage`
  (§7), la séquence devient : `BasDePage` → page break → `HautDePage`.
  Un `BasDePage` final est aussi imprimé après `FinDeDocument`.

---

## 5. Hooks lifecycle : `Filtre`, `Lecture`, `Fermeture`

Trois procédures réservées que le runtime appelle à des points fixes
de la séquence ci-dessus :

| Procédure | Appelée | Sémantique runtime actuelle |
|---|---|---|
| `Lecture()` (alias `Read`) | Étape 7 | Place canon pour la **boucle d'itération** + calculs dérivés |
| `Filtre()` (alias `Filter`) | Étape 8 | Renvoie un booléen — **valeur de retour non consommée** |
| `Fermeture()` (alias `Closing`) | Étape 12 | Cleanup final. Ne peut rien annuler |

> **`Filtre` n'agit pas comme un filtre actif.** La procédure est
> appelée à l'étape 8 et son retour est ignoré : retourner `Faux` ne
> saute pas l'enregistrement. Pour filtrer effectivement, conditionner
> manuellement l'appel à `iImprimeBloc("Corps")` dans `Lecture`.

---

## 6. Itération manuelle : `iImprimeBloc`

Pour produire un rapport à N lignes, le dev gère sa propre boucle dans
`Lecture` (ou ailleurs), et appelle `iImprimeBloc("nom_du_bloc")` à
chaque tour. La cible peut être :

- Un sous-bloc canon (`Corps`, `EnTêteDocument`, ...) — réimprimé
- Un bloc `Itération` nommé — imprimé à la demande

```owl
nLigneCourante est un entier = 0

ETAT_Liste est un État
    ligne est un Corps
        Trace("ligne " + nLigneCourante)
    FIN

    PROCEDURE Lecture()
        // 4 itérations explicites + 1 rendu automatique de Corps = 5 lignes
        POUR i = 1 A 4
            nLigneCourante = i
            iImprimeBloc("ligne")
        FIN
        nLigneCourante = 5     // pour le rendu automatique final
    FIN
FIN
```

Plusieurs blocs `Itération` peuvent coexister avec des noms distincts —
utile pour des récaps intermédiaires, séparateurs, encarts
conditionnels.

`iImprimeBloc` appelé hors d'un État en cours d'impression renvoie
`Faux` et ne fait rien.

---

## 7. Multi-page : `iSautePage`

`iSautePage()` force un saut de page dans la séquence d'impression. Le
runtime imprime `BasDePage`, ouvre une nouvelle page, et imprime
`HautDePage` sur cette nouvelle page.

```owl
PROCEDURE Lecture()
    POUR i = 1 A 3
        iImprimeBloc("ligne")
        SI i < 3 ALORS iSautePage()
    FIN
FIN
```

La propriété `SautDePageApresBloc = Vrai` sur un sous-bloc déclenche
le même mécanisme automatiquement après son rendu (cf. §9).

---

## 8. Paramètres d'État

Une déclaration peut prendre des paramètres typés ; ils sont accessibles
comme **globales** dans tout le code de l'État (sous-blocs et
procédures) :

```owl
ETAT_Facture est un État(sClient est une chaîne, mMontant est un réel)
    intro est un EnTêteDocument
        Trace("Client : " + sClient)
    FIN
    ligne est un Corps
        Trace("Montant : " + mMontant)
    FIN
FIN

iDestination(iPDFGénérique, "/tmp/f.pdf")
iImprimeEtat(ETAT_Facture, "Alice", 1499.99)
iFinImprime()
```

Les valeurs passées à `iImprimeEtat` sont injectées dans les globales
correspondantes **au début de la séquence** (étape 1), avant
`Ouverture`.

---

## 9. Propriétés

### Au niveau racine de l'État

| Propriété | Type | Valeurs |
|---|---|---|
| `Format` | enum | `formatA4` (défaut), `formatA3`, `formatA5`, `formatLetter`, `formatLegal` |
| `Orientation` | enum | `orientPortrait` (défaut), `orientPaysage` |
| `MargeHaut`, `MargeBas`, `MargeGauche`, `MargeDroite` | entier (mm) | — |

### Au niveau de chaque sous-bloc

| Propriété | Type | Effet |
|---|---|---|
| `Hauteur` | entier (mm) | Hauteur réservée |
| `Visible` | booléen (défaut Vrai) | Faux → bloc skippé à l'impression |
| `CouleurFond` | couleur | Fond du bloc |
| `Sécable` | booléen | Peut être coupé entre deux pages |
| `SautDePageApresBloc` | booléen | Saut de page automatique après ce bloc |

---

## 10. Différences principales avec Fenêtre

| Aspect | Fenêtre | État |
|---|---|---|
| Boucle d'événements | Oui (clic, focus, ...) | Aucune — séquence linéaire d'impression |
| Code top-level body | Rejeté | Accepté → compilé en event `Ouverture` |
| Hook équivalent à `FinInitialisation` | Oui (`FinInitialisation`) | Non. L'event `Ouverture` joue ce rôle |
| Plusieurs déclarations / fichier | Non (1 Fenêtre par fichier) | Oui |
| Mix Fenêtre + État dans un fichier | Interdit | Interdit |
| `Ouvre` / `OuvreFille` / `OuvreSoeur` | Légitime (ouverture d'une autre fenêtre) | Refusé compile-time |
| Annulation possible | `AutorisationFermeture` | Aucun mécanisme |

---

## 10bis. `MoiMême` dans un État

Le mot-clé `MoiMême` (alias EN `MySelf`) fonctionne aussi dans les
sous-blocs et top-levels d'État, avec une sémantique strictement
analogue à celle des fenêtres (cf. `15_lifecycle_champs.md` §7bis) :

- **Top-level d'un sous-champ** d'État (code de niveau 0 dans le body
  du champ, hors `PROCEDURE`) : `MoiMême` désigne le sous-champ
  courant. `MoiMême..Libellé = "..."` y est équivalent à
  `LIB_X..Libellé = "..."`.
- **Top-level de l'État** (code de niveau 0, équivalent compilé de
  `Ouverture`) et hooks `Lecture` / `Filtre` / `Fermeture` :
  `MoiMême` référence l'État lui-même. L'accès à des propriétés
  via `MoiMême..Prop` y est réservé pour une version ultérieure ;
  pour l'instant, lire et écrire les paramètres et propriétés par
  leur nom direct.

`MoiMême` s'hérite le long de la chaîne d'appel : une procédure
appelée depuis un top-level sous-champ voit le même `MoiMême` que
son appelant.

---

## 11. Pièges courants

- **Filtre ne filtre pas**. Le retour de `Filtre` n'est pas consommé
  par le runtime actuel. Pour filtrer effectivement, tester la
  condition et appeler `iImprimeBloc` conditionnellement dans `Lecture`.
- **Pas de boucle automatique sur les données**. `Corps` est appelé
  une seule fois par défaut. Pour N lignes, boucler dans `Lecture`
  avec `iImprimeBloc("Corps")` (ou via `iSautePage` entre les pages).
- **Corps tire N+1 fois si tu boucles N fois dans Lecture**. Le rendu
  automatique de `Corps` à l'étape 9 s'ajoute aux appels manuels
  faits depuis `Lecture`. Si tu veux exactement N rendus, n'appelle
  pas `iImprimeBloc("Corps")` la dernière fois — laisse le rendu
  automatique le faire.
- **`iImprimeEtat` sans `iDestination` au préalable** → erreur runtime.
  Toujours `iDestination(...)`, puis `iImprimeEtat`, puis
  `iFinImprime()`.
- **Bloc `Corps` absent** → erreur compile. C'est le seul sous-bloc
  obligatoire.
- **Variables top-level body**. Elles sont **portées par l'État**
  (pas globales du programme) et initialisées à la **1re impression**
  uniquement ; leur valeur **persiste** ensuite d'une impression à la
  suivante. Le top-level (`Ouverture`) ne rejoue pas aux impressions
  suivantes : le réinitialiser là ne rafraîchit rien au-delà de la
  première. Pour repartir d'un état frais à chaque impression,
  réinitialiser dans un hook rejoué à chaque impression, par exemple
  `Lecture`.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Contrôle de flux | `03_controle_de_flux.md` |
| Cycle de vie d'une Fenêtre | `15_lifecycle_champs.md` |
| Conventions de code | `10_conventions_de_code.md` |
