# Vendor — versions pinnées

État au 2026-05-08. Toutes les libs externes sont vendorées (zéro
dépendance dynamique côté utilisateur). Ce tableau est la **source de
vérité** : si une version diffère ici de la réalité, c'est un bug.

`make versions` affiche l'état courant. `make check-updates` interroge
upstream pour signaler les nouvelles releases dispo.

## Submodules git (tags pinnés)

| Vendor | Version | Date pin OWL | Upstream | License |
|---|---|---|---|---|
| `qt6/qtbase` | v6.10.0 | 2026-04-26 | https://code.qt.io/qt/qtbase.git | LGPL-3.0 / commercial |
| `qt6/qtsvg` | v6.10.0 | 2026-06-06 | https://code.qt.io/qt/qtsvg.git | LGPL-3.0 / commercial |
| `qpdf` | v12.3.2 | 2026-05-08 | https://github.com/qpdf/qpdf.git | Apache-2.0 |
| `zlib` | v1.3.1 | 2026-05-08 | https://github.com/madler/zlib.git | Zlib |
| `libjpeg-turbo` | 3.1.4 | 2026-05-08 | https://github.com/libjpeg-turbo/libjpeg-turbo.git | BSD-3 + IJG |
| `duckdb/source` | v1.4.4 LTS | 2026-05-13 | https://github.com/duckdb/duckdb.git | MIT |
| `zstd` | v1.5.7 | 2026-06-06 | https://github.com/facebook/zstd.git | BSD-3 (dual GPL-2.0) |

**Bump submodule** (exemple qpdf 12.3.2 → 12.4.0) :

```bash
cd vendor/qpdf
git fetch --tags
git checkout v12.4.0
cd ../..
git add vendor/qpdf
make clean && make qt6 && make && make test && make mega
# Smoke test : un .owl avec iFinImprime + mot de passe → vérifier PDF chiffré
git commit -m "build(vendor): bump qpdf 12.3.2 → 12.4.0"
```

## Snapshots de données (script-driven, pas de submodule)

Snapshots manuels d'assets data, refresh via script bash.
Vendorés dans `vendor/<nom>/`, fichier `SNAPSHOT.md` interne avec date
de refresh + hash commit amont + SHA256 par fichier.

| Snapshot | Date refresh | Sources | Script refresh | Licences |
|---|---|---|---|---|
| `iconify-sets` | 2026-06-06 (commit amont `5d7fcf8`) | https://github.com/iconify/icon-sets (12 sets cherry-pickés cocktail T4, ~62 MB total snapshot — fluent multi-sizes deduplique au build T4 = `-24-` only ; icon-park ecarte par dev pour qualite visuelle insuffisante) | `scripts/refresh_iconify_sets.sh` | Apache-2.0 + MIT + CC0 + CC-BY-3.0 |

**Bump snapshot** : relancer le script. Idempotent (skip si SHA256 inchangé).
Fréquence recommandée pour iconify-sets : 6-12 mois.

## Toolchain externe (téléchargé)

| Outil | Version | Pin | Upstream |
|---|---|---|---|
| zig (clang+lld+sysroots) | 0.16.0 | `ZIG_VERSION` dans `scripts/setup_zig.sh:10` | https://ziglang.org/download/ |

**Bump zig** :

```bash
# Editer scripts/setup_zig.sh : ZIG_VERSION="0.17.0"
rm -rf vendor/zig/x86_64-windows
bash scripts/setup_zig.sh   # re-DL + recompile wrappers
make mega                    # vérif que le pipeline tient
git commit -am "build(vendor): bump zig 0.16.0 → 0.17.0"
```

## Sources copiées en dur (pas de submodule)

Ces libs sont copiées dans `vendor/<nom>/` directement (sources C, header-only,
ou bundle généré). Bumper = remplacer le contenu par le nouveau tarball.
Idéalement à migrer en submodule git un jour si l'upstream a un repo propre.

| Vendor | Version | Date pin OWL | Source | License |
|---|---|---|---|---|
| `mbedtls` | 3.6.2 | 2026-02 | https://github.com/Mbed-TLS/mbedtls/releases (LTS branch) | Apache-2.0 |
| `miniz` | 11.0.2 | 2026-02 | https://github.com/richgel999/miniz/releases | MIT |
| `pcre2` | 10.44 | 2026-02 | https://github.com/PCRE2Project/pcre2/releases | BSD-2 |
| `lzma_sdk` | 23.01 | 2026-02 | https://www.7-zip.org/sdk.html | Public Domain |
| `md4c` | (à daté) | — | https://github.com/mity/md4c | MIT |
| `mpdecimal` | 4.0.1 | 2026-06-27 | https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-4.0.1.tar.gz | BSD-2 |
| `sqlite` | 3.46.1 | 2026-05-10 | https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip | Public Domain |
| `zint` | 2.16.0 | 2026-07-18 | https://github.com/zint/zint/releases/tag/2.16.0 | BSD-3-Clause (backend only) |
| `argon2` | 20190702 | 2026-07-30 | https://github.com/P-H-C/phc-winner-argon2/releases/tag/20190702 | CC0-1.0 / Apache-2.0 (dual) |
| `llhttp` | 9.4.2 | 2026-07-30 | https://github.com/nodejs/llhttp — tag `release/v9.4.2` (commit `f831650b`) | MIT |
| `llama.cpp` | b10442 | 2026-08-15 | https://github.com/ggml-org/llama.cpp — tag `b10442` (commit `9b0a2ce8`) | MIT |
| `libxls` | 1.6.3 | 2026-08-26 | https://github.com/libxls/libxls (commit `881f6ec3`, master 2026-08-21) | BSD-2-Clause |
| `libssh2` | 1.11.2_DEV (commit `cdbda3cd`) | 2026-08-26 | https://github.com/libssh2/libssh2 (master, PAS le tag 1.11.1 — vulnerable) | BSD-3-Clause |
| `stb/stb_image.h` | v2.30 | — | https://github.com/nothings/stb | MIT/Public Domain |
| `ca/cacert.pem` | Mozilla 2026-02-11 | 2026-02 | https://curl.se/docs/caextract.html | MPL-2.0 |
| `winapi_stubs/*.a` | depuis MSYS2 ucrt64-x86_64 | 2026-05-08 | `mingw-w64-ucrt-x86_64-headers` | Public Domain (import libs) |

