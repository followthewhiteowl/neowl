#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# check_error_template_args.sh — Verrou anti-regression : tout appel a
# owl_emit_native_error(CODE, "Native", "cle", valeur, ..., NULL) DOIT
# alimenter tous les marqueurs {cle} du gabarit de CODE (src/owl_errors.def).
#
# Pourquoi : un marqueur non alimente n'est pas efface, il est rendu
# LITTERALEMENT dans le message lu par l'utilisateur final :
#
#   HeureVersChaine: argument '{arg}' attendu Heure, ..., recu {actual}.
#
# Le correctif n'est jamais de retirer le marqueur du gabarit (le gabarit
# est juste, c'est l'appel qui ne l'alimente pas) : c'est d'ajouter la
# paire cle/valeur manquante au site d'emission.
#
# Le marqueur {native} est exclu du controle : il est alimente d'office par
# le 2e parametre positionnel (native_name) via owl_error_render_bind_native,
# donc structurellement impossible a oublier. Tous les autres marqueurs
# dependent d'une paire cle/valeur explicite au site d'emission.
#
# Usage: scripts/check_error_template_args.sh <src_dir>
# Exit 0 si propre, exit 1 + liste fichier:ligne sinon.
#
# Portee : le rendu FR et le rendu EN partagent la meme va_list, donc
# l'union des marqueurs FR + EN doit etre alimentee (un marqueur present
# seulement cote EN sortirait brut en mode `--lang en`).

set -uo pipefail

SRC_DIR="${1:-src}"
DEF_FILE="$SRC_DIR/owl_errors.def"

if [ ! -f "$DEF_FILE" ]; then
    echo "check_error_template_args: $DEF_FILE introuvable" >&2
    exit 1
fi

