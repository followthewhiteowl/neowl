#!/usr/bin/env bash
# Liste les types builtin structurés exposés au plugin VSCode.
# Extrait la ligne "builtin_types_runtime": [...], tokenize et trie.
grep '"builtin_types_runtime":' \
  | sed -E 's/.*"builtin_types_runtime": \[([^]]+)\].*/\1/' \
  | tr ',' '\n' \
  | sed -E 's/"([^"]+)".*/\1/; s/^ *//' \
  | sort -u
