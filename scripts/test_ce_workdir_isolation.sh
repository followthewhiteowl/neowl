#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Scenarios de validation des workdirs du self-test compile-error.
#
#   A. INVISIBILITE : le workdir du harnais n'est plus decouvert par le walk
#      d'`owl test`. C'etait la cause racine des aborts transitoires : les
#      fixtures copiees dans build/_ce_selftest_*/ etaient chargees comme
#      compagnons d'une campagne, et un self-test concurrent les supprimait
#      sous les pieds du runner.
#   A2. MEME CONVENTION POUR LES AUTRES HARNAIS : tout harnais qui pose des
#      `.owl` sous build/ pendant qu'il tourne ouvre la meme fenetre de course.
#      La convention est unique (nom commencant par un point) ; ce scenario la
#      verifie sur chaque producteur, en LISANT le nom dans sa source.
#   A3. MEME CONVENTION POUR LES HARNAIS C : plusieurs self-tests deposent des
#      `.owl` dans le repertoire courant (la racine du depot sous `make`),
#      dossier de travail ou fichier nu. Meme fenetre de course, meme regle ;
#      le balayage recense les producteurs plutot que de les lister a la main.
#   B. BORNAGE : les workdirs `_KEPT` (conserves volontairement sur echec pour
#      debug post-mortem) sont limites aux N plus recents ; les workdirs sans
#      `_KEPT` laisses par un run tue en vol sont ramasses au-dela d'1h.
#   C. CONCURRENCE : deux self-tests + un `owl test` simultanes ne se
#      detruisent pas mutuellement.
#
# Usage : bash scripts/test_ce_workdir_isolation.sh   (depuis la racine repo)

cd "$(dirname "$0")/.." || exit 1
OWL=./build/owl.exe
[ -x "$OWL" ] || { echo "owl.exe absent : lance mingw32-make all d'abord"; exit 1; }

pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ok    $1"; }
ko()   { fail=$((fail+1)); echo "  FAIL  $1"; }
NOW=$(date +%s)

echo "=== A. le workdir du harnais est invisible pour le walk owl test ==="
mkdir -p "build/.ce_selftest_90001_${NOW}"
printf 'chaine non terminee = "\n' > "build/.ce_selftest_90001_${NOW}/fixture_cassee.owl"
# Controle : meme contenu SANS le point initial -> doit etre vu (c'etait le trou).
mkdir -p "build/_ce_ctrl_90002_${NOW}"
printf 'chaine non terminee = "\n' > "build/_ce_ctrl_90002_${NOW}/fixture_cassee.owl"

listing=$($OWL test build --list-files 2>&1)
if printf '%s' "$listing" | grep -q "ce_selftest_90001"; then
  ko "workdir .ce_selftest_* decouvert par owl test (isolation cassee)"
else
  ok "workdir .ce_selftest_* invisible pour owl test"
fi
if printf '%s' "$listing" | grep -q "_ce_ctrl_90002"; then
  ok "controle : un dossier sans point initial EST decouvert (le point fait le job)"
else
  ko "controle inoperant : le walk ne descend pas dans build/, revoir le scenario"
fi
rm -rf "build/.ce_selftest_90001_${NOW}" "build/_ce_ctrl_90002_${NOW}"

