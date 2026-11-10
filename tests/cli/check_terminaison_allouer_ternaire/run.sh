#!/usr/bin/env bash
# Harnais boite noire : garde anti-boucle-infinie sur `owl --check`.
#
# Contexte (cf. *.md) : un groupe `(` laisse
# ouvert ET contenant un closer de récupération (`sinon`/`faire`/`enfin`) a
# l'intérieur fait boucler `parse_precedence` / `synchronize()` à l'infini
# (ni l'un ni l'autre n'avance le curseur sur ce token). Déclencheur réel
# rencontré sur du code converti depuis un autre dialecte :
# `allouer un cX(1 ? "a" sinon "")`
# (`allouer` n'est pas un mot-clé OWL, et le ternaire laisse le groupe
# ouvert). Repro mesurée : runaway RAM (3,8 Go, tué par l'OS) + CPU 100%.
#
# Ce test NE PEUT PAS s'exécuter in-process (owl.exe boucle réellement avant
# le fix) : chaque cas passe par un sous-process borné par `timeout`, jamais
# un `owl test` in-process qui bloquerait toute la campagne. Rouge AVANT fix
# = TIMEOUT->echec borne (exit 124, jamais un hang de CE harnais). Vert
# APRES fix = terminaison rapide + erreur de compilation propre.
#
# case_baseline_sans_ternaire : meme `allouer`, SANS ternaire — sert de
# temoin "le mecanisme d'erreur normal marche toujours" (deja vert
# aujourd'hui, ne doit jamais regresser).
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
    echo "check_terminaison_allouer_ternaire: ERROR owl.exe introuvable: $OWL_BIN" >&2
    exit 1
fi

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD=(timeout 5)
else
    # Sans `timeout` dispo, pas de garde-fou possible : on refuse de lancer
    # un cas qui peut hanger reellement AVANT le fix plutot que de risquer
    # de bloquer la campagne.
    echo "check_terminaison_allouer_ternaire: SKIP (commande 'timeout' absente, impossible de borner un cas qui peut boucler)" >&2
    exit 0
fi

FAIL=0

# $1 = nom du cas, $2 = fixture, $3 = "hang_guard" (doit terminer ET errorer)
# ou "clean_error" (meme attente, cas temoin deja vert aujourd'hui).
run_case() {
    local name="$1" fixture="$2"
    local out rc
    out="$("${TIMEOUT_CMD[@]}" "$OWL_BIN" --check "$ROOT/$fixture" 2>&1)"
    rc=$?

    if [ "$rc" = 124 ]; then
        echo "check_terminaison_allouer_ternaire_${name}: RESULT: RED (timeout 5s — non-terminaison, bug non corrige)"
        FAIL=1
        return
    fi

    # Doit terminer avec une erreur de compilation propre (pas un crash, pas
    # un succes silencieux : `allouer` n'est jamais une variable valide).
    if ! printf '%s' "$out" | grep -qF '"errors":[{'; then
        echo "check_terminaison_allouer_ternaire_${name}: RESULT: RED (termine mais pas d'erreur de compilation — attendu: 'allouer' signale)"
        echo "  --- sortie ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
        return
    fi

    echo "check_terminaison_allouer_ternaire_${name}: RESULT: GREEN (rc=$rc, termine, erreur de compilation propre)"
}

# --- Matrice confirmee (coordinator, ) : groupe ouvert `allouer un
# cX(` + closer `sinon` du ternaire a l'interieur -> boucle, QUEL QUE SOIT
# le parenthesage de la condition. ---
run_case "sinon_nu"              "case_sinon/fixture.owl"
run_case "sinon_cond_parenthese" "case_sinon_cond_parenthese/fixture.owl"
run_case "sinon_full_parenthese" "case_sinon_full_parenthese/fixture.owl"

# --- Temoin : sans ternaire, `allouer` seul reste "variable inconnue"
# propre (deja vert aujourd'hui, verrouille la non-regression du cas sain).
run_case "baseline_sans_ternaire" "case_baseline_sans_ternaire/fixture.owl"

exit $FAIL
