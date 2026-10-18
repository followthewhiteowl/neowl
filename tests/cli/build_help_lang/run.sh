#!/usr/bin/env bash
# Harnais boite noire : aide d'owl-build — casse du flag de langue, "defaut"
# francise, octets accentues UTF-8.
#
# Contexte (cf. *.md, report support #414) :
#   (a) le flag de langue etait compare avec strcmp -> `--EN`, `--lang EN`,
#       `--LANG en` etaient ignores et l'aide restait en FR. Fix : comparaison
#       insensible a la casse du flag ET de sa valeur.
#   (b) le bloc FR melait « default: » (anglais) et « defaut: » (sans accent).
#       Fix : tout le bloc FR en « défaut : » accentue.
#   (d) owl-build n'appelait jamais owl_force_utf8_console() -> mojibake des
#       accents sur une console Windows non-UTF-8. Fix : appel en tete de main.
#
# NB (d) : le CP de la console ne se teste pas de facon fiable en boite noire
# (ce shell tourne deja probablement en UTF-8). On se contente donc de verifier
# que l'aide FR sort bien les OCTETS UTF-8 attendus (« Pré-requis ») sans crash.
# Asserter le code-page serait un test fragile ; on documente la limite ici.
#
# Vehicule : owl-build.exe. ATTENTION — owl-build est EXCLUDE_FROM_ALL dans
# CMakeLists.txt : `make` ne le construit PAS. Ce harnais n'est donc PAS cable
# dans la batterie make/CI ; il SKIP proprement (exit 0) si le binaire est
# absent. A rejouer a la main apres un relink de owl-build quand on touche
# build_main.c.
#
# Usage :
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
    echo "build_help_lang: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit 0
fi

# Marqueurs stables de chaque variante de l'aide :
EN_MARKER="OWL project directory"      # present uniquement dans le bloc EN
FR_MARKER="Dossier du projet"          # present uniquement dans le bloc FR

FAIL=0

report() {
    local name="$1" ok="$2" out="$3"
    if [ "$ok" = 1 ]; then
        echo "build_help_lang_${name}: RESULT: GREEN"
    else
        echo "  --- sortie owl-build ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
    fi
}

# Verifie que la sortie CONTIENT `must_have`, ne contient PAS `must_not`
# (vide = pas de controle), et que l'exit vaut `want_rc`. Les arguments apres
# want_rc/must_have/must_not sont passes tels quels a owl-build.
run_case() {
    local name="$1" want_rc="$2" must_have="$3" must_not="$4"
    shift 4
    local out rc ok=1
    out="$("$OWL_BUILD" "$@" 2>&1)"
    rc=$?
    if [ "$rc" != "$want_rc" ]; then
        echo "build_help_lang_${name}: RESULT: RED (exit $rc, attendu $want_rc)"; ok=0
    fi
    if [ -n "$must_have" ] && ! printf '%s' "$out" | grep -qF "$must_have"; then
        echo "build_help_lang_${name}: RESULT: RED (sortie ne contient pas: $must_have)"; ok=0
    fi
    if [ -n "$must_not" ] && printf '%s' "$out" | grep -qF "$must_not"; then
        echo "build_help_lang_${name}: RESULT: RED (sortie contient a tort: $must_not)"; ok=0
    fi
    report "$name" "$ok" "$out"
}

# --- (a) flag de langue insensible a la casse -> aide EN -------------------
run_case "upper_EN"      0 "$EN_MARKER" "$FR_MARKER" --help --EN
run_case "lang_space_EN" 0 "$EN_MARKER" "$FR_MARKER" --help --lang EN
run_case "LANG_upper"    0 "$EN_MARKER" "$FR_MARKER" --help --LANG en
run_case "lang_eq_EN"    0 "$EN_MARKER" "$FR_MARKER" --help --lang=EN

# --- (a) non-regression : les formes minuscules marchent toujours ----------
run_case "lower_en"      0 "$EN_MARKER" "$FR_MARKER" --help --en
run_case "lower_fr"      0 "$FR_MARKER" "$EN_MARKER" --help --fr
run_case "lang_space_en" 0 "$EN_MARKER" "$FR_MARKER" --help --lang en

# --- (b) l'aide FR (defaut) ne doit plus afficher « default: », mais « défaut »
run_case "fr_no_default" 0 "défaut" "default:" --help

# --- (d) l'aide FR sort les octets UTF-8 accentues attendus, sans crash ----
#         (le CP console n'est pas testable de facon fiable ici, cf. entete)
run_case "fr_accents"    0 "Pré-requis" "" --help

exit $FAIL
