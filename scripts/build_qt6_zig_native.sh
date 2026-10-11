#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Build Qt6 statique compile sous zig cc (libc++) — necessaire pour le
# pipeline single-file mega cross-compile.
#
# Le Qt6 systeme (MSYS2 / apt) est compile sous gcc/libstdc++. Mais le mega
# est buildle sous zig cc/libc++, et l'ABI C++ est incompatible : impossible
# de melanger libstdc++ et libc++ dans le meme binaire. On rebuild donc
# Qt6 avec la meme toolchain zig que le mega, une fois par host (~30 min).
#
# Output : <ROOT>/_local/qt6/<host>/install/lib/cmake/Qt6/Qt6Config.cmake
# Ce dossier est gitignore. Lance ~1 fois par bump Qt6 (rare).
#
# Pre-requis verifie :
#   - vendor/qt6/qtbase clone (submodule)
#   - vendor/zig/<host>/zig (sinon : bash scripts/setup_zig.sh)
#   - <ROOT>/_local/host_tools/qt6 (Qt6 gcc deja build, fournit les host
#                                   tools moc/rcc/syncqt — appel par make qt6)
#
# Usage : bash scripts/build_qt6_zig_native.sh
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

case "$(uname -s)" in
    Linux*) HOST="x86_64-linux"; EXE_EXT="" ;;
    MSYS*|MINGW*|CYGWIN*) HOST="x86_64-windows"; EXE_EXT=".exe" ;;
    Darwin*)
        if [ "$(uname -m)" = "arm64" ]; then HOST="aarch64-macos"
        else HOST="x86_64-macos"; fi
        EXE_EXT="" ;;
    *) echo "ERROR: OS non supporte $(uname -s)" >&2; exit 1 ;;
esac

QT6_SRC="$ROOT/vendor/qt6/qtbase"
QTSVG_SRC="$ROOT/vendor/qt6/qtsvg"
ZIG_BIN="$ROOT/vendor/zig/$HOST/zig$EXE_EXT"
QT6_HOST_TOOLS="${OWL_QT_HOST_PATH:-$ROOT/_local/host_tools/qt6}"

# Profil musl (OWL_MUSL=1) : cible x86_64-linux-musl pour le binaire Linux
# statique universel (Alpine/scratch/glibc). Additif — n'affecte ni Windows
# ni le build glibc. Toolchain musl (isolation sysroot glibc de l'hote),
# profil OFFSCREEN (pas d'OpenGL/xcb/fontconfig), tout bundled. Les outils
# hote (moc/rcc/qmake) restent le Qt6 host glibc (QT_HOST_PATH).
TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_native.cmake"
if [ "${OWL_MUSL:-0}" = "1" ]; then
    case "$HOST" in
        *-linux) ;;
        *) echo "ERROR: OWL_MUSL=1 uniquement sur hote Linux (host=$HOST)" >&2; exit 1 ;;
    esac
    HOST="x86_64-linux-musl"
    TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_musl.cmake"
    echo "==> Profil MUSL actif : cible $HOST, toolchain $(basename "$TOOLCHAIN_FILE")"
fi

INSTALL_DIR="$ROOT/_local/qt6/$HOST"
MARKER_QTBASE="$INSTALL_DIR/install/lib/cmake/Qt6/Qt6Config.cmake"
MARKER_QTSVG="$INSTALL_DIR/install/lib/cmake/Qt6Svg/Qt6SvgConfig.cmake"

# Idempotence : si TOUT est deja build (qtbase + qtsvg), exit.
# Si qtbase present mais qtsvg manquant, on enchaine sur qtsvg seul
# (cas "ancien install" pre- a completer).
if [ -f "$MARKER_QTBASE" ] && [ -f "$MARKER_QTSVG" ]; then
    echo "==> Qt6 zig native deja installe (qtbase + qtsvg) : $INSTALL_DIR/install/"
    echo "    (supprime $INSTALL_DIR pour forcer un rebuild complet)"
    exit 0
fi

# Pre-checks
[ -d "$QT6_SRC" ] || {
    echo "ERROR: vendor/qt6/qtbase manquant." >&2
    echo "       Lance : git submodule update --init --recursive" >&2
    exit 2
}
[ -x "$ZIG_BIN" ] || {
    echo "==> zig non vendore, lance scripts/setup_zig.sh"
    bash "$ROOT/scripts/setup_zig.sh"
}
[ -f "$QT6_HOST_TOOLS/lib/cmake/Qt6/Qt6Config.cmake" ] || {
    echo "ERROR: Qt6 host tools manquants ($QT6_HOST_TOOLS)." >&2
    echo "       Lance : make qt6   # ~30-60 min, one-time" >&2
    exit 2
}

