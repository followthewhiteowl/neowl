# Markdown fixture

Ce fichier ne doit JAMAIS etre touche par `owl fmt`. Le formatter ne
sait traiter que des fichiers `.owl` et appliquer ses regles de casse
canonique a du markdown casse le contenu (cas reel : CLAUDE.md
massacre, restored from git).

Le self-test `--self-test-cli-ops` charge ce fichier, lance
`owl fmt <ce_fichier>`, et verifie :
  - exit code 64 (option/usage)
  - contenu identique apres l'appel

Ne PAS modifier ce fichier sans mettre a jour le test associe.

## Quelques formes que owl fmt aurait massacrees

- `trace`, `Trace`, `TRACE` (casse aleatoire en markdown)
- `si`, `SINON` (mots-cles en prose)
- `iPDFGenerique` (formes canon volees au registry)
