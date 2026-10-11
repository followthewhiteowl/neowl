#!/usr/bin/env bash
#  — verrou LC_NUMERIC de owl_real_to_str (src/value.c).
#
# Extrait le corps REEL de owl_real_to_str depuis src/value.c (bornage sur
# la signature exacte de la fonction) et le compile a la volee avec
# test_main.c (driver setlocale + assertions). Aucune reimplementation :
# on teste le code source du jour, pas une copie qui pourrait diverger.
#
# Exit code : 0 (vert ou skip loggé faute de locale virgule dispo), 1 rouge,
# 2 erreur d'infrastructure (extraction/compilation impossible).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"
SRC_VALUE_C="$OWL_ROOT/src/value.c"

if [ ! -f "$SRC_VALUE_C" ]; then
    echo "real_to_str_locale: src/value.c introuvable ($SRC_VALUE_C)"
    exit 2
fi

EXTRACTED="$ROOT/_extracted_owl_real_to_str.c"
awk '
/^int owl_real_to_str\(double v, char\* buf, int bufSize\) \{/ { found=1 }
found {
    print
    n = gsub(/\{/, "{"); depth += n
    n = gsub(/\}/, "}"); depth -= n
    if (depth == 0 && NR > 1) { exit }
}
' "$SRC_VALUE_C" > "$EXTRACTED"

if [ ! -s "$EXTRACTED" ]; then
    echo "real_to_str_locale: extraction de owl_real_to_str depuis src/value.c a echoue — signature introuvable (marqueur casse ?)"
    rm -f "$EXTRACTED"
    exit 2
fi

FULL="$ROOT/_full_test.c"
cat "$ROOT/test_main.c" "$EXTRACTED" > "$FULL"

BIN="$ROOT/real_to_str_locale_test.exe"
gcc -O0 -Wall -o "$BIN" "$FULL" -lm 2> "$ROOT/_compile.log"
if [ $? -ne 0 ]; then
    echo "real_to_str_locale: echec compilation du harness"
    cat "$ROOT/_compile.log"
    rm -f "$EXTRACTED" "$FULL" "$ROOT/_compile.log"
    exit 2
fi
rm -f "$ROOT/_compile.log"

"$BIN"
rc=$?

rm -f "$EXTRACTED" "$FULL" "$BIN"

if [ $rc -eq 77 ]; then
    echo "real_to_str_locale: SKIP (aucune locale a separateur virgule disponible sur cette machine — non concluant)"
    exit 0
fi
exit $rc
