#!/usr/bin/env bash
# Harnais boite noire : owl-build NU affiche l'usage et sort.
#
# Contexte (cf. *.md) : owl-build lance SANS
# dossier projet positionnel (invocation nue, ou flags seuls sans dossier)
# preinitialisait dir="." et partait builder+extraire le dossier courant. Un
# premier utilisateur a tape `owl-build` pour "voir les commandes" et a
# declenche l'extraction de 978 Mo + un build de son home.
#
# Le fix pose dir=NULL comme sentinelle (pose uniquement par un argument
# positionnel) et, si aucun dossier n'est fourni en fin de parsing, affiche
# l'usage et sort (exit 0) AVANT toute extraction ou build. Pour builder le
# dossier courant, l'utilisateur tape desormais `owl-build .` explicitement.
#
# NB extraction : le binaire TARGET owl-build.exe (relinke par
# `cmake --build build --target owl-build`) n'a PAS de payload (seul le mega
# en a), donc il n'extrait rien de toute facon. L'assertion "pas
# d'extraction" se teste ici via l'absence du message d'empaquetage + exit 0
# sur l'usage. Le vrai gain 978 Mo concerne le mega mais decoule
# mecaniquement du return avant owl_payload_setup.
#
# Vehicule : owl-build.exe. ATTENTION — owl-build est EXCLUDE_FROM_ALL dans
# CMakeLists.txt : `make` ne le construit PAS. Ce harnais n'est donc PAS
# cable dans la batterie make/CI ; il SKIP proprement (exit 0) si le binaire
# est absent. A rejouer a la main apres un relink de owl-build quand on
# touche build_main.c / la frontiere.
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
    echo "build_usage: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit 0
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_build_frontier"
FAIL=0

report() {
    local name="$1" ok="$2" out="$3"
    if [ "$ok" = 1 ]; then
        echo "build_usage_${name}: RESULT: GREEN"
    else
        echo "  --- sortie owl-build ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
    fi
}

# Verifie que la sortie CONTIENT `must_have`, ne contient PAS `must_not`
# (vide = pas de controle), et que l'exit vaut `want_rc`. Les arguments
# apres want_rc/must_have/must_not sont passes tels quels a owl-build.
run_case() {
    local name="$1" want_rc="$2" must_have="$3" must_not="$4"
    shift 4
    local out rc
    out="$("$OWL_BUILD" "$@" 2>&1)"
    rc=$?
    local ok=1
    if [ "$rc" != "$want_rc" ]; then
        echo "build_usage_${name}: RESULT: RED (exit $rc, attendu $want_rc)"
        ok=0
    fi
    if [ -n "$must_have" ] && ! printf '%s' "$out" | grep -qF "$must_have"; then
        echo "build_usage_${name}: RESULT: RED (sortie ne contient pas: $must_have)"
        ok=0
    fi
    if [ -n "$must_not" ] && printf '%s' "$out" | grep -qF "$must_not"; then
        echo "build_usage_${name}: RESULT: RED (sortie contient a tort: $must_not)"
        ok=0
    fi
    report "$name" "$ok" "$out"
}

# --- Cas 1 : invocation NUE (aucun argument) -> usage + exit 0, AUCUN
#             empaquetage. On lance depuis un dossier temp propre et on
#             verifie en plus qu'aucun exe/ n'y a ete cree. ------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
out_bare="$(cd "$TMP" && "$OWL_BUILD" 2>&1)"
rc_bare=$?
ok=1
if [ "$rc_bare" != 0 ]; then
    echo "build_usage_nu: RESULT: RED (exit $rc_bare, attendu 0)"; ok=0
fi
if ! printf '%s' "$out_bare" | grep -qF "owl-build ["; then
    echo "build_usage_nu: RESULT: RED (sortie ne contient pas l'usage: 'owl-build [')"; ok=0
fi
if printf '%s' "$out_bare" | grep -qiE "empaquetage|packaging"; then
    echo "build_usage_nu: RESULT: RED (sortie contient un message d'empaquetage — build lance a tort)"; ok=0
fi
if [ -d "$TMP/exe" ]; then
    echo "build_usage_nu: RESULT: RED (un dossier exe/ a ete cree — build lance a tort)"; ok=0
fi
report "nu" "$ok" "$out_bare"

# --- Cas 1bis : invocation avec flags SEULS (langue, pas de dossier) -> usage
#                + exit 0. Une sentinelle robuste ne doit pas confondre un flag
#                consomme avec un dossier positionnel. ----------------------
run_case "flags_seuls" 0 "owl-build [" "empaquetage" --fr

# --- Cas 2 : owl-build . dans un dossier projet a 1 marqueur top-level ->
#             passe la frontiere et procede (ici compile error dans la
#             fixture nominal), AUCUN usage affiche. Non-regression du build
#             du dossier courant explicite. --------------------------------
out_dot="$(cd "$FIXT/nominal" && "$OWL_BUILD" . 2>&1)"
rc_dot=$?
ok=1
# Le dossier courant explicite doit depasser la grille d'usage : soit on
# atteint l'empaquetage/portail compile, soit une erreur de resolution —
# jamais le texte d'usage. On verifie l'absence du synopsis d'usage.
if printf '%s' "$out_dot" | grep -qF "owl-build ["; then
    echo "build_usage_dot: RESULT: RED (l'usage s'affiche alors qu'un dossier '.' est fourni)"; ok=0
fi
# Preuve que le build a bien demarre sur le dossier courant : empaquetage lance.
if ! printf '%s' "$out_dot" | grep -qiE "empaquetage|packaging"; then
    echo "build_usage_dot: RESULT: RED (le build du dossier courant '.' n'a pas demarre)"; ok=0
fi
report "dot" "$ok" "$out_dot"

# --- Cas 3 : dossier explicite -> non-regression (procede, pas d'usage). ----
run_case "dir_explicite" 65 "ne compile pas" "owl-build [" "$FIXT/nominal"

# --- Cas 4 : --help / -v inchanges ------------------------------------------
run_case "help" 0 "owl-build [" "empaquetage" --help
run_case "version" 0 "owl-build " "owl-build [" -v

# --- Cas 5 : variante EN de l'usage nu (flags seuls --en) -------------------
run_case "usage_en" 0 "OWL packager" "Packager OWL" --en

# --- Cas 6 : flags de BUILD (-o / --target) SANS dossier positionnel ->
#             intention = builder, dossier oublie. On distingue ce cas de
#             l'invocation nue : erreur actionnable sur stderr + exit != 0
#             (2 = mauvais usage), PAS le dump d'usage muet exit 0.
#             must_not "owl-build [" prouve que le synopsis d'usage n'est PAS
#             crache ; le message d'erreur renvoie vers `owl-build .`. --------
run_case "flags_sans_dossier" 2 "aucun dossier de projet indiqué" "owl-build [" -o TestBuild2
run_case "flags_sans_dossier_en" 2 "no project directory given" "owl-build [" --en -o TestBuild2
run_case "target_sans_dossier" 2 "aucun dossier de projet indiqué" "owl-build [" --target linux-x86_64

exit $FAIL
