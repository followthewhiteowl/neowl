#!/usr/bin/env bash
# Harnais boite noire : RENVOYER <entier> au top-level pilote le code de
# sortie du process. Chaque cas est joue en SOUS-PROCESS, le
# harnais `owl test` in-process ne peut pas observer $?/%ERRORLEVEL%.
#
# Contexte (cf. *.md puis correction
# , mesure oracle de reference) : RENVOYER 7 top-level d'un
# SCRIPT -> exit 7, RENVOYER 3.9 -> exit 3 (tronque), RENVOYER "abc"
# (non-entier) -> exit 0, pas de RENVOYER -> exit 0. RENVOYER dans une
# PROCEDURE appelee (pas top-level) n'affecte PAS le code de sortie (reste
# 0).
#
# Modele PROJET corrige (, amende  qui avait cable le
# top-level de projet.owl -- mauvaise lecture coordinator) : le code de
# sortie d'un PROJET est porte par le RENVOYER de Fermeture() (teardown,
# execute en dernier, apres le corps + la fenetre de demarrage eventuelle).
# Le RENVOYER au top-level de projet.owl est IGNORE. Sans Fermeture(), le
# projet retombe sur le defaut standard (0 si succes, 65/70 si erreur).
#
# Etat OWL AVANT fix (mesure) : RENVOYER top-level accepte au --check mais
# la VALEUR est jetee pour un SCRIPT -> exit 0 partout, y compris 7/255/3.9.
# Donc les cas case_script_7/255/reel sont ROUGES ATTENDUS aujourd'hui,
# pour la BONNE raison (rc=0 observe au lieu de la valeur attendue).
# case_script_0, case_script_absent, case_script_nonentier et
# case_script_proc sont deja VERTS aujourd'hui (0 attendu == 0 observe --
# non-regression a proteger). Cote PROJET, l'impl  capture encore
# le top-level et ignore Fermeture() : case_projet_top_level_ignore,
# case_projet_fermeture, case_projet_fermeture_seule et
# case_projet_fenetre_fermeture sont ROUGES ATTENDUS aujourd'hui (pour la
# bonne raison : rc observe = ancien modele, pas le nouveau) ;
# case_projet_sans_fermeture est deja VERT (0 attendu == 0 observe, aucun
# des deux modeles ne diverge sur ce chemin).
#
# Les deux cas FinProgramme ne sont PAS des cas figes : la semantique
# "RENVOYER dans Fermeture() du projet" portee par FinProgramme n'est pas
# tranchee par ce ticket -- ce harnais rapporte l'OBSERVE (rc + sentinelle
# fichier) sans faire echouer la campagne sur ces 2-la, SAUF
# case_finprogramme_vrai qui verrouille un bug deja identifie et nomme
# dans le ticket d'origine (le booleen <codeFermeture> de FinProgramme est
# inerte aujourd'hui : FinProgramme appelle exit(0) direct, sans jamais
# executer Fermeture(), Vrai ou Faux).
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si tous les cas NON-observationnels matchent l'attendu,
# 1 sinon.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL_BIN="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"

if [ ! -x "$OWL_BIN" ]; then
    echo "renvoyer_exit_code: ERROR owl.exe introuvable: $OWL_BIN" >&2
    exit 1
fi

FAIL=0

# $1 = nom du cas, $2 = chemin (fichier .owl ou dossier projet), $3 = rc attendu
run_case() {
    local name="$1" target="$2" expected="$3"
    local out rc
    out="$("$OWL_BIN" "$ROOT/$target" 2>&1)"
    rc=$?

    if [ "$rc" = "$expected" ]; then
        echo "renvoyer_exit_code_${name}: RESULT: GREEN (rc=$rc, attendu=$expected)"
    else
        echo "renvoyer_exit_code_${name}: RESULT: RED (rc=$rc, attendu=$expected)"
        echo "  --- sortie ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
    fi
}

# --- Cas script (top-level d'un fichier lance directement) ------------------
run_case "script_7"         "case_script_7/script.owl"         7
run_case "script_0"         "case_script_0/script.owl"         0
run_case "script_255"       "case_script_255/script.owl"       255
run_case "script_reel_3_9"  "case_script_reel/script.owl"      3
run_case "script_nonentier" "case_script_nonentier/script.owl" 0
run_case "script_absent"    "case_script_absent/script.owl"    0
run_case "script_proc"      "case_script_proc/script.owl"      0

# --- Cas projet : le top-level de projet.owl est IGNORE pour l'exit code ---
# Modele corrige (, amende ) : le RENVOYER top-level d'un
# projet n'est PLUS capture pour le code de sortie. Sans Fermeture(), le
# projet retombe sur le defaut standard (0 si succes). Ici top-level pose
# RENVOYER 7, pas de Fermeture() -> exit attendu = 0 (et non 7).
run_case "projet_top_level_ignore" "case_projet_top_level" 0

