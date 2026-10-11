# Makefile - delegue vers CMake/Ninja (toolchain UCRT64).
#
# Cibles principales :
#   make setup        # one-time : submodules + zig + Qt6 (gcc + zig) + DuckDB
#   make              # Build owl.exe + owl-lsp.exe (~25s full, ~2s incremental)
#                     # + batterie de controles, arret au premier echec
#   make check-all    # Meme batterie sans arret : etat complet + recapitulatif
#   make mega         # Build owl-build.exe single-file cross-compile (~5 min)
#   make test         # Lance les tests OWL
#   make run          # Lance owl.exe sur examples/login.owl
#   make clean        # rm -rf build/
#   make qt6-clean    # rm -rf Qt6 builds (force rebuild)
#   make distclean    # tout nettoyer y compris Qt6 + zig

# Toolchain prefix : auto-detection MSYS2 UCRT64 sur Windows. Override via
# OWL_TOOLCHAIN_PREFIX si le contributeur a un autre layout (ex: msys2 dans
# D:/msys64). Sur Linux/macOS la variable n'est pas utilisee, cmake/ninja
# viennent du PATH systeme.
ifeq ($(OS),Windows_NT)
    PREFIX_PATH ?= $(if $(OWL_TOOLCHAIN_PREFIX),$(OWL_TOOLCHAIN_PREFIX),/c/msys64/ucrt64)
    PATH_GUARD  := $(PREFIX_PATH)/bin/cmake.exe
    CMAKE       := $(PREFIX_PATH)/bin/cmake.exe
    export PATH := $(PREFIX_PATH)/bin:$(PATH)
else
    PREFIX_PATH ?=
    CMAKE       := cmake
    PATH_GUARD  :=
endif

BUILDDIR           = build
# Pre-build dirs regroupes sous _local/ (gitignore en bloc). Aucun n'est
# committe — tous regenerables via setup / build_qt6_zig_native.sh /
# build_duckdb_zig_native.sh.
BUILDDIR_QT6        = _local/host_tools/qt6_build
BUILDDIR_QT6_INST   = _local/host_tools/qt6
QT6_SOURCE          = vendor/qt6/qtbase
QT6_INSTALL_MARKER  = $(BUILDDIR_QT6_INST)/lib/cmake/Qt6/Qt6Config.cmake
# qtsvg : module Qt6 build sur qtbase pour QSvgRenderer ( icones).
# Installe dans le meme prefix que qtbase, accessible via find_package(Qt6 COMPONENTS Svg).
BUILDDIR_QTSVG      = _local/host_tools/qtsvg_build
QTSVG_SOURCE        = vendor/qt6/qtsvg
QTSVG_INSTALL_MARKER = $(BUILDDIR_QT6_INST)/lib/cmake/Qt6Svg/Qt6SvgConfig.cmake

# ----------------------------------------------------------
# Qt6 vendore : build local depuis vendor/qt6/qtbase (lance une fois,
# ~30-60 min). Installe dans _local/host_tools/qt6/. Toutes les libs deps
# (pcre2, freetype, harfbuzz, libpng, zlib, etc.) sont bundled — zero
# dependance MSYS2. Toolchain entierement embarquee, autonome.
#
# -static-libstdc++ -static-libgcc sur les host tools (moc/rcc/syncqt) :
# les host tools tournent pendant le build Qt6 zig et doivent rester
# fonctionnels meme si MSYS2 met a jour libstdc++-6.dll (qui peut perdre
# des symboles std::filesystem entre versions, cf incident gcc 15.2).
# En statique = autonomes, immunises aux maj systeme.
# ----------------------------------------------------------
qt6: $(QT6_INSTALL_MARKER) $(QTSVG_INSTALL_MARKER)

