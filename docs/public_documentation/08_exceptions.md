# Exceptions

Une exception interrompt le flux normal d'une procédure pour signaler
une condition que le code appelant doit traiter. OWL fournit deux
formes complémentaires pour les attraper, un bloc dédié au nettoyage
garanti des ressources, une fonction pour les lever, et un petit jeu
d'outils pour inspecter ou propager l'exception en cours.

Les deux formes principales partagent la même mécanique de capture
mais répondent à deux besoins différents : protéger **un bloc précis**
(forme spécifique) ou **toute la suite d'une procédure** (forme
générale).

---

## 1. Forme spécifique : protéger un bloc

```owl
QUAND EXCEPTION DANS
    n est un entier = EvalueExpression(sFormule)
    nResultat est un entier = 100 / n
FAIRE
    Trace("Calcul impossible : " + ExceptionInfo(errMessage))
FIN
```

Le bloc `DANS … FAIRE` est la zone protégée. Si une exception y est
levée, l'exécution saute immédiatement au bloc `FAIRE` qui décide quoi
faire — souvent : tracer, replier sur une valeur par défaut, ou
propager.

> Attention : toutes les natives ne lèvent pas d'exception sur erreur.
> Beaucoup (lecture fichier, parsing JSON, requêtes SQL…) renvoient une
> valeur neutre (chaîne vide, Null…) et positionnent `ErreurDétectée` à
> Vrai. Le bloc `FAIRE` ne se déclencherait pas pour ce type d'échec.
> Pour ces cas, utiliser le pattern soft-error :
>
> ```owl
> contenu est une chaîne = fChargeTexte("config.txt")
> Si ErreurDétectée Alors
>     Trace("Lecture impossible : " + ErreurInfo())
> Sinon
>     Trace("OK, " + Taille(contenu) + " caractères lus")
> Fin
> ```
>
> Les exceptions sont réservées aux erreurs réellement bloquantes
> (division par zéro, déréférencement Null, échec d'allocation…) ou aux
> erreurs explicitement promues via `ExceptionDéclenche` ou
> `ErreurPropage()`.

Quand aucune exception n'est levée, le bloc `FAIRE` n'est **pas**
exécuté, et le programme continue après le `FIN`. Pour distinguer
explicitement les deux issues, ajouter un bloc `SINON` qui ne s'exécute
qu'en cas de succès :

```owl
QUAND EXCEPTION DANS
    bDone est un booléen = TraiterFichier(sChemin)
FAIRE
    Trace("Échec sur " + sChemin)
SINON
    Trace("Succès sur " + sChemin)
FIN
```

Après le `FIN`, le programme reprend son cours normal — qu'une
exception ait été attrapée ou non. La forme spécifique est l'outil de
choix quand on veut continuer la procédure après le traitement.

---

## 2. `ENFIN` : nettoyage garanti

Un bloc `ENFIN` placé avant le `FIN` s'exécute **toujours**, quelle
que soit l'issue du bloc protégé :

```owl
f est un fichier
QUAND EXCEPTION DANS
    f = fOuvre("data.txt", foLecture)
    contenu est une chaîne = fLisLigne(f)
FAIRE
    Trace("Lecture impossible : " + ExceptionInfo(errMessage))
ENFIN
    fFerme(f)              // exécuté que l'on ait lu ou planté
FIN
```

C'est le mécanisme à utiliser dès qu'une ressource doit être relâchée
de manière fiable : fichier ouvert, verrou pris, transaction commencée,
connexion à libérer. Sans `ENFIN`, ce nettoyage devrait être dupliqué
manuellement dans le `FAIRE` **et** après le `FIN`, ce qui est une
source classique de fuites.

### Syntaxe et ordre des clauses

`ENFIN` est optionnel et se place **après** les éventuels `FAIRE` et
`SINON`. L'ordre des clauses est strict :

```
QUAND EXCEPTION DANS <try>
    [FAIRE   <handler>]
    [SINON   <else>]
    [ENFIN   <cleanup>]
FIN
```

Toutes les clauses sont optionnelles individuellement, mais le bloc
`DANS` ne sert à rien sans au moins l'une d'entre elles. Un seul
`ENFIN` autorisé par bloc.

### Quatre chemins, un seul cleanup

Quel que soit le chemin d'exécution, le bloc `ENFIN` s'exécute :

| Chemin | Trigger | Ordre |
|---|---|---|
| Succès | aucune exception levée | `DANS` → (`SINON` si présent) → `ENFIN` |
| Attrapée | exception levée et attrapée par `FAIRE` | `DANS` (partiel) → `FAIRE` → `ENFIN` |
| Traverse | exception levée sans `FAIRE` pour l'attraper | `DANS` (partiel) → `ENFIN` → propagation |
| Re-lancée | `FAIRE` re-lance via `EXCEPTION` | `DANS` (partiel) → `FAIRE` (partiel) → `ENFIN` → propagation |

Sur les deux derniers chemins, le `ENFIN` s'exécute **avant** que
l'exception remonte au scope parent : le cleanup a lieu, puis
l'exception poursuit son chemin.

### Le cleanup ne doit pas interrompre le flux

Le bloc `ENFIN` ne peut pas contenir d'instruction qui modifie le flux
de la procédure : ces formes sont refusées compile-time.

| Interdit dans `ENFIN` | Pourquoi |
|---|---|
| `RENVOYER`, `RETOUR` | écraserait silencieusement le retour du `DANS` ou du `FAIRE` |
| `SORTIR`, `CONTINUER` | masquerait le contrôle de boucle de la procédure |
| `EXCEPTION` (re-lance) | masquerait l'exception en cours de propagation |
| `ExceptionDéclenche` | idem, et empêcherait le diagnostic d'origine |

Diagnostic compile-time : « *'<keyword>' is not allowed inside a
'ENFIN' (FINALLY) block. Cleanup must not interrupt the flow.* »

Cette discipline garantit que le bloc protégé décide **toujours** du
résultat de la procédure : `ENFIN` nettoie, point. Si le nettoyage
lui-même peut échouer (ex. fermer une connexion réseau qui timeout),
envelopper l'appel risqué dans un sous-bloc `QUAND EXCEPTION DANS`
imbriqué dans le `ENFIN`.

### Restriction à la forme spécifique

`ENFIN` n'a de sens qu'avec la forme `QUAND EXCEPTION DANS`. Le
compilateur le refuse dans la forme générale (cf. section 3) :
celle-ci termine déjà la procédure après son handler, il n'y a rien
à nettoyer entre le handler et un hypothétique code suivant.

---

## 3. Forme générale : filet pour la suite de la procédure

```owl
PROCEDURE TraiterLot(t est un tableau de chaînes)
    QUAND EXCEPTION
        Trace("Lot interrompu : " + ExceptionInfo(errMessage))
        RENVOYER
    FIN
    POUR TOUT s DE t
        TraiterUn(s)
    FIN
FIN
```

`QUAND EXCEPTION … FIN` sans `DANS` déclare un handler qui couvre
**tout le reste** de la procédure courante. Toute exception levée
après ce point — directement ou depuis une fonction appelée — est
capturée.

**La procédure se termine après l'exécution du handler.** Le code écrit
après le `FIN` du handler ne s'exécute pas quand le handler a été
déclenché ; il ne s'exécute que sur le flux normal, c'est-à-dire quand
aucune exception n'est levée.

```owl
PROCEDURE ProcEcho(s est une chaîne) : chaîne
    QUAND EXCEPTION
        RENVOYER "[erreur] " + ExceptionInfo(errMessage)
    FIN
    // Flux normal : le handler n'est PAS appelé.
    RENVOYER "echo:" + s
FIN
```

Cette terminaison automatique évite la classe de bugs où une exception
serait attrapée puis re-levée silencieusement par le code qui suit
(par exemple parce qu'il dépend d'une variable qui n'a pas été
initialisée correctement).

Si du code doit s'exécuter après le handler, deux options :

- l'écrire **avant** la déclaration du handler ;
- utiliser la forme spécifique `QUAND EXCEPTION DANS … FAIRE`, qui
  reprend naturellement le flux après le bloc.

### Un seul handler général actif à la fois

Une procédure peut déclarer plusieurs handlers généraux successifs. À
partir de la ligne qui suit le `FIN` d'un handler, **seul le dernier
déclaré** est actif. La déclaration suivante remplace la précédente
pour la suite de la procédure.

```owl
PROCEDURE Etape()
    QUAND EXCEPTION
        Trace("preparation : erreur")
        RENVOYER
    FIN
    Preparer()      // erreur ici -> handler 1

    QUAND EXCEPTION
        Trace("execution : erreur")
        RENVOYER
    FIN
    Executer()      // erreur ici -> handler 2 (pas handler 1)
FIN
```

---

## 4. Lever une exception

`ExceptionDéclenche` lève une exception avec un code et un message
optionnels :

```owl
ExceptionDéclenche("Fichier introuvable")
ExceptionDéclenche(404, "Ressource absente")
ExceptionDéclenche(2)
```

Un troisième argument optionnel transporte des informations
complémentaires distinctes du message — typiquement un contexte
machine-lisible (clé/valeur, JSON court) destiné à un handler qui
veut prendre une décision conditionnelle :

```owl
ExceptionDéclenche(404, "Ressource absente", "url=/api/users/42")
```

Le handler le récupère via `ExceptionInfo(errInfo)`. Sans 3ᵉ
argument, `errInfo` retourne une chaîne vide.

Les paramètres sont indépendants : on peut fournir un message seul,
un code seul, le code et le message, ou les trois ensemble. Sans
argument, l'exception porte un message générique.

L'exception remonte la pile d'appels jusqu'au premier handler qui peut
la traiter. Si aucun handler ne la capture, l'exécution s'arrête avec
un message d'erreur incluant la trace d'appels.

---

## 5. Inspecter l'exception en cours

Depuis un handler, `ExceptionInfo` retourne les détails de l'exception
qui vient d'être attrapée. Sans argument, elle retourne le message.
Avec une constante `errXxx`, elle retourne le champ correspondant :

```owl
QUAND EXCEPTION DANS
    ExceptionDéclenche(42, "boom")
FAIRE
    Trace(ExceptionInfo())                  // "boom"
    Trace(ExceptionInfo(errCode))           // 42
    Trace(ExceptionInfo(errMessage))        // "boom"
    Trace(ExceptionInfo(errLigne))          // n° de ligne de la levée
    Trace(ExceptionInfo(errPile))           // trace d'appels formatée
FIN
```

Les constantes disponibles :

| Constante | Contenu |
|---|---|
| `errMessage` | Message complet de l'exception (chaîne) |
| `errCode` | Code numérique de l'exception (entier) |
| `errLigne` | Ligne source où l'exception a été levée (entier) |
| `errÉlément` | Nom du fichier source où la levée a eu lieu (chaîne) |
| `errTraitement` | Nom de la procédure courante au moment de la levée (chaîne) |
| `errPendant` | Nom de la fonction qui a réellement déclenché l'exception (chaîne) |
| `errPile` | Pile d'appels formatée (chaîne multi-ligne) |
| `errRésumé` | Première ligne du message, tronquée à 80 caractères (chaîne) |
| `errInfo` | Informations complémentaires passées en 3ᵉ argument à `ExceptionDéclenche`, sinon chaîne vide |
| `errDateHeure` | Horodatage Unix milliseconde de la levée (entier) |
| `errComplet` | Toutes les infos concaténées (chaîne) |
| `errCodeSystème` | Code d'erreur système si l'origine en a fourni un, 0 sinon (entier) |
| `errMessageSystème` | Message d'erreur système si l'origine en a fourni un, vide sinon (chaîne) |
| `errNombreSousErreur` | Nombre de sous-erreurs (entier, actuellement toujours 0) |

### Portée du contexte d'exception

Le contexte lu par `ExceptionInfo` suit strictement le handler en cours :

- **Dans un handler** : `ExceptionInfo` décrit l'exception attrapée par
  ce handler précis (message, code, ligne, etc.).
- **Hors de tout handler** : le contexte est vide. `ExceptionInfo()`
  et `ExceptionInfo(errMessage)` retournent une chaîne vide,
  `ExceptionInfo(errCode)` retourne `0`, et ainsi de suite. Aucune
  donnée résiduelle de la dernière exception traitée ne persiste après
  le `FIN` du handler. C'est volontaire : un helper de log peut
  s'exécuter indifféremment dans les deux contextes sans lever
  d'erreur ni lire des valeurs périmées.
- **Handlers imbriqués** : quand un handler en contient un autre, le
  contexte interne s'applique tant que le handler interne s'exécute.
  À la sortie du handler interne, le contexte du handler englobant est
  restauré : les lectures qui suivent, toujours dans le handler externe,
  retrouvent l'exception externe et non celle, désormais close, du bloc
  interne.

```owl
QUAND EXCEPTION DANS
    ExceptionDéclenche(7, "externe")
FAIRE
    Trace(ExceptionInfo(errCode))       // 7

    QUAND EXCEPTION DANS
        ExceptionDéclenche(3, "interne")
    FAIRE
    FIN

    Trace(ExceptionInfo(errCode))       // 7 — le contexte externe est restauré
FIN
Trace(ExceptionInfo(errCode))           // 0 — hors handler, contexte vide
```

---

## 6. Propager au niveau supérieur

Un handler peut décider qu'il ne sait pas traiter l'exception et la
laisser remonter. Deux formes équivalentes :

```owl
QUAND EXCEPTION DANS
    Action()
FAIRE
    SI ExceptionInfo(errCode) = 99 ALORS
        Trace("traitement local")
    SINON
        EXCEPTION              // re-lève l'exception courante
    FIN
FIN
```

Le mot-clé `EXCEPTION` seul, à l'intérieur d'un handler, re-lève
l'exception en cours avec son message, son code et son contexte
intacts. La même chose peut s'écrire en fonction :

```owl
ExceptionPropage()
```

Les deux formes sont strictement équivalentes. Le mot-clé est plus
lisible quand on enchaîne une décision conditionnelle ; la fonction
peut être pratique pour la passer en argument à un helper.

---

## 7. Interdiction dans une boucle

La forme générale `QUAND EXCEPTION` (sans `DANS`) ne peut pas être
déclarée à l'intérieur d'une boucle `POUR`, `POUR TOUT`, `TANTQUE` ou
`BOUCLE`. Le compilateur refuse :

```owl
POUR i = 1 A 10
    QUAND EXCEPTION           // ERREUR compile
        Trace("oups")
    FIN
    Traiter(i)
FIN
```

Diagnostic compile-time : « *'QUAND EXCEPTION' (general form) not
allowed inside a loop. Use 'QUAND EXCEPTION DANS ... FAIRE ... FIN' to
protect a specific block.* »

La raison est sémantique : la forme générale termine la procédure
après son handler. Placée dans une boucle, son comportement serait
trompeur (une seule itération attrape, le reste passe par le code qui
suit le `FIN` du handler, qui n'est jamais censé être atteint).

Deux remplacements selon l'intention :

- Pour protéger **chaque itération** indépendamment, utiliser la forme
  spécifique :

  ```owl
  POUR TOUT s DE liste
      QUAND EXCEPTION DANS
          Traiter(s)
      FAIRE
          Trace("Échec sur " + s)
      FIN
  FIN
  ```

- Pour protéger **toute la procédure**, déclarer le handler **avant**
  la boucle :

  ```owl
  PROCEDURE TraiterTout(liste est un tableau de chaînes)
      QUAND EXCEPTION
          Trace("Lot interrompu")
          RENVOYER
      FIN
      POUR TOUT s DE liste
          Traiter(s)
      FIN
  FIN
  ```

---

## 8. Choix entre les deux formes

| Besoin | Forme adaptée |
|---|---|
| Protéger une ligne ou un bloc précis, poursuivre après | `QUAND EXCEPTION DANS … FAIRE … FIN` |
| Protéger toute la suite d'une procédure, terminer si erreur | `QUAND EXCEPTION … FIN` |
| Libérer une ressource quelle que soit l'issue | `QUAND EXCEPTION DANS … ENFIN … FIN` |
| Protéger chaque itération d'une boucle | Forme spécifique à l'intérieur de la boucle |
| Filet global d'une procédure qui contient une boucle | Forme générale, déclarée avant la boucle |

---

## 9. Pièges courants

- **Code après le `FIN` d'un handler général** : ce code ne s'exécute
  que sur le flux normal (aucune exception levée). Si on veut qu'il
  s'exécute aussi après un traitement d'erreur, écrire la logique dans
  le handler lui-même ou utiliser la forme spécifique.
- **Capturer une exception puis l'oublier** : un handler vide
  (`QUAND EXCEPTION … FIN` qui ne fait rien) avale silencieusement
  toutes les erreurs. À réserver aux cas où l'absence de réaction est
  un choix conscient et documenté.
- **`EXCEPTION` hors d'un handler** : le mot-clé seul n'est reconnu
  qu'à l'intérieur du corps d'un `QUAND EXCEPTION` ou `FAIRE`. Ailleurs,
  le compilateur le refuse.
- **Cleanup risqué dans `ENFIN`** : la fermeture d'une ressource peut
  elle-même échouer. Si c'est probable (réseau, IO distant), envelopper
  l'appel dans un sous-bloc `QUAND EXCEPTION DANS` à l'intérieur du
  `ENFIN` plutôt que de laisser une seconde exception masquer la
  première.

---

## 10. Aliases anglais

| Français | Anglais |
|---|---|
| `QUAND EXCEPTION … FIN` | `WHEN EXCEPTION … END` |
| `QUAND EXCEPTION DANS … FAIRE … [SINON …] [ENFIN …] FIN` | `WHEN EXCEPTION IN … DO … [ELSE …] [FINALLY …] END` |
| `ENFIN` | `FINALLY` |
| `EXCEPTION` (mot-clé re-raise) | `EXCEPTION` |
| `ExceptionDéclenche` | `ExceptionThrow` |
| `ExceptionInfo` | `ExceptionInfo` |
| `ExceptionPropage` | `ExceptionPropagate` |

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Contrôle de flux (`SI`, `POUR`, `TANTQUE`…) | `03_controle_de_flux.md` |
| Conventions de code | `10_conventions_de_code.md` |
| Charte de test (assertions, scénarios) | `11_charte_de_test.md` |

Pour la liste des constantes `errXxx` disponibles : `owl explain
ExceptionInfo`. Pour le détail d'une fonction Exception\* :
`owl explain ExceptionDéclenche`, `owl explain ExceptionPropage`, etc.
