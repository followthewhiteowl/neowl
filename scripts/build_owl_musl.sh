#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# build_owl_musl.sh — Produit le binaire Linux musl-static UNIVERSEL (Alpine,
# scratch, distroless, et toutes les distros glibc). Le pendant exact du .exe
# Windows : un seul fichier qui contient TOUT, y compris sa libc musl.
#
# Genere dans release/linux-musl/ :
#   - owl       interpreteur all-in-one (run/test/fmt/SQL/PDF offscreen/mcp...)
#   - owl-lsp   Language Server
#   - owl-dap   Debug Adapter
#
# Chaine additive : n'impacte NI Windows (UCRT -static) NI le build
# Linux-glibc. Cible x86_64-linux-musl via zig cc + toolchain_zig_musl.cmake.
# A LANCER DEPUIS LINUX / WSL2 (le build musl est intrinsequement Linux) — ce
# n'est PAS un target mingw32-make.
#
# Pre-requis (provisionnes automatiquement si absents) :
#   - vendor/zig/x86_64-linux/zig        (sinon scripts/setup_zig.sh)
#   - _local/qt6/x86_64-linux-musl/install       (sinon build_qt6_zig_native.sh OWL_MUSL=1)
#   - _local/duckdb/x86_64-linux-musl/install    (sinon build_duckdb_zig_native.sh OWL_MUSL=1)
#   - _local/host_tools/qt6              (Qt6 host glibc : moc/rcc/qmake ; via `make qt6`)
#
# Idempotent : relançable. Le build est incremental (build dir conserve) ;
# les prerequis lourds (Qt6/DuckDB musl) ne sont rebuild que s'ils manquent.
#
# Usage : bash scripts/build_owl_musl.sh
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# --- Garde plateforme ------------------------------------------------------
# musl-static est un binaire Linux ; le build lui-meme doit tourner sous Linux
# (zig cross-compile depuis Linux vers musl). Refus explicite ailleurs.
case "$(uname -s)" in
    Linux*) ;;
    *) echo "ERROR: build_owl_musl.sh doit tourner sous Linux/WSL2 (uname=$(uname -s))." >&2
       echo "       Le binaire musl-static est la cible Linux universelle (cf ADR_0142)." >&2
       exit 1 ;;
esac

# --- Chemins canoniques ----------------------------------------------------
MUSL_TRIPLE="x86_64-linux-musl"
QT6_MUSL="$ROOT/_local/qt6/$MUSL_TRIPLE/install"
DUCKDB_MUSL="$ROOT/_local/duckdb/$MUSL_TRIPLE/install"
BUILD_DIR="$ROOT/_local/build/release_$MUSL_TRIPLE"
OUT_DIR="$ROOT/release/linux-musl"

# --- Qt6 host tools : DOIVENT etre Linux (ELF) -----------------------------
# Le build musl tourne sous Linux/WSL2 -> moc/rcc/qmake/syncqt doivent etre des
# binaires LINUX. Piege connu (repo partage MSYS2<->WSL2) : le canonique
# _local/host_tools/qt6 peut contenir les host tools WINDOWS (.exe) si `make qt6`
# a tourne cote Windows -> syncqt.exe echoue sous WSL2 ("Unable to open rsp
# file: @"). On resout un host path Linux valide (ELF), sinon on exige
# OWL_QT_HOST_PATH explicite.
_is_linux_qt_host() {
    # $1 = prefix Qt6 host. moc est en libexec/ (Linux) ou bin/ (selon layout).
    local p="$1" moc
    for moc in "$p/libexec/moc" "$p/bin/moc"; do
        [ -f "$moc" ] || continue
        file -b "$moc" 2>/dev/null | grep -q "ELF" && { echo "$p"; return 0; }
    done
    return 1
}
QT6_HOST_TOOLS=""
# Ordre : override explicite, puis le host Linux fige (qt6-linux, ELF garanti,
# hors repo git via _local/), puis le canonique qt6 (souvent .exe Windows dans
# un repo partage MSYS2<->WSL2 -> rejete par _is_linux_qt_host).
for cand in "$OWL_QT_HOST_PATH" "$ROOT/_local/host_tools/qt6-linux" "$ROOT/_local/host_tools/qt6"; do
    [ -n "$cand" ] || continue
    if resolved="$(_is_linux_qt_host "$cand")"; then QT6_HOST_TOOLS="$resolved"; break; fi
done