**Bump source copié** : DL le tarball release officiel, écraser le contenu de
`vendor/<nom>/`, vérifier que le `.gitignore` n'exclut rien d'important,
rebuild + test, commit.

**Note mpdecimal** : on ne vendore que `libmpdec/` (15 `.c` + headers internes),
pas la couche C++ `libmpdec++/` ni les tests/benchmarks. Le header public
`mpdecimal.h` est **généré une fois** depuis `mpdecimal.h.in` (bloc GNU
`MPD_CONFIG_64`, ABI 64-bit, visibility hidden), PAS le header VC
`mpdecimal64vc.h`. Build via `-DCONFIG_64 -DANSI -DHAVE_UINT128_T` (chemin
arithmétique portable `__uint128_t`, zéro asm inline → identique Win/Linux/mac).
Au bump : régénérer `mpdecimal.h` depuis le nouveau `.h.in` (le `configure`
upstream substitue le bloc CONFIG selon la cible ; reprendre le bloc 64-bit).

**Note zint** : projet globalement GPL-3, MAIS le dossier `backend/` est
**BSD-3-Clause** (arrangement dual acté par l'auteur en 2013, cf. `LICENSE` :
« Change to BSD-license is done for backend [...] for the frontends and
Qt4-backend the GPL is still valid »). On ne vendore **QUE** `backend/`
(52 `.c` + 44 `.h`), **jamais** les frontends CLI/GUI/Qt (GPL). Deux `.c`
backend écartés à la copie : `png.c` (seule source tirant une dép externe :
libpng + zlib) et `dllversion.c` (versioning DLL Windows via shlwapi, hors
bibliothèque). Build via `-DZINT_NO_PNG` (raster.c bypasse alors le chemin
PNG) → **zéro dépendance externe ajoutée**. `gs1.c` ne tire `gs1encoders.h`
que sous `ZINT_HAVE_GS1SE` (non défini). Sortie consommée : **buffer-only**
(`ZBarcode_Buffer`, raster RGB mémoire). Sources 100% C portable, zéro asm
inline → identique host gcc / zig clang sur Win/Linux/mac. Au bump :
re-télécharger le tarball release, réécraser `backend/` en ré-excluant
`png.c` et `dllversion.c`, revérifier les en-têtes de licence BSD-3.

**Note argon2** : implémentation de référence PHC, vendorée comme enabler du socle
d'authentification (mbedTLS ne fournit **aucun** hachage de mot de passe moderne,
seulement PBKDF2 ; Argon2id est le premier choix OWASP). Licence **dual
CC0-1.0 / Apache-2.0**, au choix du consommateur. On ne vendore que la
bibliothèque de calcul (5 `.c` : `argon2.c`, `core.c`, `encoding.c`, `ref.c`,
`blake2/blake2b.c`) : BLAKE2b est **embarqué**, zéro dépendance externe, pas même
`libm`. Écartés à la copie : `opt.c` + `blamka-round-opt.h` (chemin SIMD x86 **sans
dispatch runtime**, `emmintrin.h` en dur → casserait aarch64 et rendrait le binaire
dépendant du `-march` de la machine de build ; la sortie est **bit-à-bit identique**
entre `ref` et `opt`, ce n'est qu'un compromis perf/portabilité — mesure locale
m=19456 t=2 p=1 : ref 31 ms, opt SSE2 28 ms, opt AVX2 18 ms), `thread.c`
(parallélisme intra-hash inutile), et `run.c`/`bench.c`/`test.c`/`genkat.*` (CLI,
bench, KAT). `thread.h` **est** vendoré car `core.c` l'inclut inconditionnellement,
mais son corps entier est derrière `#if !defined(ARGON2_NO_THREADS)` → header vide.
Build via `-DARGON2_NO_THREADS` : supprime toute référence à pthread /
`_beginthreadex`, `p>1` reste **accepté et produit le même tag** (lanes traitées en
série), cohérent avec la VM mono-thread. `include/` seul est `PUBLIC` ; `src/` reste
`PRIVATE` car il contient `core.h`, `encoding.h` et surtout `thread.h`, noms
génériques qui masqueraient des headers système s'ils atteignaient le search-path
global d'`owl_core` (cf. le piège mpdecimal `<io.h>`/`<bits.h>`). Sources 100%
C89/C99 portable, zéro asm inline, zéro I/O. Validation : `build/argon2_smoketest.exe`
rejoue deux vecteurs officiels upstream (dont un à `p=2`, qui vérifie qu'
`ARGON2_NO_THREADS` ne change pas le tag), les paramètres OWASP m=19456/t=2/p=1 avec
`verify` + mismatch, et le rejet d'un encodage tronqué. Au bump : re-télécharger le
tarball release, réécraser `include/` et `src/` en ré-excluant `opt.c`,
`blamka-round-opt.h`, `thread.c` et les exécutables, relancer le smoke test.

