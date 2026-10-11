#!/usr/bin/env bash
# Sondes d'atteignabilite des limites publiees par owl_limits.def.
#
# Une limite annoncee doit etre le VRAI mur, et le message recu doit nommer
# CETTE limite. Deux cas par limite sondee :
#
#   N-1  doit compiler sans erreur      -> revele les murs empruntes (une autre
#                                          limite, ou une table dimensionnee sur
#                                          le nom d'une autre, frappe avant)
#   N+1  doit echouer en nommant N      -> revele les saturations muettes et les
#                                          messages qui accusent autre chose
#
# Le cas N-1 est le seul qui revele un mur emprunte : le controle statique de
# `scripts/check_capacity_saturation.sh` voit les chemins d'erreur, pas la
# table reellement dimensionnee derriere le nom.
#
# Les valeurs ne sont PAS ecrites ici : elles sont lues sur le binaire
# (`owl list limits --json`). Changer une valeur dans owl_limits.def deplace
# donc les deux sondes avec elle.
#
# Usage :
#   bash tests/limits/run.sh            # sous-ensemble par defaut (~1 min)
#   bash tests/limits/run.sh --all      # + les sondes longues
#   bash tests/limits/run.sh MAX_X ...  # limites nommees
#   OWL_BIN=./build/owl.exe bash tests/limits/run.sh

set -u

OWL="${OWL_BIN:-./build/owl.exe}"
#  : base de travail DANS build/ et pas /tmp (= AppData/Local/Temp sous
# MSYS2, scanne real-time par l'antivirus, ~20x sur les I/O). Suffixe de
# processus : deux sondes simultanees ne se marchent plus dessus. Cleanup
# garanti par trap EXIT, y compris sur erreur ou Ctrl-C.
#
#  : nom commencant par un point. Les walkers du depot sautent les
# entrees pointees, donc les sondes generees ici restent invisibles pour une
# campagne `owl test` concurrente, qui sinon les charge comme compagnons et
# echoue quand la sonde suivante les remplace. La convention s'ajoute au
# nettoyage, elle ne le remplace pas.
WORK="${OWL_LIMITS_WORK:-build/.limits_probe_$$}"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Sondees par defaut. Budget mesure : ~30 s sur un poste de dev, aucune sonde
# au-dela de 10 s a elle seule.
# ---------------------------------------------------------------------------
DEFAULT_SET="
MAX_LOOP_DEPTH
MAX_BREAKS
MAX_AVEC_DEPTH
MAX_USER_PROC_PARAMS
MAX_METHOD_PARAMS
MAX_LOCALS_PER_FUNCTION
MAX_CLASS_SUPER
MAX_ABSTRACT_CLASSES
MAX_INTERFACE_METHODS
MAX_CLASS_INTERFACES
MAX_USER_TYPES
MAX_CONST_GLOBALS
MAX_DSL_PROC_ALIASES
MAX_USER_PROCS
MAX_PENDING_TYPE_REFS
MAX_DECLARED_GLOBALS
MAX_CLASS_CONSTANTS
MAX_CLASS_FIELD_VIS
MAX_DECLARED_METHODS
MAX_TOP_LEVEL_RANGES
MAX_USER_TYPE_FIELDS_TOTAL
"

# Sonde longue : 47 s a elle seule (deux fichiers de ~65 000 lignes), pour un
# mur structurel du bytecode qui ne bouge pas. Jouee par --all, pas dans la
# batterie : un controle qu'on finit par desactiver parce qu'il est lent ne
# protege personne.
LONG_SET="
MAX_CHUNK_CONSTANTS
"

# NON SONDEES, et pourquoi (mettre a jour en meme temps que le defaut) :
#
#   (aucune, hors LONG_SET)

# ---------------------------------------------------------------------------
# Generateurs : gen_<LIMIT> <N> <dir>. Chaque cas vit dans son propre dossier
# (les .owl voisins d'un meme dossier sont compiles ensemble).
# ---------------------------------------------------------------------------
seqn() { local n=$1 i=1; while [ "$i" -le "$n" ]; do echo "$i"; i=$((i + 1)); done; }

gen_MAX_LOOP_DEPTH() { local N=$1 d=$2 k
  { echo "PROCEDURE P()"; echo "	i est un entier"
    for k in $(seqn "$N"); do echo "	POUR i = 1 A 2"; done
    echo "	Trace(i)"
    for k in $(seqn "$N"); do echo "	FIN"; done
    echo "FIN"; echo "P()"; } > "$d/a.owl"; }