# Sur Linux : nettoyer le PATH des entries /mnt/c/* (Windows) qui pollueraient
# le cmake auto-detect avec des libs MSYS2 (path .lib + .h Windows).
# *-linux* couvre aussi x86_64-linux-musl (profil OWL_MUSL).
case "$HOST" in
    *-linux*)
        export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        ;;
esac

mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

# Skip qtbase si deja installe (cas reprise apres echec qtsvg).
if [ -f "$MARKER_QTBASE" ]; then
    echo "==> qtbase zig native deja installe, on enchaine sur qtsvg."
    SKIP_QTBASE=1
else
    SKIP_QTBASE=0
fi

# Note hardening : tentative d'ajout de -ffile-prefix-map + QT_NO_DEBUG_OUTPUT
# au CFLAGS/CXXFLAGS Qt6 a echoue (crash STATUS_ENTRYPOINT_NOT_FOUND sur
# PCRE2 JIT, incompat zig + flags sur submodules 3rdparty). Les 3 paths
# Qt6 qui leak dans owl.exe (qabstractspinbox.cpp, qdatetimeedit.cpp,
# _local/qt6/<host>/install) sont accepted comme limite documentee.

if [ "$SKIP_QTBASE" = "0" ]; then
    echo "==> Configure qtbase sous zig cc ($HOST), ~30 min..."
    CMAKE_ARGS=(
        -G Ninja
        -S "$QT6_SRC"
        -B build
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/install"
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DFEATURE_optimize_size=ON
        -DFEATURE_dbus=OFF
        -DFEATURE_sql=OFF
        -DFEATURE_network=OFF
        -DFEATURE_xml=OFF
        -DFEATURE_testlib=OFF
        -DFEATURE_concurrent=OFF
        -DBUILD_TESTING=OFF
        -DQT_HOST_PATH="$QT6_HOST_TOOLS"
    )
    case "$HOST" in
        *-linux-musl)
            # Universel statique : profil offscreen, tout bundled, zero dep
            # systeme (pas de fontconfig/xcb/opengl). INPUT_opengl=no evite la
            # report-error Qt "OpenGL functionality tests failed".
            CMAKE_ARGS+=(
                -DINPUT_opengl=no -DINPUT_xcb=no -DINPUT_fontconfig=no
                -DFEATURE_opengl=OFF -DFEATURE_xcb=OFF -DFEATURE_fontconfig=OFF
                -DFEATURE_openssl=OFF -DFEATURE_ssl=OFF
                -DFEATURE_system_zlib=OFF -DFEATURE_system_libpng=OFF
                -DFEATURE_system_libjpeg=OFF -DFEATURE_system_pcre2=OFF
                -DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF
                -DFEATURE_system_doubleconversion=OFF -DFEATURE_glib=OFF
                -DFEATURE_icu=OFF
            )
            ;;
        *-linux)
            CMAKE_ARGS+=(-DFEATURE_system_freetype=ON -DFEATURE_fontconfig=ON)
            ;;
        *-windows)
            CMAKE_ARGS+=(
                -DFEATURE_system_pcre2=OFF
                -DFEATURE_system_zlib=OFF
                -DFEATURE_system_freetype=OFF
                -DFEATURE_system_harfbuzz=OFF
                -DFEATURE_system_doubleconversion=OFF
            )
            ;;
    esac
    cmake "${CMAKE_ARGS[@]}"

    echo "==> Build + install qtbase..."
    cmake --build build --target install --parallel
    echo "==> qtbase zig native installe : $INSTALL_DIR/install/"
fi

# --------------------------------------------------------------------
# qtsvg : module Qt6 Svg pour QSvgRenderer ( catalogue icones).
# Build sur qtbase deja installe, install dans le meme prefix. ~5-10 min.
# FEATURE_svg_widget=OFF : on n'a pas besoin de QSvgWidget (qui requiert
# Qt6::Widgets). QSvgRenderer suffit pour rasteriser des SVG.
# --------------------------------------------------------------------
[ -d "$QTSVG_SRC" ] || {
    echo "ERROR: vendor/qt6/qtsvg manquant." >&2
    echo "       Lance : git submodule update --init --recursive" >&2
    exit 2
}

echo "==> Configure qtsvg sous zig cc ($HOST), ~5-10 min..."
mkdir -p "$INSTALL_DIR/build_qtsvg"
cmake \
    -G Ninja \
    -S "$QTSVG_SRC" \
    -B "$INSTALL_DIR/build_qtsvg" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_PREFIX_PATH="$INSTALL_DIR/install" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/install" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DFEATURE_svg_widget=OFF \
    -DQT_HOST_PATH="$QT6_HOST_TOOLS"

echo "==> Build + install qtsvg..."
cmake --build "$INSTALL_DIR/build_qtsvg" --target install --parallel
echo "==> qtsvg zig native installe : $INSTALL_DIR/install/"

echo "==> Qt6 (qtbase + qtsvg) zig native pret : $INSTALL_DIR/install/"
