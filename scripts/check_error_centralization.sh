#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# check_error_centralization.sh — Verrou anti-regression : tout message /
# code d'erreur user-facing DOIT passer par owl_emit_*_error (cf  /
# ), jamais par une emission ad-hoc ou un transport legacy rappele
# avec un litteral.
#
# Le self-test binaire (--self-test-registry) n'a pas acces aux sources :
# c'est ce script de build qui scanne le code C et refuse le build si un
# pattern d'emission d'erreur ad-hoc est introduit hors des sites autorises.
#
# Usage: scripts/check_error_centralization.sh <src_dir>
# Exit 0 si propre, exit 1 + liste fichier:ligne sinon.
#
# ------------------------------------------------------------------
# PATTERNS INTERDITS detectes
# ------------------------------------------------------------------
#  1. fprintf(stderr, "<message prose>"...) dans src/stdlib_*.c : une
#     native doit emettre via owl_emit_native_error. On flague un litteral
#     dont le 1er caractere est une lettre (= message prose ad-hoc). Les
#     diagnostics internes des sous-systemes UI/PDF sont prefixes [Composant]
#     (1er car '['), et les helpers de print (Erreur()) utilisent des
#     litteraux de format pur ("%s", " ", "\n") — non flagues.
#  2. runtime_error("<litteral>"...) hors du pont vm_emit_runtime_error_rendered
#     (vm.c) : une erreur runtime doit passer par owl_emit_runtime_error(E_CODE).
#  3. error_at( / error_at_current( / error( / warning_at( avec un litteral
#     string en arg message, hors definitions helpers + pont
#     compile_emit_diagnostic_at (compiler.c) : doit passer par owl_emit_error_at.
#  4. owl_set_error( : symbole supprime , filet anti-reintroduction.
#  5. consume( / compile_emit_diagnostic_at( avec un litteral prose : le
#     message d'un `consume(TOKEN_X, "...")` doit venir du catalogue
#     (owl_errors.def), pas d'une prose anglaise en dur (cf ).
#
# ------------------------------------------------------------------
# TROU DE PERIMETRE FERME PAR  — a lire avant d'ajouter un pattern
# ------------------------------------------------------------------
# Ce controle a longtemps conclu "OK, aucune emission ad-hoc" alors que
# src/compiler.c portait 148 messages d'erreur en dur (monolingues). Deux
# defauts cumules, tous deux structurels :
#
#  (a) PERIMETRE DEFINI PAR NOM D'EMETTEUR, PAS PAR PROVENANCE DU MESSAGE.
#      Le pattern 3 enumere quatre noms (error_at, error_at_current,
#      warning_at, error). `consume(TokenType, const char* message)` etait
#      un WRAPPER : il forwardait son argument a error_at_current(message)
#      — un IDENTIFIANT, pas un litteral, donc explicitement non flague par
#      le pattern 3 (« les definitions/declarations ne matchent pas car
#      l'arg est un identifiant »). Le litteral vivait une frame en amont,
#      a un site dont le nom de fonction n'etait pas dans l'enumeration.
#      => Tout nouveau wrapper qui transporte un `const char* message`
#         jusqu'a un emetteur DOIT etre ajoute a HELPER_CALLS ci-dessous.
#
#  (b) MATCHING LIGNE-A-LIGNE. Le pattern exigeait le litteral sur la MEME
#      ligne physique que le nom de l'emetteur. Quatre sites y echappaient
#      par simple retour a la ligne :
#          error_at_current(
#              "A single 'Theme' block is allowed per file.");
#      => Le scan reconstitue desormais la LIGNE LOGIQUE (jonction des
#         continuations tant que les parentheses sont desequilibrees).
#
# Preuve de non-regression : reintroduire `consume(TOKEN_FIN, "Expected
# 'FIN'.");` dans src/compiler.c doit faire sortir ce script en 1.
#
# ------------------------------------------------------------------
# LISTE BLANCHE (sites legitimes, non flagues)
# ------------------------------------------------------------------
#  - Fichiers helpers : owl_errors.c, owl_errors_compile.c,
#    owl_errors_runtime.c, owl_errors_native.c
#  - Ponts : compile_emit_diagnostic_at (compiler.c),
#    vm_emit_runtime_error_rendered (vm.c), owl_set_error_native
#  - Tests : src/internal_tests/
#  - Commentaires /* */, //, et lignes de docstring (1er non-blanc = * // /*)
#
# ------------------------------------------------------------------
# EXCLUSIONS DECLAREES — litteraux volontairement conserves
# ------------------------------------------------------------------
#  - error_capacity_exceeded("MAX_XXX", MAX_XXX) : l'argument litteral est
#    un IDENTIFIANT DE REGISTRE (cle de owl_limits.def), pas une prose
#    utilisateur. Il alimente le placeholder {limite} du code catalogue
#    E_SYNTAX_CAPACITY_EXCEEDED et la commande `owl explain <name>`. Rien a
#    traduire : un nom de limite est le meme en FR et en EN. Non flague —
#    le nom de la fonction n'est pas dans HELPER_CALLS.
#  - Aucune autre exclusion : les 148 messages recenses par  ont
#    tous ete rapatries au catalogue. Si un site futur doit rester en dur,
#    il se declare ICI avec sa raison, pas en silence.
#
# Calibrage : 0 faux positif sur le code actuel (deja migre). Conservateur
# par design — mieux vaut rater un cas exotique que casser le build sur du
# legitime. On resserre apres si besoin.

