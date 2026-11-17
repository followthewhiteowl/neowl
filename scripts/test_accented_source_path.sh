#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Non-regression : OWL doit OUVRIR et LIRE un fichier source .owl dont le NOM
# contient des accents (UTF-8). Classe de bug  : la couche loader
# (src/project.c::read_file, derriere --check / run / test) ouvrait la source
# via fopen NARROW. Sur Windows, un chemin UTF-8 passe a fopen narrow est relu
# via la code page ANSI -> "cCommentaireCreation.owl" devient introuvable
# (meme classe que  cote natives fichier, mais loader non couvert par
# le lint narrow-io qui ne scanne que stdlib_file.c / stdlib_xls.c).
#
# Le nom accentue n'est PAS committe (survie git/export incertaine) : le
# fichier est cree a la volee ici, dans un dossier temp ASCII.
#
# Vert sur chemin ASCII (temoin), rouge seulement quand le loader relit mal
# l'UTF-8 : c'est un defaut SILENCIEux que seul ce chemin accentue attrape.

set -u

OWL="${OWL:-./build/owl.exe}"
if [ ! -x "$OWL" ]; then
    # Tolere l'absence de .exe (POSIX) : resolution via PATH-like relative.
    OWL="${OWL%.exe}"
fi
if ! command -v "$OWL" >/dev/null 2>&1 && [ ! -f "$OWL" ]; then
    echo "SKIP: binaire owl introuvable ($OWL)" >&2
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Nom accentue representatif des cas user (é + è).
SRC="$TMP/cEncaissementNonAllouè.owl"
printf 'cEncaissementNonAllouè est une Classe\nPUBLIC\n\tnSolde est un entier\nFIN\n' > "$SRC"

rc=0

# 1. --check : doit lire le fichier (pas de "Cannot read file") et valider.
out_check="$("$OWL" --check "$SRC" 2>&1)"
if printf '%s' "$out_check" | grep -qiE "cannot read file|cannot open file"; then
    echo "FAIL: --check n'ouvre pas le fichier accentue" >&2
    printf '  sortie: %s\n' "$out_check" >&2
    rc=1
fi

# 2. run direct : le top-level d'un fichier accentue doit s'executer.
RUN="$TMP/exécuté.owl"
printf 'Trace("ok-accent")\n' > "$RUN"
out_run="$("$OWL" "$RUN" 2>&1)"
if ! printf '%s' "$out_run" | grep -q "ok-accent"; then
    echo "FAIL: run d'un fichier accentue ne produit pas sa sortie" >&2
    printf '  sortie: %s\n' "$out_run" >&2
    rc=1
fi

# 3. Temoin ASCII : meme contenu, nom ASCII -> doit toujours marcher
#    (garde contre une regression qui casserait le cas nominal).
ASCII="$TMP/cAscii.owl"
printf 'Trace("ok-ascii")\n' > "$ASCII"
out_ascii="$("$OWL" "$ASCII" 2>&1)"
if ! printf '%s' "$out_ascii" | grep -q "ok-ascii"; then
    echo "FAIL: temoin ASCII casse (regression du cas nominal)" >&2
    printf '  sortie: %s\n' "$out_ascii" >&2
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "OK: fichiers source accentues lus par --check et run"
fi
exit $rc