$(QT6_INSTALL_MARKER):
	@if [ ! -d "$(QT6_SOURCE)" ]; then \
	    echo "Qt6 source manquante. Clone-la d'abord :" && \
	    echo "  mkdir -p vendor/qt6 && cd vendor/qt6 && \\" && \
	    echo "  git clone --depth 1 --branch v6.8.1 https://code.qt.io/qt/qtbase.git qtbase" && \
	    exit 1; \
	fi
	@echo "==== Building Qt6 from $(QT6_SOURCE) (~30-60 min, one-time) ===="
	$(CMAKE) -S $(QT6_SOURCE) -B $(BUILDDIR_QT6) -GNinja \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX="$(CURDIR)/$(BUILDDIR_QT6_INST)" \
	  -DBUILD_SHARED_LIBS=OFF \
	  -DCMAKE_EXE_LINKER_FLAGS_INIT="-static-libstdc++ -static-libgcc" \
	  -DFEATURE_optimize_size=ON \
	  -DFEATURE_dbus=OFF -DFEATURE_sql=OFF -DFEATURE_network=OFF \
	  -DFEATURE_xml=OFF -DFEATURE_testlib=OFF -DFEATURE_concurrent=OFF \
	  -DBUILD_TESTING=OFF \
	  -DFEATURE_system_pcre2=OFF -DFEATURE_system_zlib=OFF \
	  -DFEATURE_system_freetype=OFF -DFEATURE_system_harfbuzz=OFF \
	  -DFEATURE_system_doubleconversion=OFF
	$(CMAKE) --build $(BUILDDIR_QT6) --target install
	@echo "==== Qt6 installed in $(BUILDDIR_QT6_INST) ===="

# qtsvg (host tools) — module Qt6 Svg pour QSvgRenderer. Depend de qtbase
# deja installe, install dans le meme prefix. Build ~5 min sur qtbase
# existant. Le marker depend de QT6_INSTALL_MARKER pour ordonner.
# FEATURE_svg_widget=OFF : on n'utilise pas QSvgWidget (qui necessiterait
# Qt6::Widgets a build-time). QSvgRenderer + QSvgGenerator suffisent
# pour rasteriser des SVG en QPixmap/QImage cote .
$(QTSVG_INSTALL_MARKER): $(QT6_INSTALL_MARKER)
	@if [ ! -d "$(QTSVG_SOURCE)" ]; then \
	    echo "qtsvg source manquante. Lance : git submodule update --init" && \
	    exit 1; \
	fi
	@echo "==== Building qtsvg from $(QTSVG_SOURCE) (~5 min) ===="
	$(CMAKE) -S $(QTSVG_SOURCE) -B $(BUILDDIR_QTSVG) -GNinja \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_PREFIX_PATH="$(CURDIR)/$(BUILDDIR_QT6_INST)" \
	  -DCMAKE_INSTALL_PREFIX="$(CURDIR)/$(BUILDDIR_QT6_INST)" \
	  -DBUILD_SHARED_LIBS=OFF \
	  -DCMAKE_EXE_LINKER_FLAGS_INIT="-static-libstdc++ -static-libgcc" \
	  -DFEATURE_svg_widget=OFF \
	  -DBUILD_TESTING=OFF
	$(CMAKE) --build $(BUILDDIR_QTSVG) --target install
	@echo "==== qtsvg installed in $(BUILDDIR_QT6_INST) ===="

qt6-clean:
	rm -rf $(BUILDDIR_QT6) $(BUILDDIR_QT6_INST) $(BUILDDIR_QTSVG)

# ----------------------------------------------------------
# qt6-linux — host tools Qt6 Linux (ELF) figes dans
# _local/host_tools/qt6-linux, distincts du canonique qt6 (qui porte les .exe
# Windows en repo partage MSYS2<->WSL2). A lancer DEPUIS Linux/WSL2. PATH
# assaini (sans /mnt/c) pour ne pas ramasser libs/headers MSYS2 Windows.
# Resolu par defaut par build_owl_musl.sh -> reproductible, plus d'etape manuelle.
# ----------------------------------------------------------
qt6-linux:
	@PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" $(MAKE) qt6 CMAKE=cmake BUILDDIR_QT6_INST=_local/host_tools/qt6-linux BUILDDIR_QT6=_local/host_tools/qt6-linux_build BUILDDIR_QTSVG=_local/host_tools/qtsvg-linux_build

