#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Setup toolchain Linux pour build OWL natif (depuis WSL2 ou Linux natif).
#
# A lancer DANS WSL2 ou Linux, depuis la racine du repo OWL :
#   bash scripts/setup_linux.sh
#
# Installe :
#   - build essentials (gcc, cmake, ninja, make, perl, python3, git)
#   - Qt6 build deps (X11, OpenGL, fontconfig, etc.) — pour le build de
#     vendor/qt6/qtbase from source
#   - libqpdf-dev (PDF post-traitement, dynamic link)
#
# Apres setup, lancer :
#   make qt6      # ~30-60 min, build Qt6 from vendor/qt6/qtbase
#   make          # build owl natif Linux
#   ./build/owl test
#
# ----------------------------------------------------------------------
# IMPORTANT — WSL2 sur PC corporate avec proxy/firewall
# ----------------------------------------------------------------------
# Si apt update echoue avec "Network is unreachable", c'est que WSL2 utilise
# son NAT par defaut, bloque par le firewall corporate. Activer le mode
# "mirrored networking" qui fait que WSL utilise les interfaces reseau de
# Windows directement (et heritera du proxy/firewall). Cote Windows, creer
# C:\Users\<user>\.wslconfig avec :
#
#     [wsl2]
#     networkingMode=mirrored
#     dnsTunneling=true
#     autoProxy=true
#
# Puis depuis PowerShell :  wsl --shutdown
# Au prochain wsl --distribution ..., le reseau marchera.
# Necessite WSL >= 2.0 (verifier avec `wsl --version`).
set -e

if ! command -v apt-get &>/dev/null; then
    echo "Ce script utilise apt (Debian/Ubuntu). Adapter pour autre distro."
    exit 1
fi

echo "==> Installation des paquets de base..."
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build \
    git perl python3 \
    libqpdf-dev

echo "==> Installation des deps Qt6 build (X11, OpenGL, etc.)..."
sudo apt-get install -y \
    libgl1-mesa-dev libegl1-mesa-dev \
    libxkbcommon-dev libxkbcommon-x11-dev \
    libfontconfig1-dev libfreetype6-dev \
    libx11-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev \
    libxcb1-dev libx11-xcb-dev \
    libxcb-glx0-dev libxcb-keysyms1-dev libxcb-image0-dev libxcb-shm0-dev \
    libxcb-icccm4-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-shape0-dev \
    libxcb-randr0-dev libxcb-render-util0-dev libxcb-util-dev \
    libxcb-xinerama0-dev libxcb-xkb-dev libxcb-cursor-dev

echo "==> Verification toolchain..."
gcc --version | head -1
cmake --version | head -1
ninja --version
perl --version | head -2 | tail -1
python3 --version

echo ""
echo "==> Setup termine."
echo "    Prochaines etapes :"
echo "      1. Recuperer Qt6 source (submodule git pinned a v6.10.0) :"
echo "           git submodule update --init --recursive"
echo "         Note: le checkout se fait dans vendor/qt6/qtbase. Le filesystem"
echo "         /mnt/c (Plan9) est tres lent pour le build Qt (10MB/min). Pour"
echo "         Linux il est recommande de cloner aussi dans le home WSL :"
echo "           git clone --branch v6.10.0 https://code.qt.io/qt/qtbase.git ~/qtbase"
echo "         (la version a aligner avec le submodule, cf .gitmodules)"
echo "      2. make qt6   # ~30-60 min one-time"
echo "      3. make       # build owl natif"
echo "      4. ./build/owl test"
echo ""
echo "    Pour produire un AppImage Linux portable (Phase 2) :"
echo "      bash scripts/setup_appimage_tools.sh   # download outils (~35 MB)"
echo "      bash scripts/build_appimage.sh         # genere dist/owl-*.AppImage"
