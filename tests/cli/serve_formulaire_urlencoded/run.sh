#!/usr/bin/env bash
# Harnais boîte noire pour `owl serve` — binding d'un corps
# `application/x-www-form-urlencoded` aux paramètres homonymes de
# `surRequête` via la propriété `Formulaire` d'une `OpérationHTTP`
# (, miroir de `Requête` pour la query — 40_services_rest.md §4).
#
# Fixture : tests/internal_fixtures/cli_serve_formulaire_urlencoded/
# service_formulaire_urlencoded.owl — une route POST /api/inscription dont
# le handler lie nom/email/age depuis le corps urlencoded (age coercé vers
# un entier) et renvoie "nom|email|age+1".
#
# AUJOURD'HUI (propriété absente) : `Formulaire` n'est pas une propriété
# reconnue d'`OpérationHTTP` -- refus à la COMPILATION, avant que `owl
# serve` ne monte. Rouge attendu, bonne raison si le message cite
# 'Formulaire' et 'OpérationHTTP'.
#
# APRÈS IMPLÉMENTATION, deux cas :
#   - nominal : POST --data "nom=jean&email=a@b.c&age=30" -> 200, corps
#     exact "jean|a@b.c|31" (preuve de la coercition entière de age).
#   - mauvais Content-Type : POST d'un corps JSON (Content-Type:
#     application/json) sur la même route -> 415, sans jamais appeler
#     surRequête.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si les deux cas sont VERTS, 1 si au moins un est ROUGE
# (compilation en échec pour une AUTRE raison que 'Formulaire' inconnue,
# ou code/corps obtenu divergent), 2 si owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT_DIR="$OWL_ROOT/tests/internal_fixtures/cli_serve_formulaire_urlencoded"
FICHIER="service_formulaire_urlencoded.owl"
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
if [ "$STATE" = "monte" ]; then
    # --- Cas nominal : corps urlencoded lié par nom, coercé ----------------
    TMP_BODY_NOM="$(mktemp)"
    CODE_NOM="$(curl -s -o "$TMP_BODY_NOM" -w '%{http_code}' \
        --data "nom=jean&email=a%40b.c&age=30" \
        "http://127.0.0.1:$PORT/api/inscription" 2>/dev/null)"
    CORPS_NOM="$(cat "$TMP_BODY_NOM" 2>/dev/null)"
    rm -f "$TMP_BODY_NOM"

    # --- Cas mauvais Content-Type : JSON là où un formulaire est attendu ---
    TMP_BODY_415="$(mktemp)"
    CODE_415="$(curl -s -o "$TMP_BODY_415" -w '%{http_code}' \
        -H "Content-Type: application/json" \
        --data '{"nom":"jean","email":"a@b.c","age":30}' \
        "http://127.0.0.1:$PORT/api/inscription" 2>/dev/null)"
    rm -f "$TMP_BODY_415"

    CONTENU="$(cat "$OUT")"
    rm -f "$OUT"

    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi

    OK_NOM=1
    if [ "$CODE_NOM" != "200" ] || [ "$CORPS_NOM" != "jean|a@b.c|31" ]; then OK_NOM=0; fi
    OK_415=1
    if [ "$CODE_415" != "415" ]; then OK_415=0; fi

    if [ "$OK_NOM" -eq 1 ]; then
        echo "cli_serve_formulaire_urlencoded_nominal: RESULT: GREEN (nom/email/age liés par nom, age coercé en entier -- $CODE_NOM $CORPS_NOM)"
    else
        echo "cli_serve_formulaire_urlencoded_nominal: RESULT: RED (MAUVAISE raison -- code=$CODE_NOM corps=[$CORPS_NOM], attendu 200+\"jean|a@b.c|31\")"
        RC=1
    fi

    if [ "$OK_415" -eq 1 ]; then
        echo "cli_serve_formulaire_urlencoded_415: RESULT: GREEN (Content-Type JSON sur une route Formulaire rejeté en 415)"
    else
        echo "cli_serve_formulaire_urlencoded_415: RESULT: RED (MAUVAISE raison -- code=$CODE_415, attendu 415)"
        RC=1
    fi

    if [ "$OK_NOM" -eq 1 ] && [ "$OK_415" -eq 1 ]; then
        RC=0
    fi
    if [ "$RC" -ne 0 ]; then
        echo "--- sortie 'owl serve' ---"
        echo "$CONTENU"
    fi
elif [ "$STATE" = "sorti" ]; then
    CONTENU="$(cat "$OUT")"
    rm -f "$OUT"
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    if echo "$CONTENU" | grep -q "Formulaire" \
        && echo "$CONTENU" | grep -qi "OpérationHTTP"; then
        echo "cli_serve_formulaire_urlencoded: RESULT: RED (bonne raison -- 'Formulaire' inconnue pour OpérationHTTP, propriété pas encore implémentée)"
    else
        echo "cli_serve_formulaire_urlencoded: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessous)"
        echo "--- sortie 'owl serve' ---"
        echo "$CONTENU"
    fi
    RC=1
else
    CONTENU="$(cat "$OUT")"
    rm -f "$OUT"
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    echo "cli_serve_formulaire_urlencoded: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
    echo "--- sortie 'owl serve' ---"
    echo "$CONTENU"
    RC=1
fi

exit $RC
