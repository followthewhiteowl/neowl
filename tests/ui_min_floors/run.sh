#!/usr/bin/env bash
# Garde-fou des planchers de taille exposes a l'editeur (, ).
#
# Le generateur (`owl --vscode-data`, section "ui_min_floors") publie, pour
# chaque famille de champ, ce que l'editeur ecrivait autrefois a la main :
#   - minWidth / minHeight : la taille en dessous de laquelle un layout ne la
#     descend jamais ;
#   - optionSpacing        : l'espacement entre deux lignes d'option ;
#   - contentAgnostic      : le plancher tient-il constant quoi qu'on mette dans
#     la famille ? (Table / Liste / ZoneRepetee : oui) ;
#   - floorFontPt          : la police a laquelle le plancher a ete mesure, de
#     quoi extrapoler un plancher qui suit sa police.
#
# Ce que ce controle verifie : que ces nombres sont bien ceux que le moteur
# APPLIQUE. Les deux cotes sont mesures, par deux chemins differents :
#
#   - cote publie  : un exemplaire neuf de la famille, cree par la meme
#                    fonction que le montage reel, interroge avec l'operateur
#                    qu'un layout Qt utilise (QWidgetItem::minimumSize).
#   - cote applique: le chemin COMPLET — une fenetre declaree a 1 x 1, un champ
#                    dedans, et la geometrie relevee apres montage (sonde/).
#
# Trois confrontations :
#   1. plancher : publie == applique, la ou il y a un plancher a tenir.
#   2. contentAgnostic : publie == (base == stressee). La sonde monte chaque
#      famille une seconde fois dans une fenetre SEPAREE, police/options
#      gonflees ; si son plancher ne bouge pas, il est insensible au contenu.
#      Deux chemins independants (le moteur gonfle la police en Qt ; la sonde la
#      gonfle en DSL et charge le champ) : un accord fortuit est improbable.
#   3. floorFontPt : publie == `..Police..Taille`, la ou la DSL l'expose.
#
# Sans ce controle, on n'aurait fait que deplacer le nombre grave a la main :
# une valeur publiee qui derive de ce que le moteur applique est exactement le
# defaut que  a laisse vivre dix mois (30 au lieu de 40).
#
# Tolerance unique, et elle est structurelle : une fenetre de haut niveau ne
# peut pas mesurer zero pixel. Pour les familles sans plancher propre
# (plancher publie = 0), le champ mesure 0 ou 1 selon qu'il s'etire ou non dans
# l'unique pixel disponible. On n'exige donc l'egalite stricte que la ou il y a
# un plancher a tenir.
#
# Son angle mort, mesure et borne : une famille dont le plancher reel est 0 et
# qui s'etire a 1 px passe le controle si le publie derive de 0 vers 1 — les
# deux valeurs y sont indiscernables. Mutation +1 sur le plancher publie :
# 12 familles sur 14 rougissent en hauteur (Image et CodeBarres passent, elles
# mesurent 1), 11 sur 14 en largeur (Conteneur passe aussi, il s'etire lui
# aussi a 1 en largeur). Toute derive superieure a 1 px, et toute derive sur
# une famille qui a un plancher, est attrapee. Ce trou ne concerne donc que le
# passage 0 -> 1 des trois familles sans plancher propre, ou il ne change rien
# a ce que l'editeur dessine.
#
# Mutation (preuve de discriminance) : poser OWL_VSCODE_DATA_FILE=<fichier json>
# court-circuite `owl --vscode-data` et fait lire la section publiee dans ce
# fichier. Muter une valeur (plancher, contentAgnostic, floorFontPt) puis
# relancer doit rougir ; remettre doit reverdir.

set -u

OWL="${OWL_BIN:-./build/owl.exe}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="${TMPDIR:-/tmp}/owl_ui_min_floors.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$OWL" ]; then
    echo "ui-min-floors: binaire introuvable ($OWL)" >&2
    exit 1
fi

