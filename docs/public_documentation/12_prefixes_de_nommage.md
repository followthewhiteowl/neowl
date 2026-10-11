# Préfixes de nommage

Cette fiche détaille les préfixes de nom pour indiquer **la portée**,
**le type** et **la nature** d'une entité. Ces préfixes sont une
convention, pas une obligation du compilateur : le code compile et
tourne sans eux. Ils existent pour deux raisons :

1. **Lecture rapide.** Un nom préfixé renseigne sur ses propriétés
   sans qu'il faille remonter à la déclaration.
2. **Discipline d'écriture cohérente.** Quand plusieurs personnes
   écrivent dans le même projet, des préfixes partagés évitent les
   disparités stylistiques.

Chaque équipe peut imposer ou alléger sa propre charte ; les tables
ci-dessous décrivent celle qui sert de référence dans la
documentation et les exemples.

---

## 1. Préfixes de portée

Le préfixe de portée vient **avant** le préfixe de type. Il indique où
la variable est visible.

| Portée | Préfixe |
|---|---|
| Variable locale (procédure, bloc) | aucun |
| Variable globale du projet, d'une fenêtre, d'un état | `g` |
| Membre d'instance de classe | `m_` |
| Membre global d'une classe (équivalent statique) | `mg_` |
| Propriété d'instance | `p_` |
| Propriété globale | `pg_` |

```owl
// dans projet.owl, au niveau supérieur du projet
gbModeTest est un booléen = Faux       // global projet, booléen

PROCEDURE TraiterCommande()
    nLigne est un entier = 0           // locale, entier
    POUR i = 1 A 10
        nLigne += i
    FIN
FIN

cCompte est une Classe
PUBLIC
    m_solde est un monétaire
    mg_compteur est un entier
FIN
```

---

## 2. Préfixes de type pour les variables

Les préfixes ci-dessous se collent **directement** au nom (sans
souligné), en minuscules. Le nom qui suit commence par une majuscule
(camelCase).

| Type | Préfixe | Exemple |
|---|---|---|
| `chaîne` | `s` | `sNom`, `sAdresseEmail` |
| `entier` | `n` | `nAge`, `nNbLignes` |
| `réel` | `r` | `rTaux`, `rDistance` |
| `monétaire` | `mo` | `moMontant`, `moPrixHT` |
| `numérique` | `x` | `xTotal`, `xCoefficient` |
| `booléen` | `b` | `bActif`, `bAffiche` |
| `date` | `d` | `dNaissance`, `dEcheance` |
| `heure` | `h` | `hOuverture`, `hDepart` |
| `dateheure` | `dh` | `dhCreation`, `dhDerniereModif` |
| `durée` | `du` | `duTimeout`, `duVisite` |
| `tableau` | `tab` | `tabClients`, `tabLignes` |
| `buffer` | `buf` | `bufImage`, `bufContenu` |

> Note : `s` pour `chaîne` correspond à *string* en anglais — la
> mnémonique fonctionne dans les deux langues.

---

## 3. Préfixes pour les types user-defined et leurs instances

Une convention en deux temps distingue **le type** (la définition) et
**l'instance** (la variable qui contient une valeur de ce type) :

| Catégorie | Définition (le type) | Instance (la variable) | Exemple |
|---|---|---|---|
| Énumération | `E` + PascalCase | `e` + PascalCase | `EStatut` type, `eEtatActuel` instance |
| Combinaison | `CO` + PascalCase | `co` + PascalCase | `COPermissions` type, `coDroitsUtilisateur` instance |
| Structure | `ST` + PascalCase | `st` + PascalCase | `STAdresse` type, `stLivraison` instance |
| Classe | `c` + PascalCase | `o` + PascalCase | `cCompte` type, `oCompteCourant` instance |

```owl
EStatut est une Énumération
    actif
    suspendu
    ferme
FIN

STAdresse est une Structure
    sRue est une chaîne
    sCodePostal est une chaîne
    sVille est une chaîne
FIN

cCompte est une Classe
PUBLIC
    m_solde est un monétaire
FIN

PROCEDURE TraiterCompte()
    eStatutCourant est un EStatut = actif
    stAdresse est un STAdresse
    oCompte est un cCompte
FIN
```

Note sur les classes : le `c` minuscule désigne le **type** (la classe
elle-même), le `o` (pour *objet*) désigne une **instance**. Cette
distinction n'est pas symétrique avec les structures/énumérations (où
le type est en MAJUSCULES et l'instance en minuscules) ; c'est une
convention assumée.

---

## 4. Préfixes des champs d'interface

Les champs d'une fenêtre ou d'un état portent un préfixe en
**MAJUSCULES** suivi d'un souligné, puis du nom du champ en PascalCase.
Ce préfixe identifie le type de champ en un coup d'œil dans le code et
dans le designer.

### Conteneurs principaux

| Type | Préfixe | Exemple |
|---|---|---|
| Fenêtre | `FEN_` | `FEN_Login` |
| État | `ETAT_` | `ETAT_Facture` |

### Champs (utilisables dans Fenêtre et État)

