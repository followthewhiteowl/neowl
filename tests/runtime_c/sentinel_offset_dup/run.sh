#!/usr/bin/env bash
#  — verrou de la detection de collision d'offset sentinelle
# (regle no_duplicate_sentinel_offset du registry self-test).
#
# Extrait le corps REEL d'owl_sentinel_scan_duplicates depuis
# src/registry_self_test.c (bornage sur la signature exacte) et le compile a
# la volee avec test_main.c (driver + buffers synthetiques). Aucune
# reimplementation : on teste le code source du jour, pas une copie qui
# pourrait diverger.
#
# Exit code : 0 vert, 1 rouge, 2 erreur d'infrastructure (extraction ou
# compilation impossible — p.ex. fonction absente avant impl).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"
SRC_SELFTEST_C="$OWL_ROOT/src/registry_self_test.c"

if [ ! -f "$SRC_SELFTEST_C" ]; then
    echo "sentinel_offset_dup: src/registry_self_test.c introuvable ($SRC_SELFTEST_C)"
    exit 2
fi

EXTRACTED="$ROOT/_extracted_scan.c"
awk '
/^void owl_sentinel_scan_duplicates\(/ { found=1 }
found {
    print
    n = gsub(/\{/, "{"); depth += n
    n = gsub(/\}/, "}"); depth -= n
    if (depth == 0 && NR > 1 && found_body) { exit }
    if (depth > 0) found_body=1
}
' "$SRC_SELFTEST_C" > "$EXTRACTED"

if [ ! -s "$EXTRACTED" ]; then
    echo "sentinel_offset_dup: extraction d'owl_sentinel_scan_duplicates depuis src/registry_self_test.c a echoue — signature introuvable (fonction absente ou marqueur casse ?)"
    rm -f "$EXTRACTED"
    exit 2
fi

FULL="$ROOT/_full_test.c"
cat "$ROOT/test_main.c" "$EXTRACTED" > "$FULL"

BIN="$ROOT/sentinel_offset_dup_test.exe"
gcc -O0 -Wall -o "$BIN" "$FULL" 2> "$ROOT/_compile.log"
if [ $? -ne 0 ]; then
    echo "sentinel_offset_dup: echec compilation du harness"
    cat "$ROOT/_compile.log"
    rm -f "$EXTRACTED" "$FULL" "$ROOT/_compile.log"
    exit 2
fi
rm -f "$ROOT/_compile.log"

"$BIN"
rc=$?

rm -f "$EXTRACTED" "$FULL" "$BIN"
exit $rc