# zig vendore (l'hote Linux produit le musl). Le wrapper zig-cc lit OWL_ZIG.
export OWL_ZIG="${OWL_ZIG:-$ROOT/vendor/zig/x86_64-linux/zig}"

# PATH propre : sous WSL2 le PATH herite du Windows (/mnt/c/...) et pollue le
# cmake auto-detect avec des libs MSYS2. On restreint au Linux pur (meme regle
# que build_qt6/duckdb_zig_native.sh).
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

echo "==> OWL musl-static universel — cible $MUSL_TRIPLE"
echo "    ROOT      : $ROOT"
echo "    OWL_ZIG   : $OWL_ZIG"
echo "    build dir : $BUILD_DIR"
echo "    output    : $OUT_DIR"
echo ""

# --- [0/4] Prerequis (provisioning idempotent) -----------------------------
echo "==> [0/4] Verification / provisioning des prerequis"

if [ ! -x "$OWL_ZIG" ]; then
    echo "    zig absent -> scripts/setup_zig.sh"
    bash "$ROOT/scripts/setup_zig.sh"
fi
[ -x "$OWL_ZIG" ] || { echo "ERROR: zig introuvable apres setup ($OWL_ZIG)." >&2; exit 2; }

# Qt6 host tools (moc/rcc/syncqt) : requis par le build Qt6-musl (cross) ET par
# le configure d'owl (moc sur ui_qt.cpp). DOIVENT etre Linux (ELF), cf resolver.
if [ -z "$QT6_HOST_TOOLS" ] || [ ! -f "$QT6_HOST_TOOLS/lib/cmake/Qt6/Qt6Config.cmake" ]; then
    # Self-heal : plus d'etape manuelle. On build le host Linux dans
    # _local/host_tools/qt6-linux via la cible dediee (PATH assaini). ~30-60 min,
    # une seule fois. Le canonique _local/host_tools/qt6 peut porter les .exe
    # Windows (repo partage MSYS2<->WSL2) -> on ne le touche pas.
    echo "    Host Qt6 Linux absent -> make qt6-linux (~30-60 min, une seule fois)" >&2
    ( cd "$ROOT" && make qt6-linux ) || {
        echo "ERROR: make qt6-linux a echoue (voir ci-dessus)." >&2; exit 2; }
    if resolved="$(_is_linux_qt_host "$ROOT/_local/host_tools/qt6-linux")"; then
        QT6_HOST_TOOLS="$resolved"
    fi
    [ -n "$QT6_HOST_TOOLS" ] && [ -f "$QT6_HOST_TOOLS/lib/cmake/Qt6/Qt6Config.cmake" ] || {
        echo "ERROR: host Qt6 Linux toujours introuvable apres make qt6-linux." >&2
        echo "       Override possible : OWL_QT_HOST_PATH=<prefix-qt6-host-linux>" >&2
        exit 2; }
fi
export OWL_QT_HOST_PATH="$QT6_HOST_TOOLS"   # propage aux sub-builds Qt6/DuckDB
echo "    Qt6 host tools (Linux) : $QT6_HOST_TOOLS"

# DuckDB-musl : rebuild si le marker d'install manque.
if [ ! -f "$DUCKDB_MUSL/.owl_install_complete" ]; then
    echo "    DuckDB-musl absent -> build_duckdb_zig_native.sh (OWL_MUSL=1, ~15 min)"
    OWL_MUSL=1 bash "$ROOT/scripts/build_duckdb_zig_native.sh"
fi
[ -f "$DUCKDB_MUSL/.owl_install_complete" ] || {
    echo "ERROR: DuckDB-musl introuvable apres build ($DUCKDB_MUSL)." >&2; exit 2; }

# Qt6-musl : rebuild si le Qt6Config.cmake (qtbase) manque.
if [ ! -f "$QT6_MUSL/lib/cmake/Qt6/Qt6Config.cmake" ]; then
    echo "    Qt6-musl absent -> build_qt6_zig_native.sh (OWL_MUSL=1, ~40 min)"
    OWL_MUSL=1 bash "$ROOT/scripts/build_qt6_zig_native.sh"
fi
[ -f "$QT6_MUSL/lib/cmake/Qt6/Qt6Config.cmake" ] || {
    echo "ERROR: Qt6-musl introuvable apres build ($QT6_MUSL)." >&2; exit 2; }

echo "    Prerequis OK."
echo ""

