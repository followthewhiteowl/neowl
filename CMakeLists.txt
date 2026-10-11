cmake_minimum_required(VERSION 3.21)
project(owl LANGUAGES C CXX VERSION 1.0.2)

# ------------------------------------------------------------------
# Build options
# ------------------------------------------------------------------
# owl.exe est all-in-one : interpréteur + GUI Qt6 + PDF (qpdf) + LSP-friendly.
# Plus d'option OWL_BUILD_GUI / OWL_GUI_STATIC : Qt6 est toujours actif et
# toujours static (vendore depuis vendor/qt6/qtbase). Cf decision actee dans
# docs/internal_documentation/cross_platform_strategy.md.
option(OWL_BUILD_LSP       "Build owl-lsp.exe (Language Server)" ON)
option(OWL_BUILD_DAP       "Build owl-dap.exe (Debug Adapter — Lot D, en cours)" ON)
# OWL_DEV_TOOLS : binaires utiles uniquement aux contributeurs OWL
# (pas distribues aux dev utilisateurs du langage) :
#   - owl-cli       : variante owl sans Qt6/GUI/qpdf pour bench / CI
#   - owl-lsp-test  : harness de tests in-process du LSP
#   - owl-lsp-bench : benchmark perf du LSP
# Default OFF -> build standard ne produit que owl.exe + owl-lsp.exe.
option(OWL_DEV_TOOLS       "Build dev-only binaries (owl-cli, owl-lsp-test, owl-lsp-bench)" OFF)
# OWL_BUILD_OWL : si OFF, on skip la cible owl(.exe) GUI et find_package(Qt6).
# Permet un build CLI-only quand Qt6 n'est pas dispo (par ex. CI Linux
# minimale qui veut juste valider owl-cli et les tests). owl-cli, owl-build,
# owl-lsp, owl-dap restent buildables car ne dependent pas de Qt6.
option(OWL_BUILD_OWL       "Build owl(.exe) GUI all-in-one (necessite Qt6 vendore)" ON)
option(OWL_RELEASE_DISTRIB "Build distribuable (--strip-all + surface publique seule : pas de --self-test-*, --vscode-data, --dump-lsp-catalog, snapshot)" OFF)

# OWL_RELEASE_DISTRIB doit etre visible du C (gating compile-time de la
# surface interne, ). Pose le define AVANT toute cible pour qu'il
# s'applique partout (owl/owl-lsp/owl-dap + owl_core). OFF = dev inchange.
if(OWL_RELEASE_DISTRIB)
    add_compile_definitions(OWL_RELEASE_DISTRIB)
endif()

# Profil musl-static universel (offscreen). Expose le flag au C/C++ pour que
# le code applicatif puisse conditionner les specificites du profil (ex : ne
# pas Q_IMPORT_PLUGIN le plugin plateforme xcb, absent quand FEATURE_xcb=OFF).
# Le flag CMake est pose par cmake/toolchain_zig_musl.cmake. OFF = build
# Windows/glibc inchange.
if(OWL_MUSL_STATIC)
    add_compile_definitions(OWL_MUSL_STATIC)
endif()

