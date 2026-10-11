#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#  puis  — la sortie deja ecrite doit survivre a un arret brutal
# du process, DANS LES DEUX REGIMES DE TAMPON.
#
# Contexte  : setvbuf(stdout, NULL, _IOLBF, 0) etait pose expres par
#  "pour ne pas perdre le dernier nom de test avant crash". Il ne l'a
# jamais fait : MSVCRT aliase le line-buffering sur le full-buffering des que le
# handle n'est pas un terminal, c'est-a-dire dans le seul cas ou la protection
# sert (sortie capturee en fichier ou en pipe). Le defaut a survecu des mois
# parce que rien ne le testait.  est passe en _IONBF sur les modes
# concernes ; ce script est le verrou qui empeche de revenir en arriere.
#
# Contexte  : ce verrou ne couvrait QUE le mode OWL_GC_POISON, celui qui
# arme un setvbuf non tamponne. En usage NORMAL — stdout pleinement tamponne,
# comme pour n'importe quel lancement d'owl redirige vers un fichier ou une pipe
# — la totalite de la sortie etait encore perdue. Le non tamponne generalise
# n'est pas la reponse (mesure  : ~3 s pour 20 000 lignes contre 7 ms
# en tamponne, facteur ~420). La reponse est un fflush(NULL) en tete du filtre
# d'exception non gere (src/platform.c). Le mode "nominal" ci-dessous est le
# verrou de cette protection : il tourne SANS aucune variable d'environnement,
# donc avec le tampon par defaut.
#
# Mesures sur ce toolchain (owl test sur la fixture, stdout redirige) :
#   poison,  _IOLBF (avant ) : exit 139, stdout 0 octet
#   poison,  _IONBF (apres ) : exit 139, stdout 185 octets
#   nominal, avant           : exit 139, stdout 0 octet, stderr 0 octet
#   nominal, apres           : exit 139, stdout 185 octets
#
# Contexte  : le meme plantage doit aussi laisser un DUMP DE PILE
# exploitable dans %TEMP%\owl_crash.txt. Ce verrou ne regardait que stdout ;
# personne ne lisait le dump, donc un retour au mode texte ("a" au lieu de "ab")
# ou au mode tamponne repassait vert EN SILENCE — exactement le scenario qui a
# survecu des mois avant . Les trois assertions ajoutees plus bas
# correspondent aux trois regressions distinctes possibles :
#   fichier absent/vide      -> mode texte : le premier fputs refaute avant
#                               d'ecrire un octet (traduction LF->CRLF d'UCRT,
#                               plusieurs Ko de pile reserves)
#   en-tete sans frame       -> symbolisation dbghelp remise avant le
#                               deroulement brut : SymInitialize refaute et
#                               emporte toute la backtrace
#   contenu perdu            -> fichier tamponne : tout ce qui attendait le
#                               fflush final meurt avec le second defaut
#
# Le crash est REEL et sans instrumentation : la fixture z_crash/ sature la
# pile C du parser recursif-descendant (60 000 parentheses imbriquees). Deux
# tests triviaux s'achevent avant, donc le runner a forcement ecrit sur stdout
# avant de mourir.
#
# Comme pour run_segfault_unwind_test.sh apres , on exige la PREUVE DE
# PRESENCE : un verrou dont le critere est une simple absence devient vert et
# vide des que la fixture cesse de faire ce qu'on croit. On verifie donc que le
# crash a bien eu lieu AVANT de conclure quoi que ce soit sur la sortie.
#
# Usage: scripts/run_stdout_survives_crash_test.sh [path/to/owl.exe]

set -u

OWL_EXE="${1:-./build/owl.exe}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE_DIR="$DIR/tests/compiler/internal_fixtures/stdout_survives_crash"
FIXTURE_CRASH="$FIXTURE_DIR/z_crash/test_99_pile_saturee.owl"

# Nom du dernier test achevé avant le crash. C'est l'information qu'un
# diagnostic post-mortem cherche en premier, et celle que _IOLBF perdait.
MARK_LAST_TEST="test_02_dernier.owl"
MARK_FIRST_TEST="test_01_premier.owl"