# ----------------------------------------------------------
# Setup one-time : submodules + zig vendore + Qt6 gcc + Qt6 zig native.
# ~1h30 a froid (90% Qt6 builds), idempotent. Chaine tout ce dont un
# nouveau dev a besoin pour faire `make` ET `make mega` ensuite.
# ----------------------------------------------------------
setup:
	@echo "==== [1/5] git submodules (Qt6, qpdf, zlib, libjpeg-turbo, DuckDB) ===="
	git submodule update --init --recursive
	@echo "==== [2/5] zig vendor + wrappers + winapi_stubs ===="
	bash scripts/setup_zig.sh
	@echo "==== [3/5] Qt6 gcc statique (_local/host_tools/qt6/, ~30 min) ===="
	@$(MAKE) qt6
	@echo "==== [4/5] Qt6 zig native (_local/qt6/<host>/install/, ~30 min) ===="
	bash scripts/build_qt6_zig_native.sh
	@echo "==== [5/5] DuckDB zig native (_local/duckdb/<host>/install/, ~15 min) ===="
	bash scripts/build_duckdb_zig_native.sh
	@echo "==== Setup OK. Tu peux lancer : make && make mega ===="

# ----------------------------------------------------------
# Build release : produit release/owl[.exe] (~78 MB interpreteur all-in-one)
# + release/owl-build[.exe] (~550 MB single-file packager). Necessite
# setup complet (zig + Qt6 zig native + DuckDB zig).
# ----------------------------------------------------------
mega:
	bash scripts/build_mega.sh

# ----------------------------------------------------------
# Build release publique : produit release/owl[.exe] + owl-lsp[.exe] +
# owl-dap[.exe], statiques et en config OWL_RELEASE_DISTRIB (surface publique
# seule : pas de --self-test-*, pas de snapshot ; cf. ). C'est ce que
# l'orchestrateur d'installateur public consomme. owl-build est
# hors scope ici (cf. `make mega`). Memes pre-requis que mega (zig + Qt6 native).
# ----------------------------------------------------------
release:
	bash scripts/build_release.sh

# ----------------------------------------------------------
# Release Linux musl-static, orchestree depuis Windows via WSL2. Le build musl
# est intrinsequement Linux (zig cross-compile depuis Linux) -> on delegue a
# WSL2 (build natif Linux, host tools Qt6 Linux figes dans _local/host_tools/
# qt6-linux, resolus par defaut sans OWL_QT_HOST_PATH). Produit release/
# linux-musl/{owl,owl-lsp,owl-dap}. Distro overridable via WSL_DISTRO.
# wslpath traduit le CWD Windows ($(pwd -W)) en chemin /mnt/c/... pour WSL.
# ----------------------------------------------------------
WSL_DISTRO ?= Ubuntu-24.04
release-linux:
	@wsl_dir=$$(wsl.exe -d $(WSL_DISTRO) -e wslpath -u "$$(pwd -W)"); \
	wsl.exe -d $(WSL_DISTRO) -e bash -lc "cd \"$$wsl_dir\" && bash scripts/build_owl_musl.sh"

# Release complete cross-plateforme depuis Windows : Windows (release) PUIS
# Linux musl (release-linux). Resultat : release/ (Windows) + release/linux-musl/
# (Linux) en une commande.
release-all: release release-linux

