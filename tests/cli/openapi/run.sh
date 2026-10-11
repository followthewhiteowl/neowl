#!/usr/bin/env bash
# Harnais boite noire pour `owl openapi <service.owl>`.
# Compile openapi_cli_scan.c a la volee (gcc) et le lance contre owl.exe,
# meme patron que tests/cli/doc_lang/run.sh.
#
# Usage:
#   ./run.sh                 # utilise ./build/owl.exe (racine repo)
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si le scanner passe, 1 sinon, 2 si owl.exe introuvable ou
# echec de compilation du scanner.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

BIN="$ROOT/openapi_cli_scan.exe"
gcc -O0 -Wall -o "$BIN" "$ROOT/openapi_cli_scan.c" || {
    echo "cli_openapi: echec compilation openapi_cli_scan.c"
    exit 2
}

# Le scanner passe des chemins de fixture RELATIFS a la racine du repo
# (tests/internal_fixtures/cli_openapi/...) : on se place a la racine avant
# de l'invoquer, comme owl_openapi_test le ferait via `owl openapi <fichier>`
# depuis le CWD habituel de la suite.
(cd "$OWL_ROOT" && "$BIN" "$OWL")
rc=$?

rm -f "$BIN"
exit $rc
