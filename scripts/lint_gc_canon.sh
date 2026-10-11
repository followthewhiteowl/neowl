#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Lint statique GC : interdit `table_set(&inst->fields, ...)` et
# `table_set(&vm.globals, ...)` hors des fichiers du canon /0058.
#
# Verrou STRUCTUREL : tout code C qui pose un champ dans une ObjInstance
# ou dans vm.globals DOIT passer par les helpers canon (object.h) :
#
#   inst_field_set_str(inst, "name", const char* val)
#   inst_field_set_int(inst, "name", int64_t val)
#   inst_field_set_bool(inst, "name", bool val)
#   inst_field_set_value(inst, "name", Value val)
#   obj_globals_set("name", Value val)
#
# Le canon centralise la discipline push/pop vm.stack autour du table_set
# qui peut grow et declencher un GC.
#
# Lecon historique ( + ) : les sentinels stress-GC sont
# une couverture d'EXECUTION (attrapent ce qu'ils touchent). Sans ce lint
# STATIQUE, n'importe quel module peut introduire son propre helper
# bugue (cf. inst_set_string HTTP non couvert par les sentinels jusqu'a
# decouverte expert externe). Le lint ferme la classe par construction.
#
# Whitelist (fichiers autorises a faire table_set direct) :
#
#   GROUPE A — Canon (table_set direct legitime, c'est leur boulot) :
#     - src/object.c       : canon helpers inst_field_set_*, obj_globals_set,
#                            obj_inst_set_kv, obj_globals_set_kv
#     - src/owl_registry.c : helpers globals canon (owl_register_global_string)
#     - src/vm.c           : opcodes runtime safe par construction (values
#                            deja sur vm.stack via vm_pop/vm_peek/AS_STRING)
#
#   GROUPE B LEGACY : ELIMINE. Tous les sites legacy migres
#   vers canon. Whitelist reduite au Groupe A pur. Si un nouveau cas
#   legitime emerge, ajout requiert justification et update .
#
# Echappatoire : `OWL_SKIP_LINT_GC=1 mingw32-make all` (responsabilite dev).
#
# Usage : bash scripts/lint_gc_canon.sh
# Exit code : 0 = OK, 1 = violation detectee.

set -u

if [ "${OWL_SKIP_LINT_GC:-0}" = "1" ]; then
  echo "[lint-gc] SKIPPED (OWL_SKIP_LINT_GC=1)"
  exit 0
fi

# Resolution racine repo (script peut etre invoque depuis n'importe ou)
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo ".")
cd "$REPO_ROOT" || exit 1

# Pattern 1 : table_set(&XXX->fields, ...) ou table_set(&vm.globals, ...)
# Etendu  : aussi vm.types et les index secondaires canon
# (vm.globals_canon_idx, vm.types_canon_idx). Ces 3 nouvelles tables
# stockent ObjString* fresh et doivent passer par les helpers canon
# de object.c (globals_set_bilateral + vm_type_register) qui font le
# rooting GC correct (modele owl_define_native).
# Accepte espaces variables autour de la parenthese et de l'ampersand.
PATTERN='table_set[[:space:]]*\([[:space:]]*&[[:space:]]*([a-zA-Z_][a-zA-Z0-9_]*->fields|vm\.globals|vm\.types|vm\.globals_canon_idx|vm\.types_canon_idx)'

# Pattern 2 : value_array_write(&XXX->elements, OBJ_VAL(...))
# Le pattern OBJ_VAL en arg = risque UAF si val non-rootee + array grow.
# Forcer obj_array_push (canon object.c) qui push arr + val avant grow.
# Note : value_array_write avec primitives (INTEGER_VAL, BOOL_VAL, etc.)
# reste autorise — c'est le primitif. Seul OBJ_VAL est dangereux.
PATTERN2='value_array_write[[:space:]]*\([[:space:]]*&[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*->elements[[:space:]]*,[[:space:]]*OBJ_VAL'

# Liste des fichiers a checker : src/*.c sauf whitelist Groupe A pur.
WHITELIST_BASENAMES="object.c owl_registry.c vm.c"

