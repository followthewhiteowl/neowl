#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Low-level compiler / formatter tests.
#
# Categories:
#   fmt-stability/  — input `.owl` + expected `.expected.owl` (after `owl fmt`)
#   catalog/        — snapshot-based tests of `owl --vscode-data` / --dump-lsp-catalog
#   signature/      — `.owl` input + `.expected.json` from `owl --check` on signature issues
#
# Usage: scripts/run_compiler_tests.sh [path/to/owl.exe]

set -u

OWL_EXE="${1:-./owl.exe}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$DIR/tests/compiler"

if [[ ! -x "$OWL_EXE" ]]; then
    echo "ERROR: owl exe not found: $OWL_EXE" >&2
    exit 2
fi

# Convert OWL_EXE to absolute path for subshells with changed cwd
OWL_EXE="$(cd "$(dirname "$OWL_EXE")" && pwd)/$(basename "$OWL_EXE")"

# OWL_GUI_EXE = pendant avec UI natives (Ferme/Ouvre…). Optionnel : si
# absent, les tests catalog-gui sont skippés. Auto-detect dans le meme
# repertoire que OWL_EXE.
OWL_GUI_EXE="${OWL_GUI_EXE:-${OWL_EXE%/*}/owl.exe}"
if [[ -x "$OWL_GUI_EXE" ]]; then
    # Augment PATH so owl can find Qt6 DLLs from MSYS2.
    if [[ -d "/c/msys64/ucrt64/bin" ]]; then
        export PATH="/c/msys64/ucrt64/bin:$PATH"
    fi
fi
if [[ ! -d "$ROOT" ]]; then
    #  : corpus absent = ECHEC, plus un `exit 0` vert. Ce harnais est
    # branche sur `make all` ; s'il ne trouve pas tests/compiler/, c'est qu'on
    # l'execute sans corpus, pas qu'il n'y a rien a verifier.
    echo "ERROR: CORPUS INTROUVABLE — $ROOT absent. 0 cas examine." >&2
    echo "Un controle qui n'examine rien ne prouve rien." >&2
    exit 1
fi

#  : base de tmpdirs DANS build/ au lieu de /tmp (= AppData/Local/Temp
# sous MSYS2 Windows, scanne real-time par Windows Defender, ~20x slowdown
# I/O). build/ est interne au repo et exclu/peu scanne. Cleanup garanti par
# trap EXIT meme en cas d'erreur ou Ctrl-C.
#
#  : nom commencant par un point. Tous les walkers du depot sautent les
# entrees pointees, donc cette base est invisible pour une campagne `owl test`
# concurrente. Sans ca, elle enumerait les fixtures copiees ici comme
# compagnons et echouerait quand ce harnais les supprime en route — le trap
# ci-dessous nettoie bien, mais la fenetre de course est OUVERTE tant que le
# harnais tourne. La convention s'ajoute au nettoyage, elle ne le remplace pas.
OWL_TMP_BASE="$DIR/build/.compiler_tests_$$"
mkdir -p "$OWL_TMP_BASE"
trap 'rm -rf "$OWL_TMP_BASE"' EXIT

# Wrapper timeout : un cas qui bloque (auto-open GUI, boucle infinie) est tue
# et compte comme echec (exit 124), jamais un hang infini du harness ni un
# faux pass. Sans `timeout` dispo (rare), on execute nu (pas de garde-fou).
OWL_RUN_TIMEOUT="${OWL_RUN_TIMEOUT:-15}"
if command -v timeout >/dev/null 2>&1; then
    RUN_TIMEOUT=(timeout "${OWL_RUN_TIMEOUT}")
else
    RUN_TIMEOUT=()
fi

pass=0
fail=0
failed=()

# ---------------------------------------------------------------------------
# Comptabilite de non-vacuite (, patron pose par ).
#
# Ce harnais disait `SKIP` dans deux situations opposees : une fixture qui
# releve d'une AUTRE section (legitime, la boucle balaie tous les case_*) et
# une fixture amputee de son entree ou de son snapshot (defaut). Les deux se
# ressemblaient, sur stderr, sans effet sur le verdict — donc invisibles.
#
# Trois mecanismes remplacent ce SKIP fourre-tout :
#   1. `miss` — un ecart de corpus est un ECHEC nomme, jamais un silence.
#   2. `sect_begin`/`sect_end` — chaque section affiche le nombre de cas
#      qu'elle a reellement executes et ECHOUE si ce nombre est nul. Le
#      chiffre visible attrape la chute anormale, la garde n'attrape que zero.
#   3. `covered` — les sections qui se partagent la famille check-project
#      declarent les cas qu'elles prennent en charge ; en fin de course, tout
#      case_* que PERSONNE n'a examine est un echec. C'est la regle rendue
#      totale, donc testable par la negative : une fixture ne peut plus
#      exister sans etre examinee par exactement une section.
# ---------------------------------------------------------------------------
declare -A COVERED=()
SECT_NAME=""
SECT_START=0
SECT_REPORT=()

sect_begin() { SECT_NAME="$1"; SECT_START=$((pass + fail)); }

sect_end() {
    local n=$((pass + fail - SECT_START))
    SECT_REPORT+=("$(printf '  %-38s %4d cas' "$SECT_NAME" "$n")")
    if [[ $n -eq 0 ]]; then
        fail=$((fail + 1))
        failed+=("$SECT_NAME (SECTION VIDE)")
        echo "SECTION VIDE $SECT_NAME : 0 cas execute — le corpus de cette" >&2
        echo "  section a disparu ou n'est plus reconnu. Un controle qui" >&2
        echo "  n'examine rien ne prouve rien." >&2
    fi
}

# Section volontairement non executee (binaire optionnel absent) : visible au
# recap, jamais confondue avec un vert.
sect_skip() { SECT_REPORT+=("$(printf '  %-38s    - %s' "$1" "$2")"); }

miss() {
    fail=$((fail + 1))
    failed+=("$1")
    echo "MANQUE $1 : $2" >&2
}

cover() { COVERED["$1"]=1; }

