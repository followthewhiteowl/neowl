#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Assemble le layout de distribution owl-build dans dist/owl-build-<host>/
# qui contient tout pour produire des binaires natifs et cross :
#
#   dist/owl-build-<host>/
#   ├── owl-build[.exe]            (l'orchestrateur, ~5 MB)
#   ├── zig/<host>/zig[.exe]       (compilateur LLVM, ~80 MB)
#   ├── zig/winapi_stubs/*.a       (libs Win API absentes du sysroot zig)
#   ├── zig/wrappers/*.exe         (Windows uniquement)
#   ├── include/owl/*.h            (~200 KB)
#   └── lib/<target>/*.a + cfg     (un sous-dossier par target supportee)
#
# Usage : bash scripts/package_dist.sh [BUILD_DIR]
#   BUILD_DIR par defaut : build_zig (Windows) ou build_owl_zig (Linux)
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in
    Linux*)   HOST="x86_64-linux" ;;
    MSYS*|MINGW*|CYGWIN*) HOST="x86_64-windows" ;;
    Darwin*)
        if [ "$(uname -m)" = "arm64" ]; then HOST="aarch64-macos"
        else HOST="x86_64-macos"; fi ;;
esac

BUILD_DIR="${1:-${BUILD_DIR:-}}"
if [ -z "$BUILD_DIR" ]; then
    if [ "$HOST" = "x86_64-windows" ]; then BUILD_DIR="$ROOT/build_zig"
    else BUILD_DIR="$ROOT/build_owl_zig"; fi
fi
[ -d "$BUILD_DIR" ] || { echo "ERROR: BUILD_DIR introuvable : $BUILD_DIR" >&2; exit 2; }

DIST="$ROOT/_local/build/release-stage/$HOST"
echo "==> Cleaning $DIST"
rm -rf "$DIST"
mkdir -p "$DIST/zig" "$DIST/include" "$DIST/lib"

# Relocalise un cfg de link : remplace les prefixes absolus de deps (lus dans
# les lignes *_ROOT_DEFAULT du cfg) par les placeholders @QT6_ROOT@/@DUCKDB_ROOT@/
# @LLAMA_ROOT@, puis vide les valeurs DEFAULT. owl-build resout les placeholders
# a l'extraction (probe <owlRoot>/<dep>/<target>). Garantit ZERO chemin absolu
# de build dans le mega (relocalisation + anti-fuite), quel que soit l'etat du
# cfg genere (certaines cibles n'ont pas la passe placeholder au build, ex. musl).
# No-op si deja placeholderise / DEFAULT vide. Idempotent.
relocatabilize_cfg() {
    local cfg="$1" name var val ph
    for name in QT6 DUCKDB LLAMA; do
        var="${name}_ROOT_DEFAULT"
        ph="@${name}_ROOT@"
        val=$(grep "^${var}=" "$cfg" | head -1 | sed "s#^${var}=##")
        if [ -n "$val" ] && [ "$val" != "$ph" ]; then
            # Remplace toutes les occurrences du prefixe absolu par le placeholder.
            sed -i "s#${val}#${ph}#g" "$cfg"
        fi
        # Vide la valeur DEFAULT (sinon elle reintroduit un chemin/placeholder).
        sed -i "s#^${var}=.*#${var}=#" "$cfg"
    done
}

# 1. owl-build binaire
EXE_EXT=""
[ "$HOST" = "x86_64-windows" ] && EXE_EXT=".exe"
echo "==> Copy owl-build$EXE_EXT"
cp "$BUILD_DIR/owl-build$EXE_EXT" "$DIST/"

