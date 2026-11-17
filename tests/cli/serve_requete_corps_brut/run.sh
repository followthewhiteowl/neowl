#!/usr/bin/env bash
# Harnais boîte noire pour `owl serve` — `MoiMême..Requête..Corps`, lecture du
# corps brut de la requête entrante (, voie b), symétrique de
# `MoiMême..Réponse..Corps` déjà en place (40_services_rest.md §6).
#
# Fixture : tests/internal_fixtures/cli_serve_requete_corps_brut/
# service_requete_corps_brut.owl — une route POST /api/echo dont le handler
# renvoie `MoiMême..Requête..Corps` tel quel.
#
# AUJOURD'HUI (propriété absente) : `Corps` n'est pas une sous-propriété
# reconnue de `MoiMême..Requête` (cf. §6 : Méthode/Chemin/Identité/Entêtes
# seulement) -- refus à la COMPILATION, avant que `owl serve` ne monte.
# Rouge attendu, bonne raison si le message cite 'Corps' et la requête d'un
# service.
#
# APRÈS IMPLÉMENTATION : la compilation réussit, le service monte, et un POST
# avec un corps arbitraire (texte quelconque, quel que soit le Content-Type)
# est renvoyé octet pour octet -- vérifié par `cmp`, pas par une substitution
# de commande qui mangerait un éventuel newline final (cf piège harnais
# connu, tests/cli/serve_sse_evenements/run.sh).
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si le corps brut est restitué byte-exact (vert attendu après
# implémentation), 1 si la compilation échoue pour une AUTRE raison que la
# propriété 'Corps' inconnue, ou si le corps obtenu diverge, 2 si owl.exe
# introuvable. L'échec attendu AUJOURD'HUI (compile error sur 'Corps') sort
# en 1 -- c'est le rouge voulu.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT_DIR="$OWL_ROOT/tests/internal_fixtures/cli_serve_requete_corps_brut"
FICHIER="service_requete_corps_brut.owl"
PORT="${OWL_SERVE_TEST_PORT:-19102}"

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
CODE=""
TMP_BODY=""
TMP_SENT=""
if [ "$STATE" = "monte" ]; then
    TMP_SENT="$(mktemp)"
    # Corps arbitraire, pas un simple champ urlencoded : ponctuation et un
    # newline final, pour prouver la restitution octet pour octet quel que
    # soit le Content-Type (curl --data-binary pose text/plain par défaut,
    # pas x-www-form-urlencoded).
    printf 'corps=brut&x=1\nligne2\n' > "$TMP_SENT"
    TMP_BODY="$(mktemp)"
    CODE="$(curl -s -o "$TMP_BODY" -w '%{http_code}' \
        --data-binary "@$TMP_SENT" \
        "http://127.0.0.1:$PORT/api/echo" 2>/dev/null)"
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

case "$STATE" in
    monte)
        if [ "$CODE" = "200" ] && cmp -s "$TMP_BODY" "$TMP_SENT"; then
            echo "cli_serve_requete_corps_brut: RESULT: GREEN (corps brut restitué byte-exact)"
            RC=0
        else
            echo "cli_serve_requete_corps_brut: RESULT: RED (MAUVAISE raison -- code=$CODE, corps obtenu (od -c) ci-dessous, attendu (od -c) ensuite)"
            echo "--- corps obtenu ---"
            od -c "$TMP_BODY" 2>/dev/null
            echo "--- corps attendu ---"
            od -c "$TMP_SENT" 2>/dev/null
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
            RC=1
        fi
        ;;
    sorti)
        if echo "$CONTENU" | grep -q "Corps" \
            && echo "$CONTENU" | grep -qi "la requête d'un service"; then
            echo "cli_serve_requete_corps_brut: RESULT: RED (bonne raison -- 'Corps' inconnue pour MoiMême..Requête, propriété pas encore implémentée)"
        else
            echo "cli_serve_requete_corps_brut: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessous)"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
        fi
        RC=1
        ;;
    timeout)
        echo "cli_serve_requete_corps_brut: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
        RC=1
        ;;
esac

rm -f "$TMP_BODY" "$TMP_SENT" 2>/dev/null

exit $RC
