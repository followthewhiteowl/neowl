#!/usr/bin/env bash
# Verifie que les natives UI critiques sont bien exposees au plugin
# VSCode via --vscode-data quand on utilise owl.exe. Sans ca, les
# noms 'Ferme', 'Ouvre', etc. ne sont pas dans le pattern TextMate et
# perdent leur coloration de fonction native (regression 2026-04-28).
python -c "
import json, sys
data = json.load(sys.stdin)
rt = set(data.get('natives_runtime', []))
required = ['Ferme', 'Close', 'Ouvre', 'Open', 'OuvreFille', 'OpenChild', 'FinProgramme', 'EndProgram', 'Info', 'Erreur', 'Avertissement', 'Confirmer', 'OuiNon']
for n in sorted(required):
    print(n, 'OK' if n in rt else 'MISSING')
"