awk -v deffile="$DEF_FILE" '
# ---------------------------------------------------------------
# Decoupe une liste d args C en respectant chaines, chars et
# imbrication de parentheses/crochets/accolades.
# Remplit le tableau global ARG[1..NARG].
# ---------------------------------------------------------------
function split_args(s,   i, c, depth, instr, inchr, esc, cur, n) {
    delete ARG; n = 0; depth = 0; instr = 0; inchr = 0; esc = 0; cur = ""
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (esc)                       { cur = cur c; esc = 0; continue }
        if (c == "\\" && (instr||inchr)){ cur = cur c; esc = 1; continue }
        if (instr) { cur = cur c; if (c == "\"") instr = 0; continue }
        if (inchr) { cur = cur c; if (c == "'"'"'") inchr = 0; continue }
        if (c == "\"") { instr = 1; cur = cur c; continue }
        if (c == "'"'"'") { inchr = 1; cur = cur c; continue }
        if (c == "(" || c == "[" || c == "{") depth++
        if (c == ")" || c == "]" || c == "}") depth--
        if (c == "," && depth == 0) { ARG[++n] = trim(cur); cur = ""; continue }
        cur = cur c
    }
    if (trim(cur) != "") ARG[++n] = trim(cur)
    NARG = n
    return n
}

function trim(s) { sub(/^[ \t\n]+/, "", s); sub(/[ \t\n]+$/, "", s); return s }

# Extrait les marqueurs {ident} de s dans le set MK (global).
function collect_markers(s, set,   i, c, key, inkey, ok, j) {
    i = 1
    while (i <= length(s)) {
        if (substr(s, i, 1) == "{") {
            j = index(substr(s, i), "}")
            if (j == 0) break
            key = substr(s, i + 1, j - 2)
            ok = (length(key) > 0)
            if (ok && key !~ /^[A-Za-z0-9_]+$/) ok = 0
            if (ok) set[key] = 1
            i = i + j
        } else i++
    }
}

BEGIN {
    # ---- Pass 1 : gabarits de owl_errors.def -------------------
    entry = ""; depth = 0; collecting = 0
    while ((getline line < deffile) > 0) {
        if (!collecting) {
            p = index(line, "OWL_ERROR(")
            if (p == 0) continue
            collecting = 1
            entry = substr(line, p + length("OWL_ERROR("))
            depth = 1
        } else {
            entry = entry "\n" line
        }
        # recalcule la profondeur sur ce qu on a accumule
        depth = 1; instr = 0; esc = 0
        for (i = 1; i <= length(entry); i++) {
            c = substr(entry, i, 1)
            if (esc) { esc = 0; continue }
            if (c == "\\" && instr) { esc = 1; continue }
            if (c == "\"") { instr = !instr; continue }
            if (instr) continue
            if (c == "(") depth++
            if (c == ")") { depth--; if (depth == 0) { entry = substr(entry, 1, i - 1); break } }
        }
        if (depth != 0) continue
        collecting = 0
        split_args(entry)
        if (NARG < 4) continue
        code = ARG[1]
        delete MK
        collect_markers(ARG[3], MK)
        collect_markers(ARG[4], MK)
        lst = ""
        for (k in MK) { if (k != "native") lst = lst (lst == "" ? "" : " ") k }
        TPL[code] = lst
        KNOWN[code] = 1
    }
    close(deffile)
}

# ---- Pass 2 : sites d emission ---------------------------------
{
    if (!inCall) {
        p = index($0, "owl_emit_native_error(")
        if (p == 0) next
        inCall = 1; startLine = FNR
        call = substr($0, p + length("owl_emit_native_error("))
    } else {
        call = call " " $0
    }
    # profondeur
    depth = 1; instr = 0; esc = 0; endPos = 0
    for (i = 1; i <= length(call); i++) {
        c = substr(call, i, 1)
        if (esc) { esc = 0; continue }
        if (c == "\\" && instr) { esc = 1; continue }
        if (c == "\"") { instr = !instr; continue }
        if (instr) continue
        if (c == "(") depth++
        if (c == ")") { depth--; if (depth == 0) { endPos = i; break } }
    }
    if (depth != 0) next
    body = substr(call, 1, endPos - 1)
    inCall = 0

    split_args(body)
    if (NARG < 2) next
    code = ARG[1]
    native = ARG[2]
    if (code !~ /^E_[A-Z0-9_]+$/) { skipped++; next }   # code calcule / prototype
    if (!(code in KNOWN)) {
        printf "%s:%d: code inconnu du catalogue: %s\n", FILENAME, startLine, code
        bad++
        next
    }
    # cles fournies : args 3, 5, 7, ... (paires cle/valeur, NULL final)
    delete GIVEN
    dyn = 0
    for (i = 3; i <= NARG; i++) {
        a = ARG[i]
        if (a == "NULL") break
        if ((i - 3) % 2 != 0) continue          # position valeur
        if (a ~ /^".*"$/) {
            k = substr(a, 2, length(a) - 2)
            GIVEN[k] = 1
        } else dyn = 1                           # cle non litterale
    }
    if (dyn) { skipped++; next }                 # cle non litterale

    n = split(TPL[code], want, " ")
    miss = ""
    for (i = 1; i <= n; i++) if (want[i] != "" && !(want[i] in GIVEN))
        miss = miss (miss == "" ? "" : ",") want[i]
    if (miss != "") {
        printf "%s:%d: %s: marqueur(s) non alimente(s): {%s}\n",
               FILENAME, startLine, code, miss
        gsub(/,/, "} {", miss)
        bad++
    }
}

END {
    if (bad > 0) {
        printf "\ncheck_error_template_args: %d site(s) rendraient un marqueur brut a l utilisateur.\n", bad
        print  "Corriger le SITE (ajouter la paire \"cle\", valeur), pas le gabarit."
        exit 1
    }
    printf "check_error_template_args: OK — tous les marqueurs de gabarit sont alimentes (%d site(s) non decidables statiquement, ignores).\n", skipped + 0
}
' "$SRC_DIR"/*.c