echo "=== A2. les autres harnais nomment leur base de travail de la meme facon ==="
# Le nom attendu n'est PAS recopie ici : il est LU dans la source du
# producteur. Renommer le dossier deplace la sonde avec lui ; renommer la
# variable qui le porte casse l'extraction, et l'echec le dit au lieu de
# passer au vert sur un ensemble vide.
n_prod=0
producteur() { # $1 label  $2 nom de base lu dans la source du producteur
  local label=$1 raw=$2 leaf ctrl listing
  n_prod=$((n_prod + 1))
  if [ -z "$raw" ]; then
    ko "$label : base de travail introuvable dans la source (extraction a revoir)"
    return
  fi
  # `$$` est le PID du producteur a l'execution : ici un faux PID suffit, seule
  # la forme du nom nous interesse.
  leaf="${raw//\$\$/9400$n_prod}"
  # Temoin : meme cas de figure, nom garanti sans point initial. Sans lui, un
  # walk qui ne descendrait plus dans build/ rendrait la sonde muette au vert.
  ctrl="ce_ctrl_prod${n_prod}_${leaf#.}"
  mkdir -p "build/$leaf" "build/$ctrl"
  printf 'chaine non terminee = "\n' > "build/$leaf/fixture_cassee.owl"
  printf 'chaine non terminee = "\n' > "build/$ctrl/fixture_cassee.owl"
  listing=$($OWL test build --list-files 2>&1)
  if printf '%s' "$listing" | grep -qF "build/$leaf/"; then
    ko "$label : base de travail '$leaf' VISIBLE pour le walk owl test"
  else
    ok "$label : base de travail '$leaf' invisible pour le walk owl test"
  fi
  if printf '%s' "$listing" | grep -qF "build/$ctrl/"; then
    ok "$label : controle (meme dossier sans point initial) bien decouvert"
  else
    ko "$label : controle inoperant, le walk ne descend pas dans build/"
  fi
  rm -rf "build/$leaf" "build/$ctrl"
}

producteur "run_compiler_tests.sh" \
  "$(sed -n 's|^OWL_TMP_BASE="\$DIR/build/\(.*\)"$|\1|p' scripts/run_compiler_tests.sh)"
producteur "tests/limits/run.sh" \
  "$(sed -n 's|^WORK="${OWL_LIMITS_WORK:-build/\(.*\)}"$|\1|p' tests/limits/run.sh)"

echo "=== A3. les harnais C nomment leurs temporaires .owl de la meme facon ==="
# Meme convention, autres producteurs : des self-tests C deposent des `.owl`
# dans le repertoire courant (la racine du depot quand `make` les lance), sous
# forme de dossier de travail ou de fichier nu. Un `owl test` concurrent les
# enumere comme compagnons et abandonne quand le self-test les remplace.
#
# La regle eprouvee ici est celle du WALK (« une entree dont le nom commence
# par un point est sautee »), pas celle d'un emplacement : la sonde materialise
# donc les noms sous build/, ou le walk descend (prouve par le scenario A) et
# ou la mesure coute une seconde. Un probe pose a la racine imposerait une
# campagne complete a chaque assertion.
#
# Les noms ne sont PAS recopies : ils sont LUS dans la source du producteur.
n_prodc=0
producteur_c() { # $1 label  $2 nom de la feuille lu dans la source  $3 dir|fichier
  local label=$1 leaf=$2 kind=$3 ctrl listing
  n_prodc=$((n_prodc + 1))
  if [ -z "$leaf" ]; then
    ko "$label : nom introuvable dans la source (extraction a revoir)"
    return
  fi
  # Temoin : meme cas de figure, nom garanti sans point initial. Sans lui, un
  # walk qui cesserait de descendre ici rendrait la sonde muette au vert.
  ctrl="ce_ctrl_prodc${n_prodc}_${leaf#.}"
  if [ "$kind" = dir ]; then
    mkdir -p "build/$leaf" "build/$ctrl"
    printf 'chaine non terminee = "\n' > "build/$leaf/fixture_cassee.owl"
    printf 'chaine non terminee = "\n' > "build/$ctrl/fixture_cassee.owl"
  else
    printf 'chaine non terminee = "\n' > "build/$leaf"
    printf 'chaine non terminee = "\n' > "build/$ctrl"
  fi
  listing=$($OWL test build --list-files 2>&1)
  if printf '%s' "$listing" | grep -qF "build/$leaf"; then
    ko "$label : temporaire '$leaf' VISIBLE pour le walk owl test"
  else
    ok "$label : temporaire '$leaf' invisible pour le walk owl test"
  fi
  if printf '%s' "$listing" | grep -qF "build/$ctrl"; then
    ok "$label : controle (meme nom sans point initial) bien decouvert"
  else
    ko "$label : controle inoperant, le walk ne descend pas dans build/"
  fi
  rm -rf "build/$leaf" "build/$ctrl"
}

