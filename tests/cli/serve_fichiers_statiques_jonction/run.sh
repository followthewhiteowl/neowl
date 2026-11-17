#!/usr/bin/env bash
# Harnais boite noire pour `owl serve` — garde anti-traversée du montage
# statique déclaratif, vecteur JONCTION/LIEN SYMBOLIQUE à
# l'intérieur du dossier monté (distinct du vecteur URL de
# serve_fichiers_statiques_traversee/run.sh).
#
# La garde anti-traversée actuelle est purement LEXICALE sur l'URL : elle
# bloque "../" et ses variantes d'encodage, mais ne fait aucune résolution
# canonique (realpath / GetFinalPathNameByHandle) du chemin final sur
# disque. Donc une jonction (Windows) ou un lien symbolique (POSIX) posé
# DANS le dossier monté et qui pointe HORS de ce dossier n'est jamais vu
# par la garde lexicale : l'URL elle-même ne contient aucun "..".
#
# Fixture : tests/internal_fixtures/cli_serve_fichiers_statiques/
#   - service_fichiers_statiques.owl monte ./assets sous /api/static/
#   - secret_hors_montage/secret_hors.txt vit HORS de ./assets (commité,
#     fichier plat ordinaire, contenu connu)
#   - CE harnais crée lui-même, au SETUP, une jonction/lien
#     assets/escape -> ../secret_hors_montage (jamais commitée : git ne
#     versionne pas proprement une jonction Windows) et la supprime au
#     TEARDOWN, succès ou échec, sans résidu.
#
# Pas de cas de contrôle "jonction légitime interne" dans ce harnais : le
# seul vecteur qui compte ici est la fuite hors montage (priorité du
# ticket). Le cas "fichier légitime servi normalement" est déjà couvert
# par serve_fichiers_statiques/run.sh (sans jonction).
#
# AUJOURD'HUI (garde purement lexicale, trou démontré par le reviewer
# ) : GET /api/static/escape/secret_hors.txt répond 200 avec le
# contenu de secret_hors.txt -> FUITE CONFIRMEE. Rouge attendu, pour la
# BONNE raison (fuite réelle, pas une erreur de montage ou de compilation).
#
# APRÈS LE FIX (résolution canonique + contrôle de confinement
# post-résolution) : la même requête doit répondre 404 (le réel du fichier
# résolu sort du dossier monté).
#
# Si la plateforme ne permet pas de créer la jonction/le lien (droits,
# outil absent) : SKIP explicite (exit 2), jamais un vert par défaut.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si la fuite est bloquée (vert attendu après le fix),
# 1 si la fuite est confirmée (rouge attendu aujourd'hui) ou si le montage
# échoue pour une autre raison, 2 si owl.exe ou la création de la
# jonction/lien est impossible sur cette plateforme (SKIP).

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
PORT="${OWL_SERVE_TEST_PORT:-19313}"

LINK_PATH="$FIXT_DIR/assets/escape"
TARGET_DIR="$FIXT_DIR/secret_hors_montage"
SECRET_FILE="$TARGET_DIR/secret_hors.txt"

if [ ! -f "$SECRET_FILE" ]; then
    echo "fixture incomplete : $SECRET_FILE absent"
    exit 2
fi

PID=""
LINK_CREATED=0

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    if [ "$LINK_CREATED" = "1" ]; then
        rm -rf "$LINK_PATH" 2>/dev/null
    fi
}
trap cleanup EXIT

# --- SETUP : jonction (Windows) ou lien symbolique (POSIX) dans assets/ ---
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        if ! command -v cygpath >/dev/null 2>&1; then
            echo "cli_serve_fichiers_statiques_jonction: SKIP (cygpath introuvable, impossible de créer la jonction)"
            exit 2
        fi
        WIN_LINK="$(cygpath -w "$LINK_PATH")"
        WIN_TARGET="$(cygpath -w "$TARGET_DIR")"
        if MSYS2_ARG_CONV_EXCL="/J" cmd //c mklink /J "$WIN_LINK" "$WIN_TARGET" >/dev/null 2>&1; then
            LINK_CREATED=1
        fi
        ;;
    *)
        if ln -s "$TARGET_DIR" "$LINK_PATH" 2>/dev/null; then
            LINK_CREATED=1
        fi
        ;;
esac

if [ "$LINK_CREATED" != "1" ] || [ ! -e "$LINK_PATH" ]; then
    echo "cli_serve_fichiers_statiques_jonction: SKIP (impossible de créer la jonction/lien sur cette plateforme)"
    exit 2
fi

# --- Lance owl serve ---
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
LEAK=0
RAPPORT=""

if [ "$STATE" = "monte" ]; then
    BASE="http://127.0.0.1:$PORT/api/static"

    TMP_BODY="$(mktemp)"
    CODE="$(curl -s -o "$TMP_BODY" -w '%{http_code}' "$BASE/escape/secret_hors.txt" 2>/dev/null)"
    if [ "$CODE" = "200" ] && cmp -s "$TMP_BODY" "$SECRET_FILE"; then
        RAPPORT+="  escape/secret_hors.txt: FUITE CONFIRMEE (code=200, corps = secret_hors_montage/secret_hors.txt)\n"
        LEAK=1
    elif [ "$CODE" = "200" ]; then
        RAPPORT+="  escape/secret_hors.txt: code=200 (corps different -- 200 reste un rejet attendu en echec, a verifier)\n"
        LEAK=1
    else
        RAPPORT+="  escape/secret_hors.txt: code=$CODE (rejete, OK)\n"
    fi
    rm -f "$TMP_BODY" 2>/dev/null
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

case "$STATE" in
    monte)
        if [ "$LEAK" = "0" ]; then
            echo "cli_serve_fichiers_statiques_jonction: RESULT: GREEN (jonction hors montage rejetee, aucune fuite de secret_hors.txt)"
            echo -e "$RAPPORT"
            RC=0
        else
            echo "cli_serve_fichiers_statiques_jonction: RESULT: RED (VULNERABILITE -- jonction/lien dans le dossier monte laisse fuiter un fichier hors montage)"
            echo -e "$RAPPORT"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
            RC=1
        fi
        ;;
    sorti)
        echo "cli_serve_fichiers_statiques_jonction: RESULT: RED (MAUVAISE raison -- 'owl serve' est sorti avant de monter)"
        echo "--- sortie 'owl serve' ---"
        echo "$CONTENU"
        RC=1
        ;;
    timeout)
        echo "cli_serve_fichiers_statiques_jonction: RESULT: RED (timeout -- montage non detecte sous 5s)"
        RC=1
        ;;
esac

exit $RC