# ----------------------------------------------------------
# Verif rapide de portabilite musl (compile SEULE, NON-destructif : n'ecrit PAS
# dans release/linux-musl/). A lancer apres avoir touche du bas-niveau POSIX
# (net/http/pthread/time) : le chemin Linux/musl n'est pas dans le CI, il rote
# en silence sinon (cf. binaire 1.0.6 shippe par erreur en 1.0.7/1.0.8).
# Reutilise les deps musl deja provisionnees + le BUILD_DIR persistant -> rapide
# (incremental). Provision one-time des deps : OWL_MUSL=1 build_llama_zig_native.sh
# + build_qt6/duckdb zig native.
# ----------------------------------------------------------
musl-check:
	@wsl_dir=$$(wsl.exe -d $(WSL_DISTRO) -e wslpath -u "$$(pwd -W)"); \
	wsl.exe -d $(WSL_DISTRO) -e bash -lc "cd \"$$wsl_dir\" && OWL_MUSL_CHECK=1 bash scripts/build_owl_musl.sh"

# ----------------------------------------------------------
# Toolchain : zig clang+lld+libc++ (vendore via vendor/zig/).
#
# Unification 2026-05 : on a quitte MSYS2 GCC+libstdc++ qui posait des
# problemes d'ABI C++ et bugs de codegen (typeinfo weak comdat pour
# DuckDB amalgamation par exemple). zig fournit clang 21 + LLD + libc++,
# c'est aligne avec `make mega` (cross-compile single-file) et debloque
# DuckDB static.
#
# Pre-requis :
#  - vendor/zig/ + scripts/zig_wrappers/*.exe (via scripts/setup_zig.sh)
#  - _local/qt6/<host>/install/ (via scripts/build_qt6_zig_native.sh)
#  - _local/host_tools/qt6/ (GCC, fournit moc/rcc/syncqt — necessaire au
#    bootstrap car Qt6 ne peut pas auto-compiler ses propres host tools.
#    Linke en -static-libstdc++ -static-libgcc pour etre immunise aux
#    mises a jour MSYS2 qui retirent des symboles std::filesystem.)
#
# MSYS2 UCRT64 reste requis pour : cmake, ninja, mingw32-make, gcc.
# Le gcc n'est plus utilise que pour le bootstrap Qt6 host tools.
# ----------------------------------------------------------
QT6_ZIG_INST = _local/qt6/x86_64-windows/install

all: qt6 $(BUILDDIR)/build.ninja
	@OWL_BUILDDIR=$(BUILDDIR) OWL_CMAKE="$(CMAKE)" bash scripts/run_all_checks.sh

# Boucle de dev : meme batterie que `all`, moins les deux controles longs.
# Mesure de la batterie complete : 511 s, dont 203 s pour compiler-tests (182
# cas, un spawn d'owl.exe chacun) et 200 s pour net (handshakes TLS + SQL
# reels). Ces deux-la valent 79 % du temps et ne repondent quasiment jamais a
# l'edition en cours -> hors de la boucle courte, mais TOUJOURS dans `all`.
#
# `quick` sert a iterer ; `all` reste le portail obligatoire avant de rendre
# un lot ou de committer. Un vert `quick` n'est PAS un vert `all`.
# hook-tests suit la meme logique : 23 s pour +26 % sur cette boucle,
# et les hooks ne bougent JAMAIS pendant une iteration sur src/. Le cout vient des
# 34 cas dry-run, pas du cas reel : chaque invocation du hook spawn ~12 sous-process
# que MSYS facture 0,69 s. Mesure, pas estimation.
quick: qt6 $(BUILDDIR)/build.ninja
	@OWL_BUILDDIR=$(BUILDDIR) OWL_CMAKE="$(CMAKE)" bash scripts/run_all_checks.sh --skip=compiler-tests,net,hook-tests

