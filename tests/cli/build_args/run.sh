#!/usr/bin/env bash
# Harnais : un exe construit par owl-build recoit bien sa ligne de commande.
#
# Contexte (cf. *.md) : le `main` genere pour
# l'exe applicatif NON-service etait emis `int main(void)` — il ignorait
# argc/argv, donc n'appelait jamais stdlib_console_set_script_args(). Resultat :
# LigneCommande(1)/LigneCommande(2) rendaient "" dans l'exe construit, alors que
# `owl projet.owl bonjour monde` fonctionnait (c'est main.c qui pose les args).
# Le fix passe le main genere a `int main(int argc, char* argv[])` et pose les
# deux setters (script args + args de la fenetre de demarrage) AVANT project_run,
# en strippant argv[0] (nom de l'exe) : argc-1/argv+1.
#
# Deux niveaux de verification :
#
#   1. STATIQUE (toujours jouee, CI-safe) — garde sur le code genere dans
#      src/project_run.c : la branche applicative pose bien les setters argv.
#      C'est le filet de non-regression deterministe : rouge si quelqu'un
#      reverte le main genere a `int main(void)` ou retire la capture d'argv.
#
#   2. BOUT-EN-BOUT (opt-in) — empaquette un mini-projet qui echo
#      LigneCommande(1)/(2) et verifie qu'il recoit "bonjour monde".
#      Vehicule : owl-build.exe (EXCLUDE_FROM_ALL, bati par `make mega`, hors
#      batterie). SKIP propre si le binaire est absent OU si le link est
#      incomplet dans ce build dir (aucun exe produit). A rejouer apres un
#      `make mega` quand on touche la generation du main applicatif.
#
# Usage:
#   ./run.sh
#   OWL_BUILD_BIN=/path/to/owl-build.exe ./run.sh
#
# Exit code : 0 si tous les cas joues passent (le e2e peut SKIP), 1 sinon.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"
FAIL=0

# --- 1. Garde statique sur le code genere (toujours jouee) -------------------
GEN="$OWL_ROOT/src/project_run.c"
if [ ! -f "$GEN" ]; then
    echo "build_args_static: RED (src/project_run.c introuvable)"
    exit 1
fi
# On grep la LIGNE D'APPEL du corps du main genere, pas le simple nom du
# symbole : les deux noms apparaissent aussi dans les forward-declares
# (signature `(int argc, char** argv)`). Seul l'appel reel porte le strip
# d'argv[0] `(argc - 1, argv + 1)`. Un revert qui retire les appels mais laisse
# les forward-declares doit rougir — d'ou le match sur la sous-chaine d'appel.
if grep -qF "set_script_args(argc - 1, argv + 1)" "$GEN" \
   && grep -qF "set_cli_args(argc - 1, argv + 1)" "$GEN"; then
    echo "build_args_static: RESULT: GREEN (main applicatif genere cable sur argv)"
else
    echo "build_args_static: RESULT: RED (main applicatif genere n'appelle pas les setters argv)"
    FAIL=1
fi

# --- 2. Bout-en-bout sur un vrai exe empaquete (opt-in) ----------------------
OWL_BUILD="${OWL_BUILD_BIN:-$OWL_ROOT/build/owl-build.exe}"
FIXT="$OWL_ROOT/tests/internal_fixtures/cli_build_args"

if [ ! -f "$OWL_BUILD" ]; then
    echo "build_args_e2e: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit $FAIL
fi

rm -rf "$FIXT/exe"
cleanup() { rm -rf "$FIXT/exe"; }
trap cleanup EXIT

build_out="$("$OWL_BUILD" "$FIXT" 2>&1)"
build_rc=$?
EXE="$(find "$FIXT/exe" -type f \( -name '*.exe' -o -perm -u+x \) 2>/dev/null | head -n 1)"
if [ -z "$EXE" ]; then
    echo "build_args_e2e: SKIP (aucun exe produit — link incomplet dans ce build dir, cf. ticket)"
    echo "  --- sortie owl-build (exit $build_rc) ---"
    printf '%s\n' "$build_out" | sed 's/^/  /'
    exit $FAIL
fi

run_out="$("$EXE" bonjour monde 2>&1)"
if printf '%s' "$run_out" | grep -qF "mot 1 = <bonjour>" \
   && printf '%s' "$run_out" | grep -qF "mot 2 = <monde>"; then
    echo "build_args_e2e: RESULT: GREEN (exe recoit 'bonjour monde')"
else
    echo "build_args_e2e: RESULT: RED (args non recus)"
    echo "  --- sortie exe ---"
    printf '%s\n' "$run_out" | sed 's/^/  /'
    FAIL=1
fi

exit $FAIL
