#!/usr/bin/env bash
# Harnais boite noire : garde de FRONTIERE DE PROJET de owl-build.
#
# Contexte (cf. *.md) : owl-build lance depuis
# un dossier qui n'est PAS lui-meme une frontiere de projet (pas de projet.owl
# a son top-level) recursivait quand meme tout le sous-arbre, ramassait a plat
# des marqueurs de projets etrangers enfouis dans des sous-dossiers sans
# rapport, et firait le garde-fou "deux graphies ambigues" avec un message
# trompeur (cas du home d'un premier utilisateur : un projet.owl FR et un
# project.owl EN dans deux sous-dossiers distincts de Downloads/).
#
# Le fix verifie la presence d'un marqueur de projet AU TOP-LEVEL du dossier
# donne (pas en recursif) AVANT le scan :
#   - 0 marqueur top-level  -> "aucun projet.owl dans '<dir>'" + forme minimale
#   - 2 marqueurs top-level -> ambiguite LEGITIME (projet.owl + project.owl dans
#                              le MEME dossier), message "garder une seule
#                              graphie" correctement scope a la frontiere
#   - 1 marqueur top-level  -> on procede au scan recursif (frontiere legitime)
#
# Vehicule : owl-build.exe. ATTENTION — owl-build est EXCLUDE_FROM_ALL dans
# CMakeLists.txt : `make` ne le construit PAS (c'est le mega, bati via
# `make mega`). Ce harnais n'est donc PAS cable dans la batterie make/CI ;
# il SKIP proprement (exit 0) si le binaire est absent. A rejouer a la main
# apres un `make mega` quand on touche build_project / la frontiere.
#
# Usage:
#   ./run.sh
#   OWL_BUILD_BIN=/path/to/owl-build.exe ./run.sh
#
# Exit code : 0 si tous les cas passent (ou si owl-build absent = SKIP),
#             1 si au moins un cas diverge.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL_BUILD="${OWL_BUILD_BIN:-$OWL_ROOT/build/owl-build.exe}"

if [ ! -f "$OWL_BUILD" ]; then
    echo "build_frontier: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit 0
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_build_frontier"
FAIL=0

# Verifie que la sortie de owl-build sur `dir` (args suppl. optionnels)
# CONTIENT `must_have`, ne contient PAS `must_not`, et sort avec `want_rc`.
# must_not vide = pas de controle d'absence.
check_case() {
    local name="$1" dir="$2" want_rc="$3" must_have="$4" must_not="$5"
    shift 5
    local out rc
    out="$("$OWL_BUILD" "$dir" "$@" 2>&1)"
    rc=$?
    local ok=1
    if [ "$rc" != "$want_rc" ]; then
        echo "build_frontier_${name}: RESULT: RED (exit $rc, attendu $want_rc)"
        ok=0
    fi
    if ! printf '%s' "$out" | grep -qF "$must_have"; then
        echo "build_frontier_${name}: RESULT: RED (sortie ne contient pas: $must_have)"
        ok=0
    fi
    if [ -n "$must_not" ] && printf '%s' "$out" | grep -qF "$must_not"; then
        echo "build_frontier_${name}: RESULT: RED (sortie contient a tort: $must_not)"
        ok=0
    fi
    if [ "$ok" = 1 ]; then
        echo "build_frontier_${name}: RESULT: GREEN"
    else
        echo "  --- sortie owl-build ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
    fi
}

# --- Cas 1 : pas de marqueur top-level, projets enfouis dans des sous-dossiers
#             (reproduit le cas du premier utilisateur). Doit dire "pas de
#             projet ici", PAS l'ambiguite trompeuse. --------------------------
check_case "no_marker_fr" "$FIXT/no_marker" 65 \
    "aucun projet.owl" "ambigu"

# --- Cas 2 : deux marqueurs AU MEME top-level -> ambiguite legitime, scopee
#             a la frontiere. ------------------------------------------------
check_case "two_markers" "$FIXT/two_markers" 65 \
    "ambigu" "aucun projet.owl"

# --- Cas 3 : un seul marqueur top-level -> la frontiere laisse passer ;
#             owl-build procede (ici la source a une erreur de compile, donc on
#             s'arrete au portail compile). Non-regression : AUCUN message de
#             frontiere (ni "aucun projet.owl", ni "ambigu"). ------------------
check_case "nominal" "$FIXT/nominal" 65 \
    "ne compile pas" "ambigu"

# --- Cas 4 : variante EN du cas 1 (sous --en) -------------------------------
check_case "no_marker_en" "$FIXT/no_marker" 65 \
    "no projet.owl" "ambiguous" --en

# --- Cas 5 : parite front-end avec l'interpreteur. Un projet.owl
#             AU TOP-LEVEL (frontiere legitime, donc on passe le garde )
#             mais depourvu de bloc `est un Projet` : nom reserve sans
#             declaration. `owl <dir>` rejette (E_SEM_PROJET_BLOC_MANQUANT) ;
#             owl-build DOIT rejeter a l'identique via son portail de compile
#             (/2226) et NE PRODUIRE AUCUN exe. Regression guard : le
#             portail a deja ete desarme une fois (, faux-rejet UI) ;
#             sans ce verrou, un futur desarmement reintroduit la divergence en
#             silence. --------------------------------------------------------
rm -rf "$FIXT/reserved_no_block/exe"
check_case "reserved_no_block" "$FIXT/reserved_no_block" 65 \
    "est un Projet" "ambigu"
# ET aucun exe produit (critere ).
if find "$FIXT/reserved_no_block/exe" -name '*.exe' 2>/dev/null | grep -q .; then
    echo "build_frontier_reserved_no_block: RESULT: RED (un exe a ete produit a tort)"
    FAIL=1
else
    echo "build_frontier_reserved_no_block_no_exe: RESULT: GREEN"
fi
rm -rf "$FIXT/reserved_no_block/exe"

# --- Cas 6 : un build qui ECHOUE ne doit laisser AUCUN dossier `exe/`.
#             Avant le fix, owl-build creait `<dir>/exe/<target>/` TOT, avant le
#             portail de compile ; un projet qui ne compile pas s'arretait au
#             portail (exit 65) mais laissait derriere lui un `exe/<target>/`
#             VIDE -> l'utilisateur voyait un dossier vide et croyait a un succes
#             sans resultat. Le fixture `nominal` ne compile pas (fonction
#             indefinie) : il s'arrete au portail, bien avant tout link, donc ce
#             cas est deterministe meme sans toolchain de link complete. Le
#             critere n'est plus "aucun .exe" (deja couvert, mais aveugle au
#             dossier vide) mais "aucun dossier exe/ du tout". ------------------
rm -rf "$FIXT/nominal/exe"
"$OWL_BUILD" "$FIXT/nominal" >/dev/null 2>&1
rc=$?
if [ "$rc" != 65 ]; then
    echo "build_frontier_fail_no_exe_dir: RESULT: RED (exit $rc, attendu 65)"
    FAIL=1
elif [ -d "$FIXT/nominal/exe" ]; then
    echo "build_frontier_fail_no_exe_dir: RESULT: RED (un dossier exe/ a ete cree par un build rate)"
    FAIL=1
else
    echo "build_frontier_fail_no_exe_dir: RESULT: GREEN"
fi
rm -rf "$FIXT/nominal/exe"

exit $FAIL