# 2. zig (binaire + lib + winapi_stubs)
echo "==> Copy zig $HOST (~80 MB)"
mkdir -p "$DIST/zig/$HOST"
cp -r "$ROOT/vendor/zig/$HOST"/* "$DIST/zig/$HOST/"
if [ -d "$ROOT/vendor/zig/winapi_stubs" ]; then
    cp -r "$ROOT/vendor/zig/winapi_stubs" "$DIST/zig/"
fi
# Wrappers exe Windows uniquement
if [ "$HOST" = "x86_64-windows" ]; then
    mkdir -p "$DIST/zig/wrappers"
    cp "$ROOT/scripts/zig_wrappers"/*.exe "$DIST/zig/wrappers/" 2>/dev/null || true
fi

# 3. headers OWL
echo "==> Copy include/owl"
cp -r "$BUILD_DIR/include/owl" "$DIST/include/"

# 4. Qt6 install par target. Permet de relocaliser via @QT6_ROOT@ :
# l'owl-build cherchera <DIST>/qt6/<target>/ pour chaque cible.
# Layout source : _local/qt6/<host>/install/ (canon depuis chantier reorg).
#
# owl-build ne consomme QUE lib/ (libs .a + objects-Release/*.o) et plugins/
# (.a + _init .o) — cf. OBJECTS/LIBS_ABS du cfg. On stage donc UNIQUEMENT ces
# deux arbres. Le reste de l'install Qt6 est du poids mort ET nuisible :
#   - bin/ : qmake, qmake6, qtpaths... dont des SYMLINKS versionnes que
#     l'extracteur megafile ne sait pas poser sous Windows -> extraction KO.
#   - include/ mkspecs/ lib/cmake/ libexec/ doc/ : inutiles au link (aucun
#     compile Qt ni find_package cote owl-build), poids + chemins de build bakes.
# Gain : extraction fiable + taille reduite + moins de surface de fuite.
stage_qt6() {  # $1 = install source, $2 = dossier target de destination
    local src="$1" dst="$2"
    [ -d "$src" ] || return 0
    mkdir -p "$dst"
    cp -r "$src/lib" "$dst/" 2>/dev/null || true
    [ -d "$src/plugins" ] && { cp -r "$src/plugins" "$dst/" 2>/dev/null || true; }
    # Purge le poids mort residuel sous lib/ (non reference par le cfg).
    rm -rf "$dst/lib/cmake" "$dst/lib/metatypes" "$dst/lib/pkgconfig" 2>/dev/null || true
    find "$dst/lib" -maxdepth 1 -type f \( -name '*.prl' -o -name '*.pri' -o -name '*.pc' \) -delete 2>/dev/null || true
}
QT6_LINUX="$ROOT/_local/qt6/x86_64-linux-musl/install"
QT6_WIN="$ROOT/_local/qt6/x86_64-windows/install"
if [ -d "$QT6_LINUX" ]; then
    echo "==> Copy qt6/linux-x86_64 (lib + plugins)"
    stage_qt6 "$QT6_LINUX" "$DIST/qt6/linux-x86_64"
fi
if [ -d "$QT6_WIN" ]; then
    echo "==> Copy qt6/windows-x86_64 (lib + plugins)"
    stage_qt6 "$QT6_WIN" "$DIST/qt6/windows-x86_64"
fi

# 4b. DuckDB + llama install (lib/) par target. Relocalises via @DUCKDB_ROOT@ /
# @LLAMA_ROOT@ : owl-build cherche <DIST>/<name>/<target>/lib/*.a. Sans ca, le
# link spec reference les .a par chemin d'arbre de build -> owl-build casse hors
# machine de build. Seul lib/ est requis au link (include/ inutile ici).
for dep in duckdb llama; do
    for pair in "linux-x86_64:x86_64-linux-musl" "windows-x86_64:x86_64-windows"; do
        tgt="${pair%%:*}"; host="${pair##*:}"
        src="$ROOT/_local/$dep/$host/install/lib"
        if [ -d "$src" ]; then
            echo "==> Copy $dep/$tgt/lib"
            mkdir -p "$DIST/$dep/$tgt/lib"
            cp "$src"/*.a "$DIST/$dep/$tgt/lib/" 2>/dev/null || true
        fi
    done
done

# 4. libs OWL par target. Sources multiples (cross-compile) :
#   - $BUILD_DIR               : build hote courant (ex. windows-x86_64)
#   - release_x86_64-linux-musl : build linux musl (produit par make release-linux
#     via WSL). Permet qu'un owl-build windows cible linux (et inversement).
# Chaque cfg staged est relocalise (placeholders + DEFAULT vides) : zero chemin
# absolu de build dans le mega.
LINUX_BUILD_DIR="$ROOT/_local/build/release_x86_64-linux-musl"

# Garde-fou de staleness. Les libs owl linux viennent d'un build separe
# (release-linux via WSL) que mega NE relance PAS : toute modif d'owl_core
# les perime silencieusement, et le cross-build linux casse plus tard sur un
# symbole manquant. Si la lib linux existe mais est plus vieille qu'une source
# de src/, on refuse ici plutot que de stager une cible morte. Si LINUX_BUILD_DIR
# est absent (mega sans cible linux), on ne fait rien : comportement inchange.
LIBOWL_LINUX="$LINUX_BUILD_DIR/lib/linux-x86_64/libowl_core.a"
if [ -f "$LIBOWL_LINUX" ]; then
    if [ -n "$(find "$ROOT/src" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer "$LIBOWL_LINUX" -print -quit)" ]; then
        echo "package_dist: libs owl linux perimees (plus anciennes que src/) — relance 'make release-linux' avant 'make mega'." >&2
        exit 1
    fi
fi

for bd in "$BUILD_DIR" "$LINUX_BUILD_DIR"; do
    [ -d "$bd/lib" ] || continue
    for triple_dir in "$bd/lib"/*/ ; do
        [ -d "$triple_dir" ] || continue
        triple=$(basename "$triple_dir")
        [ -d "$DIST/lib/$triple" ] && continue   # deja stage depuis un autre build dir
        echo "==> Copy lib/$triple (depuis $(basename "$bd"))"
        mkdir -p "$DIST/lib/$triple"
        cp "$triple_dir"/*.a "$DIST/lib/$triple/" 2>/dev/null || true
        cp "$triple_dir"/owl_link_gui.cfg "$DIST/lib/$triple/" 2>/dev/null || true

        # 4c. Stage a plat TOUTES les libs que le cfg reference dans LIBS_REL.
        # owl-build les resout en -lNAME via -L lib/<triple>, donc chaque .a doit
        # etre present a plat ici. Deux familles dans l'arbre de build :
        #   - libowl_*.a sous-libs     -> <bd>/libowl_X.a (racine du build ;
        #     lib/<triple>/ n'en contient qu'un sous-ensemble)
        #   - vendorees chemin d'arbre  -> <bd>/vendor/.../libX.a (qpdf, zstd,
        #     zlib/libz, jpeg... ; noms variables par OS)
        # Resolution contre CE build dir (source de verite = le cfg du target,
        # auto-maintenu : toute lib ajoutee au link owl est stagee sans edit).
        cfg="$DIST/lib/$triple/owl_link_gui.cfg"
        if [ -f "$cfg" ]; then
            libs_rel=$(grep '^LIBS_REL=' "$cfg" | sed 's/^LIBS_REL=//')
            for tok in $libs_rel; do
                [ -n "$tok" ] || continue
                base=$(basename "$tok")
                src="$bd/$tok"
                if [ -f "$src" ]; then
                    cp "$src" "$DIST/lib/$triple/$base"
                elif [ ! -f "$DIST/lib/$triple/$base" ]; then
                    echo "   WARN: lib LIBS_REL introuvable : $src" >&2
                fi
            done
            relocatabilize_cfg "$cfg"
        fi
    done
done

# 5. Stats
echo ""
echo "==> Done"
du -sh "$DIST" | awk '{print "    Total size :", $1}'
echo "    Layout    : $DIST"
ls -la "$DIST/" | head -20
