#!/usr/bin/env bash
# Harnais boite noire : les raccourcis de langue `--en` / `--fr` doivent etre
# reconnus en position PRE-commande, au meme titre que `--lang en` / `--lang fr`
#.
#
# Bug d'origine : `parse_session_flags` (src/main.c) strippait `--lang`/`--lang=`
# mais PAS les raccourcis `--en`/`--fr`. Donc `owl --en --check <fichier>`
# laissait `--en` dans argv -> le dispatch de commande le prenait pour la
# commande -> commande inconnue -> banniere d'usage imprimee au lieu du
# diagnostic. (`owl --lang en --check` fonctionnait, lui.)
#
# Attendu apres fix :
#   owl --en --check fixture  -> diagnostic ANGLAIS ("Unterminated string.")
#                                et PAS la banniere d'usage.
#   owl --fr --check fixture  -> diagnostic FRANCAIS ("Chaine non terminee.").
#   owl --lang en --check     -> inchange (temoin de non-regression).
#
# Rouge AVANT fix = la sortie contient "Usage : owl" (banniere) au lieu du
# diagnostic attendu.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si tous les cas passent, 1 si au moins un diverge.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL_BIN="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"

if [ ! -x "$OWL_BIN" ]; then
    echo "lang_flag_precommande: ERROR owl.exe introuvable: $OWL_BIN" >&2
    exit 1
fi

FIXTURE="$ROOT/fixture.owl"
FAIL=0

# $1 = nom du cas ; $2 = flag langue pre-commande ; $3 = message attendu
# (sous-chaine exacte, grep -F) ; le cas echoue aussi si la banniere d'usage
# apparait (preuve que le flag a ete pris pour la commande).
run_case() {
    local name="$1" langflag="$2" expected="$3"
    local out

    # $langflag peut etre "--en", "--fr", ou "--lang en" (mot a mot).
    out="$("$OWL_BIN" $langflag --check "$FIXTURE" 2>&1)"

    if printf '%s' "$out" | grep -qF 'Usage : owl'; then
        echo "lang_flag_precommande_${name}: RESULT: RED (banniere d'usage imprimee — flag langue pris pour la commande)"
        echo "  --- sortie ---"
        printf '%s\n' "$out" | head -3 | sed 's/^/  /'
        FAIL=1
        return
    fi

    if ! printf '%s' "$out" | grep -qF "$expected"; then
        echo "lang_flag_precommande_${name}: RESULT: RED (diagnostic attendu absent : '$expected')"
        echo "  --- sortie ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
        return
    fi

    echo "lang_flag_precommande_${name}: RESULT: GREEN ('$expected' present, pas d'usage)"
}

# --- Raccourcis pre-commande (coeur du fix) ---
run_case "en_shortcut"  "--en"      "Unterminated string."
run_case "fr_shortcut"  "--fr"      "Chaîne non terminée."

# --- Temoin de non-regression : la forme longue marchait deja ---
run_case "lang_en_long" "--lang en" "Unterminated string."
run_case "lang_fr_long" "--lang fr" "Chaîne non terminée."

exit $FAIL