# --- Projet + Fermeture() : Fermeture() gagne, le top-level est ignore -----
# Decision coordinator : le code de sortie = RENVOYER de
# Fermeture() (teardown projet, execute en dernier). Le RENVOYER top-level
# est ignore. Ici top-level pose RENVOYER 5, Fermeture() pose RENVOYER 9 ->
# exit attendu = 9.
run_case "projet_fermeture" "case_projet_fermeture" 9

# --- Projet SANS RENVOYER top-level, Fermeture() seule pose le code --------
run_case "projet_fermeture_seule" "case_projet_fermeture_seule" 3

# --- Projet SANS Fermeture() et sans RENVOYER pertinent : defaut succes ----
run_case "projet_sans_fermeture" "case_projet_sans_fermeture" 0

# --- Projet avec fenetre de demarrage (FenetreDemarrage) qui se ferme ------
# elle-meme (FinInitialisation -> Ferme()), puis Fermeture() pose le code de
# sortie. Execution GUI reelle (TestModeHeadless(Faux)) sous QT_QPA_PLATFORM=
# offscreen, bornee par timeout (meme pattern que
# tests/compiler/internal_fixtures/run-align-project/case_083_*) : la
# fenetre ne doit jamais rester ouverte en attente d'un humain.
run_case_fenetre() {
    local name="$1" target="$2" expected="$3"
    local out rc
    out="$(QT_QPA_PLATFORM=offscreen timeout 15 "$OWL_BIN" "$ROOT/$target" 2>&1)"
    rc=$?

    if [ "$rc" = "$expected" ]; then
        echo "renvoyer_exit_code_${name}: RESULT: GREEN (rc=$rc, attendu=$expected)"
    else
        echo "renvoyer_exit_code_${name}: RESULT: RED (rc=$rc, attendu=$expected)"
        echo "  --- sortie ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
    fi
}
run_case_fenetre "projet_fenetre_fermeture" "case_projet_fenetre_fermeture" 4

# --- Projet avec fenetre de demarrage dont le PARAMETRAGE est REFUSE --------
# (parametre de type avance Date, cf. case_projet_fenetre_refus/FEN_R.owl). Le
# refus de parametrage (code 65, detecte AVANT tout montage Qt) PRIME et
# devient le code de sortie : il n'est PAS ecrase par le RENVOYER de
# Fermeture() (qui pose 4). Verrouille la regression  (le bloc
# teardown ne doit pas avaler un wr != 0). Exit attendu = 65, pas 4, pas 0.
run_case_fenetre "projet_fenetre_refus" "case_projet_fenetre_refus" 65

# --- FinProgramme : booleen <codeFermeture> execute (Vrai) ou non (Faux) le
# teardown du projet. Observable via sentinelle fichier ecrite par Fermeture().
run_finprogramme_case() {
    local name="$1" dir="$2" expect_teardown="$3"
    local sentinel="$ROOT/$dir/sentinelle.txt"
    rm -f "$sentinel"
    local out rc
    out="$("$OWL_BIN" "$ROOT/$dir" 2>&1)"
    rc=$?

    local teardown_ran="non"
    [ -f "$sentinel" ] && teardown_ran="oui"
    rm -f "$sentinel"

    if [ "$teardown_ran" = "$expect_teardown" ]; then
        echo "renvoyer_exit_code_${name}: RESULT: GREEN (rc=$rc, teardown_execute=$teardown_ran, attendu=$expect_teardown)"
    else
        echo "renvoyer_exit_code_${name}: RESULT: RED (rc=$rc, teardown_execute=$teardown_ran, attendu=$expect_teardown)"
        FAIL=1
    fi
}

# Vrai -> Fermeture() DOIT s'executer (sentinelle presente). Rouge attendu
# aujourd'hui : FinProgramme appelle exit(0) direct, Fermeture() n'est
# jamais invoque, Vrai ou Faux.
run_finprogramme_case "finprogramme_vrai_execute_fermeture" "case_finprogramme_vrai" "oui"

# Faux -> Fermeture() ne doit PAS s'executer (sentinelle absente). ATTENTION :
# ce cas est vert aujourd'hui PAR COINCIDENCE (FinProgramme ignore le
# booleen et saute TOUJOURS le teardown, Vrai comme Faux) -- pas parce que
# le booleen est honore. A re-verifier apres le fix (cf. test-writing-pitfalls
# section 3 cas B) : ce vert doit rester vert pour la VRAIE raison une fois
# le booleen cable.
run_finprogramme_case "finprogramme_faux_skip_fermeture" "case_finprogramme_faux" "non"

exit $FAIL
