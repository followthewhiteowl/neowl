#!/usr/bin/env bash
# Harnais boite noire sur 4 familles de tokens que `owl translate` laisse
# en francais silencieusement.
#
# Contrat verrouille ici, mesure a la main avant impl :
#   A. cles DSL du bloc `est un Projet`      : Langues<->Languages,
#      FenetreDemarrage<->StartupWindow.       CASSE dans LES DEUX sens.
#   B. acces propriete en cascade `..Prop`   : ..Valeur<->..Value (Tenseur),
#      ..Libelle<->..Caption (widget).         CASSE UNIQUEMENT --to en :
#      --to fr traduit deja correctement (asymetrie mesuree, pas une
#      hypothese — cf rapport test_writer). Le contrat verrouille donc les
#      DEUX faces separement : le sens casse (rouge attendu) ET le sens deja
#      correct (garde de non-regression, pour detecter une casse future).
#   C. methodes fluentes accentuees/specifiques du Tenseur :
#      .Retropropage<->.Backpropagate, .EntropieCroisee<->.CrossEntropy,
#      .DescenteGradient<->.GradientDescent.   CASSE dans LES DEUX sens.
#   D. elision `d'` devant voyelle             : `tableau d'entiers` ->
#      `array of integer` (FR->EN CASSE : sortie mixte `array d'integer`).
#      `array of integer` -> `tableau d'entiers` (EN->FR CASSE aussi :
#      decision dev, `de`+voyelle doit s'elider en `d'` cote francais.
#      Rouge attendu tant que l'elision EN->FR n'est pas implementee —
#      pas de garde de non-regression sur cette face, les deux sens sont
#      desormais alignes sur le meme contrat.)
#
# + round-trip sur fichier composite (les 4 familles) et non-regression sur
#   ce qui marche deja (mots-cles, natives, types, constantes d'enum, props
#   DANS un bloc PROPRIETES, methodes fluentes deja couvertes).
#
# Usage:
#   ./run.sh
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

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t owl_tr_fam)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

ok()  { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
ko()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }

# check <id> <attendu-present-1> <attendu-present-2...>  -- helper generique
# ci-dessous, on utilise directement grep -q pour rester lisible par famille.

# translate_to <lang> <src> <dossier_sortie> — sortie isolee dans son dossier.
translate_to() {
    local lang="$1" src="$2" out="$WORK/$3"
    mkdir -p "$out"
    "$OWL" translate --to "$lang" "$src" > "$out/a.owl" 2>"$out/err.txt"
    printf '%s' "$out/a.owl"
}

# compiles <fichier> — vrai si --check rend rc 0.
compiles() { "$OWL" --check "$1" >/dev/null 2>&1; }

# contains <fichier> <needle> — vrai si le needle est present (grep -F).
contains() { grep -qF -- "$2" "$1" 2>/dev/null; }

# absent <fichier> <needle> — vrai si le needle est absent (grep -F).
absent() { ! grep -qF -- "$2" "$1" 2>/dev/null; }

# ================================================================
# A. Cles DSL du bloc `est un Projet` : Langues<->Languages
# ================================================================
echo "== A. Cles DSL bloc Projet (Langues/Languages)"

mkdir -p "$WORK/a_fr"
printf 'MonApp est un Projet\n\tPROPRIETES\n\t\tLangues = [langueFran\xc3\xa7ais]\n\tFIN\nFIN\n' \
    > "$WORK/a_fr/projet.owl"
out="$(translate_to en "$WORK/a_fr/projet.owl" "a_fr_en")"
if contains "$out" "Languages =" && absent "$out" "Langues ="; then
    ok "A/Langues -> Languages (FR->EN)"
else
    ko "A/Langues -> Languages (FR->EN) — sortie : $(cat "$out" 2>/dev/null | tr '\n' '|')"
fi
if compiles "$out"; then ok "A/sortie EN compile"; else ko "A/sortie EN compile — rc != 0"; fi

