#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Build DuckDB statique sous zig cc (libc++) avec core_functions + parquet +
# json + icu lies en interne. Equivalent du build Qt6 zig : ABI libc++
# coherente avec le reste du runtime OWL.
#
# Output : <ROOT>/_local/duckdb/<host>/install/lib/cmake/DuckDB/DuckDBConfig.cmake
# + libduckdb_static.a + libcore_functions_extension.a + libparquet_extension.a
# + libjson_extension.a + libicu_extension.a
#
# Le dossier _local/duckdb/<host> est gitignore. Lance ~1 fois par bump DuckDB.
#
# Pre-requis :
#   - vendor/duckdb/source clone et pinne v1.4.4 (submodule)
#   - vendor/zig/<host>/zig (sinon : bash scripts/setup_zig.sh)
#
# Usage : bash scripts/build_duckdb_zig_native.sh
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

DUCKDB_SRC="$ROOT/vendor/duckdb/source"
ZIG_BIN="$ROOT/vendor/zig/$HOST/zig$EXE_EXT"

# Profil musl (OWL_MUSL=1) : cible x86_64-linux-musl pour le binaire Linux
# statique universel. Additif — n'affecte ni Windows ni le build glibc.
# Sortie dans _local/duckdb/x86_64-linux-musl (distincte du build glibc),
# via la toolchain musl (isolation du sysroot glibc de l'hote).
TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_native.cmake"
if [ "${OWL_MUSL:-0}" = "1" ]; then
    case "$HOST" in
        *-linux) ;;
        *) echo "ERROR: OWL_MUSL=1 uniquement supporte sur hote Linux (host=$HOST)" >&2; exit 1 ;;
    esac
    HOST="x86_64-linux-musl"
    TOOLCHAIN_FILE="$ROOT/cmake/toolchain_zig_musl.cmake"
    echo "==> Profil MUSL actif : cible $HOST, toolchain $(basename "$TOOLCHAIN_FILE")"
fi

INSTALL_DIR="$ROOT/_local/duckdb/$HOST"
MARKER="$INSTALL_DIR/install/.owl_install_complete"

# Idempotence
if [ -f "$MARKER" ]; then
    echo "==> DuckDB zig native deja installe : $INSTALL_DIR/install/"
    echo "    (supprime $INSTALL_DIR pour forcer un rebuild)"
    exit 0
fi

# Pre-checks
[ -d "$DUCKDB_SRC/src" ] || {
    echo "ERROR: vendor/duckdb/source manquant ou vide." >&2
    echo "       Lance : git submodule update --init --recursive vendor/duckdb/source" >&2
    exit 2
}
[ -x "$ZIG_BIN" ] || {
    echo "==> zig non vendore, lance scripts/setup_zig.sh"
    bash "$ROOT/scripts/setup_zig.sh"
}

# Sur Linux : PATH propre, sans /mnt/c/* (Windows pollution sous WSL2).
# *-linux* couvre aussi x86_64-linux-musl (profil OWL_MUSL).
case "$HOST" in
    *-linux*)
        export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        ;;
esac

mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

# zig clang plus strict que gcc — DuckDB compile en -Werror et trip sur :
#   -Wdate-time (pcg_extras.hpp utilise __DATE__/__TIME__ macros)
#   -Wdeprecated-declarations (vieux APIs Windows)
# On downgrade ces warnings en non-erreurs, sans toucher au reste.
export CFLAGS="-Wno-error=date-time -Wno-error=deprecated-declarations -DDUCKDB_STATIC_BUILD"
export CXXFLAGS="-Wno-error=date-time -Wno-error=deprecated-declarations -DDUCKDB_STATIC_BUILD"

