#!/usr/bin/env bash
# Verifie que owl --vscode-data exporte les mots-cles de declaration et de
# visibilite avec leur forme canon ACCENTUEE (FR). Bug 0156 : l'export ne
# sortait que les formes d'entree ASCII (enumeration, prive) -> la grammar
# VSCode, insensible a la casse mais PAS aux accents, ne pouvait pas colorer
# Enumeration / PRIVE / PROTEGE accentues.
# Les formes EN (ASCII) sont deja exportees et hors-scope de ce ticket.
# La visibilite vit dans le bucket keywords_visibility, la declaration dans
# keywords_declaration : on couvre les deux.
grep -oE '"keywords_(declaration|visibility)": \[[^]]*\]' \
  | grep -oE '"[^"]+"' \
  | tr -d '"' \
  | grep -E '^(Énumération|PRIVÉ|PROTÉGÉ)$' \
  | sort -u