mkdir -p "$WORK/a_en"
printf 'MyApp is a Project\n\tPROPERTIES\n\t\tLanguages = [languageFrench]\n\tEND\nEND\n' \
    > "$WORK/a_en/projet.owl"
out="$(translate_to fr "$WORK/a_en/projet.owl" "a_en_fr")"
if contains "$out" "Langues =" && absent "$out" "Languages ="; then
    ok "A/Languages -> Langues (EN->FR)"
else
    ko "A/Languages -> Langues (EN->FR) — sortie : $(cat "$out" 2>/dev/null | tr '\n' '|')"
fi
if compiles "$out"; then ok "A/sortie FR compile"; else ko "A/sortie FR compile — rc != 0"; fi

# ================================================================
# A'. FenetreDemarrage<->StartupWindow (companion Fenetre requis pour compiler)
# ================================================================
echo "== A'. Cles DSL bloc Projet (FenetreDemarrage/StartupWindow)"

mkdir -p "$WORK/ap_fr"
cat > "$WORK/ap_fr/FEN_Accueil.owl" <<'EOF'
FEN_Accueil est une Fenêtre
	PROPRIETES
		Titre = "Accueil"
	FIN
FIN
EOF
printf 'MonApp est un Projet\n\tPROPRIETES\n\t\tLangues          = [langueFran\xc3\xa7ais]\n\t\tFen\xc3\xaatreD\xc3\xa9marrage = FEN_Accueil\n\tFIN\nFIN\n' \
    > "$WORK/ap_fr/projet.owl"
"$OWL" translate --to en "$WORK/ap_fr/projet.owl" > "$WORK/ap_fr/projet_en.owl" 2>/dev/null
if contains "$WORK/ap_fr/projet_en.owl" "StartupWindow" \
   && absent "$WORK/ap_fr/projet_en.owl" "FenêtreDémarrage"; then
    ok "A'/FenêtreDémarrage -> StartupWindow (FR->EN)"
else
    ko "A'/FenêtreDémarrage -> StartupWindow (FR->EN) — sortie : $(cat "$WORK/ap_fr/projet_en.owl" | tr '\n' '|')"
fi
# La sortie EN + le companion (non traduit, l'identifiant FEN_Accueil ne
# bouge pas) doivent compiler ensemble.
mkdir -p "$WORK/ap_fr_en_ctx"
cp "$WORK/ap_fr/projet_en.owl" "$WORK/ap_fr_en_ctx/projet.owl"
cp "$WORK/ap_fr/FEN_Accueil.owl" "$WORK/ap_fr_en_ctx/FEN_Accueil.owl"
if compiles "$WORK/ap_fr_en_ctx/projet.owl"; then ok "A'/sortie EN + companion compile"
else ko "A'/sortie EN + companion compile — rc != 0"; fi

# ================================================================
# B. Acces propriete en cascade `..Prop` : ..Valeur<->..Value (Tenseur)
# ================================================================
echo "== B. Cascade ..Prop sur Tenseur (..Valeur/..Value)"

mkdir -p "$WORK/b_fr"
printf 'v est un r\xc3\xa9el = tc..Valeur[1]\n' > "$WORK/b_fr/a.owl"
out="$(translate_to en "$WORK/b_fr/a.owl" "b_fr_en")"
if contains "$out" "..Value" && absent "$out" "..Valeur"; then
    ok "B/..Valeur -> ..Value (FR->EN)"
else
    ko "B/..Valeur -> ..Value (FR->EN) — sortie : $(cat "$out" | tr '\n' '|')"
fi

# Garde de non-regression : le sens --to fr traduit deja ..Value -> ..Valeur
# correctement (mesure avant impl). Un rouge ici serait une VRAIE regression,
# pas le sujet du ticket.
mkdir -p "$WORK/b_en"
printf 'v is a real = tc..Value[1]\n' > "$WORK/b_en/a.owl"
out="$(translate_to fr "$WORK/b_en/a.owl" "b_en_fr")"
if contains "$out" "..Valeur" && absent "$out" "..Value"; then
    ok "B/..Value -> ..Valeur (EN->FR, deja correct — garde de non-regression)"