echo "==> Configure DuckDB sous zig cc ($HOST), ~10-20 min..."
# BUILD_EXTENSIONS = liste semi-colon separee. core_functions porte SUM/MIN/
# MAX/AVG + 100+ scalaires. parquet/json/icu pour lecture/ecriture de
# fichiers et timezones.
#
# CMAKE_DISABLE_FIND_PACKAGE_Python3 : neutralise Python. DuckDB CMake invoque
# Python via custom_target ALL (create_local_extension_repo.py) pour generer
# un dossier .duckdb_extension repository qu'on n'utilise pas (on link static).
# Sans Python, le target tombe dans la branche "vide" et le build est propre.
CMAKE_ARGS=(
    -G Ninja
    -S "$DUCKDB_SRC"
    -B build
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/install"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DBUILD_BENCHMARKS=OFF
    -DENABLE_SANITIZER=OFF
    -DENABLE_UBSAN=OFF
    -DEXTENSION_STATIC_BUILD=1
    -DBUILD_EXTENSIONS="core_functions;parquet;json;icu"
    -DDISABLE_EXTENSION_LOAD=OFF
    -DBUILD_COMPLETE_EXTENSION_SET=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE
)
cmake "${CMAKE_ARGS[@]}"

# Build seulement les cibles statiques. On EXCLUT les cibles
# *_loadable_extension (.duckdb_extension shared lib) car leur link utilise
# -Wl,--exclude-libs,ALL non supporte par lld-link sous Windows+Clang.
# Comme on link nos .a directement dans owl.exe, les shared sont inutiles.
echo "==> Build (cibles statiques uniquement)..."
cmake --build build --parallel --target \
    duckdb_static \
    core_functions_extension \
    parquet_extension \
    json_extension \
    icu_extension

# Install manuel : recopie les .a + headers necessaires dans install/.
echo "==> Install manuel..."
INST="$INSTALL_DIR/install"
mkdir -p "$INST/lib" "$INST/include"

# Main lib + extensions
cp build/src/libduckdb_static.a "$INST/lib/"
cp build/extension/core_functions/libcore_functions_extension.a "$INST/lib/"
cp build/extension/parquet/libparquet_extension.a "$INST/lib/"
cp build/extension/json/libjson_extension.a "$INST/lib/"
cp build/extension/icu/libicu_extension.a "$INST/lib/"

# Sur Linux (et macOS), DuckDB build aussi libjemalloc_extension.a en
# meme temps que les extensions standards. libduckdb_static.a y refere
# des symboles JemallocExtension::* — il faut donc linker cette lib
# sinon le link final d'owl-cli (ou owl natif) crashe sur ~6 symboles
# unresolved. Cf CMakeLists owl_duckdb INTERFACE qui detecte la
# presence du fichier et l'ajoute conditionnellement.
if [ -f build/extension/jemalloc/libjemalloc_extension.a ]; then
    cp build/extension/jemalloc/libjemalloc_extension.a "$INST/lib/"
fi

# Third-party libs internes referencees par duckdb_static
find build/third_party -name "libduckdb_*.a" -exec cp {} "$INST/lib/" \;
find build/extension/icu/third_party -name "libicu_*.a" 2>/dev/null -exec cp {} "$INST/lib/" \;

# Headers publics
cp "$DUCKDB_SRC/src/include/duckdb.h" "$INST/include/" 2>/dev/null || true
cp "$DUCKDB_SRC/src/include/duckdb.hpp" "$INST/include/" 2>/dev/null || true
cp "$DUCKDB_SRC/src/include/duckdb_extension.h" "$INST/include/" 2>/dev/null || true

# Rename mbedtls_* -> duckdb_mbedtls_* dans libduckdb_mbedtls.a pour eviter
# le conflit avec owl_mbedtls.a au link final dans owl.exe. Les wrappers C++
# duckdb_mbedtls::* sont deja namespaces, c'est juste les symboles C bruts
# (~229) qui collisionnent.
echo "==> Rename mbedtls_* dans libduckdb_mbedtls.a..."
bash "$ROOT/scripts/rename_duckdb_mbedtls.sh" "$INST/lib/libduckdb_mbedtls.a"

# Marker file pour idempotence du script
touch "$INST/.owl_install_complete"

echo "==> DuckDB zig native installe : $INST/"
ls -la "$INST/lib/" | head -20
