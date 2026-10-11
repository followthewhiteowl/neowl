#!/usr/bin/env bash
# Liste les méthodes CANONIQUES (non-alias) sur MockNative.
# Le LSP utilise cette liste pour proposer en autocomplétion (pas les aliases EN).
# Format: {"type":"MockNative","name":"Quand","arity":1}  (sans le flag isAlias car
# --vscode-data ne l'expose pas; c'est lsp_catalog_gen.c qui le fait).
# Ici on se contente de compter: on DOIT avoir au moins 8 méthodes canoniques.
grep -oE '\{"type":"MockNative","name":"[A-Za-z]+","arity":-?[0-9]+\}' \
  | sort -u \
  | grep -v 'Activate\|With\|Returns\|When\|Deactivate\|Reset\|VerifyCount\|ReturnsSequence'
