#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# scripts/refresh_codepage_tables.sh
#
# Refresh des index WHATWG Encoding Standard pour les codepages DBCS
# asiatiques (Shift-JIS, EUC-JP, EUC-KR, Big5, GB18030) et regeneration de
# `src/generated/codepage_tables.inc` (cf. ).
#
# Pipeline :
#   1. telecharge les index depuis encoding.spec.whatwg.org (idempotent SHA256)
#   2. ecrit vendor/whatwg-encoding/SNAPSHOT.md (URL + SHA256 + date + format)
#   3. compile tools/gen_codepage_tables.c a la volee (cc, toolchain C-only)
#   4. le generateur AUTO-VERIFIE des vecteurs de reference puis emet le .inc
#
# Usage : ./scripts/refresh_codepage_tables.sh
# Idempotent : skip le download si SHA256 inchange. Le .inc genere est
# COMMITE ; ce script ne tourne qu'au refresh amont (pas a chaque build).
# Frequence recommandee : manuel, tous les 12-24 mois (spec tres stable).
#
# Prerequis : bash + curl + sha256sum + cc (deja dans le toolchain OWL,
# pas de python/perl).

set -euo pipefail

cd "$(dirname "$0")/.."  # racine du repo

VENDOR_DIR="vendor/whatwg-encoding"
SNAPSHOT="$VENDOR_DIR/SNAPSHOT.md"
GEN_SRC="tools/gen_codepage_tables.c"
# Binaire intermediaire dans un dossier reel du repo (les tools natifs msys
# ne voient pas toujours un dossier mktemp -d) ; supprime en fin de run.
GEN_BIN="tools/.gen_codepage_tables.tmp.exe"
OUT_INC="src/generated/codepage_tables.inc"
BASE_URL="https://encoding.spec.whatwg.org"

# Index necessaires (ordre = ordre des arguments du generateur).
INDEXES=(
  "index-jis0208.txt"        # Shift-JIS + EUC-JP (table partagee)
  "index-jis0212.txt"        # EUC-JP sequences prefixe 0x8F
  "index-euc-kr.txt"         # EUC-KR
  "index-big5.txt"           # Big5
  "index-gb18030.txt"        # GB18030 (2 octets)
  "index-gb18030-ranges.txt" # GB18030 (plages 4 octets)
)

human_size() {
  local bytes="$1"
  if [ "$bytes" -lt 1024 ]; then
    echo "${bytes} B"
  elif [ "$bytes" -lt 1048576 ]; then
    local kb=$((bytes * 10 / 1024)); echo "$((kb / 10)).$((kb % 10)) KB"
  else
    local mb=$((bytes * 10 / 1048576)); echo "$((mb / 10)).$((mb % 10)) MB"
  fi
}

mkdir -p "$VENDOR_DIR" "$(dirname "$OUT_INC")"

echo ">> WHATWG encoding index refresh"
echo ">> Vendor dir : $VENDOR_DIR"
echo ""

TIMESTAMP=$(date +%Y-%m-%dT%H:%M:%S%z)
TMP_SNAPSHOT=$(mktemp)
cat > "$TMP_SNAPSHOT" <<HEADER
# WHATWG Encoding — index snapshot vendore

