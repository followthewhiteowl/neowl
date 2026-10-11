#!/usr/bin/env bash
#  — la wordlist TextMate `constant.language.owl` doit CONSERVER les
# variantes SANS-ACCENT de l'enum couleur (GrisFonce, RougeFonce,
# CouleurDefaut...). Ces alias, presents dans `.values` -> `constants_runtime`
# de --vscode-data, sont accent-sensibles cote coloration : ils colorent la
# forme sans-accent tapee AVANT restauration par le formatter. Le dedup
# d'affichage de `owl explain` est purement local au rendu explain
# et NE DOIT PAS toucher ce calque. On isole le tableau constants_runtime et on
# compte l'occurrence exacte de "GrisFonce" : si le dedup fuit vers ce calque,
# le compte tombe a 0 et la coloration casse.
grep '"constants_runtime"' \
  | sed -n 's/.*"constants_runtime": \[\([^]]*\)\].*/\1/p' \
  | grep -o '"GrisFonce"' | wc -l | tr -d ' '
