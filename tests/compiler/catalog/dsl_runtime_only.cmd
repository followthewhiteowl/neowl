#!/usr/bin/env bash
# Verifie le flag "runtime_only" expose dans la section dsl_properties de
# --vscode-data. Les proprietes d'etat runtime (position de curseur, valeur
# live d'une selection, nombre d'elements, etc.) sont lues/ecrites a
# l'execution et n'ont aucune semantique de design : elles portent
# "runtime_only":true pour que le designer les exclue de son inspector. Les
# proprietes de configuration (ValeurInitiale, MaxLongueur, Contenu, ...) ne
# le portent PAS.
#
# On extrait toutes les props flaggees sous la forme <kind>.<nameEN>, triees.
# Une prop de config marquee par erreur ajoute une ligne inattendue ; une
# prop d'etat oubliee en retire une -> le diff casse dans les deux sens.
grep -oE '\{"kind":"[^{}]*"runtime_only":true[^{}]*\}' \
| while IFS= read -r obj; do
    kind=$(printf '%s' "$obj" | grep -oE '"kind":"[^"]+"'   | sed -E 's/.*:"([^"]+)"/\1/')
    en=$(printf '%s'   "$obj" | grep -oE '"nameEN":"[^"]+"' | sed -E 's/.*:"([^"]+)"/\1/')
    printf '%s.%s\n' "$kind" "$en"
  done | sort -u
