#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Build llama.cpp + ggml en CPU-only statique sous zig cc (clang + libc++).
# Meme toolchain que Qt6 / DuckDB : ABI libc++ coherente avec le runtime OWL
# (owl.exe est lui-meme compile via cmake/toolchain_zig_native.cmake).
#
# Output : <ROOT>/_local/llama/<host>/install/
#   lib/libllama.a  lib/libggml.a  lib/libggml-cpu.a  lib/libggml-base.a
#   include/llama.h include/llama-cpp.h include/ggml*.h include/gguf.h
#
# Le dossier _local/llama/<host> est gitignore. Lance ~1 fois par bump llama.
#
# Pre-requis :
#   - vendor/llama.cpp/ (sous-ensemble cure : ggml/ + src/llama* + include/,
#     backends GPU + examples/tests/tools/common elagues — cf vendor/VERSIONS.md)
#   - vendor/zig/<host>/zig (sinon : bash scripts/setup_zig.sh)
#
# Usage : bash scripts/build_llama_zig_native.sh
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

LLAMA_SRC="$ROOT/vendor/llama.cpp"
ZIG_BIN="$ROOT/vendor/zig/$HOST/zig$EXE_EXT"

# Profil musl (OWL_MUSL=1) : cible x86_64-linux-musl pour le binaire Linux
# statique universel (Alpine/scratch/distroless). Additif — n'affecte ni
# Windows ni le build glibc. Sortie dans _local/llama/x86_64-linux-musl.
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

INSTALL_DIR="$ROOT/_local/llama/$HOST"
MARKER="$INSTALL_DIR/install/.owl_install_complete"

# Idempotence
if [ -f "$MARKER" ]; then
    echo "==> llama.cpp zig native deja installe : $INSTALL_DIR/install/"
    echo "    (supprime $INSTALL_DIR pour forcer un rebuild)"
    exit 0
fi

# Pre-checks
[ -d "$LLAMA_SRC/src" ] || {
    echo "ERROR: vendor/llama.cpp manquant ou vide." >&2
    exit 2
}
[ -x "$ZIG_BIN" ] || {
    echo "==> zig non vendore, lance scripts/setup_zig.sh"
    bash "$ROOT/scripts/setup_zig.sh"
}

# Sur Linux : PATH propre, sans /mnt/c/* (Windows pollution sous WSL2).
case "$HOST" in
    *-linux*)
        export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        ;;
esac

mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

# Hardening applique des la compilation des .a (cf docs build_hardening.md) :
#   -fvisibility=hidden      : cache les internes ggml/llama
#   -ffile-prefix-map        : anonymise les paths source dev dans le binaire
#   -ffunction/-fdata-sections : permet a --gc-sections d'evacuer le non-consomme
#   -DNDEBUG                 : contrat Release (Release le pose deja, on aligne)
# zig clang plus strict que gcc : downgrade quelques warnings ggml en non-erreurs.
HARDEN="-fvisibility=hidden -ffile-prefix-map=$LLAMA_SRC=. -ffunction-sections -fdata-sections -DNDEBUG"
RELAX="-Wno-error=date-time -Wno-error=deprecated-declarations -Wno-error=unused-function"
export CFLAGS="$HARDEN $RELAX"
export CXXFLAGS="$HARDEN $RELAX"

