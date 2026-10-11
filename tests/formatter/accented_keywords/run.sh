#!/usr/bin/env bash
# Harnais boite noire sur les mots-cles accentues de la table du formatter
#. Quatre formes n'avaient AUCUNE entree dans fmt_keywords[] et
# survivaient par absence de traitement : À, _À_, ÉLÉMENT, SÉPARÉE. Plus un
# defaut lie : PAR en tete d'iteration fuyait en minuscule.
#
# Contrat verrouille ici :
#   - `owl fmt` restaure l'accent + la casse canon de SÉPARÉE et le PAR
#     d'iteration passe en MAJUSCULE (symetrique du DE, )
#   - le `par` de dimensions/tri (hors iteration) reste MINUSCULE (non-regression)
#   - la borne de plage `a`/`_a_` passe en À/_À_ accentue majuscule
#   - `owl translate --to en` rend les canons EN : ÉLÉMENT->ELEMENT,
#     SÉPARÉE->SEPARATED, À->TO, PAR d'iteration->BY
#   - point fixe atteint sur l'aller-retour EN->FR->EN de la ligne SÉPARÉE
#   - les quatre formes restent ACCEPTEES en entree (--check rc 0)
#
# Chaque fixture vit dans SON PROPRE dossier (contexte script d'OWL).
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

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t owl_fmt_acc)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
ko()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
check() {  # <id> <attendu> <obtenu>
    if [ "$2" = "$3" ]; then ok "$1"
    else ko "$1 — attendu [$2] obtenu [$3]"; fi
}

line() { sed -n "${2}p" "$1" | tr -d '\r'; }  # <fichier> <n>

# fmt_line <id_dossier> <contenu> <ligne_a_verifier> -> imprime la ligne formatee
fmt_line() {
    local dir="$WORK/$1"
    mkdir -p "$dir"
    printf '%b' "$2" > "$dir/a.owl"
    "$OWL" fmt "$dir/a.owl" >/dev/null 2>&1
    line "$dir/a.owl" "$3"
}

# translate_line <id_dossier> <lang> <contenu> <ligne> -> ligne traduite
translate_line() {
    local dir="$WORK/$1" out="$WORK/$1/out"
    mkdir -p "$dir" "$out"
    printf '%b' "$3" > "$dir/a.owl"
    "$OWL" translate --to "$2" "$dir/a.owl" > "$out/a.owl" 2>/dev/null
    line "$out/a.owl" "$4"
}

compiles() {  # <id_dossier> <contenu>
    local dir="$WORK/$1"
    mkdir -p "$dir"
    printf '%b' "$2" > "$dir/a.owl"
    "$OWL" --check "$dir/a.owl" >/dev/null 2>&1
}

SEP_FR='POUR TOUTE cha\xc3\xaene x DE "a,b" S\xc3\x89PAR\xc3\x89E PAR ","\n\tTrace(x)\nFIN\n'
SEP_EN='FOR EACH string x OF "a,b" SEPARATED BY ","'

echo "== A. owl fmt : SÉPARÉE + PAR d'iteration restaures (accent + MAJUSCULE)"
got="$(fmt_line A_sep 'pour toute cha\xc3\xaene x de "a,b" separee par ","\n\tTrace(x)\nfin\n' 1)"
check "A/SEPAREE PAR canon" 'POUR TOUTE chaîne x DE "a,b" SÉPARÉE PAR ","' "$got"

echo "== B. owl fmt : le par de dimensions/tri reste minuscule (non-regression)"
got="$(fmt_line B_dims 't est un tableau de 3 par 4 entiers\n' 1)"
check "B/par dims minuscule" 't est un tableau de 3 par 4 entiers' "$got"

echo "== C. owl fmt : borne de plage a/_a_ -> À/_À_"
got="$(fmt_line C_a 'POUR i = 1 a 5\n\tTrace(i)\nFIN\n' 1)"
check "C/a -> À" 'POUR i = 1 À 5' "$got"
got="$(fmt_line C_asc 'POUR i = 1 _a_ 5\n\tTrace(i)\nFIN\n' 1)"
check "C/_a_ -> _À_" 'POUR i = 1 _À_ 5' "$got"

echo "== D. owl translate --to en : ÉLÉMENT -> ELEMENT"
got="$(translate_line D_elem en 'arr est un tableau de entiers = [1, 2, 3]\nPOUR TOUT \xc3\x89L\xc3\x89MENT x DE arr\n\tTrace(x)\nFIN\n' 2)"
check "D/ELEMENT" 'FOR EACH ELEMENT x OF arr' "$got"

echo "== E. owl translate --to en : SÉPARÉE -> SEPARATED, PAR -> BY"
got="$(translate_line E_sep en "$SEP_FR" 1)"
check "E/SEPARATED BY" "$SEP_EN" "$got"

echo "== F. owl translate --to en : borne À -> TO"
got="$(translate_line F_a en 'POUR i = 1 \xc3\x80 5\n\tTrace(i)\nFIN\n' 1)"
check "F/À -> TO" 'FOR i = 1 TO 5' "$got"

echo "== G. Aller-retour EN->FR->EN : point fixe sur la ligne SÉPARÉE"
mkdir -p "$WORK/G_rt"
printf '%s\n\tTrace(x)\nFIN\n' "$SEP_EN" > "$WORK/G_rt/en0.owl"
"$OWL" translate --to fr "$WORK/G_rt/en0.owl" > "$WORK/G_rt/fr1.owl" 2>/dev/null
"$OWL" translate --to en "$WORK/G_rt/fr1.owl" > "$WORK/G_rt/en2.owl" 2>/dev/null
check "G/point fixe EN" "$SEP_EN" "$(line "$WORK/G_rt/en2.owl" 1)"

