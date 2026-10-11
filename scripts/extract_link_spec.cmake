# extract_link_spec.cmake — Extrait la link cmd de la cible 'owl' depuis
# build.ninja et produit lib/owl_link_gui.cfg, consume par owl-build.
#
# Le .cfg liste les flags/libs/objects pour linker un bundle OWL en .exe
# GUI complet (Qt6 statique + plugins). owl-build remplace les sources
# main.c/owl_stdlib/etc. par son _owl_bundle.c.obj.
#
# Format du .cfg (key=value, separateur newline) :
#   LINK_FLAGS=...
#   OBJECTS=...           liste de .obj externes (plugins, qrc)
#   LIBS_REL=...          libs relatives au build/lib/
#   LIBS_ABS=...          libs absolues (Qt6, system)
#   LIBS_NAMES=...        -lname args
#
# Usage : cmake -DBUILD_DIR=<...> -DOUT_CFG=<...> -P extract_link_spec.cmake

if(NOT DEFINED BUILD_DIR OR NOT DEFINED OUT_CFG)
    message(FATAL_ERROR "Usage: cmake -DBUILD_DIR=<dir> -DOUT_CFG=<file> -P extract_link_spec.cmake")
endif()

set(NINJA_FILE "${BUILD_DIR}/build.ninja")
if(NOT EXISTS "${NINJA_FILE}")
    message(FATAL_ERROR "build.ninja not found at ${NINJA_FILE}")
endif()

file(READ "${NINJA_FILE}" NINJA_CONTENT)

# Trouve le bloc "build owl.exe:" (ou "build owl:" sur Linux). Le bloc
# va du "build owl..." jusqu'au prochain "build " en debut de ligne (ou EOF).
# CMake string(REGEX MATCH) avec mode dotall implicite via [\s\S] equivalent.
string(REGEX MATCH "[\r\n]build owl(\\.exe)?: [^\n]*\n([ \t][^\n]*\n)+"
    OWL_BLOCK "${NINJA_CONTENT}")
if(OWL_BLOCK STREQUAL "")
    message(FATAL_ERROR "'build owl.exe' rule not found in ${NINJA_FILE}")
endif()

# Extrait LINK_FLAGS = ... et LINK_LIBRARIES = ...
string(REGEX MATCH "[ \t]+LINK_FLAGS[ \t]*=[ \t]*([^\n]*)"
    _link_flags_line "${OWL_BLOCK}")
set(LINK_FLAGS "${CMAKE_MATCH_1}")

string(REGEX MATCH "[ \t]+LINK_LIBRARIES[ \t]*=[ \t]*([^\n]*)"
    _link_libs_line "${OWL_BLOCK}")
set(LINK_LIBRARIES "${CMAKE_MATCH_1}")

# Tokenize LINK_LIBRARIES (split sur espaces multiples). Decode "$:" -> ":"
# (echappement ninja).
string(REPLACE "$:" ":" LINK_LIBRARIES "${LINK_LIBRARIES}")
# Replace tab/CR par espace
string(REGEX REPLACE "[\t\r]" " " LINK_LIBRARIES "${LINK_LIBRARIES}")
# Split en liste sur espaces (CMake list = ;-separated)
string(REGEX REPLACE " +" ";" LIBRARIES_TOKENS "${LINK_LIBRARIES}")

set(OBJECTS "")
set(LIBS_REL "")
set(LIBS_ABS "")
set(LIBS_NAMES "")

foreach(TOK ${LIBRARIES_TOKENS})
    if(TOK STREQUAL "")
        # skip
    elseif(TOK MATCHES "^-l")
        # -lXXX library name
        set(LIBS_NAMES "${LIBS_NAMES} ${TOK}")
    elseif(TOK MATCHES "\\.(obj|o)$")
        # .obj/.o file
        set(OBJECTS "${OBJECTS} ${TOK}")
    elseif(TOK MATCHES "\\.(a|so)(\\.[0-9.]+)?$")
        # static .a or dynamic .so library
        if(TOK MATCHES "^([A-Za-z]:[/\\\\])|^/")
            set(LIBS_ABS "${LIBS_ABS} ${TOK}")
        else()
            set(LIBS_REL "${LIBS_REL} ${TOK}")
        endif()
    else()
        # Other flag
        set(LINK_FLAGS "${LINK_FLAGS} ${TOK}")
    endif()
endforeach()

# Pour owl-build : le bundle.c remplace les sources de owl (main.c,
# owl_stdlib.c, ui_qt.cpp, etc.). Ces sources sont deja dans
# libowl_runtime_gui.a, qu'on prepend a LIBS_REL.
set(LIBS_REL "libowl_runtime_gui.a${LIBS_REL}")