else
    ko "B/..Value -> ..Valeur (EN->FR) — sortie : $(cat "$out" | tr '\n' '|')"
fi

# ================================================================
# B'. Acces propriete en cascade `..Prop` sur widget : ..Libelle<->..Caption
# ================================================================
echo "== B'. Cascade ..Prop sur widget (..Libellé/..Caption)"

mkdir -p "$WORK/bp_fr"
cat > "$WORK/bp_fr/FEN_X.owl" <<'EOF'
FEN_X est une Fenêtre
	PROPRIETES
		Titre       = "Test"
		Disposition = dispLibre
	FIN

	BTN_X est un Champ Bouton
		PROPRIETES
			Libellé = "Valider"
			X       = 10
			Y       = 10
			Largeur = 100
			Hauteur = 30
		FIN
	FIN
FIN
EOF
cat > "$WORK/bp_fr/a.owl" <<'EOF'
TestModeHeadless(Vrai)
Ouvre(FEN_X)
s est une chaîne = BTN_X..Libellé
BTN_X..Libellé = "OK"
Ferme(FEN_X)
EOF
"$OWL" translate --to en "$WORK/bp_fr/a.owl" > "$WORK/bp_fr/a_en.owl" 2>/dev/null
if contains "$WORK/bp_fr/a_en.owl" "..Caption" && absent "$WORK/bp_fr/a_en.owl" "..Libellé"; then
    ok "B'/..Libellé -> ..Caption (FR->EN, widget)"
else
    ko "B'/..Libellé -> ..Caption (FR->EN, widget) — sortie : $(cat "$WORK/bp_fr/a_en.owl" | tr '\n' '|')"
fi
mkdir -p "$WORK/bp_fr_en_ctx"
cp "$WORK/bp_fr/a_en.owl" "$WORK/bp_fr_en_ctx/a.owl"
cp "$WORK/bp_fr/FEN_X.owl" "$WORK/bp_fr_en_ctx/FEN_X.owl"
if compiles "$WORK/bp_fr_en_ctx/a.owl"; then ok "B'/sortie EN + companion compile (bilingue accepte au compile-time)"
else ko "B'/sortie EN + companion compile — rc != 0"; fi

# ================================================================
# C. Methodes fluentes Tenseur : Retropropage/Backpropagate,
#    EntropieCroisee/CrossEntropy, DescenteGradient/GradientDescent
# ================================================================
echo "== C. Methodes fluentes Tenseur"

mkdir -p "$WORK/c_fr"
cat > "$WORK/c_fr/a.owl" <<'EOF'
ta est un Tenseur = Tenseur([2.0])
tb est un Tenseur = Tenseur([3.0])
tc est un Tenseur = ta.EntropieCroisée(tb)
tc.Rétropropage()
td est un Tenseur = ta.DescenteGradient(0.1)
EOF
out="$(translate_to en "$WORK/c_fr/a.owl" "c_fr_en")"
if contains "$out" ".Backpropagate(" && absent "$out" ".Rétropropage("; then
    ok "C/.Rétropropage -> .Backpropagate (FR->EN)"
else
    ko "C/.Rétropropage -> .Backpropagate (FR->EN) — sortie : $(cat "$out" | tr '\n' '|')"
fi
if contains "$out" ".CrossEntropy(" && absent "$out" ".EntropieCroisée("; then
    ok "C/.EntropieCroisée -> .CrossEntropy (FR->EN)"
else
    ko "C/.EntropieCroisée -> .CrossEntropy (FR->EN) — sortie : $(cat "$out" | tr '\n' '|')"
fi
if contains "$out" ".GradientDescent(" && absent "$out" ".DescenteGradient("; then
    ok "C/.DescenteGradient -> .GradientDescent (FR->EN)"
else
    ko "C/.DescenteGradient -> .GradientDescent (FR->EN) — sortie : $(cat "$out" | tr '\n' '|')"
