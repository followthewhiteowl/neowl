# Indirection

L'indirection désigne un élément — variable globale, champ, fenêtre — par un
**nom calculé à l'exécution** plutôt qu'écrit en dur. Le nom, une expression
chaîne, se place entre accolades : `{ expression }`. La forme fonctionne en
lecture comme en écriture. Les exemples de ce chapitre sont vérifiés sur le
binaire ; ceux qui manipulent une fenêtre ou un champ supposent l'élément
correspondant déclaré dans un projet.

## Principe

Une accolade contenant une chaîne résout la variable globale portant ce nom :

```owl
score est un entier = 42

Trace({"score"})            // 42   — lecture par nom
{"score"} = 100             // écriture par nom
Trace(score)                // 100
```

Le nom peut venir d'une variable, donc être décidé à l'exécution :

```owl
score est un entier = 42
nom est une chaîne = "score"

Trace({nom})                // 42   — nom calculé
```

Un nom qui ne correspond à aucun élément connu déclenche une erreur à
l'exécution, rattrapable par un bloc `QUAND EXCEPTION` :

```owl
QUAND EXCEPTION DANS
    Trace({"absente"})      // nom introuvable
FAIRE
    Trace("nom introuvable")
FIN
```

## Indirection sur un champ de structure

Préfixée par une instance suivie d'un point, l'accolade résout un membre par
son nom. La lecture et l'écriture suivent les mêmes règles :

```owl
Pers est une Structure
    nom est une chaîne
    age est un entier
FIN

p est un Pers
p.{"nom"} = "Jeff"          // écriture du membre par nom
Trace(p.{"nom"})            // Jeff
```

Un membre absent déclenche une erreur, comme pour une variable introuvable.

## Chaînage de propriété

L'accolade se chaîne avec `..` pour lire ou écrire une propriété de l'élément
résolu. C'est le moyen d'atteindre une fenêtre dont le nom est calculé :

```owl
sNomFenetre est une chaîne = "FEN_Demo"
Ouvre(FEN_Demo)

n est un entier = {sNomFenetre, indWindow}..Largeur
```

## Scope typé

Une accolade admet un second terme optionnel, une constante de **scope**, qui
dirige la résolution vers une famille d'éléments précise :

| Constante | Résolution |
|---|---|
| `indWindow` / `indFenêtre` | une fenêtre désignée par son nom |
| `indControl` / `indChamp` | un champ désigné par son nom |
| `indVariable` | une variable globale |

```owl
sChamp est une chaîne = "LIB_D"
Trace({sChamp, indChamp}..Libellé)      // libellé du champ résolu
Trace({"score", indVariable})           // valeur de la variable globale
```

Sans scope, `{ nom }` résout une variable globale ; le scope explicite lève
l'ambiguïté et valide la famille attendue.

## Portée

La résolution d'une variable cible les **variables globales** ; le second
terme optionnel se limite aux constantes de scope du tableau ci-dessus.

## Pour aller plus loin

| Sujet | Référence |
|---|---|
| Opérateurs et accès (`.`, `..`) | [`05_operateurs.md`](05_operateurs.md) |
| Types et structures | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Null et valeurs absentes | [`02_null_et_nullable.md`](02_null_et_nullable.md) |