Source : https://encoding.spec.whatwg.org/
Script : \`scripts/refresh_codepage_tables.sh\`
Generateur : \`tools/gen_codepage_tables.c\` -> \`src/generated/codepage_tables.inc\`
Refresh recommande : manuel, tous les 12-24 mois (spec tres stable).

Licence des donnees : **CC-BY** (faits de correspondance octet<->codepoint du
WHATWG Encoding Standard). Voir THIRD-PARTY-NOTICES.

## Etat du snapshot

- **Date refresh** : $TIMESTAMP
- **Format des lignes** : \`<pointer>\\t0x<CODEPOINT_HEX>\\t<glyphe> (<NOM>)\`
  (le fichier \`index-gb18030-ranges.txt\` n'a que \`<pointer>\\t0x<HEX>\`).

## Fichiers vendorises

| Fichier | Taille | SHA256 | Usage |
|---|---:|---|---|
HEADER

CHANGED=0; SKIPPED=0; FAILED=0

usage_of() {
  case "$1" in
    index-jis0208.txt)        echo "Shift-JIS + EUC-JP (table partagee)";;
    index-jis0212.txt)        echo "EUC-JP sequences 0x8F";;
    index-euc-kr.txt)         echo "EUC-KR";;
    index-big5.txt)           echo "Big5";;
    index-gb18030.txt)        echo "GB18030 (2 octets)";;
    index-gb18030-ranges.txt) echo "GB18030 (plages 4 octets)";;
    *)                        echo "-";;
  esac
}

for NAME in "${INDEXES[@]}"; do
  URL="$BASE_URL/$NAME"
  LOCAL="$VENDOR_DIR/$NAME"
  # NB : ne PAS nommer cette variable TMP/TEMP/TMPDIR -> ce sont les env vars
  # que cc/gcc lit pour ses fichiers intermediaires.
  DL=$(mktemp)
  printf ">> %-28s " "$NAME..."
  if ! curl -sf "$URL" -o "$DL"; then
    echo "FAIL (download)"; rm -f "$DL"; FAILED=$((FAILED + 1)); continue
  fi
  NEW_SHA=$(sha256sum "$DL" | cut -d' ' -f1)
  SIZE=$(wc -c < "$DL" | tr -d ' ')
  HUMAN=$(human_size "$SIZE")
  if [ -f "$LOCAL" ] && [ "$(sha256sum "$LOCAL" | cut -d' ' -f1)" = "$NEW_SHA" ]; then
    rm -f "$DL"; echo "skip ($HUMAN, inchange)"; SKIPPED=$((SKIPPED + 1))
  else
    mv "$DL" "$LOCAL"; echo "downloaded ($HUMAN)"; CHANGED=$((CHANGED + 1))
  fi
  echo "| \`$NAME\` | $HUMAN | \`$NEW_SHA\` | $(usage_of "$NAME") |" >> "$TMP_SNAPSHOT"
done

if [ "$FAILED" -gt 0 ]; then
  echo ""; echo "!! $FAILED fichier(s) en echec. Refresh abandonne." >&2
  rm -f "$TMP_SNAPSHOT"; exit 1
fi

cat >> "$TMP_SNAPSHOT" <<FOOTER

## Regeneration

\`\`\`bash
./scripts/refresh_codepage_tables.sh
\`\`\`

Idempotent : skip les fichiers dont le SHA256 n'a pas change. Le generateur
auto-verifie des vecteurs de reference (あ/漢, 가, 中, U+0080, U+10FFFF...)
AVANT d'ecrire le .inc : un seul vecteur KO et aucune table n'est publiee.
FOOTER

mv "$TMP_SNAPSHOT" "$SNAPSHOT"
echo ""
echo ">> SNAPSHOT.md : $SNAPSHOT ($CHANGED downloaded, $SKIPPED skipped)"
echo ""

echo ">> Compilation du generateur ($GEN_SRC)..."
cc -O2 -Wall -Wextra -std=c99 -o "$GEN_BIN" "$GEN_SRC"

echo ">> Generation + auto-verification..."
"$GEN_BIN" "$OUT_INC" \
  "$VENDOR_DIR/index-jis0208.txt" \
  "$VENDOR_DIR/index-jis0212.txt" \
  "$VENDOR_DIR/index-euc-kr.txt" \
  "$VENDOR_DIR/index-big5.txt" \
  "$VENDOR_DIR/index-gb18030.txt" \
  "$VENDOR_DIR/index-gb18030-ranges.txt"

rm -f "$GEN_BIN"
echo ""
echo ">> OK. $OUT_INC regenere ($(human_size "$(wc -c < "$OUT_INC")") de source)."
echo ">> Pense a commiter vendor/whatwg-encoding/, $OUT_INC et vendor/VERSIONS.md."