fi
if compiles "$out"; then ok "C/sortie EN compile (Tenseur autonome)"
else ko "C/sortie EN compile — rc != 0"; fi

mkdir -p "$WORK/c_en"
cat > "$WORK/c_en/a.owl" <<'EOF'
ta is a Tensor = Tensor([2.0])
tb is a Tensor = Tensor([3.0])
tc is a Tensor = ta.CrossEntropy(tb)
tc.Backpropagate()
td is a Tensor = ta.GradientDescent(0.1)
EOF
out="$(translate_to fr "$WORK/c_en/a.owl" "c_en_fr")"
if contains "$out" ".Rétropropage(" && absent "$out" ".Backpropagate("; then
    ok "C/.Backpropagate -> .Rétropropage (EN->FR)"
else
    ko "C/.Backpropagate -> .Rétropropage (EN->FR) — sortie : $(cat "$out" | tr '\n' '|')"
fi
if contains "$out" ".EntropieCroisée(" && absent "$out" ".CrossEntropy("; then
    ok "C/.CrossEntropy -> .EntropieCroisée (EN->FR)"
else
    ko "C/.CrossEntropy -> .EntropieCroisée (EN->FR) — sortie : $(cat "$out" | tr '\n' '|')"
fi
if contains "$out" ".DescenteGradient(" && absent "$out" ".GradientDescent("; then
    ok "C/.GradientDescent -> .DescenteGradient (EN->FR)"
else
    ko "C/.GradientDescent -> .DescenteGradient (EN->FR) — sortie : $(cat "$out" | tr '\n' '|')"
fi
if compiles "$out"; then ok "C/sortie FR compile"; else ko "C/sortie FR compile — rc != 0"; fi

# Non-regression : les methodes fluentes deja couvertes restent traduites.
mkdir -p "$WORK/c_nr"
cat > "$WORK/c_nr/a.owl" <<'EOF'
ta est un Tenseur = Tenseur([[1.0, 2.0]])
tb est un Tenseur = Tenseur([[3.0], [4.0]])
r est un Tenseur = ta.ProduitMatriciel(tb)
s est un Tenseur = ta.ReLU()
EOF
out="$(translate_to en "$WORK/c_nr/a.owl" "c_nr_en")"
if contains "$out" ".MatrixProduct(" && absent "$out" ".ProduitMatriciel("; then
    ok "C/non-regression .ProduitMatriciel -> .MatrixProduct"
else
    ko "C/non-regression .ProduitMatriciel -> .MatrixProduct — sortie : $(cat "$out" | tr '\n' '|')"
fi
if contains "$out" ".ReLU("; then
    ok "C/non-regression .ReLU identique FR/EN"
else
    ko "C/non-regression .ReLU identique FR/EN — sortie : $(cat "$out" | tr '\n' '|')"
fi

# ================================================================
# D. Elision `d'` devant voyelle : tableau d'entiers <-> array of integer
# ================================================================
echo "== D. Elision d' (tableau d'entiers / array of integer)"

mkdir -p "$WORK/d_fr"
printf "idx est un tableau d'entiers\n" > "$WORK/d_fr/a.owl"
out="$(translate_to en "$WORK/d_fr/a.owl" "d_fr_en")"
if contains "$out" "array of integer" && absent "$out" "d'integer"; then
    ok "D/tableau d'entiers -> array of integer (FR->EN)"
else
    ko "D/tableau d'entiers -> array of integer (FR->EN) — sortie : $(cat "$out" | tr '\n' '|')"
fi

mkdir -p "$WORK/d_fr_img"
printf "idx est un tableau d'Image\n" > "$WORK/d_fr_img/a.owl"
out="$(translate_to en "$WORK/d_fr_img/a.owl" "d_fr_img_en")"
if contains "$out" "array of Image" && absent "$out" "d'Image"; then
    ok "D/tableau d'Image -> array of Image (FR->EN)"
