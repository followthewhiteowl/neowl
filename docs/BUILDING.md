# Building OWL

OWL produit deux artifacts distincts selon le rôle :

| Artifact | Pour qui | Comment construire |
|---|---|---|
| `owl[.exe]` + `owl-lsp[.exe]` | Contributeurs OWL + utilisateurs qui éditent du `.owl` | `make` |
| `owl-build[.exe]` | Utilisateurs finaux qui packagent leur projet OWL | `make mega` |

Les utilisateurs finaux téléchargent uniquement le **mega** single-file.
Les contributeurs (ce repo) rebuild le runtime depuis les sources.

---

## Prérequis

- **CMake 3.21+** et **Ninja**
- **GCC 13+** (bootstrap CMake) — le runtime est ensuite recompilé sous zig
  cc pour le mega
- **Qt6 source vendoré** via submodule `vendor/qt6/qtbase/` (pinned v6.10.0)

### Windows — MSYS2 UCRT64

[**MSYS2**](https://www.msys2.org/) installé dans `C:\msys64`, puis :

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja
```

Ajouter `C:\msys64\ucrt64\bin` au `PATH`.

C'est tout ce dont MSYS2 a besoin. **Pas de `qt6-static`, `qt6-base`, ni
autres libs graphiques** — Qt6 et ses dépendances (pcre2, freetype,
harfbuzz, libpng, zlib, etc.) sont vendorées dans `vendor/` et buildées
localement par `make qt6`. Le rôle de MSYS2 UCRT64 se limite à fournir
gcc / cmake / ninja / mingw32-make.

### Linux

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev
# Ou : bash scripts/setup_linux.sh (toutes les deps + Qt6)
```

---

## Build

> **Windows (MSYS2 UCRT64) :** la commande est **`mingw32-make`**, pas `make`
> — MSYS2 ne fournit pas d'alias `make`. Les blocs ci-dessous l'écrivent `make`
> pour Linux/macOS ; sous Windows, lire `mingw32-make` (`mingw32-make setup`,
> `mingw32-make mega`, …).

```bash
git clone --recursive <url-du-dépôt>    # IMPORTANT : --recursive (submodules Qt6/DuckDB/zlib/…)
# (si déjà cloné sans --recursive : git submodule update --init --recursive)
make setup                               # one-time, ~1h30 : zig + Qt6 gcc + Qt6 zig native (idempotent)
bash scripts/build_llama_zig_native.sh   # one-time, ~3 min : moteur d'inférence local (REQUIS par cmake)
make                                     # incrémental ~2s, full ~25s
make mega                                # owl-build single-file cross-compile (~5 min)
make test                                # exécute la suite de tests
```

> **Submodules + llama obligatoires.** L'arbre doit être un clone git **avec
> submodules** (un simple tarball/zip ne suffit pas : `make setup` fait
> `git submodule update`). Et `scripts/build_llama_zig_native.sh` doit tourner
> **avant `make`** : la configuration cmake échoue sinon (le moteur d'inférence
> est lié en dur, pas d'option off).

Produits dans `build/` :

| Fichier | Taille indicative | Rôle |
|---|---|---|
| `owl[.exe]` | ~63 MB | All-in-one : REPL + run + test + LSP client + GUI Qt6 + PDF qpdf |
| `owl-lsp[.exe]` | ~900 KB | Language Server (lancé par VS Code automatiquement) |
| `owl-build[.exe]` | ~550 MB | Mega single-file cross-compile |

`make setup` enchaîne les 4 étapes one-time (idempotent). Tu peux les
invoquer séparément : `git submodule update --init --recursive`,
`bash scripts/setup_zig.sh`, `make qt6`,
`bash scripts/build_qt6_zig_native.sh`.

Option dev : `cmake -DOWL_DEV_TOOLS=ON` active les binaires bench/test
internes (`owl-cli`, `owl-lsp-test`, `owl-lsp-bench`). Off par défaut.

---

## Mega single-file pour utilisateurs finaux

`make mega` (alias `bash scripts/build_mega.sh`) produit
`build/owl-build[.exe]`. Premier run du mega chez l'utilisateur final :
extraction du payload dans `%TEMP%/owl-build-<hash>/` (~5-10s sur SSD),
runs suivants depuis le cache.

`owl-build` embarque sa propre toolchain : **aucun compilateur externe**
(ni gcc, ni autre) n'est requis sur la machine qui package. Il produit un
binaire autonome, sans dépendance hôte.

Un binaire `owl-build` cible les plateformes dont il **embarque** les
bibliothèques (Qt6 + libs OWL). Le mega assemble toutes les cibles disponibles
dans l'arbre de build :

- `make mega` seul → embarque la plateforme hôte (ex. `windows-x86_64`).
- `make release-linux` (musl, via WSL) **avant** `make mega` → le mega embarque
  **en plus** `linux-x86_64`. Un même `owl-build` Windows produit alors aussi
  bien des binaires Windows que Linux (cross-compile, cf. ci-dessous).

Recette cross-plateforme complète depuis Windows :

```bash
make release-linux   # one-time/maj : libs musl Linux (WSL)
make mega            # assemble windows-x86_64 + linux-x86_64 dans owl-build
```

---

## Choix de la cible

```bash
./owl-build mon_projet/                           # → exe/<hôte>/mon_projet[.exe]
./owl-build --target windows-x86_64 mon_projet/   # → exe/windows-x86_64/mon_projet.exe
./owl-build --target linux-x86_64 mon_projet/     # → exe/linux-x86_64/mon_projet (ELF statique)
```

`--target TRIPLE` choisit la plateforme du binaire produit (défaut : l'hôte).
Les cibles disponibles sont **celles embarquées dans ce `owl-build`**. Le mega
multi-cibles (cf. recette ci-dessus) embarque `windows-x86_64` **et**
`linux-x86_64` : un même `owl-build` Windows produit donc des binaires Linux
statiques autonomes (et inversement). Un mega bâti sans `make release-linux`
n'embarque que l'hôte.

macOS (`aarch64-macos`, `x86_64-macos`) est câblé dans le code mais non
validé — en attente de hardware Mac.

---

## Troubleshooting

**`make setup` ou Qt6 zig native échoue sur WSL2** — le `PATH` contient
`/mnt/c/msys64/ucrt64/bin`, Qt configure pulls les headers Windows dans
le build Linux. Le script `build_qt6_zig_native.sh` nettoie `PATH` sur
hosts Linux, mais une invocation manuelle peut casser.

**`owl-build` extrait puis rien** — premier run extrait ~700 MB. SSD =
~5-10s, HDD plus lent. Runs suivants hit le cache.

**`cannot find -lm`** — pas dans le shell MSYS2 UCRT64. Utiliser le bon
shell ou ajouter `C:\msys64\ucrt64\bin` au `PATH`.

**`owl-lsp.exe` locké au link** — VS Code utilise le binaire. Fermer VS
Code ou `Stop-Process -Name owl-lsp -Force` avant `make`.

**Tests fail** — `./build/owl test -v` pour le détail. Si compile passe
mais tests fail, probablement plateforme (line endings, locale).

---

## Intégration éditeur

L'extension VS Code est fournie en asset de release. Le LSP
(`owl-lsp[.exe]`) est auto-lancé par l'extension s'il est sur le `PATH`.
