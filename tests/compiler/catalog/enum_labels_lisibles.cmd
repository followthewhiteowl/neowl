#!/usr/bin/env bash
# Verifie que les enums exposes dans l'inspecteur du designer portent des
# libelles d'affichage FR/EN lisibles, derives du nom de constante par le
# transformateur backend (retrait du prefixe commun de l'enum + mise en forme
# camelCase). Les cas ou l'heuristique echoue passent par une table d'overrides.
#
# On echantillonne quatre entrees de presentation (une ligne JSON auto-portee
# chacune) et on rend, par entree, la cle <kind>.<name> suivie de ses libelles
# FR puis EN. Un enum dont AUCUNE valeur ne gagne a la mise en forme (toutes
# deja lisibles a l'identique) n'emet PAS de libelles : le front retombe sur la
# valeur brute -> ligne "<kind>.<name> NO_LABELS". Des qu'une seule valeur gagne
# (ex AffichageSeulement -> "Affichage seulement"), le tableau complet est emis,
# les valeurs deja lisibles y reprenant leur forme brute (alignement parallele).
#
# Format de sortie (une entree par bloc, ordre fige) :
#   <kind>.<name>
#   FR:<lib1>|<lib2>|...
#   EN:<lib1>|<lib2>|...
# ou
#   <kind>.<name>
#   NO_LABELS

emit_entry() {
  local kind="$1" name="$2" data="$3"
  local line
  line=$(printf '%s' "$data" | grep -oE "\\{\"kind\":\"$kind\",\"name\":\"$name\",[^}]*\"editor\"[^}]*\\}" | head -1)
  printf '%s.%s\n' "$kind" "$name"
  if printf '%s' "$line" | grep -q '"enum_subset_labels_fr"'; then
    local fr en
    fr=$(printf '%s' "$line" | grep -oE '"enum_subset_labels_fr":\[[^]]*\]' \
         | sed -E 's/"enum_subset_labels_fr":\[//; s/\]//; s/","/|/g; s/"//g')
    en=$(printf '%s' "$line" | grep -oE '"enum_subset_labels_en":\[[^]]*\]' \
         | sed -E 's/"enum_subset_labels_en":\[//; s/\]//; s/","/|/g; s/"//g')
    printf 'FR:%s\n' "$fr"
    printf 'EN:%s\n' "$en"
  else
    printf 'NO_LABELS\n'
  fi
}

DATA=$(cat)

# Alignement (Bouton) : prefixe derive "align" -> Debut/Centre/Fin/Etire.
emit_entry "Bouton" "Alignement" "$DATA"
# TypeSaisie (Saisie) : prefixe derive "typSaisie" + camelCase multi-mot
# (typSaisieMotDePasse -> "Mot de passe").
emit_entry "Saisie" "TypeSaisie" "$DATA"
# TypeBouton (Bouton) : override (EN irregulier Interrupt/Abort/Help).
emit_entry "Bouton" "TypeBouton" "$DATA"
# Etat (Saisie) : sous-ensemble Actif/AffichageSeulement/Grisé. Actif et Grisé
# sont deja lisibles (label == valeur), mais AffichageSeulement gagne au split
# camelCase -> le tableau est emis, Actif/Grisé reprenant leur forme brute.
emit_entry "Saisie" "Etat" "$DATA"
