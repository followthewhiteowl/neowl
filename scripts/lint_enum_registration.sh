#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Lint statique enums : tout `OwlEnum E_xxx = { ... }` defini dans un
# `src/stdlib_*.c` DOIT etre reference dans un tableau de registration
# `const OwlEnum* ... [] = { &E_xxx, ... }` presente a
# owl_registry_register_enums().
#
# Pourquoi ce lint EXISTE alors qu'on a deja `--self-test-registry` :
#
# Le self-test itere LE REGISTRE, c'est-a-dire l'etat d'ARRIVEE. Un enum
# oublie du tableau de registration n'est jamais presente au registre,
# donc le self-test ne peut pas le manquer : `violation_count` reste a 0
# et `enums_checked` ne bouge pas. La classe de defaut vit EN AMONT, dans
# l'ecart entre ce qui est declare dans le source et ce qui est presente
# au registre. Un audit du registre ne peut pas la voir par construction.
# Seule une lecture du SOURCE le peut.
#
# Symptomes d'un enum oublie (aucun bug runtime, d'ou l'invisibilite) :
#   owl explain <enum>       -> symbole inconnu
#   owl examples <enum>      -> aucun scenario
#   owl --vscode-data        -> 0 occurrence (pas d'autocomplete LSP)
# Les constantes membres, elles, resolvent parfaitement et la contrainte
# compile-time fonctionne (reference C directe par pointeur). Cas reel :
# E_base64Format, absent de STRING_ENUMS[] pendant tout  ;
# detecte par revue humaine uniquement.
#
# Detection des tableaux : par TYPE (`OwlEnum*` + `[] =`), pas par nom.
# La convention `*_ENUMS[]` n'est pas universelle (cf. `ENUMS_I18N[]` dans
# stdlib_i18n.c, `ENUMS[]` dans ui_catalog.c) — matcher le nom laisserait
# des trous.
#
# Portee : definitions dans `src/stdlib_*.c` (perimetre du ticket).
# Les tableaux de registration sont collectes sur TOUT `src/*.c` : un enum
# non-static peut legitimement etre enregistre depuis un fichier voisin.
#
# Echappatoire : `OWL_SKIP_LINT_ENUM=1 mingw32-make all` (responsabilite dev).
#
# Usage : bash scripts/lint_enum_registration.sh
# Exit code : 0 = OK, 1 = enum non enregistre detecte.

set -u

if [ "${OWL_SKIP_LINT_ENUM:-0}" = "1" ]; then
  echo "[lint-enum] SKIPPED (OWL_SKIP_LINT_ENUM=1)"
  exit 0
fi

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo ".")
cd "$REPO_ROOT" || exit 1

# Passe 1 : ensemble des enums references dans un tableau de registration,
# sur l'ensemble de src/*.c (registration cross-fichier legitime).
REGISTERED=$(awk '
  # Entree dans un tableau de pointeurs OwlEnum : "OwlEnum* ... [] = {"
  /OwlEnum[ \t]*\*/ && /\[[ \t]*\][ \t]*=/ { inblock = 1 }
  inblock {
    line = $0
    while (match(line, /&[ \t]*E_[A-Za-z0-9_]+/)) {
      tok = substr(line, RSTART, RLENGTH)
      sub(/&[ \t]*/, "", tok)
      print tok
      line = substr(line, RSTART + RLENGTH)
    }
    if (index($0, "};") > 0) { inblock = 0 }
  }
' src/*.c | sort -u)

# Passe 2 : definitions d'enums dans src/stdlib_*.c ("OwlEnum E_xxx =").
# `extern const OwlEnum E_xxx;` est une DECLARATION (pas de "="), exclue.
DEFINED=$(awk '
  /^[ \t]*(static[ \t]+)?(const[ \t]+)?OwlEnum[ \t]+E_[A-Za-z0-9_]+[ \t]*=/ {
    match($0, /E_[A-Za-z0-9_]+/)
    print FILENAME ":" FNR ":" substr($0, RSTART, RLENGTH)
  }
' src/stdlib_*.c)

VIOLATIONS=""
CHECKED=0
while IFS= read -r entry; do
  [ -z "$entry" ] && continue
  CHECKED=$((CHECKED + 1))
  name=${entry##*:}
  if ! printf '%s\n' "$REGISTERED" | grep -qx "$name"; then
    VIOLATIONS="$VIOLATIONS$entry"$'\n'
  fi
done <<EOF
$DEFINED
EOF

# Garde de non-vacuite : le compte etait affiche mais jamais
# verifie. `CHECKED=0` (motif `src/stdlib_*.c` qui ne correspond plus a rien,
# awk qui ne matche plus la forme de definition) rendait ce lint vert en
# n'ayant examine aucun enum.
if [ "$CHECKED" -eq 0 ]; then
  cat <<EOF >&2
================================================================
[OWL lint-enum] CORPUS VIDE : 0 enum examine.
Aucune definition \`OwlEnum E_xxx =\` trouvee dans src/stdlib_*.c
depuis $REPO_ROOT. Soit le motif de fichiers ne correspond plus a
rien, soit la forme de definition a change et l'extraction est
aveugle. Dans les deux cas le lint ne prouve rien : echec.
================================================================
EOF
  exit 1
fi

if [ -n "$VIOLATIONS" ]; then
  cat <<EOF >&2
================================================================
[OWL lint-enum] BLOCKED: enum defini mais JAMAIS enregistre.
================================================================

Enums concernes (fichier:ligne:symbole) :
$VIOLATIONS
Consequence : l'enum n'existe pas pour l'introspection.
  owl explain <nom>   -> symbole inconnu
  owl examples <nom>  -> aucun scenario
  owl --vscode-data   -> pas d'autocomplete LSP
Aucun bug runtime ne le signalera : les constantes membres resolvent
et la contrainte compile-time fonctionne (pointeur C direct). C'est
exactement pour ca que ce lint existe.

Reco : ajouter \`&<symbole>\` au tableau de registration du fichier
(ex. \`STRING_ENUMS[]\`), celui passe a owl_registry_register_enums().

Skip d'urgence : OWL_SKIP_LINT_ENUM=1 mingw32-make all
================================================================
EOF
  exit 1
fi

echo "[lint-enum] OK ($CHECKED enums definis dans src/stdlib_*.c, tous enregistres)"
exit 0
