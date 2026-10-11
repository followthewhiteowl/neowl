#!/usr/bin/env bash
# Verifie que la prop FormatAffichage de la Saisie porte une condition de
# visibilite inter-propriete (visible_if). La mise en forme riche n'a d'effet
# qu'en saisie multiligne : le designer ne doit proposer FormatAffichage que si
# TypeSaisie vaut typSaisieMultiligne (masque sur Texte simple, Mot de passe...).
#
# Chaque entree de presentation est une ligne JSON auto-portee. On isole
# l'entree de PRESENTATION Saisie.FormatAffichage : elle est ancree sur
# "labelFR" (immediatement apres "name"), ce qui la distingue de l'entree de
# catalogue semantique du meme kind/name (qui, elle, porte "defaultValue").
#
# Format de sortie (ordre fige) :
#   Saisie.FormatAffichage
#   visible_if.prop:<prop>
#   visible_if.value:<valeur>

DATA=$(cat)
line=$(printf '%s' "$DATA" | grep -E '"kind":"Saisie","name":"FormatAffichage","labelFR"' | head -1)
prop=$(printf '%s' "$line" | grep -oE '"visible_if":\{"prop":"[^"]*"' | sed -E 's/.*"prop":"//; s/"$//')
value=$(printf '%s' "$line" | grep -oE '"value":"[^"]*"\}' | sed -E 's/"value":"//; s/"\}$//')
printf 'Saisie.FormatAffichage\n'
printf 'visible_if.prop:%s\n' "$prop"
printf 'visible_if.value:%s\n' "$value"
