#!/usr/bin/env bash
#  : revele une troncature silencieuse de la table de prescan
# classMethodVis[512] (src/prescan.h), table SŒUR de classMethods[512]
# mais dediee a la VISIBILITE (PUBLIC/PRIVE/PROTEGE) des methodes
# de classe pour les checks cross-fichier POO qui n'ont pas full-compile les
# voisins — exactement le chemin `owl --check <fichier> --project <dir>`
# (celui qu'utilise le LSP au save,  / ).
#
# Temoin : une classe cTarget avec N methodes PUBLIC triviales suivies d'une
# methode PROCEDURE PRIVÉE Secret(). Un second fichier du meme projet
# (other.owl, hors de la classe) tente l'acces illegal `o.Secret()`. En
# `owl --check <fichier> --project <dir>` (prescan seul, pas de full-compile
# des voisins), la visibilite de Secret n'est connue QUE via la table
# classMethodVis[] hydratee par compiler_hydrate_method_vis_from_symbols
# (src/compiler.c, ). Cette table n'enregistre une entree que si son
# index global est < 512 (src/prescan.c:1069, garde
# "t->classMethodVisCount < 512" — sinon abandon silencieux, sans message).
#
# Quand N est assez grand pour que l'entree de Secret tombe au-dela de cet
# index, l'acces illegal `o.Secret()` depuis other.owl n'est PLUS REFUSE par
# `--check --project` : la visibilite de Secret est inconnue -> le garde de
# visibilite (compiler.c ~14211, "vis >= 0 && vis != OWL_VIS_PUBLIC") ne se
# declenche jamais -> AUCUNE erreur n'est emise. C'est un FAUX NEGATIF
# silencieux (acces prive illegalement autorise en diagnostic statique), pas
# un crash ni un faux positif. La compilation complete (`owl test`, qui
# compile reellement le fichier de classe dans le meme run plutot que de
# s'appuyer sur la table de prescan tronquee) refuse correctement l'acces
# DANS LES DEUX CAS : le symptome est borne au chemin mono-fichier+projet.
#
# Deux cas, generes a la volee (aucun fichier de 600+ methodes committe) :
#   under  400 methodes avant Secret (index < 512)  -> --check --project REFUSE (sain)
#   over   600 methodes avant Secret (index > 512)  -> --check --project ACCEPTE a tort
#            (observe aujourd'hui : errors:[] vide -- c'est exactement la
#             troncature a corriger, pas une erreur de capacite nommee)
#
# Usage :
#   bash tests/limits/prescan_classmethodvis_truncation.sh
#   OWL_BIN=./build/owl.exe bash tests/limits/prescan_classmethodvis_truncation.sh

set -u

OWL="${OWL_BIN:-./build/owl.exe}"
WORK="${OWL_LIMITS_WORK:-build/.prescan_classmethodvis_probe_$$}"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

seqn() { local n=$1 i=1; while [ "$i" -le "$n" ]; do echo "$i"; i=$((i + 1)); done; }

# gen_project <N methodes PUBLIC avant Secret> <dossier de sortie>
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
  {
    echo "cTarget est une Classe"
    echo "PUBLIC"
    echo "	PROCEDURE Constructeur()"
    echo "	FIN"
    echo
    for k in $(seqn "$N"); do
      echo "	PROCEDURE PUBLIC F$k() : entier"
      echo "		RENVOYER $k"
      echo "	FIN"
      echo
    done
    echo "	PROCEDURE PRIVÉE Secret() : entier"
    echo "		RENVOYER 999"
    echo "	FIN"
    echo "FIN"
  } > "$d/cTarget.owl"
  {
    echo 'o est un cTarget()'
    echo 'n est un entier = o.Secret()'
    echo 'dbgVérifieEgalité(n, 999, "acces illegal a Secret() -- ne devrait jamais executer sainement")'
    echo 'dbgBilanTest()'
  } > "$d/other.owl"
}

n_fail=0
echo "Sonde anti-troncature prescan classMethodVis[512] (fix/2505)"

# --- Cas sous l'ancien plafond : controle, --check --project doit refuser
# l'acces illegal a Secret() aujourd'hui.
d="$WORK/under"; gen_project 400 "$d"
out=$("$OWL" --check "$d/other.owl" --project "$d" 2>&1)
if echo "$out" | grep -F -q "est PRIVÉ"; then
  echo "  OK --check --project (400 methodes avant Secret, sous le plafond) : acces illegal refuse"
else
  echo "  ECHEC --check --project (sous plafond, 400 methodes avant Secret) : acces illegal NON refuse !"
  echo "        $out"
  n_fail=$((n_fail + 1))
fi
run_out=$("$OWL" test "$d/other.owl" -vv 2>&1)
if echo "$run_out" | grep -q "CRASH" && echo "$run_out" | grep -F -q "est PRIVÉ"; then
  echo "  OK execution (sous plafond, compilation complete refuse aussi l'acces)"
else
  echo "  ATTENTION execution (sous plafond) : resultat inattendu"
  echo "$run_out"
fi

# --- Cas au-dela de l'ancien plafond : LE temoin. --check --project doit
# refuser l'acces illegal a Secret() EXACTEMENT comme le cas 'under'. Si une
# limite de capacite subsiste et est atteinte, le message doit la NOMMER
# explicitement (error_capacity_exceeded ou equivalent) — jamais un silence
# qui laisse passer l'acces illegal.
d="$WORK/over"; gen_project 600 "$d"
out=$("$OWL" --check "$d/other.owl" --project "$d" 2>&1)
if echo "$out" | grep -F -q "est PRIVÉ"; then
  echo "  OK --check --project (600 methodes avant Secret, au-dela de l'ancien plafond) : acces illegal refuse"
elif echo "$out" | grep -q '"errors":\[\]'; then
  echo "  ECHEC --check --project (au-dela du plafond, 600 methodes avant Secret) :"
  echo "        FAUX NEGATIF -- Secret() est PRIVÉE mais la table de prescan a"
  echo "        tronque silencieusement son entree de visibilite au-dela de"
  echo "        l'index 512 : l'acces illegal o.Secret() n'est PAS refuse."
  echo "        sortie : $out"
  n_fail=$((n_fail + 1))
else
  echo "  ATTENTION --check --project (au-dela du plafond) refuse pour une autre"
  echo "             raison -- verifier qu'elle NOMME une limite de capacite"
  echo "             explicite : $out"
fi
run_out=$("$OWL" test "$d/other.owl" -vv 2>&1)
if echo "$run_out" | grep -q "CRASH" && echo "$run_out" | grep -F -q "est PRIVÉ"; then
  echo "  OK execution (au-dela du plafond, compilation complete refuse aussi l'acces -- le"
  echo "       symptome est borne au chemin --check --project mono-fichier)"
else
  echo "  ATTENTION execution (au-dela du plafond) : resultat inattendu"
  echo "$run_out"
fi

echo "  ---"
printf '  %d echec(s)\n' "$n_fail"
exit "$n_fail"