# B.5 : remplace les paths absolus Qt6 par @QT6_ROOT@/...
# Permet a owl-build de relocaliser le cfg vers son install layout
# (cross-compile sur un autre host, packaging owl-build, etc.).
if(DEFINED QT6_ROOT)
    # Normalise QT6_ROOT (toujours forward slashes pour le replace)
    string(REPLACE "\\" "/" QT6_ROOT_NORM "${QT6_ROOT}")
    # OBJECTS et LIBS_ABS contiennent des paths qui commencent par QT6_ROOT
    string(REPLACE "${QT6_ROOT_NORM}" "@QT6_ROOT@" OBJECTS  "${OBJECTS}")
    string(REPLACE "${QT6_ROOT_NORM}" "@QT6_ROOT@" LIBS_ABS "${LIBS_ABS}")
endif()
# Idem DuckDB + llama (leurs .a sont dans LIBS_ABS en chemin absolu d'install).
# Sans ca : chemin d'arbre de build en dur -> owl-build casse sur poste vierge
# (libs introuvables) + fuite du chemin machine. Avec placeholder : relocalise
# a l'extraction + zero chemin absolu dans le cfg.
if(DEFINED DUCKDB_ROOT)
    string(REPLACE "\\" "/" DUCKDB_ROOT_NORM "${DUCKDB_ROOT}")
    string(REPLACE "${DUCKDB_ROOT_NORM}" "@DUCKDB_ROOT@" LIBS_ABS "${LIBS_ABS}")
endif()
if(DEFINED LLAMA_ROOT)
    string(REPLACE "\\" "/" LLAMA_ROOT_NORM "${LLAMA_ROOT}")
    string(REPLACE "${LLAMA_ROOT_NORM}" "@LLAMA_ROOT@" LIBS_ABS "${LIBS_ABS}")
endif()

# Trim leading spaces
string(REGEX REPLACE "^ +" "" LINK_FLAGS "${LINK_FLAGS}")
string(REGEX REPLACE "^ +" "" OBJECTS    "${OBJECTS}")
string(REGEX REPLACE "^ +" "" LIBS_REL   "${LIBS_REL}")
string(REGEX REPLACE "^ +" "" LIBS_ABS   "${LIBS_ABS}")
string(REGEX REPLACE "^ +" "" LIBS_NAMES "${LIBS_NAMES}")

# Output
get_filename_component(OUT_DIR "${OUT_CFG}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIR}")

set(_qt6_default "")
if(DEFINED QT6_ROOT)
    string(REPLACE "\\" "/" _qt6_default "${QT6_ROOT}")
endif()
set(_duckdb_default "")
if(DEFINED DUCKDB_ROOT)
    string(REPLACE "\\" "/" _duckdb_default "${DUCKDB_ROOT}")
endif()
set(_llama_default "")
if(DEFINED LLAMA_ROOT)
    string(REPLACE "\\" "/" _llama_default "${LLAMA_ROOT}")
endif()

set(CFG_CONTENT
"# owl_link_gui.cfg — genere par scripts/extract_link_spec.cmake
# Consume par owl-build pour link les bundles GUI.
# NE PAS EDITER — regenere a chaque build owl.

# Placeholders : @QT6_ROOT@ = remplace par owl-build au runtime selon
# l'install layout (env OWL_QT6_ROOT_<target>, ou QT6_ROOT_DEFAULT
# ci-dessous comme fallback dev mode).
QT6_ROOT_DEFAULT=${_qt6_default}
DUCKDB_ROOT_DEFAULT=${_duckdb_default}
LLAMA_ROOT_DEFAULT=${_llama_default}
LINK_FLAGS=${LINK_FLAGS}
OBJECTS=${OBJECTS}
LIBS_REL=${LIBS_REL}
LIBS_ABS=${LIBS_ABS}
LIBS_NAMES=${LIBS_NAMES}
")
file(WRITE "${OUT_CFG}" "${CFG_CONTENT}")

# Stats pour user feedback
string(LENGTH "${LINK_FLAGS}" _flags_len)
string(REGEX REPLACE "[^ ]+" "X" _objs_count_str "${OBJECTS}")
string(LENGTH "${_objs_count_str}" _objs_count)
string(REGEX REPLACE "[^ ]+" "X" _abs_count_str "${LIBS_ABS}")
string(LENGTH "${_abs_count_str}" _abs_count)
message(STATUS "==> ${OUT_CFG} written")
message(STATUS "    LINK_FLAGS  : ${_flags_len} chars")
message(STATUS "    OBJECTS     : ${_objs_count} files")
message(STATUS "    LIBS_ABS    : ${_abs_count} libs")
