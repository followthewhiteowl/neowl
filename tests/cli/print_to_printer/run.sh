#!/usr/bin/env bash
# Lot 0  — verrou : iDestination(iImprimante) produit un flux
# d'impression REEL, plus un stub qui retombe sur du PDF fichier ou un warning.
#
# Headless : le backend derive le QPrinter vers un PDF fichier via
# OWL_PRINTER_OUTPUT_FILE (precedent getenv runtime), ce qui exerce le chemin
# painter->QPrinter->spool sans imprimante physique. Verdict : le fichier
# existe, est non vide, et commence par la signature %PDF (le QPrinter en mode
# PdfFormat a bien rendu les primitives).
#
# Usage :
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si le flux PDF attendu est produit, 1 sinon (ROUGE tant que
# iDestination(iImprimante) est un stub), 2 si owl.exe est introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"
OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"

if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

OUT="$ROOT/_print_out.pdf"
rm -f "$OUT"
trap 'rm -f "$OUT"' EXIT

# owl.exe est natif Windows : les variables d'env ne subissent pas la
# conversion de chemin MSYS (contrairement aux arguments). On passe donc un
# chemin Windows a slashes via cygpath -m, forme que Qt accepte directement.
OUT_WIN="$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")"

OWL_PRINTER_OUTPUT_FILE="$OUT_WIN" "$OWL" "$ROOT/print.owl"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL: owl.exe a retourne $rc sur la fixture d'impression"
    exit 1
fi

if [ ! -s "$OUT" ]; then
    echo "FAIL: aucun flux d'impression produit (fichier absent ou vide)"
    echo "       -> iDestination(iImprimante) n'a pas peint sur le QPrinter"
    exit 1
fi

magic="$(head -c 4 "$OUT")"
if [ "$magic" != "%PDF" ]; then
    echo "FAIL: le flux produit n'est pas un PDF (magic='$magic')"
    exit 1
fi

echo "PASS: iDestination(iImprimante) a produit un flux PDF de $(wc -c < "$OUT") octets"

# --- Lot 1 : iParametre recto-verso + couleur (round-trip set/get) ---
POUT="$ROOT/_params_out.pdf"
rm -f "$POUT"
trap 'rm -f "$OUT" "$POUT"' EXIT
POUT_WIN="$(cygpath -m "$POUT" 2>/dev/null || echo "$POUT")"

params_log="$(OWL_PRINTER_OUTPUT_FILE="$POUT_WIN" "$OWL" "$ROOT/params/params.owl" 2>&1)"
prc=$?
if [ "$prc" -ne 0 ]; then
    echo "FAIL: owl.exe a retourne $prc sur la fixture iParametre"
    echo "$params_log"
    exit 1
fi
for tok in PASS_COULEUR_MONO PASS_COULEUR_COUL PASS_DUPLEX_VERT PASS_DUPLEX_SIMPLE PASS_BAC_HAUT PASS_BAC_UNESEULE; do
    if ! printf '%s' "$params_log" | grep -q "$tok"; then
        echo "FAIL: round-trip iParametre : marqueur manquant ($tok)"
        echo "$params_log"
        exit 1
    fi
done
if printf '%s' "$params_log" | grep -q "FAIL_"; then
    echo "FAIL: round-trip iParametre a signale un echec de relecture"
    echo "$params_log"
    exit 1
fi
echo "PASS: iParametre recto-verso + couleur round-trip OK"

# --- Impression d'un Etat (Format non-A4) : le begin differe doit poser la
#     mise en page AVANT que le painter soit actif (pas de warning Qt). ---
EOUT="$ROOT/_etat_out.pdf"
rm -f "$EOUT"
trap 'rm -f "$OUT" "$POUT" "$EOUT"' EXIT
EOUT_WIN="$(cygpath -m "$EOUT" 2>/dev/null || echo "$EOUT")"

etat_log="$(OWL_PRINTER_OUTPUT_FILE="$EOUT_WIN" "$OWL" "$ROOT/etat/print_etat.owl" 2>&1)"
erc=$?
if [ "$erc" -ne 0 ]; then
    echo "FAIL: owl.exe a retourne $erc sur l'impression d'un Etat"
    echo "$etat_log"
    exit 1
fi
if printf '%s' "$etat_log" | grep -qi "Cannot be changed while printer is active"; then
    echo "FAIL: warning Qt de mise en page sur printer actif (begin differe casse)"
    echo "$etat_log"
    exit 1
fi
if [ ! -s "$EOUT" ] || [ "$(head -c 4 "$EOUT")" != "%PDF" ]; then
    echo "FAIL: l'impression de l'Etat n'a pas produit de flux PDF"
    exit 1
