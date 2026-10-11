#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Cree le single-file owl-build geant (PyInstaller-style) :
#   [owl-build.exe normal]
#   [zip payload]
#   [footer 24 bytes : offset_u64 + size_u64 + magic "OWLBUNDL"]
#
# Au runtime owl-build lit le footer, extract le zip dans temp si pas
# en cache, et utilise le chemin temp comme installRoot.
#
# Pre-requis : dist/owl-build-<host>/ assemble par package_dist.sh.
# Usage : bash scripts/pack_megafile.sh [DIST_DIR] [OUTPUT]
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in
    Linux*) HOST="x86_64-linux"; EXE_EXT="" ;;
    MSYS*|MINGW*|CYGWIN*) HOST="x86_64-windows"; EXE_EXT=".exe" ;;
    Darwin*)
        if [ "$(uname -m)" = "arm64" ]; then HOST="aarch64-macos"
        else HOST="x86_64-macos"; fi
        EXE_EXT="" ;;
esac

DIST="${1:-$ROOT/_local/build/release-stage/$HOST}"
OUTPUT="${2:-$ROOT/release/owl-build-mega-$HOST$EXE_EXT}"
mkdir -p "$(dirname "$OUTPUT")"

[ -d "$DIST" ] || { echo "ERROR: release stage introuvable : $DIST (lance d'abord package_dist.sh)" >&2; exit 2; }
[ -f "$DIST/owl-build$EXE_EXT" ] || { echo "ERROR: owl-build$EXE_EXT manquant dans $DIST" >&2; exit 2; }

STAGE="$ROOT/_local/build/release-stage/_stage_megafile"
echo "==> Staging payload (sans owl-build lui-meme) dans $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
# Copie tout sauf owl-build
(cd "$DIST" && find . -mindepth 1 \( ! -name "owl-build$EXE_EXT" -o -path "./*/*" \) -print0) | \
    while IFS= read -r -d '' rel; do
        :
    done
# Plus simple : cp -r tout puis remove owl-build
cp -r "$DIST"/. "$STAGE/"
rm -f "$STAGE/owl-build$EXE_EXT"

echo "==> Creation du zip payload (uncompressed, plus rapide a extract)"
PAYLOAD="$ROOT/_local/build/release-stage/_payload.zip"
mkdir -p "$(dirname "$PAYLOAD")"
rm -f "$PAYLOAD"
# Compile pack_zip a la volee si pas dispo (vendor miniz interne, pas de
# dep externe genre `zip` cli absent sur MSYS2).
PACKER="$ROOT/scripts/pack_zip$EXE_EXT"
if [ ! -x "$PACKER" ] || [ "$ROOT/scripts/pack_zip.c" -nt "$PACKER" ]; then
    echo "==> Compiling scripts/pack_zip$EXE_EXT"
    gcc -O2 -I"$ROOT/vendor/miniz" \
        "$ROOT/scripts/pack_zip.c" "$ROOT/vendor/miniz/miniz.c" \
        -o "$PACKER"
fi
"$PACKER" "$STAGE" "$PAYLOAD"
PAYLOAD_SIZE=$(stat -c%s "$PAYLOAD" 2>/dev/null || stat -f%z "$PAYLOAD")
echo "    payload zip : $PAYLOAD_SIZE bytes"

echo "==> Concatenation owl-build + payload + footer"
cp "$DIST/owl-build$EXE_EXT" "$OUTPUT"
EXE_SIZE=$(stat -c%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT")
cat "$PAYLOAD" >> "$OUTPUT"

# Footer 24 bytes via mini binaire C (cf scripts/write_footer.c).
# Compile a la volee si pas deja la — pas de dep externe (gcc requis,
# meme dep que pour builder OWL lui-meme).
WRITER="$ROOT/scripts/write_footer$EXE_EXT"
if [ ! -x "$WRITER" ] || [ "$ROOT/scripts/write_footer.c" -nt "$WRITER" ]; then
    echo "==> Compiling scripts/write_footer$EXE_EXT"
    gcc -O2 -o "$WRITER" "$ROOT/scripts/write_footer.c"
fi
"$WRITER" "$EXE_SIZE" "$PAYLOAD_SIZE" "$OUTPUT"

# Cleanup intermediaires
rm -rf "$STAGE" "$PAYLOAD"

# Stats
TOTAL=$(stat -c%s "$OUTPUT" 2>/dev/null || stat -f%z "$OUTPUT")
chmod +x "$OUTPUT"
echo ""
echo "==> Done : $OUTPUT"
echo "    exe   : $EXE_SIZE bytes (orchestrateur)"
echo "    payload : $PAYLOAD_SIZE bytes (zig + libs OWL + Qt6 + headers)"
echo "    total : $TOTAL bytes ($(echo "scale=1; $TOTAL / 1024 / 1024" | bc) MB)"
