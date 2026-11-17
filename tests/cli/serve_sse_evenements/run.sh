#!/usr/bin/env bash
# Harnais boite noire pour `owl serve` — famille d'événements SSE nommés
# : FluxÉvénement / FluxIdentifiant / FluxCommentaire, posées sur
# le même helper que FluxÉcrit.
#
# Fixture : tests/internal_fixtures/cli_serve_sse_evenements/
# service_sse_evenements.owl — une route GET /api/journal déclarée
# Flux = fluxSSE dont le handler appelle, dans l'ordre :
#   FluxÉvénement("log", "ligne1")
#   FluxIdentifiant("42")
#   FluxCommentaire("heartbeat")
#
# Framing SSE attendu (RFC — octet pour octet, une fois le corps dé-chunké) :
#   event: log
#   data: ligne1
#   <ligne vide>
#   id: 42
#   : heartbeat
#
# AUJOURD'HUI (natives absentes) : `owl serve` échoue à la compilation avant
# même de monter — rouge attendu, bonne raison si le message nomme les 3
# fonctions comme inconnues. Après l'implémentation : le service démarre, la
# route répond et le corps matche EXACTEMENT le framing ci-dessus (vert).
#
# Même patron que tests/cli/serve/run.sh : lancé en tâche de fond, scruté
# avec une borne de temps courte, tué systématiquement (jamais de process
# orphelin après ce script, succès ou échec).
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si le framing SSE nommé est byte-exact (vert attendu après
# implémentation), 1 si la compilation échoue pour une AUTRE raison que les
# 3 natives inconnues, ou si le framing obtenu diverge, 2 si owl.exe
# introuvable. L'échec attendu AUJOURD'HUI (compile error sur les 3 natives)
# sort en 1 — c'est le rouge voulu, documenté dans le message.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT_DIR="$OWL_ROOT/tests/internal_fixtures/cli_serve_sse_evenements"
FICHIER="service_sse_evenements.owl"
PORT="${OWL_SERVE_TEST_PORT:-18995}"

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
CORPS=""
CODE=""
TMP_BODY=""
if [ "$STATE" = "monte" ]; then
    TMP_BODY="$(mktemp)"
    CODE="$(curl -s -o "$TMP_BODY" -w '%{http_code}' "http://127.0.0.1:$PORT/api/journal" 2>/dev/null)"
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

# Framing SSE attendu, écrit dans un fichier pour une comparaison byte-exacte
# via `cmp`. Une capture `$(cat fichier)` mangerait le newline FINAL du corps
# (bash strippe les \n de fin d'une substitution de commande) et rendrait tout
# corps SSE correctement terminé non-égal à l'attendu — faux rouge de harnais.
TMP_EXP="$(mktemp)"
printf 'event: log\ndata: ligne1\n\nid: 42\n: heartbeat\n' > "$TMP_EXP"

case "$STATE" in
    monte)
        if [ "$CODE" = "200" ] && cmp -s "$TMP_BODY" "$TMP_EXP"; then
            echo "cli_serve_sse_evenements: RESULT: GREEN (framing SSE nommé byte-exact : event: log, data: ligne1, id: 42, commentaire heartbeat)"
            RC=0
        else
            echo "cli_serve_sse_evenements: RESULT: RED (MAUVAISE raison -- code=$CODE, corps obtenu (od -c) ci-dessous, attendu : event: log / data: ligne1 / <vide> / id: 42 / : heartbeat)"
            echo "--- corps obtenu (od -c) ---"
            od -c "$TMP_BODY" 2>/dev/null
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
            RC=1
        fi
        ;;
    sorti)
        if echo "$CONTENU" | grep -q "FluxÉvénement" \
            && echo "$CONTENU" | grep -q "FluxIdentifiant" \
            && echo "$CONTENU" | grep -q "FluxCommentaire" \
            && echo "$CONTENU" | grep -qi "inconnue"; then
            echo "cli_serve_sse_evenements: RESULT: RED (bonne raison -- FluxÉvénement/FluxIdentifiant/FluxCommentaire inconnues du compilateur, famille pas encore implémentée)"
        else
            echo "cli_serve_sse_evenements: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessous)"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
        fi
        RC=1
        ;;
    timeout)
        echo "cli_serve_sse_evenements: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
        RC=1
        ;;
esac

rm -f "$TMP_BODY" "$TMP_EXP" 2>/dev/null

exit $RC