# --- 1. Cote publie : la section du generateur. --------------------------
# Override pour la preuve de discriminance par mutation : un fichier json fourni
# remplace le generateur, ce qui permet de muter une valeur publiee sans
# reconstruire le binaire.
if [ -n "${OWL_VSCODE_DATA_FILE:-}" ]; then
    cp "$OWL_VSCODE_DATA_FILE" "$TMP/data.json" || {
        echo "ui-min-floors: OWL_VSCODE_DATA_FILE illisible ($OWL_VSCODE_DATA_FILE)" >&2
        exit 1
    }
else
    "$OWL" --vscode-data > "$TMP/data.json" 2>"$TMP/data.err" || {
        echo "ui-min-floors: 'owl --vscode-data' a echoue" >&2
        cat "$TMP/data.err" >&2
        exit 1
    }
fi

sed -n 's/.*"kindEN":"\([^"]*\)".*"minWidth":\(-\{0,1\}[0-9]*\),"minHeight":\(-\{0,1\}[0-9]*\).*/\1 \2 \3/p' \
    "$TMP/data.json" | sort > "$TMP/publie.txt"

# contentAgnostic + floorFontPt par famille.
sed -n 's/.*"kindEN":"\([^"]*\)".*"contentAgnostic":\(true\|false\),"floorFontPt":\([0-9]*\|null\).*/\1 \2 \3/p' \
    "$TMP/data.json" | sort > "$TMP/publie_meta.txt"

if [ ! -s "$TMP/publie.txt" ]; then
    echo "ui-min-floors: aucune entree 'ui_min_floors' dans owl --vscode-data." >&2
    echo "  Le generateur ne publie plus les planchers : l'editeur n'a plus de" >&2
    echo "  quoi calculer la taille reelle d'une fenetre." >&2
    exit 1
fi

if [ ! -s "$TMP/publie_meta.txt" ]; then
    echo "ui-min-floors: 'contentAgnostic'/'floorFontPt' absents de owl --vscode-data." >&2
    echo "  L'editeur devra de nouveau ecrire a la main la liste des familles" >&2
    echo "  dont le plancher ne suit pas le contenu, et la police de mesure." >&2
    exit 1
fi

# --- 2. Cote applique : le chemin complet, hors ecran. -------------------
QT_QPA_PLATFORM=offscreen "$OWL" "$HERE/sonde/planchers.owl" > "$TMP/sonde.out" 2>"$TMP/sonde.err" || {
    echo "ui-min-floors: la sonde de plancher a echoue" >&2
    cat "$TMP/sonde.err" >&2
    exit 1
}

sed -n 's/^PLANCHER \(.*\)$/\1/p' "$TMP/sonde.out" | sort > "$TMP/applique.txt"
sed -n 's/^STRESSED \(.*\)$/\1/p' "$TMP/sonde.out" | sort > "$TMP/stresse.txt"
sed -n 's/^FONTPT \(.*\)$/\1/p'   "$TMP/sonde.out" | sort > "$TMP/fontpt.txt"

if [ ! -s "$TMP/applique.txt" ]; then
    echo "ui-min-floors: la sonde n'a releve aucun plancher." >&2
    cat "$TMP/sonde.err" >&2
    exit 1
fi

if [ ! -s "$TMP/stresse.txt" ]; then
    echo "ui-min-floors: la sonde n'a releve aucun plancher stresse." >&2
    echo "  contentAgnostic ne peut plus etre confronte a la mesure." >&2
    exit 1
fi

if [ ! -s "$TMP/fontpt.txt" ]; then
    echo "ui-min-floors: la sonde n'a releve aucune police." >&2
    echo "  floorFontPt ne peut plus etre confronte a la mesure." >&2
    exit 1
fi

# --- 3. Confrontation des planchers. -------------------------------------
rc=0