**Note llhttp** : parseur HTTP/1.1 strict (RFC 9112), la machine à états de Node —
donc la surface HTTP la plus fuzzée du marché. Vendoré comme enabler du chantier
serveur HTTP, mais le gain est **acquis indépendamment** de ce chantier : le client
HTTP OWL (`src/stdlib_http.c`) parse aujourd'hui les réponses **à la main**, et llhttp
parse les réponses (`HTTP_RESPONSE`) aussi bien que les requêtes.

*Pourquoi le tag `release/*` et pas les releases GitHub.* llhttp est écrit en
**TypeScript** et se compile en machine à états C via `llparse` : la branche `main`
**ne contient aucun C**, et les releases GitHub **n'ont aucun asset attaché**. Le seul
endroit où le C généré est committé, c'est le tag `release/vX.Y.Z` — exactement ce que
`nodejs/node` vendore dans `deps/llhttp/src/`. Zéro toolchain Node requise. À noter :
`git archive release/v9.4.2 …` ne marche pas contre github.com (`upload-archive`
désactivé côté serveur) ; il faut `git clone --depth 1 --branch release/v9.4.2`.

*Sous-ensemble vendoré (6 fichiers).* `src/llhttp.c` (321 498 o, la machine à états),
`src/api.c` (API publique), `src/http.c` (`llhttp_message_needs_eof` /
`llhttp_should_keep_alive`), `include/llhttp.h`, plus `LICENSE` et `LICENSE-MIT`.
Écartés : le `CMakeLists.txt` amont (il ne propose que `SHARED` par défaut, on câble
notre propre cible), `llhttp.gyp` + `common.gypi` (build Node), `libllhttp.pc.in`
(pkg-config, inutile en statique), `README.md`. **Licence MIT lue verbatim** dans les
deux fichiers de licence (texte MIT intégral, copyright Fedor Indutny 2018) ; les `.c`
et le `.h` générés ne portent **aucun en-tête de licence par fichier**, c'est la
licence du dépôt qui couvre — même situation que chez node.