else
    ko "D/tableau d'Image -> array of Image (FR->EN) — sortie : $(cat "$out" | tr '\n' '|')"
fi

# Decision dev : `owl translate --to fr` DOIT elider `de`+voyelle
# en `d'` (francais correct). Rouge attendu ici tant que l'implementer n'a
# pas ajoute l'elision cote EN->FR — le formatter n'est PAS touche (il
# tolere toujours "de entiers", follow-up separe).
mkdir -p "$WORK/d_en"
printf "idx is an array of integer\n" > "$WORK/d_en/a.owl"
out="$(translate_to fr "$WORK/d_en/a.owl" "d_en_fr")"
if contains "$out" "tableau d'entiers" && absent "$out" "of integer"; then
    ok "D/array of integer -> tableau d'entiers (EN->FR, elision)"
else
    ko "D/array of integer -> tableau d'entiers (EN->FR) — sortie : $(cat "$out" | tr '\n' '|')"
fi

# ================================================================
# E. Round-trip : fichier composite (les 4 familles) FR -> EN -> FR
# ================================================================
echo "== E. Round-trip FR->EN->FR sur fichier composite"

mkdir -p "$WORK/e_src"
cat > "$WORK/e_src/projet.owl" <<'EOF'
MonApp est un Projet
	PROPRIETES
		Langues = [langueFrançais]
	FIN

	ta est un Tenseur = Tenseur([2.0])
	tb est un Tenseur = Tenseur([3.0])
	tc est un Tenseur = ta.EntropieCroisée(tb)
	tc.Rétropropage()
	td est un Tenseur = ta.DescenteGradient(0.1)
	v est un réel = tc..Valeur[1]
	idx est un tableau d'entiers
FIN
EOF
cp "$WORK/e_src/projet.owl" "$WORK/e_src/canon.owl"
"$OWL" fmt "$WORK/e_src/canon.owl" >/dev/null 2>&1

en1="$(translate_to en "$WORK/e_src/projet.owl" "e_en1")"
fr2="$(translate_to fr "$en1" "e_fr2")"
if cmp -s "$fr2" "$WORK/e_src/canon.owl"; then
    ok "E/round-trip FR->EN->FR == fmt(source) — point fixe atteint"
else
    ko "E/round-trip FR->EN->FR != fmt(source) — au moins une des 4 familles casse le round-trip (attendu apres impl, PAS avant)"
fi
# Facette isolee : le round-trip perd le PLURIEL sur le tableau d'entiers,
# via la forme mixte 'array d'integer' non reconnue comme le motif pluriel
# 'array of X' au retour EN->FR (constat, edge case en plus de la matrice).
if contains "$fr2" "tableau d'entiers"; then
    ok "E/pluriel 'entiers' preserve au round-trip"
else
    ko "E/pluriel 'entiers' preserve au round-trip — obtenu : $(grep -F 'tableau' "$fr2" 2>/dev/null | tr '\n' '|')"
fi

# ================================================================
# F. Non-regression generale (deja traduit correctement, aucune casse ici)
# ================================================================
echo "== F. Non-regression : mots-cles, natives, types, constantes enum, PROPRIETES"

mkdir -p "$WORK/f_kw"
cat > "$WORK/f_kw/a.owl" <<'EOF'
n est un entier
POUR i = 1 A 3
	SI n > 0 ALORS
		Trace(fLitLigne(1))
	FIN