# Meme batterie, mais sans arret au premier echec : tous les controles
# independants tournent, recapitulatif final, exit != 0 s'il reste un rouge
#. A utiliser pour mesurer l'etat reel du depot ; `make` reste
# fail-fast pour la boucle de dev et les hooks.
# Rejouer un controle seul (faux rouge de build concurrent) :
#   bash scripts/run_all_checks.sh --only=compiler-tests
#   bash scripts/run_all_checks.sh --list
check-all: qt6 $(BUILDDIR)/build.ninja
	@OWL_BUILDDIR=$(BUILDDIR) OWL_CMAKE="$(CMAKE)" bash scripts/run_all_checks.sh --keep-going

$(BUILDDIR)/build.ninja:
	@if [ ! -f "$(QT6_ZIG_INST)/lib/cmake/Qt6/Qt6Config.cmake" ]; then \
	    echo "Qt6 zig native manquant. Lance : bash scripts/build_qt6_zig_native.sh"; \
	    exit 1; \
	fi
	$(CMAKE) -G Ninja -B $(BUILDDIR) \
	  -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/cmake/toolchain_zig_native.cmake \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DOWL_QT6_INSTALL=$(CURDIR)/$(QT6_ZIG_INST) \
	  -DQT_HOST_PATH=$(CURDIR)/$(BUILDDIR_QT6_INST) \
	  -DOWL_DEV_TOOLS=ON

# ----------------------------------------------------------
# Tests
# ----------------------------------------------------------
test: all
	./$(BUILDDIR)/owl.exe test

# ----------------------------------------------------------
# Tests GUI : smoke tests Qt (PDF, fenetres). owl.exe inclut maintenant
# le backend GUI (all-in-one) donc on peut directement test tests/gui/.
# ----------------------------------------------------------
test-gui: all
	./$(BUILDDIR)/owl.exe test tests/gui/

# ----------------------------------------------------------
# Test LSP (harness C in-process — voir tests/lsp/lsp_test_main.c)
# ----------------------------------------------------------
test-lsp: all
	./$(BUILDDIR)/owl-lsp-test.exe

# ----------------------------------------------------------
# Test DAP (harness C boite noire — spawn owl-dap.exe et lui parle
# en vrai protocole DAP. Voir tests/dap/dap_test_main.c)
# ----------------------------------------------------------
test-dap: all
	OWL_DAP_EXE=./$(BUILDDIR)/owl-dap.exe OWL_EXE=./$(BUILDDIR)/owl.exe ./$(BUILDDIR)/owl-dap-test.exe

# ----------------------------------------------------------
# Tests de conformance vs oracle externe (executable proprietaire).
# Hors de `make test` par defaut : dependent d'un executable proprietaire
# absent du repo. Skip propre (exit 0) si l'oracle est introuvable, donc
# safe en CI sans l'oracle installe. Voir tests/conformance/README.md.
# ----------------------------------------------------------
test-conformance: all
	@bash tests/conformance/run.sh

# ----------------------------------------------------------
# Self-test registry : verifie les invariants du langage (cf
# docs/internal_documentation/cahier_des_charges_langage.md). Hurle si une constante
# accentuee n'a pas son alias ASCII, si une valeur d'enum n'existe pas,
# etc. 0 violation attendu sur main.
# ----------------------------------------------------------
self-test-registry: all
	@./$(BUILDDIR)/owl.exe --self-test-registry > /dev/null && \
	    echo "self-test-registry: OK (0 violation)" || \
	    (echo "self-test-registry: VIOLATIONS — relancer pour le detail :" && \
	     ./$(BUILDDIR)/owl.exe --self-test-registry; exit 1)

# ----------------------------------------------------------
# Audits de dérive du code : détectent les surfaces "branchées à rien"
# (fonctions exportées sans caller, constantes orphelines en user-facing).
# Hors `make all` par défaut — opt-in. Idéal pour les revues périodiques
# et la gate `make audit-strict` qui peut être intégrée à un CI ou hook
# pre-merge si on veut bloquer la dérive (seuils via env vars :
# OWL_DEAD_CODE_THRESHOLD, OWL_ORPHAN_CONST_THRESHOLD).
# ----------------------------------------------------------
audit-dead-code: all
	@if [ -f scripts/audit_dead_code.sh ]; then \
		bash scripts/audit_dead_code.sh; \
	else echo "scripts/audit_dead_code.sh non publie (outillage interne, repo prive)"; fi

