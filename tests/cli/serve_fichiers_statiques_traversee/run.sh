#!/usr/bin/env bash
# Harnais boite noire pour `owl serve` — garde ANTI-TRAVERSÉE DE CHEMIN du
# montage statique déclaratif. C'est la raison d'être du
# ticket : un seul cas de traversée qui fuit hors du dossier monté est une
# vulnérabilité, pas un détail.
#
# Fixture : tests/internal_fixtures/cli_serve_fichiers_statiques/ (même
# fixture que tests/cli/serve_fichiers_statiques/). Le service monte
# ./assets sous /api/static/. Un fichier `secret.txt` vit à la racine de la
# fixture, HORS de ./assets : s'il devient lisible via une URL sous
# /api/static/, la garde anti-traversée a un trou.
#
# AUJOURD'HUI (propriété absente) : `owl serve` échoue à la COMPILATION
# avant de monter -- même rouge que serve_fichiers_statiques/run.sh
# (propriété 'Statique' inconnue). Rouge attendu, bonne raison.
#
# APRÈS IMPLÉMENTATION : chaque variante de traversée ci-dessous DOIT être
# rejetée (code HTTP != 200 ; le choix exact entre 403 et 404 est documenté
# comme un doute ouvert dans le rapport du test_writer -- les deux sont
# acceptés ici). Si l'une d'elles renvoie 200 ET que le corps correspond au
# contenu de secret.txt, c'est un échec dur (fuite confirmée), peu importe
# le reste.
#
# `curl --path-as-is` empêche curl lui-même de normaliser `../` avant
# l'envoi -- sans ça, le client "corrigerait" l'URL et on ne testerait
# jamais vraiment le serveur.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si TOUTES les variantes de traversée sont rejetées sans
# fuite (vert attendu après implémentation), 1 si la compilation échoue
# pour une AUTRE raison que 'Statique' inconnue, ou si UNE SEULE variante
# fuite ou répond 200, 2 si owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT_DIR="$OWL_ROOT/tests/internal_fixtures/cli_serve_fichiers_statiques"
FICHIER="service_fichiers_statiques.owl"
PORT="${OWL_SERVE_TEST_PORT:-19312}"

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
RAPPORT=""
LEAK=0

if [ "$STATE" = "monte" ]; then
    BASE="http://127.0.0.1:$PORT/api/static"
    SECRET="$FIXT_DIR/secret.txt"

    # Nom -> suffixe d'URL après le préfixe /api/static/
    declare -A VARIANTES=(
        [litterale]="../secret.txt"
        [encodee_point_slash_litteral]="%2e%2e/secret.txt"
        [encodee_point_et_slash]="%2e%2e%2fsecret.txt"
        [double_encodee]="%252e%252e%252fsecret.txt"
        [backslash_encode]="%2e%2e%5csecret.txt"
        [triple_remontee]="../../../secret.txt"
    )

    for nom in "${!VARIANTES[@]}"; do
        suffixe="${VARIANTES[$nom]}"
        TMP_BODY="$(mktemp)"
        CODE="$(curl -s --path-as-is -o "$TMP_BODY" -w '%{http_code}' "$BASE/$suffixe" 2>/dev/null)"
        if [ "$CODE" = "200" ] && cmp -s "$TMP_BODY" "$SECRET"; then
            RAPPORT+="  $nom: FUITE CONFIRMEE (code=200, corps = secret.txt)\n"
            LEAK=1
        elif [ "$CODE" = "200" ]; then
            RAPPORT+="  $nom: code=200 (corps different de secret.txt, mais 200 est un rejet attendu en echec -- a verifier)\n"
            LEAK=1
        else
            RAPPORT+="  $nom: code=$CODE (rejete, OK)\n"
        fi
        rm -f "$TMP_BODY" 2>/dev/null
    done
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

case "$STATE" in
    monte)
        if [ "$LEAK" = "0" ]; then
            echo "cli_serve_fichiers_statiques_traversee: RESULT: GREEN (toutes les variantes de traversee rejetees, aucune fuite de secret.txt)"
            echo -e "$RAPPORT"
            RC=0
        else
            echo "cli_serve_fichiers_statiques_traversee: RESULT: RED (VULNERABILITE -- au moins une variante de traversee n'est pas rejetee)"
            echo -e "$RAPPORT"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
            RC=1
        fi
        ;;
    sorti)
        if echo "$CONTENU" | grep -q "Statique" && echo "$CONTENU" | grep -qi "inconnue"; then
            echo "cli_serve_fichiers_statiques_traversee: RESULT: RED (bonne raison -- propriété 'Statique' inconnue de ServiceREST, montage pas encore implémenté)"
        else
            echo "cli_serve_fichiers_statiques_traversee: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessous)"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
        fi
        RC=1
        ;;
    timeout)
        echo "cli_serve_fichiers_statiques_traversee: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
        RC=1
        ;;
esac

exit $RC