# Standards
set(CMAKE_C_STANDARD   99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default: Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Modern UCRT runtime (cohérent avec MSYS2 UCRT64). MinGW seulement.
if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

# ------------------------------------------------------------------
# Vendor libs (toutes compilées en static, tagguées comme une seule lib
# interne pour simplifier le link)
# ------------------------------------------------------------------
set(VENDOR_DIR "${CMAKE_SOURCE_DIR}/vendor")

# --- mbedTLS ---
file(GLOB MBEDTLS_SRCS "${VENDOR_DIR}/mbedtls/library/*.c")
add_library(owl_mbedtls STATIC ${MBEDTLS_SRCS})
target_include_directories(owl_mbedtls PUBLIC
    "${VENDOR_DIR}/mbedtls/include"
    "${VENDOR_DIR}/mbedtls/library"
    "${VENDOR_DIR}/mbedtls"
)
target_compile_options(owl_mbedtls PRIVATE
    -O2 -fno-strict-aliasing
    # mbedtls debug_internal.h:32 utilise __attribute__((format(gnu_printf,...)))
    # qui n'est supporte que par gcc, pas clang. Vendor code, on suppress.
    -Wno-ignored-attributes
)
# Hardening : NDEBUG vire les assert() C standard, MBEDTLS_NO_PLATFORM_ENTROPY
# n'est PAS desactive (on en a besoin pour le RNG TLS). On set juste NDEBUG,
# CMake en Release l'ajoute deja globalement mais on l'explicite pour clarte.
#
# MBEDTLS_USER_CONFIG_FILE : override apres mbedtls_config.h pour neutraliser
# MBEDTLS_DEBUG_C (module debug TLS non consomme par OWL — retire ~5 strings
# "assertion" du module debug.c). Le fichier vit dans src/ pour rester
# OWL-local (jamais touche vendor/).
target_compile_definitions(owl_mbedtls PRIVATE
    NDEBUG
    MBEDTLS_USER_CONFIG_FILE="owl_mbedtls_user_config.h"
)
target_include_directories(owl_mbedtls PRIVATE "${CMAKE_SOURCE_DIR}/src")

# --- miniz ---
add_library(owl_miniz STATIC "${VENDOR_DIR}/miniz/miniz.c")
target_include_directories(owl_miniz PUBLIC "${VENDOR_DIR}/miniz")

# --- LZMA SDK ---
file(GLOB LZMA_SRCS "${VENDOR_DIR}/lzma_sdk/*.c")
add_library(owl_lzma STATIC ${LZMA_SRCS})
target_include_directories(owl_lzma PUBLIC "${VENDOR_DIR}/lzma_sdk")
target_compile_definitions(owl_lzma PUBLIC _7ZIP_ST Z7_ST)

# --- PCRE2 ---
set(PCRE2_SRCS
    "${VENDOR_DIR}/pcre2/src/pcre2_auto_possess.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_chartables.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_chkdint.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_compile.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_config.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_context.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_error.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_extuni.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_find_bracket.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_maketables.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_match.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_match_data.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_newline.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_ord2utf.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_pattern_info.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_script_run.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_string_utils.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_study.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_substitute.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_substring.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_tables.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_ucd.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_valid_utf.c"
    "${VENDOR_DIR}/pcre2/src/pcre2_xclass.c"
)
add_library(owl_pcre2 STATIC ${PCRE2_SRCS})
target_include_directories(owl_pcre2 PUBLIC "${VENDOR_DIR}/pcre2/src")
target_compile_definitions(owl_pcre2 PUBLIC HAVE_CONFIG_H PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC)

# --- CA bundle (Mozilla) ---
add_library(owl_ca STATIC "${VENDOR_DIR}/ca/cacert_embed.c")

# --- md4c (Markdown parser, CommonMark + GFM) ---
add_library(owl_md4c STATIC
    "${VENDOR_DIR}/md4c/md4c.c"
    "${VENDOR_DIR}/md4c/md4c-html.c"
    "${VENDOR_DIR}/md4c/entity.c"
)
target_include_directories(owl_md4c PUBLIC "${VENDOR_DIR}/md4c")
target_compile_options(owl_md4c PRIVATE -O2)

# --- mpdecimal 4.0.1 (bignum decimal, precision arbitraire, IEEE 754-2008) ---
# Brique du modele de nombre v2 (consommee plus tard par  : OBJ_NUMERIC
# wrappe au GC). Ici : juste la brique disponible et lien-testee, aucun usage OWL.
# Header GNU genere une fois depuis mpdecimal.h.in (bloc CONFIG_64, ABI 64-bit,
# PAS le header VC). Chemin portable __uint128_t (ANSI + HAVE_UINT128_T), zero
# asm inline -> identique Win/Linux/mac sur la toolchain zig.
set(MPDECIMAL_SRCS
    "${VENDOR_DIR}/mpdecimal/basearith.c"
    "${VENDOR_DIR}/mpdecimal/constants.c"
    "${VENDOR_DIR}/mpdecimal/context.c"
    "${VENDOR_DIR}/mpdecimal/convolute.c"
    "${VENDOR_DIR}/mpdecimal/crt.c"
    "${VENDOR_DIR}/mpdecimal/difradix2.c"
    "${VENDOR_DIR}/mpdecimal/fnt.c"
    "${VENDOR_DIR}/mpdecimal/fourstep.c"
    "${VENDOR_DIR}/mpdecimal/io.c"
    "${VENDOR_DIR}/mpdecimal/mpalloc.c"
    "${VENDOR_DIR}/mpdecimal/mpdecimal.c"
    "${VENDOR_DIR}/mpdecimal/mpsignal.c"
    "${VENDOR_DIR}/mpdecimal/numbertheory.c"
    "${VENDOR_DIR}/mpdecimal/sixstep.c"
    "${VENDOR_DIR}/mpdecimal/transpose.c"
)
add_library(owl_mpdecimal STATIC ${MPDECIMAL_SRCS})
# IMPORTANT : seul include/ (= mpdecimal.h public) est PUBLIC. Le dossier racine
# vendor/mpdecimal contient des headers internes a NOM GENERIQUE (io.h, bits.h,
# crt.h, constants.h...) qui, propages sur le search-path global d'owl_core via
# owl_vendor, masqueraient les <io.h>/<bits.h> systeme (-> _setmode introuvable
# dans debug_stdio.c). On expose donc UNIQUEMENT le header public, sans collision.
target_include_directories(owl_mpdecimal PUBLIC "${VENDOR_DIR}/mpdecimal/include")
target_include_directories(owl_mpdecimal PRIVATE "${VENDOR_DIR}/mpdecimal")
# CONFIG_64/ANSI/HAVE_UINT128_T : selectionnent le chemin arithmetique portable
# (__uint128_t, sans asm). PRIVATE imperatif : ces noms (CONFIG_64, ANSI...) sont
# generiques et ne servent qu'a la compile des .c mpdecimal (headers internes
# bits.h/typearith.h/umodarith.h). Le header public mpdecimal.h est autonome
# (hardcode MPD_CONFIG_64) : aucun consommateur n'a besoin de ces defines, et les
# propager (PUBLIC) contaminerait owl_core via owl_vendor (-DANSI casse _setmode).
# Hardening : NDEBUG (assert off), visibility hidden + sections separees pour
# que --gc-sections puisse virer tout le non-consomme (cf build_hardening.md).
target_compile_definitions(owl_mpdecimal PRIVATE CONFIG_64 ANSI HAVE_UINT128_T NDEBUG)
target_compile_options(owl_mpdecimal PRIVATE
    -O2
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)

# --- zint 2.16.0 (backend code-barres, BSD-3-Clause) ---
# Enabler du chantier code-barres OWL (type + champ CodeBarre, +).
# On ne vendore QUE backend/ (BSD-3 ; les frontends CLI/GUI/Qt de zint sont
# GPL-3 et ne sont PAS embarques). Backend complet : 40+ symbologies 1D/2D
# (QR, Code128, DataMatrix, Aztec, PDF417, EAN/UPC, postal...).
#
# Sortie BUFFER-ONLY : on ne consomme que ZBarcode_Buffer (raster RGB en
# memoire), jamais la sortie fichier. png.c est EXCLU (seule source tirant
# une dep externe : libpng + zlib) et ZINT_NO_PNG est defini -> zint compile
# sans aucune dependance externe (raster.c bypasse alors zint_png_pixel_plot,
# cf. ses gardes #ifndef ZINT_NO_PNG). dllversion.c exclu aussi (versioning
# DLL Windows via shlwapi, hors bibliotheque). gs1.c ne tire gs1encoders.h
# que sous ZINT_HAVE_GS1SE (non defini) -> pas de dep GS1 Syntax Engine.
# Sources 100% C portable, zero asm inline -> identique host gcc / zig
# clang+libc++ sur Win/Linux/mac.
set(ZINT_SRCS
    "${VENDOR_DIR}/zint/backend/2of5.c"
    "${VENDOR_DIR}/zint/backend/2of5inter.c"
    "${VENDOR_DIR}/zint/backend/2of5inter_based.c"
    "${VENDOR_DIR}/zint/backend/auspost.c"
    "${VENDOR_DIR}/zint/backend/aztec.c"
    "${VENDOR_DIR}/zint/backend/bc412.c"
    "${VENDOR_DIR}/zint/backend/bmp.c"
    "${VENDOR_DIR}/zint/backend/channel.c"
    "${VENDOR_DIR}/zint/backend/codabar.c"
    "${VENDOR_DIR}/zint/backend/codablock.c"
    "${VENDOR_DIR}/zint/backend/code.c"
    "${VENDOR_DIR}/zint/backend/code1.c"
    "${VENDOR_DIR}/zint/backend/code11.c"
    "${VENDOR_DIR}/zint/backend/code128.c"
    "${VENDOR_DIR}/zint/backend/code128_based.c"
    "${VENDOR_DIR}/zint/backend/code16k.c"
    "${VENDOR_DIR}/zint/backend/code49.c"
    "${VENDOR_DIR}/zint/backend/common.c"
    "${VENDOR_DIR}/zint/backend/composite.c"
    "${VENDOR_DIR}/zint/backend/dmatrix.c"
    "${VENDOR_DIR}/zint/backend/dotcode.c"
    "${VENDOR_DIR}/zint/backend/dxfilmedge.c"
    "${VENDOR_DIR}/zint/backend/eci.c"
    "${VENDOR_DIR}/zint/backend/emf.c"
    "${VENDOR_DIR}/zint/backend/filemem.c"
    "${VENDOR_DIR}/zint/backend/general_field.c"
    "${VENDOR_DIR}/zint/backend/gif.c"
    "${VENDOR_DIR}/zint/backend/gridmtx.c"
    "${VENDOR_DIR}/zint/backend/gs1.c"
    "${VENDOR_DIR}/zint/backend/hanxin.c"
    "${VENDOR_DIR}/zint/backend/imail.c"
    "${VENDOR_DIR}/zint/backend/large.c"
    "${VENDOR_DIR}/zint/backend/library.c"
    "${VENDOR_DIR}/zint/backend/mailmark.c"
    "${VENDOR_DIR}/zint/backend/maxicode.c"
    "${VENDOR_DIR}/zint/backend/medical.c"
    "${VENDOR_DIR}/zint/backend/output.c"
    "${VENDOR_DIR}/zint/backend/pcx.c"
    "${VENDOR_DIR}/zint/backend/pdf417.c"
    "${VENDOR_DIR}/zint/backend/plessey.c"
    "${VENDOR_DIR}/zint/backend/postal.c"
    "${VENDOR_DIR}/zint/backend/ps.c"
    "${VENDOR_DIR}/zint/backend/qr.c"
    "${VENDOR_DIR}/zint/backend/raster.c"
    "${VENDOR_DIR}/zint/backend/reedsol.c"
    "${VENDOR_DIR}/zint/backend/rss.c"
    "${VENDOR_DIR}/zint/backend/svg.c"
    "${VENDOR_DIR}/zint/backend/telepen.c"
    "${VENDOR_DIR}/zint/backend/tif.c"
    "${VENDOR_DIR}/zint/backend/ultra.c"
    "${VENDOR_DIR}/zint/backend/upcean.c"
    "${VENDOR_DIR}/zint/backend/vector.c"
)
add_library(owl_zint STATIC ${ZINT_SRCS})
# PUBLIC = dir du zint.h public. Les headers internes (common.h, output.h...)
# vivent dans le meme backend/ donc remontent aussi sur le search-path via
# owl_vendor -> owl_core. Sans risque : contrairement au piege mpdecimal
# (<io.h>/<bits.h> en chevrons masquant les headers systeme), aucun header
# zint ne porte un nom de header systeme, et owl inclut son propre common.h
# en guillemets (resolution locale src/ prioritaire). Pas d'export de nom.
target_include_directories(owl_zint PUBLIC "${VENDOR_DIR}/zint/backend")
# ZINT_NO_PNG en PRIVATE : ne concerne QUE la compile du backend (garde le
# chemin de sortie PNG hors build, dep libpng non vendoree ; raster.c/library.c
# le testent en #ifdef interne). Le header PUBLIC zint.h ne reference pas cette
# macro -> aucun consommateur (smoke test, natives 0900 via ZBarcode_*) n'en a
# besoin. La laisser en PUBLIC contaminerait les flags de tout owl_core (full
# rebuild inutile). Meme logique que les defines mpdecimal (PRIVATE).
# Pas de define anti-dllexport a poser : zint.h n'emet __declspec(dllexport)
# que si DLL_EXPORT/PIC/_USRDLL est defini (build shared) ; en statique le
# defaut est `extern` simple -> ZBarcode_* n'entre pas dans la table PE.
# Hardening (calque mpdecimal) : NDEBUG (assert off), visibility hidden +
# sections separees pour que --gc-sections (herite au link owl) vire tout le
# non-consomme (la majorite des 40 symbologies + les formats de sortie fichier
# bmp/emf/gif/pcx/ps/svg/tif restent inertes tant que non appeles).
target_compile_definitions(owl_zint PRIVATE ZINT_NO_PNG NDEBUG)
target_compile_options(owl_zint PRIVATE
    -O2
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)

# --- argon2 20190702 (Argon2id, dual CC0-1.0 / Apache-2.0) ---
# Enabler du socle d'authentification OWL : mbedTLS ne fournit AUCUN hachage
# de mot de passe moderne (PBKDF2 seulement). Argon2id est le premier choix
# OWASP. Implementation de reference PHC, BLAKE2b embarque, zero dependance
# externe (pas meme libm).
#
# Sous-ensemble vendore (11 fichiers) : la lib de calcul et rien d'autre.
# ECARTE volontairement :
#   - opt.c + blake2/blamka-round-opt.h : chemin SIMD x86 SANS dispatch
#     runtime (emmintrin.h en dur) -> casse la compile sur aarch64 (macos /
#     linux-arm) et rend le binaire dependant du -march de la machine de
#     build. Mesure locale (gcc 15.2, m=19456 t=2 p=1) : ref 31 ms, opt SSE2
#     28 ms, opt AVX2 18 ms. La sortie est BIT-A-BIT IDENTIQUE entre ref et
#     opt (verifie sur les KAT officiels) : ce n'est qu'un compromis perf/
#     portabilite, jamais un compromis de resultat. Un dispatch runtime cote
#     OWL (cpu_supports avx2 -> TU compilee -mavx2) est une optimisation
#     future possible, hors perimetre de l'enabler.
#   - thread.c : parallelisme intra-hash, inutile ici (cf ARGON2_NO_THREADS).
#     thread.h EST vendore car core.c l'inclut inconditionnellement ; son
#     corps entier est derriere #if !defined(ARGON2_NO_THREADS) -> header vide.
#   - run.c / bench.c / test.c / genkat.{c,h} : CLI, bench, KAT. core.c
#     n'inclut genkat.h que sous #ifdef GENKAT (non defini).
#
# ARGON2_NO_THREADS : supprime toute reference a pthread / _beginthreadex.
# fill_memory_blocks() route alors systematiquement vers la variante
# single-thread. p>1 reste ACCEPTE et produit le MEME tag (les lanes sont
# traitees en serie au lieu d'en parallele) -> aucune divergence de hash,
# seulement pas de gain de temps. Coherent avec la VM mono-thread OWL.
#
# Sources 100% C89/C99 portable, zero asm inline, zero I/O (aucun printf),
# malloc/free standard (hooks allocate_cbk/free_cbk dispo si un jour on veut
# router vers l'allocateur OWL).
set(ARGON2_SRCS
    "${VENDOR_DIR}/argon2/src/argon2.c"
    "${VENDOR_DIR}/argon2/src/core.c"
    "${VENDOR_DIR}/argon2/src/encoding.c"
    "${VENDOR_DIR}/argon2/src/ref.c"
    "${VENDOR_DIR}/argon2/src/blake2/blake2b.c"
)
add_library(owl_argon2 STATIC ${ARGON2_SRCS})
# PUBLIC = include/ seul (un unique argon2.h, nom non ambigu). src/ reste
# PRIVATE : il contient core.h, encoding.h et surtout thread.h, noms
# generiques qui, propages sur le search-path global d'owl_core via
# owl_vendor, masqueraient des headers systeme (cf. le piege mpdecimal
# <io.h>/<bits.h>). blake2/blake2.h fait `#include <argon2.h>` en chevrons
# -> couvert par le -I PUBLIC.
target_include_directories(owl_argon2 PUBLIC "${VENDOR_DIR}/argon2/include")
target_include_directories(owl_argon2 PRIVATE "${VENDOR_DIR}/argon2/src")
# ARGON2_NO_THREADS en PRIVATE : ne concerne que la compile des .c (thread.h
# inclus par core.c). Le header PUBLIC argon2.h ne teste jamais cette macro
# -> aucun consommateur n'en a besoin, et la mettre PUBLIC contaminerait les
# flags de tout owl_core. Meme logique que ZINT_NO_PNG / les defines mpdecimal.
# Pas de define anti-dllexport a poser : argon2.h n'emet __declspec(dllexport)
# que sous _MSC_VER (cf. son bloc "Symbols visibility control"), jamais sous
# gcc/clang -> ARGON2_PUBLIC est vide chez nous, les argon2*_hash_* n'entrent
# pas dans la table d'export PE. A revalider si on passait un jour a MSVC.
# Hardening (calque zint/mpdecimal) : NDEBUG, visibility hidden + sections
# separees pour que --gc-sections (herite au link owl) vire le non-consomme
# (argon2d/argon2i, les variantes _ctx et le decodage restent inertes tant
# que non appeles).
target_compile_definitions(owl_argon2 PRIVATE ARGON2_NO_THREADS NDEBUG)
target_compile_options(owl_argon2 PRIVATE
    -O2
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)

# --- libxls 1.6.3 (lecteur .xls binaire herite BIFF8/OLE2, BSD-2-Clause) ---
# Enabler de la lecture du format Excel 97-2003 (.xls) : le modele
# xlsDocument/XlsSheet/XlsCell (src/stdlib_xls.c) sait deja lire le .xlsx (ZIP
# + XML via miniz/libxml2) ; le seul format manquant en lecture est le .xls
# binaire (BIFF8 dans un conteneur OLE2). Le cablage dans stdlib_xls.c
# (xls_sniff_format voie OLE2 -> peuplement XlsSheet/XlsCell) est un lot feature
# separe ; ici on ne fournit que la brique de parsing vendoree + durcie.
#
# Sous-ensemble vendore (5 .c + config.h) : la bibliotheque de parsing SEULE.
# ECARTE a la copie : src/xls2csv.c (frontend CLI, hors bibliotheque). test/,
# fuzz/, configure.ac, Makefile.am, bootstrap (build autotools amont non
# utilise : on compile les .c directement, comme zint/argon2).
#
# ZERO DEPENDANCE EXTERNE — le point dur du vendoring etait iconv. libxls peut
# tirer libiconv pour les charsets BIFF (non vendorable proprement = cascade
# refusee), MAIS iconv est OPTIONNEL : notre config.h ne definit PAS HAVE_ICONV.
# Le chemin sans-iconv de xlstool.c convertit deja proprement vers UTF-8 :
#   - chaines compressees BIFF8 (latin1)  -> transcode_latin1_to_utf8()
#   - chaines UTF-16LE                     -> unicode_decode_wcstombs() avec une
#     locale UTF-8 FORCEE par plateforme (locale.c : _create_locale(".65001")
#     sur Windows, newlocale "C.UTF-8" sur Linux, "UTF-8" sur macOS). Ce n'est
#     donc PAS la locale systeme qui decide -> conversion deterministe.
# Valide au RUNTIME (libxls_smoketest) sur du vietnamien a diacritiques
# (codepage 1200 = UTF-16) : octets UTF-8 exacts. Seule degradation connue et
# assumee : les chaines BIFF5 (Excel 5/95) a codepage exotique tombent en copie
# brute sans iconv (codepage_decode #else) — BIFF8 (Excel 97-2003), la vraie
# cible du "legacy .xls", est integralement couvert.
#
# _GNU_SOURCE : requis cote Linux/musl (newlocale, uselocale, strdup) ; benin
# sur mingw/UCRT (deja declares). En PRIVATE : seul le build des .c en a besoin.
set(LIBXLS_SRCS
    "${VENDOR_DIR}/libxls/src/endian.c"
    "${VENDOR_DIR}/libxls/src/locale.c"
    "${VENDOR_DIR}/libxls/src/ole.c"
    "${VENDOR_DIR}/libxls/src/xls.c"
    "${VENDOR_DIR}/libxls/src/xlstool.c"
)
add_library(owl_libxls STATIC ${LIBXLS_SRCS})
# PUBLIC = include/ (xls.h racine, nom NON ambigu). Les headers internes vivent
# sous include/libxls/ (endian.h, locale.h, ole.h...) : sur le search-path
# global d'owl_core ils s'ecrivent "libxls/endian.h", jamais "endian.h" nu ->
# aucun masquage des <endian.h>/<locale.h> systeme (contrairement au piege
# mpdecimal <io.h>/<bits.h>). src/ en PRIVATE (config.h + inclusions relatives
# "../include/..." internes aux .c, aucun -I supplementaire requis).
target_include_directories(owl_libxls PUBLIC "${VENDOR_DIR}/libxls/include")
target_include_directories(owl_libxls PRIVATE "${VENDOR_DIR}/libxls/src")
# Pas de define anti-dllexport : xls.h n'emet aucun __declspec(dllexport)
# (l'API est en `extern` simple) -> les xls_* n'entrent pas dans la table PE.
# Hardening (calque zint/argon2) : NDEBUG (assert off), visibility hidden +
# sections separees pour que --gc-sections (herite au link owl) vire tout le
# non-consomme tant qu'aucune native ne tape l'API.
target_compile_definitions(owl_libxls PRIVATE _GNU_SOURCE HAVE_CONFIG_H NDEBUG)
target_compile_options(owl_libxls PRIVATE
    -O2
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)

# --- llhttp 9.4.2 (parseur HTTP/1.1 strict, MIT) ---
# Enabler du chantier serveur HTTP, mais surtout durcissement d'une surface
# DEJA en production : src/stdlib_http.c parse les reponses HTTP a la main.
# llhttp parse les reponses aussi bien que les requetes (HTTP_RESPONSE /
# HTTP_REQUEST / HTTP_BOTH), donc le gain est acquis independamment du verdict
# sur l'architecture serveur.
#
# Sources : le C GENERE et committe du tag amont release/v9.4.2 (llhttp est
# ecrit en TypeScript et se compile en machine a etats C via llparse ; la
# branche main ne contient PAS de C). Exactement ce que nodejs/node vendore
# dans deps/llhttp/src/. Aucune toolchain Node requise chez nous.
#
# 3 .c, liste explicite, pas de GLOB :
#   llhttp.c (321 Ko) : la machine a etats generee. Zero malloc, zero I/O,
#                       zero assert, aucun appel systeme. SIMD (SSE4.2 /
#                       NEON / wasm) correctement gardee par #ifdef sur les
#                       macros de la cible -> pas de -march impose, pas de
#                       piege aarch64 (contrairement a l'argon2 opt.c).
#   api.c            : API publique + llhttp_alloc/llhttp_free (malloc, non
#                       utilises si le parseur est un membre de struct).
#   http.c           : llhttp_message_needs_eof / llhttp_should_keep_alive.
#
# SECURITE — AUCUN FLAG LENIENT_* N'EST ACTIVE, ET C'EST UN CHOIX, PAS UN OUBLI.
# llhttp est strict par defaut : les 11 llhttp_set_lenient_*() existent
# precisement pour RELACHER le defaut. Chacun rouvre une classe de request
# smuggling (CL+TE simultanes, chunked non final, CR ou LF seul en delimiteur,
# espace avant les deux-points -> CVE-2024-27982, CR seul -> CVE-2023-30589,
# version HTTP arbitraire, caracteres de controle en valeur de header...).
# Le durcissement du defaut est la raison meme du choix de llhttp : on ne le
# desarme pas. Toute future activation d'un llhttp_set_lenient_* exige un
# ticket justifie par un client reel casse, et fera tomber
# src/internal_tests/llhttp_smoketest.c (qui asserte lenient_flags == 0 et
# rejoue 17 rejets stricts).
set(LLHTTP_SRCS
    "${VENDOR_DIR}/llhttp/src/llhttp.c"
    "${VENDOR_DIR}/llhttp/src/api.c"
    "${VENDOR_DIR}/llhttp/src/http.c"
)
add_library(owl_llhttp STATIC ${LLHTTP_SRCS})
# PUBLIC = include/ seul, et il ne contient QUE llhttp.h (nom non ambigu, zero
# risque de masquer un header systeme). Contrairement a argon2, aucun -I PRIVATE
# sur src/ n'est necessaire : les 3 .c n'ont aucun header prive, ils incluent
# "llhttp.h" qui vit dans include/.
target_include_directories(owl_llhttp PUBLIC "${VENDOR_DIR}/llhttp/include")
# LLHTTP_EXPORT vide, en PUBLIC — exception assumee au pattern PRIVATE de
# zint/argon2/mpdecimal, et la raison est structurelle : LLHTTP_EXPORT n'est pas
# une option de compilation de la lib, c'est une macro du HEADER PUBLIC posee sur
# CHAQUE declaration. Amont, sur _WIN32, elle vaut __declspec(dllexport) sans
# aucune garde de type BUILDING_LLHTTP. Mesure faite sur le smoke test avant
# correction : 34 fonctions llhttp remontees dans la table d'export PE du .exe,
# dont les 11 llhttp_set_lenient_* — soit la surface exacte permettant de
# desarmer les regles anti-smuggling depuis l'exterieur du binaire.
# --gc-sections n'y peut rien : dllexport epingle les symboles. Il faut donc que
# le define atteigne AUSSI les consommateurs, sinon leurs declarations
# re-exportent ce que la lib n'exporte plus -> PUBLIC. Le header vendore porte
# la seule divergence OWL vs amont : une garde #ifndef LLHTTP_EXPORT.
# Apres correction : 0 export, table .edata absente du binaire.
target_compile_definitions(owl_llhttp PUBLIC LLHTTP_EXPORT=)
# NDEBUG en PRIVATE : llhttp n'a aucun assert(), mais on aligne le contrat
# Release des autres vendors. A noter pour le wrappage OWL : api.c contient
# 4 abort() en branche `default:` de switch sur des enums internes (etats
# theoriquement inatteignables) et un llhttp__debug() qui ecrit sur stderr —
# ce dernier n'est reference nulle part dans le C genere, donc --gc-sections
# le vire avec sa dependance fprintf.
target_compile_definitions(owl_llhttp PRIVATE NDEBUG)
target_compile_options(owl_llhttp PRIVATE
    -O2
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)

# --- libssh2 (client SSH-2 / SFTP, BSD-3-Clause, backend mbedTLS) ---
# Enabler du Lot D FTP : le SFTP est du SSH-2 file transfer, PAS du FTP-sur-TLS
# (protocole entierement different). Vendore comme brique de transport ; le
# wrappage OWL (natives FTP* routees sur libssh2_sftp_* quand prefixe sftp://)
# est un lot feature SEPARE. Ici : la lib vendoree + durcie, rien d'autre.
#
# PIN : commit cdbda3cd (master, 2026-08-26), PAS le tag 1.11.1. Raison de
# securite NON negociable : le dernier tag (1.11.1, oct-2024) est vulnerable a
# trois CVE non backportees en release, dont CVE-2026-55200 (CVSS 9.x, ecriture
# hors-bornes dans ssh2_transport_read() declenchee PRE-AUTH par un serveur SSH
# malveillant via un packet_length forge -> RCE cote client). C'est exactement
# le modele de menace d'OWL en client SFTP. Les trois fixes sont dans master :
#   2dae3024 (2025-10-10) sftp_symlink OOB read      -> CVE-2025-15661
#   17626857 (2026-04-21) EXT_INFO get_string check  -> CVE-2026-55199
#   97acf3df (2026-06-12) transport packet_length     -> CVE-2026-55200
# + le hardening transport/sftp de juillet-aout 2026. Aucun patch OWL local
# (contrairement a llhttp) : le pin EST le mecanisme de correction. Au bump vers
# le futur tag 1.11.2 (attendu, bundlera ces fixes), verifier via merge-base que
# les trois commits ci-dessus y sont ancetres, sinon rester sur un commit master.
#
# Backend crypto = mbedTLS (deja vendoree) -> ZERO nouvelle dependance crypto.
# Le backend n'exige aucune feature mbedtls absente de owl_mbedtls_config.h :
# ECDH_C / ECDSA_C / SECP256R1|384R1|521R1 / CTR_DRBG_C sont tous presents
# (hard #error dans mbedtls.h sinon). Le Diffie-Hellman group est implemente en
# bignum brut (mbedtls_mpi), PAS via mbedtls_dhm -> MBEDTLS_DHM_C inutile.
# Config par defaut SECURE : MD5/DSA/RSA-SHA1/blowfish/RC4/3DES desactives sauf
# activation explicite (crypto_config.h).
#
# Sous-ensemble vendore : CSOURCES amont MOINS les 3 backends crypto non
# retenus (openssl.c, libgcrypt.c, os400qc3.c, wincng.c) ; seul mbedtls.c est
# compile. blowfish.c est present car #include par bcrypt_pbkdf.c (cle privee
# OpenSSH), pas compile separement. Ecarte aussi : les 4 headers de backend
# (openssl.h/libgcrypt.h/os400qc3.h/wincng.h), jamais atteints (crypto.h est une
# chaine if/elif ou seul mbedtls.h est pris sous LIBSSH2_MBEDTLS).
set(LIBSSH2_SRCS
    "${VENDOR_DIR}/libssh2/src/agent.c"
    "${VENDOR_DIR}/libssh2/src/bcrypt_pbkdf.c"
    "${VENDOR_DIR}/libssh2/src/chacha.c"
    "${VENDOR_DIR}/libssh2/src/channel.c"
    "${VENDOR_DIR}/libssh2/src/cipher-chachapoly.c"
    "${VENDOR_DIR}/libssh2/src/comp.c"
    "${VENDOR_DIR}/libssh2/src/crypt.c"
    "${VENDOR_DIR}/libssh2/src/global.c"
    "${VENDOR_DIR}/libssh2/src/hostkey.c"
    "${VENDOR_DIR}/libssh2/src/keepalive.c"
    "${VENDOR_DIR}/libssh2/src/kex.c"
    "${VENDOR_DIR}/libssh2/src/knownhost.c"
    "${VENDOR_DIR}/libssh2/src/mac.c"
    "${VENDOR_DIR}/libssh2/src/mbedtls.c"
    "${VENDOR_DIR}/libssh2/src/misc.c"
    "${VENDOR_DIR}/libssh2/src/packet.c"
    "${VENDOR_DIR}/libssh2/src/pem.c"
    "${VENDOR_DIR}/libssh2/src/poly1305.c"
    "${VENDOR_DIR}/libssh2/src/publickey.c"
    "${VENDOR_DIR}/libssh2/src/scp.c"
    "${VENDOR_DIR}/libssh2/src/session.c"
    "${VENDOR_DIR}/libssh2/src/sftp.c"
    "${VENDOR_DIR}/libssh2/src/transport.c"
    "${VENDOR_DIR}/libssh2/src/userauth.c"
    "${VENDOR_DIR}/libssh2/src/userauth_kbd_packet.c"
    "${VENDOR_DIR}/libssh2/src/version.c"
)
add_library(owl_libssh2 STATIC ${LIBSSH2_SRCS})
# PUBLIC = include/ seul (libssh2.h / libssh2_sftp.h / libssh2_publickey.h, noms
# prefixes libssh2_, non ambigus). src/ en PRIVATE : il contient des headers a
# noms generiques (session.h, packet.h, misc.h, transport.h, crypto.h...) qui,
# propages sur le search-path global d'owl_core via owl_vendor, masqueraient des
# headers systeme ou d'autres vendors (cf. le piege mpdecimal <io.h>/<bits.h>).
target_include_directories(owl_libssh2 PUBLIC "${VENDOR_DIR}/libssh2/include")
target_include_directories(owl_libssh2 PRIVATE "${VENDOR_DIR}/libssh2/src")
# owl_mbedtls en PRIVATE : compile mbedtls.c (backend) contre les headers mbedtls
# (propages via owl_mbedtls PUBLIC) et fournit les .a crypto au link final. Le
# lien reste interne au .a libssh2 : aucun consommateur d'owl_libssh2 n'a besoin
# des headers mbedtls (libssh2.h n'expose que des types opaques).
target_link_libraries(owl_libssh2 PRIVATE owl_mbedtls)
# LIBSSH2_MBEDTLS selectionne le backend crypto (crypto.h if/elif). PRIVATE : ne
# concerne que la compile des .c libssh2, jamais les consommateurs.
# Pas de define anti-dllexport a poser : libssh2.h n'emet __declspec(dllexport)
# que sous (_WIN32 && LIBSSH2_EXPORTS), et LIBSSH2_EXPORTS n'est jamais defini en
# statique -> LIBSSH2_API est vide, les libssh2_* n'entrent pas dans la table
# d'export PE (verifie : 0 export sur le smoke test, contrairement a llhttp).
target_compile_definitions(owl_libssh2 PRIVATE LIBSSH2_MBEDTLS NDEBUG)
# HAVE_* : sur Windows/mingw, libssh2_setup.h hand-crafte deja la config (le
# build zig windows-gnu ET le dev UCRT64 compilent sans aucun HAVE_). Sur Linux/
# macOS, setup.h ne pose rien hors HAVE_CONFIG_H -> on fournit les detections
# POSIX consommees par les sources (compile prouvee en cross linux-musl).
if(NOT WIN32)
    target_compile_definitions(owl_libssh2 PRIVATE
        HAVE_UNISTD_H HAVE_INTTYPES_H HAVE_SYS_SELECT_H HAVE_SYS_UIO_H
        HAVE_SYS_IOCTL_H HAVE_SYS_UN_H HAVE_POLL HAVE_O_NONBLOCK
        HAVE_FIONBIO HAVE_GETTIMEOFDAY
    )
endif()
# Hardening (calque zint/argon2/libxls) : NDEBUG (assert off), visibility hidden
# + sections separees pour que --gc-sections (herite au link owl) evacue tout le
# non-consomme tant qu'aucune native ne tape l'API (etat actuel : owl_libssh2 est
# lie a owl_vendor mais AUCUNE native ne le reference encore -> delta owl.exe ~0).
target_compile_options(owl_libssh2 PRIVATE
    -O2
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)

# --- SQLite (moteur SQL embarque, amalgamation single-file) ---
# THREADSAFE=1 (mode serialise) : protege par mutex interne, securise pour
# le futur multithread OWL. FTS5/JSON1/RTree pour rester moderne.
add_library(owl_sqlite STATIC "${VENDOR_DIR}/sqlite/sqlite3.c")
target_include_directories(owl_sqlite PUBLIC "${VENDOR_DIR}/sqlite")
target_compile_definitions(owl_sqlite PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_ENABLE_RTREE
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_PROGRESS_CALLBACK
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_USE_ALLOCA
)
target_compile_options(owl_sqlite PRIVATE -O2)

# --- DuckDB v1.4.4 LTS (moteur SQL OLAP embarque, submodule build) ---
# Build from source via scripts/build_duckdb_zig_native.sh (output dans
# _local/duckdb/<host>/install/). Toolchain zig clang+libc++, idem Qt6,
# ABI coherente avec le runtime OWL. Extensions linkees en statique :
# core_functions (SUM/MIN/MAX/AVG + 100+ fns scalaires), parquet (lecture/
# ecriture Parquet), json (json_extract...), icu (timezones, locales).
#
# Variables :
#   OWL_DUCKDB_INSTALL : chemin install (override pour CI / cross). Defaut
#   _local/duckdb/<host>/install relatif a CMAKE_SOURCE_DIR.
if(WIN32)
    set(_owl_duckdb_host "x86_64-windows")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_owl_duckdb_host "aarch64-macos")
    else()
        set(_owl_duckdb_host "x86_64-macos")
    endif()
elseif(OWL_MUSL_STATIC)
    # Profil musl-static universel : DuckDB recompile musl (install distinct
    # de x86_64-linux glibc). Cf scripts/build_duckdb_zig_native.sh OWL_MUSL=1.
    set(_owl_duckdb_host "x86_64-linux-musl")
else()
    set(_owl_duckdb_host "x86_64-linux")
