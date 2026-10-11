# Texte riche

Un dialecte de Markdown étendu (couleurs, taille de police,
alignement) est interprété sur les libellés de fenêtres et d'États.
Il permet de mettre en forme des chaînes sans piloter manuellement
les propriétés Police / Couleur des champs.

## Activation

Le texte riche n'est interprété que sur les champs dont la prop
`FormatAffichage` vaut `formatTexteRiche` :

```owl
LIB_X est un Libellé
    PROPRIETES
        Libellé         = "**Important** : [c:RougeClair]erreur[/c]"
        FormatAffichage = formatTexteRiche
    FIN
FIN
```

Défaut : `formatTexte` (texte littéral). Pas de surprise pour les
chaînes ordinaires (`Titre` de fenêtre, raccourcis clavier, etc.) qui
restent littérales.

Champs qui supportent la prop :

- `Libellé` (Fenêtre)
- `Saisie` multi-ligne (`TypeSaisie = typSaisieMultiligne`) — en
  affichage uniquement
- `Libellé` d'État (édition PDF)

## Inline (fragments)

Le sous-ensemble Markdown CommonMark est repris tel quel :

| Effet | Syntaxe |
|---|---|
| Gras | `**…**` |
| Italique | `*…*` ou `_…_` |
| Souligné | `__…__` |
| Barré | `~~…~~` |
| Code mono | `` `…` `` |
| Lien | `[texte](url)` |

**Imbrication libre** : `**[c:rouge]gras rouge[/c]**` produit
gras + couleur.

## Extensions au Markdown

Tags `[tag:val]…[/tag]` — la syntaxe est choisie pour ne pas entrer en
conflit avec les liens Markdown `[texte](url)`.

| Tag | Effet | Exemple |
|---|---|---|
| `c` | Couleur de texte | `[c:RougeFoncé]erreur[/c]` |
| `cf` | Couleur de fond | `[cf:JauneBulle]highlight[/cf]` |
| `s` | Taille de police (pt) | `[s:14]gros[/s]` |
| `p` | Nom de police | `[p:iPoliceArial]…[/p]` |
| `esp` | Espacement entre lettres (px) | `[esp:2]ESPACÉ[/esp]` |
| `j` | Alignement du paragraphe | `[j:justifie]…[/j]` |

### Couleurs : 3 syntaxes acceptées

```
[c:RougeFoncé]      → constante de couleur nommée
[c:#FF6600]         → hex direct, 6 chiffres
[c:#F60]            → hex direct court, 3 chiffres (étendu en #FF6600)
```

### Modes d'alignement (`j`)

| FR | EN |
|---|---|
| `gauche` | `left` |
| `droite` | `right` |
| `centre` | `center` |
| `justifie` | `justified`, `justify` |

## Blocs (paragraphes)

Repris du Markdown standard :

| Construction | Syntaxe |
|---|---|
| Liste à puces | lignes `- …` |
| Liste numérotée | lignes `1. …` |
| Tableaux | GitHub-Flavored Markdown (pipes `\|`) |
| Citation | lignes `> …` |
| Bloc de code | clôture par triple backtick |

## Échappement

`\[` et `\]` pour caractères littéraux. Les autres caractères Markdown
suivent l'échappement CommonMark standard (`\*`, `\_`, etc.).

## Tag invalide → littéral

Si un tag n'est pas reconnu (ex : `[truc:bidule]`) ou si la valeur ne
résout pas (ex : `[c:CouleurInexistante]`), le préprocesseur **émet le
texte tel quel** sans échec ni avertissement.

## Points d'attention

- **Imbrication d'un même tag à plusieurs niveaux** : un tag refermé
  cible toujours la dernière ouverture du **même** tag.
  `[c:rouge]A [c:bleu]B[/c] C[/c]` traite la première fermeture comme
  fermant `[c:bleu]`, pas `[c:rouge]`. Un seul niveau d'imbrication
  pour un tag donné suffit en pratique.
- **Couleur via `RVB(r, g, b)` dans `[c:…]`** : utiliser la forme hex
  (`[c:#FF6600]`) ou une constante de couleur nommée.
- **Hex `[c:#…]`** : case-sensitive — `#FF6600` doit être en
  majuscules dans le source.
- **Saisie multi-ligne** : le texte riche est rendu en affichage ;
  l'édition WYSIWYG (toolbar **B**/*I*/__U__ contextuel) sera ajoutée
  ultérieurement.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Cycle de vie des champs | `15_lifecycle_champs.md` |
| Cycle de vie des États | `16_lifecycle_etats.md` |
| Conventions de code | `10_conventions_de_code.md` |
| Préfixes de nommage (`LIB_`, `SAI_`…) | `12_prefixes_de_nommage.md` |