# --- [1/4] Configure + build ----------------------------------------------
echo "==> [1/4] Configure + build owl + owl-lsp + owl-dap (musl-static)"
cd "$ROOT"
CMAKE_ARGS=(
    -G Ninja
    -S "$ROOT" -B "$BUILD_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_musl.cmake"
    -DOWL_MUSL_STATIC=ON
    -DOWL_ZIG_MUSL_TARGET="$MUSL_TRIPLE"
    -DCMAKE_BUILD_TYPE=Release
    -DOWL_QT6_INSTALL="$QT6_MUSL"
    -DOWL_DUCKDB_INSTALL="$DUCKDB_MUSL"
    -DQT_HOST_PATH="$QT6_HOST_TOOLS"
    -DOWL_BUILD_LSP=ON
    -DOWL_BUILD_DAP=ON
    -DOWL_DEV_TOOLS=OFF
    -DOWL_RELEASE_DISTRIB=ON
)
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --target owl owl-lsp owl-dap owl_distrib

# Mode check (make musl-check) : la compile ci-dessus est le seul geste qui
# attrape les regressions de portabilite musl (le chemin Linux/musl n'est PAS
# dans le CI -> il rote en silence sinon, cf. binaire 1.0.6 shippe par erreur
# en 1.0.7/1.0.8). On s'arrete ici : NON-destructif (pas de depot dans release/,
# pas de strip/scrub).
if [ "${OWL_MUSL_CHECK:-0}" = "1" ]; then
    echo ""
    echo "==> musl-check : compile musl OK (owl/owl-lsp/owl-dap). Rien depose."
    exit 0
fi

# --- [2/4] Depot des binaires + strip -------------------------------------
echo ""
echo "==> [2/4] Depot dans $OUT_DIR + strip"
mkdir -p "$OUT_DIR"
for exe in owl owl-lsp owl-dap; do
    cp "$BUILD_DIR/$exe" "$OUT_DIR/$exe"
done
# strip --strip-all : retire toutes les tables de symboles/debug. Le binaire
# musl-static est destine au deploiement (conteneur/serveur), pas au debug.
for exe in owl owl-lsp owl-dap; do
    strip --strip-all "$OUT_DIR/$exe"
done

# --- [3/4] Anonymisation : scrub du chemin de build absolu -----------------
echo ""
echo "==> [3/4] Anonymisation : scrub du chemin de build baked (Qt6)"
# Qt6 bake son prefix d'install (qt_prfxpath) dans le binaire -> le path de dev
# .../_local/qt6/x86_64-linux-musl/install/lib/fonts fuite (et n'existe pas en
# prod). On l'ecrase par des '/' (meme longueur, offsets preserves, binaire
# valide) : Qt tombe sur ses fontes embarquees via qrc au runtime, l'ecrasement
# est neutre a l'execution. cf. scripts/scrub_build_paths.c (meme approche que
# build_release.sh cote Windows/glibc). NB : le warning Qt "Cannot find font
# directory" subsiste (Qt cherche un dossier fonts qui n'existe pas), mais il
# est non-bloquant (fontes Liberation embarquees) et ne fuit plus de path dev.
SCRUB="$BUILD_DIR/scrub_build_paths"
"$OWL_ZIG" cc -O2 -o "$SCRUB" "$ROOT/scripts/scrub_build_paths.c"
for exe in owl owl-lsp owl-dap; do
    "$SCRUB" "$OUT_DIR/$exe" "$ROOT"
done

# --- [4/4] Recap -----------------------------------------------------------
echo ""
echo "==> [4/4] Verification autonomie + tailles"
for exe in owl owl-lsp owl-dap; do
    kind="$(file -b "$OUT_DIR/$exe" 2>/dev/null || echo '?')"
    printf "    %-9s %s\n" "$exe" "$kind"
done
echo ""
echo "    'not a dynamic executable' attendu (ldd) :"
ldd "$OUT_DIR/owl" 2>&1 | head -1 | sed 's/^/      /' || true
echo ""
ls -la "$OUT_DIR"/owl "$OUT_DIR"/owl-lsp "$OUT_DIR"/owl-dap \
    | awk '{printf "    %s  (%.1f MB)\n", $9, $5/1024/1024}'
echo ""
echo "==> Done. Binaires musl-static universels : $OUT_DIR/"
echo "    Validation conteneur : chroot Alpine + owl --version / SQL / PDF offscreen."
