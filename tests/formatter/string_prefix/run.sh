#!/usr/bin/env bash
# Harnais boite noire : round-trip du prefixe de chaine interpolee %"..." par
# `owl fmt`.
#
# Le prefixe `%` colle au guillemet ouvre une chaine interpolee (cf. scanner
# scan_string_interp : au `%`, si le caractere suivant est `"`, c'est une
# chaine, pas l'operateur modulo). Le formatter inserait un espace parasite
# (`%"..."` -> `% "..."`), ce qui transforme `%` en operateur modulo et
# corrompt du code valide au save. Le formatter est gatekeeper canonique
# (invariant 4) : il DOIT round-tripper byte-identique.
#
# Contrat verrouille ici :
#   - `%"a""b"` round-trippe byte-identique (bug principal)
#   - `%"..."` avec interpolation `[%var%]` round-trippe byte-identique
#   - l'operateur modulo `a % b` GARDE ses espaces (non-regression : ne pas
#     confondre le `%` prefixe colle au `"` avec le `%` operateur infixe)
#   - les 3 autres formes de chaine ("...", `...`, [ ... ]) round-trippent
#   - idempotence : 2 passes de fmt sur %"..." donnent le meme octet
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

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t owl_fmt_strpfx)"
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

# roundtrip <id_dossier> <contenu> -> rc 0 si fmt laisse le fichier byte-identique
roundtrip() {
    local dir="$WORK/$1"
    mkdir -p "$dir"
    printf '%b' "$2" > "$dir/a.owl"
    cp "$dir/a.owl" "$dir/orig.owl"
    "$OWL" fmt "$dir/a.owl" >/dev/null 2>&1
    cmp -s "$dir/orig.owl" "$dir/a.owl"
}

echo "== A. Bug principal : %\"a\"\"b\" round-trip byte-identique"
roundtrip A_pfx 'x est une cha\xc3\xaene = %"a""b"\n' \
    && ok "A/%\"a\"\"b\" byte-identique" \
    || ko "A/%\"a\"\"b\" byte-identique — le fmt a injecte un espace parasite"

echo "== B. Interpolation %\"a[%v%]b\" round-trip byte-identique"
roundtrip B_interp 'v est une cha\xc3\xaene = "x"\ny est une cha\xc3\xaene = %"a[%v%]b"\nTrace(y)\n' \
    && ok "B/%\"...[%v%]...\" byte-identique" \
    || ko "B/%\"...[%v%]...\" byte-identique — le fmt a modifie la ligne interpolee"

echo "== C. Non-regression : l'operateur modulo a % b garde ses espaces"
got="$(fmt_line C_mod 'a est un entier = 7\nb est un entier = 3\nc est un entier = a%b\nTrace(c)\n' 3)"
check "C/modulo espace" 'c est un entier = a % b' "$got"
# La forme deja espacee est un point fixe.
roundtrip C_mod_fix 'a est un entier = 7\nb est un entier = 3\nc est un entier = a % b\nTrace(c)\n' \
    && ok "C/modulo espace byte-identique" \
    || ko "C/modulo espace byte-identique — le fmt a touche l'operateur modulo"

echo "== D. Non-regression : les 3 autres formes de chaine round-trippent"
roundtrip D_guill 's est une cha\xc3\xaene = "abc"\nTrace(s)\n' \
    && ok "D/\"...\" byte-identique" || ko "D/\"...\" byte-identique"
roundtrip D_backtick 's est une cha\xc3\xaene = `abc`\nTrace(s)\n' \
    && ok "D/backtick byte-identique" || ko "D/backtick byte-identique"
roundtrip D_crochet 's est une cha\xc3\xaene = [\nabc\n]\nTrace(s)\n' \
    && ok "D/[ ... ] byte-identique" || ko "D/[ ... ] byte-identique"

echo "== E. Idempotence : 2 passes de fmt sur %\"...\" donnent le meme octet"
mkdir -p "$WORK/E_idem"
printf '%b' 'x est une cha\xc3\xaene = %"a""b"\n' > "$WORK/E_idem/a.owl"
"$OWL" fmt "$WORK/E_idem/a.owl" >/dev/null 2>&1
cp "$WORK/E_idem/a.owl" "$WORK/E_idem/pass1.owl"
"$OWL" fmt "$WORK/E_idem/a.owl" >/dev/null 2>&1
cmp -s "$WORK/E_idem/pass1.owl" "$WORK/E_idem/a.owl" \
    && ok "E/idempotent (2 passes)" \
    || ko "E/idempotent (2 passes) — la 2e passe diverge"

echo
echo "formatter_string_prefix: $PASS ok, $FAIL FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
