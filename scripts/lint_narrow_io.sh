#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Garde anti-regression : empeche la reintroduction d'un appel fichier NARROW
# (code page ANSI) dans la couche fichier (src/stdlib_file.c, src/stdlib_xls.c).
#
# Pourquoi : OWL stocke les chemins en UTF-8. Sur Windows, un chemin UTF-8 passe
# a fopen / opendir / GetFileAttributesA narrow est relu via la code page ANSI
# du systeme (CP-1252 en FR). Un dossier accentue ("Modeles test", profil "Zoe")
# devient alors introuvable -- bug public v1.6.0. La couche doit passer par les
# wrappers wide (owl_* dans stdlib_file.c, xls_* dans stdlib_xls.c) qui
# convertissent UTF-8 -> UTF-16 au dernier moment.
#
# Ce defaut est SILENCIEUX : il ne casse rien sur un chemin ASCII (temoin vert),
# et la suite de tests tourne sur des dossiers ASCII. Seul un controle du SOURCE
# peut l'attraper avant qu'il ne reparte en prod.
#
# Exception legitime : une pass-through POSIX (narrow = deja UTF-8) ou un wrapper
# de bas niveau s'annote en fin de ligne par /* narrow-ok */. Les lignes de
# commentaire C (premier caractere non-blanc `*`, `/`) sont ignorees.

set -u

FILES="src/stdlib_file.c src/stdlib_xls.c"

# Tokens narrow interdits sous forme d'APPEL (`token(`), + GetFileAttributesA
# (le `A` = ANSI explicite) et xls_open_file (API libxls par chemin, fopen
# narrow interne).
PAT='\b(fopen|_fsopen|freopen|opendir|readdir|closedir|mkdir|_mkdir|rmdir|_rmdir|getcwd|_getcwd|chdir|_chdir|stat|_stat|access|_access|getenv)[[:space:]]*\(|GetFileAttributesA|xls_open_file'

rc=0
while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    # hit = "fichier:ligne:contenu"
    content="${hit#*:}"; content="${content#*:}"
    # Ignore les lignes de commentaire (debut `*`, `//`, `/*`).
    trimmed="${content#"${content%%[![:space:]]*}"}"
    case "$trimmed" in
        '*'*|'/'*) continue ;;
    esac
    # Ignore les lignes explicitement annotees.
    case "$content" in
        *'/* narrow-ok */'*) continue ;;
    esac
    echo "$hit"
    rc=1
done < <(grep -nHE "$PAT" $FILES 2>/dev/null)

if [ "$rc" -ne 0 ]; then
    echo "" >&2
    echo "LINT NARROW I/O : appel fichier narrow non wrappe (cf. ci-dessus)." >&2
    echo "Utilise les wrappers wide (owl_* / xls_*) ou annote /* narrow-ok */" >&2
    echo "si c'est une pass-through POSIX legitime (cf. fix/2336)." >&2
fi
exit $rc