endif()
set(OWL_DUCKDB_INSTALL "${CMAKE_SOURCE_DIR}/_local/duckdb/${_owl_duckdb_host}/install"
    CACHE PATH "Path to DuckDB submodule install (built via scripts/build_duckdb_zig_native.sh)")

if(NOT EXISTS "${OWL_DUCKDB_INSTALL}/.owl_install_complete")
    message(FATAL_ERROR
        "DuckDB submodule build manquant.\n"
        "  Lance : bash scripts/build_duckdb_zig_native.sh   (~15 min, one-time)\n"
        "  Cherche : ${OWL_DUCKDB_INSTALL}/.owl_install_complete")
endif()

# Cible INTERFACE : agrege libduckdb_static + extensions + third_party libs
# internes (zstd/mbedtls/yyjson/fmt/pg_query/re2/utf8proc/...).
add_library(owl_duckdb INTERFACE)
target_include_directories(owl_duckdb INTERFACE "${OWL_DUCKDB_INSTALL}/include")
target_compile_definitions(owl_duckdb INTERFACE DUCKDB_STATIC_BUILD)

# Ordre du link : extensions d'abord (referencent symboles dans duckdb_static),
# puis duckdb_static, puis third_party qu'il consomme. lld-link tolere l'ordre
# arbitraire sur archives (multi-pass), mais on garde l'ordre canon par
# habitude (utile sur ld classique).
target_link_libraries(owl_duckdb INTERFACE
    "${OWL_DUCKDB_INSTALL}/lib/libcore_functions_extension.a"
    "${OWL_DUCKDB_INSTALL}/lib/libparquet_extension.a"
    "${OWL_DUCKDB_INSTALL}/lib/libjson_extension.a"
    "${OWL_DUCKDB_INSTALL}/lib/libicu_extension.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_static.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_fastpforlib.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_fmt.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_fsst.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_hyperloglog.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_mbedtls.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_miniz.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_pg_query.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_re2.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_skiplistlib.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_utf8proc.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_yyjson.a"
    "${OWL_DUCKDB_INSTALL}/lib/libduckdb_zstd.a"
)
# DuckDB Linux build le jemalloc_extension separement (Windows non) :
# libduckdb_static.a y reference des symboles JemallocExtension::*, donc
# il faut linker libjemalloc_extension.a quand disponible.
if(EXISTS "${OWL_DUCKDB_INSTALL}/lib/libjemalloc_extension.a")
    target_link_libraries(owl_duckdb INTERFACE
        "${OWL_DUCKDB_INSTALL}/lib/libjemalloc_extension.a")
endif()
if(WIN32)
    target_link_libraries(owl_duckdb INTERFACE ws2_32 rstrtmgr bcrypt)
endif()

# --- llama.cpp / ggml b10442 (moteur d'inference LLM GGUF, CPU-only, MIT) ---
# Enabler bataille 2 (LLM local par GGUF, inference seule). Sous-ensemble cure
# vendore dans vendor/llama.cpp/ (ggml/ CPU-only + src/llama* + include/ ;
# backends GPU, examples/tests/tools/common elagues — cf vendor/VERSIONS.md).
# Build from source via scripts/build_llama_zig_native.sh (output dans
# _local/llama/<host>/install/). Meme toolchain zig clang+libc++ qu'owl.exe /
# DuckDB : ABI libc++ coherente. Baseline ISA x86_64_v2 STRICTE (SSE4.2, pas
# d'AVX2) -> tourne partout, Docker inclus.
#
# Il n'y a AUCUN consommateur OWL de cette lib pour l'instant (la surface
# IAModele.Charge/.Genere est le sous-chantier B). owl.exe la linke tout de
# meme (whole-archive ggml-cpu, cf infra) pour prouver la chaine de bout en
# bout ; llamacpp_smoketest est la preuve runtime.
if(WIN32)
    set(_owl_llama_host "x86_64-windows")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_owl_llama_host "aarch64-macos")
    else()
        set(_owl_llama_host "x86_64-macos")
    endif()
elseif(OWL_MUSL_STATIC)
    set(_owl_llama_host "x86_64-linux-musl")
else()
    set(_owl_llama_host "x86_64-linux")
endif()
set(OWL_LLAMA_INSTALL "${CMAKE_SOURCE_DIR}/_local/llama/${_owl_llama_host}/install"
    CACHE PATH "Path to llama.cpp build (built via scripts/build_llama_zig_native.sh)")

if(NOT EXISTS "${OWL_LLAMA_INSTALL}/.owl_install_complete")
    message(FATAL_ERROR
        "llama.cpp build manquant.\n"
        "  Lance : bash scripts/build_llama_zig_native.sh   (~3 min, one-time)\n"
        "  Cherche : ${OWL_LLAMA_INSTALL}/.owl_install_complete")
endif()

add_library(owl_llama INTERFACE)
target_include_directories(owl_llama INTERFACE "${OWL_LLAMA_INSTALL}/include")
# whole-archive UNIQUEMENT sur libggml-cpu.a : l'enregistrement du backend CPU
# (piege upstream #8166) est un appel gate sous #ifdef GGML_USE_CPU dans
# ggml-backend-reg.cpp — le linker peut dropper l'objet porteur, et notre
# hardening --gc-sections AGGRAVE le drop -> "no backend registered" au RUNTIME
# alors que le LINK a reussi. --whole-archive force l'inclusion complete de
# ggml-cpu ; verifie a l'execution par llamacpp_smoketest (ggml_backend_dev_
# count() >= 1 + device CPU present), PAS au link. no-whole-archive referme
# juste apres pour ne pas gonfler llama/ggml-base (tires par references).
# libllama.a en tete (consomme ggml), puis ggml, puis ggml-base.
target_link_libraries(owl_llama INTERFACE
    "${OWL_LLAMA_INSTALL}/lib/libllama.a"
    -Wl,--whole-archive
    "${OWL_LLAMA_INSTALL}/lib/libggml-cpu.a"
    -Wl,--no-whole-archive
    "${OWL_LLAMA_INSTALL}/lib/libggml.a"
    "${OWL_LLAMA_INSTALL}/lib/libggml-base.a"
)
if(UNIX AND NOT APPLE)
    # ggml-base tire dl (backend registry) et m (math) sur Linux.
    target_link_libraries(owl_llama INTERFACE ${CMAKE_DL_LIBS} m)
endif()

# --- libxml2 2.13.4 (vendorisé — DOM, XPath, namespace, validation XSD) ---
# Options minimales : UTF-8 seul, pas de réseau, pas de threads, pas de
# compression (zlib/lzma gérés ailleurs), pas de bindings Python/Perl.
set(LIBXML2_WITH_ICONV    OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_ZLIB     OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_LZMA     OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_PYTHON   OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_TESTS    OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_THREADS  OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_HTTP     OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_FTP      OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_LEGACY   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)   # force libxml2 static (no DLL)

# Workaround feature detection libxml2 sur Windows : les check_include_files
# de fcntl.h / sys/stat.h / unistd.h peuvent etre cached a FALSE par un
# build initial transitoirement defaillant (zig sysroot bootstrap incomplet
# au premier passage, etc.). Une fois cached false, CMake ne re-tente plus
# le check meme apres un make clean — la valeur stale persiste et provoque
# un build break (xmlIO.c _O_* undeclared, catalog.c O_RDONLY undeclared).
# Sur Windows ces headers sont toujours fournis par UCRT/MinGW/zig sysroot,
# on force le define en amont de add_subdirectory pour bypass le check
# fragile.
if(WIN32)
    set(HAVE_FCNTL_H    1 CACHE INTERNAL "fcntl.h toujours present UCRT/MinGW")
    set(HAVE_SYS_STAT_H 1 CACHE INTERNAL "sys/stat.h toujours present UCRT/MinGW")
    set(HAVE_UNISTD_H   1 CACHE INTERNAL "unistd.h toujours present UCRT/MinGW")
endif()

add_subdirectory("${VENDOR_DIR}/libxml2" EXCLUDE_FROM_ALL)
# encoding.c 3534/3599/3648 : init de char[N] avec une string de N+1 bytes
# (le \0 final est intentionnellement tronque, idiome historique libxml2).
# Vendor code, on suppress.
target_compile_options(LibXml2 PRIVATE -Wno-unterminated-string-initialization)

# --- zlib (static, deps de qpdf) ---
# zlib expose deux targets : `zlib` SHARED et `zlibstatic` STATIC.
# On utilise UNIQUEMENT zlibstatic. Le target SHARED est buildé inconditionnellement
# par le CMakeLists upstream mais EXCLUDE_FROM_ALL le retire du build par defaut.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${VENDOR_DIR}/zlib" EXCLUDE_FROM_ALL)

# --- zstd v1.5.7 (static, decompress-only —  catalogue icones) ---
# Vendoree pour le runtime catalogue icones embarque (~14 MB blob compresse
# de SVG Iconify a venir). Mode decompress-only : aucun compresseur dans
# owl.exe, la compression cote build-time sera faite par le tool gen du
# ticket feature. Economie ~400 KB sur owl.exe vs full lib.
#
# Le upstream zstd a son CMakeLists sous build/cmake/, qui reference les
# sources vendor/zstd/lib/ via un ../.. relatif. add_subdirectory pointe
# donc vers build/cmake. Cible produite : libzstd_static.
set(ZSTD_BUILD_PROGRAMS         OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB          OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED           OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC           ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_COMPRESSION      OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DECOMPRESSION    ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER      OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DEPRECATED       OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT         OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT    OFF CACHE BOOL "" FORCE)
add_subdirectory("${VENDOR_DIR}/zstd/build/cmake" EXCLUDE_FROM_ALL)
# Wrapper INTERFACE pour exposer un nom owl_* coherent + include dir vers
# vendor/zstd/lib/ (zstd.h / zstd_errors.h). Les clients linkent owl_zstd.
add_library(owl_zstd INTERFACE)
target_link_libraries(owl_zstd INTERFACE libzstd_static)
target_include_directories(owl_zstd INTERFACE "${VENDOR_DIR}/zstd/lib")

# --- libjpeg-turbo (static, deps de qpdf) ---
# libjpeg-turbo refuse add_subdirectory() upstream (jconfig.h generee, multi-precision
# wrappers, SIMD opt). On le build via ExternalProject_Add dans son propre sous-build,
# puis on expose un IMPORTED target `owl_jpeg`.
include(ExternalProject)
set(_jpeg_install ${CMAKE_BINARY_DIR}/vendor/owl_jpeg_install)
set(_jpeg_lib     ${_jpeg_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX})
set(_jpeg_args
    -DCMAKE_INSTALL_PREFIX=${_jpeg_install}
    -DCMAKE_BUILD_TYPE=Release
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=OFF
    -DWITH_TOOLS=OFF
    -DWITH_TESTS=OFF
    -DWITH_JAVA=OFF
)
if(CMAKE_TOOLCHAIN_FILE)
    # Resolu en ABSOLU : l'ExternalProject libjpeg configure dans son propre
    # build-dir ; un chemin relatif (ex. "cmake/toolchain_zig_native.cmake" passe
    # par le Makefile) n'y resout plus. Latent en build cache, revele par un
    # build frais (preuve D2).
    get_filename_component(_jpeg_tc "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    list(APPEND _jpeg_args "-DCMAKE_TOOLCHAIN_FILE=${_jpeg_tc}")
endif()
ExternalProject_Add(owl_jpeg_ep
    SOURCE_DIR        "${VENDOR_DIR}/libjpeg-turbo"
    PREFIX            "${CMAKE_BINARY_DIR}/vendor/owl_jpeg_build"
    CMAKE_ARGS        ${_jpeg_args}
    BUILD_BYPRODUCTS  "${_jpeg_lib}"
    INSTALL_DIR       "${_jpeg_install}"
    UPDATE_DISCONNECTED 1
)
file(MAKE_DIRECTORY "${_jpeg_install}/include")
add_library(owl_jpeg STATIC IMPORTED GLOBAL)
set_target_properties(owl_jpeg PROPERTIES
    IMPORTED_LOCATION "${_jpeg_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_jpeg_install}/include")
add_dependencies(owl_jpeg owl_jpeg_ep)

# --- qpdf (static, deps zlib + jpeg vendores, crypto natif, no doc, no install) ---
# Empeche qpdf de chercher zlib/jpeg via pkg-config (qui pointerait sur MSYS2 ou systeme).
# qpdf fait `include(FindPkgConfig)` direct (pas find_package), donc on neutralise
# en invalidant PKG_CONFIG_EXECUTABLE en cache : pkg_check_modules echouera
# silencieusement et tombera sur le fallback find_path/find_library qu'on a pre-set.
set(PKG_CONFIG_EXECUTABLE "PKG_CONFIG_DISABLED_FOR_OWL_VENDOR" CACHE FILEPATH "" FORCE)

# Options qpdf : static-only, no doc, no install, crypto natif (pas openssl/gnutls).
set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS         ON  CACHE BOOL "" FORCE)
set(BUILD_DOC                 OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG         OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE     OFF CACHE BOOL "" FORCE)
set(INSTALL_EXAMPLES          OFF CACHE BOOL "" FORCE)
set(INSTALL_MANUAL            OFF CACHE BOOL "" FORCE)
set(USE_IMPLICIT_CRYPTO       OFF CACHE BOOL "" FORCE)
set(REQUIRE_CRYPTO_NATIVE     ON  CACHE BOOL "" FORCE)

# Calcule le chemin litteral de l'archive zlibstatic (cross-platform).
get_target_property(_zlib_outname zlibstatic OUTPUT_NAME)
if(NOT _zlib_outname)
    set(_zlib_outname zlibstatic)
endif()
set(_zlib_archive "${CMAKE_BINARY_DIR}/vendor/zlib/${CMAKE_STATIC_LIBRARY_PREFIX}${_zlib_outname}${CMAKE_STATIC_LIBRARY_SUFFIX}")

# Pre-set des caches que find_path/find_library de qpdf consultent.
# Une fois la variable cache definie, find_path/find_library skippe toute recherche.
set(ZLIB_H_PATH      "${VENDOR_DIR}/zlib"        CACHE PATH     "" FORCE)
set(ZLIB_LIB_PATH    "${_zlib_archive}"          CACHE FILEPATH "" FORCE)
set(LIBJPEG_H_PATH   "${_jpeg_install}/include"  CACHE PATH     "" FORCE)
set(LIBJPEG_LIB_PATH "${_jpeg_lib}"              CACHE FILEPATH "" FORCE)

add_subdirectory("${VENDOR_DIR}/qpdf" EXCLUDE_FROM_ALL)