set -u

SRC_DIR="${1:-src}"

if [[ ! -d "$SRC_DIR" ]]; then
    echo "ERROR: src dir not found: $SRC_DIR" >&2
    exit 2
fi

# Fichiers entierement whitelistes (helpers du registry erreurs).
HELPER_FILES=(
    "owl_errors.c"
    "owl_errors_compile.c"
    "owl_errors_runtime.c"
    "owl_errors_native.c"
)

violations=()
# Garde de non-vacuite : ce controle conclut "OK (aucune emission
# ad-hoc)" en ne trouvant rien — verdict indistinguable de "je n'ai lu aucun
# fichier". On compte les fichiers reellement scannes, on l'affiche, et on
# echoue si le compte est nul.
scanned=0

is_helper_file() {
    local base; base="$(basename "$1")"
    local h
    for h in "${HELPER_FILES[@]}"; do
        [[ "$base" == "$h" ]] && return 0
    done
    return 1
}

# Vrai si la ligne est un commentaire (1er car non-blanc dans * // /*) ou
# contient un debut de commentaire avant le pattern. Heuristique simple mais
# suffisante : on retire la portion commentaire // et on rejette les lignes
# de bloc (* ... ou /* ...).
strip_and_is_comment() {
    # $1 = ligne brute. echo la ligne sans la portion // ; renvoie 1 si la
    # ligne est purement un commentaire de bloc.
    local line="$1"
    local trimmed="${line#"${line%%[![:space:]]*}"}"  # ltrim
    case "$trimmed" in
        '*'*|'//'*|'/*'*) return 1 ;;  # commentaire de bloc / ligne
    esac
    return 0
}

add_violation() {
    violations+=("$1")
}

# Pre-filtre grep : le balayage lourd (toutes les lignes de tous les .c) est
# delegue a grep (C optimise) au lieu d'une boucle bash ligne-par-ligne sur
# ~138k lignes (qui prenait ~40s sous MSYS2). Le pattern combine est un
# SUR-ENSEMBLE : il capture toute ligne qu'un des 4 patterns precis pourrait
# matcher (plus des candidats benins). La logique de decision exacte (strip
# commentaire + 4 patterns + whitelist) est appliquee ENSUITE, a l'identique,
# sur les seules candidates. Sortie strictement iso-fonctionnelle :
# - meme ordre fichiers (find -print0)
# - meme ordre lignes (grep -n croissant)
# - meme ordre patterns par ligne (p4, p1, p2, p3)
# - meme numero de ligne (grep -n donne le vrai lineno)
CANDIDATE_RE='owl_set_error\(|fprintf\(stderr|runtime_error\(|error_at\(|error_at_current\(|warning_at\(|error\(|consume\(|compile_emit_diagnostic_at\('

