#!/usr/bin/env bash
# Harnais boite noire pour `owl openapi` -- verrouille exécution unique du
# top-level projet + stdout JSON propre.
#
# Repro (mesuré par le coordinator) : `owl openapi <entrée>` dans un projet
# fait une pré-passe `compagnons_service_count()` (owl_serve.c) pour calculer
# l'index des services de l'entrée, PUIS la vraie compilation -- chaque
# compagnon (dont projet.owl) tourne donc 2 fois. Le `Trace` du top-level de
# projet.owl part en plus sur stdout, polluant le document OpenAPI.
#
# Fixture : tests/internal_fixtures/cli_openapi/double_exec_probe/
#   - projet.owl : top-level trace un marqueur unique "OPENAPI_PROBE".
#   - bonjour.owl : entrée, un ServiceREST / une OpérationHTTP (/api/bonjour).
#
# Attendu APRES fix :
#   1. "OPENAPI_PROBE" apparaît exactement 1 fois au total (stdout + stderr).
#   2. stdout ne contient AUCUNE occurrence de "OPENAPI_PROBE" (JSON pur).
#   3. stderr contient "OPENAPI_PROBE" exactement 1 fois (trace user redirigée).
#   4. stdout reste un document OpenAPI valide documentant /api/bonjour.
#
# Sur le binaire actuel (bug) : "OPENAPI_PROBE" apparaît 2 fois, sur stdout.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si les 4 attentes sont vérifiées, 1 sinon, 2 si owl.exe
# introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL -- lance 'make' d'abord"
    exit 2
fi

PROJET_DIR="$OWL_ROOT/tests/internal_fixtures/cli_openapi/double_exec_probe"

OUT="$(mktemp)"
ERR="$(mktemp)"

(cd "$PROJET_DIR" && "$OWL" openapi bonjour.owl) >"$OUT" 2>"$ERR"

STDOUT_CONTENT="$(cat "$OUT")"
STDERR_CONTENT="$(cat "$ERR")"

COUNT_STDOUT=$(grep -c "OPENAPI_PROBE" "$OUT" || true)
COUNT_STDERR=$(grep -c "OPENAPI_PROBE" "$ERR" || true)
COUNT_TOTAL=$((COUNT_STDOUT + COUNT_STDERR))

FAIL=0

echo "--- stdout ---"
echo "$STDOUT_CONTENT"
echo "--- stderr ---"
echo "$STDERR_CONTENT"
echo "--- comptage OPENAPI_PROBE : stdout=$COUNT_STDOUT stderr=$COUNT_STDERR total=$COUNT_TOTAL ---"

# --- Attente 1 : exécution unique (total == 1) ------------------------------
if [ "$COUNT_TOTAL" -eq 1 ]; then
    echo "openapi_double_exec_count: RESULT: GREEN (top-level exécuté 1 fois)"
elif [ "$COUNT_TOTAL" -eq 2 ]; then
    echo "openapi_double_exec_count: RESULT: RED (bonne raison -- top-level exécuté 2 fois, bug fix/2379 reproduit)"
    FAIL=1
else
    echo "openapi_double_exec_count: RESULT: RED (MAUVAISE raison -- total=$COUNT_TOTAL inattendu)"
    FAIL=1
fi

# --- Attente 2 : stdout sans le marqueur (JSON propre) ----------------------
if [ "$COUNT_STDOUT" -eq 0 ]; then
    echo "openapi_double_exec_stdout_clean: RESULT: GREEN (stdout sans marqueur)"
else
    echo "openapi_double_exec_stdout_clean: RESULT: RED (bonne raison -- marqueur present $COUNT_STDOUT fois sur stdout, bug fix/2379 reproduit)"
    FAIL=1
fi

# --- Attente 3 : stderr contient le marqueur exactement 1 fois --------------
if [ "$COUNT_STDERR" -eq 1 ]; then
    echo "openapi_double_exec_stderr_marker: RESULT: GREEN (trace user redirigée sur stderr, 1 fois)"
else
    echo "openapi_double_exec_stderr_marker: RESULT: RED (marqueur présent $COUNT_STDERR fois sur stderr, attendu 1)"
    FAIL=1
fi

# --- Attente 4 : document OpenAPI valide, documente /api/bonjour -----------
if echo "$STDOUT_CONTENT" | grep -q '"openapi"' && echo "$STDOUT_CONTENT" | grep -q '/api/bonjour' && echo "$STDOUT_CONTENT" | grep -qi '"get"'; then
    echo "openapi_double_exec_document: RESULT: GREEN (document OpenAPI valide, /api/bonjour en GET)"
else
    echo "openapi_double_exec_document: RESULT: RED (document OpenAPI absent ou incorrect)"
    FAIL=1
fi

rm -f "$OUT" "$ERR"
exit $FAIL