# Le `zconf.h` de zlib est genere dans le build-dir, qpdf doit le trouver via l'include
# (zlib.h fait #include "zconf.h"). Le source-dir suffit pour zlib.h, mais pas zconf.h.
target_include_directories(libqpdf_object SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/vendor/zlib")
target_include_directories(libqpdf        SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/vendor/zlib")
add_dependencies(libqpdf_object zlibstatic owl_jpeg_ep)
add_dependencies(libqpdf        zlibstatic owl_jpeg_ep)

# --- ODBC (backend SQL generique : HFSQL via son driver ODBC, Oracle, DB2,
#         Access, Sybase, et n'importe quel SGBD avec un driver ODBC) ---
#
# Windows : odbc32.dll est present nativement sur tout poste Windows depuis
#           Win 2000. On link dynamiquement -lodbc32. Zero submodule, zero
#           build externe, zero install user, 0 octet ajoute au binaire.
#
# Linux / macOS : pas de DriverManager vendore. Le Provider sqlODBC generique
#           resout libodbc.so.2 en runtime via dlopen (lib systeme de
#           l'utilisateur, cf owl_odbc_vt_load_default) — rien n'est embarque
#           dans le binaire pour l'ODBC generique. sqlHFSQL utilise iODBC
#           vendore (headers ci-dessous).
add_library(owl_odbc INTERFACE)
set(OWL_HAS_ODBC ON CACHE BOOL "ODBC backend disponible" FORCE)
target_compile_definitions(owl_odbc INTERFACE OWL_HAS_ODBC=1)

if(WIN32)
    # sqlODBC generique : odbc32.dll natif Windows (rien a vendor).
    target_link_libraries(owl_odbc INTERFACE odbc32 odbccp32)
    message(STATUS "ODBC: Windows odbc32.dll (natif, 0 octet ajoute)")
else()
    # sqlODBC generique sur Linux/macOS : depend du DriverManager ODBC systeme
    # (libodbc.so.2). Resolu en runtime via dlopen — pas de link statique ici.
    # Si l'user n'a pas de DriverManager installe, le runtime sqlODBC echouera
    # proprement avec un message clair (cf owl_odbc_vt_load_default). sqlHFSQL
    # (cf ci-dessous) n'en depend pas : il utilise iODBC vendore.
    target_include_directories(owl_odbc INTERFACE
        "${VENDOR_DIR}/iodbc/include")
    message(STATUS "ODBC: Linux/macOS — headers vendores iODBC, "
                   "runtime dlopen sur libodbc.so.2 (DriverManager systeme) ou "
                   "libiodbc.so.2 (sqlHFSQL).")
endif()

# --- HFSQL ODBC driver embedded (pack optionnel, build prive) ---
# Mecanisme generique d'embarquement d'un pack de bibliotheques dans le
# binaire. Au build, un sous-ensemble de fichiers est extrait du pack
# source, recompresse en tar.lzma (gain ~55% sur l'archive), puis
# objcopy-e en .o linkable.
#
# Au premier usage de cnx..Provider = sqlHFSQL, owl_hfsql_install.c
# decompresse le blob, parse le tar et ecrit les fichiers dans le dossier
# d'install utilisateur (LOCALAPPDATA Windows / XDG_DATA_HOME Linux). La
# bibliotheque est ensuite chargee directement via LoadLibrary/dlopen.
#
# Sur Linux, des dependances link-time supplementaires (libiodbc) sont
# packees avec et pre-chargees via dlopen RTLD_GLOBAL avant la lib
# principale (cf owl_odbc_vt_load_hfsql_direct).
#
# Les noms version-stampes du pack + des bibliotheques vivent dans un
# fichier PRIVE (cmake/hfsql_embed_local.cmake, exclu de l'export). Present
# au build prive -> definit HFSQL_PACK_SRC / HFSQL_PACK_FILES /
# HFSQL_OBJCOPY_FORMAT / HFSQL_EXTRA_FILES. Absent au build public -> ces
# variables restent vides, HFSQL_PACK_SRC pointe nulle part, et le pack
# n'est pas embarque (OWL_HAS_HFSQL_EMBED=0, branche elseif ci-dessous).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hfsql_embed_local.cmake" OPTIONAL)

set(HFSQL_PACK_OBJ "${CMAKE_BINARY_DIR}/hfsql_pack.o")
find_program(XZ_EXECUTABLE xz)
if(EXISTS "${HFSQL_PACK_SRC}" AND XZ_EXECUTABLE)
    set(HFSQL_EXTRACT_DIR "${CMAKE_BINARY_DIR}/hfsql_extract")
    set(HFSQL_TAR         "${CMAKE_BINARY_DIR}/hfsql_pack.tar")
    set(HFSQL_TARLZMA     "${CMAKE_BINARY_DIR}/hfsql_pack.tar.lzma")

    # Pipeline build :
    #  1. extract des fichiers selectionnes depuis le ZIP source du driver
    #  2. copie des extras (libiodbc Linux) dans le dossier d'extract
    #  3. tar non-compresse via cmake -E tar
    #  4. xz --format=lzma --extreme : compression LZMA1
    #  5. objcopy --input binary : embed .tar.lzma comme blob lie
    set(_extra_copy_cmds "")
    set(_extra_tar_files "")
    foreach(f IN LISTS HFSQL_EXTRA_FILES)
        get_filename_component(fname "${f}" NAME)
        # Sur Linux on renomme libiodbc.so.2.1.20 -> libiodbc.so.2
        # (le soname attendu par le driver). Idem libiodbcinst.
        if(fname MATCHES "^(libiodbc(inst)?)\\.so\\.")
            set(target_name "${CMAKE_MATCH_1}.so.2")
        else()
            set(target_name "${fname}")
        endif()
        list(APPEND _extra_copy_cmds
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${f}" "${HFSQL_EXTRACT_DIR}/${target_name}")
        list(APPEND _extra_tar_files "${target_name}")
    endforeach()

    add_custom_command(
        OUTPUT "${HFSQL_TARLZMA}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${HFSQL_EXTRACT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${HFSQL_EXTRACT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${HFSQL_EXTRACT_DIR}"
                ${CMAKE_COMMAND} -E tar xf "${HFSQL_PACK_SRC}"
                -- ${HFSQL_PACK_FILES}
        ${_extra_copy_cmds}
        COMMAND ${CMAKE_COMMAND} -E chdir "${HFSQL_EXTRACT_DIR}"
                ${CMAKE_COMMAND} -E tar cf "${HFSQL_TAR}"
                -- ${HFSQL_PACK_FILES} ${_extra_tar_files}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${HFSQL_TARLZMA}"
        COMMAND ${XZ_EXECUTABLE} --format=lzma --extreme --keep
                --compress "${HFSQL_TAR}"
        DEPENDS "${HFSQL_PACK_SRC}" ${HFSQL_EXTRA_FILES}
        COMMENT "HFSQL: extract + repack tar.lzma (target ${HFSQL_OBJCOPY_FORMAT})"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${HFSQL_PACK_OBJ}"
        COMMAND objcopy -I binary -O ${HFSQL_OBJCOPY_FORMAT} -B i386:x86-64
                --rename-section .data=.rodata,alloc,load,readonly,data,contents
                hfsql_pack.tar.lzma hfsql_pack.o
        DEPENDS "${HFSQL_TARLZMA}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Embed HFSQL pack (tar.lzma) via objcopy"
        VERBATIM
    )
    add_library(owl_hfsql_pack STATIC "${HFSQL_PACK_OBJ}")
    set_source_files_properties("${HFSQL_PACK_OBJ}" PROPERTIES
        EXTERNAL_OBJECT TRUE GENERATED TRUE)
    set_target_properties(owl_hfsql_pack PROPERTIES LINKER_LANGUAGE C)
    target_compile_definitions(owl_odbc INTERFACE OWL_HAS_HFSQL_EMBED=1)
    target_link_libraries(owl_odbc INTERFACE owl_hfsql_pack owl_lzma)
    if(NOT WIN32)
        target_link_libraries(owl_odbc INTERFACE ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "HFSQL: pack embedded (${CMAKE_SYSTEM_NAME}, tar.lzma)")
elseif(NOT EXISTS "${HFSQL_PACK_SRC}")
    target_compile_definitions(owl_odbc INTERFACE OWL_HAS_HFSQL_EMBED=0)
    message(WARNING "HFSQL: ${HFSQL_PACK_SRC} absent, pack non embed.")
else()
    target_compile_definitions(owl_odbc INTERFACE OWL_HAS_HFSQL_EMBED=0)
    message(WARNING "HFSQL: xz CLI introuvable au PATH, pack non embed. "
                    "Installer xz (msys2: pacman -S xz / debian: apt install xz-utils).")
endif()

# Vendor meta-target (lib agrégée) — partagée par owl_core (toutes cibles).
# Le stack SQL (sqlite/duckdb/odbc) est volontairement EXCLU d'ici : il vit
# dans owl_vendor_sql, lié uniquement à owl_sql (donc owl.exe), pas owl-cli.
# duckdb_static seul pèse ~71 MB : owl-cli (benchmarks langage pur) n'en a
# pas besoin.
add_library(owl_vendor INTERFACE)
target_link_libraries(owl_vendor INTERFACE owl_mbedtls owl_miniz owl_lzma owl_pcre2 owl_ca owl_md4c owl_zstd owl_mpdecimal owl_zint owl_argon2 owl_llhttp owl_libxls owl_libssh2 LibXml2::LibXml2)

# Vendor SQL — sqlite + duckdb + odbc. Lié seulement à owl_sql (cf plus bas),
# donc présent dans owl.exe mais absent d'owl-cli.
add_library(owl_vendor_sql INTERFACE)
target_link_libraries(owl_vendor_sql INTERFACE owl_sqlite owl_duckdb owl_odbc)

# ------------------------------------------------------------------
# OWL runtime core (lib statique partagée par owl/owl-cli)
#
# INVARIANT : owl_core est compilee UNE SEULE FOIS, SANS
# OWL_HAS_GUI, et linkee a la fois par owl.exe (avec Qt) ET par
# owl-cli.exe (sans Qt, variante benchmark). Consequences strictes :
#
#   1. AUCUNE source de cette liste ne doit referencer du Qt
#      (`#include <Q...>`, identifiants type QApplication/QWidget/...).
#      Si vous ajoutez du code Qt-dependant, il va dans les fichiers
#      per-target (owl_stdlib.c / stdlib_runtime.c — compiles deux fois,
#      avec et sans flag) ou directement dans les sources GUI dediees
#      (stdlib_ui.c / ui_qt.cpp / stdlib_pdf.c / pdf_qt.cpp) qui ne
#      sont PAS dans OWL_CORE_SRCS.
#
#   2. AUCUNE source de cette liste ne doit utiliser `#ifdef OWL_HAS_GUI`.
#      Le flag n'est jamais defini ici donc toute branche conditionnee
#      est garantie morte a la compile (pieges historiques  +
#      ). Les seuls fichiers legitimes pour ce ifdef sont les
#      per-target listes ci-dessus et leurs headers d'interface
#      (owl_stdlib.h / owl_ui.h / owl_pdf.h / stdlib_ui.h / stdlib_pdf.h).
#
# Ces deux invariants sont verifies automatiquement au build par le
# garde-fou `owl_core_guard` (tools/owl_core_guard/). Echec = build
# rouge avec message pointant le fichier/ligne coupable.
# ------------------------------------------------------------------
set(OWL_CORE_SRCS
    src/platform.c
    src/scanner.c
    src/memory.c
    src/value.c
    src/object.c
    src/chunk.c
    src/table.c
    src/debug.c
    src/debug_api.c
    src/debug_stdio.c
    # JSON helper partage entre LSP, DAP et le debug stdio interne
    # (mutualise via owl_core). owl-lsp en garde encore une copie locale
    # par historique — pas de conflit car owl-lsp ne link pas owl_core.
    lsp/lsp_json.c
    src/compiler.c
    src/vm.c
    src/gc.c
    src/mock.c
    # owl_stdlib.c est compile par target (owl vs owl-cli) car contient
    # un #ifdef OWL_HAS_GUI conditionnel qui change le linkage final.
    src/owl_registry.c
    src/owl_name_normalize.c
    src/owl_limits.c
    src/owl_errors.c
    # Pont compile-aware du registry erreurs : linke uniquement avec
    # owl_core (a besoin de compiler.c). Pas inclus dans owl-lsp.
    src/owl_errors_compile.c
    # Pont runtime-aware du registry erreurs : linke uniquement avec
    # owl_core (a besoin de vm.c). Pas inclus dans owl-lsp.
    src/owl_errors_runtime.c
    # Pont native-aware du registry erreurs ( Lot 4) : linke
    # uniquement avec owl_core (a besoin de stdlib_erreur.c). Pas inclus
    # dans owl-lsp.
    src/owl_errors_native.c
    src/registry_self_test.c
    src/encoding_self_test.c
    src/stdlib_io.c
    # stdlib_runtime.c est compile par target (owl vs owl-cli) car contient
    # les ifdefs OWL_HAS_GUI / OWL_HAS_QPDF pour OwlInfoLibrairies.
    src/stdlib_string.c
    src/string_compare_opts.c
    src/stdlib_math.c
    src/stdlib_array.c
    src/stdlib_vecteur.c
    src/stdlib_matrice.c
    src/stdlib_tenseur.c
    # Pool de threads COMPUTE fork-join "parallel-for" : threade les
    # kernels chauds (produit matriciel, convolution) sur des double* bruts.
    # API OS (CreateThread / pthread), aucune dependance nouvelle, aucun Obj*
    # ne franchit la frontiere de thread (isolation VM/GC).
    src/owl_parallel.c
    src/stdlib_iamodele.c
    # Arbre de decision CART : premier modele ML classique, socle du
    # patron d'estimateur tabulaire (types distincts + verbes .Entraîne/.Prédit).
    src/stdlib_mlarbre.c
    # Modeles lineaires : MLRegressionLineaire (OLS forme close, Gauss)
    # et MLRegressionLogistique (softmax + descente de gradient).
    src/stdlib_mllineaire.c
    # Clustering non-supervise : MLKMeans (Lloyd + init k-means++,
    # PRNG local seede par ..Graine pour reproductibilite bit-exacte).
    src/stdlib_mlcluster.c
    # Lecteur protobuf ONNX minimal/securise : decode le sous-schema
    # necessaire a l'import d'un reseau de classif sequentiel. Module neutre
    # (aucune dependance VM/GC), consomme par stdlib_iamodele.c (.ChargeONNX).
    src/onnx_import.c
    # Writer protobuf ONNX minimal (, lot 2) : serialise un IAModele en
    # `.onnx` (inverse de onnx_import.c). Module neutre (aucune dependance
    # VM/GC), consomme par stdlib_iamodele.c (.SauveONNX).
    src/onnx_export.c
    src/stdlib_dataset.c
    # Primitives de detection d'objets : IoU + SuppressionNonMax.
    # Module neutre (aucune dependance VM/GC hors tableau resultat), socle du
    # pipeline de detection au-dessus du Tenseur/IAModele.
    src/stdlib_detection.c
    src/stdlib_xls.c
    src/stdlib_llm.c
    # serve-llm : API LLM locale compatible OpenAI (POST
    # /v1/chat/completions SSE + GET /v1/models). Compose le socle streaming
    # mode C (http_exec) et le seam C VM-free du moteur (stdlib_llm.h). Aucune
    # native ni surface de langage : un handler C au-dessus du transport.
    src/serve_llm.c
    src/stdlib_file.c
    src/project.c
    src/project_run.c
    src/prescan.c
    src/testing.c
    src/formatter.c
    src/formatter_canon_registry.c
    src/formatter_context.c
    src/owl_readline.c
    src/stdlib_json.c
    src/owl_methods.c
    src/stdlib_date.c
    src/stdlib_date_parsers.c
    src/stdlib_crypto.c
    src/stdlib_uuid.c
    src/stdlib_i18n.c
    src/stdlib_parametre.c
    src/stdlib_ini.c
    src/sha3.c
    src/ripemd.c
    src/blake2.c
    src/stdlib_regex.c
    src/stdlib_http.c
    src/stdlib_http_service.c
    src/stdlib_ftp.c
    # Fondations courriel (, Lot 0) : types Email / emailSessionSMTP /
    # emailAttache + constantes (priorite, confidentialite, options). Types de
    # donnees seuls, pas de transport SMTP ni d'encodage MIME (lots ulterieurs).
    src/stdlib_email.c
    #  : import .msg Outlook (Lot 5) — conteneur OLE2/CFBF via libxls
    # (reutilisation du header public libxls/ole.h), couche MAPI + LZFu + RTF->HTML.
    src/stdlib_email_msg.c
    src/aws_service_models.c
    #  : signeur SigV4 pur (mbedTLS, aucune dependance VM) + couche
    # VM-aware (native AWSRequête, enum awsService, cadrage protocole).
    src/stdlib_aws.c
    src/stdlib_aws_native.c
    src/stdlib_bitwise.c
    src/stdlib_similarity.c
    src/stdlib_reflect.c
    src/stdlib_console.c
    src/stdlib_eval.c
    src/stdlib_erreur.c
    src/stdlib_timer.c
    src/stdlib_compression.c
    src/stdlib_color.c
    src/stdlib_markdown.c
    src/stdlib_text_riche.c
    src/stdlib_mock.c
    src/stdlib_xml.c
    # stdlib_builtin_types.c est compile par target (owl/owl-cli/runtimes)
    # car il enregistre conditionnellement les types SQL (sqlConnexion/
    # sqlRequete/sqlReponse) sous #ifdef OWL_HAS_SQL — absents d'owl-cli.
    src/stdlib_image.c
    src/stdlib_icones.c
    src/stdlib_codebarres.c
    src/stdlib_theme.c
    src/owl_image_stb.c
    # Encodeurs MIME internes : Quoted-Printable + mots-encodes
    # RFC 2047. C pur, aucune native ni registre ; servent la fabrique/le
    # parseur email des lots suivants. Self-test joue par --self-test-net.
    src/mime_codec.c
    src/internal_tests/mime_codec_test.c
    # --- Couche réseau pure (réutilisée par HTTP + SQL) — reste dans le core ---
    src/net_be_buffer.c
    src/internal_tests/net_be_buffer_test.c
    src/net_le_buffer.c
    src/internal_tests/net_le_buffer_test.c
    src/net_tcp.c
    src/internal_tests/net_tcp_test.c
    src/net_tcp_tls.c
    src/internal_tests/net_tcp_tls_test.c
    # Pair TCP de test controlant les octets emis et le decoupage des
    # write(). Consomme par le self-test du parsing de reponse HTTP
    # et, a terme, par le corpus d'attaque du serveur HTTP.
    src/net_mock_peer.c
    src/internal_tests/http_llhttp_integration_test.c
    # Chemin CONNECT du client : proxy simule + amont TLS reel,
    # seule facon d'exercer proxy_connect_tunnel, qui n'est emprunte que
    # pour une cible HTTPS.
    src/internal_tests/http_proxy_connect_test.c
    # Precedence proxy explicite / environnement : trois pairs
    # aux corps distincts, le corps rendu designe le chemin pris. En clair
    # (pas de CONNECT) : c'est resolve_proxy qu'on mesure, pas le tunnel.
    src/internal_tests/http_proxy_precedence_test.c
    # Jar de cookies + suivi des Location : pair local
    # multi-tours, aucune native mockee. Le jar et resolve_redirect sont
    # static : la native est le seul point d'entree reel.
    src/internal_tests/http_jar_redirect_test.c
    # Serveur HTTP, phase A0.1 : socket serveur a echeances
    # (poll/WSAPoll) + transport HTTP/1.1 sur llhttp. Aucune native, aucune
    # surface de langage — ce module ne connait pas la VM.
    src/net_tcp_server.c
    src/http_server.c
    src/internal_tests/http_server_corpus_test.c
    # Cœur réacteur, chantier W1 : boucle poll-many + roue de
    # timers, mécanique événementielle PURE (aucune logique HTTP, aucune
    # VM). Fondation des chantiers W2-W5 ; pas encore branché sur le
    # serving (le pool reste maître). Isolation GC par construction :
    # 100 % C-heap, aucun Obj*.
    src/net_reacteur.c
    src/internal_tests/net_reacteur_test.c
    # Reveil inter-thread self-pipe, chantier W4 : paire de
    # sockets loopback portable, 1 octet ecrit depuis un thread tiers
    # pour reveiller la boucle, draine cote boucle. 100 % C-heap, aucun
    # Obj* ne franchit la frontiere de thread (isolation GC §8 Q1).
    src/net_reveil.c
    src/internal_tests/net_reveil_test.c
    # Executeur HTTP, phase A0.2 : file bornee, thread
    # proprietaire unique de la VM, marshalling, type opaque de reponse.
    # Seule moitie du serveur qui connait la VM — le transport n'en voit
    # que le type opaque.
    src/http_exec.c
    src/internal_tests/http_exec_test.c
    # Socle Message Queue, E1 « pont de livraison » : registre
    # d'abonnements (handlers OWL tenus comme racines GC) + livraison C->OWL
    # + ancrage reacteur (lecture -> deliver). AGNOSTIQUE du protocole : E1
    # recoit un payload deja delimite, ne parse rien. Seule moitie du socle
    # qui connait la VM ; net_reacteur reste 100 % C-heap (isolation GC).
    src/stdlib_mq.c
    src/internal_tests/mq_livraison_test.c
    # Client MQTT 3.1.1, premier client protocolaire sur E1 :
    # type mqttSession + MQTTConnecte/MQTTDéconnecte + test wire (mock broker).
    src/stdlib_mqtt.c
    src/internal_tests/mqtt_connexion_test.c
    # Client AMQP 0-9-1 / RabbitMQ, 2e client protocolaire sur E1 :
    # type amqpConnection + AMQPConnecte/AMQPDéconnecte (Lot A). Wire recode
    # maison (frames method/header/body, field-tables, SASL PLAIN).
    src/stdlib_amqp.c
    # Client STOMP 1.2, 3e client protocolaire sur E1 : types
    # stompSession/stompMessage + STOMPConnecte/Déconnecte/Publie/Abonne/Attend.
    # Wire texte recode maison (frames COMMANDE\nheader:valeur\n\ncorps\0).
    src/stdlib_stomp.c
    # Client NATS, 4e client protocolaire sur E1 : types
    # natsConnexion/natsMessage + NATSConnecte/Déconnecte/Publie/Abonne/Attend.
    # Wire texte recode maison (lignes INFO/CONNECT/PING/PONG/PUB/SUB/MSG \r\n).
    src/stdlib_nats.c
    # Fondation runtime du DSL de service,  : registre de routes
    # runtime (survit au reset de compilation) + bout-en-bout minimal
    # (compile surRequete -> route -> handler -> reponse, MoiMême = reponse).
    src/http_service_runtime.c
    #  — cache de cles JWKS (rotation) + fetch cache-first borne sur le
    # chemin d'auth JWT. Touche uniquement par le thread VM (aucun verrou).
    src/jwks_cache.c
    src/internal_tests/http_dsl_service_test.c
    #  P3 — 3e mode d'execution (flux) du DSL de service : une route
    # Flux=fluxSSE/fluxOctets streame E2E via execute_service_job (ouverture
    # paresseuse au 1er FluxEcrit). Digue test-first ; comme la famine ci-dessus,
    # volontairement HORS de run_all_checks.sh tant que la Piece #1 n'a pas rendu
    # les CHECK verts (cf. en-tete du fichier). Cablee dans la batterie APRES le
    # vert, jamais l'inverse.
    src/internal_tests/http_service_flux_test.c
    # `owl serve` : montage compile -> executeur owning-VM ->
    # listener reel binde -> boucle accept. La GLUE qui rend un ServiceREST
    # lancable ; self-test bout-en-bout sur un vrai port + vrai client TCP.
    src/owl_serve.c
    src/internal_tests/http_serve_test.c
    # Pool de workers reseau, phase A1.1b : N connexions
    # menees en parallele derriere la MEME file bornee et le MEME
    # executeur. Niveau 1 de concurrence — la VM reste possedee par un
    # seul thread, ce que la famille C4 mesure.
    src/internal_tests/http_concurrence_test.c
    # Digue anti-famine VM,  (LOT 0) : verrouille le contrat de
    # determinisme  §3 AVANT le pool N-threads + GIL cooperatif
    # (LOTs 1-4). Rouge aujourd'hui pour la bonne raison (executeur
    # mono-thread hx_thread_fn) ; volontairement NON cablee dans
    # --self-test-net (cf. en-tete du fichier) tant que le mecanisme
    # qu'elle verifie n'existe pas.
    src/internal_tests/vm_concurrence_famine_test.c
    # Gardes stress concurrence/ressource,  : famine fd, spin
    # POLLOUT (+ back-pressure), UAF close-mid-stream, fuites fd/refcount,
    # et le gate anti-hang (repro automatise du hang serve-llm contre le
    # pool actuel). Digue test-first posee AVANT le reacteur W2.
    src/internal_tests/http_stress_gardes_test.c
    # Streaming HTTP mode C,  : corps de reponse incremental
    # (Transfer-Encoding: chunked en sortie) via file bornee VM->transport.
    # La VM POUSSE, le transport DRAINE ; socle generique pour serve-llm.
    src/internal_tests/http_stream_test.c
    # Native FluxEcrit + slot ambiant flux courant + park/restore,  P2 :
    # surface OWL du DSL de service streaming (framing SSE data:, emit-and-continue).
    src/internal_tests/http_flux_ecrit_test.c
    # serve-llm,  : routage OpenAI + framing SSE via faux generateur
    # injecte (ServeLlmCtx.generate), sans le moindre GGUF. Le seam C reel
    # (owl_llm_*) y est teste a part, gate par OWL_LLAMA_TEST_GGUF.
    src/internal_tests/serve_llm_test.c
    # tool-calling,  : convertisseur schema JSON -> GBNF (Voie B).
    # Model-free : parse un schema (json_parse_cstr) puis asserte la GBNF par
    # sous-chaines. Les familles serve-llm tool-calling vivent dans serve_llm_test.c.
    src/internal_tests/llm_tool_grammar_test.c
    # TLS serveur, phase A1.2 : handshake non bloquant
    # entrelace avec poll(), contexte mbedTLS POSSEDE par worker (aucun
    # objet mbedTLS mutable partage, donc pas de MBEDTLS_THREADING_C et
    # aucun define global). Le routage vit dans net_tcp_server.c : les
    # couches du dessus ne savent pas que le transport est chiffre.
    src/net_tls_server.c
    src/internal_tests/http_tls_test.c
    # TLS serveur pilote par le reacteur, chantier W3 :
    # handshake + I/O chiffres menes sans poll interne, uniquement par
    # re-pompage sur evenement de la boucle W1. Vrai handshake mbedTLS sur
    # socket loopback, client sur thread tiers. Montage que W2 consommera.
    src/internal_tests/net_tls_reacteur_test.c
    # Machine a etats service HTTP, chantier W2 : gates
    # comportementaux boite-noire (fast-path controle, back-pressure
    # ecriture, keep-alive/pipelining via l'executeur, trou de couverture
    # cible du reset d'etat en pipeline) poses AVANT la reecriture de la
    # boucle de service. Certains sont ROUGES tant que l'impl vit dans
    # owl_http_serveur_sert_connexion_acceptee (boucle bloquante).
    src/internal_tests/http_reacteur_etats_test.c
    # Flux SSE dans la boucle reacteur, chantier W5 : digue
    # test-first AVANT l'integration du mode C flux au reacteur.
    # Aujourd'hui le data-plane streaming est dispatche a des workers
    # BLOQUANTS (net_tcp_server.c::tp_reac_worker) ; le discriminant prouve
    # qu'un flux long retient son worker (ROUGE) tant que le reacteur ne
    # drainera pas via POLLOUT sans dispatch worker (VERT attendu apres W5).
    src/internal_tests/http_reacteur_stream_test.c
    # Annulation de generation a la deconnexion client,  : digue
    # test-first model-free (producteur C paced, PAS de LLM) qui mesure la
    # PROMPTITUDE de la detection d'abandon (nombre de chunks perdus avant
    # ABANDON) et l'absence de spirale (N generations abandonnees ne
    # ralentissent pas la suivante) contre l'executeur mono-thread reel.
    src/internal_tests/http_llm_annulation_test.c
    # Repro ISOLE (hors --self-test-net par defaut) du CRASH race streaming
    # concurrent,  priorite 1 : N flux via l'executeur async (chemin
    # de production W7) + fermeture RST brutale mid-stream en boucle. Point
    # d'entree separe --self-test-net-crash-1723 (cf. main.c) : un crash tue
    # le process hote, donc PAS dans net_familles tant que la race n'est pas
    # corrigee (casserait --self-test-net pour tous les agents en parallele).
    src/internal_tests/http_llm_annulation_crash_test.c
    # Politique HTTP, phase A0.3 : frontiere de securite des
    # chemins, service statique (ETag / If-Modified-Since), allow-list de
    # Host. L'UN SEUL endroit ou vit ce qui depend de la configuration —
    # exigence du contrat §6, sans quoi la revue de securite a deux
    # endroits a regarder au lieu d'un.
    src/http_politique.c
    src/internal_tests/http_politique_test.c
    src/auth_scram.c
    src/internal_tests/auth_scram_test.c
    # Client FTP en clair,  : serveur mock STATEFUL multi-tours
    # (banniere/USER/PASS/FEAT/PASV/MLSD/RETR/STOR) + e2e du coeur protocole
    # VM-free (owl_ftp_*). Le net_mock_peer mono-payload ne peut pas mener le
    # dialogue PASV (2e canal de donnees annonce) ; ce mock, modele sur
    # sql_postgres_mock_server.c, le peut. Aucune native mockee : c'est la
    # vraie preuve du protocole (le .owl mocke chaque native, il ne le teste pas).
    src/ftp_mock_server.c
    src/internal_tests/ftp_e2e_test.c
    # --- Stack SQL (stdlib_sql + drivers + leurs tests) : migré dans la lib
    #     owl_sql (OWL_HAS_SQL). Absent d'owl-cli. Cf OWL_SQL_SRCS plus bas. ---
    src/compress_ae2.c
    src/compress_aes_zip.c
    src/compress_7z.c
    src/ui_decl.c
    src/ui_catalog.c
    src/ui_presentation.c
    src/etat_decl.c
    src/service_decl.c
    src/http_route.c
    # Generateur OpenAPI 3 derive de l'AST ServiceREST/OperationHTTP :
    # source de verite unique (service_decl), consomme par `owl openapi` et la
    # route GET /openapi.json. Module neutre (aucune dependance VM/GC/Qt).
    src/http_openapi.c
    # Parseur multipart/form-data du DSL de service : decoupe un
    # corps bufferise en parties (metadonnees + contenu), sans objet VM.
    src/http_multipart.c
    src/owl_event_catalog.c
    src/event_catalog_widgets.c
    src/owl_keyword_catalog.c
    src/owl_operator_catalog.c
    src/owl_globalvar_catalog.c
    # CLI doc — palette ANSI + embed des fondations/scénarios.
    # embedded_docs.c est généré au build (cf. add_custom_command plus bas).
    src/cli_colors.c
    src/cli_doc.c
    src/cli_mcp.c
    src/internal_tests/cli_doc_test.c
    src/internal_tests/cli_mcp_test.c
    src/internal_tests/owl_errors_test.c
    src/internal_tests/owl_errors_compile_test.c
    src/internal_tests/owl_errors_runtime_test.c
    src/internal_tests/owl_errors_native_test.c
    src/internal_tests/multi_instance_test.c
    src/internal_tests/test_runner_test.c
    src/internal_tests/cli_ops_test.c
    src/internal_tests/translate_test.c
    src/internal_tests/fmt_stability_test.c
    src/internal_tests/fmt_canon_test.c
    #  — separe de fmt_stability_test.c : ce harnais compile, et
    # fmt_stability_test.c est aussi linke dans owl-lsp-test.exe (sans
    # compilateur).
    src/internal_tests/fmt_contract_test.c
    src/internal_tests/compile_error_test.c
    src/internal_tests/doc_phantom_lint_test.c
    src/internal_tests/signature_test.c
    src/internal_tests/temporal_parity_test.c
    src/internal_tests/vm_dispatch_test.c
    src/internal_tests/compile_split_test.c
    src/internal_tests/format_projet_leak_test.c
    src/internal_tests/const_index_test.c
    src/internal_tests/go_local_test.c
    src/internal_tests/repl_completion_test.c
    src/internal_tests/prescan_depth_test.c
    src/internal_tests/collection_prescan_guard_test.c
    src/internal_tests/block_opener_guard_test.c
    src/internal_tests/catalog_bilingual_test.c
    src/internal_tests/debug_locals_test.c
    src/internal_tests/multilingue_print_test.c
    src/internal_tests/case_accent_guard_test.c
    # Math pure du centrage + clamp de position de fenetre.
    # Helper dans owl_core (sans Qt) + son self-test unitaire.
    src/ui_position.c
    src/internal_tests/ui_position_test.c
    src/internal_tests/xls_styles_test.c
    src/internal_tests/xls_preservation_test.c
    src/internal_tests/xls_table_export_test.c
    "${CMAKE_BINARY_DIR}/embedded_docs.c"
    # Catalogue d'icones genere au build par tools/icons_catalog_gen
    # ( Lot 2a-i). Code dormant a ce stade : le resolver
    # stdlib_icones.c ne reference pas encore OWL_ICONS_CATALOG, ce
    # cablage arrive en Lot 2a-ii. On le link maintenant pour valider
    # la chaine build complete (tool host + custom_command + emission C).
    "${CMAKE_BINARY_DIR}/icons_catalog_gen.c"
    # Naming canon FR/EN icones genere au build par tools/icons_canon_gen
    #. Parse tools/icons_canon_fr_en/mapping.def et emet la
    # table OWL_ICONS_FR_EN_MAPPING utilisee par owl_icones_fr_to_en.
    "${CMAKE_BINARY_DIR}/icons_canon_gen.c"
    src/icons_canon.c
    #  - Dump SVG/JSON catalogue Iconify pour le plugin VSCode
    # (designer panel browse icons). Phase 0a chantier task #18.
    src/cli_gen_assets.c
)

# ------------------------------------------------------------------
# Scoping licence : les self-tests internes (src/internal_tests/) sont des
# OUTILS de test (« liste B » : AGPL pur), pas du code embarque dans les exe
# utilisateur (« liste A »). On les EXTRAIT de l'archive owl_core (reutilisee
# telle quelle pour builder les exe utilisateur via owl_runtime_console/gui)
# vers une cible dediee owl_internal_tests, linkee uniquement dans les
# binaires de dev (owl.exe / owl-cli). Le linker les elimine deja en dead-code
# d'un exe utilisateur (main.c seul les appelle), mais l'archive owl_core ne
# doit pas MELANGER les deux listes de licence. Filtre par chemin : source de
# verite unique (la liste ci-dessus), tout futur src/internal_tests/*.c ajoute
# a OWL_CORE_SRCS est route automatiquement vers la lib de test.
set(OWL_CORE_TEST_SRCS ${OWL_CORE_SRCS})
list(FILTER OWL_CORE_TEST_SRCS INCLUDE REGEX "src/internal_tests/")
list(FILTER OWL_CORE_SRCS EXCLUDE REGEX "src/internal_tests/")

# ------------------------------------------------------------------
# embed_docs — outil de build qui embarque docs/public_documentation/*.md,
# docs/public_templates/*.owl et tests/scenarios/**/test_*.owl dans le
# binaire owl.
#
# Compilé pour la machine de build (host), pas pour la cible finale.
# Le custom command qui suit le lance avant la compile de owl_core
# pour produire ${CMAKE_BINARY_DIR}/embedded_docs.c.
# ------------------------------------------------------------------
add_executable(embed_docs tools/embed_docs/embed_docs.c)
target_compile_options(embed_docs PRIVATE -Wall -Wextra -O2)
# owl_strip_pdb pas appliqué : la fonction est définie plus bas dans le
# CMakeLists, et un outil de build n'a pas besoin du strip de toute façon.

# ------------------------------------------------------------------
# zstd_smoketest — validation standalone du vendoring zstd v1.5.7
# decompress-only. Lance manuellement post-build pour confirmer que
# la lib link et que le round-trip de decompression fonctionne sur
# un blob de reference. Non inclus dans owl.exe.
# ------------------------------------------------------------------
add_executable(zstd_smoketest src/internal_tests/zstd_smoketest.c)
target_link_libraries(zstd_smoketest PRIVATE owl_zstd)
target_compile_options(zstd_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# mpdecimal_smoketest — validation standalone du vendoring mpdecimal
# 4.0.1 (modele de nombre v2, enabler ). Prouve compile +
# link + fonctionnement (1/3@50, 0.1+0.2=0.3 exact, 10^40 exact,
# overflow i64 detecte). Non inclus dans owl.exe ; consomme plus tard
# par .
# ------------------------------------------------------------------
add_executable(mpdecimal_smoketest src/internal_tests/mpdecimal_smoketest.c)
target_link_libraries(mpdecimal_smoketest PRIVATE owl_mpdecimal)
target_compile_options(mpdecimal_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# zint_smoketest — validation standalone du vendoring zint 2.16.0
# (enabler code-barres ). Prouve compile + link + sortie
# raster : genere un QR (BARCODE_QRCODE) et un Code128 (BARCODE_CODE128)
# via ZBarcode_Encode + ZBarcode_Buffer, verifie le bitmap RGB non vide.
# Non inclus dans owl.exe ; consomme plus tard par les natives .
# ------------------------------------------------------------------
add_executable(zint_smoketest src/internal_tests/zint_smoketest.c)
target_link_libraries(zint_smoketest PRIVATE owl_zint)
target_compile_options(zint_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# argon2_smoketest — validation standalone du vendoring argon2 20190702
# (enabler socle d'authentification). Prouve compile + link + CONFORMITE :
# rejoue deux vecteurs officiels du src/test.c upstream (dont un a p=2,
# qui verifie qu'ARGON2_NO_THREADS ne change pas le tag), les parametres
# OWASP m=19456/t=2/p=1 avec verify + mismatch, et le rejet d'un encodage
# tronque. Non inclus dans owl.exe ; consomme plus tard par les natives
# de hachage de mot de passe.
# ------------------------------------------------------------------
add_executable(argon2_smoketest src/internal_tests/argon2_smoketest.c)
target_link_libraries(argon2_smoketest PRIVATE owl_argon2)
target_compile_options(argon2_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# libxls_smoketest — validation standalone du vendoring libxls 1.6.3
# (enabler lecture .xls binaire herite BIFF8/OLE2). Prouve compile + link +
# DECODAGE : lit deux .xls reels (fixtures vendor/libxls/testdata/), verifie
# une cellule texte + une numerique (mtcars) et surtout les octets UTF-8
# EXACTS d'une chaine UTF-16 a diacritiques vietnamiennes (preuve que la
# conversion UTF-16 -> UTF-8 SANS iconv est correcte, le point dur du
# vendoring). Non inclus dans owl.exe ; consomme plus tard par le cablage
# OLE2 de stdlib_xls.c. Chemins fixtures relatifs a la racine repo (CWD).
# ------------------------------------------------------------------
add_executable(libxls_smoketest src/internal_tests/libxls_smoketest.c)
target_link_libraries(libxls_smoketest PRIVATE owl_libxls)
target_compile_options(libxls_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# libssh2_smoketest — validation standalone du vendoring libssh2 (backend
# mbedTLS). Prouve compile + link mais SURTOUT l'INIT RUNTIME du socle SSH-2 :
# libssh2_init + session, version >= 1.11.1 (garde-fou anti-tag-vulnerable), et
# la preuve que le backend mbedTLS a ENREGISTRE des algos (listes KEX/HOSTKEY non
# vides, presence de curve25519/ecdh + ecdsa/rsa-sha2) — un backend muet linkerait
# mais casserait tout handshake. Amorce aussi known-hosts (socle ..VerifieHote).
# Aucun reseau : le protocole SFTP en ligne sera prouve par le harness sshd
# conteneurise + le lot feature (natives FTP* routees sur libssh2_sftp_*).
# ------------------------------------------------------------------
add_executable(libssh2_smoketest src/internal_tests/libssh2_smoketest.c)
target_link_libraries(libssh2_smoketest PRIVATE owl_libssh2 owl_mbedtls)
# ws2_32 (winsock, tire par session/agent), bcrypt/crypt32/advapi32 (entropy
# mbedTLS via BCryptGenRandom sur Windows). owl_core linke deja ces libs.
if(WIN32)
    target_link_libraries(libssh2_smoketest PRIVATE ws2_32 bcrypt crypt32 advapi32)
endif()
target_compile_options(libssh2_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# aws_sigv4_smoketest — validation byte-exacte du coeur AWS Signature V4
# (enabler chantier AWS). Rejoue l'exemple canonique documente par AWS
# (GET iam / us-east-1) dont chaque valeur intermediaire est publiee :
# hash de la canonical request, string-to-sign, signature finale, header
# Authorization complet. src/stdlib_aws.c est un helper C interne pur
# (mbedTLS + libc, ni VM ni registre) — pas encore une native OWL ; il
# n'est compile QUE dans ce smoke test tant qu'aucune surface publique ne
# le consomme. La cascade de derivation de cle HMAC est binaire (mbedTLS
# direct), sans hexer entre les etapes.
# ------------------------------------------------------------------
add_executable(aws_sigv4_smoketest
    src/internal_tests/aws_sigv4_smoketest.c
    src/stdlib_aws.c)
target_include_directories(aws_sigv4_smoketest PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(aws_sigv4_smoketest PRIVATE owl_mbedtls)
# mbedtls entropy_poll referme BCryptGenRandom sur Windows (non tire par notre
# coeur SigV4, mais present dans la lib statique). owl_core linke deja bcrypt.
if(WIN32)
    target_link_libraries(aws_sigv4_smoketest PRIVATE bcrypt)
endif()
target_compile_options(aws_sigv4_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# llhttp_smoketest — validation standalone du vendoring llhttp 9.4.2
# (enabler parsing HTTP/1.1). Prouve compile + link mais SURTOUT la
# STRICTESSE : un parseur HTTP qui linke mais accepte du smuggling ne
# vaut rien. Asserte lenient_flags == 0 (apres init ET apres reset),
# parse en nominal une requete et une reponse (Content-Length, chunked,
# corps delimite par EOF, 204 sans corps), puis rejoue 17 vecteurs de
# rejet issus du corpus de tests upstream : CL+TE dans les deux ordres,
# CL duplique / non numerique / vide / en overflow, espace avant les
# deux-points (CVE-2024-27982), CR seul (CVE-2023-30589), LF seul,
# `chunked` non final (liste et header duplique), version HTTP hors
# {0.9,1.0,1.1,2.0}, token de header invalide, repli de ligne,
# caractere de controle en valeur.
# Non inclus dans owl.exe ; consomme plus tard par le client HTTP
# (src/stdlib_http.c, qui parse les reponses a la main aujourd'hui)
# puis par le serveur.
# ------------------------------------------------------------------
add_executable(llhttp_smoketest src/internal_tests/llhttp_smoketest.c)
target_link_libraries(llhttp_smoketest PRIVATE owl_llhttp)
target_compile_options(llhttp_smoketest PRIVATE -Wall -Wextra -O2)

# ------------------------------------------------------------------
# llamacpp_smoketest — validation standalone du vendoring llama.cpp/ggml.
# PREUVE DE DONE de l'enabler : prouve au RUNTIME que le backend CPU ggml est
# enregistre une fois lie statiquement (ggml_backend_dev_count() >= 1 + device
# CPU) — c.-a-d. que --whole-archive sur ggml-cpu a survecu a --gc-sections
# (piege #8166). Exerce aussi le loader GGUF (chemin bidon -> NULL propre). Si
# un GGUF est fourni (argv[1] / OWL_LLAMA_TEST_GGUF), deroule l'inference
# complete (load -> tokenize -> decode -> logits). Deterministe sans modele :
# on ne vendore aucun GGUF. Lie avec les MEMES flags qu'owl.exe (gc-sections +
# whole-archive) pour que la preuve soit representative. Cable dans la batterie
# (reg llama-smoke, scripts/run_all_checks.sh).
# ------------------------------------------------------------------
add_executable(llamacpp_smoketest src/internal_tests/llamacpp_smoketest.c)
target_link_libraries(llamacpp_smoketest PRIVATE owl_llama)
set_target_properties(llamacpp_smoketest PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(llamacpp_smoketest PRIVATE -Wall -Wextra -O2)
target_link_options(llamacpp_smoketest PRIVATE -Wl,--gc-sections)
find_package(Threads REQUIRED)
target_link_libraries(llamacpp_smoketest PRIVATE Threads::Threads)
if(WIN32)
    target_link_options(llamacpp_smoketest PRIVATE
        -Wl,-subsystem,console
        -static -static-libgcc -static-libstdc++
    )
elseif(UNIX AND NOT APPLE AND OWL_MUSL_STATIC)
    target_link_options(llamacpp_smoketest PRIVATE -static)
endif()

# ------------------------------------------------------------------
# icons_catalog_gen — tool host  Lot 2a-i.
# Parse vendor/iconify-sets/*.json, filtre fluent -24- / line-md
# svg-static, calcule xxh3-64 par body, concatene en blob compresse
# zstd niveau 19, emet ${CMAKE_BINARY_DIR}/icons_catalog_gen.c.
#
# Le tool a besoin de la compression zstd (decompress-only suffit
# pour owl.exe mais pas pour generer le blob). Comme l'add_subdirectory
# zstd plus haut force ZSTD_BUILD_COMPRESSION=OFF (pour ne pas alourdir
# owl.exe), on amalgame ici les sources de compress/ + common/ requis,
# uniquement pour ce tool host.
# ------------------------------------------------------------------
set(ZSTD_COMPRESS_SRCS
    "${VENDOR_DIR}/zstd/lib/common/debug.c"
    "${VENDOR_DIR}/zstd/lib/common/entropy_common.c"
    "${VENDOR_DIR}/zstd/lib/common/error_private.c"
    "${VENDOR_DIR}/zstd/lib/common/fse_decompress.c"
    "${VENDOR_DIR}/zstd/lib/common/pool.c"
    "${VENDOR_DIR}/zstd/lib/common/threading.c"
    "${VENDOR_DIR}/zstd/lib/common/xxhash.c"
    "${VENDOR_DIR}/zstd/lib/common/zstd_common.c"
    "${VENDOR_DIR}/zstd/lib/compress/fse_compress.c"
    "${VENDOR_DIR}/zstd/lib/compress/hist.c"
    "${VENDOR_DIR}/zstd/lib/compress/huf_compress.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_compress.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_compress_literals.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_compress_sequences.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_compress_superblock.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_double_fast.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_fast.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_lazy.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_ldm.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_opt.c"
    "${VENDOR_DIR}/zstd/lib/compress/zstd_preSplit.c"
)
add_executable(icons_catalog_gen
    tools/icons_catalog_gen/icons_catalog_gen.c
    ${ZSTD_COMPRESS_SRCS}
)
target_include_directories(icons_catalog_gen PRIVATE
    "${VENDOR_DIR}/zstd/lib"
    "${VENDOR_DIR}/zstd/lib/common"
)
target_compile_definitions(icons_catalog_gen PRIVATE
    ZSTD_DISABLE_ASM           # evite l'asm specifique requerant linkage particulier
)
# zstd a beaucoup de warnings benigns (unused-function, etc.). On baisse a -O2
# sans -Wextra strict pour le tool — c'est du code upstream, pas du nous.
target_compile_options(icons_catalog_gen PRIVATE -O2 -w)
# Tool host : pas de strip/hardening — c'est un outil de build, pas un binaire livre.

# Liste des JSON sources Iconify : si l'un change, le custom_command relance.
file(GLOB ICONS_SOURCES CONFIGURE_DEPENDS
    "${VENDOR_DIR}/iconify-sets/*.json"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/icons_catalog_gen.c"
    COMMAND $<TARGET_FILE:icons_catalog_gen>
            "${VENDOR_DIR}/iconify-sets"
            "${CMAKE_BINARY_DIR}/icons_catalog_gen.c"
    DEPENDS icons_catalog_gen ${ICONS_SOURCES}
    COMMENT "Generate icons_catalog_gen.c (cache SHA256 interne)"
    VERBATIM
)
set_source_files_properties(
    "${CMAKE_BINARY_DIR}/icons_catalog_gen.c"
    PROPERTIES GENERATED TRUE
)

# ------------------------------------------------------------------
# icons_canon_gen — tool host .
# Parse tools/icons_canon_fr_en/mapping.def (curation ~3100 termes FR
# vers tokens EN Iconify) et emet ${CMAKE_BINARY_DIR}/icons_canon_gen.c
# avec la table OWL_ICONS_FR_EN_MAPPING triee par fr ASC pour bsearch.
# Aucune dependance externe (libc seule).
# ------------------------------------------------------------------
add_executable(icons_canon_gen
    tools/icons_canon_gen/icons_canon_gen.c
)
target_compile_options(icons_canon_gen PRIVATE -O2 -Wall)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/icons_canon_gen.c"
    COMMAND $<TARGET_FILE:icons_canon_gen>
            "${CMAKE_SOURCE_DIR}/tools/icons_canon_fr_en/mapping.def"
            "${CMAKE_BINARY_DIR}/icons_canon_gen.c"
    DEPENDS icons_canon_gen
            "${CMAKE_SOURCE_DIR}/tools/icons_canon_fr_en/mapping.def"
    COMMENT "Generate icons_canon_gen.c (FR/EN mapping)"
    VERBATIM
)
set_source_files_properties(
    "${CMAKE_BINARY_DIR}/icons_canon_gen.c"
    PROPERTIES GENERATED TRUE
)

# Glob des sources à embarquer (CMake re-glob à la régen).
# tests/scenarios/ est récursif : language/, natives/, types/, ui/ sont
# plats (1 niveau), mais poo/<scenario>/{C*.owl, test_*.owl} demande
# un walk récursif.
file(GLOB EMBED_DOC_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/docs/public_documentation/*.md"
    "${CMAKE_SOURCE_DIR}/docs/public_templates/*.owl"
)
file(GLOB_RECURSE EMBED_DOC_SCENARIOS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/scenarios/*.owl"
)
list(APPEND EMBED_DOC_INPUTS ${EMBED_DOC_SCENARIOS})

# Dépendre aussi des répertoires embarqués, pas seulement des fichiers.
# Un rename `mv A B` préserve le mtime du fichier : si B est plus ancien
# que embedded_docs.c, la seule comparaison de mtime fichier ne déclenche
# PAS la régénération et le build recompile l'ancien littéral (stale).
# Le mtime du DOSSIER, lui, est bien mis à jour sur tout ajout / rename /
# suppression d'une entrée -> en dépendre force la régénération, quel que
# soit le générateur. On collecte récursivement les sous-dossiers de
# scénarios (poo/<cas>/… descend jusqu'à ~5 niveaux).
file(GLOB_RECURSE EMBED_DOC_TREE LIST_DIRECTORIES true CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/scenarios/*"
)
set(EMBED_DOC_DIRS
    "${CMAKE_SOURCE_DIR}/tests/scenarios"
    "${CMAKE_SOURCE_DIR}/docs/public_documentation"
    "${CMAKE_SOURCE_DIR}/docs/public_templates"
)
foreach(_embed_path ${EMBED_DOC_TREE})
    if(IS_DIRECTORY "${_embed_path}")
        list(APPEND EMBED_DOC_DIRS "${_embed_path}")
    endif()
endforeach()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/embedded_docs.c"
    COMMAND $<TARGET_FILE:embed_docs>
            "${CMAKE_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/embedded_docs.c"
    DEPENDS embed_docs ${EMBED_DOC_INPUTS} ${EMBED_DOC_DIRS}
    COMMENT "Embed docs/public_documentation + docs/public_templates + tests/scenarios into embedded_docs.c"
    VERBATIM
)
set_source_files_properties(
    "${CMAKE_BINARY_DIR}/embedded_docs.c"
    PROPERTIES GENERATED TRUE
)

# ------------------------------------------------------------------
# owl_core_guard — garde-fou build .
#
# Outil host qui scanne chaque source de OWL_CORE_SRCS et hurle si une
# fuite Qt (include / identifiant) ou un `#ifdef OWL_HAS_GUI` est
# detecte. Cf commentaire INVARIANT au-dessus de OWL_CORE_SRCS.
#
# Mecanisme : on ecrit la liste OWL_CORE_SRCS dans un .txt genere
# (single source of truth -> impossible de diverger avec une copie
# hardcodee), le tool prend ce .txt + la racine du repo et scanne.
# Le stamp `${CMAKE_BINARY_DIR}/owl_core_guard.stamp` est cree apres
# succes, et owl_core en depend -> le check tourne avant que owl_core
# soit compile, et le build casse net sur violation.
# ------------------------------------------------------------------
add_executable(owl_core_guard tools/owl_core_guard/owl_core_guard.c)
target_compile_options(owl_core_guard PRIVATE -Wall -Wextra -O2)

# Genere la liste des sources owl_core dans un .txt (regenere a chaque
# CMake regen). Le ; entre les items est remplace par newline.
set(_OWL_CORE_GUARD_LIST "${CMAKE_BINARY_DIR}/owl_core_srcs.txt")
string(REPLACE ";" "\n" _OWL_CORE_GUARD_LIST_CONTENT "${OWL_CORE_SRCS}")
file(WRITE "${_OWL_CORE_GUARD_LIST}" "${_OWL_CORE_GUARD_LIST_CONTENT}\n")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/owl_core_guard.stamp"
    COMMAND $<TARGET_FILE:owl_core_guard>
            "${_OWL_CORE_GUARD_LIST}"
            "${CMAKE_SOURCE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch
            "${CMAKE_BINARY_DIR}/owl_core_guard.stamp"
    DEPENDS owl_core_guard
            "${_OWL_CORE_GUARD_LIST}"
            ${OWL_CORE_SRCS}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Garde-fou owl_core : Qt et #ifdef OWL_HAS_GUI interdits"
    VERBATIM
)
add_custom_target(owl_core_guard_check ALL
    DEPENDS "${CMAKE_BINARY_DIR}/owl_core_guard.stamp"
)

# ------------------------------------------------------------------
# owl_codeview_debug — info de debogage CodeView pour le dump de crash.
#
# La toolchain zig pose -g0 en Release (cmake/toolchain_zig_native.cmake) :
# lld-link produit alors un pdb qui ne porte QUE les symboles publics. Une
# fonction `static` n'y figure pas, et dbghelp la resout sur le symbole global
# qui la precede -- le dump affiche donc un nom FAUX, pas un nom absent. Sur
# ~3550 definitions `static` dans src/, ca vise une frame sur deux : un lecteur
# presse part fouiller la mauvaise fonction. Mesure : une faute dans
# json_esc_stderr (static de vm.c) sortait sous le nom du global voisin, avec
# un deplacement de 0x69 — assez petit pour paraitre credible.
#
# -g2 -gcodeview remet les statiques ET les numeros de ligne dans le pdb. Les
# sections .debug$S/.debug$T vivent dans les .obj et sont consommees par
# lld-link : le binaire livre n'en porte pas un octet (verifie A/B).
# Applique uniquement a ce qui atterrit dans owl.exe (owl_core, owl_sql, owl) :
# les vendors gardent le -g0 de la toolchain.
#
# Effet de bord assume sur owl-cli / owl-dap / owl-build / owl-lsp : ils linkent
# owl_core, donc leur pdb s'alourdit lui aussi avant d'etre supprime par
# owl_strip_pdb. Mesure du gaspillage (rejeu du link d'owl-cli hors arbre de
# build) : 12,0 Mo de pdb ecrits puis jetes, et le link passe de 0,37 s
# (-Wl,--strip-all, aucun pdb) a 0,40 s. Le remede tiendrait en un drapeau de
# lien, il rapporterait ~30 ms par relink : sous le bruit de ce poste, donc pas
# pris.
#
# -g2 et non -g : le -g0 de la toolchain arrive AVANT dans la ligne de commande,
# et un `-g` qui suit ne le renverse pas (mesure sur un .c minimal :
# `-g0 -g -gcodeview` ne produit aucun .debug$T, `-g0 -g2 -gcodeview` si).
function(owl_codeview_debug tgt)
    # PAS en release distribuable : le codeview (.debug$S) des .obj -> .a embarque
    # le chemin de build absolu + la commande -cc1 complete (isystem zig, forme
    # backslash echappee) dans le megafile owl-build. Fuite machine (preuve D2) +
    # poids. En dev on le garde (statiques + lignes dans le pdb, cf. ci-dessus).
    if(WIN32 AND NOT OWL_RELEASE_DISTRIB)
        target_compile_options(${tgt} PRIVATE -g2 -gcodeview)
    endif()
endfunction()

add_library(owl_core STATIC ${OWL_CORE_SRCS})
owl_codeview_debug(owl_core)
add_dependencies(owl_core owl_core_guard_check)
target_include_directories(owl_core PUBLIC src lsp "${VENDOR_DIR}/stb")
target_link_libraries(owl_core PUBLIC owl_vendor)
# Modele de nombre v2 : le type numerique = bignum decimal adosse a
# mpdecimal (OBJ_NUMERIC heap-manage par le GC). PUBLIC : propage l'include dir
# (mpdecimal.h public uniquement) aux consommateurs d'owl_core.
target_link_libraries(owl_core PUBLIC owl_mpdecimal)
#  : type LLM (stdlib_llm.c) — inference GGUF via llama.cpp/ggml.
# PUBLIC : stdlib_llm.c est dans owl_core, donc tout consommateur d'owl_core
# (owl.exe, owl-build, owl-cli, owl-dap) doit lier llama + recevoir l'include
# dir (owl_llama est une INTERFACE lib qui propage les deux, dont le
# --whole-archive sur ggml-cpu). owl-lsp ne lie PAS owl_core : non impacte.
target_link_libraries(owl_core PUBLIC owl_llama)
target_compile_options(owl_core PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-missing-field-initializers
    -fno-strict-aliasing
    # === Anonymisation / hardening (cf docs/internal_documentation/build_hardening.md) ===
    -fvisibility=hidden                                  # symboles internes non exportes (effectif sur ELF/Linux, no-op MinGW PE — pas de regression)
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.              # __FILE__ relatif : cache le path source local du dev
    -fdata-sections                                      # chaque var dans sa propre section -> linker peut la virer si inutile
    -ffunction-sections                                  # idem pour les fonctions
    # NB : -DNDEBUG est deja injecte par CMake en mode Release (CMAKE_C_FLAGS_RELEASE = "-O3 -DNDEBUG")
)
# Relaxations Linux : GCC Linux est plus strict que MinGW sur certains
# warnings benigns (unused-result de fread/scanf, maybe-uninitialized
# avec analyse interproc plus poussée). Le code est en cours de portage,
# on relaxe les warnings, pas le -Werror global.
if(UNIX AND NOT APPLE)
    target_compile_options(owl_core PRIVATE
        -Wno-error=unused-result
        -Wno-error=maybe-uninitialized
        -Wno-error=stringop-truncation
        -Wno-error=array-bounds
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=use-after-free
    )
endif()

# Win32 deps qu'OWL utilise (sockets, RNG)
if(WIN32)
    target_compile_definitions(owl_core PUBLIC WIN32_LEAN_AND_MEAN NOCRYPT)
    # dbghelp : StackWalk64 + Sym* pour le crash handler (platform.c, ).
    target_link_libraries(owl_core PUBLIC ws2_32 bcrypt dbghelp)
else()
    # pthread requis par debug_stdio.c (Lot D.2.3) qui spawn un thread
    # de lecture stdin pendant que la VM tourne sur le main thread.
    find_package(Threads REQUIRED)
    target_link_libraries(owl_core PUBLIC m Threads::Threads)
endif()

# Kernels numeriques (produit matriciel, convolution) : coupe la contraction
# mul+add -> FMA du compilateur (-O3 a -ffp-contract=on par defaut sous clang).
# Un FMA fond les deux operations en un seul arrondi et divergerait du chemin
# scalaire de la VM (deux arrondis), cassant la parite bit-a-bit avec l'oracle
# OWL. Double ceinture avec le `#pragma STDC FP_CONTRACT OFF` en tete de ces
# fichiers. Cible les sources kernel uniquement (pas de ralentissement global).
set_source_files_properties(
    src/stdlib_matrice.c
    src/stdlib_tenseur.c
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off"
)

# ------------------------------------------------------------------
# owl_internal_tests : self-tests internes extraits d'owl_core (scoping
# licence, cf. commentaire au-dessus d'OWL_CORE_TEST_SRCS). Linke owl_core en
# PUBLIC (les self-tests exercent le core ; herite includes + defines). Linke
# UNIQUEMENT dans les binaires de dev (owl.exe / owl-cli), jamais copie dans le
# layout de distribution `owl build`. Memes flags de compile qu'owl_core pour
# une compilation a l'identique (aucun changement de comportement des tests).
add_library(owl_internal_tests STATIC ${OWL_CORE_TEST_SRCS})
owl_codeview_debug(owl_internal_tests)
target_include_directories(owl_internal_tests PUBLIC src lsp "${VENDOR_DIR}/stb")
target_link_libraries(owl_internal_tests PUBLIC owl_core)
target_compile_options(owl_internal_tests PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-missing-field-initializers
    -fno-strict-aliasing
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)
if(UNIX AND NOT APPLE)
    target_compile_options(owl_internal_tests PRIVATE
        -Wno-error=unused-result
        -Wno-error=maybe-uninitialized
        -Wno-error=stringop-truncation
        -Wno-error=array-bounds
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=use-after-free
    )
endif()

# ------------------------------------------------------------------
# owl_sql : stack SQL (moteur embarque + drivers mysql/mssql/postgres/odbc
# + duckdb) isole dans sa propre lib statique. Compile avec OWL_HAS_SQL et
# tire owl_vendor_sql (sqlite/duckdb/odbc). Linke par owl.exe + les runtimes
# owl-build (console/gui), PAS par owl-cli qui sert aux benchmarks de langage
# pur et n'a aucun besoin du SQL (duckdb_static seul = ~71 MB).
# ------------------------------------------------------------------
set(OWL_SQL_SRCS
    src/stdlib_sql.c
    src/sql_cnx_string.c
    src/internal_tests/sql_cnx_string_test.c
    src/sql_mysql_wire.c
    src/internal_tests/sql_mysql_wire_test.c
    src/auth_mysql_native.c
    src/auth_mysql_caching.c
    src/internal_tests/auth_mysql_test.c
    src/sql_mysql_conn.c
    src/internal_tests/sql_mysql_conn_test.c
    src/sql_mysql_mock_server.c
    src/internal_tests/sql_mysql_e2e_test.c
    src/sql_mysql_query.c
    src/internal_tests/sql_mysql_query_test.c
    src/sql_mysql_types.c
    src/internal_tests/sql_mysql_types_test.c
    src/sql_duckdb_conn.c
    src/sql_duckdb_query.c
    src/sql_duckdb_types.c
    src/internal_tests/sql_duckdb_test.c
    src/sql_odbc_conn.c
    src/sql_odbc_loader.c
    src/sql_odbc_query.c
    src/sql_odbc_types.c
    src/owl_hfsql_install.c
    src/sql_mssql_packet.c
    src/internal_tests/sql_mssql_packet_test.c
    src/sql_mssql_prelogin.c
    src/internal_tests/sql_mssql_prelogin_test.c
    src/sql_mssql_tls.c
    src/internal_tests/sql_mssql_tls_test.c
    src/sql_mssql_login.c
    src/internal_tests/sql_mssql_login_test.c
    src/sql_mssql_query.c
    src/internal_tests/sql_mssql_query_test.c
    src/sql_mssql_types.c
    src/internal_tests/sql_mssql_types_test.c
    src/sql_mssql_conn.c
    src/sql_postgres_wire.c
    src/internal_tests/sql_postgres_wire_test.c
    src/sql_postgres_types.c
    src/internal_tests/sql_postgres_types_test.c
    src/sql_postgres_conn.c
    src/internal_tests/sql_postgres_conn_test.c
    src/sql_postgres_mock_server.c
    src/internal_tests/sql_postgres_e2e_test.c
    src/sql_postgres_query.c
    src/internal_tests/sql_postgres_query_test.c
)
# Scoping licence (cf. OWL_CORE_TEST_SRCS) : owl_sql est aussi reutilisee pour
# builder les exe utilisateur (owl_runtime_console/gui la linkent). On extrait
# ses self-tests vers owl_sql_internal_tests, hors de l'archive distribuee.
set(OWL_SQL_TEST_SRCS ${OWL_SQL_SRCS})
list(FILTER OWL_SQL_TEST_SRCS INCLUDE REGEX "src/internal_tests/")
list(FILTER OWL_SQL_SRCS EXCLUDE REGEX "src/internal_tests/")
add_library(owl_sql STATIC ${OWL_SQL_SRCS})
owl_codeview_debug(owl_sql)
target_include_directories(owl_sql PUBLIC src lsp "${VENDOR_DIR}/stb")
target_compile_definitions(owl_sql PUBLIC OWL_HAS_SQL)
target_link_libraries(owl_sql PUBLIC owl_core owl_vendor_sql)
target_compile_options(owl_sql PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-missing-field-initializers
    -fno-strict-aliasing
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)
if(UNIX AND NOT APPLE)
    target_compile_options(owl_sql PRIVATE
        -Wno-error=unused-result
        -Wno-error=maybe-uninitialized
        -Wno-error=stringop-truncation
        -Wno-error=array-bounds
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=use-after-free
    )
endif()

# owl_sql_internal_tests : self-tests SQL extraits d'owl_sql (scoping licence).
# Linke owl_sql en PUBLIC (herite OWL_HAS_SQL + includes). Linke uniquement
# dans owl.exe (owl-cli n'a pas le SQL). Non copie dans le layout `owl build`.
add_library(owl_sql_internal_tests STATIC ${OWL_SQL_TEST_SRCS})
owl_codeview_debug(owl_sql_internal_tests)
target_include_directories(owl_sql_internal_tests PUBLIC src lsp "${VENDOR_DIR}/stb")
target_link_libraries(owl_sql_internal_tests PUBLIC owl_sql)
target_compile_options(owl_sql_internal_tests PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-missing-field-initializers
    -fno-strict-aliasing
    -fvisibility=hidden
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdata-sections
    -ffunction-sections
)
if(UNIX AND NOT APPLE)
    target_compile_options(owl_sql_internal_tests PRIVATE
        -Wno-error=unused-result
        -Wno-error=maybe-uninitialized
        -Wno-error=stringop-truncation
        -Wno-error=array-bounds
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=use-after-free
    )
endif()

# ------------------------------------------------------------------
# owl_runtime_console : owl_stdlib.c + stdlib_runtime.c COMPILES SANS
# OWL_HAS_GUI. Ces 2 fichiers sont per-target (la version GUI est
# compilee directement dans owl.exe avec -DOWL_HAS_GUI). Pour `owl
# build` qui produit un binaire console-only, on a besoin de leur
# variante sans GUI — d'ou cette lib statique dediee.
add_library(owl_runtime_console STATIC
    src/owl_stdlib.c
    src/stdlib_runtime.c
    src/stdlib_builtin_types.c
    #  : donnees de registre PDF (Qt-free). owl_stdlib.c appelle
    # stdlib_pdf_defs_register() inconditionnellement -> requis en console.
    src/stdlib_pdf_defs.c
    #  : donnees de registre UI (Qt-free). owl_stdlib.c appelle
    # stdlib_ui_defs_register() inconditionnellement -> requis en console.
    src/stdlib_ui_defs.c
)
target_compile_definitions(owl_runtime_console PUBLIC OWL_HAS_SQL)
target_link_libraries(owl_runtime_console PUBLIC owl_core owl_sql)
target_compile_options(owl_runtime_console PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-missing-field-initializers
    -fno-strict-aliasing
)
if(UNIX AND NOT APPLE)
    target_compile_options(owl_runtime_console PRIVATE
        -Wno-error=unused-result
        -Wno-error=maybe-uninitialized
        -Wno-error=stringop-truncation
        -Wno-error=array-bounds
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=use-after-free
    )
endif()

# ------------------------------------------------------------------
# Distribution layout : prepare lib/ + include/owl/ pour `owl build`
#
# `owl build foo.owl` link contre les .a pre-compilees au lieu de
# recompiler tout le vendor a chaque fois (~10s -> ~2s) et evite la
# duplication des listes de sources entre CMake et project.c (la
# liste OWL_CORE_SRCS ci-dessus est l'unique source de verite).
# Cf docs/internal_documentation/owl_build_architecture.md
# ------------------------------------------------------------------
# Headers OWL exposes pour le bundle genere par `owl build` (le bundle
# fait #include <owl/common.h>, <owl/vm.h>, etc.). On copie tous les
# .h car les transitives (chunk.h, value.h, table.h, ...) sont
# largement intriquees et tracker un sous-ensemble fiable demanderait
# un audit recurrent. Le surcout disque est ~200 Ko.
file(GLOB _OWL_HEADERS "${CMAKE_SOURCE_DIR}/src/*.h")
foreach(_hdr ${_OWL_HEADERS})
    get_filename_component(_name ${_hdr} NAME)
    configure_file(${_hdr} "${CMAKE_BINARY_DIR}/include/owl/${_name}" COPYONLY)
endforeach()

# Custom target qui copie les .a apres leur build. ALL = inclus dans
# le build par defaut. Les sources des libs vendor (mbedtls/pcre2/etc.)
# sont les memes que ceux compiles dans libowl_core.a etc. — on rend
# juste les .a accessibles a un emplacement stable pour `owl build`.
# Detecte le triple cible (Phase B.5 : un sous-dossier lib/<triple>/ par
# target permet a owl-build de cross-compiler via --target).
if(WIN32)
    set(OWL_TARGET_TRIPLE "windows-x86_64")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(OWL_TARGET_TRIPLE "macos-aarch64")
    else()
        set(OWL_TARGET_TRIPLE "macos-x86_64")
    endif()
else()
    set(OWL_TARGET_TRIPLE "linux-x86_64")
endif()
set(OWL_LIB_OUT "${CMAKE_BINARY_DIR}/lib/${OWL_TARGET_TRIPLE}")
message(STATUS "OWL: target triple = ${OWL_TARGET_TRIPLE}")

if(OWL_BUILD_OWL)
    set(_owl_distrib_gui_deps owl_runtime_gui)
    set(_owl_distrib_gui_copy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:owl_runtime_gui>" "${OWL_LIB_OUT}/")
else()
    set(_owl_distrib_gui_deps "")
    set(_owl_distrib_gui_copy "")
endif()
add_custom_target(owl_distrib ALL
    DEPENDS owl_core owl_sql owl_runtime_console ${_owl_distrib_gui_deps} owl_mbedtls owl_pcre2 owl_miniz owl_lzma owl_md4c owl_sqlite owl_ca LibXml2
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OWL_LIB_OUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_core>"            "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_sql>"             "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_runtime_console>" "${OWL_LIB_OUT}/"
    ${_owl_distrib_gui_copy}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_mbedtls>"   "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_pcre2>"     "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_miniz>"     "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_lzma>"      "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_md4c>"      "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_sqlite>"    "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:owl_ca>"        "${OWL_LIB_OUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:LibXml2>"       "${OWL_LIB_OUT}/"
    COMMENT "Preparing OWL distrib layout (lib/${OWL_TARGET_TRIPLE}/*.a + include/owl/*.h)"
)

# Cleanup PDB post-link. zig clang/lld-link produit des .pdb meme en Release
# (MSVC-style debug info side file). Useless pour les outils annexes (DWARF
# inline suffit pour gdb), 30-250 MB de gaspillage sinon.
#
# EXCEPTION owl.exe : son pdb est la seule source de noms de
# fonctions pour le dump de crash (src/platform.c, montage dbghelp de
#  — search path sur le dossier de l'exe, chargement EAGER,
# SymLoadModuleEx a la base reelle). Le strip rendait ce montage inoperant :
# les modules systeme sortaient nommes, owl.exe restait en adresses nues.
# Poids rechiffre sur cette cible precise : 53,7 Mo de fichier annexe (42 Mo
# avant que owl_codeview_debug n'y remette les statiques, loin des 30-250 Mo qui
# couvraient tous les executables), zero octet dans owl.exe
# (A/B dos a dos : taille identique des deux cotes) et zero surcout de link —
# lld-link produit le pdb dans les deux cas, le strip n'etait qu'un
# `cmake -E remove` post-link. Le pdb n'est pas un artefact de release : il
# ne part dans aucune distribution.
#
# Cout sur le geste quotidien (edition d'un .c du coeur puis relink de la seule
# cible, 10-11 passes par etat, medianes) : owl.exe 3,16 s avec CodeView contre
# 3,04 s sans ; owl-cli 2,45 s contre 2,46 s ; owl-dap 2,38 s contre 2,30 s.
# L'ecart change de signe selon la cible et la dispersion interne a un etat
# atteint 1,2 a 1,7 s : le surcout de relink est sous le bruit du poste, pas
# mesurable ici. Le link nu ne pese d'ailleurs que ~0,4 s dans ces 2-3 s, le
# reste etant la recompilation du .c, l'archive et les etapes post-link.
#
# Fonction definie HORS du gate OWL_BUILD_OWL car utilisee aussi pour
# owl-cli, owl-lsp, owl-dap.
function(owl_strip_pdb tgt)
    if(WIN32)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E remove -f "$<TARGET_FILE_DIR:${tgt}>/${tgt}.pdb"
            COMMENT "Strip ${tgt}.pdb"
        )
    endif()
endfunction()

# ------------------------------------------------------------------
# Cible : owl.exe — all-in-one (interpreteur + GUI Qt6 + PDF qpdf)
#
# Qt6 vendore en static : built localement depuis vendor/qt6/qtbase/ vers
# _local/host_tools/qt6/ (host tools moc/rcc/syncqt en gcc/libstdc++ static)
# et _local/qt6/<host>/install/ (target libs en zig/libc++). Lance
# `make qt6` puis `bash scripts/build_qt6_zig_native.sh` une fois
# (~60-90 min total) avant le premier `make`. Toutes les libs Qt deps
# (pcre2, freetype, harfbuzz, libpng, zlib, etc.) sont bundled — zero
# dependance MSYS2 cote Qt.
#
# Tout le bloc owl.exe (depuis find_package(Qt6) jusqu'a la fin du
# target owl + ses link options) est gate sur OWL_BUILD_OWL=ON.
# OFF -> on saute owl.exe et les targets owl_runtime_gui. owl-cli,
# owl-build, owl-lsp et owl-dap restent buildables car ils n'ont pas
# de dependance Qt6.
# ------------------------------------------------------------------
if(OWL_BUILD_OWL)
if(NOT DEFINED OWL_QT6_INSTALL)
    # Fallback : pointe vers Qt6 zig native (target libs). Le Makefile
    # passe deja OWL_QT6_INSTALL explicitement, ce fallback couvre les
    # invocations cmake hors Makefile (ex: configure manuel pour debug).
    if(WIN32)
        set(_owl_qt6_host "x86_64-windows")
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_owl_qt6_host "aarch64-macos")
        else()
            set(_owl_qt6_host "x86_64-macos")
        endif()
    elseif(OWL_MUSL_STATIC)
        # Qt6 recompile musl offscreen (install distinct du glibc x86_64-linux).
        # Cf scripts/build_qt6_zig_native.sh OWL_MUSL=1.
        set(_owl_qt6_host "x86_64-linux-musl")
    else()
        set(_owl_qt6_host "x86_64-linux")
    endif()
    set(OWL_QT6_INSTALL "${CMAKE_SOURCE_DIR}/_local/qt6/${_owl_qt6_host}/install")
endif()
if(NOT EXISTS "${OWL_QT6_INSTALL}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR
        "Qt6 vendore non builde. Lance d'abord : make qt6\n"
        "(install attendu dans ${OWL_QT6_INSTALL})\n"
        "ou desactive owl.exe via -DOWL_BUILD_OWL=OFF "
        "(build owl-cli + owl-lsp + owl-build seuls).")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${OWL_QT6_INSTALL}")
if(WIN32)
    # Sur MinGW, prioriser .a (static) sur .dll.a (import lib).
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;.dll.a")
endif()
add_compile_definitions(QT_STATIC)
message(STATUS "OWL: Qt6 vendore (${OWL_QT6_INSTALL})")

# Restaure pkg-config, neutralise plus haut (L487) uniquement pour isoler qpdf
# des libs systeme (zlib/jpeg). qpdf est le seul consommateur avant ce point ;
# Qt6 en a besoin sur Linux pour localiser xkbcommon-x11. No-op sur Windows
# (Qt6 vendore n'utilise pas pkg-config), qpdf reste sur les libs vendorees.
unset(PKG_CONFIG_EXECUTABLE CACHE)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Svg)

# Qt resource avec les fontes Liberation + DejaVu (rendu PDF deterministe
# cross-platform). Le .qrc est compile en .cpp et embed les .ttf
# directement dans owl.exe. Cf assets/fonts.qrc et apply_owl_fonts()
# dans pdf_qt.cpp.
qt_add_resources(OWL_FONT_RESOURCES "assets/fonts.qrc")

# ------------------------------------------------------------------
# owl_runtime_gui : variante FULL GUI de owl_runtime_console.
# Contient owl_stdlib.c + stdlib_runtime.c COMPILES AVEC OWL_HAS_GUI,
# plus stdlib_ui + ui_qt.cpp + stdlib_pdf + pdf_qt.cpp + pdf_postprocess
# + qrc_fonts. C'est ce que owl-build link pour produire un binaire
# autonome avec Qt6 statique.
#
# Decision (Phase B) : tout binaire produit par owl-build est GUI
# par defaut. Le mode console-only reste accessible via owl-cli (dev only).
# ------------------------------------------------------------------
qt_add_resources(_OWL_FONT_RESOURCES_GUI "assets/fonts.qrc")
add_library(owl_runtime_gui STATIC
    src/owl_stdlib.c
    src/stdlib_runtime.c
    src/stdlib_builtin_types.c
    src/ui_qt.cpp
    src/stdlib_ui.c
    src/stdlib_ui_defs.c        #  : donnees de registre UI (Qt-free)
    src/pdf_qt.cpp
    src/pdf_postprocess.cpp
    src/stdlib_pdf.c
    src/stdlib_pdf_defs.c       #  : donnees de registre PDF (Qt-free)
    src/icone_render_qt.cpp
    ${_OWL_FONT_RESOURCES_GUI}
)
target_compile_definitions(owl_runtime_gui PUBLIC OWL_HAS_GUI OWL_HAS_SQL)
target_link_libraries(owl_runtime_gui PUBLIC
    owl_core
    owl_sql
    Qt6::PrintSupport Qt6::Widgets Qt6::Gui Qt6::Core Qt6::Svg
    #  : plugin de rendu hors-ecran (capture PNG sans display).
    # Non auto-linke (QT_DEFAULT_PLUGIN=0) -> link explicite requis.
    Qt6::QOffscreenIntegrationPlugin
)
target_compile_options(owl_runtime_gui PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-error=unused-function
    -Wno-error=unused-const-variable
    -Wno-error=comment
    -Wno-missing-field-initializers
    -fno-strict-aliasing
)
if(UNIX AND NOT APPLE)
    target_compile_options(owl_runtime_gui PRIVATE
        -Wno-error=unused-result
        -Wno-error=maybe-uninitialized
        -Wno-error=stringop-truncation
        -Wno-error=array-bounds
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=use-after-free
    )
endif()

add_executable(owl
    src/main.c
    src/owl_stdlib.c
    src/stdlib_runtime.c
    src/stdlib_builtin_types.c
    src/ui_qt.cpp
    src/stdlib_ui.c
    src/stdlib_ui_defs.c        #  : donnees de registre UI (Qt-free)
    src/pdf_qt.cpp
    src/pdf_postprocess.cpp
    src/stdlib_pdf.c
    src/stdlib_pdf_defs.c       #  : donnees de registre PDF (Qt-free)
    src/icone_render_qt.cpp
    ${OWL_FONT_RESOURCES}
)
target_compile_definitions(owl PRIVATE OWL_HAS_GUI OWL_HAS_SQL)
target_link_libraries(owl PRIVATE
    # self-tests internes (--self-test-*, appeles depuis main.c) : hors des
    # archives owl_core/owl_sql pour le scoping licence, re-linkes ici pour le
    # binaire de dev. Listes avant owl_core/owl_sql (dependance de link).
    owl_internal_tests
    owl_sql_internal_tests
    owl_core
    owl_sql
    Qt6::PrintSupport Qt6::Widgets Qt6::Gui Qt6::Core Qt6::Svg
    #  : plugin de rendu hors-ecran (capture PNG sans display).
    Qt6::QOffscreenIntegrationPlugin
)
# Aligné sur les autres targets : OwlEnum est typedef forward dans
# owl_stdlib.h et complet dans owl_registry.h — pattern volontaire (deux
# fichiers indépendants), C11 feature qui passe en warning sous -Werror.
target_compile_options(owl PRIVATE -Wno-error=typedef-redefinition)
owl_codeview_debug(owl)

# qpdf — post-traitement PDF pour iParametrePDF Option B :
# chiffrement + signets + PDF/A-3b + attachments. Vendore source-built
# en static (cf section "qpdf vendore" plus haut). Aucune lib dynamique
# requise sur la machine de l'utilisateur final.
target_compile_definitions(owl PRIVATE OWL_HAS_QPDF)
target_link_libraries(owl PRIVATE libqpdf)
# Idem owl_runtime_gui : il contient pdf_postprocess.cpp et a besoin du
# define + des headers qpdf (sinon le code qpdf-conditionnel ne se compile
# pas dans la lib partagee, et l'integration dans owl-build serait incomplete).
target_compile_definitions(owl_runtime_gui PUBLIC OWL_HAS_QPDF)
target_link_libraries(owl_runtime_gui PUBLIC libqpdf)

# llama.cpp / ggml — enabler LLM GGUF (bataille 2). Depuis , le type
# LLM (src/stdlib_llm.c) consomme reellement llama/ggml : owl_llama est linke en
# PUBLIC sur owl_core, donc tire transitivement ici (llama + ggml-base ne sont
# plus gc'es faute de reference, et le --whole-archive sur ggml-cpu se propage
# via l'INTERFACE owl_llama). Aucun link explicite requis sur owl.

# Console subsystem pour avoir stdout/stderr visibles
if(WIN32)
    target_link_options(owl PRIVATE -Wl,-subsystem,console)
endif()

# Anonymisation : --gc-sections vire les sections (functions / globales)
# non-referencees, complementaire de -ffunction-sections / -fdata-sections
# applique a owl_core. Reduit la taille binaire et l'empreinte des
# strings/symboles "morts" embarques.
target_link_options(owl PRIVATE -Wl,--gc-sections)

# Anonymisation : owl.exe est un EXEC, pas une DLL — il n'a aucune raison
# d'exporter des symboles. DuckDB (vendore en archive statique) force des
# __declspec(dllexport) sur 462 fonctions duckdb_* qui remontent dans la
# table d'export PE de owl.exe -> `dumpbin /exports owl.exe` revele
# immediatement l'embarquement DuckDB. --exclude-all-symbols vide la
# table d'export du PE final, fermant cette leak de stack technique.
if(WIN32)
    # Note : sur MinGW/lld via zig wrapper, les flags d'export PE classiques
    # (--exclude-all-symbols, --no-export-all-symbols, /noimplib) sont rejetes
    # par le wrapper zig-c++ qui filtre l'arg list. La table d'export DuckDB
    # est neutralisee au build DuckDB via DUCKDB_BUILD_LIBRARY=0 + DUCKDB_API=
    # injectes dans scripts/build_duckdb_zig_native.sh.
endif()

# Pas de owl_strip_pdb(owl) : cf.  ci-dessus — owl.pdb reste a cote
# de l'exe pour que le dump de crash nomme les fonctions d'owl.exe.

# Static link Qt6 sur Windows : on lie explicitement toutes les deps
# transitives non auto-resolues par CMake (Win32 system libs surtout).
# Les bundled libs (pcre2/freetype/harfbuzz/etc.) viennent automatiquement
# des targets Qt6Bundled* exposees par le find_package.
if(WIN32)
    set(OWL_QT_WIN_DEPS
        # Compression / Misc
        atomic m
        # Windows API supplementaires non-tirees par owl_core
        d2d1 dwrite dwmapi d3d9 d3d11 d3d12 dxgi dxguid
        uxtheme usp10 imm32 oleaut32 setupapi winspool wtsapi32
        shcore comdlg32 shlwapi rpcrt4 synchronization mpr
        userenv authz netapi32 ntdll runtimeobject uuid version winmm
    )
    target_link_libraries(owl PRIVATE ${OWL_QT_WIN_DEPS})
    target_link_options(owl PRIVATE
        -static -static-libgcc -static-libstdc++
        "LINKER:--start-group"
    )
    if(OWL_RELEASE_DISTRIB)
        target_link_options(owl PRIVATE -Wl,--strip-all)
    endif()
elseif(UNIX AND NOT APPLE AND OWL_MUSL_STATIC)
    # Linux musl-static universel (Alpine / scratch / distroless / glibc).
    # Profil OFFSCREEN : rendu PDF/image/SVG via le plugin QOffscreen (deja
    # linke plus haut), aucune pile graphique X11/OpenGL/fontconfig requise.
    # -static embarque TOUT, y compris la libc musl -> le binaire ne depend
    # d'aucune lib hote (le pendant exact du .exe Windows). Freetype/harfbuzz
    # sont bundled dans Qt6 (system_* OFF a la compil musl). Seuls Threads,
    # dl et m viennent de la libc musl elle-meme (statique).
    find_package(Threads REQUIRED)
    target_link_libraries(owl PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    target_link_options(owl PRIVATE -static)
    if(OWL_RELEASE_DISTRIB)
        target_link_options(owl PRIVATE -Wl,--strip-all)
    endif()
elseif(UNIX AND NOT APPLE)
    # Linux static link Qt6 : libs systeme standard (X11, fontconfig,
    # OpenGL, etc.). Threads et dl sont des deps minimales de tout
    # binaire Qt linke statiquement.
    find_package(Threads REQUIRED)
    find_package(Fontconfig)
    find_package(Freetype)
    set(OWL_QT_LINUX_DEPS
        Threads::Threads
        ${CMAKE_DL_LIBS}
        m
    )
    if(Fontconfig_FOUND)
        list(APPEND OWL_QT_LINUX_DEPS Fontconfig::Fontconfig)
    endif()
    if(Freetype_FOUND)
        list(APPEND OWL_QT_LINUX_DEPS Freetype::Freetype)
    endif()
    # X11 + xkbcommon pour le platform plugin xcb
    find_package(X11)
    if(X11_FOUND)
        list(APPEND OWL_QT_LINUX_DEPS ${X11_LIBRARIES})
    endif()
    target_link_libraries(owl PRIVATE ${OWL_QT_LINUX_DEPS})
endif()

# ------------------------------------------------------------------
# qtsvg_smoketest — validation standalone du vendoring qtsvg v6.10.0
# (decompresse SVG inline -> QSvgRenderer -> QImage 64x64 -> compte
# pixels rendus). Lance manuellement post-build pour confirmer que
# QSvgRenderer fonctionne. Pre-requis pour  (catalogue icones).
# Lie a Qt6::Gui + Qt6::Svg + plugin offscreen (pas de display reel).
# ------------------------------------------------------------------
add_executable(qtsvg_smoketest src/internal_tests/qtsvg_smoketest.cpp)
target_link_libraries(qtsvg_smoketest PRIVATE
    Qt6::Gui Qt6::Core Qt6::Svg
    Qt6::QOffscreenIntegrationPlugin
)
target_compile_options(qtsvg_smoketest PRIVATE -Wall -Wextra -O2)
if(WIN32)
    target_link_libraries(qtsvg_smoketest PRIVATE ${OWL_QT_WIN_DEPS})
    target_link_options(qtsvg_smoketest PRIVATE
        -static -static-libgcc -static-libstdc++
        "LINKER:--start-group"
    )
elseif(UNIX AND NOT APPLE AND OWL_MUSL_STATIC)
    # Meme profil musl-static que owl : tout embarque, zero dep hote.
    find_package(Threads REQUIRED)
    target_link_libraries(qtsvg_smoketest PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    target_link_options(qtsvg_smoketest PRIVATE -static)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(qtsvg_smoketest PRIVATE ${OWL_QT_LINUX_DEPS})
endif()
endif()  # OWL_BUILD_OWL

# ------------------------------------------------------------------
# Cible : owl-cli.exe (interpreteur sans Qt6/UI/PDF — startup rapide)
#
# Variante CLI-only du binaire OWL : meme interpreteur, meme stdlib core,
# mais sans le backend GUI Qt6 statique (~30 MB) ni qpdf. Resultat :
# binaire ~10x plus petit, startup ~3-5x plus rapide. Utilise pour les
# benchmarks (qui sont CLI par nature) et tout usage scripting / CI.
#
# Compile sans OWL_HAS_GUI : owl_stdlib.c et stdlib_runtime.c degradent
# proprement via les #ifdef. main.c expose deja un fallback `#else` pour
# `handle_preview_command` qui invite a utiliser owl.exe (le full).
#
# Conformite type : un programme contenant `Ouvre()` ou `dPolice()` est
# refuse a la compilation par owl-cli (commit e0337cb : unknown global
# identifiers are hard compile errors). Pas de stub runtime, pas de
# plantage cryptique.
# ------------------------------------------------------------------
if(OWL_DEV_TOOLS)
    add_executable(owl-cli
        src/main.c
        src/owl_stdlib.c
        src/stdlib_runtime.c
        src/stdlib_builtin_types.c
        #  : donnees de registre PDF (Qt-free). owl-cli --check doit
        # resoudre les natives PDF a l'identique d'owl.exe (parite du verdict).
        src/stdlib_pdf_defs.c
        #  : donnees de registre UI (Qt-free). owl-cli --check doit
        # resoudre les natives UI a l'identique d'owl.exe (parite du verdict).
        src/stdlib_ui_defs.c
    )
    # owl_internal_tests avant owl_core (dependance de link). Pas de SQL sur
    # owl-cli -> owl_sql_internal_tests absent (main.c garde les appels SQL
    # sous #ifdef OWL_HAS_SQL, non defini ici).
    target_link_libraries(owl-cli PRIVATE owl_internal_tests owl_core)
    set_target_properties(owl-cli PROPERTIES LINKER_LANGUAGE CXX)
    # Même rationale que sur target owl : OwlEnum forward/full coexistent
    # entre owl_stdlib.h et owl_registry.h, warning C11 benign.
    target_compile_options(owl-cli PRIVATE -Wno-error=typedef-redefinition)
    if(WIN32)
        target_link_options(owl-cli PRIVATE
            -Wl,-subsystem,console
            -static -static-libgcc -static-libstdc++
        )
    endif()
    owl_strip_pdb(owl-cli)
endif()

# ------------------------------------------------------------------
# duckdb_smoketest — validation standalone du vendoring DuckDB en profil
# musl-static universel (Alpine/scratch sans glibc). Ouvre une base memoire,
# execute SELECT 40+2, verifie 42. Cible presente UNIQUEMENT en profil musl
# (sa raison d'etre) -> Windows/glibc inchanges. Lance manuellement post-build
# dans le chroot Alpine pour prouver que libduckdb_static.a musl s'execute.
# ------------------------------------------------------------------
if(OWL_MUSL_STATIC)
    add_executable(duckdb_smoketest src/internal_tests/duckdb_smoketest.c)
    target_link_libraries(duckdb_smoketest PRIVATE owl_duckdb)
    set_target_properties(duckdb_smoketest PROPERTIES LINKER_LANGUAGE CXX)
    find_package(Threads REQUIRED)
    target_link_libraries(duckdb_smoketest PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    target_link_options(duckdb_smoketest PRIVATE -static)
endif()

# ------------------------------------------------------------------
# Cible : owl-build.exe — packager OWL standalone
#
# Phase A : binaire dedie au packaging d'un projet OWL en exe natif.
# Reutilise build_project() de project.c, ajoute le layout de sortie
# <PROJET>/exe/<config>/<nom>[.exe]. Pas de cross-compile en Phase A
# (gcc PATH requis). Phase B ajoutera zig cc bundle pour cross-target.
#
# Le binaire ne contient ni VM ni Qt6 — juste l'orchestrateur de build.
# Il consomme lib/ + include/owl/ deja produits par owl_distrib.
# ------------------------------------------------------------------
# EXCLUDE_FROM_ALL : `make` ne build pas owl-build. Seul `make mega`
# (via cmake --build --target owl-build dans scripts/build_mega.sh) le
# construit explicitement. Le binaire 30 MB en mode dev a ete juge
# redondant : on garde uniquement le single-file ~575 MB de `make mega`,
# qui est l'output officiel destine aux end users.
add_executable(owl-build EXCLUDE_FROM_ALL
    src/build_main.c
    src/payload_extract.c
)
target_include_directories(owl-build PRIVATE src ${VENDOR_DIR}/miniz)
# OWL_VERSION : source unique = src/common.h (inclus par build_main.c).
# Pas de -DOWL_VERSION ici (collision macro redefinie avec common.h).
target_compile_options(owl-build PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=type-limits
    -Wno-error=format-truncation
    -Wno-missing-field-initializers
    -fno-strict-aliasing
)
# build_project est implemente dans project.c qui appartient a owl_core.
# Link contre la lib pour reutiliser sans dupliquer.
target_link_libraries(owl-build PRIVATE owl_core owl_runtime_console)
set_target_properties(owl-build PROPERTIES LINKER_LANGUAGE CXX)
if(WIN32)
    target_link_options(owl-build PRIVATE
        -Wl,-subsystem,console
        -static -static-libgcc -static-libstdc++
    )
endif()
if(OWL_RELEASE_DISTRIB)
    target_link_options(owl-build PRIVATE -Wl,--strip-all)
endif()
owl_strip_pdb(owl-build)
add_dependencies(owl-build owl_distrib)

# Apres le link de owl, extrait sa link cmd dans
# build/lib/<target_triple>/owl_link_gui.cfg pour que owl-build puisse
# reproduire le meme link contre un bundle.c. Le sous-dossier <triple>
# permet le cross-build (Phase B.5 : owl-build --target X).
if(OWL_BUILD_OWL)
    add_custom_command(TARGET owl POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DBUILD_DIR=${CMAKE_BINARY_DIR}
                -DOUT_CFG=${OWL_LIB_OUT}/owl_link_gui.cfg
                -DTARGET_TRIPLE=${OWL_TARGET_TRIPLE}
                -DQT6_ROOT=${OWL_QT6_INSTALL}
                -DDUCKDB_ROOT=${OWL_DUCKDB_INSTALL}
                -DLLAMA_ROOT=${OWL_LLAMA_INSTALL}
                -P ${CMAKE_SOURCE_DIR}/scripts/extract_link_spec.cmake
        VERBATIM
    )
endif()

# ------------------------------------------------------------------
# Doc HTML offline
# owl lit le template committe et injecte les donnees registry :
#   owl --dump-doc-html <template> > docs/reference/owl-doc.html
# Regenere a chaque build (DEPENDS owl), meme logique que le catalog LSP.
# Pas de relink registry, pas de sous-commande user-facing (flag interne).
# ------------------------------------------------------------------
set(OWL_DOC_TEMPLATE "${CMAKE_SOURCE_DIR}/docs/reference/_owl-doc-template.html")
set(OWL_DOC_HTML     "${CMAKE_SOURCE_DIR}/docs/reference/owl-doc.html")
set(OWL_DOC_HTML_TMP "${CMAKE_BINARY_DIR}/owl-doc.html.tmp")
# Le dump tourne a CHAQUE build (DEPENDS owl) : c'est ce qui garantit qu'aucun
# changement de registre ne peut passer inapercu, sans avoir a maintenir la
# liste des sources qui l'alimentent (un oubli = doc perimee en silence).
# Mais on n'ECRIT la sortie que si les octets ont bouge : sinon le mtime ne
# bouge pas, et rien en aval ne se recompile. Ecrire 2,4 Mo sans raison a
# chaque relink coutait cher sous antivirus temps reel.
add_custom_command(
    OUTPUT ${OWL_DOC_HTML}
    COMMAND $<TARGET_FILE:owl> --dump-doc-html ${OWL_DOC_TEMPLATE} > ${OWL_DOC_HTML_TMP}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OWL_DOC_HTML_TMP} ${OWL_DOC_HTML}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${OWL_DOC_HTML_TMP}
    DEPENDS owl ${OWL_DOC_TEMPLATE}
    COMMENT "Generate docs/reference/owl-doc.html from registry"
    VERBATIM
)
add_custom_target(owl_doc_html ALL DEPENDS ${OWL_DOC_HTML})

# ------------------------------------------------------------------
# Cible : owl-lsp.exe (Language Server)
# ------------------------------------------------------------------
if(OWL_BUILD_LSP)
    # Auto-regen du catalog avant de build le LSP. Le catalog dump
    # n'instancie aucune ressource Qt, juste lit le registry — donc OK
    # meme avec owl all-in-one qui contient le backend Qt6.
    set(LSP_CATALOG_GEN     "${CMAKE_SOURCE_DIR}/lsp/lsp_catalog_gen.c")
    set(LSP_CATALOG_GEN_TMP "${CMAKE_BINARY_DIR}/lsp_catalog_gen.c.tmp")
    # Meme raisonnement que owl-doc.html ci-dessus, avec un enjeu plus fort :
    # ce .c est une SOURCE d'owl-lsp. Le dump reste inconditionnel (donc le
    # catalogue ne peut jamais partir perime en release), mais tant que les
    # octets sont identiques on ne touche pas le fichier — sinon un one-liner
    # dans compiler.c, qui n'alimente pas le registre, payait quand meme la
    # recompilation des 857 Ko generes puis le relink d'owl-lsp, owl-lsp-test
    # et owl-dap-test.
    add_custom_command(
        OUTPUT ${LSP_CATALOG_GEN}
        COMMAND $<TARGET_FILE:owl> --dump-lsp-catalog > ${LSP_CATALOG_GEN_TMP}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LSP_CATALOG_GEN_TMP} ${LSP_CATALOG_GEN}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${LSP_CATALOG_GEN_TMP}
        DEPENDS owl
        VERBATIM
    )

    # Sources LSP partagees entre l'exe LSP et le harness de test.
    set(OWL_LSP_LIB_SRCS
        lsp/lsp_server.c
        lsp/lsp_handlers.c
        lsp/lsp_scope.c               #  -- moteur de portee/liaison
        lsp/lsp_document_structure.c  #  -- documentSymbol + foldingRange (stub)
        lsp/lsp_json.c
        lsp/lsp_memory.c
        lsp/lsp_catalog.c
        lsp/lsp_proc.c          #  : spawn borne (timeout owl --check)
        ${LSP_CATALOG_GEN}
        src/scanner.c
        # Scanner consomme le registry owl_errors depuis  pour
        # signaler ses 6 erreurs lex (Unterminated string / backtick /
        # multiline / Unexpected character ...). Pas de dependance externe.
        src/owl_errors.c
        # Catalogue d'events DSL — partage avec owl_core mais le LSP ne
        # link pas owl_core (volontaire : LSP leger). 2 fichiers pas de
        # dependance externe, OK de les compiler dans le LSP aussi.
        src/owl_event_catalog.c
        src/event_catalog_widgets.c
        # Catalogue pedagogique des mots-cles (fiches descFR/descEN +
        # label_en + syntax par TokenType). Source unique consultee par le
        # LSP pour la completion + le hover bilingues des mots-cles
        #. Ne depend que du scanner (deja linke) + string.h —
        # aucune dependance VM/registre.
        src/owl_keyword_catalog.c
        # ui_decl.c expose CONTROL_KINDS + owl_control_kind_from_token,
        # source unique consultee par le LSP pour reconnaitre les widgets
        # DSL. Avant le refactor, le LSP dupliquait les noms en dur.
        # ui_decl.c n'a pas de dependances internes (utilise juste stdlib.h
        # pour malloc/free et scanner_strip_accents qui est deja dans le
        # build LSP).
        src/ui_decl.c
        # Formatter hebergé dans le LSP (ticket 0326) : meme formatter.c que
        # owl.exe, branche sur le backend CATALOGUE (formatter_canon_catalog.c
        # -> tables statiques generees) au lieu du registre runtime. prescan +
        # project (partie discover, compiler-free) pour le contexte cross-
        # fichier. owl_name_normalize partage avec le registre -> normalisation
        # identique. Aucun de ces fichiers ne tire la VM.
        src/formatter.c
        src/formatter_canon_catalog.c
        src/formatter_context.c
        src/owl_name_normalize.c
        src/prescan.c
        src/project.c
        #  — completion `icones.<prefix>` cote LSP : besoin du
        # catalogue Iconify (OWL_ICONS_CATALOG / OWL_ICONS_SET_NAMES)
        # pour generer les items. Le fichier est genere au build par
        # icons_catalog_gen et ne contient que des tables statiques
        # (pas de dependance zstd — la decompression du blob SVG reste
        # cote owl.exe via stdlib_icones).
        "${CMAKE_BINARY_DIR}/icons_catalog_gen.c"
    )

    add_executable(owl-lsp
        lsp/lsp_main.c
        ${OWL_LSP_LIB_SRCS}
    )
    target_include_directories(owl-lsp PRIVATE src lsp)
    target_compile_options(owl-lsp PRIVATE
        -Wall -Wextra -Werror -Wno-missing-field-initializers
        # Hygiène (aligné sur owl.exe) : sections séparées + gc-sections au
        # link drop le code/data mort divers. NB : ça NE suffit PAS à virer le
        # blob d'icônes (donnée à linkage externe, --gc-sections PE ne mord pas
        # dessus) — cf. OWL_ICONS_NO_BLOB ci-dessous.
        -ffunction-sections -fdata-sections
    )
    #  — le LSP linke icons_catalog_gen.c (, complétion
    # `icones.`) mais n'utilise QUE les noms (OWL_ICONS_CATALOG). Le blob SVG
    # OWL_ICONS_BLOB_ZSTD (~11,5 MB) n'est référencé que par stdlib_icones.c
    # (owl.exe), pas par le LSP. On l'EXCLUT de la compilation côté LSP via la
    # garde #ifndef OWL_ICONS_NO_BLOB du fichier généré. owl-lsp : ~15 → ~3 MB.
    target_compile_definitions(owl-lsp PRIVATE OWL_ICONS_NO_BLOB)
    target_link_options(owl-lsp PRIVATE -Wl,--gc-sections)
    owl_strip_pdb(owl-lsp)

    if(OWL_DEV_TOOLS)
        # Harness de test C : drive le LSP en in-process (zero IPC).
        # Lance via 'make test-lsp'. Voir tests/lsp/lsp_test_main.c.
        add_executable(owl-lsp-test
            tests/lsp/lsp_test_main.c
            # Gate de parite fmt-stability backend catalogue (ticket 0326) :
            # reutilise le meme harness qu'owl.exe, lance via
            # `owl-lsp-test --self-test-fmt-stability`.
            src/internal_tests/fmt_stability_test.c
            ${OWL_LSP_LIB_SRCS}
        )
        target_include_directories(owl-lsp-test PRIVATE src lsp)
        target_compile_options(owl-lsp-test PRIVATE
            -Wall -Wextra -Werror -Wno-missing-field-initializers
        )
        target_compile_definitions(owl-lsp-test PRIVATE OWL_ICONS_NO_BLOB)  # 
        owl_strip_pdb(owl-lsp-test)
    endif()

    if(OWL_DEV_TOOLS)
        # Benchmark perf : mesure handle_completion sur un fichier reel.
        add_executable(owl-lsp-bench
            tests/lsp/lsp_bench.c
            ${OWL_LSP_LIB_SRCS}
        )
        target_include_directories(owl-lsp-bench PRIVATE src lsp)
        target_compile_definitions(owl-lsp-bench PRIVATE OWL_ICONS_NO_BLOB)  # 
        owl_strip_pdb(owl-lsp-bench)
    endif()
endif()

# ------------------------------------------------------------------
# Cible : owl-dap.exe (Debug Adapter — Lot D)
# ------------------------------------------------------------------
# Reutilise lsp/lsp_json.{c,h} (parser/serializer JSON commun aux deux
# protocoles JSON-RPC). Mutualisation explicite via include path lsp/.
# Le DAP linke aussi owl_core pour acceder a debug_api.h (D.1) — la VM
# debuggee tournera ici-meme dans D.2.3 quand on aura le threading.
if(OWL_BUILD_DAP)
    add_executable(owl-dap
        dap/dap_main.c
        dap/dap_server.c
        dap/dap_child.c
        # lsp_json.c est deja dans owl_core (linke ci-dessous), pas de
        # double compilation.
    )
    target_include_directories(owl-dap PRIVATE src lsp dap)
    target_link_libraries(owl-dap PRIVATE owl_core owl_vendor)
    set_target_properties(owl-dap PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_options(owl-dap PRIVATE
        -Wall -Wextra -Werror -Wno-missing-field-initializers
    )
    owl_strip_pdb(owl-dap)

    if(OWL_DEV_TOOLS)
        # Harness boite noire : spawn owl-dap.exe et lui parle en vrai DAP.
        # Ne linke PAS owl_core (98 MB de Qt inutiles) — juste lsp_json.c
        # pour parser le framing. Lance via 'make test-dap'.
        add_executable(owl-dap-test
            tests/dap/dap_test_main.c
            lsp/lsp_json.c
        )
        target_include_directories(owl-dap-test PRIVATE lsp)
        target_compile_options(owl-dap-test PRIVATE
            -Wall -Wextra -Werror -Wno-missing-field-initializers
        )
        owl_strip_pdb(owl-dap-test)
    endif()
endif()

# ------------------------------------------------------------------
# Status summary
# ------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== OWL build configuration ====")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler      : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  Build owl.exe : YES (all-in-one : interpreter + GUI Qt6 + PDF qpdf)")
message(STATUS "  Build owl-lsp : ${OWL_BUILD_LSP}")
message(STATUS "  Build owl-dap : ${OWL_BUILD_DAP}")
message(STATUS "  Dev tools     : ${OWL_DEV_TOOLS} (owl-cli, owl-lsp-test, owl-lsp-bench)")
message(STATUS "==================================")
message(STATUS "")