if [[ ! -x "$OWL_EXE" ]]; then
    echo "ERROR: owl exe not found: $OWL_EXE" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE_CRASH" ]]; then
    echo "ERROR: fixture not found: $FIXTURE_CRASH" >&2
    exit 2
fi

OWL_EXE="$(cd "$(dirname "$OWL_EXE")" && pwd)/$(basename "$OWL_EXE")"

# Augment PATH for Qt6 DLLs (idem run_compiler_tests.sh).
if [[ -d "/c/msys64/ucrt64/bin" ]]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

fail=0

# ---------------------------------------------------------------------------
# Localisation du dump de pile.
# Le process teste resout son chemin avec GetTempPathA, qui lit %TMP%/%TEMP% —
# les memes variables que ce shell transmet a son enfant. On les convertit donc
# plutot que de supposer /tmp. DUMP_FILE vide = verrou neutralise (le dump est
# une surface Windows ; sur un autre hote il n'y a rien a verifier, et un echec
# y serait un faux positif).
# ---------------------------------------------------------------------------
DUMP_FILE=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        _tmpdir="${TMP:-${TEMP:-}}"
        if [[ -n "$_tmpdir" ]] && command -v cygpath >/dev/null 2>&1; then
            _tmpdir="$(cygpath -u "$_tmpdir")"
        fi
        [[ -d "${_tmpdir:-}" ]] || _tmpdir="/tmp"
        DUMP_FILE="$_tmpdir/owl_crash.txt"
        ;;
esac

# stdout et stderr sont captures SEPAREMENT : en mode poison, stderr porte les
# jalons [POISON-START] et masquerait le defaut teste, qui ne concerne que
# stdout. La redirection vers un fichier suffit a sortir du cas terminal —
# c'est le cas qui nous interesse.
STDOUT_FILE="$(mktemp)"
STDERR_FILE="$(mktemp)"
trap 'rm -f "$STDOUT_FILE" "$STDERR_FILE"' EXIT

# Lance le runner sur la fixture. $1 = valeur de OWL_GC_POISON ("" = absente).
# Le shell qui lance un enfant mort sur SIGSEGV ecrit lui-meme "Segmentation
# fault" sur SA sortie d'erreur. Ici le crash est attendu : ce bruit ferait
# croire a un echec dans le log de la batterie. On delegue donc le lancement a
# un bash intermediaire dont la sortie d'erreur part au neant — celle du
# process teste, elle, est deja capturee dans le fichier.
run_fixture() {
    local poison="$1"
    : > "$STDOUT_FILE"
    : > "$STDERR_FILE"
    if [[ -n "$poison" ]]; then
        bash -c 'OWL_GC_POISON=1 "$1" test "$2" >"$3" 2>"$4"' _ \
             "$OWL_EXE" "$FIXTURE_DIR" "$STDOUT_FILE" "$STDERR_FILE" 2>/dev/null
    else
        bash -c '"$1" test "$2" >"$3" 2>"$4"' _ \
             "$OWL_EXE" "$FIXTURE_DIR" "$STDOUT_FILE" "$STDERR_FILE" 2>/dev/null
    fi
    return $?
}