audit-orphan-constants: all
	@if [ -f scripts/audit_orphan_constants.sh ]; then \
		bash scripts/audit_orphan_constants.sh; \
	else echo "scripts/audit_orphan_constants.sh non publie (outillage interne, repo prive)"; fi

audit: audit-dead-code audit-orphan-constants

audit-strict: all
	@if [ -f scripts/audit_dead_code.sh ]; then \
		bash scripts/audit_dead_code.sh --strict; \
	else echo "scripts/audit_dead_code.sh non publie (outillage interne, repo prive)"; fi
	@if [ -f scripts/audit_orphan_constants.sh ]; then \
		bash scripts/audit_orphan_constants.sh --strict; \
	else echo "scripts/audit_orphan_constants.sh non publie (outillage interne, repo prive)"; fi

# ----------------------------------------------------------
# Snapshot du registre : versionne tools/registry_snapshot.json pour
# que toute PR montre clairement quels symboles ont ete ajoutes/retires.
# Le snapshot est dump par owl --vscode-data (format JSON existant).
# Lance manuellement par 'make snapshot'. CI verifie via 'make snapshot-check'
# que le snapshot versionne est a jour.
# ----------------------------------------------------------
# snapshot (LIBRE, per-change, appartient a owl) : regenere UNIQUEMENT la baseline
# anti-regression du registre. A relancer des qu'un ticket change le registre
# (natives/types/constantes). Ne touche PLUS la baseline des fiches Nouveautes
# (cf. doc-baseline) : cadences opposees, elles se marchaient dessus.
snapshot: all
	./$(BUILDDIR)/owl.exe --vscode-data > tools/registry_snapshot.json
	@echo "Snapshot registre regenere : tools/registry_snapshot.json"

# doc-baseline (RELEASE UNIQUEMENT, moment choisi a la main) : fige les fiches de
# la version SORTIE comme reference de la page 'Nouveautes' (Nouveautes = fiches
# courantes - cette baseline). A lancer APRES une release, jamais entre deux
# (sinon la page Nouveautes se vide). Pilote par la routine de release publique.
doc-baseline: all
	./$(BUILDDIR)/owl.exe --dump-doc-fiches > tools/doc_fiches_baseline.tsv
	@echo "Baseline fiches Nouveautes refigee : tools/doc_fiches_baseline.tsv"
	@echo "  (reference pour la PROCHAINE version — ne pas relancer avant la prochaine release)"

snapshot-check: all
	@./$(BUILDDIR)/owl.exe --vscode-data > tools/registry_snapshot.tmp.json
	@if diff -q tools/registry_snapshot.json tools/registry_snapshot.tmp.json > /dev/null 2>&1; then \
	    rm tools/registry_snapshot.tmp.json; \
	    echo "snapshot-check: OK (a jour)"; \
	else \
	    echo "snapshot-check: snapshot OBSOLETE. Lancer 'make snapshot' et committer le diff."; \
	    diff tools/registry_snapshot.json tools/registry_snapshot.tmp.json | head -40; \
	    rm tools/registry_snapshot.tmp.json; \
	    exit 1; \
	fi

