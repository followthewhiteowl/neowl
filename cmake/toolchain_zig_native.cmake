# Toolchain CMake : zig cc en mode native (host=target).
# Pas de cross-compile — zig cc remplace gcc/clang systeme.
#
# Usage :
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_zig_native.cmake -S . -B build
#
# Pre-requis : scripts/setup_zig.sh execute pour avoir vendor/zig/<host>/zig

if(NOT DEFINED CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "Toolchain doit etre passee via -DCMAKE_TOOLCHAIN_FILE")
endif()

# Build natif x86_64. Ce toolchain ne declare pas CMAKE_SYSTEM_NAME (host=target),
# donc CMake devine le processeur depuis l'hote : sur Windows via
# %PROCESSOR_ARCHITECTURE%. Un bash.exe MSYS2 spawne non-interactivement
# (CI, bash -c, bash script.sh) n'herite PAS de cette variable -> processeur
# vide -> un ExternalProject qui branche dessus (libjpeg-turbo) casse au
# configure. On l'epingle ici quand il est absent.
if(NOT CMAKE_SYSTEM_PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR "x86_64")
endif()

set(_owl_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_wrappers "${_owl_root}/scripts/zig_wrappers")

if(WIN32)
    # Launcher exe natif (zig_launcher.c). Compile via :
    #   gcc scripts/zig_wrappers/zig_launcher.c -o scripts/zig_wrappers/zig-cc.exe
    # puis copy en zig-c++.exe / zig-ar.exe / zig-ranlib.exe.
    # Le launcher detecte la sous-commande zig depuis son propre nom.
    # Plus robuste que .cmd / .sh : pas de shell intermediaire qui
    # corromprait les args avec quotes (-DVAR=\"...\").
    set(_ext ".exe")
else()
    set(_ext "")
endif()

set(CMAKE_C_COMPILER       "${_wrappers}/zig-cc${_ext}")
set(CMAKE_CXX_COMPILER     "${_wrappers}/zig-c++${_ext}")
set(CMAKE_AR               "${_wrappers}/zig-ar${_ext}")
set(CMAKE_RANLIB           "${_wrappers}/zig-ranlib${_ext}")

# Sur Windows MSYS2 ou Linux WSL2, CMake peut rajouter des -I vers
# /c/msys64/ucrt64/include ou /usr/include qui contiennent des headers
# C systeme incompatibles avec libc++ zig (math.h MSYS2 macros qui
# cassent <cmath> libc++). Blacklist ces paths pour que zig n'utilise
# que son propre sysroot MinGW/glibc.
if(WIN32)
    set(CMAKE_IGNORE_PATH "C:/msys64;C:/msys64/ucrt64;C:/msys64/ucrt64/include;C:/msys64/ucrt64/lib;/c/msys64;/c/msys64/ucrt64;/c/msys64/ucrt64/include;/c/msys64/ucrt64/lib")
endif()

# clang plus strict que gcc — relax les warnings communs OWL
add_compile_options(
    -Wno-error=typedef-redefinition  # OwlEnum forward decl present 2x (legal en C11)
    -Wno-error=parentheses-equality
    -Wno-error=unknown-warning-option
    # zig clang detecte les features CPU du build host et peut produire du
    # code qui plante sur des CPU plus anciens (AVX-512 / cmpccxadd / etc.).
    # Force x86_64_v2 = baseline 2009+ (SSE4.2, POPCNT) couvrant 99% des CPU.
    # Note : zig syntax uses underscores (mcpu=x86_64_v2), not GCC's
    # -march=x86-64-v2.
    -mcpu=x86_64_v2
)

# Suppression PDB : zig clang sur Windows-GNU produit des .pdb (MSVC-style
# debug info) meme en Release. Ca ajoute 100-250 MB par binaire sur disque
# sans benefice (gdb lit le DWARF inline, pas le PDB). On les supprime
# via flag linker. `-Wl,--no-pdb` ou equivalent direct.
if(WIN32)
    # lld-link en Release : -g0 reduit fortement les PDB mais en laisse
    # encore quelques MB. On les supprime post-build dans CMakeLists.txt
    # via add_custom_command sur chaque executable cible.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_compile_options(-g0)
    endif()
endif()
