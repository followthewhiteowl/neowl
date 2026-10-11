#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# ============================================================
# Verrou doc <-> template pour les champs d'un type integre.
#
# Compare, pour chaque type documente, la table de champs publiee dans
# docs/public_documentation/ avec les champs REELS du template
# (source de verite : `owl explain <type> --json`).
#
# Raison d'etre : `13_sql_modele.md` a documente pendant des
# mois deux champs de sqlConnexion (`TimeoutCnx`, `Acces`) qui n'ont
# jamais existe dans le template — `cnx..TimeoutCnx = 30` refusait de
# compiler alors que la doc le promettait. Rien ne criait : ni le
# self-test registry (qui itere le registre, pas la doc), ni la suite,
# qui n'a aucune raison d'ecrire un champ qu'elle ne connait pas.
#
# La liste des types couverts est DERIVEE, pas maintenue :
# on scanne docs/public_documentation/ pour toute table dont l'en-tete
# est `| Field FR | Field EN |`, et on remonte la section `### \`<type>\``
# qui l'englobe. Toute future table de champs entre donc d'office dans
# le controle — c'est le coeur du ticket : ne pas recreer le defaut d'une
# liste tenue a la main qui prend du retard en silence.
#
# Usage : bash scripts/check_doc_type_fields.sh
#         OWL_BIN=... bash scripts/check_doc_type_fields.sh
# ============================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OWL="${OWL_BIN:-$ROOT/build/owl.exe}"
DOCDIR="$ROOT/docs/public_documentation"

if [ ! -x "$OWL" ]; then
    echo "check_doc_type_fields: binaire introuvable ($OWL)" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- Derivation des cibles : type|fichier|section.
# Chaque table `| Field FR | Field EN |` sous une section `### \`<type>\``
# devient une cible. Le nom du fichier est relatif au depot pour les
# messages. On lit tous les .md, la section courante suit le dernier
# titre `### \`...\`` rencontre.
DERIVED="$TMP/targets.txt"
: > "$DERIVED"
while IFS= read -r doc_file; do
    # `cur` = section de type courante. Il doit se comporter comme le
    # marqueur `insec` de l'extraction cote doc (plus bas), qui coupe au
    # PREMIER `^#` : sinon une table posee bien plus loin dans le fichier
    # se voit rattachee a un type dont l'extraction ne la lira jamais ->
    # cible verte a vide. On reset donc `cur` a chaque titre qui n'est pas
    # une section de type pure `### `<type>``.
    awk -v file="$doc_file" '
        /^###[[:space:]]+`[^`]+`[[:space:]]*$/ {
            cur = $0
            sub(/^###[[:space:]]+`/, "", cur)
            sub(/`[[:space:]]*$/, "", cur)
            next
        }
        /^#/ { cur = ""; next }
        /^\|[[:space:]]*Field FR[[:space:]]*\|[[:space:]]*Field EN[[:space:]]*\|/ {
            if (cur != "") print cur "|" file "|### `" cur "`"
            else print "__ORPHAN__|" file "|" FNR
        }
    ' "$doc_file" >> "$DERIVED"
done < <(find "$DOCDIR" -type f -name '*.md' | LC_ALL=C sort)

if [ ! -s "$DERIVED" ]; then
    echo "check_doc_type_fields: aucune table \`| Field FR | Field EN |\` trouvee dans $DOCDIR." >&2
    echo "  La sonde de derivation est devenue muette (format d'en-tete change ?) — la reparer, pas la retirer." >&2
    exit 1
fi

rc=0
n_types=0

while IFS='|' read -r type_name doc_file section; do
    [ -z "$type_name" ] && continue

    # Table de champs qui ne vit sous aucune section `### `<type>``.
    # On ne sait pas a quel type la rattacher -> rouge, jamais silence.
    if [ "$type_name" = "__ORPHAN__" ]; then
        rel="${doc_file#$ROOT/}"
        echo "check_doc_type_fields: table \`| Field FR | Field EN |\` sans section \`### \`<type>\`\` englobante ($rel ligne $section)." >&2
        echo "  Impossible de deriver le type a verifier — poser la table sous sa section de type, pas la laisser orpheline." >&2
        rc=1
        continue
    fi

    n_types=$((n_types + 1))

    # --- Cote template : `owl explain --json`.
    # Les champs se lisent en sequence dans le tableau "fields" ; le nom
    # affiche a l'utilisateur est la forme accentuee quand elle existe.
    "$OWL" explain "$type_name" --json > "$TMP/explain.json" 2>"$TMP/explain.err" || {
        echo "check_doc_type_fields: 'owl explain $type_name --json' a echoue :" >&2
        cat "$TMP/explain.err" >&2
        exit 1
    }

    awk '
        function val(s,   t) { t = s; sub(/^[^:]*: "/, "", t); sub(/".*$/, "", t); return t }
        /"fields": \[/            { inf = 1; next }
        inf && /"name": /          { fr = val($0) }
        inf && /"name_accented": / { fr = val($0) }
        inf && /"name_en": /       { print fr "\t" val($0) }
    ' "$TMP/explain.json" | tr -d '\r' | LC_ALL=C sort > "$TMP/template.txt"

    # --- Cote doc : la table markdown de la section.
    awk -v section="$section" '
        $0 == section            { insec = 1; next }
        insec && /^#/            { insec = 0 }
        !insec                   { next }
        /^\|[- :|]*\|[ ]*$/      { next }        # ligne de separation
        /^\|/ {
            n = split($0, c, "|")
            if (n < 3) next
            fr = c[2]; en = c[3]
            gsub(/[` \t]/, "", fr); gsub(/[` \t]/, "", en)
            if (fr == "" || fr == "FieldFR") next
            print fr "\t" en
        }
    ' "$doc_file" | tr -d '\r' | LC_ALL=C sort > "$TMP/doc.txt"

    if [ ! -s "$TMP/doc.txt" ]; then
        rel="${doc_file#$ROOT/}"
        echo "check_doc_type_fields: aucune ligne de table trouvee sous \"$section\" dans $rel." >&2
        echo "  La sonde est devenue muette (titre de section renomme ?) — la reparer, pas la retirer." >&2
        rc=1
        continue
    fi

    rel="${doc_file#$ROOT/}"
    fantomes="$(LC_ALL=C comm -23 "$TMP/doc.txt" "$TMP/template.txt")"
    absents="$(LC_ALL=C comm -13 "$TMP/doc.txt" "$TMP/template.txt")"

    if [ -n "$fantomes" ]; then
        rc=1
        echo "[$type_name] documente mais ABSENT du template (l'utilisateur lit une promesse que le compilateur refuse) :" >&2
        printf '%s\n' "$fantomes" | sed 's/^/    /' >&2
    fi
    if [ -n "$absents" ]; then
        rc=1
        echo "[$type_name] present dans le template mais ABSENT de $rel (champ indecouvrable) :" >&2
        printf '%s\n' "$absents" | sed 's/^/    /' >&2
    fi
done < "$DERIVED"

if [ "$rc" -ne 0 ]; then
    echo "" >&2
    echo "Chaque ligne est un couple <nom FR>\\t<nom EN>. Un ecart de casse ou" >&2
    echo "d'accent compte : la doc publie la forme canon, pas une approximation." >&2
    exit 1
fi

echo "check_doc_type_fields: OK ($n_types type(s) derive(s) et verifie(s))"
exit 0
