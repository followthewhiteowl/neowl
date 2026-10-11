#!/usr/bin/env bash
# Harnais boite noire pour `owl serve` en VRAI mode projet (,
# follow-up /) : cascade i18n de `projet.owl`.
#
# Repro : `handle_serve_command` (src/main.c ~7549-7557) force
# `projet.mode_projet = 0` (contournement , motive par le gap
# prescan ServiceREST). Consequence mesuree empiriquement (probe manuel,
# owl.exe actuel) : `project_run_ex` ne boote l'i18n
# (`owl_boot_project_i18n`, src/project_run.c:330-332) QUE si
# `project->isProjectMode` est vrai. Sous serve aujourd'hui, la langue
# declaree du projet (`Langues = [langueAnglais]`) n'est JAMAIS chargee --
# `LangueSource()` retombe sur le defaut "fr-FR" au lieu de "en-US".
#
# Fixture : tests/internal_fixtures/cli_serve_projet_i18n/{projet.owl,
# api.owl}. `projet.owl` declare `Langues = [langueAnglais]`. `api.owl`
# (ServiceREST-only, voisin) expose GET /langue -> corps = LangueSource().
#
# Deux montages testes (item 4 du ticket -- fichier explicite ET dossier) :
#   - `owl serve api.owl`  (cible = fichier)
#   - `owl serve .`        (cible = dossier, entree = projet.owl)
# Les deux DOIVENT renvoyer "en-US" une fois  lande (vrai mode
# projet). Aujourd'hui : "fr-FR" (repli par defaut, i18n jamais boote).
#
# `owl serve` reste bloque en ecoute (jusqu'a Ctrl-C) : meme patron que
# tests/cli/serve/run.sh -- lance en tache de fond, scrute la sortie avec
# une borne de temps, tue systematiquement le process a la fin (jamais de
# serveur de test qui survit a ce script).
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si les deux montages renvoient "en-US" (vert attendu apres
# fix), 1 si au moins un montage renvoie autre chose (rouge attendu
# aujourd'hui : "fr-FR"), 2 si owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_serve_projet_i18n"
FAIL=0

# Lance `owl serve $1` (cible relative au CWD $FIXT) sur le port $2, attend
# le montage (borne 5 s), interroge GET /langue, tue le process, imprime le
# resultat sous le label $3. Retourne 0 si "en-US", 1 sinon.
monte_et_interroge() {
    local cible="$1" port="$2" label="$3"
    local out; out="$(mktemp)"

    (cd "$FIXT" && "$OWL" serve "$cible" --port "$port") > "$out" 2>&1 &
    local pid=$!

    local deadline=$((SECONDS + 5))
    local state="timeout"
    while [ $SECONDS -lt $deadline ]; do
        if grep -q "en écoute" "$out" 2>/dev/null || grep -q "en ecoute" "$out" 2>/dev/null; then
            state="monte"
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            state="sorti"
            break
        fi
        sleep 0.2
    done

    local corps="" rc=1
    if [ "$state" = "monte" ]; then
        corps="$(curl -s "http://127.0.0.1:$port/langue" 2>/dev/null)"
    fi

    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    fi

    local contenu; contenu="$(cat "$out")"
    rm -f "$out"

    case "$state" in
        monte)
            if [ "$corps" = "en-US" ]; then
                echo "$label: RESULT: GREEN (LangueSource = en-US, i18n de projet boote sous serve)"
                rc=0
            else
                echo "$label: RESULT: RED (bonne raison -- LangueSource = '$corps' (attendu en-US), serve tourne encore en mode script sans boot i18n, bug fix/1986 reproduit)"
                rc=1
            fi
            ;;
        sorti)
            echo "$label: RESULT: RED (MAUVAISE raison -- process sorti avant montage)"
            echo "--- sortie ---"
            echo "$contenu"
            rc=1
            ;;
        timeout)
            echo "$label: RESULT: RED (timeout -- ni montage ni sortie detectes sous 5s)"
            rc=1
            ;;
    esac
    return $rc
}

monte_et_interroge "api.owl" "${OWL_SERVE_TEST_PORT:-18821}" \
    "cli_serve_projet_i18n_fichier" || FAIL=1

monte_et_interroge "." "${OWL_SERVE_TEST_PORT_DIR:-18822}" \
    "cli_serve_projet_i18n_dossier" || FAIL=1

exit $FAIL
