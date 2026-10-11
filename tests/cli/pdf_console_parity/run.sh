#!/usr/bin/env bash
# Harnais boite noire : PARITE du verdict de compile-check entre le
# registre CONSOLE (owl-cli.exe, sans Qt6/UI/PDF) et le registre GUI
# (owl.exe) pour les natives PDF (, pilote 42 natives).
#
# Contexte (cf. *.md, section Re-scope) :
# owl-build compile le C dans un process CONSOLE (owl_core +
# owl_runtime_console, sans OWL_HAS_GUI). Avant le split
# stdlib_pdf_defs.c, ce registre console n'a AUCUNE signature PDF
# (stdlib_pdf_init gate inconditionnellement derriere OWL_HAS_GUI,
# owl_stdlib.c:95-98) : un projet PDF valide y est refuse, un projet PDF
# fautif y est refuse pour la MAUVAISE raison (symbole inconnu, pas
# l'erreur de signature attendue). Le contrat vise : apres le split,
# ce que le portail console resout/rejette doit matcher EXACTEMENT
# owl.exe (parite du verdict), puisque l'exe produit par owl-build
# recompile au demarrage avec le vrai registre GUI — le compile-check
# console n'est qu'un pur portail.
#
# owl-cli.exe est le vehicule de test retenu : meme interpreteur, meme
# stdlib core, SANS backend GUI Qt6/PDF (CMakeLists.txt "Cible :
# owl-cli.exe (interpreteur sans Qt6/UI/PDF...)"), deja construit par
# `make` sous OWL_DEV_TOOLS=ON. Pas besoin du mega build owl-build pour
# exercer la resolution de natives en contexte console.
#
# Quatre fixtures isolees chacune dans son propre dossier (le mode
# SCRIPT d'OWL scanne les voisins du meme repertoire ; une fixture par
# dossier evite toute contamination croisee entre cas) :
#   valid_fr/  : iDestination(iPDFGénérique, ...) + iFinImprime() valide.
#                Parite positive, forme FR.
#   valid_en/  : iDestination(iGenericPDF, ...) + iEndPrinting() valide.
#                Parite positive, forme EN (bilingue, Regle 6 invariant 3).
#   arity/     : iFinImprime(42) — mauvaise arite (attend 0 argument).
#                Parite negative : meme message d'erreur de signature
#                attendu cote console et cote GUI.
#   enum/      : iImprimeZone(..., ttCroissant) — valeur hors du set
#                enum iAlignement attendu pour le parametre alignement.
#                Parite negative : meme message d'erreur d'appartenance
#                enum attendu cote console et cote GUI.
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe OWL_CLI_BIN=/path/to/owl-cli.exe ./run.sh
#
# Exit code : 0 si les quatre cas sont en parite (APRES le split
# stdlib_pdf_defs.c), 1 si au moins un cas diverge (AVANT le split :
# les quatre divergent, ROUGE ATTENDU), 2 si un des deux binaires est
# introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
OWL_CLI="${OWL_CLI_BIN:-$OWL_ROOT/build/owl-cli.exe}"

if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi
if [ ! -f "$OWL_CLI" ]; then
    echo "owl-cli.exe introuvable: $OWL_CLI — lance 'make' (OWL_DEV_TOOLS=ON) d'abord"
    exit 2
fi

FIXT="$OWL_ROOT/tests/internal_fixtures/cli_pdf_console_parity"
FAIL=0

check_parity_exact() {
    # $1 = nom du cas, $2 = chemin du fichier .owl cible
    local name="$1" file="$2"
    local out_gui out_cli
    out_gui="$("$OWL" --check "$file" 2>&1)"
    out_cli="$("$OWL_CLI" --check "$file" 2>&1)"
    if [ "$out_gui" = "$out_cli" ]; then
        echo "pdf_console_parity_${name}: RESULT: GREEN (console et GUI rendent le MEME verdict)"
    else
        echo "pdf_console_parity_${name}: RESULT: RED (verdicts divergents entre console et GUI)"
        echo "  --- owl.exe (GUI) ---"
        echo "  $out_gui"
        echo "  --- owl-cli.exe (console) ---"
        echo "  $out_cli"
        FAIL=1
    fi
}

# --- Cas 1 : projet PDF valide, forme FR -----------------------------------
check_parity_exact "valid_fr" "$FIXT/valid_fr/valid.owl"

# --- Cas 2 : projet PDF valide, forme EN -----------------------------------
check_parity_exact "valid_en" "$FIXT/valid_en/valid_en.owl"

# --- Cas 3 : mauvaise arite — meme message de signature attendu -----------
check_parity_exact "arity" "$FIXT/arity/arity.owl"

# --- Cas 4 : valeur enum hors set — meme message d'appartenance attendu ---
check_parity_exact "enum" "$FIXT/enum/enum.owl"

exit $FAIL
