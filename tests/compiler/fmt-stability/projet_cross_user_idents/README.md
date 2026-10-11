# Cas cross-fichier : canonisation des identifiants utilisateur

Le formateur canonise les identifiants déclarés dans un **autre fichier du
même projet** : la graphie de déclaration fait autorité sur les graphies
d'usage.

- `projet.owl` — marqueur de frontière de projet, non formaté.
- `declaration.companion.owl` — compagnon : déclare `cClient`, `sNomClient`,
  `nMontant`, `CalculerTotal` sous leur graphie canon. N'est pas un cas.
- `usage.owl` / `usage.expected.owl` — le cas : les mêmes identifiants
  employés avec des casses divergentes, et le résultat attendu.

Exécuté par `owl --self-test-fmt-stability`, qui construit lui-même le contexte
de formatage à partir des fichiers voisins. Convention des suffixes : voir le
`README.md` du dossier parent.
