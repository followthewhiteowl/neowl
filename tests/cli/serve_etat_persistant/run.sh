#!/usr/bin/env bash
# Harnais boîte noire pour `owl serve` — preuve empirique du modèle d'état
# du serveur (, cadrage trou n°4 : « comment un service web OWL
# tient son état entre requêtes »).
#
# Fixture : tests/internal_fixtures/cli_serve_etat_persistant/
# service_compteur_persistant.owl — une route GET /api/compteur dont le
# handler incrémente une variable globale déclarée dans le corps du
# service, puis la renvoie.
#
# HYPOTHÈSE TESTÉE : le serveur `owl serve` tourne sur une VM unique
# longue-vie -> les variables globales persistent entre requêtes HTTP
# (exécution des handlers sérialisée, exécuteur mono-thread). Trois
# requêtes GET successives sur le MÊME process doivent renvoyer 1, puis 2,
# puis 3. Si le compteur reste bloqué à 1 (remis à zéro entre requêtes),
# l'hypothèse est FAUSSE -- résultat important à remonter, pas un simple
# bug de fixture.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si la séquence observée est exactement 1,2,3 (vert attendu
# -- confirme le modèle "globales persistantes"), 1 si la compilation/le
# montage échoue ou si la séquence observée diverge (le modèle n'est pas
# celui attendu -- à documenter dans la note de cadrage), 2 si owl.exe
# introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT_DIR="$OWL_ROOT/tests/internal_fixtures/cli_serve_etat_persistant"
FICHIER="service_compteur_persistant.owl"
PORT="${OWL_SERVE_TEST_PORT:-19103}"

OUT="$(mktemp)"

(cd "$FIXT_DIR" && "$OWL" serve "$FICHIER" --port "$PORT") > "$OUT" 2>&1 &
PID=$!

DEADLINE=$((SECONDS + 5))
STATE="timeout"
while [ $SECONDS -lt $DEADLINE ]; do
    if grep -qE "en écoute|en ecoute" "$OUT" 2>/dev/null; then
        STATE="monte"
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        STATE="sorti"
        break
    fi
    sleep 0.2
done

RC=1
C1="" ; C2="" ; C3=""
if [ "$STATE" = "monte" ]; then
    C1="$(curl -s "http://127.0.0.1:$PORT/api/compteur" 2>/dev/null)"
    C2="$(curl -s "http://127.0.0.1:$PORT/api/compteur" 2>/dev/null)"
    C3="$(curl -s "http://127.0.0.1:$PORT/api/compteur" 2>/dev/null)"
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

echo "cli_serve_etat_persistant: valeurs observées (req1,req2,req3) = $C1,$C2,$C3"

case "$STATE" in
    monte)
        if [ "$C1" = "1" ] && [ "$C2" = "2" ] && [ "$C3" = "3" ]; then
            echo "cli_serve_etat_persistant: RESULT: GREEN (hypothèse CONFIRMÉE -- globales persistantes entre requêtes)"
            RC=0
        else
            echo "cli_serve_etat_persistant: RESULT: RED (hypothèse NON CONFIRMÉE -- séquence attendue 1,2,3 -- voir sortie 'owl serve' ci-dessous)"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
            RC=1
        fi
        ;;
    sorti)
        echo "cli_serve_etat_persistant: RESULT: RED (le service ne s'est pas monté -- sortie 'owl serve' ci-dessous)"
        echo "--- sortie 'owl serve' ---"
        echo "$CONTENU"
        RC=1
        ;;
    timeout)
        echo "cli_serve_etat_persistant: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
        RC=1
        ;;
esac

exit $RC
