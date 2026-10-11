#!/usr/bin/env bash
# Harnais boite noire sur le mot d'iteration de `owl translate`.
#
# Contrat verrouille ici :
#   - FR -> EN : TOUT / TOUTE / TOUS / CHAQUE rendent tous EACH
#   - EN -> FR : EACH / EVERY / ALL          rendent tous TOUT
#   - aucune des sept formes ne survit dans la langue opposee (sur la SORTIE)
#   - les sept formes restent ACCEPTEES en entree (--check rc 0) : on ne retire
#     aucun mot du langage, seule la sortie de translate change
#   - la sortie traduite compile dans les deux sens
#   - aller-retour : point fixe atteint en un tour (le canon est stable, la
#     variante normalise au premier tour puis ne bouge plus)
#   - le collapse ne se declenche QU'AU CHANGEMENT de langue : une cible egale
#     a la langue du fichier laisse la forme ecrite telle quelle (l'accord
#     grammatical FR ne doit pas etre casse par un simple passage a --to fr)
#   - `owl list keywords --lang en` ne rend plus de mot d'iteration FR
#
# Chaque fixture vit dans SON PROPRE dossier : le contexte script d'OWL, ce
# sont les voisins du meme repertoire, et deux .owl cote a cote se compilent
# ensemble (faux masquages sans rapport avec le sujet).
#
# Usage:
#   ./run.sh                  # utilise ./build/owl.exe (racine repo)
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si toutes les assertions passent, 1 sinon.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"
OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"

if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t owl_tr_iter)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
ko()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }

# check <id> <attendu> <obtenu>
check() {
    if [ "$2" = "$3" ]; then ok "$1"
    else ko "$1 — attendu [$2] obtenu [$3]"; fi
}

FR_FORMS="TOUT TOUTE TOUS CHAQUE"
EN_FORMS="EACH EVERY ALL"

# fixture <dossier> <fr|en> <mot> — un fichier, un dossier, pas de voisin.
fixture() {
    local dir="$WORK/$1" lang="$2" word="$3"
    mkdir -p "$dir"
    if [ "$lang" = "fr" ]; then
        printf 'arr est un tableau de entiers = [1, 2, 3]\nPOUR %s e DE arr\n\tTrace(e)\nFIN\n' \
               "$word" > "$dir/a.owl"
    else
        printf 'arr is an array of integer = [1, 2, 3]\nFOR %s e OF arr\n\tTrace(e)\nEND\n' \
               "$word" > "$dir/a.owl"
    fi
    printf '%s' "$dir/a.owl"
}

# iterline <fichier> — la ligne d'iteration (ligne 2 de toutes les fixtures).
iterline() { sed -n '2p' "$1" | tr -d '\r'; }

# translate_to <lang> <src> <dossier_sortie> — sortie isolee dans son dossier.
translate_to() {
    local lang="$1" src="$2" out="$WORK/$3"
    mkdir -p "$out"
    "$OWL" translate --to "$lang" "$src" > "$out/a.owl" 2>"$out/err.txt"
    printf '%s' "$out/a.owl"
}

# compiles <fichier> — vrai si --check rend rc 0.
compiles() { "$OWL" --check "$1" >/dev/null 2>&1; }

echo "== A. Les quatre formes FR rendent EACH (--to en)"
for w in $FR_FORMS; do
    src="$(fixture "src_fr_$w" fr "$w")"
    out="$(translate_to en "$src" "out_en_$w")"
    check "A/$w -> EACH" "FOR EACH e OF arr" "$(iterline "$out")"
done

echo "== B. Les trois formes EN rendent TOUT (--to fr)"
for w in $EN_FORMS; do
    src="$(fixture "src_en_$w" en "$w")"
    out="$(translate_to fr "$src" "out_fr_$w")"
    check "B/$w -> TOUT" "POUR TOUT e DE arr" "$(iterline "$out")"
done

echo "== C. Aucune forme ne survit dans la langue opposee (critere central)"
for w in $FR_FORMS; do
    survivant="$(grep -Eiow '(TOUT|TOUTE|TOUS|CHAQUE)' "$WORK/out_en_$w/a.owl" 2>/dev/null | tr '\n' ' ')"
    check "C/$w pas de FR dans la sortie EN" "" "$survivant"
done
for w in $EN_FORMS; do
    survivant="$(grep -Eiow '(EACH|EVERY|ALL)' "$WORK/out_fr_$w/a.owl" 2>/dev/null | tr '\n' ' ')"
    check "C/$w pas d'EN dans la sortie FR" "" "$survivant"
done

echo "== D. Acceptation preservee en entree (non-regression, aucun mot retire)"
for w in $FR_FORMS; do
    if compiles "$WORK/src_fr_$w/a.owl"; then ok "D/POUR $w compile"
    else ko "D/POUR $w compile — --check rc != 0"; fi
done
for w in $EN_FORMS; do
    if compiles "$WORK/src_en_$w/a.owl"; then ok "D/FOR $w compile"
    else ko "D/FOR $w compile — --check rc != 0"; fi
done

echo "== E. La sortie traduite compile (une traduction non compilable est pire)"
for w in $FR_FORMS; do
    if compiles "$WORK/out_en_$w/a.owl"; then ok "E/$w sortie EN compile"
    else ko "E/$w sortie EN compile — --check rc != 0"; fi
