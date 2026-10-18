#!/usr/bin/env bash
# Harnais boite noire : un projet qui declare un ServiceREST, package par
# owl-build, produit un exe SERVEUR — pas un exe run-once qui
# sortirait aussitot le top-level fini ("starts then stops").
#
# Contexte (cf. *.md) : owl-build detecte un
# bloc `<Nom> est un ServiceREST` dans la frontiere projet (registre
# compile-time peuple par le portail de compile) et emet un main de montage
# serve (bind listener + boucle accept, meme mecanique qu'`owl serve`) au lieu
# du project_run one-shot. L'exe produit se configure A L'EXECUTION :
#   <exe> [--port N] [--bind ADR]   (defauts 127.0.0.1:8080 ; 0.0.0.0 = reseau)
#
# Ce harnais prouve sur un vrai exe empaquete :
#   - round-trip : l'exe lance avec --port <libre> bind, repond a GET /ping par
#     le corps "pong-feat2240", puis est arrete proprement ;
#   - argv : --help sort 0 avec une ligne Usage ; un --port invalide sort != 0.
#
# Vehicule : owl-build.exe. ATTENTION — owl-build est EXCLUDE_FROM_ALL dans
# CMakeLists.txt : `make` ne le construit PAS (c'est le mega, bati via
# `make mega`). Ce harnais n'est donc PAS cable dans la batterie make/CI ;
# il SKIP proprement (exit 0) si le binaire est absent OU si le link est
# incomplet dans ce build dir (aucun exe produit). A rejouer apres un
# `make mega` quand on touche la generation du main serveur (build_project).
#
# Usage:
#   ./run.sh
#   OWL_BUILD_BIN=/path/to/owl-build.exe ./run.sh
#   OWL_BUILD_SERVE_PORT=NNNNN ./run.sh
#
# Exit code : 0 si tous les cas passent (ou SKIP), 1 si au moins un diverge.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL_BUILD="${OWL_BUILD_BIN:-$OWL_ROOT/build/owl-build.exe}"

if [ ! -f "$OWL_BUILD" ]; then
    echo "build_serve: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit 0
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_build_serve"
PORT="${OWL_BUILD_SERVE_PORT:-18749}"
BODY="pong-feat2240"
FAIL=0

# Nettoie toute sortie precedente avant/apres (exe non commite).
rm -rf "$FIXT/exe"
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$FIXT/exe"
}
trap cleanup EXIT

# 1. Empaquette la fixture serveur. Un echec de LINK (libs vendor non stagees
#    dans un build dir dev) n'est PAS un echec de test : on SKIP si aucun exe
#    n'est produit.
build_out="$("$OWL_BUILD" "$FIXT" 2>&1)"
build_rc=$?

EXE="$(find "$FIXT/exe" -type f \( -name '*.exe' -o -perm -u+x \) 2>/dev/null | head -n 1)"
if [ -z "$EXE" ]; then
    echo "build_serve: SKIP (aucun exe produit — link incomplet dans ce build dir, cf. ticket)"
    echo "  --- sortie owl-build (exit $build_rc) ---"
    printf '%s\n' "$build_out" | sed 's/^/  /'
    exit 0
fi

# Mini-client HTTP GET sur 127.0.0.1:$PORT$1 — curl si dispo, sinon /dev/tcp.
# Echo le corps de reponse (ligne(s) apres l'en-tete) sur stdout.
http_get() {
    local path="$1"
    if command -v curl >/dev/null 2>&1; then
        curl -s --max-time 3 "http://127.0.0.1:$PORT$path"
        return $?
    fi
    exec 3<>"/dev/tcp/127.0.0.1/$PORT" || return 1
    printf 'GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' "$path" >&3
    # Separe en-tete et corps sur la premiere ligne vide.
    local line body="" in_body=0
    while IFS= read -r line <&3; do
        line="${line%$'\r'}"
        if [ "$in_body" = 1 ]; then
            body="$body$line"
        elif [ -z "$line" ]; then
            in_body=1
        fi
    done
    exec 3<&- 3>&-
    printf '%s' "$body"
}

# 2. Lance l'exe serveur sur un port libre, en tache de fond.
SRV_OUT="$(mktemp)"
"$EXE" --port "$PORT" > "$SRV_OUT" 2>&1 &
SERVER_PID=$!

# Attente bornee que le port reponde (le montage bind est synchrone cote serve,
# mais le process met un instant a demarrer son thread de boucle).
DEADLINE=$((SECONDS + 8))
READY=0
while [ $SECONDS -lt $DEADLINE ]; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break  # le serveur est mort avant d'ecouter
    fi
    if resp="$(http_get /ping)" && printf '%s' "$resp" | grep -qF "$BODY"; then
        READY=1
        break
    fi
    sleep 0.3
done

if [ "$READY" = 1 ]; then
    echo "build_serve_round_trip: RESULT: GREEN (GET /ping -> $BODY)"
else
    echo "build_serve_round_trip: RESULT: RED (pas de reponse '$BODY' sur GET /ping sous 8s)"
    echo "  --- sortie serveur ---"
    sed 's/^/  /' "$SRV_OUT"
    FAIL=1
fi

# Arret du serveur (le reste des cas est synchrone).
if kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
fi
SERVER_PID=""
rm -f "$SRV_OUT"

# 3. --help : exit 0 + usage enrichi (ligne Usage, defaut port 8080, Ctrl-C).
help_out="$("$EXE" --help 2>&1)"
help_rc=$?
if [ "$help_rc" = 0 ] \
   && printf '%s' "$help_out" | grep -qF "Usage:" \
   && printf '%s' "$help_out" | grep -qF "8080" \
   && printf '%s' "$help_out" | grep -qF "Ctrl-C"; then
    echo "build_serve_help: RESULT: GREEN"
else
    echo "build_serve_help: RESULT: RED (exit $help_rc, sortie: $help_out)"
    FAIL=1
fi

# 4. Port invalide : exit != 0.
"$EXE" --port 99999 >/dev/null 2>&1
bad_rc=$?
if [ "$bad_rc" != 0 ]; then
    echo "build_serve_bad_port: RESULT: GREEN (exit $bad_rc)"
else
    echo "build_serve_bad_port: RESULT: RED (port invalide accepte, exit 0)"
    FAIL=1
fi

exit $FAIL
