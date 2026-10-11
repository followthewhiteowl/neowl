# Corpus fmt-stability

Verrou de stabilité du formateur. Chaque cas est un fichier d'entrée formaté
en mémoire par `owl_format`, comparé octet à octet (modulo fin de ligne) à son
snapshot. Exécuté par `owl --self-test-fmt-stability` et par
`owl-lsp-test.exe --self-test-fmt-stability` (parité des deux backends de
canon), tous deux branchés sur `make all`.

Harnais : `src/internal_tests/fmt_stability_test.c`.

## Le rôle d'un fichier est dans son nom

Trois suffixes, trois rôles. Aucune liste out-of-band, aucun marqueur caché :
lister le dossier suffit à savoir ce qui est testé et ce qui ne l'est pas.

| Nom | Rôle |
|---|---|
| `<cas>.owl` | Entrée du cas. **Doit** avoir son `<cas>.expected.owl`. |
| `<cas>.expected.owl` | Résultat attendu. **Doit** avoir son `<cas>.owl`. |
| `<cas>.companion.owl` | Fichier de contexte : n'est pas un cas, fournit seulement des symboles au voisinage. Jamais formaté, jamais comparé. |
| `projet.owl` | Marqueur de frontière de projet, dans les sous-dossiers cross-fichier. Non formaté (sa DSL est un chantier à part). |

Les trois écarts possibles sont des **échecs**, chacun avec son propre mot :

- `MANQUE` — une entrée sans snapshot. Soit on pose le snapshot, soit le
  fichier n'était pas un cas et se renomme en `.companion.owl`.
- `ORPHELIN` — un snapshot sans entrée. Il n'est apparié à rien, donc jamais
  formaté : il simule une couverture. Écrire l'entrée, ou le supprimer.
- `CORPUS VIDE` — aucun cas apparié. Un contrôle qui n'examine rien ne prouve
  rien : « 0 échec » sur un corpus vide n'est pas un succès.

Le bilan affiche toujours le nombre de cas réellement exécutés et le nombre de
compagnons déclarés.

## Deux formes de cas

- **Intra-fichier** : à la racine du dossier. Le fichier est formaté seul.
- **Cross-fichier** : dans un sous-dossier portant un `projet.owl`. Le contexte
  de formatage est construit à partir des autres fichiers du sous-dossier —
  c'est là que les `.companion.owl` servent : ils déclarent les symboles dont
  l'entrée teste la canonisation.

## Fins de ligne

Les snapshots sont versionnés en LF ; la comparaison de contenu normalise
CRLF → LF des deux côtés. Quelques entrées sont volontairement verrouillées en
CRLF via `.gitattributes` (le défaut qu'elles couvrent n'existe que si le `\r`
traîne en fin de ligne). La préservation de la fin de ligne, elle, est couverte
à part par la section `fmt-eol-parity` du harnais, qui travaille en mémoire et
en octets bruts.