gen_MAX_BREAKS() { local N=$1 d=$2 k
  { echo "PROCEDURE P()"; echo "	i est un entier"; echo "	POUR i = 1 A 2"
    for k in $(seqn "$N"); do echo "		SI i = $k ALORS SORTIR"; done
    echo "	FIN"; echo "FIN"; echo "P()"; } > "$d/a.owl"; }

gen_MAX_AVEC_DEPTH() { local N=$1 d=$2 k
  { echo "STA est une Structure"; echo "	nA est un entier"; echo "FIN"
    echo "PROCEDURE P()"; echo "	o est une STA"
    for k in $(seqn "$N"); do echo "	AVEC o"; done
    echo "	Trace(o.nA)"
    for k in $(seqn "$N"); do echo "	FIN"; done
    echo "FIN"; echo "P()"; } > "$d/a.owl"; }

gen_MAX_USER_PROC_PARAMS() { local N=$1 d=$2 k ps=""
  for k in $(seqn "$N"); do
    if [ -z "$ps" ]; then ps="p$k est un entier"; else ps="$ps, p$k est un entier"; fi; done
  { echo "PROCEDURE P($ps)"; echo "	Trace(p1)"; echo "FIN"; echo "P(1)"; } > "$d/a.owl"; }

# Interface ET classe portent la meme signature : le cas N-1 verifie qu'une
# signature conforme n'est jamais refusee au motif de la table interne, et le
# cas N+1 que la limite est nommee au lieu d'etre ecretee.
gen_MAX_METHOD_PARAMS() { local N=$1 d=$2 k ps=""
  for k in $(seqn "$N"); do
    if [ -z "$ps" ]; then ps="p$k est un entier"; else ps="$ps, p$k est un entier"; fi; done
  printf 'iM est une Interface\n\tPROCEDURE MF(%s) : entier\nFIN\n' "$ps" > "$d/iM.owl"
  { echo "cM est une Classe"; echo "	implémente iM"; echo "PUBLIC"
    echo "	nA est un entier"
    echo "	PROCEDURE MF($ps) : entier"; echo "		RENVOYER p1"; echo "	FIN"
    echo "FIN"; } > "$d/cM.owl"
  printf 'o est un cM\nTrace(o.nA)\n' > "$d/main.owl"; }

gen_MAX_LOCALS_PER_FUNCTION() { local N=$1 d=$2 k
  { echo "PROCEDURE P()"
    for k in $(seqn "$N"); do echo "	nL$k est un entier = 1"; done
    echo "	Trace(nL1)"; echo "FIN"; echo "P()"; } > "$d/a.owl"; }

gen_MAX_CLASS_SUPER() { local N=$1 d=$2 k
  printf 'cBase est une Classe\nPUBLIC\n\tnA est un entier\nFIN\n' > "$d/cBase.owl"
  for k in $(seqn "$N"); do
    { echo "cS$k est une Classe"; echo "	hérite de cBase"; echo "PUBLIC"
      echo "	nB$k est un entier"; echo "FIN"; } > "$d/cS$k.owl"; done
  printf 'o est un cS1\nTrace(o.nB1)\n' > "$d/main.owl"; }

# Aucun `hérite de` : c'est le point du cas. La table des classes abstraites a
# longtemps ete dimensionnee sur la limite d'heritage, et saturait en accusant
# une relation d'heritage que le code ne contenait pas.
gen_MAX_ABSTRACT_CLASSES() { local N=$1 d=$2 k
  for k in $(seqn "$N"); do
    { echo "cAbs$k est une Classe, abstraite"; echo "PUBLIC"
      echo "	nA$k est un entier"; echo "FIN"; } > "$d/cAbs$k.owl"; done
  printf 'Trace("ok")\n' > "$d/main.owl"; }

gen_MAX_INTERFACE_METHODS() { local N=$1 d=$2 k
  { echo "iX est une Interface"
    for k in $(seqn "$N"); do echo "	PROCEDURE F$k()"; done
    echo "FIN"; } > "$d/iX.owl"
  { echo "cX est une Classe"; echo "	implémente iX"; echo "PUBLIC"
    echo "	nA est un entier"
    for k in $(seqn "$N"); do echo "	PROCEDURE F$k()"; echo "		:nA = $k"; echo "	FIN"; done
    echo "FIN"; } > "$d/cX.owl"
  printf 'o est un cX\nTrace(o.nA)\n' > "$d/main.owl"; }