*Zéro dépendance, zéro piège de portabilité.* `llhttp.c` : aucun `malloc`, aucun I/O,
aucun `assert`, aucun appel système. Les intrinsèques SIMD (SSE4.2 / NEON /
wasm_simd128) sont **correctement gardées** par `#ifdef` sur les macros de la cible —
pas de `-march` imposé, pas de piège aarch64 (contrairement à l'`opt.c` d'argon2).
À savoir : `cmake/toolchain_zig_native.cmake` force `-mcpu=x86_64_v2`, qui **définit
`__SSE4_2__`**, donc le chemin SIMD de `llhttp.c` est bien actif dans les builds
`make mega` / `make release` (vérifié : 4 `pcmpistri`/`pcmpestri` émis avec
`-mcpu=x86_64_v2`, 0 sans). **Les deux chemins sont validés** par le smoke test, avec
et sans `-mcpu=x86_64_v2`, sur les deux cibles — résultats de parsing identiques.
`api.c` apporte `malloc`/`free` (via `llhttp_alloc`/`llhttp_free`, inutiles si le
parseur est un membre de struct), **4 `abort()`** en branche `default:` de `switch` sur
des enums internes (états théoriquement inatteignables — point de vigilance pour le
wrappage : llhttp peut tuer le process sur corruption d'état interne), et un
`llhttp__debug()` qui écrit sur `stderr` mais **n'est référencé nulle part** dans le C
généré, donc `--gc-sections` le supprime avec sa dépendance `fprintf`.

*⚠️ PATCH OWL — la seule divergence avec l'amont, à re-appliquer au bump.* Dans
`include/llhttp.h`, une garde `#ifndef LLHTTP_EXPORT` a été ajoutée autour du bloc qui
définit la macro. Raison : amont, sur `_WIN32`, `LLHTTP_EXPORT` vaut
`__declspec(dllexport)` **sans aucune garde de type `BUILDING_LLHTTP`**. En link
statique dans un `.exe`, ld/lld remonte alors les **34** fonctions publiques de llhttp
dans la table d'export PE du binaire final — dont les **11 `llhttp_set_lenient_*`**,
c'est-à-dire précisément la surface permettant de **désarmer les règles
anti-smuggling** depuis l'extérieur du binaire. `--gc-sections` n'y peut rien :
`dllexport` épingle les symboles. Mesuré sur le smoke test : **34 exports avant
correction, 0 après** (la section `.edata` disparaît entièrement du binaire). Le
comportement amont reste accessible en ne définissant pas la macro. Vérification :
```bash
objdump -p build/llhttp_smoketest.exe | grep -i "export table"   # doit être vide
objdump -p build/owl.exe             | grep -c llhttp            # doit valoir 0
```

*Build.* `-DLLHTTP_EXPORT=` en **PUBLIC** — exception assumée au pattern `PRIVATE` de
zint / argon2 / mpdecimal, pour une raison structurelle : `LLHTTP_EXPORT` n'est pas une
option de compilation de la lib, c'est une macro du **header public** posée sur
**chaque déclaration**. Si le define n'atteint pas les consommateurs, leurs
déclarations re-exportent ce que la lib n'exporte plus. `-DNDEBUG` en `PRIVATE`
(llhttp n'a aucun `assert`, mais on aligne le contrat Release). `include/` seul est
`PUBLIC` et ne contient que `llhttp.h`, nom non ambigu : **aucun `-I` privé sur `src/`
n'est nécessaire** (les 3 `.c` n'ont aucun header privé), donc pas de risque de
masquage de header système (cf. le piège mpdecimal `<io.h>`/`<bits.h>`). Hardening
standard par ailleurs : `-fvisibility=hidden`, `-ffile-prefix-map`,
`-ffunction-sections`/`-fdata-sections` pour que `--gc-sections` évacue le non-consommé.

*🔒 AUCUN FLAG `LENIENT_*` N'EST ACTIVÉ — c'est un choix de sécurité, pas un oubli.*
llhttp est strict par défaut ; les **11** `llhttp_set_lenient_*()` existent précisément
pour **relâcher** ce défaut, et chacun rouvre une classe de request smuggling. Le
durcissement du défaut *est* la raison du choix de llhttp (les CVE-2023-30589 « CR seul
accepté comme délimiteur » et CVE-2024-27982 « espace avant `Content-Length` » ont été
fermées en durcissant le défaut, le laxisme relégué derrière un flag nommé). Toute
activation future exige un ticket justifié par un client réel cassé. À savoir :
`LENIENT_HEADERS` est le plus dangereux du lot — il rouvre à lui seul la CVE-2024-27982
(l'espace avant les deux-points n'est **pas** durci en dur, contrairement à ce qui avait
été écrit dans `docs/research/serveur_http_2026-07.md` §6.2, corrigé depuis). La seule
règle réellement sans échappatoire est la famille `Content-Length` malformé (dupliqué,
vide, non numérique, en overflow).

*Validation.* `build/llhttp_smoketest.exe` asserte `lenient_flags == 0` (après `init`
**et** après `reset`, qui préserve volontairement les flags), parse en nominal une
requête et une réponse (`Content-Length`, `chunked` réassemblé, corps délimité par EOF
via `llhttp_finish`, `204` sans corps), puis rejoue **17 vecteurs de rejet** repris du
corpus de tests amont (les `.md` de `test/request` et `test/response`, MIT, non
vendorés car absents du tag `release/*` — ils donnent les octets bruts *et* le code
d'erreur attendu). Cross-compile prouvée à l'exécution, pas seulement au link :
```bash
./vendor/zig/x86_64-windows/zig.exe cc -target x86_64-windows-gnu \
  -std=c99 -O2 -DNDEBUG -DLLHTTP_EXPORT= -fvisibility=hidden \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Ivendor/llhttp/include vendor/llhttp/src/{llhttp,api,http}.c \
  src/internal_tests/llhttp_smoketest.c -o smoke_win.exe
./vendor/zig/x86_64-windows/zig.exe cc -target x86_64-linux-musl -static \
  ... (mêmes flags) ... -o smoke_linux
```
Au bump : re-cloner le tag `release/*`, réécraser `include/` + `src/`,
**re-appliquer la garde `#ifndef LLHTTP_EXPORT`**, revérifier les deux fichiers de
licence, relancer le smoke test et le contrôle `objdump` des exports.

**Note llama.cpp** : moteur d'inférence LLM local par fichiers GGUF (inférence
seule, CPU-only), enabler de la bataille 2. Licence **MIT** (`LICENSE` vendoré
verbatim, copyright Georgi Gerganov & contributeurs). Vendoré en **sous-ensemble
copié** (pas submodule) : le repo amont drague ses propres sous-modules imbriqués,
18 Mo de backends GPU (cuda/vulkan/metal/sycl/...) qu'on ne compile jamais, et
12 Mo de glue CLI (`common/`+examples+tools, avec tentation curl/OpenSSL). Le gel
d'un sous-ensemble curé = reproductibilité (l'API interne amont casse à chaque
release, on assume le coût de bump) et cohérence avec la discipline OWL
(zint backend-only, argon2 ref-only, mpdecimal libmpdec-only).

