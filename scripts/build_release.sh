#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# build_release.sh — Produit les 3 binaires distribuables de la release publique
# pour le host courant, en config OWL_RELEASE_DISTRIB (statique + surface
# publique seule : pas de --self-test-*, pas de snapshot ; cf. ).
#
# Genere dans release/ :
#   - owl[.exe]      interpreteur all-in-one (REPL/run/test/fmt/GUI/PDF/mcp)
#   - owl-lsp[.exe]  Language Server
#   - owl-dap[.exe]  Debug Adapter
#
# owl-build (packaging cross-compile) est HORS scope de la release publique :
# voir scripts/build_mega.sh pour ce binaire.
#
# Pre-requis (memes que build_mega.sh) : zig vendore + Qt6 zig native.
# Usage : bash scripts/build_release.sh
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
BUILD_DIR="$ROOT/_local/build/release_$HOST"
mkdir -p "$ROOT/release"

# Pre-checks (memes invariants que build_mega.sh)
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
QT_HOST_PATH=""
[ -d "$ROOT/_local/host_tools/qt6" ] && QT_HOST_PATH="$ROOT/_local/host_tools/qt6"

cd "$ROOT"

echo "==> [1/3] Configure + build owl + owl-lsp + owl-dap (release distrib)"
CMAKE_ARGS=(
    -G Ninja
    -S "$ROOT" -B "$BUILD_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_native.cmake"
    -DCMAKE_BUILD_TYPE=Release
    -DOWL_QT6_INSTALL="$QT6_INSTALL"
    -DOWL_BUILD_LSP=ON
    -DOWL_BUILD_DAP=ON
    -DOWL_DEV_TOOLS=OFF
    -DOWL_RELEASE_DISTRIB=ON
)
[ -n "$QT_HOST_PATH" ] && CMAKE_ARGS+=(-DQT_HOST_PATH="$QT_HOST_PATH")
cmake "${CMAKE_ARGS[@]}" > /dev/null
cmake --build "$BUILD_DIR" --target owl owl-lsp owl-dap

echo ""
echo "==> [2/3] Copie des binaires dans release/"
for exe in owl owl-lsp owl-dap; do
    cp "$BUILD_DIR/$exe$EXE_EXT" "$ROOT/release/$exe$EXE_EXT"
done

# Doc HTML offline livree A COTE de owl. Le plugin VSCode ouvre
# release/owl-doc.html relativement a l'exe (F1 sur un mot-cle). Genere avec le
# binaire release (registry version-matched), pas de chemin de build dans le HTML.
echo "==> [2b] Doc HTML offline -> release/owl-doc.html"
"$ROOT/release/owl$EXE_EXT" --dump-doc-html \
    "$ROOT/docs/reference/_owl-doc-template.html" > "$ROOT/release/owl-doc.html"

echo ""
echo "==> [3/3] Anonymisation : scrub du chemin de build absolu (machine)"
# Les libs vendorees (Qt6) peuvent baker le chemin de build absolu dans le
# binaire (qt_prfxpath, __FILE__). On l'efface : ce chemin n'existe pas sur la
# machine de l'utilisateur final, donc l'ecrasement est neutre a l'execution
# (Qt tombe sur ses plugins statiques). cf. scripts/scrub_build_paths.c.
SCRUB="$ROOT/_local/build/scrub_build_paths$EXE_EXT"
gcc -O2 -o "$SCRUB" "$ROOT/scripts/scrub_build_paths.c"
WINROOT="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"      # /chemin/vers/le/repo (Qt6 : slash)
WINROOT_BS="$(cygpath -w "$ROOT" 2>/dev/null || echo "$ROOT")"   # chemin Windows absolu (llama.cpp __FILE__ : backslash)
# Chemins absolus eventuellement bakes sous le profil utilisateur (ex. fixtures
# de test). Derives au build (jamais de nom en dur : ce script devient public).
HOME_M="$(cygpath -m "$USERPROFILE" 2>/dev/null || true)"        # C:/Users/<user>
HOME_W="$(cygpath -w "$USERPROFILE" 2>/dev/null || true)"        # C:\Users\<user>
for exe in owl owl-lsp owl-dap; do
    bin="$ROOT/release/$exe$EXE_EXT"
    "$SCRUB" "$bin" "$WINROOT"
    [ -n "$WINROOT_BS" ] && [ "$WINROOT_BS" != "$WINROOT" ] && "$SCRUB" "$bin" "$WINROOT_BS"
    [ -n "$HOME_M" ] && "$SCRUB" "$bin" "$HOME_M"
    [ -n "$HOME_W" ] && [ "$HOME_W" != "$HOME_M" ] && "$SCRUB" "$bin" "$HOME_W"
done

echo ""
echo "==> [4/4] Pre-chauffe antivirus : premier lancement des binaires"
# Windows Defender (protection temps-reel + behavior monitoring) scanne
# integralement un PE a son PREMIER lancement apres ecriture. Le scrub ci-dessus
# vient de reecrire les 3 binaires -> cache Defender invalide. Ce scan mesure
# ~10 s sur owl.exe (95 Mo) et ~1,5 s sur owl-lsp la premiere fois. Sans ce step,
# c'est l'utilisateur qui l'encaisse au 1er "Run" / a la 1re coloration VSCode
# apres chaque release. On le front-load ICI, pendant le build (machine, dev
# absent). Best-effort strict : jamais bloquant (|| true) et borne par timeout,
# une release ne doit pas echouer parce qu'un warmup a traine.
#   - owl.exe        : --version charge le PE puis sort.
#   - owl-lsp/owl-dap: lisent stdin (JSON-RPC / DAP) et sortent proprement sur
#                      EOF (</dev/null), donc apres chargement+scan du PE.
warm_run() { timeout 30 "$@" >/dev/null 2>&1 || true; }
WT0=$(date +%s)
warm_run "$ROOT/release/owl$EXE_EXT" --version
warm_run "$ROOT/release/owl-lsp$EXE_EXT" < /dev/null
warm_run "$ROOT/release/owl-dap$EXE_EXT" < /dev/null
echo "    Scan Defender front-loade en $(( $(date +%s) - WT0 )) s (au lieu de le refiler au dev)."

echo ""
echo "==> Done. Binaires release (host $HOST) :"
ls -la "$ROOT/release/owl$EXE_EXT" "$ROOT/release/owl-lsp$EXE_EXT" "$ROOT/release/owl-dap$EXE_EXT" \
    | awk '{printf "    %s  (%.1f MB)\n", $9, $5/1024/1024}'
echo ""
echo "    Verif surface : release/owl$EXE_EXT --self-test-registry  (doit etre inconnu)"
echo "                    release/owl$EXE_EXT help                  (pas de section interne)"