done
for w in $EN_FORMS; do
    if compiles "$WORK/out_fr_$w/a.owl"; then ok "E/$w sortie FR compile"
    else ko "E/$w sortie FR compile — --check rc != 0"; fi
done

echo "== F. Aller-retour : point fixe atteint en un tour"
# Source FR : t1 = FR->EN, t2 = t1->FR, t3 = t2->EN.
# t2 doit porter le canon FR (TOUT) et t3 doit etre byte-identique a t1.
for w in $FR_FORMS; do
    t2="$(translate_to fr "$WORK/out_en_$w/a.owl" "rt_fr_$w")"
    check "F/$w retour FR canon" "POUR TOUT e DE arr" "$(iterline "$t2")"
    t3="$(translate_to en "$t2" "rt_en_$w")"
    if cmp -s "$t3" "$WORK/out_en_$w/a.owl"; then ok "F/$w point fixe en un tour"
    else ko "F/$w point fixe en un tour — t3 != t1 ($(iterline "$t3"))"; fi
done
# Source EN : symetrique.
for w in $EN_FORMS; do
    t2="$(translate_to en "$WORK/out_fr_$w/a.owl" "rt_en2_$w")"
    check "F/$w retour EN canon" "FOR EACH e OF arr" "$(iterline "$t2")"
    t3="$(translate_to fr "$t2" "rt_fr2_$w")"
    if cmp -s "$t3" "$WORK/out_fr_$w/a.owl"; then ok "F/$w point fixe en un tour"
    else ko "F/$w point fixe en un tour — t3 != t1 ($(iterline "$t3"))"; fi
done

echo "== G. Casse d'entree toleree : la sortie reste canonique"
mkdir -p "$WORK/src_low"
printf 'arr est un tableau de entiers = [1, 2, 3]\npour chaque e de arr\n\tTrace(e)\nfin\n' \
    > "$WORK/src_low/a.owl"
out="$(translate_to en "$WORK/src_low/a.owl" "out_low")"
check "G/pour chaque minuscule -> FOR EACH" "FOR EACH e OF arr" "$(iterline "$out")"

echo "== H. Commentaires et litteraux chaine intacts (garde anti-remplacement brut)"
mkdir -p "$WORK/src_txt"
printf 'msg est une chaine = "POUR CHAQUE ligne"\n// note : POUR CHAQUE reste du texte\nTrace(msg)\n' \
    > "$WORK/src_txt/a.owl"
out="$(translate_to en "$WORK/src_txt/a.owl" "out_txt")"
check "H/litteral intact" 'msg is a string = "POUR CHAQUE ligne"' "$(sed -n '1p' "$out" | tr -d '\r')"
check "H/commentaire intact" '// note : POUR CHAQUE reste du texte' "$(sed -n '2p' "$out" | tr -d '\r')"

echo "== I. Listing des mots-cles : pas de mot d'iteration de l'autre langue"
fuite_en="$("$OWL" list keywords --lang en 2>&1 | grep -Eiw '(TOUT|TOUTE|TOUS|CHAQUE)' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | tr '\n' '|')"
check "I/list keywords --lang en sans FR" "" "$fuite_en"
fuite_fr="$("$OWL" list keywords --lang fr 2>&1 | grep -Eiw '(EACH|EVERY|ALL)' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | tr '\n' '|')"
check "I/list keywords --lang fr sans EN" "" "$fuite_fr"

echo "== J. Cible = langue du fichier : la forme ecrite survit (pas de collapse intra-langue)"
# Comparateur : byte-identite avec l'ENTREE PASSEE AU FORMATEUR, pas avec
# l'entree brute — translate met aussi en forme, et comparer au brut ferait
# rougir sur une indentation sans rapport avec le mot d'iteration. Comparer la
# seule presence du mot serait au contraire trop laxiste (une sortie contenant
# TOUTE ailleurs passerait). L'egalite byte a byte tranche les deux.
same_lang_keeps() {   # <fr|en> <mot> <libelle>
    local lang="$1" word="$2" id="$3" src ref out
    src="$(fixture "self_src_${lang}_$word" "$lang" "$word")"
    mkdir -p "$WORK/self_ref_${lang}_$word"
    ref="$WORK/self_ref_${lang}_$word/a.owl"
    cp "$src" "$ref"
    "$OWL" fmt "$ref" >/dev/null 2>&1
    out="$(translate_to "$lang" "$src" "self_out_${lang}_$word")"
    if cmp -s "$out" "$ref"; then ok "$id"
    else ko "$id — sortie != entree formatee (attendu [$(iterline "$ref")] obtenu [$(iterline "$out")])"; fi
}
same_lang_keeps fr TOUTE  "J/--to fr sur FR laisse TOUTE (accord preserve)"
same_lang_keeps fr CHAQUE "J/--to fr sur FR laisse CHAQUE"
same_lang_keeps fr TOUS   "J/--to fr sur FR laisse TOUS"
same_lang_keeps en EVERY  "J/--to en sur EN laisse EVERY"
same_lang_keeps en ALL    "J/--to en sur EN laisse ALL"

echo
echo "translate_iteration: $PASS ok, $FAIL FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