# ----------------------------------------------------------
# VSCode plugin : regenere tmLanguage + package.json + prop_registry_gen.js
# (TextMate grammar, designer JS, LSP catalog) depuis owl --vscode-data.
# Apres 'make vscode' il reste a recharger la fenetre VSCode pour voir
# les changements (Ctrl+Shift+P > Developer: Reload Window). Pour un
# rebuild complet du VSIX + reinstallation : voir 'make vscode-install'.
# ----------------------------------------------------------
vscode: all
	@if [ -d ../owl-integration ]; then \
		echo "==> Regen artefacts VSCode depuis owl --vscode-data"; \
		cd ../owl-integration && python generate_vscode.py --owl-exe ../owl/$(BUILDDIR)/owl.exe; \
		echo "==> Pour appliquer : VSCode Ctrl+Shift+P > Developer: Reload Window"; \
	else \
		echo "owl-integration absent — extension VS Code fournie en asset de release (cible mainteneur)"; \
	fi

# Idem mais lance aussi le build.ps1 qui rebuild + reinstalle le VSIX.
# Necessite PowerShell + droits d'install code-cli. Plus lourd que 'vscode'
# mais zero manuel apres.
vscode-install: all
	@if [ -d ../owl-integration ]; then \
		echo "==> Regen + rebuild VSIX + reinstall via build.ps1"; \
		cd ../owl-integration && powershell -File build.ps1 -OwlExe ../owl/$(BUILDDIR)/owl.exe; \
	else \
		echo "owl-integration absent — extension VS Code fournie en asset de release (cible mainteneur)"; \
	fi

# ----------------------------------------------------------
# Run dev (Qt6 vendore static, plus besoin de DLL Qt dans le PATH ;
# qpdf reste dynamic donc PATH UCRT64 requis pour libqpdf.dll).
# ----------------------------------------------------------
run: all
	./$(BUILDDIR)/owl.exe examples/login.owl

# ----------------------------------------------------------
# Cleanup
# ----------------------------------------------------------
clean:
	rm -rf $(BUILDDIR)

distclean: clean qt6-clean
	rm -f owl.exe owl-lsp.exe
	rm -rf vendor/zig/x86_64-* vendor/zig/aarch64-* vendor/zig/winapi_stubs
	rm -rf _local/ release/

# ----------------------------------------------------------
# Versions des libs vendorees (vendor/VERSIONS.md = source de verite).
# `make versions` affiche l'etat fige courant.
# `make check-updates` interroge upstream pour signaler les nouvelles releases.
# ----------------------------------------------------------
versions:
	@if [ -f scripts/check_vendor_versions.sh ]; then \
		bash scripts/check_vendor_versions.sh 2>/dev/null | head -25 || true; \
	else echo "scripts/check_vendor_versions.sh non publie (outillage interne, repo prive)"; fi
	@echo ""
	@echo "Doc complete + process bump : vendor/VERSIONS.md"

check-updates:
	@if [ -f scripts/check_vendor_versions.sh ]; then \
		bash scripts/check_vendor_versions.sh; \
	else echo "scripts/check_vendor_versions.sh non publie (outillage interne, repo prive)"; fi

#  — Dump SVG + JSON catalogue Iconify pour le plugin VSCode
# (designer panel browse icons, chantier task #18 phase 0a). Pattern
# coherent avec prop_registry_gen.js et lsp_catalog_gen.c : single-source
# au build owl/, plugin VSCode statique au runtime. Cache SHA256 interne.
# Output : ../owl-integration/vscode/icons-catalog/{svg/<cat>/<nom>.svg,
# catalog.json, fr_en.json, .stamp}.
vscode-icons-assets: $(BUILDDIR)/owl.exe
	@if [ -d ../owl-integration ]; then \
		echo "==> Generation icons-catalog/ pour le plugin VSCode..."; \
		./$(BUILDDIR)/owl.exe --gen-icon-assets ../owl-integration/vscode/icons-catalog/; \
	else \
		echo "owl-integration absent — extension VS Code fournie en asset de release (cible mainteneur)"; \
	fi

.PHONY: all quick check-all setup mega release release-linux release-all musl-check qt6 qt6-clean qt6-linux test test-gui test-lsp test-dap self-test-registry snapshot doc-baseline snapshot-check vscode vscode-install vscode-icons-assets run clean distclean versions check-updates