*Sous-ensemble vendoré.* `CMakeLists.txt` racine + `LICENSE` + `cmake/` (build-info,
common, license, git-vars, templates de config) + `include/` (`llama.h`,
`llama-cpp.h`) + `src/` (les `llama-*.cpp/.h`, `llama.cpp`, `unicode*`, `models/`)
+ `ggml/` **CPU-only**. Élagués dans `ggml/src/` : les **17 backends
GPU/accélérateur** (`ggml-blas/cann/cuda/et/hip/metal/musa/opencl/rpc/sycl/
vulkan/webgpu/zdnn/zendnn/openvino/virtgpu/hexagon`). Sûr car chaque backend est
gaté dans `ggml/src/CMakeLists.txt` par `ggml_add_backend(<NAME>)` → `if(GGML_<NAME>)`
(OFF par défaut → aucun `add_subdirectory`). Seul `ggml-cpu/` est conservé. Élagués
au niveau racine : `common/`, `examples/`, `tools/`, `tests/`, `app/`, `pocs/`,
`vendor/` amont, `docs/`, les scripts Python. Résultat : ~9,6 Mo (vs ~40 Mo repo
complet).

*Build.* Script `scripts/build_llama_zig_native.sh` (calqué sur
`build_duckdb_zig_native.sh`) — même toolchain zig clang+libc++ qu'owl.exe et
DuckDB → **ABI libc++ cohérente** (l'API publique `llama.h` est `extern "C"`, le
C++ reste interne aux `.a`). Sortie dans `_local/llama/<host>/install/` (gitignore).
Cross Win+Linux prouvée dans la session de dé-risquage (smoke exécuté sur les deux
cibles) ; profil musl-static via `OWL_MUSL=1` comme DuckDB. **Flags** :
`BUILD_SHARED_LIBS=OFF` (pas de `dllexport`), `GGML_BACKEND_DL=OFF` (backends liés
en statique, registration CPU = appel direct sous `#ifdef GGML_USE_CPU`),
`LLAMA_BUILD_COMMON/APP/SERVER/EXAMPLES/TESTS/TOOLS=OFF` +
`LLAMA_CURL=OFF LLAMA_OPENSSL=OFF GGML_CURL=OFF GGML_OPENMP=OFF` (zéro dépendance
externe).

*⚠️ ISA portable-first — baseline x86_64_v2 (SSE4.2) STRICTE.* `GGML_NATIVE=OFF`
**ne suffit pas** : ggml active quand même AVX2/FMA/F16C/BMI2/AVX par défaut sur
x86, ce qui ferait **SIGILL sur tout CPU sans AVX2** (Docker/cloud baseline). On
force explicitement `GGML_AVX/AVX2/FMA/F16C/BMI2/AVX_VNNI/AVX512*=OFF` +
`GGML_SSE42=ON`. Le variant CPU se réduit alors à `-msse4.2 GGML_SSE42` (vérifié
dans le log de config), aligné sur le `-mcpu=x86_64_v2` que la toolchain OWL force
déjà. Le multi-variant AVX2 par dispatch runtime exigerait `GGML_BACKEND_DL` (libs
dynamiques, interdit en statique) — optimisation séparée ultérieure, hors scope.

*🔒 Piège #8166 — enregistrement du backend CPU au runtime.* La registration du
backend CPU (`register_backend(ggml_backend_cpu_reg())` dans `ggml-backend-reg.cpp`,
gaté `#ifdef GGML_USE_CPU`) peut être **droppée par le linker**, et notre hardening
`--gc-sections` **aggrave** le drop → `ggml_backend_dev_count()==0` au RUNTIME
alors que le LINK a réussi. Neutralisé par `-Wl,--whole-archive` sur
**`libggml-cpu.a` uniquement** (`--no-whole-archive` refermé juste après pour ne
pas gonfler llama/ggml-base, tirés par références) dans la cible `owl_llama` de
`CMakeLists.txt`. **Vérifié à l'EXÉCUTION, pas au link** :
`src/internal_tests/llamacpp_smoketest.c` asserte `ggml_backend_dev_count() >= 1`
+ device CPU présent (câblé dans la batterie, `reg llama-smoke`).

*Exports propres par construction.* Contrairement à llhttp (`dllexport` sans garde)
et DuckDB (462 exports), en build statique `LLAMA_API`/`GGML_API` sont **vides** sur
Windows (le `__declspec(dllexport)` est gaté sur `LLAMA_SHARED`/`GGML_SHARED`, non
définis quand `BUILD_SHARED_LIBS=OFF`). Aucun patch nécessaire.
`objdump -p build/owl.exe` : **0 table d'export, 0 symbole ggml/llama/gguf**.

*Intégration owl.exe.* Cible `owl_llama` (INTERFACE) liée à owl.exe avec le define
`OWL_HAS_LLAMA`. **Aucune native OWL consommatrice pour l'instant** (la surface
`IAModèle.Charge`/`.Génère` est le sous-chantier B) : sans référence, llama/ggml-base
sont gc'és, seul ggml-cpu (whole-archive) + ce qu'il atteint survit. Delta owl.exe
mesuré = **+1,14 Mo** (102,49 → 103,62 Mo). La surface d'inférence pleine
(`llamacpp_smoketest.exe` = 4,53 Mo) donne le delta attendu **post-chantier B**
(~4 Mo, cohérent avec l'estimation d'audit), très loin du plafond 150-200 Mo.