# Trouver tous les .c dans src/ (pas recursif - pas de sous-dossiers actuels)
FILES_TO_CHECK=""
for f in src/*.c; do
  # Glob non expanse (dossier vide/absent) : `src/*.c` arrive litteralement.
  # Sans ce test, le lint compterait 1 "fichier" inexistant et se croirait
  # non-vide.
  [ -f "$f" ] || continue
  basename=$(basename "$f")
  skip=0
  for w in $WHITELIST_BASENAMES; do
    if [ "$basename" = "$w" ]; then
      skip=1
      break
    fi
  done
  if [ "$skip" = "0" ]; then
    FILES_TO_CHECK="$FILES_TO_CHECK $f"
  fi
done

# Whitelist Pattern 2 : object.c (canon obj_array_push, value.c (def primitif))
# Note : `object.c` est aussi dans la whitelist Pattern 1 (table_set canon).
# `value.c` definit value_array_write primitif, doit pouvoir l'appeler.
WHITELIST_BASENAMES_P2="object.c value.c"

FILES_TO_CHECK_P2=""
for f in src/*.c; do
  [ -f "$f" ] || continue
  basename=$(basename "$f")
  skip=0
  for w in $WHITELIST_BASENAMES_P2; do
    if [ "$basename" = "$w" ]; then
      skip=1
      break
    fi
  done
  if [ "$skip" = "0" ]; then
    FILES_TO_CHECK_P2="$FILES_TO_CHECK_P2 $f"
  fi
done

# Garde de non-vacuite. Ce lint conclut "OK" en ne trouvant aucune
# violation — verdict indistinguable de "je n'ai scanne aucun fichier". Si le
# motif `src/*.c` ne correspond plus a rien (deplacement des sources, cwd
# faux, glob non expanse), grep ne matche rien et le lint rend vert en
# n'ayant rien lu. On compte, on affiche, et on echoue sur zero.
NB_FILES=$(printf '%s\n' $FILES_TO_CHECK | grep -c .)
NB_FILES_P2=$(printf '%s\n' $FILES_TO_CHECK_P2 | grep -c .)
if [ "$NB_FILES" -eq 0 ] || [ "$NB_FILES_P2" -eq 0 ]; then
  cat <<EOF >&2
================================================================
[OWL lint-gc] CORPUS VIDE : 0 fichier scanne (pattern 1: $NB_FILES,
pattern 2: $NB_FILES_P2). Le motif \`src/*.c\` ne correspond a rien
depuis $REPO_ROOT.
Un lint qui n'examine rien ne prouve rien — echec, pas un vert.
================================================================
EOF
  exit 1
fi

# Recherche pattern 1 (table_set hors canon)
VIOLATIONS=$(grep -nE "$PATTERN" $FILES_TO_CHECK 2>/dev/null || true)
# Recherche pattern 2 (value_array_write OBJ_VAL hors canon)
VIOLATIONS_P2=$(grep -nE "$PATTERN2" $FILES_TO_CHECK_P2 2>/dev/null || true)

if [ -n "$VIOLATIONS" ]; then
  cat <<EOF >&2
================================================================
[OWL lint-gc] BLOCKED: \`table_set\` direct sur ->fields ou vm.globals
hors des fichiers canon. Discipline ADR_0057 / ADR_0058 violee.
================================================================

Sites detectes :
$VIOLATIONS

Reco : remplacer par les helpers canon de \`object.h\` :
  - inst_field_set_str  (inst, "name", "value")
  - inst_field_set_int  (inst, "name", 42)
  - inst_field_set_bool (inst, "name", true)
  - inst_field_set_value(inst, "name", Value)
  - obj_globals_set     ("name", Value)

Whitelist : $(printf '%s ' $WHITELIST_BASENAMES)

Skip d'urgence : OWL_SKIP_LINT_GC=1 mingw32-make all
================================================================
EOF
  exit 1
fi

if [ -n "$VIOLATIONS_P2" ]; then
  cat <<EOF >&2
================================================================
[OWL lint-gc] BLOCKED: \`value_array_write\` avec OBJ_VAL direct hors
canon. Risque UAF (val Obj fresh non-rootee + array grow → GC).
================================================================

Sites detectes :
$VIOLATIONS_P2

Reco : remplacer par le canon \`object.h\` :
  - obj_array_push(ObjArray* arr, Value val)

Le canon push arr + val (si Obj) avant value_array_write qui peut grow.
Pour primitives (INTEGER_VAL/BOOL_VAL/REAL_VAL), value_array_write
direct reste autorise (pas concerne par UAF).

Whitelist Pattern 2 (fichiers autorises) : $(printf '%s ' $WHITELIST_BASENAMES_P2)

Skip d'urgence : OWL_SKIP_LINT_GC=1 mingw32-make all
================================================================
EOF
  exit 1
fi

# Pas de violation. Le compte est affiche : un lint doit dire ce qu'il a lu,
# pas seulement ce qu'il n'a pas trouve.
echo "[lint-gc] OK ($NB_FILES fichiers scannes pattern 1, $NB_FILES_P2 pattern 2 —" \
     "table_set + value_array_write OBJ_VAL uniquement dans canon)"
exit 0
