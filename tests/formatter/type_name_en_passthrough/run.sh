#!/usr/bin/env bash
# Harnais boite noire : `owl fmt` ne doit jamais re-ecrire le NOM d'un type
# compose EN vers sa forme FR (ou inversement).
#
# Contexte : la resolution de type ne reconnaissait pas encore la
# forme EN enregistree (owl_registry_type_find ne matchait que canonical /
# canonicalAscii / aliases, jamais nameEN). Le chemin type du formatter
# (formatter.c) n'a de son cote AUCUN chemin de preservation EN pour les
# types purs : un token de type non resolu cote formatter est laisse
# verbatim (passthrough). Ce harnais verrouille ce passthrough :
#   - une forme EN non resolue par le compilateur reste EN apres fmt
#     (pas de FR-ification silencieuse une fois la resolution corrigee)
#   - une forme EN deja resolue (cas de recouvrement natif/type,
#     invariant 5) reste EN apres fmt elle aussi : le comportement ne
#     doit pas changer suivant que la resolution de type reussit ou non
#   - la forme FR canon reste inchangee (non-regression)
#   - idempotence : deux passes de fmt donnent le meme octet
#
# Un rouge ici pointe une re-ecriture FR-ifiante introduite dans le chemin
# type du formatter, pas le compilateur.
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

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t owl_fmt_type_en)"
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
    printf '%s\n' "$2" > "$dir/a.owl"
    "$OWL" fmt "$dir/a.owl" >/dev/null 2>&1
    line "$dir/a.owl" "$3"
}

echo "== A. forme EN non resolue par le compilateur : reste EN apres fmt"
got="$(fmt_line A_unresolved 'v est un emailSMTPSession' 1)"
check "A/emailSMTPSession inchange" 'v est un emailSMTPSession' "$got"

echo "== B. forme EN deja resolue (recouvrement natif/type) : reste EN apres fmt"
got="$(fmt_line B_resolved 'v est un httpRequest' 1)"
check "B/httpRequest inchange" 'v est un httpRequest' "$got"

echo "== C. forme FR canon : inchangee (non-regression)"
got="$(fmt_line C_fr 'v est un emailSessionSMTP' 1)"
check "C/emailSessionSMTP inchange" 'v est un emailSessionSMTP' "$got"

echo "== D. idempotence : deux passes de fmt sur la forme EN donnent le meme octet"
mkdir -p "$WORK/D_idem"
printf '%s\n' 'v est un natsConnection' > "$WORK/D_idem/a.owl"
"$OWL" fmt "$WORK/D_idem/a.owl" >/dev/null 2>&1
cp "$WORK/D_idem/a.owl" "$WORK/D_idem/pass1.owl"
"$OWL" fmt "$WORK/D_idem/a.owl" >/dev/null 2>&1
cmp -s "$WORK/D_idem/pass1.owl" "$WORK/D_idem/a.owl" \
    && ok "D/natsConnection idempotent (2 passes)" \
    || ko "D/natsConnection idempotent (2 passes) — la 2e passe diverge"

echo
echo "formatter_type_name_en_passthrough: $PASS ok, $FAIL FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