*Hardening.* `-fvisibility=hidden`, `-ffile-prefix-map`, `-ffunction-sections`
`-fdata-sections`, `-DNDEBUG` appliqués dès la compilation des `.a` (dans le
build script). C++17 avec RTTI/exceptions (requis par llama.cpp).

*Au bump.* Re-cloner le nouveau tag (`git clone --depth 1 --branch bXXXXX`),
réécraser le sous-ensemble (racine `CMakeLists.txt`+`LICENSE`+`cmake/`+`include/`
+`src/`+`ggml/`) en ré-élaguant les 17 backends GPU dans `ggml/src/`, mettre à
jour `LLAMA_BUILD_NUMBER`/`LLAMA_BUILD_COMMIT` dans le build script,
`rm -rf _local/llama && bash scripts/build_llama_zig_native.sh`, rebuild owl,
relancer `llamacpp_smoketest` (avec ET sans GGUF), revérifier `objdump` exports.
L'API interne amont bouge souvent : prévoir des ajustements du smoke test.

**Note libxls** : lecteur du format Excel 97-2003 hérité (`.xls` binaire, BIFF8
dans un conteneur OLE2), enabler de la lecture `.xls` du modèle `xlsDocument`
(`src/stdlib_xls.c`, qui sait déjà lire le `.xlsx` ZIP+XML). Licence
**BSD-2-Clause** (`LICENSE` vendoré verbatim). Vendoré en **sous-ensemble
copié** (pas submodule : l'amont est autotools, on compile les `.c`
directement comme zint/argon2). Maintenance active (dernier commit amont à 5
jours du pin ; historique CVE **concentré sur ≤ 1.4**, la branche 1.5+ a été
« heavily fuzz-tested with clang's libFuzzer » et corrige la majorité des CVE
mémoire — c'est la raison de préférer libxls à un recodage BIFF maison).

*Sous-ensemble vendoré (5 `.c` + `config.h` + 9 headers).* `src/{endian,locale,
ole,xls,xlstool}.c`, un `src/config.h` **écrit à la main** (remplace le config.h
autotools amont), `include/xls.h` + `include/libxls/*.h` (8 headers), `LICENSE`,
et `testdata/{mtcars,vietnamese-utf8}.xls` (2 fixtures du corpus amont pour le
smoke test). **Écarté** à la copie : `src/xls2csv.c` (frontend CLI, hors
bibliothèque), `test/`, `fuzz/`, `configure.ac`/`Makefile.am`/`bootstrap`
(build autotools amont non utilisé).

*⚠️ LE POINT DUR — iconv, tranché NON.* libxls peut lier `libiconv` pour
convertir les charsets BIFF. libiconv n'est **pas vendorable proprement**
(cascade de dépendance externe → refus). Mais **iconv est OPTIONNEL** :
`config.h` ne définit **PAS** `HAVE_ICONV`, et le chemin sans-iconv de
`xlstool.c` convertit déjà proprement vers UTF-8 —
- chaînes compressées BIFF8 (latin1) → `transcode_latin1_to_utf8()` (inline) ;
- chaînes UTF-16LE → `unicode_decode_wcstombs()` avec une **locale UTF-8 forcée
  par plateforme** (`locale.c` : `_create_locale(LC_CTYPE, ".65001")` sur
  Windows = codepage UTF-8, `newlocale(LC_CTYPE_MASK, "C.UTF-8", NULL)` sur
  Linux, `"UTF-8"` sur macOS). **Ce n'est donc PAS la locale système** qui
  décide de l'encodage de sortie → conversion déterministe, zéro dépendance.
**Validé au RUNTIME** (`libxls_smoketest`) : cellule UTF-16 vietnamienne à
diacritiques (`Mô tả`, codepage 1200) → octets UTF-8 **exacts**
(`4d c3 b4 20 74 e1 ba a3`). Seule dégradation connue et **assumée** : les
chaînes BIFF5 (Excel 5/95) à codepage exotique tombent en copie brute sans
iconv (`codepage_decode` branche `#else`) ; BIFF8 (Excel 97-2003), la vraie
cible du « legacy .xls », est intégralement couvert.

*Toolchain / portabilité.* Zéro asm inline, uniquement des headers standard.
Compile prouvée avec **zig clang** en cross sur les deux cibles (`.a` = ~287 Ko
Windows, ~320 Ko Linux musl) ; `nm` : **aucun symbole iconv**, seule la libc en
externe. `_GNU_SOURCE` requis côté Linux/musl (`newlocale`/`uselocale`/`strdup`,
posés par `AC_USE_SYSTEM_EXTENSIONS` amont) → défini en **PRIVATE** dans la
cible CMake `owl_libxls` (bénin sur mingw/UCRT). `config.h` inclus
inconditionnellement par chaque `.c` → il **doit** rester présent.

*Build / hardening.* Cible `owl_libxls` (STATIC) dans `CMakeLists.txt`, calquée
zint/argon2 : `NDEBUG` + `-fvisibility=hidden` + `-ffile-prefix-map` +
`-ffunction-sections`/`-fdata-sections` (le `--gc-sections` hérité au link owl
vire tout tant qu'aucune native ne tape l'API). `xls.h` n'émet aucun
`__declspec(dllexport)` (API `extern` simple) → pas de fuite d'export PE, aucun
patch nécessaire (contrairement à llhttp). `include/` PUBLIC (nom `xls.h` non
ambigu ; les headers internes sont préfixés `libxls/` → pas de masquage de
`<endian.h>`/`<locale.h>` système). Liée à `owl_vendor` INTERFACE.

*⚠️ Périmètre.* Le **câblage OWL** (`xls_sniff_format` voie OLE2 →
`xls_read_file` → peuplement `XlsSheet`/`XlsCell` via l'API
`xls_open_file`/`xls_getWorkSheet`/`xls_parseWorkSheet`) N'EST PAS fait ici :
c'est un lot feature séparé. À l'ouverture, `fn_xls_ouvre` lève encore
« format .xls non supporté en lecture ».

*Au bump.* Re-cloner le repo (ou DL le tarball release), réécraser
`src/{endian,locale,ole,xls,xlstool}.c` + `include/` en ré-excluant
`xls2csv.c`, **conserver `src/config.h` écrit à la main** (ne PAS reprendre le
config.h autotools généré), revérifier que `HAVE_ICONV` reste non défini,
relancer `libxls_smoketest` (mtcars + vietnamese UTF-8).

**Note libssh2** : client SSH-2 avec support SFTP (`libssh2_sftp_*`), enabler du
**Lot D** du chantier FTP OWL. Le SFTP est du **SSH-2 file transfer**, PAS du
FTP-sur-TLS — protocole entierement different, d'ou une lib dediee (le FTPS/FTPES
du Lot C reste sur nos portes TLS mbedtls). Licence **BSD-3-Clause** (`COPYING`
vendore verbatim). Maintenance de premier ordre : mainteneur **Daniel Stenberg**
(l'auteur de curl) + Viktor Szakats + Will Cosgrove, master actif au jour du pin
(commits quotidiens). Vendore en **sous-ensemble copie** (pas submodule : on gele
un commit precis, cf. ci-dessous).

*⚠️ PIN SUR COMMIT MASTER, PAS SUR TAG — c'est une decision de securite, pas un
oubli.* Le dernier tag release (**1.11.1**, oct-2024) est vulnerable a **trois CVE**
non backportees en release taggee :
- **CVE-2026-55200** (CVSS ~9.x, **critique**) : ecriture hors-bornes dans
  `ssh2_transport_read()`, declenchee **PRE-AUTH** par un serveur SSH **malveillant**
  via un `packet_length` forge → RCE cote **client**. C'est exactement le modele de
  menace d'OWL en client SFTP (on se connecte a des serveurs tiers). Fix `97acf3df`.
- **CVE-2026-55199** (haute) : DoS (boucle CPU) via un compte d'extension bidon dans
  le handler `EXT_INFO`. Fix `17626857`.
- **CVE-2025-15661** (haute) : heap over-read dans `sftp_symlink` sur paquet malforme
  — directement sur le chemin SFTP qu'on veut utiliser. Fix `2dae3024`.

Les trois fixes sont **ancetres du commit epingle `cdbda3cd`** (verifie par
`git merge-base --is-ancestor`), et **absents de 1.11.1** (verifie). Le pin EST le
mecanisme de correction : **aucun patch OWL local** (contrairement a llhttp).
**Au bump** vers le futur tag **1.11.2** (attendu, bundlera ces fixes) : verifier via
`git merge-base --is-ancestor <commit> <tag>` que les trois commits ci-dessus y sont
ancetres ; sinon **rester sur un commit master** plus recent. Ne JAMAIS retomber sur
un tag <= 1.11.1.

*Backend crypto = mbedTLS (deja vendoree) → ZERO nouvelle dependance crypto.* Le
backend (`src/mbedtls.c`, selectionne par `-DLIBSSH2_MBEDTLS`) n'exige **aucune**
feature mbedtls absente d'`owl_mbedtls_config.h` : `MBEDTLS_ECDH_C` / `ECDSA_C` /
`ECP_DP_SECP256R1|384R1|521R1_ENABLED` / `CTR_DRBG_C` sont tous presents (sinon
`mbedtls.h` leve un `#error`). Le Diffie-Hellman group est implemente en **bignum
brut** (`mbedtls_mpi`), PAS via `mbedtls_dhm` → `MBEDTLS_DHM_C` **inutile**. Config
libssh2 **secure-by-default** (`crypto_config.h`) : MD5, DSA, RSA-SHA1, blowfish, RC4,
3DES sont **desactives** sauf activation explicite.

*Sous-ensemble vendore (27 `.c` + 19 `.h` internes + 3 headers publics).* Les
`CSOURCES` amont **moins** les 3 backends crypto non retenus (`openssl.c`,
`libgcrypt.c`, `os400qc3.c`, `wincng.c`) : seul `mbedtls.c` est compile. `blowfish.c`
est present car **`#include` par `bcrypt_pbkdf.c`** (dechiffrement de cle privee
OpenSSH), pas compile separement. Ecartes aussi : les 4 headers de backend
(`openssl.h`/`libgcrypt.h`/`os400qc3.h`/`wincng.h`), jamais atteints (`crypto.h` est
une chaine `if/elif` ou seul `mbedtls.h` est pris sous `LIBSSH2_MBEDTLS`). `include/`
= 3 headers publics (`libssh2.h`, `libssh2_sftp.h`, `libssh2_publickey.h`), tous
prefixes `libssh2_` → PUBLIC sans risque de masquage ; `src/` PRIVATE (headers a
noms generiques `session.h`/`packet.h`/`misc.h`... qui masqueraient des headers
systeme sur le search-path global, cf. piege mpdecimal).

*Config / portabilite.* Zero asm inline. Compile prouvee en **cross zig clang** sur
les DEUX cibles (compile-only linux-musl + windows-gnu sur les 27 `.c`, zero erreur)
et **link statique + execution** prouve sur windows-gnu (`libssh2_init` →
`session_init` → backend mbedTLS → `version=1.11.2_DEV`). Sur **Windows/mingw**,
`libssh2_setup.h` hand-crafte la config (aucun `HAVE_*` requis : le dev UCRT64 comme
le zig windows-gnu compilent nus). Sur **Linux/macOS**, `setup.h` ne pose rien hors
`HAVE_CONFIG_H` → les detections POSIX (`HAVE_UNISTD_H`, `HAVE_SYS_SELECT_H`,
`HAVE_POLL`...) sont fournies en **PRIVATE** dans la cible CMake `owl_libssh2`
(`if(NOT WIN32)`).

*Build / hardening.* Cible `owl_libssh2` (STATIC) dans `CMakeLists.txt`, calquee
zint/argon2/libxls : `NDEBUG` + `-fvisibility=hidden` + `-ffile-prefix-map` +
`-ffunction-sections`/`-fdata-sections` (le `--gc-sections` herite au link owl vire
tout tant qu'aucune native ne tape l'API). **Aucun patch anti-dllexport** : `libssh2.h`
n'emet `__declspec(dllexport)` que sous `(_WIN32 && LIBSSH2_EXPORTS)`, et
`LIBSSH2_EXPORTS` n'est jamais defini en statique → `LIBSSH2_API` est vide, les
`libssh2_*` **n'entrent pas dans la table d'export PE** (verifie : 0 export sur le
smoke test, contrairement a llhttp). Lie a `owl_vendor` INTERFACE mais **AUCUNE
native ne le reference encore** → gc'e du binaire, **delta owl.exe ~0** (comme
libxls/llhttp tant que non consommes). Le delta reel (~la part SSH+SFTP referencee)
sera mesure au **lot feature** de wrappage.