| Type de champ | Préfixe | Exemple |
|---|---|---|
| Libellé | `LIB_` | `LIB_Titre`, `LIB_NomClient` |
| Bouton | `BTN_` | `BTN_Valider`, `BTN_Annuler` |
| Saisie | `SAI_` | `SAI_Utilisateur`, `SAI_MotDePasse` |
| Image | `IMG_` | `IMG_Logo`, `IMG_Photo` |
| Liste | `LISTE_` | `LISTE_Produits` |
| Combo | `COMBO_` | `COMBO_Pays` |
| Sélecteur | `SEL_` | `SEL_Civilite` |
| Interrupteur | `INT_` | `INT_Newsletter` |
| Conteneur | `CONT_` | `CONT_Formulaire` |
| Table | `TABLE_` | `TABLE_Commandes` |
| Colonne de table | `COL_` | `COL_Reference`, `COL_Quantite` |
| Zone répétée | `ZR_` | `ZR_LignesFacture` |
| Jauge | `JAU_` | `JAU_Chargement`, `JAU_Progression` |

Les blocs d'un état (`EnTêteDocument`, `HautDePage`, `Corps`,
`BasDePage`, `FinDeDocument`, `Itération`) ne portent pas de préfixe :
ce sont des structures fixes du langage, pas des champs nommés
arbitrairement.

```owl
FEN_Login est une Fenêtre
    LIB_Titre est un Champ Libellé
    SAI_Utilisateur est un Champ Saisie
    SAI_MotDePasse est un Champ Saisie
    BTN_Valider est un Champ Bouton
    BTN_Annuler est un Champ Bouton
FIN
```

### Vérification automatique (opt-in projet)

Par défaut, les préfixes de champs restent une convention : rien n'est
imposé. Un projet peut activer une vérification en posant la propriété
`ActiveAvertissementConventionDeNommage` (bilingue
`EnableNamingConventionWarning`) à `Vrai` dans le
bloc `est un Projet` de son `projet.owl` :

```owl
MonApplication est un Projet
    PROPRIETES
        Langues     = [langueFrançais]
        Description = "Gestion Commandes"
        ActiveAvertissementConventionDeNommage = Vrai
    FIN
FIN
```

À `Vrai`, le compilateur signale chaque champ d'interface dont le nom
ne commence pas par le préfixe canonique de son type — `BTN_` pour un
Bouton, `SAI_` pour une Saisie, `COL_` pour une Colonne, etc. Le signal
est un **avertissement non bloquant** : le code compile et s'exécute
normalement, l'avertissement sert de rappel pour uniformiser le
nommage.

```owl
FEN_Login est une Fenêtre
    Valider est un Champ Bouton    // averti : préfixe attendu 'BTN_'
    BTN_Annuler est un Champ Bouton // conforme, aucun avertissement
FIN
```

Non renseignée ou à `Faux`, la propriété laisse la vérification
désactivée.

Cette propriété fait partie du catalogue de métadonnées du bloc `est un
Projet`, décrit en détail dans `24_projet.md` ; la fiche de référence du
type et de ses champs s'ouvre avec `owl explain Projet`.

Le périmètre de cet avertissement est volontairement étroit. Il porte
**uniquement** sur les noms des champs d'interface listés dans la table
de la section 4. Ne sont donc **jamais** concernés :

- Les **variables classiques**. Les préfixes de type décrits en
  section 2 (`n` pour un entier, `s` pour une chaîne, `r` pour un
  réel…) restent une convention purement indicative : une variable mal
  préfixée ne déclenche aucun avertissement, quelle que soit la valeur
  de la propriété.
- Les **conteneurs sans préfixe canonique** (Fenêtre, Onglet), qui
  n'ont pas de préfixe attendu à contrôler.

```owl
MonApplication est un Projet
    PROPRIETES
        Langues = [langueFrançais]
        ActiveAvertissementConventionDeNommage = Vrai
    FIN
FIN

age est un entier         // aucun avertissement : variable classique
MaFenêtre est une Fenêtre  // aucun avertissement : conteneur sans préfixe canonique
    Valider est un Champ Bouton // averti : ceci est un champ, préfixe attendu 'BTN_'
FIN
```

---

## 5. Composition des préfixes

Quand plusieurs préfixes s'appliquent, ils se composent **dans l'ordre :
portée + type + nom**. Tous les préfixes restent en minuscules collés
au nom.

```owl
gbModeTest        // globale (g) + booléen (b) + ModeTest
gsCheminConfig    // globale (g) + chaîne (s) + CheminConfig
gtabUtilisateurs  // globale (g) + tableau (tab) + Utilisateurs
m_nCompteur       // membre d'instance (m_) + entier (n) + Compteur
mg_sNomService    // membre global classe (mg_) + chaîne (s) + NomService
```

La règle se lit gauche → droite : « **portée** d'un **type** appelé
**Nom** ».

---

## 6. Quand ne pas préfixer

- **Paramètres de procédure courts.** `i`, `j`, `k` en indice de
  boucle, `it` pour l'élément d'un `POUR TOUT`, restent acceptés sans
  préfixe.
- **Contexte qui rend le préfixe redondant.** Dans une procédure
  `CalculerTVA(taux)`, le paramètre `taux` n'a pas besoin de s'appeler
  `rTaux` : le contexte d'appel et la signature dévoilent déjà le
  type.
- **Procédures et fonctions natives.** Les conventions de préfixe
  s'appliquent aux **variables**, pas aux appels.
- **Mélange dans un même fichier.** Si un fichier utilise les
  préfixes, ils s'appliquent à toutes les variables ; sinon à aucune.
  Un fichier moitié-préfixé est plus illisible qu'un fichier sans
  préfixes du tout.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Conventions générales (casse, accents, indentation, commentaires) | `10_conventions_de_code.md` |
| Charte de test | `11_charte_de_test.md` |
| Aperçu général du langage | `00_apercu_du_langage.md` |
