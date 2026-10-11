#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# check_tests_layout.sh — verrou de taxonomie de tests/.
#
# Echoue si un dossier top-level TRACKE non attendu apparait sous tests/. But :
# empecher la re-derive de classification — une feature qui se cree un
# tests/<truc>/ au lieu de se ranger sous un bucket existant. Les familles
# cli_/formatter_/translate_ sont regroupees sous tests/cli, tests/formatter,
# tests/translate (un cran de profondeur).
#
# Pour ajouter LEGITIMEMENT un bucket : l'ajouter a ALLOW ci-dessous (avec une
# raison), pas contourner le verrou.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Buckets top-level autorises sous tests/. Superset : un arbre public (export)
# en voit un sous-ensemble (conformance / _review exclus de l'export) — le
# verrou ne rougit que sur un PRESENT non liste, jamais sur un liste absent.
ALLOW="_review cli compile-error compiler conformance dap doc-lint-fixtures formatter internal_fixtures limits lsp regression runtime runtime_c scenarios translate ui_min_floors"

# Dossiers top-level portant au moins un fichier tracke (NF>2 : exclut les
# fichiers a la racine de tests/, p.ex. tests/README.md).
present="$(git ls-files tests/ 2>/dev/null | awk -F/ 'NF>2 {print $2}' | sort -u)"
if [ -z "$present" ]; then
    # Hors depot git (ex. arbre fraichement exporte avant git init) : repli FS,
    # en ignorant le scratch de build non suivi (_local).
    present="$(find tests -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null \
               | grep -vx '_local' | sort)"
fi

rc=0
for d in $present; do
    case " $ALLOW " in
        *" $d "*) ;;
        *) echo "check_tests_layout: dossier top-level INATTENDU sous tests/ : '$d'"; rc=1 ;;
    esac
done

if [ "$rc" -ne 0 ]; then
    echo "check_tests_layout: ranger sous un bucket existant (cli/formatter/translate/...)"
    echo "                    ou ajouter le dossier a ALLOW (scripts/check_tests_layout.sh) avec justification."
    exit 1
fi
echo "check_tests_layout: OK ($(echo $present | wc -w) buckets top-level, tous autorises)"
exit 0