# ---------------------------------------------------------------------------
# fmt-stability: PORTE en self-test C in-process.
# Voir `owl --self-test-fmt-stability` (src/internal_tests/fmt_stability_test.c),
# cable dans la cible Makefile `all`. La boucle bash spawnait owl.exe + mktemp
# + cp + rm par fixture (~15s) ; l'in-process partage un seul boot (< 1s) et
# teste exactement la meme canonisation intra-fichier via owl_format().
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# catalog: snapshot-based tests of --vscode-data specific slices
# ---------------------------------------------------------------------------
sect_begin "catalog"
# Un `.cmd` sans `.expected.txt` n'est jamais joue : snapshot orphelin a
# l'envers, aussi muet que le cas symetrique.
for cmdf in "$ROOT/catalog"/*.cmd; do
    [[ -e "$cmdf" ]] || continue
    [[ -f "${cmdf%.cmd}.expected.txt" ]] || \
        miss "catalog/$(basename "${cmdf%.cmd}")" \
             "commande sans snapshot .expected.txt — jamais jouee, ne teste rien"
done
for expect in "$ROOT/catalog"/*.expected.txt; do
    [[ -e "$expect" ]] || continue
    name="$(basename "${expect%.expected.txt}")"
    cmd_file="${expect%.expected.txt}.cmd"
    if [[ ! -f "$cmd_file" ]]; then
        miss "catalog/$name" "aucun .cmd — snapshot orphelin, rien a executer"
        continue
    fi
    # .cmd file contains a grep/jq pipeline applied to --vscode-data output
    actual=$("$OWL_EXE" --vscode-data 2>/dev/null | bash "$cmd_file" 2>/dev/null)
    expected_content=$(cat "$expect")
    if [[ "$actual" == "$expected_content" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("catalog/$name")
        echo "FAIL catalog/$name:" >&2
        diff <(echo "$expected_content") <(echo "$actual") | head -20 >&2
    fi
done
sect_end

# ---------------------------------------------------------------------------
# catalog-gui: snapshot tests qui requierent owl.exe (natives UI Qt).
# Binaire optionnel : son absence est declaree au recap, plus un
# silence — on ne confond pas "pas de GUI ici" et "rien a verifier".
# ---------------------------------------------------------------------------
if [[ -x "$OWL_GUI_EXE" ]]; then
    sect_begin "catalog-gui"
    for expect in "$ROOT/catalog-gui"/*.expected.txt; do
        [[ -e "$expect" ]] || continue
        name="$(basename "${expect%.expected.txt}")"
        cmd_file="${expect%.expected.txt}.cmd"
        if [[ ! -f "$cmd_file" ]]; then
            miss "catalog-gui/$name" "aucun .cmd — snapshot orphelin, rien a executer"
            continue
        fi
        actual=$("$OWL_GUI_EXE" --vscode-data 2>/dev/null | bash "$cmd_file" 2>/dev/null)
        expected_content=$(cat "$expect")
        if [[ "$actual" == "$expected_content" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("catalog-gui/$name")
            echo "FAIL catalog-gui/$name:" >&2
            diff <(echo "$expected_content") <(echo "$actual") | head -20 >&2
        fi
    done
    sect_end
else
    sect_skip "catalog-gui" "NON EXECUTE : $OWL_GUI_EXE absent"
fi

# ---------------------------------------------------------------------------
# signature: PORTE en self-test C in-process.
# Voir `owl --self-test-signature` (src/internal_tests/signature_test.c),
# cable dans la cible Makefile `all`. Meme chemin (--check + scan du dossier
# signature/, capture stdout+stderr), un seul boot au lieu d'un spawn par
# fixture.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# fmt-context-aware: formatter with cross-file context (, )
# Three layout modes:
#   1. Legacy (flat):     case_NNN/context/*.owl + target.owl (at case_dir root)
#   2. Arborescent:       case_NNN/<subdirs> with target in subdirectory
#   3. --project flag:    case_NNN/project/ + isolated/ (test --project override)
# ---------------------------------------------------------------------------
sect_begin "fmt-context-aware"
for case_dir in "$ROOT/internal_fixtures/fmt-context-aware"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    case_name=$(basename "$case_dir")

    context_dir="$case_dir/context"
    project_dir="$case_dir/project"

    # Detect layout mode
    if [[ -d "$context_dir" ]]; then
        mode="legacy"
    elif [[ -d "$project_dir" ]]; then
        mode="project-flag"
    else
        mode="arborescent"
    fi

    # --- MODE 1: Legacy (flat) --- cases 001-011
    if [[ "$mode" == "legacy" ]]; then
        # Detect target file: prefer target.owl, fallback to test_*.owl, then any *.owl
        target=""
        expected=""
        if [[ -f "$case_dir/target.owl" ]]; then
            target="$case_dir/target.owl"
            expected="$case_dir/target.expected.owl"
        else
            for candidate in "$case_dir"/*.owl; do
                [[ -e "$candidate" ]] || continue
                case "$candidate" in *.expected.owl) continue;; esac
                target="$candidate"
                expected="${candidate%.owl}.expected.owl"
                break
            done
        fi

        if [[ ! -f "$target" ]]; then
            miss "fmt-context-aware/$case_name" "aucun fichier cible (.owl) — cas non examine"
            continue
        fi
        if [[ ! -f "$expected" ]]; then
            miss "fmt-context-aware/$case_name" "snapshot ${expected##*/} absent — cas non examine"
            continue
        fi

        tmpdir=$(mktemp -d -p "$OWL_TMP_BASE")
        target_basename=$(basename "$target")

        # Copy context files (flat) + target, run formatter from tmpdir (cwd semantic, )
        if [[ -d "$context_dir" ]]; then
            cp -r "$context_dir"/* "$tmpdir/" 2>/dev/null || true
        fi
        cp "$target" "$tmpdir/$target_basename"
        # Run formatter with cwd=tmpdir so context is visible (racine = cwd)
        ( cd "$tmpdir" && "$OWL_EXE" fmt "$target_basename" > /dev/null 2>&1 ) || true
        tr -d '\r' < "$tmpdir/$target_basename" > "$tmpdir/target.lf"
        if diff -q "$tmpdir/target.lf" "$expected" > /dev/null 2>&1; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("fmt-context-aware/$case_name")
            echo "FAIL fmt-context-aware/$case_name:" >&2
            diff -u "$expected" "$tmpdir/target.lf" | head -20 >&2
        fi
        rm -rf "$tmpdir"

    # --- MODE 2: Arborescent --- case_012
    elif [[ "$mode" == "arborescent" ]]; then
        tmpdir=$(mktemp -d -p "$OWL_TMP_BASE")
        # Copy entire case_dir into tmpdir, preserving structure, then remove *.expected.owl
        cp -r "$case_dir"/. "$tmpdir/"
        find "$tmpdir" -type f -name '*.expected.owl' -delete
        # Find target file (any *.owl except expected) in tmpdir
        target_rel=""
        expected_abs=""
        while IFS= read -r -d '' candidate; do
            rel_path="${candidate#$tmpdir/}"
            expected_abs="$case_dir/${rel_path%.owl}.expected.owl"
            if [[ -f "$expected_abs" ]]; then
                target_rel="$rel_path"
                break
            fi
        done < <(find "$tmpdir" -type f -name '*.owl' ! -name '*.expected.owl' -print0)

        if [[ -z "$target_rel" ]]; then
            miss "fmt-context-aware/$case_name" "aucune paire cible/snapshot — cas non examine"
            rm -rf "$tmpdir"
            continue
        fi

        # Run formatter from tmpdir with cwd=tmpdir (racine-projet semantic)
        ( cd "$tmpdir" && "$OWL_EXE" fmt "$target_rel" > /dev/null 2>&1 ) || true
        tr -d '\r' < "$tmpdir/$target_rel" > "$tmpdir/target.lf"
        if diff -q "$tmpdir/target.lf" "$expected_abs" > /dev/null 2>&1; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("fmt-context-aware/$case_name")
            echo "FAIL fmt-context-aware/$case_name:" >&2
            diff -u "$expected_abs" "$tmpdir/target.lf" | head -20 >&2
        fi
        rm -rf "$tmpdir"

    # --- MODE 3: --project flag --- case_013
    elif [[ "$mode" == "project-flag" ]]; then
        isolated_dir="$case_dir/isolated"
        if [[ ! -d "$isolated_dir" ]]; then
            miss "fmt-context-aware/$case_name" "dossier isolated/ absent — cas non examine"
            continue
        fi

        tmpdir=$(mktemp -d -p "$OWL_TMP_BASE")
        # Copy project/ and isolated/ into tmpdir
        cp -r "$project_dir" "$tmpdir/project" 2>/dev/null || true
        cp -r "$isolated_dir" "$tmpdir/isolated" 2>/dev/null || true
        #  : tmpdir DANS build/ (perf). owl fmt --project resout la
        # racine via projet.owl ancetre (resolve_project_root). Sans ce
        # marqueur ici, le scan reste non-recursif (mode SCRIPT) et rate
        # core/cBase.owl, donc le canonical override n'est plus applique.
        # Avant  le tmpdir /tmp marchait par coincidence (un
        # /tmp/projet.owl orphelin servait de marqueur involontaire).
        # On force le marqueur explicitement = test deterministe.
        : > "$tmpdir/project/projet.owl"

        # Find target in isolated/ (first *.owl except expected)
        target_isolated=""
        for candidate in "$tmpdir/isolated"/*.owl; do
            [[ -e "$candidate" ]] || continue
            case "$candidate" in *.expected.owl) continue;; esac
            target_isolated="$candidate"
            break
        done

        if [[ -z "$target_isolated" ]]; then
            miss "fmt-context-aware/$case_name" "aucune cible dans isolated/ — cas non examine"
            rm -rf "$tmpdir"
            continue
        fi

        target_basename=$(basename "$target_isolated")
        expected_with="$isolated_dir/${target_basename%.owl}.expected.owl"
        expected_without="$isolated_dir/${target_basename%.owl}_noproject.expected.owl"

        # Test 1: WITH --project flag
        cp "$isolated_dir/$target_basename" "$tmpdir/isolated/$target_basename"
        "$OWL_EXE" fmt "$tmpdir/isolated/$target_basename" --project "$tmpdir/project" > /dev/null 2>&1 || true
        tr -d '\r' < "$tmpdir/isolated/$target_basename" > "$tmpdir/target_with.lf"
        if [[ -f "$expected_with" ]]; then
            if diff -q "$tmpdir/target_with.lf" "$expected_with" > /dev/null 2>&1; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                failed+=("fmt-context-aware/$case_name (with --project)")
                echo "FAIL fmt-context-aware/$case_name (with --project):" >&2
                diff -u "$expected_with" "$tmpdir/target_with.lf" | head -20 >&2
            fi
        fi

        # Test 2: WITHOUT --project flag (verbatim expected, cwd=isolated so no context)
        cp "$isolated_dir/$target_basename" "$tmpdir/isolated/$target_basename"
        ( cd "$tmpdir/isolated" && "$OWL_EXE" fmt "$target_basename" > /dev/null 2>&1 ) || true
        tr -d '\r' < "$tmpdir/isolated/$target_basename" > "$tmpdir/target_without.lf"
        if [[ -f "$expected_without" ]]; then
            if diff -q "$tmpdir/target_without.lf" "$expected_without" > /dev/null 2>&1; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                failed+=("fmt-context-aware/$case_name (without --project)")
                echo "FAIL fmt-context-aware/$case_name (without --project):" >&2
                diff -u "$expected_without" "$tmpdir/target_without.lf" | head -20 >&2
            fi
        fi

        rm -rf "$tmpdir"
    fi
done
sect_end

# ---------------------------------------------------------------------------
# check-project: owl --check <fichier> --project <dir>
# Tests the CLI diagnostic with project context (prescan + filtered report).
# Each case_NNN/ contains:
#   - project/.../*.owl (context files)
#   - one target file with a .expected.json sibling
# Test invokes: owl --check <target> --project <project-dir>
# Expected output: mono-file format {"errors":[...],"warnings":[...]} with
# only the target file's diagnostics (not all project files).
# Fixtures moved to internal_fixtures/ to avoid owl test crashes.
# ---------------------------------------------------------------------------
sect_begin "check-project (activeFile)"
for case_dir in "$ROOT/internal_fixtures/check-project"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    case_name=$(basename "$case_dir")

    # Cette boucle balaie TOUTE la famille check-project ; les cas sans
    # `project/` (ou sans paire cible/snapshot) relevent d'une autre section.
    # Ce n'est donc pas un ecart de corpus ici — la garde qui compte est le
    # controle de couverture en fin de fichier : un cas qu'AUCUNE section
    # n'examine est un echec.
    project_dir="$case_dir/project"
    [[ -d "$project_dir" ]] || continue

    # Find target: any .owl in project_dir (or subdirs) with a .expected.json sibling
    target_abs=""
    expected_abs=""
    while IFS= read -r -d '' candidate; do
        expected_cand="${candidate%.owl}.expected.json"
        if [[ -f "$expected_cand" ]]; then
            target_abs="$candidate"
            expected_abs="$expected_cand"
            break
        fi
    done < <(find "$project_dir" -type f -name '*.owl' ! -name '*.expected.owl' -print0)

    [[ -n "$target_abs" ]] || continue
    cover "$case_name"

    # Invoke: owl --check <target> --project <project_dir>
    actual=$("$OWL_EXE" --check "$target_abs" --project "$project_dir" 2>&1)
    expected_content=$(cat "$expected_abs")

    if [[ "$actual" == "$expected_content" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("check-project/$case_name")
        echo "FAIL check-project/$case_name:" >&2
        echo "  expected: $expected_content" >&2
        echo "  actual:   $actual" >&2
    fi
done
sect_end

# ---------------------------------------------------------------------------
# check-project-mono: non-regression mono-fichier (sans --project)
# Tests that `owl --check <fichier>` without --project still works as before.
# Now also covers script/entrypoint mode tests (case_011+, no project/ subdir).
# Fixtures moved to internal_fixtures/ to avoid owl test crashes.
# ---------------------------------------------------------------------------
sect_begin "check-project (mono)"
for case_dir in "$ROOT/internal_fixtures/check-project"/case_003_nonregression_mono/ \
                "$ROOT/internal_fixtures/check-project"/case_011_script_mode_type_lointain/ \
                "$ROOT/internal_fixtures/check-project"/case_012_script_mode_autonome/ \
                "$ROOT/internal_fixtures/check-project"/case_014_entrypoint_mode_no_subdirs/; do
    case_name=$(basename "$case_dir")
    # Liste de cas EXPLICITE : un dossier nomme ici qui n'existe plus est une
    # fixture disparue, pas un cas d'une autre section.
    if [[ ! -d "$case_dir" ]]; then
        miss "check-project/$case_name (mono)" "dossier de fixture disparu"
        continue
    fi
    cover "$case_name"

    # Find target: any .owl with .expected.json sibling (recursive search)
    target_abs=""
    expected_abs=""
    while IFS= read -r -d '' candidate; do
        expected_cand="${candidate%.owl}.expected.json"
        if [[ -f "$expected_cand" ]]; then
            target_abs="$candidate"
            expected_abs="$expected_cand"
            break
        fi
    done < <(find "$case_dir" -type f -name '*.owl' ! -name '*.expected.owl' -print0)

    if [[ -z "$target_abs" ]]; then
        miss "check-project/$case_name (mono)" "aucune paire .owl / .expected.json — cas non examine"
        continue
    fi

    # Invoke: owl --check <target> (NO --project flag)
    actual=$("$OWL_EXE" --check "$target_abs" 2>&1)
    expected_content=$(cat "$expected_abs")

    if [[ "$actual" == "$expected_content" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("check-project/$case_name (mono)")
        echo "FAIL check-project/$case_name (mono):" >&2
        echo "  expected: $expected_content" >&2
        echo "  actual:   $actual" >&2
    fi
done
sect_end

# ---------------------------------------------------------------------------
# check-project-auto-resolve: test auto-resolution frontiere
# Tests that `owl --check <fichier>` WITHOUT --project resolves projet.owl
# ancestor and scans the full tree. Covers cases with project/ subdir BUT
# launched WITHOUT --project flag to test auto-discovery.
# Fixtures moved to internal_fixtures/ to avoid owl test crashes.
# ---------------------------------------------------------------------------
sect_begin "check-project (auto-resolve)"
# case_039 vit dans check-project/ (glob "activeFile" ET auto-resolve la
# couvrent, expected identique dans les deux : elle est toujours propre).
# case_040/041 vivent dans check-project-auto-resolve-only/ :
# elles verrouillent le comportement de la boucle compagnon 
# (compile + rapporte l'erreur du 1er compagnon fautif), qui n'existe QUE
# dans le chemin auto-resolve (handle_check_command_no_vm_lifecycle_ex).
# Le chemin --project explicite (handle_check_project_command, section
# "activeFile" ci-dessus) ne compile pas les compagnons : les swept via le
# glob check-project/case_*/ y produiraient un faux willing-echec structurel
# sans rapport avec , d'ou leur exclusion de ce glob.
for case_dir in "$ROOT/internal_fixtures/check-project"/case_013_projet_mode_frontiere_resolue/ \
                "$ROOT/internal_fixtures/check-project"/case_039_auto_resolve_exclut_script_prive_voisin/ \
                "$ROOT/internal_fixtures/check-project-auto-resolve-only"/case_040_auto_resolve_conserve_erreur_voisin_participant/ \
                "$ROOT/internal_fixtures/check-project-auto-resolve-only"/case_041_auto_resolve_script_mode_voisin_conserve/; do
    case_name=$(basename "$case_dir")
    if [[ ! -d "$case_dir" ]]; then
        miss "check-project/$case_name (auto-resolve)" "dossier de fixture disparu"
        continue
    fi
    cover "$case_name"

    project_dir="$case_dir/project"
    if [[ ! -d "$project_dir" ]]; then
        miss "check-project/$case_name (auto-resolve)" "dossier project/ disparu"
        continue
    fi

    # Find target: any .owl in project_dir (or subdirs) with a .expected.json sibling
    target_abs=""
    expected_abs=""
    while IFS= read -r -d '' candidate; do
        expected_cand="${candidate%.owl}.expected.json"
        if [[ -f "$expected_cand" ]]; then
            target_abs="$candidate"
            expected_abs="$expected_cand"
            break
        fi
    done < <(find "$project_dir" -type f -name '*.owl' ! -name '*.expected.owl' -print0)

    if [[ -z "$target_abs" ]]; then
        miss "check-project/$case_name (auto-resolve)" "aucune paire .owl / .expected.json — cas non examine"
        continue
    fi

    # Invoke: owl --check <target> WITHOUT --project (auto-resolve frontiere)
    actual=$("$OWL_EXE" --check "$target_abs" 2>&1)
    expected_content=$(cat "$expected_abs")

    if [[ "$actual" == "$expected_content" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("check-project/$case_name (auto-resolve)")
        echo "FAIL check-project/$case_name (auto-resolve):" >&2
        echo "  expected: $expected_content" >&2
        echo "  actual:   $actual" >&2
    fi
done
sect_end

# ---------------------------------------------------------------------------
# check-project-cstr-cross-file:  — cstr a args requis cross-file
# Tests that `owl --check <main.owl>` detects E_SEM_INSTANCE_NEEDS_ARGS when
# the class (with required-arg constructor) lives in another file of the same
# project. Each sous-dossier {a,b,c,d}/ est un projet complet (projet.owl +
# classes + main.owl + main.expected.json). Auto-resolve frontiere (pas de
# flag --project).
# ---------------------------------------------------------------------------
case_019_root="$ROOT/internal_fixtures/check-project/case_019_cstr_cross_file"
sect_begin "check-project/case_019 (cstr cross-file)"
cover "case_019_cstr_cross_file"
if [[ -d "$case_019_root" ]]; then
    for sub_dir in "$case_019_root"/*/; do
        [[ -d "$sub_dir" ]] || continue
        sub_name=$(basename "$sub_dir")

        target_abs="$sub_dir/main.owl"
        expected_abs="$sub_dir/main.expected.json"
        if [[ ! -f "$target_abs" || ! -f "$expected_abs" ]]; then
            miss "check-project/case_019/$sub_name" \
                 "main.owl ou main.expected.json absent — sous-cas non examine"
            continue
        fi

        actual=$("$OWL_EXE" --check "$target_abs" 2>&1)
        expected_content=$(cat "$expected_abs")

        if [[ "$actual" == "$expected_content" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("check-project/case_019/$sub_name")
            echo "FAIL check-project/case_019/$sub_name:" >&2
            echo "  expected: $expected_content" >&2
            echo "  actual:   $actual" >&2
        fi
    done
else
    miss "check-project/case_019_cstr_cross_file" "dossier de fixture disparu"
fi
sect_end

# ---------------------------------------------------------------------------
# check-project-folder: owl --check <dossier> (format multi-fichier {"files":[...]})
# Fixtures moved to internal_fixtures/ to avoid owl test crashes.
#
# Deux structures supportees (retrocompatibles) :
#
#   Ancienne (case_004, flat) :
#     case_NNN/<fichiers.owl> + expected.json  (a la racine du case)
#     -> Invocation : owl --check . (cwd = case_NNN/)
#
#   Nouvelle (, dirmode) :
#     case_NNN/project/<fichiers.owl> + project/expected.json + project/dirmode
#     -> Invocation : owl --check <project_dir> (absolu, depuis la racine)
#     Le marqueur `dirmode` (fichier vide) distingue cette structure de la
#     structure activeFile (qui a aussi un project/ mais sans dirmode).
#     Le fichier `dirmode` lui-meme est ignore par owl --check (pas un .owl).
# ---------------------------------------------------------------------------
sect_begin "check-project (folder / dirmode)"
for case_dir in "$ROOT/internal_fixtures/check-project"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    case_name=$(basename "$case_dir")

    project_sub="$case_dir/project"

    if [[ -f "$project_sub/dirmode" ]]; then
        # --- Structure dirmode : owl --check <project_dir> ---
        # Le marqueur `dirmode` DECLARE que ce cas releve de cette section
        # (le role de la fixture est dans son contenu, pas dans une liste
        # hors-bande). Une fois declare, l'absence du snapshot est un ECHEC.
        cover "$case_name"
        expected_abs="$project_sub/expected.json"
        if [[ ! -f "$expected_abs" ]]; then
            miss "check-project/$case_name (folder/dirmode)" \
                 "marqueur dirmode present mais project/expected.json absent"
            continue
        fi
        # Invoke depuis project_sub pour que les paths de sortie soient
        # relatifs (./fichier.owl) et coherents avec expected.json.
        actual=$(cd "$project_sub" && "$OWL_EXE" --check . 2>&1)
        expected_content=$(cat "$expected_abs")
        if [[ "$actual" == "$expected_content" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("check-project/$case_name (folder/dirmode)")
            echo "FAIL check-project/$case_name (folder/dirmode):" >&2
            echo "  expected: $expected_content" >&2
            echo "  actual:   $actual" >&2
        fi

    elif [[ -f "$case_dir/expected.json" ]] && [[ ! -d "$project_sub" ]]; then
        # --- Structure flat (case_004) : owl --check . depuis case_dir ---
        cover "$case_name"
        expected_abs="$case_dir/expected.json"
        actual=$(cd "$case_dir" && "$OWL_EXE" --check . 2>&1)
        expected_content=$(cat "$expected_abs")
        if [[ "$actual" == "$expected_content" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("check-project/$case_name (folder)")
            echo "FAIL check-project/$case_name (folder):" >&2
            echo "  expected: $expected_content" >&2
            echo "  actual:   $actual" >&2
        fi
    fi
    # Les cas avec project/ mais sans dirmode sont geres par les autres sections
    # (check-project activeFile, check-project-auto-resolve, etc.) — on saute.
    # Un cas qu'AUCUNE section ne prend en charge est attrape par le controle
    # de couverture en fin de fichier.
done
sect_end

# ---------------------------------------------------------------------------
# check-project-flag-custom: tests `--project <dir>` redefinition
# Tests that `--project <dir>` acts as "remontee point" (resolve_project_root)
# rather than "force recursive root". Each case has custom structure and
# invocation documented inline.
# ---------------------------------------------------------------------------

# case_015: --project <mega-depot> with multiple main.owl (ambiguous pre-fix, clean post-fix)
case_015_dir="$ROOT/internal_fixtures/check-project/case_015_project_flag_mega_depot"
sect_begin "check-project/case_015 (--project mega-depot)"
cover "case_015_project_flag_mega_depot"
if [[ ! -d "$case_015_dir/depot" ]]; then
    miss "check-project-flag-custom/case_015" "dossier depot/ disparu — cas non examine"
else
    target_015="$case_015_dir/depot/script_isole.owl"
    expected_015="$case_015_dir/depot/script_isole.expected.json"
    if [[ ! -f "$target_015" || ! -f "$expected_015" ]]; then
        miss "check-project-flag-custom/case_015" \
             "script_isole.owl ou son snapshot absent — cas non examine"
    else
        actual=$("$OWL_EXE" --check "$target_015" --project "$case_015_dir/depot" 2>&1)
        expected_content=$(cat "$expected_015")
        if [[ "$actual" == "$expected_content" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("check-project-flag-custom/case_015")
            echo "FAIL check-project-flag-custom/case_015:" >&2
            echo "  expected: $expected_content" >&2
            echo "  actual:   $actual" >&2
        fi
    fi
fi
sect_end

# case_017: --project <dir sans projet.owl> (mode SCRIPT post-fix, ne voit pas sous-dossiers)
case_017_dir="$ROOT/internal_fixtures/check-project/case_017_project_flag_sans_projet_owl"
sect_begin "check-project/case_017 (--project sans projet.owl)"
cover "case_017_project_flag_sans_projet_owl"
if [[ ! -d "$case_017_dir/root" ]]; then
    miss "check-project-flag-custom/case_017" "dossier root/ disparu — cas non examine"
else
    target_017="$case_017_dir/root/models/config.owl"
    expected_017="$case_017_dir/root/models/config.expected.json"
    if [[ ! -f "$target_017" || ! -f "$expected_017" ]]; then
        miss "check-project-flag-custom/case_017" \
             "models/config.owl ou son snapshot absent — cas non examine"
    else
        actual=$("$OWL_EXE" --check "$target_017" --project "$case_017_dir/root" 2>&1)
        expected_content=$(cat "$expected_017")
        if [[ "$actual" == "$expected_content" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("check-project-flag-custom/case_017")
            echo "FAIL check-project-flag-custom/case_017:" >&2
            echo "  expected: $expected_content" >&2
            echo "  actual:   $actual" >&2
        fi
    fi
fi
sect_end

# ---------------------------------------------------------------------------
# overlay-stdin: `owl --check <dossier> --overlay-stdin` ( Lot 1)
# Contrat CLI vise (POSE ICI, PAS ENCORE IMPLEMENTE — ROUGE ATTENDU tant que
# le Lot 1 du ticket  n'a pas livre l'overlay cote compilo) :
#   owl --check <dossier> --overlay-stdin
#   stdin JSON: {"overlays":[{"path":"<chemin exact emis par --check pour ce
#   fichier>","content":"<contenu texte>"}]}
# Semantique visee : le contenu overlaye remplace le disque pour la
# compilation de ce fichier ; les autres fichiers du projet restent lus
# depuis le disque (point d'injection confirme : project.c ~L456, substituer
# .source si le path est overlaye).
#
# Deux sens verifies dans le meme fixture (case_001_overlay_pilote_verdict/
# project/) pour prouver que c'est bien l'overlay qui pilote le verdict, pas
# le disque :
#   - fichierB.owl a une erreur SUR DISQUE (FonctionDisqueInconnue) ; l'overlay
#     fournit une version CORRIGEE (fichierB_overlay_fixed.owl) -> l'erreur
#     doit DISPARAITRE du check.
#   - fichierA.owl est valide SUR DISQUE ; l'overlay fournit une version
#     CASSEE (fichierA_overlay_broken.owl, FonctionOverlayInconnue) -> une
#     NOUVELLE erreur doit APPARAITRE dans le check.
# Le `path` emis par `--check <dossier>` (avec projet.owl ancetre : mode
# projet, path absolu resolu) est calcule dynamiquement ici (jamais
# hardcode : instable d'un checkout a l'autre).
# ---------------------------------------------------------------------------
overlay_case_dir="$ROOT/internal_fixtures/overlay-stdin/case_001_overlay_pilote_verdict/project"
sect_begin "overlay-stdin/case_001"
if [[ -d "$overlay_case_dir" ]]; then
    overlay_project_abs="$(cd "$overlay_case_dir" && pwd)"
    path_a="$overlay_project_abs/fichierA.owl"
    path_b="$overlay_project_abs/fichierB.owl"

    content_a_broken=$(cat "$overlay_case_dir/fichierA_overlay_broken.owl")
    content_a_broken_json="${content_a_broken//$'\n'/\\n}"
    content_b_fixed=$(cat "$overlay_case_dir/fichierB_overlay_fixed.owl")
    content_b_fixed_json="${content_b_fixed//$'\n'/\\n}"

    overlay_json="{\"overlays\":[{\"path\":\"${path_a}\",\"content\":\"${content_a_broken_json}\"},{\"path\":\"${path_b}\",\"content\":\"${content_b_fixed_json}\"}]}"

    actual=$(printf '%s' "$overlay_json" | "$OWL_EXE" --check "$overlay_project_abs" --overlay-stdin 2>&1)

    # Attendu APRES Lot 1 : fichierB (corrige par overlay) SANS son erreur
    # disque, fichierA (casse par overlay) AVEC la nouvelle erreur overlay.
    # ROUGE actuel : le flag est ignore aujourd'hui (soit argc mismatch ->
    # dump du usage CLI, soit le disque prime encore une fois le flag
    # reconnu) donc erreur B toujours presente et/ou erreur A absente.
    overlay_ok=true
    fail_reason=""
    if echo "$actual" | grep -qF "FonctionDisqueInconnue"; then
        overlay_ok=false; fail_reason="fichierB erreur disque toujours presente (overlay ignore)"
    fi
    if ! echo "$actual" | grep -qF "FonctionOverlayInconnue"; then
        overlay_ok=false; fail_reason="$fail_reason ; fichierA erreur overlay absente (overlay ignore)"
    fi

    if $overlay_ok; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("overlay-stdin/case_001_overlay_pilote_verdict")
        echo "FAIL overlay-stdin/case_001_overlay_pilote_verdict (ROUGE ATTENDU tant que feat/1246 Lot 1 n'est pas implemente):" >&2
        echo "  reason: $fail_reason" >&2
        echo "  actual: $actual" >&2
    fi
else
    miss "overlay-stdin/case_001_overlay_pilote_verdict" \
         "fixture project/ disparue — cas non examine"
fi
sect_end

# ---------------------------------------------------------------------------
# overlay-stdin: contenu overlaye > 9 Ko NON tronque ( Lot 2)
# Le parser JSON (jp_string, src/stdlib_json.c) a un buffer FIXE de 8192
# octets pour decoder les valeurs chaine : un `content` d'overlay dont la
# taille decodee depasse ce buffer est TRONQUE SILENCIEUSEMENT (pas
# d'erreur de parse JSON), donc le compilo voit une source coupee.
#
# Fixture case_002_overlay_gros_contenu/ :
#   - project/fichierGros.owl sur disque : trivial et valide (n=1 ; Trace(n)).
#     C'est LUI la cible de l'overlay (le path envoye dans le JSON).
#   - overlay_content/fichierGros_overlay_complet.owl (HORS de project/, pour
#     ne PAS etre scanne comme membre reel du projet) : ~11.4 Ko, remplissage
#     de commentaires valides puis, TOUT AU BOUT (offset ~11469, bien au-dela
#     de 8192), un appel a une fonction inexistante `FonctionToutAuBout()`.
#     Seul son CONTENU sert a construire le JSON overlay ; le fichier
#     lui-meme n'est jamais lu par le project scan.
# Verdict attendu SI le contenu est lu integralement (>9 Ko, donc au-dela
# de la limite actuelle 8192) : le check remonte l'erreur
# `FonctionToutAuBout`. ROUGE tant que jp_string tronque a 8192 octets :
# le contenu coupe avant l'appel final => l'erreur `FonctionToutAuBout`
# n'apparait PAS (soit le check est propre, soit une autre erreur de
# parse coupe-au-milieu apparait) -> verdict different du contenu complet.
# ---------------------------------------------------------------------------
overlay_gros_case="$ROOT/internal_fixtures/overlay-stdin/case_002_overlay_gros_contenu"
overlay_gros_dir="$overlay_gros_case/project"
overlay_gros_content_dir="$overlay_gros_case/overlay_content"
sect_begin "overlay-stdin/case_002 (>9 Ko)"
if [[ -d "$overlay_gros_dir" && -f "$overlay_gros_content_dir/fichierGros_overlay_complet.owl" ]]; then
    overlay_gros_abs="$(cd "$overlay_gros_dir" && pwd)"
    path_gros="$overlay_gros_abs/fichierGros.owl"

    content_gros_complet=$(cat "$overlay_gros_content_dir/fichierGros_overlay_complet.owl")
    content_gros_complet_json="${content_gros_complet//$'\n'/\\n}"

    overlay_gros_json="{\"overlays\":[{\"path\":\"${path_gros}\",\"content\":\"${content_gros_complet_json}\"}]}"

    actual_gros=$(printf '%s' "$overlay_gros_json" | "$OWL_EXE" --check "$overlay_gros_abs" --overlay-stdin 2>&1)

    # Preuve que les >9 Ko sont lus en entier : le diagnostic de fin de
    # fichier (FonctionToutAuBout, au-dela de l'octet 8192) doit etre present.
    if echo "$actual_gros" | grep -qF "FonctionToutAuBout"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("overlay-stdin/case_002_overlay_gros_contenu")
        echo "FAIL overlay-stdin/case_002_overlay_gros_contenu (ROUGE ATTENDU tant que jp_string tronque a 8192 octets, feat/1246 Lot 2):" >&2
        echo "  reason: diagnostic 'FonctionToutAuBout' absent (contenu overlay > 9 Ko tronque avant l'appel de fin)" >&2
        echo "  actual: $actual_gros" >&2
    fi
else
    miss "overlay-stdin/case_002_overlay_gros_contenu" \
         "fixture project/ ou overlay_content/ disparue — cas non examine"
fi
sect_end

# ---------------------------------------------------------------------------
# overlay-stdin-mono: `owl --check <fichier> --overlay-stdin` en mode SCRIPT
# (, regression  Lot 1). Contrat vise :
#   owl --check <fichier_sans_projet_owl_ancetre> --overlay-stdin
#   stdin JSON: {"overlays":[{"path":"<chemin absolu exact passe en argument
#   --check>","content":"<contenu texte>"}]}
# Semantique visee : identique a `owl --check <fichier>` (JSON mono
# {"errors":[...],"warnings":[...]}, PAS le format {"files":[...]} du mode
# projet) mais avec l'overlay applique au fichier cible au lieu du disque.
#
# La fixture overlay-stdin-mono/ ne contient PAS de projet.owl ancetre :
# c'est le mode SCRIPT (par opposition aux cases overlay-stdin/case_00X qui
# sont en mode PROJET, deja couvertes par Lot 1). Verifie empiriquement :
# `owl --check cible.owl` (sans flag) emet bien {"errors":[...]} sans "path"
# ni "files", confirmant le mode mono.
#
# Deux sens verifies (memes fichiers, deux invocations separees car en mode
# mono un seul fichier cible par appel) :
#   - cible.owl a une erreur SUR DISQUE (FonctionDisqueMono inconnue) ;
#     l'overlay fournit une version CORRIGEE -> l'erreur doit DISPARAITRE.
#   - cible_valide.owl est valide SUR DISQUE ; l'overlay fournit une version
#     CASSEE (FonctionOverlayMonoInconnue) -> une NOUVELLE erreur doit
#     APPARAITRE.
# ROUGE actuel attendu (avant ) : `--check <fichier> --overlay-stdin`
# tombe sur le HELP CLI (texte usage, exit 64) au lieu du JSON de
# diagnostics, dans les deux sens.
# ---------------------------------------------------------------------------
overlay_mono_dir="$ROOT/internal_fixtures/overlay-stdin-mono"
sect_begin "overlay-stdin-mono"
if [[ -d "$overlay_mono_dir" ]]; then
    overlay_mono_abs="$(cd "$overlay_mono_dir" && pwd)"
    path_mono_cible="$overlay_mono_abs/cible.owl"
    path_mono_valide="$overlay_mono_abs/cible_valide.owl"

    content_mono_fixed=$(cat "$overlay_mono_dir/cible_overlay_fixed.owl")
    content_mono_fixed_json="${content_mono_fixed//$'\n'/\\n}"
    content_mono_broken=$(cat "$overlay_mono_dir/cible_valide_overlay_broken.owl")
    content_mono_broken_json="${content_mono_broken//$'\n'/\\n}"

    overlay_mono_ok=true
    fail_reason_mono=""

    # Sens 1 : overlay CORRIGE une erreur disque -> l'erreur doit disparaitre.
    overlay_mono_json_1="{\"overlays\":[{\"path\":\"${path_mono_cible}\",\"content\":\"${content_mono_fixed_json}\"}]}"
    actual_mono_1=$(printf '%s' "$overlay_mono_json_1" | "$OWL_EXE" --check "$path_mono_cible" --overlay-stdin 2>&1)
    if echo "$actual_mono_1" | grep -qF "FonctionDisqueMono"; then
        overlay_mono_ok=false
        fail_reason_mono="sens1: erreur disque FonctionDisqueMono toujours presente malgre overlay corrige (ou help CLI recu au lieu du JSON)"
    fi

    # Sens 2 : overlay CASSE un fichier valide -> une nouvelle erreur doit apparaitre.
    overlay_mono_json_2="{\"overlays\":[{\"path\":\"${path_mono_valide}\",\"content\":\"${content_mono_broken_json}\"}]}"
    actual_mono_2=$(printf '%s' "$overlay_mono_json_2" | "$OWL_EXE" --check "$path_mono_valide" --overlay-stdin 2>&1)
    if ! echo "$actual_mono_2" | grep -qF "FonctionOverlayMonoInconnue"; then
        overlay_mono_ok=false
        fail_reason_mono="$fail_reason_mono ; sens2: erreur overlay FonctionOverlayMonoInconnue absente (ou help CLI recu au lieu du JSON)"
    fi

    if $overlay_mono_ok; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("overlay-stdin-mono/cible")
        echo "FAIL overlay-stdin-mono/cible (ROUGE ATTENDU tant que fix/1251 n'est pas implemente):" >&2
        echo "  reason: $fail_reason_mono" >&2
        echo "  actual_1: $actual_mono_1" >&2
        echo "  actual_2: $actual_mono_2" >&2
    fi
else
    miss "overlay-stdin-mono/cible" "fixture disparue — cas non examine"
fi
sect_end

# ---------------------------------------------------------------------------
# test-file-mode-project-context: non-regression `owl test <fichier>`
# Tests that `owl test <fichier_individuel>` resolves projet.owl ancestor and
# sees the full project tree (same auto-resolve as --check since ).
# Reproduces the  gap: test under projet.owl sees sibling types.
# ---------------------------------------------------------------------------
TEST_RUNTIME_DIR="$DIR/tests/runtime"
sect_begin "test-file-mode-project-context"
if [[ -d "$TEST_RUNTIME_DIR/test_context_projet_frontiere" ]]; then
    target_file="$TEST_RUNTIME_DIR/test_context_projet_frontiere/tests/test_voit_arbre_projet.owl"
    if [[ -f "$target_file" ]]; then
        # Invoke: owl test <fichier_individuel> (mode fichier, NOT dossier)
        output=$("$OWL_EXE" test "$target_file" 2>&1)
        exit_code=$?
        # Expected: exit 0, output contains "ALL CLEAR", NO "Type inconnu"
        if [[ $exit_code -eq 0 ]] && echo "$output" | grep -q "ALL CLEAR" && ! echo "$output" | grep -q "Type inconnu"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("test-file-mode-project-context")
            echo "FAIL test-file-mode-project-context:" >&2
            echo "  exit_code=$exit_code (expected 0)" >&2
            echo "  output:" >&2
            echo "$output" | head -20 >&2
        fi
    else
        miss "test-file-mode-project-context" \
             "test_voit_arbre_projet.owl disparu — cas non examine"
    fi
else
    miss "test-file-mode-project-context" "dossier de fixture disparu — cas non examine"
fi
sect_end

# ---------------------------------------------------------------------------
# run-align-project: run (owl <fichier> / owl <dossier>) aligned with
# project boundary (, ).
# Tests the project-aware dispatch: projet.owl reserved, scope boundaries,
# entrypoint resolution.
# Each case_NNN/ contains expected.txt with exit_code + output_pattern|output_exact.
# ---------------------------------------------------------------------------
sect_begin "run-align-project"
for case_dir in "$ROOT/internal_fixtures/run-align-project"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    case_name=$(basename "$case_dir")

    expected_file="$case_dir/expected.txt"
    if [[ ! -f "$expected_file" ]]; then
        miss "run-align-project/$case_name" "expected.txt absent — cas non examine"
        continue
    fi

    # Parse expected.txt
    expected_exit_pattern=$(grep "^exit_code:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    expected_output_exact=$(grep "^output_exact:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    expected_output_pattern=$(grep "^output_pattern:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

    #  — invocation pilotee par la STRUCTURE de la fixture (modele
    # d'entree a 2 niveaux) :
    #   * projet.owl / project.owl a la racine du case  -> `owl projet.owl`
    #     (lancement projet explicite par nommage du marqueur).
    #   * sous-dossier project/ ou script/              -> run_dir = ce dossier.
    #   * sinon                                         -> run_dir = case root.
    # Puis, dans run_dir :
    #   * projet.owl/project.owl present -> `owl <run_dir>` (lancement projet).
    #   * prog.owl present               -> `owl <run_dir>/prog.owl` (script
    #                                       nomme : entree = prog.owl).
    #   * aucun des deux                 -> `owl <run_dir>` (dir nu, attendu
    #                                       en erreur "aucun point d'entree").
    target=""
    is_marker_file=false
    if [[ -f "$case_dir/projet.owl" ]]; then
        target="$case_dir/projet.owl"; is_marker_file=true
    elif [[ -f "$case_dir/project.owl" ]]; then
        target="$case_dir/project.owl"; is_marker_file=true
    elif [[ -d "$case_dir/project" ]]; then
        target="$case_dir/project"
    elif [[ -d "$case_dir/script" ]]; then
        target="$case_dir/script"
    else
        target="$case_dir"
    fi

    # Run in isolated tmpdir to avoid interference from ambient files
    tmpdir=$(mktemp -d -p "$OWL_TMP_BASE")
    if $is_marker_file; then
        # `owl projet.owl` : copie le seul marqueur (lancement projet nomme).
        cp "$target" "$tmpdir/$(basename "$target")"
        run_target="$tmpdir/$(basename "$target")"
    else
        cp -r "$target"/. "$tmpdir/"
        if [[ -f "$tmpdir/projet.owl" || -f "$tmpdir/project.owl" ]]; then
            run_target="$tmpdir"                 # lancement projet
        elif [[ -f "$tmpdir/prog.owl" ]]; then
            run_target="$tmpdir/prog.owl"        # script nomme
        else
            run_target="$tmpdir"                 # dir nu -> erreur attendue
        fi
    fi

    # Invoke: owl <run_target>
    # Capture stdout+stderr AND exit code (no || true before $?)
    # QT_QPA_PLATFORM=offscreen : un cas GUI reel (TestModeHeadless(Faux),
    # Ouvre()) ne doit jamais faire surgir de fenetre pendant la batterie ni
    # exiger un display (CI). Inoffensif pour les cas headless (Qt n'initialise
    # aucune plateforme). Le wrapper timeout borne tout cas qui bloquerait.
    actual_output=$(QT_QPA_PLATFORM=offscreen "${RUN_TIMEOUT[@]}" "$OWL_EXE" "$run_target" 2>&1)
    actual_exit=$?
    rm -rf "$tmpdir"

    # Check exit code
    exit_ok=false
    if [[ "$expected_exit_pattern" == "0" ]] && [[ $actual_exit -eq 0 ]]; then
        exit_ok=true
    elif [[ "$expected_exit_pattern" == "non-zero" ]] && [[ $actual_exit -ne 0 ]]; then
        exit_ok=true
    elif [[ "$expected_exit_pattern" =~ ^[0-9]+$ ]] && [[ $actual_exit -eq $expected_exit_pattern ]]; then
        exit_ok=true
    fi
    # Un timeout (process tue par `timeout`) est TOUJOURS un echec, meme si
    # expected=non-zero : un cas qui bloque ne doit jamais passer au vert.
    if [[ $actual_exit -eq 124 ]]; then
        exit_ok=false
    fi

    # Check output
    output_ok=false
    if [[ -n "$expected_output_exact" ]]; then
        if [[ "$actual_output" == "$expected_output_exact" ]]; then
            output_ok=true
        fi
    elif [[ -n "$expected_output_pattern" ]]; then
        if echo "$actual_output" | grep -qE "$expected_output_pattern"; then
            output_ok=true
        fi
    else
        # No output expectation: always OK
        output_ok=true
    fi

    if $exit_ok && $output_ok; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("run-align-project/$case_name")
        echo "FAIL run-align-project/$case_name:" >&2
        if ! $exit_ok; then
            echo "  exit_code=$actual_exit (expected $expected_exit_pattern)" >&2
        fi
        if ! $output_ok; then
            echo "  output mismatch:" >&2
            echo "    expected: $expected_output_exact$expected_output_pattern" >&2
            echo "    actual:   $actual_output" | head -5 >&2
        fi
    fi
done

# ---------------------------------------------------------------------------
# run-single: resolution des voisins en run fichier direct. `owl <fichier>`
# charge toujours le contexte du dossier (auto-discovery des voisins ->
# symboles resolus, top-levels voisins joues).
# Chaque case_NNN/ contient un fichier cible + des voisins + expected.txt :
#   target: <fichier cible relatif au case_dir>
#   exit_code: 0|non-zero|<n>
#   output_present:  <pattern qui DOIT apparaitre>
#   output_present2: <second pattern qui DOIT apparaitre> (optionnel)
#   output_absent:   <pattern qui NE DOIT PAS apparaitre> (optionnel)
#   args:            <arguments CLI supplementaires, splittes par espace> (optionnel,
#                     — appended a l'invocation `owl <target> <args...>`)
# Le dossier est copie dans un tmpdir pour isoler de tout main.owl ambiant.
# ---------------------------------------------------------------------------
sect_end

# ---------------------------------------------------------------------------
# check-flag-order: `owl <fichier> --check` (flag APRES le fichier, ordre CLI
# naturel « fichier puis option ») doit etre strictement symetrique de
# `owl --check <fichier>`.
#
# Avant le correctif, `--check` en position post-fichier tombait dans la
# branche run-avec-args et etait avale comme argument de script : le fichier
# partait en EXECUTION. Un fichier dont l'entree top-level n'est pas lancable
# (ici une Structure) sortait alors sur le message de lancabilite (« ce type
# ne peut pas etre lance directement ») en MASQUANT les diagnostics de
# compilation — meme quand le fichier ne compile pas (ici, appel d'une
# fonction inexistante). Le check ne porte pas de verdict de lancabilite :
# celui-ci reste l'apanage du chemin run (`owl <fichier>` sans --check).
#
# Trois preuves independantes :
#   1. SYMETRIE : sortie ET code de sortie identiques entre les deux formes.
#   2. L'erreur de compilation est REMONTEE par la forme flag-apres-fichier.
#   3. Le message de lancabilite ne MASQUE plus le diagnostic.
# ---------------------------------------------------------------------------
sect_begin "check-flag-order"
cfo_dir="$ROOT/internal_fixtures/check-flag-order"
if [[ -d "$cfo_dir" ]]; then
    cfo_file="$cfo_dir/f.owl"
    if [[ -f "$cfo_file" ]]; then
        out_cmd=$("$OWL_EXE" --check "$cfo_file" 2>&1); rc_cmd=$?
        out_flag=$("$OWL_EXE" "$cfo_file" --check 2>&1); rc_flag=$?

        cfo_ok=true
        cfo_reason=""
        # 1. Symetrie stricte (sortie octet-pour-octet + code de sortie).
        if [[ "$out_cmd" != "$out_flag" ]]; then
            cfo_ok=false; cfo_reason="sorties divergentes entre les deux formes"
        fi
        if [[ $rc_cmd -ne $rc_flag ]]; then
            cfo_ok=false; cfo_reason="$cfo_reason ; codes de sortie divergents ($rc_cmd vs $rc_flag)"
        fi
        # 2. L'erreur de compilation est bien remontee par la forme post-fichier.
        if ! echo "$out_flag" | grep -qF "fonction/procedure inconnue"; then
            cfo_ok=false; cfo_reason="$cfo_reason ; forme flag-apres-fichier ne remonte pas l'erreur de compilation"
        fi
        # 3. Le verdict de lancabilite ne masque plus le diagnostic (substring ASCII).
        if echo "$out_flag" | grep -qF "ne peut pas"; then
            cfo_ok=false; cfo_reason="$cfo_reason ; forme flag-apres-fichier masque encore derriere la lancabilite"
        fi
        # 4. Contrat de sortie exact : un fichier qui ne compile pas sort en 65.
        if [[ $rc_flag -ne 65 ]]; then
            cfo_ok=false; cfo_reason="$cfo_reason ; code de sortie attendu 65, obtenu $rc_flag"
        fi

        if $cfo_ok; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed+=("check-flag-order")
            echo "FAIL check-flag-order:" >&2
            echo "  reason: $cfo_reason" >&2
            echo "  owl --check <f> (rc=$rc_cmd): $out_cmd" >&2
            echo "  owl <f> --check (rc=$rc_flag): $out_flag" >&2
        fi
    else
        miss "check-flag-order" "fixture f.owl absente — cas non examine"
    fi
else
    miss "check-flag-order" "dossier de fixture disparu — cas non examine"
fi
sect_end

sect_begin "run-single"
for case_dir in "$ROOT/internal_fixtures/run-single"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    case_name=$(basename "$case_dir")

    expected_file="$case_dir/expected.txt"
    if [[ ! -f "$expected_file" ]]; then
        miss "run-single/$case_name" "expected.txt absent — cas non examine"
        continue
    fi

    rs_target=$(grep "^target:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    rs_exit=$(grep "^exit_code:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    rs_present=$(grep "^output_present:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    rs_present2=$(grep "^output_present2:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    rs_absent=$(grep "^output_absent:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    rs_args=$(grep "^args:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

    if [[ -z "$rs_target" ]]; then
        miss "run-single/$case_name" "aucune ligne target: dans expected.txt — cas non examine"
        continue
    fi

    tmpdir=$(mktemp -d -p "$OWL_TMP_BASE")
    cp -r "$case_dir"/. "$tmpdir/"
    rm -f "$tmpdir/expected.txt"

    if [[ -n "$rs_args" ]]; then
        # shellcheck disable=SC2206 # split volontaire par espace (args CLI simples)
        rs_args_arr=($rs_args)
        actual_output=$("${RUN_TIMEOUT[@]}" "$OWL_EXE" "$tmpdir/$rs_target" "${rs_args_arr[@]}" 2>&1)
    else
        actual_output=$("${RUN_TIMEOUT[@]}" "$OWL_EXE" "$tmpdir/$rs_target" 2>&1)
    fi
    actual_exit=$?
    rm -rf "$tmpdir"

    exit_ok=false
    if [[ "$rs_exit" == "0" ]] && [[ $actual_exit -eq 0 ]]; then
        exit_ok=true
    elif [[ "$rs_exit" == "non-zero" ]] && [[ $actual_exit -ne 0 ]]; then
        exit_ok=true
    elif [[ "$rs_exit" =~ ^[0-9]+$ ]] && [[ $actual_exit -eq $rs_exit ]]; then
        exit_ok=true
    fi
    # Un timeout (process tue par `timeout`) est TOUJOURS un echec, meme si
    # expected=non-zero : un cas qui bloque ne doit jamais passer au vert.
    timed_out=false
    if [[ $actual_exit -eq 124 ]]; then
        exit_ok=false
        timed_out=true
    fi

    out_ok=true
    fail_reason=""
    if [[ -n "$rs_present" ]] && ! echo "$actual_output" | grep -qF -- "$rs_present"; then
        out_ok=false; fail_reason="missing '$rs_present'"
    fi
    if [[ -n "$rs_present2" ]] && ! echo "$actual_output" | grep -qF -- "$rs_present2"; then
        out_ok=false; fail_reason="$fail_reason missing '$rs_present2'"
    fi
    if [[ -n "$rs_absent" ]] && echo "$actual_output" | grep -qF -- "$rs_absent"; then
        out_ok=false; fail_reason="$fail_reason unexpected '$rs_absent'"
    fi

    if $exit_ok && $out_ok; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("run-single/$case_name")
        echo "FAIL run-single/$case_name:" >&2
        if $timed_out; then
            echo "  TIMEOUT (>${OWL_RUN_TIMEOUT}s, tue) exit_code=124" >&2
        elif ! $exit_ok; then
            echo "  exit_code=$actual_exit (expected $rs_exit)" >&2
        fi
        if ! $out_ok; then
            echo "  output check: $fail_reason" >&2
            echo "  actual: $actual_output" | head -5 >&2
        fi
    fi
done

# ---------------------------------------------------------------------------
# multi-erreur-reprise: `owl --check <fichier>` doit rapporter TOUTES les
# erreurs de niveau instruction independantes, pas seulement la 1re
# ( volet B). Chaque case_NNN/ contient target.owl + expected.txt :
#   error_count: <N>            nombre attendu d'occurrences de `"line":`
#                                dans le JSON de sortie
#   contains: <texte>           (0..N lignes) chaque texte DOIT apparaitre
#                                dans la sortie
# ROUGE actuel (avant  volet B) : l'affectation a une variable non
# declaree arrete la reprise apres la 1re erreur -> error_count reel = 1.
# ---------------------------------------------------------------------------
sect_end
sect_begin "multi-erreur-reprise"
for case_dir in "$ROOT/internal_fixtures/multi-erreur-reprise"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    case_name=$(basename "$case_dir")

    target_file="$case_dir/target.owl"
    expected_file="$case_dir/expected.txt"
    if [[ ! -f "$target_file" || ! -f "$expected_file" ]]; then
        miss "multi-erreur-reprise/$case_name" "target.owl ou expected.txt absent — cas non examine"
        continue
    fi

    mer_expected_count=$(grep "^error_count:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

    actual=$("$OWL_EXE" --check "$target_file" 2>&1)
    actual_count=$(grep -o '"line":' <<<"$actual" | wc -l | tr -d '[:space:]')

    mer_ok=true
    mer_reason=""
    if [[ -n "$mer_expected_count" ]] && [[ "$actual_count" != "$mer_expected_count" ]]; then
        mer_ok=false
        mer_reason="error_count=$actual_count (expected $mer_expected_count)"
    fi

    while IFS= read -r mer_needle; do
        [[ -n "$mer_needle" ]] || continue
        if ! echo "$actual" | grep -qF -- "$mer_needle"; then
            mer_ok=false
            mer_reason="$mer_reason ; missing '$mer_needle'"
        fi
    done < <(grep "^contains:" "$expected_file" | cut -d: -f2- | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

    if $mer_ok; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failed+=("multi-erreur-reprise/$case_name")
        echo "FAIL multi-erreur-reprise/$case_name (ROUGE ATTENDU tant que feat/1253 volet B n'est pas implemente):" >&2
        echo "  reason: $mer_reason" >&2
        echo "  actual: $actual" >&2
    fi
done

sect_end

# ---------------------------------------------------------------------------
# Controle de couverture : la regle rendue totale.
#
# La famille check-project est balayee par plusieurs sections qui se partagent
# les cas selon leur LAYOUT (project/ + paire cible/snapshot, marqueur
# `dirmode`, structure flat, ou nom cite explicitement). Le role d'une fixture
# est donc dans sa forme, pas dans une liste hors-bande. Corollaire : tout
# case_* qu'aucune section n'a pris en charge est une fixture orpheline — elle
# existe, elle a l'air de couvrir quelque chose, et rien ne la joue.
# ---------------------------------------------------------------------------
cp_covered=0
cp_total=0
for case_dir in "$ROOT/internal_fixtures/check-project"/case_*/; do
    [[ -d "$case_dir" ]] || continue
    cp_total=$((cp_total + 1))
    cn=$(basename "$case_dir")
    if [[ -n "${COVERED[$cn]:-}" ]]; then
        cp_covered=$((cp_covered + 1))
    else
        miss "check-project/$cn" \
             "AUCUNE section ne l'examine — fixture orpheline. Lui donner un layout reconnu (project/ + <cible>.owl + <cible>.expected.json, ou project/dirmode + project/expected.json), ou la supprimer."
    fi
done
SECT_REPORT+=("$(printf '  %-38s %4d/%d couverts' "check-project (couverture)" "$cp_covered" "$cp_total")")

total=$((pass + fail))

echo "compiler tests — cas examines par section :"
for line in "${SECT_REPORT[@]}"; do echo "$line"; done
echo "compiler tests: $pass/$total passed"

# Garde ensemble vide globale : "0 fail" ne doit pas pouvoir signifier
# "rien n'a ete examine".
if [[ $total -eq 0 ]]; then
    echo "compiler tests: CORPUS VIDE — 0 cas execute sur l'ensemble du harnais." >&2
    echo "Un controle qui n'examine rien ne prouve rien." >&2
    exit 1
fi

if [[ $fail -gt 0 ]]; then
    echo "FAILED:" >&2
    for f in "${failed[@]}"; do echo "  - $f" >&2; done
    exit 1
fi
exit 0
