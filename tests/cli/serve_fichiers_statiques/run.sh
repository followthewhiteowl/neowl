#!/usr/bin/env bash
# Harnais boite noire pour `owl serve` — montage statique déclaratif sur un
# ServiceREST : un préfixe d'URL + un dossier disque, fichiers
# servis avec le Content-Type dérivé de l'extension.
#
# Fixture : tests/internal_fixtures/cli_serve_fichiers_statiques/
# service_fichiers_statiques.owl déclare `Statique = {Préfixe: ["static"],
# Dossier: "assets"}` sous UrlBase = ["api"] -> préfixe effectif
# /api/static/. Le dossier ./assets contient app.css, sub/x.js, logo.svg,
# page.html.
#
# AUJOURD'HUI (propriété absente) : `owl serve` échoue à la COMPILATION
# avant de monter -- la propriété `Statique` est inconnue de `ServiceREST`.
# Rouge attendu, bonne raison si le message cite 'Statique' comme propriété
# inconnue (voir probe isolée : un seul message d'erreur, sans cascade).
#
# APRÈS IMPLÉMENTATION : le service démarre, chaque fichier légitime est
# servi octet pour octet (vérifié par `cmp`, jamais `$(cat)` qui mangerait
# un newline final) avec le bon Content-Type, et un fichier absent sous le
# préfixe renvoie un 404 propre.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si tous les cas légitimes + le 404 passent (vert attendu
# après implémentation), 1 si la compilation échoue pour une AUTRE raison
# que 'Statique' inconnue, ou si un des cas diverge, 2 si owl.exe
# introuvable. L'échec attendu AUJOURD'HUI (compile error sur 'Statique')
# sort en 1 -- c'est le rouge voulu.

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
PORT="${OWL_SERVE_TEST_PORT:-19311}"

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
declare -A CODES
declare -A OK

if [ "$STATE" = "monte" ]; then
    BASE="http://127.0.0.1:$PORT/api/static"

    # 1) CSS legitime, octet pour octet
    TMP_CSS="$(mktemp)"
    CODES[css]="$(curl -s -o "$TMP_CSS" -D - "$BASE/app.css" 2>/dev/null | head -1 | tr -d '\r\n')"
    CT_CSS="$(curl -s -o /dev/null -D - "$BASE/app.css" 2>/dev/null | grep -i '^content-type:' | tr -d '\r')"
    cmp -s "$TMP_CSS" "$FIXT_DIR/assets/app.css" && [[ "$CT_CSS" == *"text/css"* ]] && OK[css]=1 || OK[css]=0

    # 2) JS dans un sous-dossier
    TMP_JS="$(mktemp)"
    curl -s -o "$TMP_JS" "$BASE/sub/x.js" 2>/dev/null
    CT_JS="$(curl -s -o /dev/null -D - "$BASE/sub/x.js" 2>/dev/null | grep -i '^content-type:' | tr -d '\r')"
    cmp -s "$TMP_JS" "$FIXT_DIR/assets/sub/x.js" && [[ "$CT_JS" == *"javascript"* ]] && OK[js]=1 || OK[js]=0

    # 3) SVG
    TMP_SVG="$(mktemp)"
    curl -s -o "$TMP_SVG" "$BASE/logo.svg" 2>/dev/null
    CT_SVG="$(curl -s -o /dev/null -D - "$BASE/logo.svg" 2>/dev/null | grep -i '^content-type:' | tr -d '\r')"
    cmp -s "$TMP_SVG" "$FIXT_DIR/assets/logo.svg" && [[ "$CT_SVG" == *"svg"* ]] && OK[svg]=1 || OK[svg]=0

    # 4) HTML
    TMP_HTML="$(mktemp)"
    curl -s -o "$TMP_HTML" "$BASE/page.html" 2>/dev/null
    CT_HTML="$(curl -s -o /dev/null -D - "$BASE/page.html" 2>/dev/null | grep -i '^content-type:' | tr -d '\r')"
    cmp -s "$TMP_HTML" "$FIXT_DIR/assets/page.html" && [[ "$CT_HTML" == *"html"* ]] && OK[html]=1 || OK[html]=0

    # 5) fichier absent sous le prefixe -> 404 propre
    CODE_404="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/absent.css" 2>/dev/null)"
    [ "$CODE_404" = "404" ] && OK[notfound]=1 || OK[notfound]=0

    rm -f "$TMP_CSS" "$TMP_JS" "$TMP_SVG" "$TMP_HTML" 2>/dev/null
fi

CONTENU="$(cat "$OUT")"
rm -f "$OUT"

if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
fi

case "$STATE" in
    monte)
        if [ "${OK[css]:-0}" = "1" ] && [ "${OK[js]:-0}" = "1" ] && [ "${OK[svg]:-0}" = "1" ] \
            && [ "${OK[html]:-0}" = "1" ] && [ "${OK[notfound]:-0}" = "1" ]; then
            echo "cli_serve_fichiers_statiques: RESULT: GREEN (css/js/svg/html servis byte-exact, bon Content-Type, 404 propre sur fichier absent)"
            RC=0
        else
            echo "cli_serve_fichiers_statiques: RESULT: RED (MAUVAISE raison -- détail des cas ci-dessous)"
            echo "  css=${OK[css]:-?} content-type=$CT_CSS"
            echo "  js=${OK[js]:-?} content-type=$CT_JS"
            echo "  svg=${OK[svg]:-?} content-type=$CT_SVG"
            echo "  html=${OK[html]:-?} content-type=$CT_HTML"
            echo "  notfound=${OK[notfound]:-?} code=$CODE_404"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
            RC=1
        fi
        ;;
    sorti)
        if echo "$CONTENU" | grep -q "Statique" && echo "$CONTENU" | grep -qi "inconnue"; then
            echo "cli_serve_fichiers_statiques: RESULT: RED (bonne raison -- propriété 'Statique' inconnue de ServiceREST, montage pas encore implémenté)"
        else
            echo "cli_serve_fichiers_statiques: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessous)"
            echo "--- sortie 'owl serve' ---"
            echo "$CONTENU"
        fi
        RC=1
        ;;
    timeout)
        echo "cli_serve_fichiers_statiques: RESULT: RED (timeout -- ni erreur de compilation ni montage détectés sous 5s)"
        RC=1
        ;;
esac

exit $RC