# N relations classe->interface, avec 64 interfaces reutilisees : classes et
# interfaces partagent le budget des types composes, une interface par relation
# saturerait ce budget-la avant celui vise.
gen_MAX_CLASS_INTERFACES() { local N=$1 d=$2 j c rest take
  local I=64
  for j in $(seqn "$I"); do
    printf 'iY%s est une Interface\n\tPROCEDURE FY%s()\nFIN\n' "$j" "$j" > "$d/iY$j.owl"; done
  c=1; rest=$N
  while [ "$rest" -gt 0 ]; do
    take=$I; [ "$rest" -lt "$I" ] && take=$rest
    { echo "cY$c est une Classe"
      for j in $(seqn "$take"); do echo "	implémente iY$j"; done
      echo "PUBLIC"; echo "	nA est un entier"
      for j in $(seqn "$take"); do echo "	PROCEDURE FY$j()"; echo "		:nA = $j"; echo "	FIN"; done
      echo "FIN"; } > "$d/cY$c.owl"
    rest=$((rest - take)); c=$((c + 1)); done
  printf 'o est un cY1\nTrace(o.nA)\n' > "$d/main.owl"; }

gen_MAX_USER_TYPES() { local N=$1 d=$2 k
  { for k in $(seqn "$N"); do
      echo "STU$k est une Structure"; echo "	nA est un entier"; echo "FIN"; done
    echo "gU est une STU1"; echo "Trace(gU.nA)"; } > "$d/a.owl"; }

gen_MAX_CONST_GLOBALS() { local N=$1 d=$2 k
  { echo "CONSTANT"
    for k in $(seqn "$N"); do echo "	KC$k = $k"; done
    echo "FIN"; echo "Trace(KC1)"; } > "$d/a.owl"; }

gen_MAX_DSL_PROC_ALIASES() { local N=$1 d=$2 k
  { echo "FEN_Probe est une Fenêtre"; echo "	PROPRIETES"
    echo "		Titre       = \"Probe\""; echo "		Disposition = dispVerticale"; echo "	FIN"
    for k in $(seqn "$N"); do echo "	PROCEDURE PA$k()"; echo "		Trace($k)"; echo "	FIN"; done
    echo "FIN"; } > "$d/FEN_Probe.owl"
  printf 'Trace("ok")\n' > "$d/main.owl"; }

gen_MAX_USER_PROCS() { local N=$1 d=$2 k
  { for k in $(seqn "$N"); do echo "PROCEDURE P$k()"; echo "	Trace($k)"; echo "FIN"; done
    echo "P1()"; } > "$d/a.owl"; }

gen_MAX_PENDING_TYPE_REFS() { local N=$1 d=$2 k
  { for k in $(seqn "$N"); do echo "gP$k est une STZ$k"; done
    for k in $(seqn "$N"); do
      echo "STZ$k est une Structure"; echo "	nA est un entier"; echo "FIN"; done
    echo "Trace(gP1.nA)"; } > "$d/a.owl"; }

# Des globales entierement ordinaires : un type explicite, aucune optionnelle.
# C'est le point du cas. Le registre de metadonnees de type, dimensionne pour
# les seules optionnelles, frappait a 1024 en accusant une notion que ce source
# n'emploie nulle part.
gen_MAX_DECLARED_GLOBALS() { local N=$1 d=$2 k
  { for k in $(seqn "$N"); do echo "gD$k est un entier = 1"; done
    echo "Trace(gD1)"; } > "$d/a.owl"; }

gen_MAX_CLASS_CONSTANTS() { local N=$1 d=$2 k
  { echo "cK est une Classe"; echo "CONSTANT"
    for k in $(seqn "$N"); do echo "	KK$k = $k"; done
    echo "FIN"; echo "PUBLIC"; echo "	nA est un entier"; echo "FIN"; } > "$d/cK.owl"
  printf 'o est un cK\nTrace(o.nA)\n' > "$d/main.owl"; }

gen_MAX_CLASS_FIELD_VIS() { local N=$1 d=$2 k
  { echo "cV est une Classe"; echo "PUBLIC"
    for k in $(seqn "$N"); do echo "	nV$k est un entier"; done
    echo "FIN"; } > "$d/cV.owl"
  printf 'o est un cV\nTrace(o.nV1)\n' > "$d/main.owl"; }

gen_MAX_DECLARED_METHODS() { local N=$1 d=$2 k
  { echo "cM est une Classe"; echo "PUBLIC"; echo "	nA est un entier"
    for k in $(seqn "$N"); do echo "	PROCEDURE M$k()"; echo "		:nA = $k"; echo "	FIN"; done
    echo "FIN"; } > "$d/cM.owl"
  printf 'o est un cM\nTrace(o.nA)\n' > "$d/main.owl"; }

