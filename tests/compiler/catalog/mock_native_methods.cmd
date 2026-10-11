#!/usr/bin/env bash
# Filtre --vscode-data pour extraire les méthodes enregistrées sur MockNative.
# Les entrées ressemblent à: {"type":"MockNative","name":"Quand","arity":1}
grep -oE '\{"type":"MockNative","name":"[A-Za-z]+","arity":-?[0-9]+\}' | sort -u
