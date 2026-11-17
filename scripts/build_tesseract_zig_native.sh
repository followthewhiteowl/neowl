#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Build Leptonica 1.85.0 + Tesseract 5.5.2 en statique (.a) sous zig cc
# (clang + libc++). Meme toolchain que Qt6 / DuckDB / llama.cpp : ABI libc++
# coherente avec le runtime OWL (owl.exe est lui-meme compile via
# cmake/toolchain_zig_native.cmake).
#
# Output : <ROOT>/_local/tesseract/<host>/install/
#   lib/libleptonica.a  lib/libtesseract55.a
#   include/leptonica/*.h  include/tesseract/*.h (dont version.h genere)
#
# Le dossier _local/tesseract/<host> est gitignore. Lance 1 fois par bump.
#
# Pre-requis :
#   - vendor/leptonica/ + vendor/tesseract/ (sous-ensembles cures, cf VERSIONS.md)
#   - vendor/zig/<host>/zig (sinon : bash scripts/setup_zig.sh)
#
# Usage : bash scripts/build_tesseract_zig_native.sh
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

LEPT_SRC="$ROOT/vendor/leptonica"
TESS_SRC="$ROOT/vendor/tesseract"
ZIG_BIN="$ROOT/vendor/zig/$HOST/zig$EXE_EXT"

# Profil musl (OWL_MUSL=1) : cible x86_64-linux-musl (Alpine/scratch/distroless),
# aligne sur le profil llama/DuckDB. Additif, n'affecte ni Windows ni le glibc.
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

INSTALL_DIR="$ROOT/_local/tesseract/$HOST"
INST="$INSTALL_DIR/install"
MARKER="$INST/.owl_install_complete"

# Idempotence
if [ -f "$MARKER" ]; then
    echo "==> tesseract/leptonica zig native deja installes : $INST/"
    echo "    (supprime $INSTALL_DIR pour forcer un rebuild)"
    exit 0
fi

# Pre-checks
[ -d "$LEPT_SRC/src" ] || { echo "ERROR: vendor/leptonica manquant ou vide." >&2; exit 2; }
[ -d "$TESS_SRC/src" ] || { echo "ERROR: vendor/tesseract manquant ou vide." >&2; exit 2; }
[ -x "$ZIG_BIN" ] || {
    echo "==> zig non vendore, lance scripts/setup_zig.sh"
    bash "$ROOT/scripts/setup_zig.sh"
}

# Sur Linux : PATH propre, sans /mnt/c/* (pollution Windows sous WSL2).
case "$HOST" in
    *-linux*) export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" ;;
esac

mkdir -p "$INSTALL_DIR"

# Hardening applique des la compilation des .a (cf docs build_hardening.md) :
#   -fvisibility=hidden      : cache les internes leptonica/tesseract
#   -ffile-prefix-map        : anonymise les paths source dev dans le binaire
#   -ffunction/-fdata-sections : permet a --gc-sections d'evacuer le non-consomme
#   -DNDEBUG                 : contrat Release (Release le pose deja, on aligne)
HARDEN_L="-fvisibility=hidden -ffile-prefix-map=$LEPT_SRC=. -ffunction-sections -fdata-sections -DNDEBUG"
HARDEN_T="-fvisibility=hidden -ffile-prefix-map=$TESS_SRC=. -ffunction-sections -fdata-sections -DNDEBUG"
# M_PI absent sous libc++/zig cible windows-gnu (baseapi.cpp, thresholder legacy)
# -> _USE_MATH_DEFINES pose les constantes math. Gotcha connue, 1 define.
MATH_DEF="-D_USE_MATH_DEFINES"

# ----------------------------------------------------------------------------
# Fix cross-compile specifique Tesseract (necessaire des que
# CMAKE_CROSSCOMPILING=TRUE, cas musl).
#
# NB : AR/RANLIB par-langage (CMAKE_<LANG>_COMPILER_AR/RANLIB, consultes par
# check_ipo_supported() / l'archivage thin-LTO) sont desormais poses par le
# toolchain partage (cmake/toolchain_zig_native.cmake, herite par le musl) pour
# toutes les libs cross. Plus de patch local ici.
#
# Cache-vars try_run (cross uniquement) : check_leptonica_tiff_support()
#    (Tesseract) lance un try_run() pour tester le support TIFF de Leptonica.
#    Interdit en cross (CMake ne peut pas EXECUTER un binaire cible) -> erreur
#    fatale "try_run() invoked in cross-compiling mode". On pre-remplit le
#    resultat : Leptonica est build SANS TIFF (codecs OFF ci-dessous), donc
#    pixWriteMemTiff renvoie non-zero -> DISABLE_TIFF, coherent avec le
#    -DDISABLE_TIFF=ON passe au configure Tesseract. Gate musl : en natif le
#    try_run s'execute reellement, on ne le masque pas.
TESS_TRYRUN_ARGS=()
if [ "${OWL_MUSL:-0}" = "1" ]; then
    TESS_TRYRUN_ARGS=(
        -DLEPT_TIFF_RESULT=1
        -DLEPT_TIFF_COMPILE_SUCCESS=TRUE
    )
