# owl-completion.bash — complétion bash pour la CLI owl.
#
# Installation :
#   source /chemin/vers/owl-completion.bash
# ou pour persister :
#   echo "source /chemin/vers/owl-completion.bash" >> ~/.bashrc
#
# Backend : la commande `<owl_bin> __complete <position> <prefix>` dump une
# complétion par ligne. Le script bash ne sait rien des sous-commandes
# d'owl, il délègue tout au binaire — la liste est donc toujours à jour
# avec la version installée.
#
# Le binaire appelé est celui que l'utilisateur a tapé (COMP_WORDS[0]),
# pas un `owl` hardcodé : `./build/owl.exe`, `owl.exe`, `owl` marchent
# tous tant qu'ils résolvent vers un binaire OWL valide.

_owl_complete() {
    local cur position bin
    cur="${COMP_WORDS[COMP_CWORD]}"
    bin="${COMP_WORDS[0]}"

    # COMP_CWORD == 1 : on complète la sous-commande elle-même.
    # COMP_CWORD >= 2 : COMP_WORDS[1] est la sous-commande, on complète son argument.
    if (( COMP_CWORD == 1 )); then
        position=""
    else
        position="${COMP_WORDS[1]}"
    fi

    local IFS=$'\n'
    # shellcheck disable=SC2207
    COMPREPLY=($("$bin" __complete "$position" "$cur" 2>/dev/null))
}

complete -F _owl_complete owl
complete -F _owl_complete owl.exe
