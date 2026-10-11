#!/usr/bin/env bash
# Harnais boite noire pour la participation ServiceREST au prescan de
# perimetre projet (, follow-up /).
#
# Repro : `prescan_after_est_is_dsl_block` (src/prescan.c:477-491) ne
# reconnait pas `ServiceREST` dans sa whitelist de blocs DSL conteneurs
# (Classe/Fenetre/Etat/Projet/Theme/CollectionDeProcedures). Consequence via
# `project_companion_in_compile_scope` (src/project_run.c) : sous un
# `projet.owl` (mode projet), un compagnon qui ne declare RIEN d'autre qu'un
# ServiceREST est classe "script plat prive" et EXCLU de la compilation --
# ni indexe, ni hydrate, ni compile. Verifie EMPIRIQUEMENT (probe manuel,
# owl.exe actuel) : une operation HTTP dupliquee (meme nom "Ping" deux fois
# dans le meme service, erreur normalement fatale
# E_SEM_DSL_OPERATION_NAME_DUPLICATE) n'est PAS detectee par `owl .` quand le
# ServiceREST est un compagnon sous projet.owl (exit 0, silencieux) -- alors
# que EXACTEMENT le meme contenu, lance en mode SCRIPT (`owl SvcSeul.owl`
# sans projet.owl ancetre), la detecte bien (exit 65, "deja declaree").
#
# Trois fixtures (tests/internal_fixtures/cli_prescan_servicerest/) :
#   - projet_service_seul_duplique/  : ServiceREST-only AVEC la duplication.
#     `owl .` DOIT rougir (bug  corrige) -- ROUGE ATTENDU aujourd'hui.
#   - projet_service_seul_valide/    : ServiceREST-only SANS erreur.
#     `owl .` doit rester propre, participe ou pas (non-regression faux
#     positif).
#   - projet_sans_service/           : aucun ServiceREST (voisin Classe
#     deja whitelistee). `owl .` doit rester propre (non-regression
#     cross-cutting : la whitelist ServiceREST ne doit RIEN changer ici).
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si les trois cas se comportent comme attendu APRES fix
# (duplication detectee + les deux non-regressions propres), 1 si au moins
# un cas diverge (avant fix : cas 1 rouge pour la bonne raison), 2 si
# owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_prescan_servicerest"
FAIL=0

# --- Cas 1 : ServiceREST-only + duplication -> DOIT etre detectee --------
SORTIE_DUP="$(cd "$FIXT/projet_service_seul_duplique" && "$OWL" . 2>&1)"
RC_DUP=$?

if [ $RC_DUP -ne 0 ] && echo "$SORTIE_DUP" | grep -qi "ping" \
        && (echo "$SORTIE_DUP" | grep -qi "déclar" ); then
    echo "cli_prescan_servicerest_duplique: RESULT: GREEN (participation ServiceREST -- duplication detectee)"
elif [ $RC_DUP -eq 0 ]; then
    echo "cli_prescan_servicerest_duplique: RESULT: RED (bonne raison -- ServiceREST-only exclu du perimetre projet, duplication muette, bug fix/1986 reproduit)"
    FAIL=1
else
    echo "cli_prescan_servicerest_duplique: RESULT: RED (MAUVAISE raison -- sortie inattendue, cf ci-dessous)"
    echo "--- sortie ---"
    echo "$SORTIE_DUP"
    FAIL=1
fi

# --- Cas 2 : ServiceREST-only valide -> DOIT rester propre ----------------
SORTIE_VALIDE="$(cd "$FIXT/projet_service_seul_valide" && "$OWL" . 2>&1)"
RC_VALIDE=$?

if [ $RC_VALIDE -eq 0 ]; then
    echo "cli_prescan_servicerest_valide: RESULT: GREEN (ServiceREST-only valide, aucun faux positif)"
else
    echo "cli_prescan_servicerest_valide: RESULT: RED (faux positif introduit -- sortie ci-dessous)"
    echo "--- sortie ---"
    echo "$SORTIE_VALIDE"
    FAIL=1
fi

# --- Cas 3 : aucun ServiceREST -> non-regression cross-cutting -----------
SORTIE_SANS="$(cd "$FIXT/projet_sans_service" && "$OWL" . 2>&1)"
RC_SANS=$?

if [ $RC_SANS -eq 0 ]; then
    echo "cli_prescan_servicerest_sans_service: RESULT: GREEN (non-regression : projet sans ServiceREST inchange)"
else
    echo "cli_prescan_servicerest_sans_service: RESULT: RED (regression cross-cutting -- sortie ci-dessous)"
    echo "--- sortie ---"
    echo "$SORTIE_SANS"
    FAIL=1
fi

exit $FAIL
