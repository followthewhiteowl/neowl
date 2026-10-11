# Littéraux d'instance `{ ... }` (candidat)

> Fiche candidate. La référence stabilisée vit dans
> `../17_init_litterale.md` ; ce document recense les types qui acceptent
> la forme littérale et sert de support aux exemples
> `examples/inline_literal/`.

La forme `{ champ = valeur, ... }` initialise un type composé sans
déclarer de variable temporaire ni affecter les sous-propriétés une par
une. Elle exige un **type cible explicite** (aucune inférence) :

```owl
maVar est un T = { champ1 = expr, champ2 = expr }
```

## Types acceptant le littéral inline

| Type | Exemple |
|---|---|
| `Cadre` | `{ Couleur = Bleu, Epaisseur = 2, Arrondi = 8 }` |
| `Style` | `{ CouleurFond = Blanc, Police = { Taille = 14 } }` |
| `Police` | `{ Nom = "Inter", Taille = 14, Gras = Vrai }` |
| `Coin` | `{ Largeur = 12, Hauteur = 8 }` |
| `Trait` | `{ Couleur = Rouge, Epaisseur = 3, Type = TraitTiret }` |
| `Couleur` | `{ Rouge = 255, Vert = 128, Bleu = 0, Opacité = 200 }` |
| `durée` | `{ Minute = 2, Seconde = 30 }` (construction par composantes) |

Pour `Couleur`, le littéral est la seule forme de déclaration qui fixe
l'`Opacité` (canal alpha) au moment de la création. Détails dans
`../29_couleurs.md`.

## Récursivité

Les sous-objets d'un type composé acceptent leur propre littéral. Le
sous-type est déduit du nom du champ cible :

```owl
monCadre est un Cadre = {
    Couleur   = BleuFoncé
    TraitBas  = { Couleur = RougeFoncé, Epaisseur = 3, Type = TraitTiret }
}
```

## Limites connues

- **Type cible obligatoire** : le littéral n'est reconnu qu'en déclaration
  typée, en réaffectation d'une variable typée, ou en argument de fonction
  native. Il n'est pas accepté dans un bloc `PROPRIETES` (DSL) ni en
  argument de procédure utilisateur — passer par une variable nommée.
- **Réaffectation `var = { ... }`** : remplace entièrement l'instance
  (les champs non cités reviennent à leur valeur par défaut, pas de
  fusion).
- **`Couleur` imbriquée** : un littéral couleur ne peut pas être imbriqué
  directement dans un autre littéral (`Style = { CadreExtérieur = {
  CouleurFond = { ... } } }`) — déclarer d'abord une variable `Couleur`.

La liste complète des sources de type cible et des points d'attention est
dans `../17_init_litterale.md`.