# Variante awk du meme pattern. `\(` est une sequence d'echappement INVALIDE
# pour une regex dynamique awk : gawk la reduit a `(` et meurt sur
# "Unmatched (". Une sortie vide silencieuse ferait conclure "OK, rien
# trouve" — le faux vert exact que  a chasse. On passe donc par une
# classe de caracteres `[(]`, sans backslash, et on echoue bruyamment si awk
# se plante malgre tout (cf. logical_candidates).
AWK_CANDIDATE_RE="${CANDIDATE_RE//\\(/[(]}"

# Reconstitution de la ligne logique (, defaut (b)). awk joint une
# ligne candidate avec ses continuations tant que les parentheses ouvertes
# ne sont pas refermees (max 4 lignes : au-dela c'est un appel structure,
# pas un litteral egare). Les portions // ... sont retirees avant jonction.
# Sortie : "<lineno de la 1re ligne>:<ligne logique>" — meme contrat que
# `grep -n`, donc la logique de decision en aval est inchangee.
logical_candidates() {
    awk -v re="$AWK_CANDIDATE_RE" '
    function strip_line_comment(s,   p) {
        p = index(s, "//")
        if (p > 0) s = substr(s, 1, p - 1)
        return s
    }
    function paren_balance(s,   i, c, n) {
        n = 0
        for (i = 1; i <= length(s); i++) {
            c = substr(s, i, 1)
            if (c == "(") n++
            else if (c == ")") n--
        }
        return n
    }
    { raw[NR] = $0 }
    END {
        for (i = 1; i <= NR; i++) {
            if (raw[i] !~ re) continue
            logical = strip_line_comment(raw[i])
            bal = paren_balance(logical)
            j = i
            while (bal > 0 && j < NR && j < i + 4) {
                j++
                cont = strip_line_comment(raw[j])
                sub(/^[ \t]+/, "", cont)
                logical = logical " " cont
                bal += paren_balance(cont)
            }
            print i ":" logical
        }
    }
    ' "$1"
}