gen_MAX_TOP_LEVEL_RANGES() { local N=$1 d=$2 k
  { echo "PROCEDURE PR()"; echo "	Trace(0)"; echo "FIN"
    for k in $(seqn "$N"); do echo "Trace($k)"; done; } > "$d/a.owl"; }

gen_MAX_USER_TYPE_FIELDS_TOTAL() { local N=$1 d=$2 k
  { echo "STF est une Structure"
    for k in $(seqn "$N"); do echo "	nF$k est un entier"; done
    echo "FIN"; echo "gF est une STF"; echo "Trace(gF.nF1)"; } > "$d/a.owl"; }

# Le squelette de la sonde consomme lui-meme quelques constantes : on vise
# N-6 / N+1 plutot que N-1 / N+1 (le mur exact est mesure a 65530 litteraux).
gen_MAX_CHUNK_CONSTANTS() { local N=$1 d=$2 k
  { echo "PROCEDURE P()"; echo "	n est un entier"
    k=1; while [ "$k" -le "$N" ]; do echo "	n = $((1000000 + k))"; k=$((k + 1)); done
    echo "	Trace(n)"; echo "FIN"; echo "P()"; } > "$d/a.owl"; }
CHUNK_CONSTANTS_SLACK=6

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
limit_value() { # <name> -> valeur annoncee par le binaire
  "$OWL" list limits --json 2>/dev/null \
    | tr -d ' ' | grep -A1 "\"name\":\"$1\"" | grep '"value"' \
    | sed 's/.*"value"://;s/,.*//'; }

# Le dossier de travail ne porte PAS le nom de la limite : les diagnostics
# citent des chemins de fichiers, et un dossier nomme d'apres la limite suffit
# a faire croire au controle que le message la nomme.
n_case=0
run_case() { # <limit> <N> <tag> -> sortie de --check
  local L=$1 N=$2 tag=$3 d entry
  n_case=$((n_case + 1)); d="$WORK/case_${n_case}_$tag"
  rm -rf "$d"; mkdir -p "$d"
  "gen_$L" "$N" "$d"
  entry="$d/main.owl"; [ -f "$entry" ] || entry=$(ls "$d"/*.owl | head -1)
  "$OWL" --check "$entry" 2>&1; }

# Premier message d'ERREUR du JSON. Le rapport de --check est precede des
# diagnostics texte sur stderr : un `sed` non ancre laisse ces lignes intactes
# et fait passer un avertissement pour la cause du refus.
first_error() { tr '{' '
' | grep -m1 '"message":"' | sed 's/.*"message":"//;s/".*//'; }

n_fail=0; n_ok=0
probe() {
  local L=$1 V lo hi slack=0 t0=$SECONDS status="OK" detail=""
  V=$(limit_value "$L")
  if [ -z "$V" ]; then
    printf '  %-28s %s\n' "$L" "ECHEC: limite absente de 'owl list limits'"
    n_fail=$((n_fail + 1)); return; fi
  [ "$L" = "MAX_CHUNK_CONSTANTS" ] && slack=$CHUNK_CONSTANTS_SLACK

  lo=$(run_case "$L" $((V - 1 - slack)) minus)
  if ! echo "$lo" | grep -q '"errors":\[\]'; then
    status="ECHEC"
    detail="N-1 refuse : $(echo "$lo" | first_error)"
  fi

  hi=$(run_case "$L" $((V + 1)) plus)
  if echo "$hi" | grep -q '"errors":\[\]'; then
    status="ECHEC"; detail="$detail${detail:+ | }N+1 accepte : saturation muette"
  elif ! echo "$hi" | grep -q "$L"; then
    status="ECHEC"
    detail="$detail${detail:+ | }N+1 refuse pour un autre motif : $(echo "$hi" | first_error)"
  fi

  printf '  %-28s %-6s %-6s %3ds  %s\n' "$L" "$V" "$status" "$((SECONDS - t0))" "$detail"
  if [ "$status" = "OK" ]; then n_ok=$((n_ok + 1)); else n_fail=$((n_fail + 1)); fi; }

SET="$DEFAULT_SET"
case "${1:-}" in
  --all)  SET="$DEFAULT_SET $LONG_SET" ;;
  --list) echo "$DEFAULT_SET $LONG_SET"; exit 0 ;;
  "")     ;;
  *)      SET="$*" ;;
esac

if [ ! -x "$OWL" ]; then echo "owl introuvable : $OWL" >&2; exit 2; fi

echo "Sondes d'atteignabilite des limites (N-1 doit passer, N+1 doit nommer la limite)"
for L in $SET; do probe "$L"; done

echo "  ---"
printf '  %d saines / %d defectueuses\n' "$n_ok" "$n_fail"
[ "$n_fail" -eq 0 ]
