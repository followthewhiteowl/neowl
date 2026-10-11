#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Telecharge zig 0.16.0 dans vendor/zig/<host_triple>/ (idempotent).
# Pre-requis pour la cross-compile via zig cc utilisee par owl-build (Phase B).
#
# Detecte le host courant (Linux x86_64 / Windows x86_64 via MSYS2/WSL) et
# DL le binaire zig correspondant. Le binaire est decompresse en place
# (~80 MB) pour usage par les wrappers scripts/zig_wrappers/.
set -e

ZIG_VERSION="${ZIG_VERSION:-0.16.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR_DIR="$ROOT/vendor/zig"

# Detecte host platform pour choisir l'archive
case "$(uname -s)" in
    Linux*)   HOST_OS="linux" ;;
    MSYS*|MINGW*|CYGWIN*) HOST_OS="windows" ;;
    Darwin*)  HOST_OS="macos" ;;
    *) echo "ERROR: unsupported OS $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) HOST_ARCH="x86_64" ;;
    arm64|aarch64) HOST_ARCH="aarch64" ;;
    *) echo "ERROR: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
HOST_TRIPLE="${HOST_ARCH}-${HOST_OS}"
ARCHIVE_EXT=$([ "$HOST_OS" = "windows" ] && echo "zip" || echo "tar.xz")
ARCHIVE_NAME="zig-${HOST_ARCH}-${HOST_OS}-${ZIG_VERSION}.${ARCHIVE_EXT}"
URL="https://ziglang.org/download/${ZIG_VERSION}/${ARCHIVE_NAME}"

INSTALL_DIR="$VENDOR_DIR/$HOST_TRIPLE"
ZIG_BIN="$INSTALL_DIR/zig"
[ "$HOST_OS" = "windows" ] && ZIG_BIN="$INSTALL_DIR/zig.exe"

# Step 1 : DL zig si absent (idempotent)
if [ -x "$ZIG_BIN" ]; then
    echo "==> zig deja installe : $ZIG_BIN ($("$ZIG_BIN" version))"
else
    echo "==> Download zig $ZIG_VERSION pour $HOST_TRIPLE..."
    mkdir -p "$VENDOR_DIR"
    cd "$VENDOR_DIR"
    rm -f "$ARCHIVE_NAME"
    curl -L -o "$ARCHIVE_NAME" "$URL"

    echo "==> Extract..."
    rm -rf "$HOST_TRIPLE" "zig-${HOST_ARCH}-${HOST_OS}-${ZIG_VERSION}"
    if [ "$HOST_OS" = "windows" ]; then
        unzip -q "$ARCHIVE_NAME"
    else
        tar xf "$ARCHIVE_NAME"
    fi
    mv "zig-${HOST_ARCH}-${HOST_OS}-${ZIG_VERSION}" "$HOST_TRIPLE"
    rm "$ARCHIVE_NAME"
    "$ZIG_BIN" version
    echo "==> Installed at: $INSTALL_DIR"
fi
cd "$ROOT"

# Step 2 : compile les wrappers exe natifs (zig_launcher.c) sur Windows.
# CMake doit invoquer zig sans passer par cmd/bash (qui corrompt les args
# avec quotes echappes). 4 binaires identiques (launcher detecte la
# sous-commande zig depuis son propre nom argv[0]).
# Idempotent : recompile si .exe absents ou .c plus recent.
if [ "$HOST_OS" = "windows" ]; then
    LAUNCHER_SRC="$ROOT/scripts/zig_wrappers/zig_launcher.c"
    WRAPPERS_DIR="$ROOT/scripts/zig_wrappers"
    NEED_REBUILD=0
    for w in zig-cc.exe zig-c++.exe zig-ar.exe zig-ranlib.exe; do
        if [ ! -x "$WRAPPERS_DIR/$w" ] || [ "$LAUNCHER_SRC" -nt "$WRAPPERS_DIR/$w" ]; then
            NEED_REBUILD=1
            break
        fi
    done
    if [ "$NEED_REBUILD" = "1" ]; then
        if command -v gcc >/dev/null 2>&1; then
            echo "==> Build wrappers exe natifs (zig_launcher.c)..."
            gcc -O2 "$LAUNCHER_SRC" -o "$WRAPPERS_DIR/zig-cc.exe" -lkernel32 -lshell32
            cp "$WRAPPERS_DIR/zig-cc.exe" "$WRAPPERS_DIR/zig-c++.exe"
            cp "$WRAPPERS_DIR/zig-cc.exe" "$WRAPPERS_DIR/zig-ar.exe"
            cp "$WRAPPERS_DIR/zig-cc.exe" "$WRAPPERS_DIR/zig-ranlib.exe"
            echo "==> wrappers exe builts (cc, c++, ar, ranlib)"
        else
            echo "==> ERROR: gcc absent (MSYS2 UCRT64 requis), wrappers .exe non compiles." >&2
            echo "    Le toolchain CMake zig sur Windows ne marchera pas." >&2
            exit 3
        fi
    else
        echo "==> wrappers exe deja a jour"
    fi
fi

# Step 3 : winapi_stubs (libsynchronization.a + libruntimeobject.a). Ces
# import libs MinGW sont absentes du sysroot zig MinGW. Versionees dans
# vendor/winapi_stubs/, on les copie au layout attendu par les wrappers
# (vendor/zig/winapi_stubs/). Plus de dependance MSYS2 pour cette etape.
if [ "$HOST_OS" = "windows" ]; then
    STUBS_DIR="$VENDOR_DIR/winapi_stubs"
    SRC_STUBS="$ROOT/vendor/winapi_stubs"
    if [ ! -f "$SRC_STUBS/libsynchronization.a" ] || [ ! -f "$SRC_STUBS/libruntimeobject.a" ]; then
        echo "==> ERROR: vendor/winapi_stubs/*.a manquants." >&2
        echo "    Lance : git submodule update --init --recursive" >&2
        exit 3
    fi
    mkdir -p "$STUBS_DIR"
    cp -u "$SRC_STUBS/libsynchronization.a" "$STUBS_DIR/"
    cp -u "$SRC_STUBS/libruntimeobject.a"   "$STUBS_DIR/"
    echo "==> winapi_stubs OK (depuis vendor/winapi_stubs/)"
fi

echo "==> setup_zig OK ($HOST_TRIPLE)"
