#!/usr/bin/env bash
# Harnais boîte noire pour `owl serve` sur un projet multi-fichiers.
#
# Repro : tests/internal_fixtures/cli_serve/projet_multi/{projet.owl,
# etat_facture.owl, api.owl}. `api.owl` déclare un ServiceREST dont
# l'opération référence `ETAT_Facture`, déclaré dans le voisin
# `etat_facture.owl`. `owl .` (ou `--check api.owl`) résout la frontière
# projet et compile sans erreur ; AVANT le fix, `owl serve api.owl` ne
# compile QUE ce fichier seul et échoue sur `'ETAT_Facture' inconnue`.
#
# `owl serve` réussi reste bloqué en écoute (jusqu'à Ctrl-C) : on ne peut
# pas se contenter d'attendre la fin du process comme pour une commande
# one-shot (cf. tests/cli/openapi/run.sh). On lance le process en tâche de
# fond, on scrute sa sortie (fichier temp) avec une borne de temps courte,
# puis on le tue systématiquement (jamais de process orphelin après ce
# script, succès ou échec).
#
# Usage:
#   ./run.sh                 # utilise ./build/owl.exe (racine repo)
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si le service monte (ETAT_Facture résolu -- vert attendu
# après fix), 1 si le bug est reproduit ou si la sortie est inattendue,
# 2 si owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

PROJET_DIR="$OWL_ROOT/tests/internal_fixtures/cli_serve/projet_multi"
API="$PROJET_DIR/api.owl"
PORT="${OWL_SERVE_TEST_PORT:-18734}"

OUT="$(mktemp)"

# Lance `owl serve api.owl` en tâche de fond, CWD = dossier du projet (comme
# un utilisateur le ferait), sortie (stdout+stderr) redirigée.
(cd "$PROJET_DIR" && "$OWL" serve "$(basename "$API")" --port "$PORT") > "$OUT" 2>&1 &
PID=$!

# Attente bornée (5 s) : soit le process meurt vite (échec de compilation --
# rouge attendu aujourd'hui), soit il imprime la ligne de montage ("en
# écoute") et reste vivant en attente de Ctrl-C (vert attendu après fix).
DEADLINE=$((SECONDS + 5))
STATE="timeout"
while [ $SECONDS -lt $DEADLINE ]; do
    if grep -q "en écoute" "$OUT" 2>/dev/null || grep -q "en ecoute" "$OUT" 2>/dev/null; then
        STATE="monte"
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        STATE="sorti"
        break
    fi
    sleep 0.2
done

# Nettoyage systématique : jamais de serveur de test qui continue à tourner
# après ce script, quel que soit l'état constaté.
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

echo "--- sortie 'owl serve' ---"
echo "$CONTENU"
echo "--- état constaté : $STATE ---"

case "$STATE" in
    monte)
        echo "cli_serve_projet_multi: RESULT: GREEN (service monté, ETAT_Facture résolu)"
        exit 0
        ;;
    sorti)
        if echo "$CONTENU" | grep -qi "ETAT_Facture" && echo "$CONTENU" | grep -qi "inconnu"; then
            echo "cli_serve_projet_multi: RESULT: RED (bonne raison -- ETAT_Facture inconnue, bug fix/1982 reproduit)"
        else
            echo "cli_serve_projet_multi: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessus)"
        fi
        exit 1
        ;;
    timeout)
        echo "cli_serve_projet_multi: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
        exit 1
        ;;
esac