fi
echo "PASS: impression d'un Etat (Format A3) sans warning de mise en page"

# --- Conformité  : iImprimeEtat autonome (ni iDestination ni
#     iFinImprime) produit une impression complète. ---
AOUT="$ROOT/_auto_out.pdf"
rm -f "$AOUT"
trap 'rm -f "$OUT" "$POUT" "$EOUT" "$AOUT"' EXIT
AOUT_WIN="$(cygpath -m "$AOUT" 2>/dev/null || echo "$AOUT")"

OWL_PRINTER_OUTPUT_FILE="$AOUT_WIN" "$OWL" "$ROOT/etat_autonome/auto.owl"
arc=$?
if [ "$arc" -ne 0 ]; then
    echo "FAIL: owl.exe a retourne $arc sur iImprimeEtat autonome"
    exit 1
fi
if [ ! -s "$AOUT" ] || [ "$(head -c 4 "$AOUT")" != "%PDF" ]; then
    echo "FAIL: iImprimeEtat seul (sans iDestination ni iFinImprime) n'a rien produit"
    exit 1
fi
echo "PASS: iImprimeEtat autonome (sans iDestination ni iFinImprime) produit un PDF"

# ---  : enchaînement de 2 états → UN document de 2 pages ---
NOUT="$ROOT/_ench_out.pdf"
rm -f "$NOUT"
trap 'rm -f "$OUT" "$POUT" "$EOUT" "$AOUT" "$NOUT"' EXIT
NOUT_WIN="$(cygpath -m "$NOUT" 2>/dev/null || echo "$NOUT")"

OWL_PRINTER_OUTPUT_FILE="$NOUT_WIN" "$OWL" "$ROOT/enchainement/ench.owl"
nrc=$?
if [ "$nrc" -ne 0 ]; then
    echo "FAIL: owl.exe a retourne $nrc sur l'enchainement"
    exit 1
fi
if [ ! -s "$NOUT" ] || [ "$(head -c 4 "$NOUT")" != "%PDF" ]; then
    echo "FAIL: l'enchainement n'a pas produit de PDF"
    exit 1
fi
ncount="$(grep -aoE '/Count[[:space:]]+[0-9]+' "$NOUT" | grep -oE '[0-9]+' | head -1)"
if [ "$ncount" != "2" ]; then
    echo "FAIL: enchainement de 2 etats attendu = 2 pages, obtenu /Count=$ncount"
    exit 1
fi
echo "PASS: enchainement de 2 etats -> un PDF de 2 pages (/Count 2)"

# ---  : un changement de config physique en cours de chaine coupe la
#     sequence en jobs de spool distincts (Qt fige duplex/couleur/bac au niveau
#     job). Un verrou par reglage job-level d'OWL (les 3 que iParametre expose).
#     En mode test, chaque job ecrit un fichier distinct (2e job suffixe _1) ;
#     sans changement un seul fichier serait produit (cf. check /Count 2 au-dessus). ---
cut_files=""
for sub in enchainement_coupure enchainement_coupure_duplex enchainement_coupure_bac; do
    cut_files="$cut_files $ROOT/$sub/_cut_out.pdf $ROOT/$sub/_cut_out_1.pdf"
done
rm -f $cut_files
cleanup() { rm -f "$OUT" "$POUT" "$EOUT" "$AOUT" "$NOUT" $cut_files; }
trap cleanup EXIT

check_cut() {
    local sub="$1" label="$2"
    local f="$ROOT/$sub/_cut_out.pdf" f1="$ROOT/$sub/_cut_out_1.pdf"
    rm -f "$f" "$f1"
    local fw; fw="$(cygpath -m "$f" 2>/dev/null || echo "$f")"
    OWL_PRINTER_OUTPUT_FILE="$fw" "$OWL" "$ROOT/$sub/ench_cut.owl"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: owl.exe a retourne $rc sur la coupure ($label)"; exit 1
    fi
    if [ ! -s "$f" ] || [ "$(head -c 4 "$f")" != "%PDF" ]; then
        echo "FAIL: coupure $label — 1er job absent ou non-PDF"; exit 1
    fi
    if [ ! -s "$f1" ] || [ "$(head -c 4 "$f1")" != "%PDF" ]; then
        echo "FAIL: coupure $label — 2e job attendu (config changee -> 2 jobs)"; exit 1
    fi
    echo "PASS: coupure sur $label -> 2 jobs de spool distincts"
}

check_cut enchainement_coupure        "couleur"
check_cut enchainement_coupure_duplex "recto-verso"
check_cut enchainement_coupure_bac    "bac papier"
exit 0
