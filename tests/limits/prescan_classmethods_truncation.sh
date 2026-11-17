#!/usr/bin/env bash
#  : revele une troncature silencieuse de la table de prescan
# classMethods[512] (src/prescan.h), distincte et PLUS BASSE que la limite
# publiee MAX_DECLARED_METHODS (src/owl_limits.def). Cette table n'est PAS
# exposee par `owl list limits --json` : le sondeur generique de
# tests/limits/run.sh ne peut donc pas la couvrir, d'ou ce probe dedie.
#
# Temoin : une classe abstraite cBase (methode abstraite MA) et une classe
# concrete VOISINE cDerived qui redefinit MA, APRES N methodes triviales. La
# table de prescan n'enregistre une signature de methode de classe que si
# son index global est < 512 (cf. prescan.c, condition
# "t->classMethodCount < 512" — sinon elle est abandonnee sans message).
# Quand N est assez grand pour que l'entree de MA tombe au-dela de cet
# index, un `--check` sur un TROISIEME fichier du meme projet (other.owl)
# accuse a tort cDerived de ne jamais redefinir MA — alors qu'elle le fait
# bel et bien. La vraie execution (`owl test`), elle, resout correctement
# l'appel cross-fichier o.MA() car elle repose sur le compile complet des
# voisins, pas sur cette table de prescan tronquee : le symptome est donc
# un FAUX POSITIF de compilation statique (`--check`/LSP), pas un echec a
# l'execution.
#
# Deux cas, generes a la volee (aucun fichier de 600 methodes committe) :
#   under  511 methodes avant MA (index < 512)  -> doit compiler ET executer
#   over   600 methodes avant MA (index > 512)  -> doit compiler ET executer
#            (observe aujourd'hui : --check rejette a tort avec le message
#             "n'est pas abstraite mais herite de la methode abstraite" —
#             c'est exactement la troncature a corriger, pas une erreur
#             de capacite nommee)
#
# Usage :
#   bash tests/limits/prescan_classmethods_truncation.sh
#   OWL_BIN=./build/owl.exe bash tests/limits/prescan_classmethods_truncation.sh

set -u

OWL="${OWL_BIN:-./build/owl.exe}"
# meme convention que tests/limits/run.sh : base de travail dans build/,
# nom commencant par un point pour rester invisible aux walkers `owl test`.
WORK="${OWL_LIMITS_WORK:-build/.prescan_classmethods_probe_$$}"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

seqn() { local n=$1 i=1; while [ "$i" -le "$n" ]; do echo "$i"; i=$((i + 1)); done; }

# gen_project <N methodes triviales avant MA> <dossier de sortie>
gen_project() {
  local N=$1 d=$2 k
  mkdir -p "$d"
  cat > "$d/projet.owl" <<'EOF'
projet est un Projet
	PROPRIETES
		Langues = "fr"
	FIN
FIN
EOF
  cat > "$d/cBase.owl" <<'EOF'
cBase est une Classe, abstraite
PUBLIC
	nA est un entier
	PROCEDURE ABSTRAITE MA() : entier
FIN
EOF
  { echo "cDerived est une Classe"
    echo "	hérite de cBase"
    echo "PUBLIC"
    for k in $(seqn "$N"); do
      echo "	PROCEDURE M$k() : entier"
      echo "		RENVOYER $k"
      echo "	FIN"
    done
    echo "	PROCEDURE MA() : entier"
    echo "		RENVOYER 999"
    echo "	FIN"
    echo "FIN"
  } > "$d/cDerived.owl"
  {
    echo 'o est un cDerived'
    echo 'n est un entier = o.MA()'
    echo 'dbgVérifieEgalité(n, 999, "appel cross-fichier o.MA() resout a 999")'
    echo 'dbgBilanTest()'
  } > "$d/other.owl"
}

n_fail=0
echo "Sonde anti-troncature prescan classMethods[512] (fix/2504)"

# --- Cas sous l'ancien plafond : controle, doit deja etre sain aujourd'hui.
d="$WORK/under"; gen_project 511 "$d"
out=$("$OWL" --check "$d/other.owl" 2>&1)
if ! echo "$out" | grep -q '"errors":\[\]'; then
  echo "  ECHEC --check (sous plafond, 511 methodes avant MA) : $out"
  n_fail=$((n_fail + 1))
else
  echo "  OK --check (511 methodes avant MA, sous l'ancien plafond)"
fi
run_out=$("$OWL" test "$d/other.owl" -vv 2>&1)
if ! echo "$run_out" | grep -q "PASS"; then
  echo "  ECHEC execution (sous plafond) :"
  echo "$run_out"
  n_fail=$((n_fail + 1))
else
  echo "  OK execution (sous plafond, o.MA() resout a 999)"
fi

# --- Cas au-dela de l'ancien plafond : LE temoin. --check doit reconnaitre
# la redefinition (table corrigee) et ne JAMAIS emettre le faux-positif
# "methode abstraite non redefinie". Si une limite de capacite subsiste et
# est atteinte, le message doit la NOMMER explicitement (error_capacity_
# exceeded ou equivalent) — jamais ce faux-positif muet sur autre chose.
d="$WORK/over"; gen_project 600 "$d"
out=$("$OWL" --check "$d/other.owl" 2>&1)
if echo "$out" | grep -F -q "n'est pas abstraite mais hérite de la méthode abstraite"; then
  echo "  ECHEC --check (au-dela du plafond, 600 methodes avant MA) :"
  echo "        FAUX POSITIF -- cDerived redefinit bien MA, mais la table de"
  echo "        prescan a tronque silencieusement son entree au-dela de"
  echo "        l'index 512 : $out"
  n_fail=$((n_fail + 1))
elif ! echo "$out" | grep -q '"errors":\[\]'; then
  echo "  ATTENTION --check (au-dela du plafond) refuse pour une autre"
  echo "             raison -- verifier qu'elle NOMME une limite de"
  echo "             capacite explicite : $out"
else
  echo "  OK --check (600 methodes avant MA, au-dela de l'ancien plafond)"
fi
run_out=$("$OWL" test "$d/other.owl" -vv 2>&1)
if ! echo "$run_out" | grep -q "PASS"; then
  echo "  ECHEC execution (au-dela du plafond) : resolution cross-fichier cassee"
  echo "$run_out"
  n_fail=$((n_fail + 1))
else
  echo "  OK execution (au-dela du plafond, o.MA() resout a 999)"
fi

echo "  ---"
printf '  %d echec(s)\n' "$n_fail"
exit "$n_fail"