*⚠️ Perimetre.* Le **cablage OWL** (natives `FTP*` routees sur `libssh2_sftp_*` quand
le nom de serveur porte le prefixe `sftp://`, propriete `..VerifieHote` sur la
verification de cle d'hote via l'API known-hosts) **N'EST PAS fait ici** : c'est un
lot feature separe, ecrit avec le dev apres cet enabler. Validation actuelle :
`build/libssh2_smoketest.exe` (init + session + version >= 1.11.1 + backend mbedTLS
enregistre des algos KEX/HOSTKEY + known-hosts amorce).

*Au bump.* Re-cloner le repo, re-verifier via `git merge-base` que les trois fixes CVE
sont ancetres du nouveau commit/tag (JAMAIS <= 1.11.1), reecraser `include/` + `src/`
en re-excluant les 3 backends non-mbedtls + leurs 4 headers (garder `blowfish.c`),
mettre a jour la ligne du tableau + ce PIN, relancer `libssh2_smoketest`, revisiter
`objdump` exports.

## Process de bump — checklist générale

1. **Identifier le bump** : `make check-updates` ou veille manuelle.
2. **Bumper** : submodule (cf qpdf above) ou re-tarball (cf source copiée).
3. **Mettre à jour CETTE ligne** dans `vendor/VERSIONS.md` (version + date).
4. **Build + test complet** :
   ```bash
   make clean && make qt6 && make && make test && make mega
   ```