# Criteres communs aux deux regimes. $1 = libelle du mode, $2 = exit code.
# Pose LAST_BYTES (taille de stdout capture) et met a jour `fail`. Pas de
# substitution de commande a l'appel : elle creerait un sous-shell ou
# l'affectation de `fail` serait perdue.
LAST_BYTES=0
check_common() {
    local mode="$1" exit_code="$2"
    local bytes
    bytes=$(wc -c < "$STDOUT_FILE" | tr -d ' ')
    LAST_BYTES="$bytes"

    # Preuve de presence : le process est bien mort brutalement. Sans crash, la
    # sortie serait videe proprement a la fermeture des flux et le verrou
    # serait vert sans rien avoir teste. Si la fixture cesse de planter (par
    # exemple une garde de profondeur ajoutee au parser), on veut un ECHEC
    # bruyant.
    if [[ $exit_code -ne 139 ]]; then
        echo "FAIL stdout_survives_crash [$mode]: exit code $exit_code, attendu 139 (SIGSEGV)." >&2
        echo "  La fixture ne plante plus : le verrou ne teste RIEN en l'etat." >&2
        echo "  $FIXTURE_CRASH doit saturer la pile C du parser." >&2
        fail=1
    fi

    # Le critere du ticket : apres un crash reel, stdout n'est pas vide.
    if [[ "$bytes" -eq 0 ]]; then
        echo "FAIL stdout_survives_crash [$mode]: stdout est VIDE apres le crash ($bytes octet)." >&2
        echo "  Tout ce que le runner avait affiche est mort dans le tampon." >&2
        fail=1
    fi

    # Et il contient le nom du DERNIER test acheve : c'est cette information
    # precise que la protection devait sauver.
    if ! grep -qF "$MARK_LAST_TEST" "$STDOUT_FILE"; then
        echo "FAIL stdout_survives_crash [$mode]: le nom du dernier test acheve est absent de stdout." >&2
        echo "  attendu : $MARK_LAST_TEST" >&2
        echo "  Un diagnostic post-mortem ne peut pas borner l'endroit du crash." >&2
        fail=1
    fi

    if ! grep -qF "$MARK_FIRST_TEST" "$STDOUT_FILE"; then
        echo "FAIL stdout_survives_crash [$mode]: le premier test acheve est absent de stdout." >&2
        echo "  attendu : $MARK_FIRST_TEST — sortie tronquee, pas seulement retardee." >&2
        fail=1
    fi
}

# Verrou  — le dump de pile du MEME plantage. A n'appeler qu'apres un
# run precede de `rm -f "$DUMP_FILE"` : le fichier est ouvert en APPEND, donc un
# dump perime le rendrait vert a tort — un verrou qui passe sur un artefact
# d'hier est pire que pas de verrou du tout.
# Pose DUMP_BYTES / DUMP_FRAMES et met a jour `fail`.
DUMP_BYTES=0
DUMP_FRAMES=0
check_dump() {
    local mode="$1"
    [[ -z "$DUMP_FILE" ]] && return 0

    if [[ ! -e "$DUMP_FILE" ]]; then
        echo "FAIL stdout_survives_crash [$mode]: aucun dump de pile produit." >&2
        echo "  attendu : $DUMP_FILE" >&2
        echo "  Le filtre d'exception n'a pas ouvert son fichier, ou il est mort avant." >&2
        fail=1
        return
    fi

    # `wc -c < fichier` et `ls -l` rapportent 0 sur ce fichier alors qu'il a du
    # contenu : le producteur meurt sans le fermer, la taille en metadonnee
    # n'est jamais mise a jour. Il faut le LIRE.
    DUMP_BYTES=$(cat "$DUMP_FILE" | wc -c | tr -d ' ')
    DUMP_FRAMES=$(cat "$DUMP_FILE" | grep -cE '^  #[0-9]+ : 0x' || true)

    if ! cat "$DUMP_FILE" | grep -qF "==== OWL CRASH"; then
        echo "FAIL stdout_survives_crash [$mode]: le dump n'a pas d'en-tete ($DUMP_BYTES octets)." >&2
        echo "  attendu : une ligne '==== OWL CRASH <horodatage> ====' dans $DUMP_FILE" >&2
        echo "  Cause probable : fichier rouvert en mode TEXTE (\"a\" au lieu de \"ab\")." >&2
        echo "  La traduction LF->CRLF d'UCRT reserve plusieurs Ko de pile, donc le tout" >&2
        echo "  premier fputs refaute avant d'ecrire un octet sur une pile saturee." >&2
        fail=1
    fi

    if [[ "$DUMP_FRAMES" -eq 0 ]]; then
        echo "FAIL stdout_survives_crash [$mode]: dump sans aucune frame ($DUMP_BYTES octets)." >&2
        echo "  attendu : au moins une ligne '  #NN : 0x...' sous 'raw_frames'" >&2
        echo "  Cause probable : la symbolisation dbghelp est repassee AVANT le" >&2
        echo "  deroulement brut. SymInitialize refaute sur une pile epuisee et emporte" >&2
        echo "  toute la backtrace ; ou le fichier est redevenu tamponne et son contenu" >&2
        echo "  est mort avec le second defaut, faute de fflush atteint." >&2
        fail=1
    fi
}

