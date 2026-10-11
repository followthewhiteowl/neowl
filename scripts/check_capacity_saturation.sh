#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Volet "chemin" du controle des limites publiees.
#
# Toute limite annoncee par owl_limits.def a un garde quelque part dans le
# compilateur : `if (compteur >= MAX_XXX)`. Ce garde doit refuser bruyamment,
# par `error_capacity_exceeded()` — le chemin unique qui produit le message
# canon, nomme la limite, donne sa valeur et renvoie vers `owl explain`.
#
# Ce qu'on empeche, mesure sur les 22 limites annoncees : les 17 qui passaient
# par ce chemin etaient toutes saines, les 5 qui ne le faisaient pas etaient
# toutes defectueuses, de trois facons :
#   - `return` muet     : la saturation ne dit rien, et pire, elle ETEINT le
#                         controle qui s'appuyait sur la table (une classe qui
#                         n'implemente que 512 des 513 methodes de son
#                         interface compilait au vert) ;
#   - ecretage silencieux suivi d'un faux diagnostic sur une cause inventee ;
#   - message maison qui ne nomme ni la limite ni sa valeur, donc sans lien
#     avec `owl explain` ni avec la table publiee.
#
# Ce controle est statique et bon marche. Il ne voit PAS les murs empruntes
# (un garde parfaitement cable sur une table dimensionnee par le nom d'une
# AUTRE limite) : seul `tests/limits/run.sh` les revele, par un cas N-1 qui
# doit passer. Les deux volets sont complementaires, aucun ne remplace l'autre.
#
# Echappatoire, pour un site dont le message dedie nomme deja la limite :
# marquer la ligne du garde avec `/* capacity-ok: <raison> */`.
#
# Usage : bash scripts/check_capacity_saturation.sh [src]
# Exit code : 0 = OK, 1 = garde muet detecte.

set -u

SRCDIR="${1:-src}"
DEF="$SRCDIR/owl_limits.def"
LOOKAHEAD=5

[ -f "$DEF" ] || { echo "owl_limits.def introuvable : $DEF" >&2; exit 2; }

# Limites hors perimetre de ce controle, avec le ticket qui les porte. Une
# entree ici est une dette nommee, pas une exemption de confort : elle
# disparait avec son ticket.
EXEMPT_LIMITS="
"

names=$(grep -o '^OWL_LIMIT([A-Z_]*' "$DEF" | sed 's/OWL_LIMIT(//')
scan=""
for n in $names; do
    case " $(echo $EXEMPT_LIMITS) " in *" $n "*) continue ;; esac
    scan="$scan $n"
done

files=""
for f in "$SRCDIR"/*.c; do
    case "$f" in *owl_limits.c) continue ;; esac
    files="$files $f"
done

# Une seule passe par fichier : compiler.c fait 1,6 Mo, un grep par limite et
# par fichier coutait des minutes.
out=$(awk -v names="$scan" -v look="$LOOKAHEAD" '
FNR == 1 { if (NR > 1) flush(); nb = 0; delete L }
{ nb++; L[nb] = $0 }
function flush(   i, j, s, name, ok) {
    for (i = 1; i <= nb; i++) {
        s = L[i]
        # Gardes seulement : `if (... >= MAX_X)` / `if (... < MAX_X)`. Les
        # bornes de boucle `for (i = 0; i < MAX_X; i++)` ne sont pas des
        # points de saturation, elles parcourent une table deja remplie.
        if (s !~ /^[ \t]*(\} *)?(else )?if \(.*[<>]=? *MAX_[A-Z_]+/) continue
        if (s ~ /capacity-ok:/) continue
        name = s; sub(/^.*[<>]=? *MAX_/, "MAX_", name); sub(/[^A-Z_].*$/, "", name)
        if (!(name in WANT)) continue
        ok = 0
        for (j = i; j <= i + look && j <= nb; j++)
            if (L[j] ~ /error_capacity_exceeded/) ok = 1
        if (!ok) printf "%s:%d: garde de saturation muet sur %s\n    %s\n", FN, i, name, s
    }
}
BEGIN { n = split(names, a, " "); for (i = 1; i <= n; i++) WANT[a[i]] = 1 }
FNR == 1 { FN = FILENAME }
END { flush() }
' $files)

fail=0
if [ -n "$out" ]; then echo "$out"; fail=1; fi

if [ "$fail" != "0" ]; then
    cat <<'EOF'

Un garde de saturation ne passe pas par error_capacity_exceeded().
Consequences constatees : saturation muette, controle semantique eteint
au-dela du plafond, ou message qui accuse une autre cause. Corriger le
site, ou — si son message dedie nomme deja la limite et renvoie vers
`owl explain` — marquer la ligne `/* capacity-ok: <raison> */`.
EOF
    exit 1
fi

echo "Gardes de saturation : tous sur error_capacity_exceeded()."
exit 0