# Itere tous les .c de src/ sauf internal_tests/.
while IFS= read -r -d '' file; do
    case "$file" in
        */internal_tests/*) continue ;;
    esac
    is_helper_file "$file" && continue

    scanned=$((scanned + 1))
    base="$(basename "$file")"
    is_stdlib=false
    [[ "$base" == stdlib_*.c ]] && is_stdlib=true

    # "lineno:ligne logique", en ordre croissant. On ne traite que les
    # candidates au lieu de toutes les lignes du fichier. Le scan est
    # materialise dans une variable (pas une substitution de processus) pour
    # que son code de sortie soit LISIBLE : un scanner qui meurt en silence
    # rendrait un vert mensonger.
    if ! candidates="$(logical_candidates "$file")"; then
        echo "CENTRALISATION ERREURS : le scan a echoue sur '$file'." >&2
        echo "Un scanner muet produit un faux vert : echec, pas un OK." >&2
        exit 1
    fi

    while IFS= read -r grepline; do
        [[ -n "$grepline" ]] || continue
        lineno="${grepline%%:*}"
        line="${grepline#*:}"

        # Skip lignes-commentaires de bloc.
        if ! strip_and_is_comment "$line"; then
            continue
        fi
        # Retire toute portion // ... (commentaire de fin de ligne).
        code="${line%%//*}"

        # --- Pattern 4 : owl_set_error( (pas _native) ---
        if [[ "$code" =~ owl_set_error\( ]]; then
            add_violation "$file:$lineno: owl_set_error() reintroduit (symbole supprime feat/0093) -> owl_emit_native_error"
        fi

        # --- Pattern 1 : fprintf(stderr, "<lettre>"...) dans stdlib_*.c ---
        # Message prose ad-hoc : 1er car du litteral est une lettre.
        # Exclut [Composant] (prefixe '[') et formats purs ("%s"/" "/"\n").
        if $is_stdlib && [[ "$code" =~ fprintf\(stderr,[[:space:]]*\"[A-Za-z] ]]; then
            add_violation "$file:$lineno: fprintf(stderr, \"<message>\") ad-hoc -> owl_emit_native_error"
        fi

        # --- Pattern 2 : runtime_error("<litteral>"...) hors pont ---
        # Le pont vm_emit_runtime_error_rendered appelle runtime_error("%s",..).
        # On flague un litteral commencant par autre chose que % (donc prose).
        if [[ "$code" =~ runtime_error\([[:space:]]*\"[^%] ]]; then
            add_violation "$file:$lineno: runtime_error(\"<litteral>\") -> owl_emit_runtime_error(E_CODE)"
        fi

        # --- Pattern 3 : error_at/error/warning_at(... , "<litteral>") ---
        # Ces helpers prennent un message const char*. Un appel avec litteral
        # contourne le registry. Les definitions/declarations (static void
        # error_at(Token*, const char* message)) ne matchent pas car l'arg
        # est un identifiant, pas un litteral. Le pont
        # compile_emit_diagnostic_at recoit aussi une variable, pas un litteral.
        # Le helper doit etre precede d'un non-identifiant (pas '_') pour
        # exclure les ponts owl_emit_warning_at / owl_emit_error_at qui
        # contiennent error_at/warning_at en sous-chaine. Ses arguments
        # litteraux (codes au format "name", etc.) ne doivent pas matcher.
        if [[ "$code" =~ (^|[^_a-zA-Z0-9])(error_at|error_at_current|warning_at|error)\([^\)]*\"[A-Za-z] ]]; then
            add_violation "$file:$lineno: error_at/error/warning_at(\"<litteral>\") -> owl_emit_error_at(E_CODE)"
        fi

        # --- Pattern 5 : wrappers qui transportent un message ---
        # `consume(TOKEN_X, "prose")` et `compile_emit_diagnostic_at(tok,
        # "prose", ...)` : le message doit venir du catalogue. Le litteral
        # flague commence par une lettre OU par une ponctuation de citation
        # de symbole (') — les messages du parser ouvrent souvent sur le
        # token attendu : "'FIN' attendu...". Les codes E_* passes en 2e arg
        # sont des identifiants, pas des litteraux : non flagues.
        if [[ "$code" =~ (^|[^_a-zA-Z0-9])(consume|compile_emit_diagnostic_at)\([^\)]*\"[A-Za-z\'] ]]; then
            add_violation "$file:$lineno: consume/compile_emit_diagnostic_at(\"<litteral>\") -> code du catalogue (owl_errors.def)"
        fi
    done <<< "$candidates"
done < <(find "$SRC_DIR" -name '*.c' -print0)

if [[ $scanned -eq 0 ]]; then
    echo "CENTRALISATION ERREURS : CORPUS VIDE — 0 fichier .c scanne sous '$SRC_DIR'." >&2
    echo "Un controle qui n'examine rien ne prouve rien : echec, pas un vert." >&2
    exit 1
fi

if [[ ${#violations[@]} -gt 0 ]]; then
    echo "CENTRALISATION ERREURS : ${#violations[@]} emission(s) ad-hoc detectee(s) :"
    for v in "${violations[@]}"; do
        echo "  $v"
    done
    echo ""
    echo "Tout message/code d'erreur doit passer par owl_emit_*_error (cf ADR_0020)."
    exit 1
fi

echo "CENTRALISATION ERREURS : OK ($scanned fichiers scannes, aucune emission ad-hoc)"
exit 0