# ---------------------------------------------------------------------------
# Mode 1 — OWL_GC_POISON (verrou , protection = setvbuf _IONBF).
# ---------------------------------------------------------------------------
run_fixture 1
poison_code=$?
check_common "poison" "$poison_code"
poison_bytes="$LAST_BYTES"

# Preuve de presence propre au mode poison : les jalons [POISON-START] montrent
# que le crash vient bien de la fixture profonde, pas d'un plantage precoce.
if ! grep -q "test_99_pile_saturee" "$STDERR_FILE"; then
    echo "FAIL stdout_survives_crash [poison]: la fixture profonde n'a pas ete atteinte." >&2
    echo "  Le process est mort avant, le scenario teste n'est pas exerce." >&2
    fail=1
fi
if [[ $fail -ne 0 ]]; then
    echo "" >&2
    echo "stdout capture [poison] ($poison_bytes octets):" >&2
    head -20 "$STDOUT_FILE" >&2
    echo "stderr capture [poison]:" >&2
    head -10 "$STDERR_FILE" >&2
fi

# ---------------------------------------------------------------------------
# Mode 2 — NOMINAL, aucune variable d'environnement (verrou ).
# stdout est pleinement tamponne : c'est le cas de n'importe quel lancement
# d'owl redirige. La protection testee ici n'est PAS un setvbuf mais le
# fflush(NULL) en tete du filtre d'exception non gere (src/platform.c).
# ---------------------------------------------------------------------------
# Le dump est ouvert en APPEND : on part d'une ardoise vierge, sinon le dump du
# mode poison (ou celui d'hier) suffirait a rendre les assertions vertes.
[[ -n "$DUMP_FILE" ]] && rm -f "$DUMP_FILE"

run_fixture ""
nominal_code=$?
check_common "nominal" "$nominal_code"
nominal_bytes="$LAST_BYTES"
check_dump "nominal"

# Preuve de presence propre au mode nominal : stderr n'y porte aucun jalon
# (rien n'ecrit sur stderr avant le crash), donc on borne autrement. Les seuls
# fichiers de la fixture sont les deux tests triviaux + le fichier profond : si
# les deux triviaux sont acheves ET que le process est mort en 139, le crash est
# forcement survenu sur le troisieme.
if grep -qF "test_99_pile_saturee.owl (" "$STDOUT_FILE"; then
    echo "FAIL stdout_survives_crash [nominal]: la fixture profonde s'est achevee." >&2
    echo "  Elle ne plante plus : le verrou ne teste RIEN en l'etat." >&2
    fail=1
fi
if [[ $fail -ne 0 ]]; then
    echo "" >&2
    echo "stdout capture [nominal] ($nominal_bytes octets):" >&2
    head -20 "$STDOUT_FILE" >&2
    echo "stderr capture [nominal]:" >&2
    head -10 "$STDERR_FILE" >&2
fi

if [[ $fail -eq 0 ]]; then
    if [[ -n "$DUMP_FILE" ]]; then
        dump_msg="dump de pile $DUMP_BYTES octets avec $DUMP_FRAMES frames"
    else
        dump_msg="dump de pile non verifie (hote non Windows)"
    fi
    echo "stdout_survives_crash: PASS (2 regimes de tampon, crash reel exit 139 chacun," \
         "stdout survit avec $poison_bytes octets [poison] et $nominal_bytes octets [nominal]," \
         "dernier test acheve nomme dans les deux, $dump_msg)"
    exit 0
else
    echo "Exit codes: poison=$poison_code nominal=$nominal_code" >&2
    exit 1
fi
