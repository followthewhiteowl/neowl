# Thèmes et styles nommés

Un **thème** regroupe des styles nommés, typés et réutilisables dans un bloc
`est un Thème` (forme anglaise `is a Theme`). Un projet en désigne un ; chaque
champ y puise son apparence via la propriété `Style`. Un style ne redéfinit que
ce qui doit changer — le reste garde la valeur par défaut du champ, si bien
qu'ajouter une propriété plus tard ne casse aucun thème existant.

## Déclarer un thème

Le bloc porte le nom du thème et enchaîne des styles nommés jusqu'à `FIN` /
`END`. Un seul bloc `est un Thème` est autorisé par fichier ; par convention il
vit dans un fichier `theme.owl` à la racine du projet.

```owl
ThèmeDémo est un Thème
	boutonPrimaire est un styleBouton = {
		CadreExtérieur = { CouleurFond = Bleu }
		Libellé        = { Couleur = Blanc }
	}

	boutonDanger est un styleBouton = {
		CadreExtérieur = { CouleurFond = Rouge }
		Libellé        = { Couleur = Blanc }
	}

	saisieCompacte est un styleSaisie = {
		ZoneSaisie = { CouleurFond = JauneClair }
	}
FIN
```

Chaque entrée déclare une variable typée par un **descripteur de style** et
initialisée par un littéral `{ ... }` (cf. `17_init_litterale.md`).

## Les descripteurs de style

À chaque type de champ stylable correspond un descripteur. Les plus courants,
avec leur forme anglaise (les deux formes sont acceptées) :

| Descripteur (FR) | Forme EN | Champ visé | Sous-éléments principaux |
|---|---|---|---|
| `styleBouton` | `styleButton` | Bouton | `Libellé`, `Image`, `CadreExtérieur` |
| `styleSaisie` | `styleInput` | Saisie | `ZoneSaisie`, `Libellé`, `CadreExtérieur` |
| `styleLibellé` | `styleLabel` | Libellé | `Libellé`, `CadreExtérieur` |
| `styleConteneur` | `styleContainer` | Conteneur | `CadreExtérieur` |

Les autres champs suivent le même schéma (`styleTable`, `styleCombo`,
`styleListe`, `styleJauge`, `styleInterrupteur`, `styleSélecteur`…).

Chaque sous-élément est lui-même un littéral `{ ... }` :

- `CadreExtérieur` / `OuterFrame` — la boîte : `CouleurFond` / `BackgroundColor`,
  `Cadre` (couleur, épaisseur, arrondi), `Ombre`, `Dégradé`, retraits.
- `Libellé` / `Caption` — le texte : `Couleur` / `Color`, `Police`…
- `ZoneSaisie` / `InputZone` — la zone éditable d'une saisie.

Un style ne mentionne que les sous-éléments et propriétés qu'il change ; tout le
reste conserve sa valeur par défaut (cf. `29_couleurs.md` pour les couleurs).