fi

# ----------------------------------------------------------------------------
# 1) Leptonica 1.85.0 — TOUS CODECS OFF (decodage image delegue a stb cote OWL).
#    STRICT_CONF=OFF : une lib absente desactive le format sans casser le configure.
#    CMAKE_INSTALL_PREFIX au CONFIGURE (pas seulement a --install) : sinon
#    LeptonicaConfig.cmake pointe les include dirs sur Program Files.
# ----------------------------------------------------------------------------
echo "==> [1/2] Configure Leptonica (codecs OFF) sous zig cc ($HOST)..."
export CFLAGS="$HARDEN_L"
export CXXFLAGS="$HARDEN_L"
cmake -G Ninja -S "$LEPT_SRC" -B "$INSTALL_DIR/lept-build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_INSTALL_PREFIX="$INST" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_PROG=OFF \
    -DSW_BUILD=OFF \
    -DSTRICT_CONF=OFF \
    -DENABLE_ZLIB=OFF \
    -DENABLE_PNG=OFF \
    -DENABLE_GIF=OFF \
    -DENABLE_JPEG=OFF \
    -DENABLE_TIFF=OFF \
    -DENABLE_WEBP=OFF \
    -DENABLE_OPENJPEG=OFF
echo "==> Build + install Leptonica..."
cmake --build "$INSTALL_DIR/lept-build" --parallel --target leptonica
cmake --install "$INSTALL_DIR/lept-build"

# ----------------------------------------------------------------------------
# 2) Tesseract 5.5.2 — legacy + LSTM presents, exec --oem 1 (LSTM)
#    au runtime. Zero dep externe tiree (curl/archive/icu/openmp/graphics OFF).
#    SW_BUILD=OFF : sinon find_package(SW) echoue sur Windows.
#    CMAKE_PREFIX_PATH = install Leptonica -> find_package(Leptonica) le trouve.
# ----------------------------------------------------------------------------
echo "==> [2/2] Configure Tesseract (legacy+LSTM, deps OFF) sous zig cc ($HOST)..."
export CFLAGS="$HARDEN_T $MATH_DEF"
export CXXFLAGS="$HARDEN_T $MATH_DEF"
cmake -G Ninja -S "$TESS_SRC" -B "$INSTALL_DIR/tess-build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    "${TESS_TRYRUN_ARGS[@]}" \
    -DCMAKE_INSTALL_PREFIX="$INST" \
    -DCMAKE_PREFIX_PATH="$INST" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TRAINING_TOOLS=OFF \
    -DBUILD_TESTS=OFF \
    -DDISABLE_TIFF=ON \
    -DDISABLE_ARCHIVE=ON \
    -DDISABLE_CURL=ON \
    -DGRAPHICS_DISABLED=ON \
    -DOPENMP_BUILD=OFF \
    -DUSE_SYSTEM_ICU=OFF \
    -DSW_BUILD=OFF \
    -DENABLE_LTO=OFF
echo "==> Build Tesseract (lib seule, cible libtesseract)..."
cmake --build "$INSTALL_DIR/tess-build" --parallel --target libtesseract
# Install MANUEL (pas `cmake --install`) : les regles install() amont tirent la
# CLI tesseract.exe qu'on ne construit jamais (lib seule) -> l'install echouerait.
# On copie a la main le .a + les headers publics (dont version.h, genere dans le
# build), exactement comme build_llama_zig_native.sh.
echo "==> Install manuel Tesseract (.a + headers)..."
mkdir -p "$INST/lib" "$INST/include/tesseract"
cp "$TESS_SRC/include/tesseract/"*.h "$INST/include/tesseract/"
# version.h est genere dans l'arbre de build (friction connue) -> on le recupere.
TESS_VERSION_H="$(find "$INSTALL_DIR/tess-build" -name version.h -path '*tesseract*' -print -quit)"
[ -n "$TESS_VERSION_H" ] && cp "$TESS_VERSION_H" "$INST/include/tesseract/"

# ----------------------------------------------------------------------------
# Normalisation des noms de .a : CMake/MinGW produit "libtesseract55.a" ou
# "libleptonica.a". On verifie leur presence et on expose un alias stable
# libtesseract.a pour un CMakeLists consommateur portable.
# ----------------------------------------------------------------------------
LEPT_A="$(find "$INST/lib" -name 'libleptonica*.a' -print -quit)"
TESS_A="$(find "$INSTALL_DIR/tess-build" -name 'libtesseract*.a' -print -quit)"
[ -n "$LEPT_A" ] || { echo "ERROR: libleptonica.a introuvable dans l'install." >&2; exit 3; }
[ -n "$TESS_A" ] || { echo "ERROR: libtesseract*.a introuvable dans le build." >&2; exit 3; }
cp "$TESS_A" "$INST/lib/"
# Alias canonique (libtesseract55.a -> libtesseract.a) pour le link OWL.
cp "$TESS_A" "$INST/lib/libtesseract.a"

touch "$MARKER"

echo "==> Leptonica + Tesseract zig native installes : $INST/"
ls -la "$INST/lib/"