5. **Smoke test fonctionnel** ciblé sur la lib bumpée :
   - qpdf : `.owl` avec `iFinImprime` + `iParametrePDF("password")` → PDF chiffré
   - Qt6 : lance un example UI (`./build/owl examples/login.owl`)
   - zig : `make mega` produit un binaire qui tourne
   - mbedtls/zlib/pcre2 : suite `make test` couvre déjà
6. **Commit** atomic au format :
   ```
   build(vendor): bump <nom> <ancienne> → <nouvelle>
   
   <changelog upstream pertinent en 2-3 lignes>
   <impact OWL : breaking changes ? rien ? doc à mettre à jour ?>
   ```
7. Si breaking change upstream : créer une PR séparée pour les fix code OWL,
   ne pas mélanger avec le bump.

## Politique de bump

- **Patches/minors** : libres, à bumper dès qu'on a une raison (CVE, bug fix
  qui nous touche, perf measurable). Pas de fréquence forcée.
- **Majors** : étudier la migration, lire le changelog complet, prévoir
  les fix nécessaires côté OWL. Un major par session, pas en batch.
- **CVE critique** : prioritaire, on bump dans la semaine.
- **Pas de bump "préventif"** : si ça marche, on ne touche pas. Reproductibilité
  et stabilité priment sur "latest".
