# Toolchain CMake : zig cc ciblant x86_64-linux-musl.
#
# But : produire un binaire Linux 100% statique et universel (Alpine,
# scratch, distroless, distros glibc) — le pendant exact du .exe Windows.
# musl est concu pour le static-link propre (glibc-static casse le DNS/NSS).
#
# ADDITIF : n'impacte NI Windows (UCRT) NI le build Linux glibc existant.
# Reutilise les wrappers zig + flags communs de toolchain_zig_native.cmake.
#
# Usage :
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_zig_musl.cmake \
#         -DQT_HOST_PATH=<qt-host-glibc-install> -S . -B build
#
# Pre-requis : scripts/setup_zig.sh (vendor/zig/<host>/zig), et pour Qt6
# un QT_HOST_PATH (outils hote moc/rcc/qmake compiles en natif glibc).

set(OWL_ZIG_MUSL_TARGET "x86_64-linux-musl" CACHE STRING "zig target triple musl")

# Marqueur lu par CMakeLists.txt pour aiguiller le link Linux : profil
# musl-static universel (offscreen, tout statique, plugin QOffscreen) au
# lieu du profil glibc-dynamique (find_package X11/Fontconfig).
set(OWL_MUSL_STATIC ON CACHE BOOL "Build Linux musl-static universel")

# --- Mode cross-compile ----------------------------------------------------
# Meme si l'hote est Linux, on cible une AUTRE libc (musl vs glibc systeme).
# Declarer CMAKE_SYSTEM_NAME force CMAKE_CROSSCOMPILING=TRUE, ce qui active
# l'isolation find-root-path : find_*/find_package cessent de fouiller le
# sysroot glibc de l'hote (/usr/include, multiarch). C'est la generalisation
# du CMAKE_IGNORE_PATH MSYS2 applique cote Windows dans toolchain_zig_native.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Compilateurs + flags communs (wrappers zig-cc/zig-c++/zig-ar/zig-ranlib).
include("${CMAKE_CURRENT_LIST_DIR}/toolchain_zig_native.cmake")

# Cible musl sur compilation ET link.
add_compile_options(-target ${OWL_ZIG_MUSL_TARGET})
add_link_options(-target ${OWL_ZIG_MUSL_TARGET})

# --- Isolation du sysroot glibc de l'hote ----------------------------------
# En cross-mode, restreindre les recherches find_* aux racines controlees.
# PROGRAM=NEVER : les outils hote (moc/rcc via QT_HOST_PATH, pkg-config) sont
# des binaires natifs legitimes, on ne les filtre pas par la root-path.
# LIBRARY/INCLUDE/PACKAGE=ONLY : jamais dans le /usr glibc de l'hote.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Ceinture + bretelles : blackliste explicitement les arbres glibc de l'hote
# pour qu'un hint code en dur ou un .pc egare ne puisse pas injecter le
# fameux -isystem /usr/include qui tire features-time64.h (glibc) dans le
# build musl et casse sur bits/wordsize.h introuvable.
list(APPEND CMAKE_IGNORE_PATH
    /usr/include
    /usr/local/include
    /usr/include/x86_64-linux-gnu
    /usr/lib
    /usr/local/lib
    /usr/lib/x86_64-linux-gnu
    # Sous WSL2, le PATH herite du PATH Windows expose /mnt/c/msys64/ucrt64/bin.
    # CMake derive ses prefixes de recherche du PATH → il trouve les packages
    # MSYS2 Windows (double-conversion, zstd, md4c, DBus1, ICU...) et injecte
    # leurs headers Windows (-I/mnt/c/msys64/ucrt64/include, _mingw.h) dans le
    # build musl. On blackliste ces arbres. Complement : nettoyer le PATH au
    # lancement (cf. scripts/build_*_zig_musl.sh, comme le fait deja duckdb).
    /mnt/c/msys64
    /mnt/c/msys64/ucrt64
    /mnt/c/msys64/ucrt64/include
    /mnt/c/msys64/ucrt64/lib
    C:/msys64
    C:/msys64/ucrt64
)

# pkg-config de l'hote ne connait que le monde glibc (.pc qui pointent
# /usr/include, /usr/lib/x86_64-linux-gnu). On ne le desactive PAS (Qt
# appelle pkg_check_modules inconditionnellement dans certains FindWrap*),
# on le NEUTRALISE : PKG_CONFIG_LIBDIR pointe un repertoire vide, donc
# pkg-config existe mais ne trouve aucun .pc de l'hote → zero -I/usr/include
# injecte. Tout est bundle de toute facon (system_* OFF).
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_CURRENT_LIST_DIR}/musl_empty_pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
