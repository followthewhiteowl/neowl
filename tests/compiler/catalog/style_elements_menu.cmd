#!/usr/bin/env bash
#  — Les racines de menu (MenuPrincipal / MenuContextuel), bien que
# NON materialisees en widget, exposent leur structure de style dans
# --vscode-data EXACTEMENT comme un widget : entree style_elements (sous-
# elements derives du TYPE de style) + etats survol des surfaces stateful
# (element_states_by_type). Ce test verrouille la parite de forme avec un
# widget (ex styleSaisie) : structure + feuilles derivees du registre, etats
# des surfaces stateful exposes.
data=$(cat)

# (1) style_elements : sous-elements de chaque racine de menu. Le MenuPrincipal
#     porte Barre + SousMenu + OptionMenu ; le MenuContextuel n'a PAS de Barre.
mp=$(printf '%s' "$data" | grep -oE '"styleMenuPrincipal": \[[^]]*\]')
mc=$(printf '%s' "$data" | grep -oE '"styleMenuContextuel": \[[^]]*\]')
for sub in Barre SousMenu OptionMenu; do
    if printf '%s' "$mp" | grep -q "\"$sub\""; then echo "principal.$sub: oui"; else echo "principal.$sub: NON"; fi
done
for sub in Barre SousMenu OptionMenu; do
    if printf '%s' "$mc" | grep -q "\"$sub\""; then echo "contextuel.$sub: oui"; else echo "contextuel.$sub: NON"; fi
done

# (2) element_states_by_type : etat survol sur les surfaces stateful du menu
#     (Barre + OptionMenu). Le SousMenu (panneau statique) ne porte pas d'etat.
for t in styleMenuPrincipalBarre styleMenuPrincipalOption styleMenuPrincipalSousMenu; do
    line=$(printf '%s' "$data" | grep -oE "\"$t\": \[[^]]*\]")
    if printf '%s' "$line" | grep -q '"fr":"survol"'; then
        echo "$t: survol"
    elif [ -n "$line" ]; then
        echo "$t: aucun-etat"
    else
        echo "$t: ABSENT"
    fi
done

# (3) menu_style_roots : mapping racine de menu -> type de style (source de
#     verite = accesseur couche 2 owl_presentation_menu_style_root_*). Les
#     racines ne sont pas des kinds DSL (pas de prop Style), donc ce mapping
#     est la SEULE facon pour le front de router une racine vers son type de
#     style sans rien hardcoder.
roots=$(printf '%s' "$data" | grep -oE '"menu_style_roots": \{[^}]*\}')
for pair in "MenuPrincipal:styleMenuPrincipal" "MenuContextuel:styleMenuContextuel"; do
    root="${pair%%:*}"; styp="${pair##*:}"
    if printf '%s' "$roots" | grep -q "\"$root\": \"$styp\""; then
        echo "root.$root: $styp"
    else
        echo "root.$root: NON"
    fi
done
