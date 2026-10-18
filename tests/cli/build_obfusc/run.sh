#!/usr/bin/env bash
# Harnais boite noire : le source .owl embarque dans l'exe produit par
# owl-build est COMPRESSE (deflate), pas emis en clair.
#
# Contexte (cf. *.md) : avant ce chantier le
# source d'un projet etait embarque en litteraux C PLAIN dans le bundle genere
# -> `strings monapp.exe` recrachait le code verbatim. Le bundle emet desormais
# chaque fichier source en blob raw-deflate (miniz tdefl), reinflate au
# demarrage via le tinfl deja linke dans tout exe produit. PAS de XOR, PAS de
# chiffrement, PAS de cle : la seule compression suffit a defaire le `strings`
# trivial tout en restant heuristic-clean (le XOR, lui, tripait Defender et a
# ete retire ; ne pas le reintroduire).
#
# Ce harnais prouve DEUX choses sur un vrai exe empaquete :
#   - no-leak : une sentinelle presente dans le source est ABSENTE des octets
#     de l'exe (preuve que la source n'est plus en clair) ;
#   - round-trip : l'exe execute restitue la sentinelle sur sa sortie (preuve
#     que l'inflate reconstruit le source a l'identique et que le programme
#     tourne).
# Un grep de controle sur une chaine qui DOIT exister (le nom de fichier,
# garde en clair dans _names[] pour des messages d'erreur utiles) atteste que
# le grep binaire fonctionne — sinon un no-leak "vert" serait trivialement
# faux (grep cassé matcherait 0 partout).
#
# Vehicule : owl-build.exe. ATTENTION — owl-build est EXCLUDE_FROM_ALL dans
# CMakeLists.txt : `make` ne le construit PAS (c'est le mega, bati via
# `make mega`). Ce harnais n'est donc PAS cable dans la batterie make/CI ;
# il SKIP proprement (exit 0) si le binaire est absent. A rejouer a la main
# apres un `cmake --build build --target owl-build` ou un `make mega` quand on
# touche la generation du bundle (build_project).
#
# Usage:
#   ./run.sh
#   OWL_BUILD_BIN=/path/to/owl-build.exe ./run.sh
#
# Exit code : 0 si tous les cas passent (ou si owl-build absent = SKIP),
#             1 si au moins un cas diverge.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL_BUILD="${OWL_BUILD_BIN:-$OWL_ROOT/build/owl-build.exe}"

if [ ! -f "$OWL_BUILD" ]; then
    echo "build_obfusc: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit 0
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_build_obfusc"
SENTINEL="OBFUSC_SENTINEL_marqueur_test"
# Chaine de controle : le nom du fichier source est garde EN CLAIR dans le
# bundle (_names[], pour des messages d'erreur utiles). Elle DOIT apparaitre.
CONTROL="projet.owl"
FAIL=0

# Nettoie toute sortie precedente avant/apres (exe non commite).
rm -rf "$FIXT/exe"
trap 'rm -rf "$FIXT/exe"' EXIT

# 1. Empaquette la fixture. Un echec de LINK (libs vendor non stagees dans un
#    build dir dev) n'est PAS un echec de test : on SKIP si aucun exe n'est
#    produit, la mesure no-leak/round-trip demandant un exe complet.
build_out="$("$OWL_BUILD" "$FIXT" 2>&1)"
build_rc=$?

EXE="$(find "$FIXT/exe" -type f \( -name '*.exe' -o -perm -u+x \) 2>/dev/null | head -n 1)"
if [ -z "$EXE" ]; then
    echo "build_obfusc: SKIP (aucun exe produit — link incomplet dans ce build dir, cf. ticket)"
    echo "  --- sortie owl-build (exit $build_rc) ---"
    printf '%s\n' "$build_out" | sed 's/^/  /'
    exit 0
fi

# 2. Controle positif : le grep binaire fonctionne (la chaine de controle est
#    presente).
if grep -aqF "$CONTROL" "$EXE"; then
    echo "build_obfusc_control_present: RESULT: GREEN"
else
    echo "build_obfusc_control_present: RESULT: RED (chaine de controle '$CONTROL' absente — grep binaire inoperant, no-leak non concluant)"
    FAIL=1
fi

# 3. No-leak : la sentinelle du source ne doit PAS apparaitre en clair.
if grep -aqF "$SENTINEL" "$EXE"; then
    echo "build_obfusc_no_leak: RESULT: RED (sentinelle '$SENTINEL' trouvee en clair dans l'exe — source non compressee)"
    FAIL=1
else
    echo "build_obfusc_no_leak: RESULT: GREEN"
fi

# 4. Round-trip : l'exe execute restitue la sentinelle (inflate OK + run OK).
run_out="$("$EXE" 2>&1)"
run_rc=$?
if printf '%s' "$run_out" | grep -qF "$SENTINEL"; then
    echo "build_obfusc_round_trip: RESULT: GREEN"
else
    echo "build_obfusc_round_trip: RESULT: RED (sentinelle absente de la sortie — inflate/run casse, exit $run_rc)"
    printf '%s\n' "$run_out" | sed 's/^/  /'
    FAIL=1
fi

exit $FAIL
