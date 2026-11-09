#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#  — non-regression segfault au teardown d'OP_INVOKE apres erreur
# runtime levee depuis une native-METHODE re-entrante (vm_call_reentrant).
# Repro : un formateur de Logger qui leve une div/0, declenche via la forme
# methode `log.Erreur(...)`. Pre-fix : exit 139 (SIGSEGV). Post-fix : exit 70,
# message "Division par zero" + pile d'appels, pas de "Segmentation fault".
#
# Jumeau de run_segfault_unwind_test.sh (, qui couvre OP_CALL via
# call_value). Celui-ci couvre OP_INVOKE, le chemin que  ne touchait
# pas. Meme doctrine : PREUVE DE PRESENCE avant de conclure a l'absence de
# crash (la fixture compile, le top-level tourne, la native re-entrante
# rappelle du bytecode utilisateur, l'erreur est levee ET propagee).
#
# Usage: scripts/run_opinvoke_unwind_test.sh [path/to/owl.exe]

set -u

OWL_EXE="${1:-./build/owl.exe}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE="$DIR/tests/compiler/internal_fixtures/opinvoke_unwind_native/crash.owl"

MARK_TOPLEVEL="opinvoke_unwind: top-level atteint"
MARK_REENTRANT="opinvoke_unwind: formateur entre"
MARK_UNREACHABLE="opinvoke_unwind: NE DOIT PAS SORTIR"

if [[ ! -x "$OWL_EXE" ]]; then
    echo "ERROR: owl exe not found: $OWL_EXE" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "ERROR: fixture not found: $FIXTURE" >&2
    exit 2
fi

OWL_EXE="$(cd "$(dirname "$OWL_EXE")" && pwd)/$(basename "$OWL_EXE")"

# Augment PATH for Qt6 DLLs (idem run_segfault_unwind_test.sh).
if [[ -d "/c/msys64/ucrt64/bin" ]]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

fail=0

# Preuve 0 : la fixture compile.
check_out=$("$OWL_EXE" --check "$FIXTURE" 2>&1)
check_code=$?
if [[ $check_code -ne 0 ]] || echo "$check_out" | grep -q '"message"'; then
    echo "FAIL opinvoke_unwind: la fixture ne compile plus (owl --check exit $check_code)" >&2
    echo "$check_out" | head -5 >&2
    fail=1
fi

# Capture merged stdout+stderr + exit code.
output=$("$OWL_EXE" "$FIXTURE" 2>&1)
exit_code=$?

# Preuve 1 : le programme a demarre.
if ! echo "$output" | grep -qF "$MARK_TOPLEVEL"; then
    echo "FAIL opinvoke_unwind: marqueur top-level absent — le programme n'a pas demarre." >&2
    fail=1
fi

# Preuve 2 : la native-methode re-entrante a rappele le formateur utilisateur
# (log.Erreur -> vm_call_reentrant -> fmtQuiLeve). Sans ca, OP_INVOKE n'est pas
# exerce, meme si le programme tourne.
if ! echo "$output" | grep -qF "$MARK_REENTRANT"; then
    echo "FAIL opinvoke_unwind: le formateur n'a pas ete rappele — scenario OP_INVOKE non exerce." >&2
    fail=1
fi

# Preuve 3 : l'erreur runtime a bien ete levee.
if ! echo "$output" | grep -q "Division par z"; then
    echo "FAIL opinvoke_unwind: message 'Division par zero' absent — l'erreur n'a pas ete levee." >&2
    fail=1
fi

# Preuve 4 : l'erreur provient du callback re-entrant (pile nomme le formateur).
if ! echo "$output" | grep -q "fmtQuiLeve()"; then
    echo "FAIL opinvoke_unwind: la pile d'appels ne mentionne pas fmtQuiLeve()." >&2
    fail=1
fi

# Preuve 5 : l'erreur a INTERROMPU le top-level (unwind effectif).
if echo "$output" | grep -qF "$MARK_UNREACHABLE"; then
    echo "FAIL opinvoke_unwind: le top-level a continue apres l'erreur — pas d'unwind." >&2
    fail=1
fi

# Preuve 6 : code de sortie = erreur runtime propagee (70). 139 = SIGSEGV.
if [[ $exit_code -ne 70 ]]; then
    echo "FAIL opinvoke_unwind: exit code $exit_code, attendu 70 (erreur runtime propagee)." >&2
    fail=1
fi
if [[ $exit_code -eq 139 ]]; then
    echo "FAIL opinvoke_unwind: exit code 139 (SIGSEGV)" >&2
    fail=1
fi
if echo "$output" | grep -q "Segmentation fault"; then
    echo "FAIL opinvoke_unwind: 'Segmentation fault' detecte dans output" >&2
    fail=1
fi

if [[ $fail -eq 0 ]]; then
    echo "opinvoke_unwind: PASS (6 preuves de presence verifiees, exit $exit_code, erreur levee depuis le formateur re-entrant via OP_INVOKE, no crash)"
    exit 0
else
    echo "" >&2
    echo "Full output captured:" >&2
    echo "$output" | head -30 >&2
    echo "Exit code: $exit_code" >&2
    exit 1
fi
