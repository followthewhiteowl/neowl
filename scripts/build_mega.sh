#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# build_mega.sh — Pipeline complet de production des livrables release pour
# le host courant. Genere :
#   - release/owl-build[.exe] : single-file mega (~547 MB) packager autonome
#   - release/owl[.exe]       : interpreteur GUI all-in-one (~78 MB)
#
# Pre-requis (verifies par le script) :
#   - cmake + ninja  (sur le PATH)
#   - gcc            (sur le PATH, juste pour compiler les helpers C)
#   - vendor/zig/<host>/  (sinon : bash scripts/setup_zig.sh)
#   - Qt6 vendore zig native build (cf instructions ci-dessous)
#
# Le build Qt6 zig native est trop long (~30 min) pour etre dans ce
# pipeline. Lance-le UNE FOIS par version Qt6 :
#   - Linux WSL2 : voir docs/internal_documentation/owl_build_architecture.md
#   - Windows MSYS2 : pareil
#
# Usage : bash scripts/build_mega.sh
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
BUILD_DIR="$ROOT/_local/build/zig_$HOST"
OUT_OWL_BUILD="$ROOT/release/owl-build$EXE_EXT"
OUT_OWL="$ROOT/release/owl$EXE_EXT"
mkdir -p "$ROOT/release"

# --- Anonymisation : scrub du chemin de build absolu (meme P0 que build_release.sh) ---
# owl-build embarque le runtime OWL injecte dans les exes utilisateur : un chemin
# de build baked (Qt qt_prfxpath, llama __FILE__) fuiterait dans CHAQUE exe produit
# par un tiers. On scrubbe le stage (payload) AVANT le pack + les binaires finaux.
# cf. scripts/scrub_build_paths.c.
mkdir -p "$ROOT/_local/build"
SCRUB="$ROOT/_local/build/scrub_build_paths$EXE_EXT"
gcc -O2 -o "$SCRUB" "$ROOT/scripts/scrub_build_paths.c"
SCRUB_WINROOT="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
SCRUB_WINROOT_BS="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"
SCRUB_HOME_M="$(cygpath -m "$USERPROFILE" 2>/dev/null || true)"
SCRUB_HOME_W="$(cygpath -w "$USERPROFILE" 2>/dev/null || true)"
# Forme WSL (/mnt/c/...) : le payload linux (Qt6 install .cmake/.prl/.pri +
# static libs) est bati sous WSL et bake le chemin de build en /mnt/<drive>/...
# Sans ce scrub, le chemin machine fuite dans owl-build (cross-compile linux).
SCRUB_WSLROOT="$(printf '%s' "$SCRUB_WINROOT" | sed 's#^\([A-Za-z]\):#/mnt/\l\1#')"
# Ce scrub couvre TOUTES les formes du chemin de build pour qu'aucun chemin
# machine ne subsiste dans owl-build : slash et simple-backslash (scrub_bin),
# backslash echappee + profil utilisateur echappe (passe de staging [2b]),
# et forme WSL /mnt/<drive>/... bakee dans le payload linux (cross-compile).
scrub_bin() {
    local bin="$1"; [ -f "$bin" ] || return 0
    "$SCRUB" "$bin" "$SCRUB_WINROOT" || true
    [ -n "$SCRUB_WINROOT_BS" ] && [ "$SCRUB_WINROOT_BS" != "$SCRUB_WINROOT" ] && { "$SCRUB" "$bin" "$SCRUB_WINROOT_BS" || true; }
    [ -n "$SCRUB_HOME_M" ] && { "$SCRUB" "$bin" "$SCRUB_HOME_M" || true; }
    [ -n "$SCRUB_HOME_W" ] && [ "$SCRUB_HOME_W" != "$SCRUB_HOME_M" ] && { "$SCRUB" "$bin" "$SCRUB_HOME_W" || true; }
    # MSYS_NO_PATHCONV : sinon Git-Bash convertit l'arg /mnt/c/... en chemin
    # Windows (prefixe par la racine d'install de Git) avant le binaire natif.
    # Mais ca desactive AUSSI la conversion du chemin du binaire : on le passe
    # donc deja en forme windows via cygpath -w (le pattern /mnt/c reste intact).
    [ -n "$SCRUB_WSLROOT" ] && [ "$SCRUB_WSLROOT" != "$SCRUB_WINROOT" ] && { MSYS_NO_PATHCONV=1 "$SCRUB" "$(cygpath -w "$bin")" "$SCRUB_WSLROOT" || true; }
    return 0
}