FIN
t est un Tenseur
c est un entier = langueFrançais
EOF
out="$(translate_to en "$WORK/f_kw/a.owl" "f_kw_en")"
if contains "$out" "FOR " && absent "$out" "POUR "; then ok "F/POUR -> FOR"
else ko "F/POUR -> FOR — sortie : $(cat "$out" | tr '\n' '|')"; fi
if contains "$out" "IF " && absent "$out" "SI "; then ok "F/SI -> IF"
else ko "F/SI -> IF — sortie : $(cat "$out" | tr '\n' '|')"; fi
if contains "$out" "END"; then ok "F/FIN -> END"
else ko "F/FIN -> END — sortie : $(cat "$out" | tr '\n' '|')"; fi
if contains "$out" "fReadLine(" && absent "$out" "fLitLigne("; then ok "F/fLitLigne -> fReadLine"
else ko "F/fLitLigne -> fReadLine — sortie : $(cat "$out" | tr '\n' '|')"; fi
if contains "$out" "integer" && absent "$out" "entier"; then ok "F/entier -> integer"
else ko "F/entier -> integer — sortie : $(cat "$out" | tr '\n' '|')"; fi
if contains "$out" "Tensor" && absent "$out" "Tenseur"; then ok "F/Tenseur -> Tensor"
else ko "F/Tenseur -> Tensor — sortie : $(cat "$out" | tr '\n' '|')"; fi
if contains "$out" "languageFrench" && absent "$out" "langueFrançais"; then ok "F/langueFrançais -> languageFrench"
else ko "F/langueFrançais -> languageFrench — sortie : $(cat "$out" | tr '\n' '|')"; fi
if compiles "$out"; then ok "F/sortie EN compile"; else ko "F/sortie EN compile — rc != 0"; fi

mkdir -p "$WORK/f_props"
cat > "$WORK/f_props/FEN_X.owl" <<'EOF'
FEN_X est une Fenêtre
	BTN_X est un Champ Bouton
		PROPRIETES
			Libellé = "OK"
		FIN
	FIN
FIN
EOF
out="$(translate_to en "$WORK/f_props/FEN_X.owl" "f_props_en")"
if contains "$out" "Caption =" && absent "$out" "Libellé ="; then
    ok "F/Libellé -> Caption DANS un bloc PROPRIETES (deja couvert)"
else
    ko "F/Libellé -> Caption DANS un bloc PROPRIETES — sortie : $(cat "$out" | tr '\n' '|')"
fi

# ================================================================
# G. Article de declaration devant une classe utilisateur
#    EN->FR : `is a <ClasseUser>` -> `est un <ClasseUser>`. Une classe user n'a
#    PAS de genre grammatical cote OWL : defaut masculin `un` (choix assume,
#    consigne dans le ticket). Une classe au nom feminin rend donc AUSSI `un` —
#    l'info de genre n'existe pas, on ne la devine pas. Le type natif garde son
#    genre (Image feminin -> une), car il vit dans le registre. Le sens FR->EN
#    etait deja correct (un/une -> a phonetique), garde de non-regression.
# ================================================================
echo "== G. Article devant classe utilisateur (defaut masculin un)"

mkdir -p "$WORK/g_en"
cat > "$WORK/g_en/cJoueur.owl" <<'EOF'
cJoueur est une Classe
	nom est une chaîne
FIN
EOF
cat > "$WORK/g_en/cJoueuse.owl" <<'EOF'
cJoueuse est une Classe
	nom est une chaîne
FIN
EOF
cat > "$WORK/g_en/a.owl" <<'EOF'
j is a cJoueur
p is a cJoueuse
img is an Image
EOF
out="$(translate_to fr "$WORK/g_en/a.owl" "g_en_fr")"
if contains "$out" "est un cJoueur" && absent "$out" "est a cJoueur"; then
    ok "G/is a cJoueur -> est un cJoueur (EN->FR)"
else
    ko "G/is a cJoueur -> est un cJoueur — sortie : $(cat "$out" | tr '\n' '|')"
fi
# Classe au nom feminin : OWL n'a pas de genre de classe -> defaut `un` assume.
if contains "$out" "est un cJoueuse"; then
    ok "G/classe au nom feminin -> defaut masculin un (choix assume)"
else
    ko "G/classe au nom feminin -> un — sortie : $(cat "$out" | tr '\n' '|')"
fi
# Non-regression : un type natif garde son genre (Image = feminin -> une).
if contains "$out" "est une Image"; then
    ok "G/type natif genre conserve (une Image, non-regression)"