while read -r fam pw ph; do
    line=$(grep -E "^$fam " "$TMP/applique.txt" || true)
    if [ -z "$line" ]; then
        echo "ui-min-floors: famille '$fam' publiee mais non mesuree." >&2
        echo "  Ajouter une fenetre sonde dans tests/ui_min_floors/sonde/ et sa" >&2
        echo "  ligne dans planchers.owl — une famille non mesuree est une famille" >&2
        echo "  dont le plancher publie n'est plus verifie par personne." >&2
        rc=1
        continue
    fi
    set -- $line
    mw="$2"; mh="$3"
    for axe in largeur hauteur; do
        if [ "$axe" = largeur ]; then p="$pw"; m="$mw"; else p="$ph"; m="$mh"; fi
        if [ "$p" -ge 1 ]; then
            if [ "$m" != "$p" ]; then
                echo "ui-min-floors: $fam, $axe — publie $p, applique $m." >&2
                rc=1
            fi
        else
            if [ "$m" -gt 1 ]; then
                echo "ui-min-floors: $fam, $axe — publie $p (aucun plancher), applique $m." >&2
                rc=1
            fi
        fi
    done
done < "$TMP/publie.txt"

while read -r fam _ _; do
    if ! grep -qE "^$fam " "$TMP/publie.txt"; then
        echo "ui-min-floors: famille '$fam' mesuree mais non publiee." >&2
        echo "  L'editeur ne recevra pas son plancher et dessinera une fenetre" >&2
        echo "  plus petite que celle qui s'ouvrira." >&2
        rc=1
    fi
done < "$TMP/applique.txt"

# --- 4. Confrontation contentAgnostic : publie == (base == stressee). ----
# Pour chaque famille STRESSEE (celles qui ont un plancher), applied_agnostic
# vaut vrai si son plancher ne bouge pas entre base et version stressee. On le
# confronte au drapeau publie. Le controle ne porte que sur les familles a
# plancher (base >= 1) : sans plancher, il n'y a rien a tenir ni a stresser.
while read -r fam s_l s_h; do
    base=$(grep -E "^$fam " "$TMP/applique.txt" || true)
    if [ -z "$base" ]; then
        echo "ui-min-floors: famille stressee '$fam' sans plancher de base." >&2
        rc=1
        continue
    fi
    set -- $base
    b_h="$3"
    [ "$b_h" -ge 1 ] || continue

    if [ "$b_h" = "$s_h" ]; then applied=true; else applied=false; fi

    meta=$(grep -E "^$fam " "$TMP/publie_meta.txt" || true)
    if [ -z "$meta" ]; then
        echo "ui-min-floors: famille '$fam' mesuree mais 'contentAgnostic' non publie." >&2
        rc=1
        continue
    fi
    set -- $meta
    pub_agnostic="$2"
    if [ "$applied" != "$pub_agnostic" ]; then
        echo "ui-min-floors: $fam, contentAgnostic — publie $pub_agnostic, mesure $applied" \
             "(plancher base $b_h, stresse $s_h)." >&2
        rc=1
    fi
done < "$TMP/stresse.txt"

# --- 5. Confrontation floorFontPt : publie == ..Police..Taille. ----------
# Sur les familles ou la DSL expose la police du champ. La valeur est la meme
# partout (le defaut du modele) ; on la confronte la ou on peut la lire.
while read -r fam pt; do
    meta=$(grep -E "^$fam " "$TMP/publie_meta.txt" || true)
    if [ -z "$meta" ]; then
        echo "ui-min-floors: famille '$fam' a une police mesuree mais aucun 'floorFontPt' publie." >&2
        rc=1
        continue
    fi
    set -- $meta
    pub_pt="$3"
    if [ "$pub_pt" != "$pt" ]; then
        echo "ui-min-floors: $fam, floorFontPt — publie $pub_pt, mesure $pt." >&2
        rc=1
    fi
done < "$TMP/fontpt.txt"

if [ "$rc" = 0 ]; then
    echo "ui-min-floors: OK ($(wc -l < "$TMP/publie.txt") familles ; planchers, contentAgnostic et floorFontPt == applique)"
fi
exit $rc
