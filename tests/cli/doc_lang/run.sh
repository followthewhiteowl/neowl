#!/usr/bin/env bash
# Harnesses boite noire sur le rendu CLI doc (owl explain / list / examples).
# Compile chaque scanner a la volee (gcc) et le lance contre owl.exe :
#   - leak_scan.c           : anti-fuite linguistique FR en --lang en
#   - internal_native_scan.c : anti native-interne dans les voisins
#   - keyword_en_scan.c     : forme EN des fiches keyword du doc HTML
#   - projet_prop_scan.c    : proprietes du bloc Projet dans DSLPROPS (F1)
#   - projet_prop_descen_scan.c : descEN des proprietes Projet (anti-franglais aide EN)
#   - enum_dup_scan.c       : anti-doublon FR+EN valeurs d'enum (owl explain)
#   - enum_accent_dup_scan.c : anti-doublon SANS-ACCENT couleur (owl explain)
#   - date_returntype_scan.c : returnType precis des natives systeme de date
#   - explain_json_lang_scan.c : export JSON de `explain` honore --lang (category/return_type/return_desc)
#   - type_field_nav_scan.c  : champs de types composites navigables F1
#   - collision_crossref_scan.c : renvoi "aussi propriete" sur collision de nom
#   - widget_prop_regression_scan.c : non-regression proprietes widgets (Largeur)
#   - doc_fiches_baseline_stable_scan.c : baseline Nouveautes non polluee
#   - list_scroll_reset_scan.c : reset ascenseur #list au changement de recherche/filtre
#   - widget_style_descen_scan.c : descEN des props Style des widgets (anti-franglais aide EN)
#   - table_seealso_scan.c  : see-also natives Table sur owl explain Table, FR+EN, exclut Tableau*
#   - crossref_pin_scan.c   : lien declaratif fiche->guide epingle (RegexRemplace -> 05_operateurs), CLI texte+JSON
#   - crossref_heuristic_regression_scan.c : non-regression heuristique occurrences (owl explain Projet)
#   - crossref_viewer_parity_scan.c : parite viewer JSON (DATA[] porte le lien fiche->guide)
#   - search_doc_first_scan.c : ranking documentation-first sans filtre (owl search naming/sql)
#   - crossref_pin_st3_sql_scan.c : pin declaratif famille SQL (SQLConnexionOuvre -> 14_sql_connecteurs)
#   - crossref_dslprop_scan.c : extension cross-ref aux fiches propriete DSL (Projet.ActiveAvertissementConventionDeNommage -> 12_prefixes_de_nommage, viewer DSLPROPS)
#   - projet_naming_backlink_scan.c : back-link guide->fiche (12_prefixes_de_nommage -> owl explain Projet)
#   - projet_example_scan.c : exemple commente Projet (FenetreDemarrage + ActiveAvertissementConventionDeNommage) dans 24_projet.md
#   - widget_member_sort_lang_scan.c : tri language-aware des proprietes/evenements de la fiche d'un controle (ordre EN coherent)
#   - category_filter_lang_scan.c : libelles de categorie language-aware (catEN du dump + rendu filtres/tag/groupes, volet #265)
#   - guide_lang_scan.c     : guides bilingues (GUIDES titleEN/mdEN par stem + anti-doublon .en + bandeau de repli EN, volet #300)
#   - cli_learn_lang_scan.c : cablage EN du CLI documentaire (owl learn/primer servent EMBEDDED_PUBLIC_DOCS_EN sous --lang en, FR inchange, compte de fiches stable )
#
# Usage:
#   ./run.sh                 # utilise ./build/owl.exe (racine repo)
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si tous les scanners passent, 1 sinon.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

rc=0
for scan in leak_scan internal_native_scan keyword_en_scan projet_prop_scan projet_prop_descen_scan enum_dup_scan enum_accent_dup_scan date_returntype_scan explain_json_lang_scan type_field_nav_scan collision_crossref_scan widget_prop_regression_scan doc_fiches_baseline_stable_scan list_scroll_reset_scan widget_style_descen_scan table_seealso_scan crossref_pin_scan crossref_heuristic_regression_scan crossref_viewer_parity_scan search_doc_first_scan crossref_pin_st3_sql_scan crossref_dslprop_scan projet_naming_backlink_scan projet_example_scan widget_member_sort_lang_scan category_filter_lang_scan guide_lang_scan cli_learn_lang_scan; do
    BIN="$ROOT/$scan.exe"
    gcc -O0 -Wall -o "$BIN" "$ROOT/$scan.c" || {
        echo "cli_doc_lang: echec compilation $scan.c"
        exit 2
    }
    "$BIN" "$OWL" || rc=1
    rm -f "$BIN"
done

exit $rc