echo "==> Configure llama.cpp CPU-only sous zig cc ($HOST)..."
# GGML_NATIVE=OFF : ne PAS detecter le CPU du host (-march=native casse la
#   portabilite Docker). La baseline ISA vient de la toolchain OWL qui force
#   -mcpu=x86_64_v2 (SSE4.2, portable 2009+), cf decision ISA portable-first.
# GGML_BACKEND_DL=OFF : backends lies en statique (pas de .so charge au runtime),
#   la registration CPU devient un appel direct sous #ifdef GGML_USE_CPU.
# LLAMA_BUILD_COMMON/APP/SERVER=OFF : ces add_subdirectory pointent sur des
#   dossiers elagues (common/, app/) — sans ces OFF, la config casse.
CMAKE_ARGS=(
    -G Ninja
    -S "$LLAMA_SRC"
    -B build
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/install"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DGGML_BACKEND_DL=OFF
    -DGGML_NATIVE=OFF
    -DGGML_CPU=ON
    # ISA portable-first : baseline x86_64_v2 (SSE4.2) STRICTE. GGML_NATIVE=OFF
    # ne suffit PAS — ggml active AVX2/FMA/F16C/BMI2/AVX par defaut sur x86, ce
    # qui ferait SIGILL sur tout CPU sans AVX2 (Docker/cloud baseline). On force
    # tout au-dessus de SSE4.2 a OFF pour un binaire qui tourne partout.
    -DGGML_SSE42=ON
    -DGGML_AVX=OFF
    -DGGML_AVX2=OFF
    -DGGML_FMA=OFF
    -DGGML_F16C=OFF
    -DGGML_BMI2=OFF
    -DGGML_AVX_VNNI=OFF
    -DGGML_AVX512=OFF
    -DGGML_AVX512_VBMI=OFF
    -DGGML_AVX512_VNNI=OFF
    -DGGML_AVX512_BF16=OFF
    -DGGML_CUDA=OFF
    -DGGML_METAL=OFF
    -DGGML_VULKAN=OFF
    -DGGML_OPENMP=OFF
    -DGGML_CURL=OFF
    -DGGML_RPC=OFF
    -DGGML_BLAS=OFF
    -DLLAMA_CURL=OFF
    -DLLAMA_OPENSSL=OFF
    -DLLAMA_BUILD_COMMON=OFF
    -DLLAMA_BUILD_APP=OFF
    -DLLAMA_BUILD_SERVER=OFF
    -DLLAMA_BUILD_EXAMPLES=OFF
    -DLLAMA_BUILD_TESTS=OFF
    -DLLAMA_BUILD_TOOLS=OFF
    # Pin cosmetique (pas de .git dans le sous-ensemble copie ; evite le
    # bruit git-vars qui interrogerait le repo OWL parent).
    -DLLAMA_BUILD_NUMBER=10442
    -DLLAMA_BUILD_COMMIT=9b0a2ce8
)
cmake "${CMAKE_ARGS[@]}"

echo "==> Build (cibles statiques CPU-only)..."
cmake --build build --parallel --target ggml-base ggml ggml-cpu llama

echo "==> Install manuel..."
INST="$INSTALL_DIR/install"
mkdir -p "$INST/lib" "$INST/include"

# Nommage amont incoherent : sous MinGW/zig, les cibles ggml produisent
# "ggml-base.a" (SANS prefixe lib), llama produit "libllama.a". On normalise
# vers lib<nom>.a canonique quel que soit le prefixe amont, pour un CMakeLists
# stable entre plateformes.
for base in ggml-base ggml ggml-cpu llama; do
    found="$(find build \( -name "lib${base}.a" -o -name "${base}.a" \) -print -quit)"
    [ -n "$found" ] || { echo "ERROR: (lib)${base}.a introuvable dans build/" >&2; exit 3; }
    cp "$found" "$INST/lib/lib${base}.a"
done
# .a auxiliaires ggml-cpu eventuels (llamafile sgemm, amx) selon les options.
find build \( -name "libggml*.a" -o -name "ggml-*.a" \) -exec sh -c '
    for f; do b=$(basename "$f"); case "$b" in lib*) n="$b";; *) n="lib$b";; esac
    [ -f "'"$INST"'/lib/$n" ] || cp "$f" "'"$INST"'/lib/$n"; done' _ {} +

# Headers publics
cp "$LLAMA_SRC/include/llama.h"      "$INST/include/"
cp "$LLAMA_SRC/include/llama-cpp.h"  "$INST/include/"
cp "$LLAMA_SRC/ggml/include/"*.h     "$INST/include/"

touch "$INST/.owl_install_complete"

echo "==> llama.cpp zig native installe : $INST/"
ls -la "$INST/lib/"