Pour un champ composé de plusieurs parties, comme le champ Table, une police et une
couleur globales posées sur le champ (`TBL..Police..Taille`, `TBL..CouleurTexte`, …)
s'appliquent comme base du rendu de toutes les parties (cellules et en-têtes). Le
style détaillé surcharge ensuite cette base par sous-élément — `TitreColonne`
(l'en-tête des colonnes), `LibelléLignes` (le texte des cellules),
`LigneSélectionnée`, etc. Les deux coexistent : la police du champ fixe le défaut, le
sous-élément l'affine. La police d'une partie se pose via son sous-élément :

```owl
tableClients est un styleTable = {
	TitreColonne     = { Police = { Nom = "Arial", Taille = 11, Gras = Vrai } }
	LibelléLignes    = { Police = { Taille = 10 }, Couleur = GrisFoncé }
	LigneSélectionnée = { Couleur = Blanc }
}
```

Une Colonne peut surcharger localement ces mêmes sous-éléments via son propre
`styleColonne` (cf. `owl explain styleColonne`).

### Surcharges par état

Un sous-élément interactif accepte un membre `Etats` qui redéfinit l'apparence
selon l'état du champ : `Survol`, `Appuyé`, `Focus`, `Grisé`, `LectureSeule`
(les états disponibles dépendent du champ).

```owl
boutonPrimaire est un styleBouton = {
	CadreExtérieur = {
		CouleurFond = Bleu
		Etats = {
			Survol = { CouleurFond = BleuClair }
			Appuyé = { CouleurFond = BleuFoncé }
		}
	}
	Libellé = { Couleur = Blanc }
}
```

## Appliquer un style à un champ

Dans les `PROPRIETES` d'un champ, la propriété `Style` reçoit le style nommé via
le préfixe `theme.` :

```owl
BTN_Valider est un Champ Bouton
	PROPRIETES
		Libellé = "Valider"
		Style   = theme.boutonPrimaire
	FIN
FIN
```

Le préfixe `theme.` désigne le thème actif du projet (celui qu'il a rattaché,
ci-dessous). Le champ hérite alors de toutes les propriétés du style.

## Styliser une fenêtre

Une fenêtre se style comme un champ : elle porte une propriété `Style` typée
`styleConteneur` (forme anglaise `styleContainer`), le même descripteur que le
Conteneur. Ce descripteur n'a qu'un sous-élément, `CadreExtérieur` /
`OuterFrame`, qui décrit le fond de la fenêtre : `CouleurFond`, retraits
(`RetraitHaut`, `RetraitBas`, `RetraitGauche`, `RetraitDroit`), `Cadre`,
`Ombre` et `Dégradé`.

Le bloc `Style` s'écrit dans les `PROPRIETES` de la fenêtre, sous forme de
littéral `{ ... }`, et ne mentionne que ce qui change :

```owl
FEN_Accueil est une Fenêtre
	PROPRIETES
		Titre = "Accueil"
		Style = {
			CadreExtérieur = {
				CouleurFond = GrisClair
				RetraitHaut = 12
				Cadre = { Couleur = GrisFoncé, Épaisseur = 1 }
			}
		}
	FIN
FIN
```

La fenêtre peut aussi puiser son apparence dans un style nommé du thème :
comme tout champ, sa propriété `Style` accepte `theme.<nom>` pointant un style
typé `styleConteneur` déclaré dans le bloc thème.

## Rattacher le thème au projet

Un projet désigne son thème actif par la propriété `Thème` (forme anglaise
`Theme`) de son bloc `est un Projet`, en pointant le **nom du bloc thème** :

```owl
MonAppThémée est un Projet
	PROPRIETES
		Description = "Démo thème"
		Langues = [langueFrançais]
		Thème = ThèmeDémo
	FIN
FIN
```

Le `theme.owl` déclare `ThèmeDémo est un Thème`, le `projet.owl` le désigne, et
tous les champs qui écrivent `Style = theme.<nom>` reçoivent leur apparence. Cf.
`24_projet.md` pour le bloc projet.

## Générer un squelette

`owl new theme <Nom>` émet un thème d'exemple prêt à compléter :

```
owl new theme MonThème
```

Dans VS Code, la commande **« Nouvel élément »** propose une carte **Thème** qui
fait la même chose et ouvre le fichier créé.

## Limites actuelles

- **Un seul bloc `est un Thème` par fichier.** Un second bloc dans le même
  fichier est refusé à la compilation.
- Un thème n'est **actif** que s'il est désigné par le projet (`Thème = <nom>`).
  Un bloc thème présent mais non désigné compile sans erreur ; ses styles ne
  s'appliquent simplement pas.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Couleurs et transparence | `29_couleurs.md` |
| Initialisation littérale `{ ... }` | `17_init_litterale.md` |
| Cycle de vie des champs | `15_lifecycle_champs.md` |
| Le bloc projet | `24_projet.md` |
