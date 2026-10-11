#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Bundle owl Linux + ses deps dynamiques dans un AppImage portable.
#
# Pre-requis :
#   ~/bin/appimagetool   (telechargeable: github.com/AppImage/appimagetool)
#   ~/bin/linuxdeploy    (telechargeable: github.com/linuxdeploy/linuxdeploy)
#   Qt6 vendor build     (make qt6, ou OWL_QT6_INSTALL pour pointer ailleurs)
#   FUSE pour lancer les AppImage tools
#
# Usage :
#   bash scripts/build_appimage.sh [OUTDIR]
#   OUTDIR par defaut : ./release/
#
# Resultat : OUTDIR/owl-<version>-x86_64.AppImage
# Test rapide :
#   ./release/owl-0.1.0-x86_64.AppImage --version
#   ./release/owl-0.1.0-x86_64.AppImage examples/cross_platform_demo/demo.owl

set -e

ROOT="${OWL_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
OUTDIR="${1:-$ROOT/release}"
APPIMAGETOOL="${APPIMAGETOOL:-$HOME/bin/appimagetool}"
LINUXDEPLOY="${LINUXDEPLOY:-$HOME/bin/linuxdeploy}"
VERSION="${OWL_VERSION:-0.1.0}"
QT6_INSTALL="${OWL_QT6_INSTALL:-$ROOT/_local/host_tools/qt6}"

# Nettoyer le PATH des entries Windows /mnt/c/* qui polluent cmake/ldd.
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$HOME/bin"

[[ -x "$APPIMAGETOOL" ]] || { echo "ERROR: appimagetool not found at $APPIMAGETOOL" >&2; exit 2; }
[[ -x "$LINUXDEPLOY"  ]] || { echo "ERROR: linuxdeploy not found at $LINUXDEPLOY"   >&2; exit 2; }
[[ -d "$QT6_INSTALL"  ]] || { echo "ERROR: Qt6 not built at $QT6_INSTALL (run make qt6 or set OWL_QT6_INSTALL)" >&2; exit 2; }

echo "==> Build owl (Release) si necessaire"
if [[ ! -x "$ROOT/build_owl/owl" ]]; then
    cmake -S "$ROOT" -B "$ROOT/build_owl" -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOWL_QT6_INSTALL="$QT6_INSTALL"
    cmake --build "$ROOT/build_owl" --target owl
fi

# Preparer AppDir from scratch
APPDIR="$ROOT/build_owl/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp "$ROOT/build_owl/owl" "$APPDIR/usr/bin/owl"

# .desktop : metadata pour les desktop env (menu, alt+tab, etc.)
cat > "$APPDIR/usr/share/applications/owl.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=OWL
GenericName=Cross-platform OWL runtime
Comment=Run OWL programs (.owl files)
Exec=owl %F
Icon=owl
Categories=Development;IDE;
Terminal=false
MimeType=text/x-owl;
EOF

# Icone placeholder (256x256 bleu uni). A remplacer par un vrai logo plus tard.
ICON="$APPDIR/usr/share/icons/hicolor/256x256/apps/owl.png"
python3 - "$ICON" <<'PYEOF'
import struct, zlib, sys
def chunk(typ, data):
    return struct.pack(">I", len(data)) + typ + data + struct.pack(">I", zlib.crc32(typ + data))
sig = b"\x89PNG\r\n\x1a\n"
ihdr = struct.pack(">IIBBBBB", 256, 256, 8, 6, 0, 0, 0)
raw = b"".join(b"\x00" + b"\x4A\x6F\xC8\xFF" * 256 for _ in range(256))
idat = zlib.compress(raw)
data = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(data)
PYEOF

echo "==> linuxdeploy : collecte des libs dynamiques (X11, fontconfig, freetype, libstdc++, libgcc_s)"
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/owl" \
    --desktop-file "$APPDIR/usr/share/applications/owl.desktop" \
    --icon-file "$ICON"

echo "==> appimagetool : bundle final"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/owl-${VERSION}-x86_64.AppImage"
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUT"

echo ""
echo "==> Done: $OUT"
ls -la "$OUT"
file "$OUT"
