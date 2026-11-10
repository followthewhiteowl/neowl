#!/usr/bin/env bash
# Garde-fou anti-regression : aucun chemin machine absolu / arbre
# de build ne doit subsister dans un owl_link_gui.cfg destine a etre publie.
#
# Contexte : owl_link_gui.cfg (genere POST_BUILD par
# scripts/extract_link_spec.cmake) liste les objets/libs necessaires pour
# relinker un bundle GUI via owl-build. Qt6/DuckDB/llama sont deja
# places sous placeholder @XXX_ROOT@ a la generation (OBJECTS/LIBS_ABS) —
# seules les lignes *_ROOT_DEFAULT gardent un chemin absolu, et c'est
# attendu : elles ne servent que de fallback dev, et package_dist.sh les
# relocalise/vide a l'empaquetage (relocatabilize_cfg). Toute lib externe
# vendoree qui n'a pas encore ete branchee sur ce mecanisme (cas historique :
# Tesseract/Leptonica, ) laisse un chemin de build en dur dans
# LIBS_ABS/OBJECTS, invisible tant qu'on ne regarde pas le contenu du cfg —
# le scrub d'export ne fait que maquiller le prefixe, pas le detecter.
#
# Ce que ce controle verifie, sur CHAQUE owl_link_gui.cfg trouve :
#   - dans LIBS_ABS / OBJECTS / LIBS_REL, aucun token .a/.obj/.o ne doit
#     etre un chemin absolu (prefixe Windows `X:/`, `X:\`, ou POSIX `/...`)
#     ni contenir `_local/` (arbre de build/vendoring local au poste).
#   - les lignes `*_ROOT_DEFAULT` sont volontairement IGNOREES : elles
#     portent un chemin absolu par design au stade build, et ne sont pas
#     ce que ce controle protege (c'est relocatabilize_cfg qui s'en charge
#     a l'empaquetage).
#
# Usage :
#   bash tests/packaging/check_link_cfg_relocatable.sh
#   OWL_LINK_CFG=path/to/owl_link_gui.cfg bash tests/packaging/check_link_cfg_relocatable.sh
#   OWL_BUILDDIR=build_zig bash tests/packaging/check_link_cfg_relocatable.sh
#
# Codes de sortie :
#   0 = tous les cfg trouves sont propres (aucun chemin absolu residuel)
#   1 = au moins un chemin absolu / `_local/` detecte dans un cfg
#   2 = aucun owl_link_gui.cfg trouve (build pas encore fait, ou chemin
#       OWL_LINK_CFG errone) — distinct du cas 1, PAS un vert par defaut.

set -u

BUILDDIR="${OWL_BUILDDIR:-build}"

CFGS=()
if [ -n "${OWL_LINK_CFG:-}" ]; then
    CFGS=("$OWL_LINK_CFG")
else
    while IFS= read -r f; do
        CFGS+=("$f")
    done < <(find "$BUILDDIR" -path "*/lib/*/owl_link_gui.cfg" 2>/dev/null)
fi

if [ "${#CFGS[@]}" -eq 0 ]; then
    echo "SKIP/FAIL : aucun owl_link_gui.cfg trouve sous '$BUILDDIR'." >&2
    echo "  -> le cfg est genere POST_BUILD par la cible 'owl' (OWL_BUILD_OWL)." >&2
    echo "  -> lancer un build (ex: mingw32-make quick, ou le build owl seul)" >&2
    echo "     avant de rejouer ce controle, ou pointer OWL_LINK_CFG=<fichier>." >&2
    exit 2
fi

FAIL=0

check_tokens_line() {
    # $1 = nom de la cle (LIBS_ABS / OBJECTS / LIBS_REL), $2 = valeur, $3 = fichier
    local key="$1" val="$2" file="$3" tok bad=0
    for tok in $val; do
        [ -z "$tok" ] && continue
        case "$tok" in
            *.a|*.obj|*.o)
                if printf '%s' "$tok" | grep -Eq '_local/'; then
                    echo "  [$key] chemin arbre de build local detecte : $tok" >&2
                    bad=1
                elif printf '%s' "$tok" | grep -Eq '^[A-Za-z]:[/\\]'; then
                    echo "  [$key] chemin absolu Windows non relocalise : $tok" >&2
                    bad=1
                elif printf '%s' "$tok" | grep -Eq '^/'; then
                    echo "  [$key] chemin absolu POSIX non relocalise : $tok" >&2
                    bad=1
                fi
                ;;
        esac
    done
    return $bad
}

for cfg in "${CFGS[@]}"; do
    [ -f "$cfg" ] || { echo "SKIP/FAIL : '$cfg' introuvable." >&2; FAIL=1; continue; }
    echo "==> $cfg"
    cfg_bad=0
    for key in LIBS_ABS OBJECTS LIBS_REL; do
        line=$(grep "^${key}=" "$cfg" | head -1 | sed "s#^${key}=##")
        [ -z "$line" ] && continue
        if ! check_tokens_line "$key" "$line" "$cfg"; then
            cfg_bad=1
        fi
    done
    if [ "$cfg_bad" -ne 0 ]; then
        echo "  -> '$cfg' contient un chemin machine absolu non relocalise." >&2
        echo "     corriger via le mecanisme @XXX_ROOT@ (cf. scripts/extract_link_spec.cmake" >&2
        echo "     + src/project_run.c + scripts/package_dist.sh), PAS en editant le cfg." >&2
        FAIL=1
    else
        echo "  OK : aucun chemin absolu residuel dans LIBS_ABS/OBJECTS/LIBS_REL."
    fi
done

exit "$FAIL"
