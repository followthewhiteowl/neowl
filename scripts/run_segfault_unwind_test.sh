#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#  — non-regression segfault au teardown apres erreur runtime levee
# depuis une native re-entrante (vm_call_reentrant). Repro : Ouvre(FEN_X)
# depuis le top-level du projet + FinInitialisation() qui leve une div/0.
# Pre-fix : exit 139 (segfault). Post-fix : exit 70, message "Division par
# zero" + pile d'appels, pas de "Segmentation fault".
#
#  — le critere d'origine etait une pure ABSENCE ("pas de SIGSEGV").
# Un programme qui ne s'execute pas ne plante jamais : la fixture a cesse de
# compiler (regle du fichier-fenetre pur) et le verrou est reste vert pendant
# des mois en ne testant plus rien. On exige donc desormais la PREUVE DE
# PRESENCE avant de conclure a l'absence de crash : la fixture doit compiler,
# le top-level doit avoir tourne, la native re-entrante doit avoir rappele du
# bytecode utilisateur, et l'erreur doit avoir ete levee ET propagee.
#
# Usage: scripts/run_segfault_unwind_test.sh [path/to/owl.exe]

set -u

OWL_EXE="${1:-./build/owl.exe}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE_DIR="$DIR/tests/compiler/internal_fixtures/segfault_unwind_native"
FIXTURE="$FIXTURE_DIR/FEN_Crash.owl"

# Marqueurs poses par la fixture. Ce sont eux qui distinguent "le programme a
# tourne et n'a pas plante" de "le programme n'a jamais demarre".
MARK_TOPLEVEL="segfault_unwind: top-level atteint"
MARK_REENTRANT="segfault_unwind: FinInitialisation entree"
MARK_UNREACHABLE="segfault_unwind: NE DOIT PAS SORTIR"

if [[ ! -x "$OWL_EXE" ]]; then
    echo "ERROR: owl exe not found: $OWL_EXE" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "ERROR: fixture not found: $FIXTURE" >&2
    exit 2
fi

OWL_EXE="$(cd "$(dirname "$OWL_EXE")" && pwd)/$(basename "$OWL_EXE")"

# Augment PATH for Qt6 DLLs (idem run_compiler_tests.sh).
if [[ -d "/c/msys64/ucrt64/bin" ]]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

fail=0

# ---------------------------------------------------------------------------
# Preuve de presence 0 : la fixture compile.
# Si une regle du langage l'invalide (c'est exactement ce qui est arrive avec
# la regle du fichier-fenetre pur), on veut un ECHEC bruyant, pas un vert.
# ---------------------------------------------------------------------------
check_out=$("$OWL_EXE" --check "$FIXTURE" 2>&1)
check_code=$?
if [[ $check_code -ne 0 ]] || echo "$check_out" | grep -q '"message"'; then
    echo "FAIL segfault_unwind: la fixture ne compile plus (owl --check exit $check_code)" >&2
    echo "  $FIXTURE" >&2
    echo "$check_out" | head -5 >&2
    echo "  => le verrou ne teste RIEN tant que ceci n'est pas repare." >&2
    fail=1
fi

# Capture merged stdout+stderr + exit code.
output=$("$OWL_EXE" "$FIXTURE_DIR" 2>&1)
exit_code=$?

# ---------------------------------------------------------------------------
# Preuve de presence 1 : le programme a demarre.
# ---------------------------------------------------------------------------
if ! echo "$output" | grep -qF "$MARK_TOPLEVEL"; then
    echo "FAIL segfault_unwind: marqueur top-level absent — le programme n'a pas demarre." >&2
    echo "  attendu dans la sortie : $MARK_TOPLEVEL" >&2
    fail=1
fi

# ---------------------------------------------------------------------------
# Preuve de presence 2 : la native re-entrante a bien rappele du bytecode
# utilisateur (Ouvre -> vm_call_reentrant -> FinInitialisation). Sans ca, le
# scenario de  n'est pas exerce, meme si le programme tourne.
# ---------------------------------------------------------------------------
if ! echo "$output" | grep -qF "$MARK_REENTRANT"; then
    echo "FAIL segfault_unwind: FinInitialisation n'a pas ete executee." >&2
    echo "  La native re-entrante n'a pas rappele de bytecode utilisateur :" >&2
    echo "  le scenario fix/0236 n'est pas exerce." >&2
    fail=1
fi

# ---------------------------------------------------------------------------
# Preuve de presence 3 : l'erreur runtime a bien ete levee (verification
# POSITIVE du message, plus un simple WARN).
# ---------------------------------------------------------------------------
if ! echo "$output" | grep -q "Division par z"; then
    echo "FAIL segfault_unwind: message 'Division par zero' absent — l'erreur n'a pas ete levee." >&2
    fail=1
fi

# ---------------------------------------------------------------------------
# Preuve de presence 4 : l'erreur provient bien du callback re-entrant, pas
# d'ailleurs. La pile d'appels doit nommer FinInitialisation.
# ---------------------------------------------------------------------------
if ! echo "$output" | grep -q "FinInitialisation()"; then
    echo "FAIL segfault_unwind: la pile d'appels ne mentionne pas FinInitialisation()." >&2
    echo "  L'erreur ne vient pas du callback de la native re-entrante." >&2
    fail=1
fi

# ---------------------------------------------------------------------------
# Preuve de presence 5 : l'erreur a bien INTERROMPU le top-level (unwind
# effectif). Si l'instruction suivant Ouvre() s'execute, l'erreur a ete
# avalee et le demontage de pile teste n'a pas eu lieu.
# ---------------------------------------------------------------------------
if echo "$output" | grep -qF "$MARK_UNREACHABLE"; then
    echo "FAIL segfault_unwind: le top-level a continue apres l'erreur — pas d'unwind." >&2
    fail=1
fi

# ---------------------------------------------------------------------------
# Preuve de presence 6 : code de sortie = erreur runtime propagee (70).
# 0 = rien ne s'est passe, 65 = erreur de compilation, 139 = SIGSEGV.
# ---------------------------------------------------------------------------
if [[ $exit_code -ne 70 ]]; then
    echo "FAIL segfault_unwind: exit code $exit_code, attendu 70 (erreur runtime propagee)." >&2
    fail=1
fi

# ---------------------------------------------------------------------------
# Critere d'origine : pas de SIGSEGV.
# ---------------------------------------------------------------------------
if [[ $exit_code -eq 139 ]]; then
    echo "FAIL segfault_unwind: exit code 139 (SIGSEGV)" >&2
    fail=1
fi

# Le shell affiche "Segmentation fault" sur SIGSEGV. Capture cross-env
# (bash MSYS2, Linux, macOS).
if echo "$output" | grep -q "Segmentation fault"; then
    echo "FAIL segfault_unwind: 'Segmentation fault' detecte dans output" >&2
    fail=1
fi

if [[ $fail -eq 0 ]]; then
    # Le compte de preuves est affiche : ce verrou n'a qu'une
    # fixture, sa non-vacuite tient au nombre de preuves de PRESENCE
    # reellement verifiees avant de conclure a l'absence de plantage.
    echo "segfault_unwind: PASS (7 preuves de presence verifiees, exit $exit_code, erreur levee depuis la native re-entrante, no crash)"
    exit 0
else
    echo "" >&2
    echo "Full output captured:" >&2
    echo "$output" | head -30 >&2
    echo "Exit code: $exit_code" >&2
    exit 1
fi