# fixture_unique(dst, taille, PREFIXE, SUFFIXE) accole le PID entre les deux :
# un faux PID suffit, seule la forme du nom nous interesse.
FAKEPID=95001

# 1. cli_ops_test.c : racine de bac a sable, contient projet.owl / theme.owl.
producteur_c "cli_ops_test.c (bac a sable)" \
  "$(sed -n 's|^#define SANDBOX_PREFIXE "\(.*\)"$|\1|p' \
       src/internal_tests/cli_ops_test.c)${FAKEPID}" dir

# 2. translate_test.c : dossier de travail, contient t.owl / g.owl / captures.
producteur_c "translate_test.c (dossier de travail)" \
  "$(sed -n 's|.*fixture_unique(dir, sizeof dir, "\([^"]*\)", "");.*|\1|p' \
       src/internal_tests/translate_test.c)${FAKEPID}" dir

# 3. fmt_stability_test.c : fichiers nus deposes dans le repertoire courant.
producteur_c "fmt_stability_test.c (accord --check)" \
  "$(sed -n 's|.*snprintf(path, sizeof path, "\([^"]*\)", caseName);.*|\1|p' \
       src/internal_tests/fmt_stability_test.c | sed "s|%s|sonde|")" fichier

# 4. Balayage : TOUT temporaire `.owl` cree par un self-test C via
#    fixture_unique. Enumerer plutot que lister a la main, pour qu'un
#    producteur ajoute demain soit couvert sans toucher a cette sonde.
n_owl_fixtures=0
while IFS='@' read -r pref suff; do
  [ -n "$pref" ] || continue
  n_owl_fixtures=$((n_owl_fixtures + 1))
  producteur_c "fixture_unique .owl : ${pref}*${suff}" \
    "${pref}${FAKEPID}${suff}" fichier
done <<EOF
$(sed -n 's|.*fixture_unique([A-Za-z_]*, sizeof[ (]*[A-Za-z_]*)*, "\([^"]*\)", "\([^"]*\.owl\)").*|\1@\2|p' \
     src/internal_tests/*.c)
EOF
if [ "$n_owl_fixtures" -ge 1 ]; then
  ok "balayage fixture_unique : $n_owl_fixtures temporaire(s) .owl recense(s)"
else
  ko "balayage fixture_unique : aucun temporaire .owl trouve, extraction a revoir"
fi

echo "=== B. bornage des _KEPT + ramassage des orphelins ==="
# La retention porte sur la POPULATION ENTIERE des `_KEPT` de build/, pas sur
# les seuls temoins que ce scenario injecte. Un `_KEPT` laisse par un vrai echec
# anterieur est forcement PLUS RECENT que nos temoins (dates dans le passe) :
# il occupe une place de retention, et les six temoins meurent ensemble. Le
# scenario rendait alors « 0 survivants » en accusant le bornage, alors que le
# bornage faisait exactement son travail. On met la population existante de cote
# le temps de la mesure, et on la restaure apres : le scenario devient
# hermetique SANS detruire les traces post-mortem d'un run precedent — qui sont
# precisement ce que ce mecanisme existe pour conserver.
CE_KEPT_GLOB='.ce_selftest_*_KEPT'
kept_stash="build/.ce_keptstash_$$_${NOW}"
mkdir -p "$kept_stash"
n_stashed=0
n_restored=0
# Filet d'interruption, meme patron que run_compiler_tests.sh / tests/limits/run.sh
# a une difference pres : ici le trap RESTAURE, il ne supprime pas. Ce qui est
# mis de cote, ce sont les traces post-mortem d'un echec anterieur ; un `rm -rf`
# de secours serait pire que l'absence de filet. Sans ce trap, une sortie en
# erreur ou un Ctrl-C dans la fenetre stash-avant-restore laissait la population
# reelle orpheline sous un nom pointe, donc invisible a tout ramasseur.
# Idempotent : apres la restauration nominale le stash n'existe plus et l'appel
# depuis le trap ne fait rien.
restore_kept_stash() {
  # Les temoins sont FABRIQUES par ce scenario : on les jette. La population
  # mise de cote est EMPRUNTEE : on la rend. Noms exacts, jamais de glob sur le
  # numero — un vrai workdir peut porter un PID qui ressemble a un temoin.
  for i in 1 2 3 4 5 6; do
    rm -rf "build/.ce_selftest_9100${i}_$((NOW - 100000 + i * 1000))_KEPT"
  done
  rm -rf "build/.ce_selftest_92001_${old}" "build/.ce_selftest_92002_${NOW}"
  [ -d "$kept_stash" ] || return 0
  for d in "$kept_stash"/${CE_KEPT_GLOB}; do
    [ -d "$d" ] || continue
    mv "$d" build/ && n_restored=$((n_restored + 1))
  done
  rmdir "$kept_stash" 2>/dev/null
}
trap restore_kept_stash EXIT
trap 'restore_kept_stash; exit 130' INT
trap 'restore_kept_stash; exit 143' TERM HUP
for d in build/${CE_KEPT_GLOB}; do
  [ -d "$d" ] || continue
  mv "$d" "$kept_stash/" && n_stashed=$((n_stashed + 1))
done

# 6 _KEPT d'ages croissants : seuls les 3 plus recents doivent survivre.
for i in 1 2 3 4 5 6; do
  ts=$((NOW - 100000 + i * 1000))
  mkdir -p "build/.ce_selftest_9100${i}_${ts}_KEPT"
  echo x > "build/.ce_selftest_9100${i}_${ts}_KEPT/_capture.out"
done
# Orphelin (sans _KEPT) vieux de 2h : run tue en vol -> a ramasser.
old=$((NOW - 7200));  mkdir -p "build/.ce_selftest_92001_${old}"
# Orphelin frais : peut appartenir a un run VIVANT -> a ne PAS toucher.
mkdir -p "build/.ce_selftest_92002_${NOW}"

$OWL --self-test-compile-error >/dev/null 2>&1

survivors=$(ls -d build/.ce_selftest_9100*_KEPT 2>/dev/null | wc -l)
[ "$survivors" -eq 3 ] && ok "3 _KEPT survivants sur 6 (diagnostic preserve, accumulation bornee)" \
                       || ko "attendu 3 _KEPT survivants, obtenu $survivors"
if [ -d "build/.ce_selftest_91006_$((NOW - 100000 + 6000))_KEPT" ] \
   && [ -d "build/.ce_selftest_91005_$((NOW - 100000 + 5000))_KEPT" ] \
   && [ -d "build/.ce_selftest_91004_$((NOW - 100000 + 4000))_KEPT" ]; then
  ok "les survivants sont bien les 3 PLUS RECENTS"
else
  ko "les survivants ne sont pas les 3 plus recents"
fi
[ -d "build/.ce_selftest_92001_${old}" ] && ko "orphelin de 2h non ramasse" \
                                        || ok "orphelin de 2h ramasse"
[ -d "build/.ce_selftest_92002_${NOW}" ] && ok "orphelin frais preserve (run voisin peut-etre vivant)" \
                                        || ko "orphelin frais supprime : risque de tuer un run concurrent"

# Retrait des temoins + restauration : ce que le scenario a emprunte lui survit. Un `_KEPT` retenu ici
# serait une trace de diagnostic detruite par le controle cense la proteger.
# Motif explicitement prefixe d'un point : `*` ne matche pas les entrees
# cachees, et toute la convention gardee ici est justement d'en poser.
restore_kept_stash
if [ ! -d "$kept_stash" ] && [ "$n_restored" -eq "$n_stashed" ]; then
  ok "population _KEPT preexistante mise de cote puis restauree ($n_stashed dossier(s)), aucun residu"
else
  ko "mise de cote des _KEPT preexistants : $n_stashed emprunte(s), $n_restored rendu(s), reste $kept_stash"
fi

echo "=== C. deux self-tests + un owl test simultanes ==="
# La course n'a de sens que si le `owl test` concurrent (a) tourne sur de VRAIS
# tests, pour durer assez longtemps et chevaucher les self-tests, et (b) walke
# `build/`, seul endroit ou les workdirs du harnais apparaissent et disparaissent
# en course. D'ou la copie du corpus tests/regression sous build/ : la campagne
# enumere build/ pendant que deux self-tests y creent, remplissent et suppriment
# leurs workdirs. Un `owl test build` sans cette copie ne trouverait aucun test
# (les workdirs du harnais sont invisibles, cf. scenario A) et n'exercerait rien.
race_dir="build/ce_race_$$_${NOW}"
mkdir -p "$race_dir"
cp -r tests/regression/. "$race_dir/"

out1="build/.ce_race_out1_$$"; out2="build/.ce_race_out2_$$"; out3="build/.ce_race_out3_$$"

# Un owl.exe fraichement lie peut encore etre verrouille par le linker ou un
# process VS Code : le lancement rend alors 126/127 sans rien executer. On
# reessaye une fois avant de conclure a un echec de concurrence.
run_resilient() {
  local out=$1; shift
  "$@" >"$out" 2>&1
  local rc=$?
  if [ $rc -eq 126 ] || [ $rc -eq 127 ]; then
    sleep 2
    "$@" >"$out" 2>&1
    rc=$?
  fi
  return $rc
}

# Un echec muet coute une demi-heure de diagnostic : on montre la sortie.
check_exit() { # $1 label  $2 rc  $3 fichier de sortie
  if [ "$2" -eq 0 ]; then
    ok "$1"
  else
    ko "$1 : exit $2"
    echo "  ---- sortie ($3, 30 dernieres lignes) ----"
    tail -n 30 "$3" 2>/dev/null | sed 's/^/  | /'
    echo "  ---- fin sortie ----"
  fi
}

run_resilient "$out1" $OWL --self-test-compile-error & p1=$!
run_resilient "$out2" $OWL --self-test-compile-error & p2=$!
run_resilient "$out3" $OWL test build & p3=$!
wait $p1; r1=$?
wait $p2; r2=$?
wait $p3; r3=$?
check_exit "self-test #1 exit 0" "$r1" "$out1"
check_exit "self-test #2 exit 0" "$r2" "$out2"

# Un exit 0 sur 0 suite ne prouverait rien : on exige des tests reellement joues.
suites=$(grep -o '([0-9]\+ suites' "$out3" 2>/dev/null | grep -o '[0-9]\+' | tail -n 1)
if [ "$r3" -eq 0 ] && [ "${suites:-0}" -ge 100 ]; then
  ok "owl test concurrent exit 0 sur ${suites} suites reelles (walk de build/ pendant la course)"
else
  ko "owl test concurrent : exit $r3, ${suites:-0} suites jouees (attendu exit 0 et >= 100 suites)"
  echo "  ---- sortie ($out3, 30 dernieres lignes) ----"
  tail -n 30 "$out3" 2>/dev/null | sed 's/^/  | /'
  echo "  ---- fin sortie ----"
fi
rm -rf "$race_dir" "$out1" "$out2" "$out3"

echo
echo "$pass/$((pass+fail)) OK"
[ "$fail" -eq 0 ] || exit 1
