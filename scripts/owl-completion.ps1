# owl-completion.ps1 — complétion PowerShell pour la CLI owl.
#
# Installation :
#   . C:\chemin\vers\owl-completion.ps1
# ou pour persister, ajouter cette ligne au profil :
#   notepad $PROFILE
#   # puis dedans :
#   . C:\chemin\vers\owl-completion.ps1
#
# Backend : la commande `<owl_bin> __complete <position> <prefix>` dump
# une complétion par ligne. Le script PowerShell ne connait pas la liste
# des sous-commandes ; il délègue au binaire que l'utilisateur a tapé
# (CommandElements[0]), pas un `owl` hardcodé. Donc `./build/owl.exe`,
# `owl.exe`, ou un `owl` aliasé fonctionnent tous.

$_owlCompleter = {
    param($wordToComplete, $commandAst, $cursorPosition)

    # CommandElements :
    #   [0] = chemin/nom du binaire owl tel que tapé (ex. `./build/owl.exe`)
    #   [1] = sous-commande si déjà tapée
    #   [2..] = arguments suivants
    $elements = $commandAst.CommandElements
    if ($elements.Count -lt 1) { return }

    $bin = $elements[0].ToString()
    $position = ''

    if ($elements.Count -ge 2) {
        $second = $elements[1].ToString()
        # On est encore en train de compléter la sous-commande → position vide.
        # Sinon, on l'a déjà tapée → on l'utilise comme position.
        if ($elements.Count -ge 3 -or $second -ne $wordToComplete) {
            $position = $second
        }
    }

    # Appel du backend. stderr supprimé : pendant la complétion
    # interactive on ne veut pas spammer le terminal sur une erreur.
    $candidates = & $bin __complete $position $wordToComplete 2>$null
    if ($null -eq $candidates) { return }

    foreach ($c in $candidates) {
        [System.Management.Automation.CompletionResult]::new(
            $c, $c, 'ParameterValue', $c
        )
    }
}

# Enregistre la complétion pour les noms standards d'invocation de owl.
# Pour un chemin de binaire non standard (ex. `./build/owl.exe`),
# l'utilisateur peut soit l'ajouter au PATH, soit créer un alias :
#   Set-Alias owl C:\chemin\vers\owl\build\owl.exe
Register-ArgumentCompleter -Native -CommandName owl     -ScriptBlock $_owlCompleter
Register-ArgumentCompleter -Native -CommandName owl.exe -ScriptBlock $_owlCompleter