echo "== H. Acceptation preservee en entree (aucun mot retire)"
compiles H_elem 'arr est un tableau de entiers = [1, 2, 3]\nPOUR TOUT \xc3\x89L\xc3\x89MENT x DE arr\n\tTrace(x)\nFIN\n' \
    && ok "H/ÉLÉMENT compile" || ko "H/ÉLÉMENT compile — rc != 0"
compiles H_sep "$SEP_FR" && ok "H/SÉPARÉE compile" || ko "H/SÉPARÉE compile — rc != 0"
compiles H_a 'POUR i = 1 \xc3\x80 5\n\tTrace(i)\nFIN\n' && ok "H/À compile" || ko "H/À compile — rc != 0"
compiles H_asc 'POUR i = 1 _\xc3\x80_ 5\n\tTrace(i)\nFIN\n' && ok "H/_À_ compile" || ko "H/_À_ compile — rc != 0"

echo "== I. owl fmt : separateur de borne dans un slice s[[i À j]]"
# Le slice partage le token de borne TOKEN_A (compiler.c string_slice) mais
# HORS contexte POUR/CAS : la borne y est le SEUL a/to possible (a/to sont
# reserves, ne nomment pas d'index). Canon identique a la borne d'iteration.
got="$(fmt_line I_a 's est une chaîne = "abcdef"\nr est une chaîne = s[[1 a 4]]\nTrace(r)\n' 2)"
check "I/slice a -> À" 'r est une chaîne = s[[1 À 4]]' "$got"
got="$(fmt_line I_to 's est une chaîne = "abcdef"\nr est une chaîne = s[[1 to 4]]\nTrace(r)\n' 2)"
check "I/slice to -> TO" 'r est une chaîne = s[[1 TO 4]]' "$got"
# Forme deja accentuee : point fixe.
got="$(fmt_line I_acc 's est une chaîne = "abcdef"\nr est une chaîne = s[[1 \xc3\x80 4]]\nTrace(r)\n' 2)"
check "I/slice À reste À" 'r est une chaîne = s[[1 À 4]]' "$got"
# Non-regression : les index VARIABLES du slice (b/d) restent intacts, seul le
# separateur bascule. (`a` ne peut pas etre un index : mot reserve.)
got="$(fmt_line I_var 's est une chaîne = "abcdef"\nb est un entier = 2\nd est un entier = 5\nr est une chaîne = s[[b À d]]\nTrace(r)\n' 4)"
check "I/slice index var intacts" 'r est une chaîne = s[[b À d]]' "$got"
# Traduction : le separateur de slice suit la langue cible comme la borne POUR.
# --to en traduit aussi la declaration (chaîne -> string) : seul le slice
# nous interesse ici, mais la comparaison porte sur la ligne entiere traduite.
got="$(translate_line I_tr en 's est une chaîne = "abcdef"\nr est une chaîne = s[[1 a 4]]\nTrace(r)\n' 2)"
check "I/slice a --to en -> TO" 'r is a string = s[[1 TO 4]]' "$got"

echo "== J. owl fmt : contenu d'un slice s[[...]] canonise (natives), hors separateur"
# , follow-up 1637 : le separateur de borne est deja canonise, mais le
# RESTE du contenu du [[ ]] etait recopie verbatim -> une native nue dans un
# index (taille(ch)) restait en minuscule. On applique le meme canon bare-word
# (find_native_norm) que partout ailleurs.
got="$(fmt_line J_nat 'ch est une chaîne = "abcdef"\nr est une chaîne = ch[[taille(ch) À 4]]\nTrace(r)\n' 2)"
check "J/slice native canonisee" 'r est une chaîne = ch[[Taille(ch) À 4]]' "$got"
# Non-regression matrice : litteral de nombres, aucun mot n'est une native ->
# byte-identique apres fmt (preuve par cmp, pas juste egalite de ligne).
mkdir -p "$WORK/J_mat"
printf '%b' 'm est un tableau de réels = [[1.0, 2.0], [3.0, 4.0]]\nTrace(m)\n' > "$WORK/J_mat/a.owl"
cp "$WORK/J_mat/a.owl" "$WORK/J_mat/orig.owl"
"$OWL" fmt "$WORK/J_mat/a.owl" >/dev/null 2>&1
cmp -s "$WORK/J_mat/orig.owl" "$WORK/J_mat/a.owl" \
    && ok "J/matrice byte-identique (cmp)" \
    || ko "J/matrice byte-identique (cmp) — le fmt a modifie le litteral matrice"
# Non-regression : un index VARIABLE (n) n'est pas une native -> inchange.
got="$(fmt_line J_var 'ch est une chaîne = "abcdef"\nn est un entier = 2\nr est une chaîne = ch[[n À 4]]\nTrace(r)\n' 3)"
check "J/slice index variable intact" 'r est une chaîne = ch[[n À 4]]' "$got"
# Idempotence : deux passes de fmt sur le slice canonise donnent le meme octet.
mkdir -p "$WORK/J_idem"
printf '%b' 'ch est une chaîne = "abcdef"\nr est une chaîne = ch[[taille(ch) À 4]]\nTrace(r)\n' > "$WORK/J_idem/a.owl"
"$OWL" fmt "$WORK/J_idem/a.owl" >/dev/null 2>&1
cp "$WORK/J_idem/a.owl" "$WORK/J_idem/pass1.owl"
"$OWL" fmt "$WORK/J_idem/a.owl" >/dev/null 2>&1
cmp -s "$WORK/J_idem/pass1.owl" "$WORK/J_idem/a.owl" \
    && ok "J/slice idempotent (2 passes)" \
    || ko "J/slice idempotent (2 passes) — la 2e passe diverge"

echo
echo "formatter_accented_keywords: $PASS ok, $FAIL FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