# Pre-checks
[ -x "$ROOT/vendor/zig/$HOST/zig$EXE_EXT" ] || {
    echo "==> zig non vendore. Lance : bash scripts/setup_zig.sh"
    bash "$ROOT/scripts/setup_zig.sh"
}

QT6_INSTALL="$ROOT/_local/qt6/$HOST/install"
[ -d "$QT6_INSTALL" ] || {
    echo "ERROR: Qt6 zig native non trouve : $QT6_INSTALL" >&2
    echo "       Lance : bash scripts/build_qt6_zig_native.sh   # ~30 min, one-time" >&2
    exit 2
}

# Find QT_HOST_PATH (host tools moc/rcc/syncqt, viennent du build gcc make qt6)
QT_HOST_PATH=""
[ -d "$ROOT/_local/host_tools/qt6" ] && QT_HOST_PATH="$ROOT/_local/host_tools/qt6"

cd "$ROOT"

echo "==> [1/4] Configure + build owl + libs zig native ($BUILD_DIR)"
CMAKE_ARGS=(
    -G Ninja
    -S "$ROOT" -B "$BUILD_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_native.cmake"
    -DCMAKE_BUILD_TYPE=Release
    -DOWL_QT6_INSTALL="$QT6_INSTALL"
    -DOWL_BUILD_LSP=OFF
    -DOWL_RELEASE_DISTRIB=ON
)
[ -n "$QT_HOST_PATH" ] && CMAKE_ARGS+=(-DQT_HOST_PATH="$QT_HOST_PATH")
cmake "${CMAKE_ARGS[@]}" > /dev/null
cmake --build "$BUILD_DIR" --target owl owl-build owl_distrib

echo ""
echo "==> [2/4] Assemble _local/build/release-stage/$HOST/"
bash "$ROOT/scripts/package_dist.sh" "$BUILD_DIR" > /dev/null
du -sh "$ROOT/_local/build/release-stage/$HOST" | awk '{print "    stage size :", $1}'
echo "==> [2b/4] Anonymisation du stage (chemin de build, toutes formes) avant pack"
# Le payload embarque les static libs (.a : codeview -> commande -cc1 + isystem
# zig en backslash ECHAPPE) et les configs cmake/.pri de Qt (chemins d'install
# bakes). Ces chemins pointent vers l'arbre de build (morts a l'extraction
# %TEMP% chez l'utilisateur). On scrubbe toutes les formes : slash + simple-bs +
# profil (scrub_bin) + DOUBLE-bs echappee (construite par sed, fiable).
STAGE="$ROOT/_local/build/release-stage/$HOST"
WINROOT_BS_ESC="$(printf '%s' "$SCRUB_WINROOT_BS" | sed 's/\\/\\\\/g')"
HOME_W_ESC="$(printf '%s' "$SCRUB_HOME_W" | sed 's/\\/\\\\/g')"
find "$STAGE" -type f \( -name '*.exe' -o -name '*.dll' -o -name '*.a' -o -name '*.o' \
     -o -name '*.cmake' -o -name '*.pri' -o -name '*.prl' -o -name '*.cfg' \
     -o -name '*.pc' -o -name '*.txt' \) -print0 2>/dev/null \
  | while IFS= read -r -d '' f; do
        scrub_bin "$f"
        [ -n "$WINROOT_BS_ESC" ] && [ "$WINROOT_BS_ESC" != "$SCRUB_WINROOT_BS" ] && { "$SCRUB" "$f" "$WINROOT_BS_ESC" || true; }
        [ -n "$HOME_W_ESC" ] && [ "$HOME_W_ESC" != "$SCRUB_HOME_W" ] && { "$SCRUB" "$f" "$HOME_W_ESC" || true; }
    done

echo ""
echo "==> [3/4] Pack single-file owl-build"
bash "$ROOT/scripts/pack_megafile.sh" \
    "$ROOT/_local/build/release-stage/$HOST" \
    "$OUT_OWL_BUILD" 2>&1 | grep -E "Compiling|Done|files added|payload zip"

echo ""
echo "==> [4/4] Copy owl interpreter all-in-one"
cp "$BUILD_DIR/owl$EXE_EXT" "$OUT_OWL"

echo ""
echo "==> [5/5] Scrub du chemin de build dans les binaires finaux (owl-build + owl)"
scrub_bin "$OUT_OWL_BUILD"
scrub_bin "$OUT_OWL"

echo ""
echo "==> Done"
ls -la "$OUT_OWL_BUILD" "$OUT_OWL" | awk '{printf "    %s  %s bytes (%.1f MB)\n", $9, $5, $5/1024/1024}'
echo ""
echo "    Test : $OUT_OWL --version"
echo "           $OUT_OWL_BUILD --help"