else
    ko "G/une Image — sortie : $(cat "$out" | tr '\n' '|')"
fi

# Le sens FR->EN etait deja correct (un/une -> a phonetique) : garde de non-reg.
mkdir -p "$WORK/g_fr"
cat > "$WORK/g_fr/a.owl" <<'EOF'
j est un cJoueur
EOF
out="$(translate_to en "$WORK/g_fr/a.owl" "g_fr_en")"
if contains "$out" "j is a cJoueur"; then
    ok "G/est un cJoueur -> is a cJoueur (FR->EN, deja correct)"
else
    ko "G/est un cJoueur -> is a cJoueur — sortie : $(cat "$out" | tr '\n' '|')"
fi

# Sortie EN->FR compile avec ses companions (article correct = code valide ;
# `est a cJoueur` ne compilait pas).
mkdir -p "$WORK/g_ctx"
cp "$WORK/g_en_fr/a.owl" "$WORK/g_ctx/a.owl"
cp "$WORK/g_en/cJoueur.owl" "$WORK/g_ctx/cJoueur.owl"
cp "$WORK/g_en/cJoueuse.owl" "$WORK/g_ctx/cJoueuse.owl"
if compiles "$WORK/g_ctx/a.owl"; then ok "G/sortie FR + companions compile"
else ko "G/sortie FR + companions compile — rc != 0"; fi

# ================================================================
# H. Pas de boucle negatif : `PAS -1` / `STEP -1` ne gagne pas d'espace
#    Le `-1` est un signe UNAIRE sur le litteral, pas un binaire. La forme EN
#    `STEP` n'etait pas reconnue comme ouvreur de contexte unaire par la
#    normalisation d'espacement (alors que `PAS`/`NOT` l'etaient) -> `STEP - 1`.
#    La negation `PAS`/`NOT` (deja correcte) sert de garde de non-regression.
# ================================================================
echo "== H. Pas de boucle negatif sans espace parasite"

mkdir -p "$WORK/h_fr"
cat > "$WORK/h_fr/a.owl" <<'EOF'
PROCEDURE compte()
	POUR i = 5 À 1 PAS -1
		Trace(i)
	FIN
	n est un entier = PAS -1
FIN
EOF
out="$(translate_to en "$WORK/h_fr/a.owl" "h_fr_en")"
if contains "$out" "STEP -1" && absent "$out" "STEP - 1"; then
    ok "H/PAS -1 -> STEP -1 sans espace (FR->EN)"
else
    ko "H/PAS -1 -> STEP -1 — sortie : $(grep -F STEP "$out" | tr '\n' '|')"
fi
# Non-regression : la negation garde son unaire colle.
if contains "$out" "NOT -1" && absent "$out" "NOT - 1"; then
    ok "H/negation NOT -1 intacte (non-regression)"
else
    ko "H/NOT -1 — sortie : $(grep -F NOT "$out" | tr '\n' '|')"
fi
if compiles "$out"; then ok "H/sortie EN compile"; else ko "H/sortie EN compile — rc != 0"; fi

# Sens EN->FR : deja correct (PAS present dans exprOpeners), garde de non-reg.
mkdir -p "$WORK/h_en"
cat > "$WORK/h_en/a.owl" <<'EOF'
PROCEDURE count()
	FOR i = 5 TO 1 STEP -1
		Trace(i)
	END
	n is an integer = NOT -1
END
EOF
out="$(translate_to fr "$WORK/h_en/a.owl" "h_en_fr")"
if contains "$out" "PAS -1" && absent "$out" "PAS - 1"; then
    ok "H/STEP -1 -> PAS -1 sans espace (EN->FR, deja correct)"
else
    ko "H/STEP -1 -> PAS -1 — sortie : $(grep -F PAS "$out" | tr '\n' '|')"
fi
if compiles "$out"; then ok "H/sortie FR compile"; else ko "H/sortie FR compile — rc != 0"; fi

echo
echo "translate_property_families: $PASS ok, $FAIL FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
