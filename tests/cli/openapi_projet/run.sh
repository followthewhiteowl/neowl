#!/usr/bin/env bash
# Harnais boite noire pour `owl openapi` sur un projet multi-fichiers.
#
# Repro testeur : `owl openapi api.owl` compile aujourd'hui en MONO-FICHIER
# (`read_file` + `vm_interpret`, handle_openapi_command src/main.c ~l.7248) :
# la frontiere projet n'est pas resolue. Sur
# tests/internal_fixtures/cli_openapi/projet_multi/{projet.owl,
# etat_facture.owl, api.owl}, `api.owl` declare un ServiceREST dont
# l'operation reference `ETAT_Facture`, declare dans le voisin
# `etat_facture.owl` : la compilation echoue sur "'ETAT_Facture' inconnue".
# `owl serve` resout deja cette meme frontiere avec succes --
# incoherence CLI vs serveur que ce ticket referme.
#
# Second cas (non-regression) : `owl openapi` sur un fichier SANS ServiceREST
# doit conserver son message d'erreur clair, meme apres le passage a la
# resolution de frontiere.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si les deux cas se comportent comme attendu (bug corrige +
# message d'erreur conserve), 1 sinon, 2 si owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

PROJET_DIR="$OWL_ROOT/tests/internal_fixtures/cli_openapi/projet_multi"
SANS_SERVICE="$OWL_ROOT/tests/internal_fixtures/cli_openapi/sans_service/sans_service.owl"

FAIL=0

# --- Cas 1 : le voisin ETAT_Facture doit etre resolu (bug ) -------
SORTIE_MULTI="$(cd "$PROJET_DIR" && "$OWL" openapi api.owl 2>&1)"

if echo "$SORTIE_MULTI" | grep -qi "ETAT_Facture" && echo "$SORTIE_MULTI" | grep -qi "inconnu"; then
    echo "cli_openapi_projet_multi: RESULT: RED (bonne raison -- ETAT_Facture inconnue, bug fix/1990 reproduit)"
    FAIL=1
elif ! echo "$SORTIE_MULTI" | grep -q '"openapi"'; then
    echo "cli_openapi_projet_multi: RESULT: RED (MAUVAISE raison -- pas de document OpenAPI en sortie)"
    echo "--- sortie ---"
    echo "$SORTIE_MULTI"
    FAIL=1
else
    echo "cli_openapi_projet_multi: RESULT: GREEN (document OpenAPI genere, ETAT_Facture resolu)"
fi

# --- Cas 2 : message d'erreur "aucun ServiceREST" conserve (non-regression) --
SORTIE_SANS_SERVICE="$("$OWL" openapi "$SANS_SERVICE" 2>&1)"

if echo "$SORTIE_SANS_SERVICE" | grep -qi "aucun ServiceREST"; then
    echo "cli_openapi_sans_service: RESULT: GREEN (message d'erreur clair conserve)"
else
    echo "cli_openapi_sans_service: RESULT: RED (message d'erreur inattendu)"
    echo "--- sortie ---"
    echo "$SORTIE_SANS_SERVICE"
    FAIL=1
fi

exit $FAIL
