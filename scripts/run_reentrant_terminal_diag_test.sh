#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#  -- cas TERMINAL reentrant (aucun QUAND EXCEPTION nulle part).
#
# Repro : TableauTrie(t, ttFonction, Cmp) ou Cmp divise TOUJOURS par zero.
# L'erreur runtime USER nait en reentrant (le comparateur est rappele PAR la
# native de tri), et faute de handler ancetre, le chemin actuel :
#   - imprime le diagnostic « Erreur d'execution » DEUX fois (une fois a la
#     frontiere reentrante, une fois au top-level) au lieu d'une seule ;
#   - ne coupe PAS la native au 1er echec du comparateur : le tri continue
#     d'appeler Cmp (mesure du ticket : 2x aujourd'hui sur ce build, 4x lors
#     de la mesure fraiche du coordinator -- l'exactitude du chiffre derive du
#     contenu du tableau/de l'etat de l'arbre, la racine est la meme : pas
#     d'arret au 1er echec).
#
# Comportement CIBLE (, amendement  -- pendant "cas terminal"
# du contrat pose par  qui ne couvrait que le cas AVEC handler
# ancetre) :
#   - diagnostic imprime EXACTEMENT 1x, au top-level ;
#   - comparateur appele EXACTEMENT 1x (le marqueur Trace("APPEL-CMP-1716")
#     ne doit apparaitre qu'une fois -- le tri s'arrete au 1er echec) ;
#   - exit code NON-NUL (70) preserve -- le cas terminal reste terminal, pas
#     de rattrapage silencieux (piege inverse explicitement signale au
#     cadrage : ne JAMAIS faire disparaitre le diagnostic ou l'exit non-nul).
#
# Meme famille de piege que scripts/run_stdout_survives_crash_test.sh : un
# fichier qui DOIT planter ne peut pas vivre dans une suite `owl test` verte
# (le runner classerait le crash comme un echec de la suite elle-meme, pas
# comme le sujet mesure). D'ou un script process-level dedie, calque sur ce
# modele : lancement direct du binaire, capture stdout/stderr separee, exit
# code inspecte explicitement.
#
# Usage: scripts/run_reentrant_terminal_diag_test.sh [path/to/owl.exe]

set -u

OWL_EXE="${1:-./build/owl.exe}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE="$DIR/tests/compiler/internal_fixtures/reentrant_terminal_diag/test_reentrant_terminal_1716.owl"

if [[ ! -x "$OWL_EXE" ]]; then
    echo "ERROR: owl exe not found: $OWL_EXE" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "ERROR: fixture not found: $FIXTURE" >&2
    exit 2
fi

OWL_EXE="$(cd "$(dirname "$OWL_EXE")" && pwd)/$(basename "$OWL_EXE")"

# Augment PATH for Qt6 DLLs (idem run_compiler_tests.sh / run_stdout_survives_crash_test.sh).
if [[ -d "/c/msys64/ucrt64/bin" ]]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

fail=0

STDOUT_FILE="$(mktemp)"
STDERR_FILE="$(mktemp)"
trap 'rm -f "$STDOUT_FILE" "$STDERR_FILE"' EXIT

# Lancement direct : pas de sous-suite `owl test`, la fixture DOIT planter
# (exit non-nul), donc elle ne peut pas etre decouverte par le runner
# standard -- c'est pour ca qu'elle vit sous internal_fixtures/ (exclu de la
# marche projet, cf. src/project.c) et non sous tests/regression/.
"$OWL_EXE" "$FIXTURE" >"$STDOUT_FILE" 2>"$STDERR_FILE"
exit_code=$?

diag_count=$(grep -c "Erreur d'execution" "$STDERR_FILE" || true)
cmp_count=$(grep -c "APPEL-CMP-1716" "$STDOUT_FILE" || true)

# --- Assertion 1 : le cas reste TERMINAL (piege inverse explicite au
# cadrage) : pas de rattrapage silencieux, exit code non-nul. ---
if [[ "$exit_code" -eq 0 ]]; then
    echo "FAIL reentrant_terminal_diag: exit code 0, attendu NON-NUL (70)." >&2
    echo "  Le cas terminal reentrant ne doit JAMAIS devenir silencieux :" >&2
    echo "  une erreur USER sans handler doit rester fatale." >&2
    fail=1
fi

# --- Assertion 2 : diagnostic imprime EXACTEMENT 1 fois. ---
if [[ "$diag_count" -ne 1 ]]; then
    echo "FAIL reentrant_terminal_diag: diagnostic « Erreur d'execution » vu ${diag_count}x sur stderr, attendu EXACTEMENT 1x." >&2
    if [[ "$diag_count" -gt 1 ]]; then
        echo "  Double impression : la frontiere reentrante ET le top-level imprimeent" >&2
        echo "  chacun leur propre diagnostic pour la MEME erreur (racine fix/1716)." >&2
    else
        echo "  Aucun diagnostic : le cas terminal est devenu silencieux, c'est le piege" >&2
        echo "  inverse explicitement proscrit par le cadrage du ticket." >&2
    fi
    fail=1
fi

# --- Assertion 3 : le comparateur s'arrete au 1er echec (EXACTEMENT 1 appel). ---
if [[ "$cmp_count" -ne 1 ]]; then
    echo "FAIL reentrant_terminal_diag: marqueur APPEL-CMP-1716 vu ${cmp_count}x, attendu EXACTEMENT 1x." >&2
    if [[ "$cmp_count" -gt 1 ]]; then
        echo "  Le tri ne s'arrete PAS au 1er echec du comparateur : l'erreur en vol" >&2
        echo "  n'interrompt pas la native TableauTrie, qui rappelle Cmp malgre l'echec" >&2
        echo "  deja survenu (racine commune avec le double-print, cf. ADR_0175)." >&2
    else
        echo "  Le comparateur n'a jamais ete appele : la fixture n'exerce pas le" >&2
        echo "  scenario reentrant, le verrou ne teste RIEN en l'etat." >&2
    fi
    fail=1
fi

if [[ $fail -ne 0 ]]; then
    echo "" >&2
    echo "exit_code=$exit_code diag_count=$diag_count cmp_count=$cmp_count" >&2
    echo "stdout capture:" >&2
    cat "$STDOUT_FILE" >&2
    echo "stderr capture:" >&2
    cat "$STDERR_FILE" >&2
fi

if [[ $fail -eq 0 ]]; then
    echo "reentrant_terminal_diag: PASS (exit=$exit_code, diagnostic 1x, comparateur 1x -- arret au 1er echec, cas terminal non silencieux)"
    exit 0
else
    exit 1
fi
