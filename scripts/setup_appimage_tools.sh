#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Telecharge appimagetool + linuxdeploy dans ~/bin (idempotent).
# Pre-requis pour scripts/build_appimage.sh.
set -e

mkdir -p "$HOME/bin"
cd "$HOME/bin"

if [[ ! -x ./appimagetool ]]; then
    echo "==> Downloading appimagetool..."
    curl -L -o appimagetool \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x appimagetool
fi

if [[ ! -x ./linuxdeploy ]]; then
    echo "==> Downloading linuxdeploy..."
    curl -L -o linuxdeploy \
        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x linuxdeploy
fi

echo ""
echo "==> Versions installees :"
./appimagetool --version 2>&1 | head -1
./linuxdeploy --version  2>&1 | head -1
echo ""
echo "==> Pret. Lance scripts/build_appimage.sh pour produire l'AppImage."
