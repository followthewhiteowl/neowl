#!/usr/bin/env bash
#  — chaque entree widget_kinds de `owl --vscode-data` porte son
# prefixe de nommage canon (.canonicalPrefix du registre src/ui_decl.c,
#  : "TABLE_", "BTN_", "SAI_"...). Source unique cote owl ; le
# designer (owl-integration) le consomme au lieu de recopier le canon a la
# main (drift ferme). Les sortes sans convention (CodeBarres, Onglet...)
# portent "prefix":null.
data=$(cat)
kinds=$(printf '%s' "$data" | grep -oE '\{"nameFR":"[^{]*\}')

extract() {
    printf '%s' "$kinds" | grep -F "\"nameFR\":\"$1\"," | grep -oE '"prefix":("[^"]*"|null)'
}

for k in Table Bouton Saisie Liste CodeBarres Onglet; do
    val=$(extract "$k")
    echo "$k: ${val:-ABSENT}"
done
