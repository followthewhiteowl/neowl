#!/usr/bin/env bash
# Harnais boite noire : option `--sign` de owl-build ( + ).
#
# Contexte : owl-build sait signer le binaire produit a partir d'un certificat
# PKCS#12 (.pfx). Le comportement s'adapte a la cible :
#   - cible Windows     -> signature Authenticode incrustee dans le PE ;
#   - cible non-Windows -> signature detachee `<out>.p7s` (PKCS#7 detache) +
#                          checksum `<out>.sha256` a cote du binaire.
# La pre-validation du certificat (present + mot de passe correct) est
# desormais commune a TOUTES les cibles, et fail-fast AVANT tout build.
#
# Le coeur crypto (code_sign.c : PKCS#7 detache, round-trip `.p7s` fichier,
# format SHA256SUMS) est couvert par --self-test-net. Ce harnais couvre le
# CABLAGE CLI, cote chemins d'ERREUR qui echouent AVANT tout build (fail-fast)
# donc sans toolchain de link ni projet compilable :
#   - certificat introuvable                -> erreur actionnable (toute cible)
#   - mot de passe / certificat invalide    -> erreur actionnable (toute cible)
#
# Le chemin de SUCCES (binaire reellement signe / fichiers detaches produits)
# exige un build complet + la toolchain embarquee ; il est valide a part :
#   - Windows : oracle PowerShell du coordinator sur un vrai exe + self-test
#     Authenticode ;
#   - non-Windows : e2e sur une VRAIE sortie linux cross-compilee (via le mega
#     owl-build.exe), oracle `openssl cms -verify` + `sha256sum -c`. Le dev tree
#     ne package pas une cible linux (libs linux absentes), donc la production
#     effective des 2 fichiers n'est PAS rejouee ici.
#
# Vehicule : owl-build.exe. ATTENTION — owl-build est EXCLUDE_FROM_ALL dans
# CMakeLists.txt : `make` ne le construit PAS (c'est le mega). Ce harnais
# SKIP proprement (exit 0) si le binaire est absent. A rejouer a la main
# apres un relink de owl-build quand on touche build_main.c.
#
# Usage:
#   ./run.sh
#   OWL_BUILD_BIN=/path/to/owl-build.exe ./run.sh
#
# Exit code : 0 si tous les cas passent (ou si owl-build absent = SKIP),
#             1 si au moins un cas diverge.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL_BUILD="${OWL_BUILD_BIN:-$OWL_ROOT/build/owl-build.exe}"

if [ ! -f "$OWL_BUILD" ]; then
    echo "codesign: SKIP (owl-build.exe introuvable: $OWL_BUILD — bati par 'make mega', hors batterie)"
    exit 0
fi

# Projet a 1 marqueur top-level : passe la frontiere. La pre-validation de
# --sign (cible / certificat) intervient AVANT le portail de compile, donc
# le fait que ce fixture ne compile pas est sans importance ici.
PROJ="$OWL_ROOT/tests/internal_fixtures/cli_build_frontier/nominal"
PFX_B64="$OWL_ROOT/tests/internal_fixtures/codesign/throwaway_codesign.pfx.b64"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Decode la fixture .pfx.b64 en vrai fichier .pfx (mdp reel = owltest).
PFX="$TMP/throwaway.pfx"
if ! base64 -d "$PFX_B64" > "$PFX" 2>/dev/null; then
    # Certaines variantes de base64 veulent -D ; fallback.
    base64 -D "$PFX_B64" > "$PFX" 2>/dev/null || true
fi

FAIL=0

# Verifie exit == want_rc, sortie CONTIENT must_have, ne contient PAS le
# mot de passe (jamais trace). Args apres must_have passes tels quels.
run_case() {
    local name="$1" want_rc="$2" must_have="$3"
    shift 3
    local out rc
    out="$("$OWL_BUILD" "$@" 2>&1)"
    rc=$?
    local ok=1
    if [ "$rc" = 0 ]; then
        echo "codesign_${name}: RESULT: RED (exit 0, attendu != 0)"; ok=0
    fi
    if [ -n "$want_rc" ] && [ "$rc" != "$want_rc" ]; then
        echo "codesign_${name}: RESULT: RED (exit $rc, attendu $want_rc)"; ok=0
    fi
    if ! printf '%s' "$out" | grep -qF "$must_have"; then
        echo "codesign_${name}: RESULT: RED (sortie ne contient pas: $must_have)"; ok=0
    fi
    if printf '%s' "$out" | grep -qF "owltest"; then
        echo "codesign_${name}: RESULT: RED (le mot de passe a fuite dans la sortie)"; ok=0
    fi
    if [ "$ok" = 1 ]; then
        echo "codesign_${name}: RESULT: GREEN"
    else
        echo "  --- sortie owl-build ---"
        printf '%s\n' "$out" | sed 's/^/  /'
        FAIL=1
    fi
}

# --- Cas 1 : cible non-Windows + mauvais mot de passe -> le certificat est
#             bien PRE-VALIDE pour une cible linux (gate "Windows uniquement"
#             levee, ) : on atteint le controle du mot de passe et on
#             echoue dessus, avant tout build. ---------------------------------
run_case "non_windows_prevalidation" "" "invalide" \
    "$PROJ" --target linux-x86_64 --sign "$PFX" --sign-pass mauvais_mdp

# --- Cas 2 : certificat introuvable -> erreur actionnable, avant build. ------
run_case "pfx_introuvable" "" "introuvable" \
    "$PROJ" --target windows-x86_64 --sign "$TMP/nexiste_pas.pfx"

# --- Cas 3 : mauvais mot de passe -> certificat/mot de passe invalide. -------
run_case "mot_de_passe_invalide" "" "invalide" \
    "$PROJ" --target windows-x86_64 --sign "$PFX" --sign-pass mauvais_mdp

# --- Cas 4 : variante EN, cible non-Windows, certificat introuvable ->
#             pre-validation (et son message) commune a toutes les cibles. ----
run_case "non_windows_introuvable_en" "" "not found" \
    --en "$PROJ" --target linux-x86_64 --sign "$TMP/nexiste_pas.pfx"

exit $FAIL
