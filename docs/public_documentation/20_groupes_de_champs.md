# Groupes de champs

Un **Groupe** est une collection nommée de champs UI qui permet
d'appliquer une même propriété à tous ses membres en une seule
écriture (broadcast). Utile pour activer/désactiver, masquer, ou
remettre à zéro un ensemble de champs liés sans boucler manuellement
dessus.

## Déclaration

Un Groupe se déclare à l'intérieur d'une `Fenêtre`, à côté de ses
champs. La valeur est une liste littérale de champs (ou vide) :

```owl
FEN_Saisie est une Fenêtre
    PROPRIETES
        Titre       = "Nouvel article"
        Disposition = dispVerticale
    FIN

    SAI_Nom      est un Champ Saisie
        PROPRIETES
            Indication = "Nom"
        FIN
    FIN

    SAI_Prix     est un Champ Saisie
        PROPRIETES
            Indication = "Prix"
        FIN
    FIN

    SAI_Quantite est un Champ Saisie
        PROPRIETES
            Indication = "Quantité"
        FIN
    FIN

    GR_Saisies est un Groupe = [SAI_Nom, SAI_Prix, SAI_Quantite]
FIN
```

Un Groupe vide est valide :

```owl
GR_AModifier est un Groupe
```

Préfixe conventionnel : `GR_` (analogue à `BTN_`, `SAI_`, `LBL_`).

## Broadcast d'une propriété

Écrire `..Prop` sur un Groupe applique la valeur à **chaque membre
qui expose cette propriété**. Les membres qui ne l'exposent pas sont
silencieusement ignorés.

```owl
// Tous les champs deviennent grisés
GR_Saisies..Grisé = Vrai

// Tous deviennent invisibles
GR_Saisies..Visible = Faux
```

**Groupe hétérogène** : aucun problème. Si un Groupe mélange Boutons,
Saisies et Libellés, écrire `..Valeur = "foo"` n'a d'effet que sur les
Saisies (qui exposent `Valeur`). Pas d'erreur, pas d'avertissement —
les autres membres restent inchangés.

```owl
GR_Mixte est un Groupe = [BTN_OK, SAI_Nom, LBL_Info]

GR_Mixte..Visible = Faux       // les 3 deviennent invisibles
GR_Mixte..Valeur  = "valeur"   // seule SAI_Nom est touchée
```

## Lecture de l'appartenance

`monChamp..Groupe` retourne un `tableau de Groupe` listant les
Groupes qui contiennent ce champ. Utile pour parcourir, tester, ou
manipuler dynamiquement l'appartenance.

```owl
tabGroupes est un tableau de Groupe = SAI_Nom..Groupe
SI tabGroupes..Occurrence > 0 ALORS
    Trace("SAI_Nom appartient à " + tabGroupes..Occurrence + " groupe(s)")
FIN
```

Un champ peut appartenir à plusieurs Groupes simultanément :

```owl
GR_Tous       est un Groupe = [BTN_OK, BTN_Annuler]
GR_Essentiels est un Groupe = [BTN_OK]

// BTN_OK figure dans les deux Groupes
nNbGroupes est un entier = BTN_OK..Groupe..Occurrence  // 2
```

## Écriture de l'appartenance

`monChamp..Groupe = [GR_X, GR_Y]` **recompose** l'appartenance du
champ. Le champ est d'abord retiré de tous les Groupes existants,
puis ajouté aux Groupes mentionnés dans la liste.

```owl
// Au départ BTN_Cible n'est dans aucun Groupe
BTN_Cible..Groupe = [GR_Premier]      // ajouté à GR_Premier

BTN_Cible..Groupe = [GR_Second]       // retiré de GR_Premier,
                                      // ajouté à GR_Second

BTN_Cible..Groupe = []                // retiré de tous les Groupes
```

## Manipulation manuelle

Un Groupe se comporte comme un `tableau de Champ` : les fonctions
tableau standard s'appliquent.

| Action | Syntaxe |
|---|---|
| Compter les membres | `GR_X..Occurrence` |
| Tester si vide | `GR_X..Vide` |
| Ajouter un membre | `Ajoute(GR_X, monChamp)` |
| Retirer un membre | `Supprime(GR_X, nIndice)` |
| Vider entièrement | `SupprimeTout(GR_X)` |
| Chercher un membre | `nIndice est un entier = Cherche(GR_X, monChamp)` |
| Itérer | `POUR TOUT champ DE GR_X FAIRE ... FIN` |

```owl
// Retirer un bouton d'un Groupe par recherche
nIdx est un entier = Cherche(GR_Boutons, BTN_Obsolete)
SI nIdx > 0 ALORS
    Supprime(GR_Boutons, nIdx)
FIN
```

## Propriétés broadcast les plus utiles

| Propriété | Effet sur les membres compatibles |
|---|---|
| `Visible`  | Affiche / masque |
| `Grisé`    | Active / désactive (avec rendu grisé) |
| `EnSaisie` | Autorise / interdit la saisie utilisateur (Saisies) |
| `Valeur`   | Initialise / efface le contenu (Saisies) |

Toute propriété déclarée sur un type de champ peut être diffusée.
Les membres qui ne l'exposent pas sont silencieusement ignorés
(cf. § Broadcast).

## Points d'attention

- **Références aux champs.** Un Groupe stocke des références : si un
  champ est détruit (fermeture de fenêtre), ses références dans les
  Groupes deviennent invalides. Un Groupe survit à la fenêtre qui le
  déclare uniquement si on en conserve une référence explicite.
- **Broadcast non récursif sur les conteneurs.** Un Groupe contenant
  un Conteneur diffuse la propriété au Conteneur lui-même, pas à ses
  enfants. Pour atteindre les enfants, les ajouter explicitement au
  Groupe.
- **Lecture d'une propriété sur un Groupe entier.** L'écriture
  `GR_X..Prop = v` diffuse à tous les membres ; en lecture, un Groupe
  n'a pas de "valeur unique" agrégée. Pour lire, itérer ou indexer
  (`GR_X[1]..Valeur`).

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Cycle de vie des champs | `15_lifecycle_champs.md` |
| Conventions de code | `10_conventions_de_code.md` |
| Préfixes de nommage (`GR_`, `BTN_`, `SAI_`…) | `12_prefixes_de_nommage.md` |

Pour la liste des propriétés exposées par un type de champ :
`owl explain <Type>`.
