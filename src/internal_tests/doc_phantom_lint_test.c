// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test de calibration du lint anti-natives-fantomes (,
 * volet B). Modelise sur compile_error_test.c : un corpus de fixtures +
 * un oracle machine-lisible.
 *
 * Corpus  : fiches .md de tests/doc-lint-fixtures (catch et ignore)
 * Oracle  : tests/doc-lint-fixtures/EXPECTED.json
 *           { "catch": {"<fichier>": [noms flagges...]},
 *             "ignore": {"<fichier>": []} }
 *
 * Pour chaque fixture, le detecteur partage owl_doc_phantom_detect
 * (doc_phantom_lint.h) — le MEME que la Regle 63 de production — doit
 * produire EXACTEMENT l'ensemble de noms de l'oracle (ni plus, ni moins).
 * La calibration est donc une preuve directe que le detecteur lache sur
 * le vrai corpus est correct.
 *
 * Patron de non-vacuite (cf. compile_error_test.c) : le harnais PROUVE
 * qu'il a itere les 16 fixtures (6 catch + 10 ignore). Corpus absent,
 * vide, fixture sans entree oracle, ou entree oracle sans fixture = ECHEC.
 *
 * Branche sur `owl --self-test-doc-phantom-lint` (cf src/main.c), gate
 * make all via scripts/run_all_checks.sh. Retour : nombre d'echecs. */

#include "doc_phantom_lint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#define DPL_FIX_DIR    "tests/doc-lint-fixtures"
#define DPL_EXPECTED   "tests/doc-lint-fixtures/EXPECTED.json"
#define DPL_MAX_NAMES  64

static int g_pass = 0;
static int g_fail = 0;
static int g_catchCases  = 0;
static int g_ignoreCases = 0;

static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Extrait dans out[][] les noms attendus pour `basename` depuis le JSON
 * oracle. Retourne le nombre de noms, ou -1 si la fixture n'a AUCUNE
 * entree dans l'oracle (orphelin -> echec). */
static int expected_names_for(const char* json, const char* basename,
                              char out[][OWL_DOC_PHANTOM_NAME_MAX], int maxNames) {
    char needle[300];
    snprintf(needle, sizeof needle, "\"%s\"", basename);
    const char* key = strstr(json, needle);
    if (!key) return -1;
    const char* lb = strchr(key, '[');
    if (!lb) return -1;
    const char* rb = strchr(lb, ']');
    if (!rb) return -1;
    int n = 0;
    for (const char* p = lb + 1; p < rb; p++) {
        if (*p != '"') continue;
        const char* s = p + 1;
        const char* e = strchr(s, '"');
        if (!e || e > rb) break;
        if (n < maxNames) {
            size_t len = (size_t)(e - s);
            if (len >= OWL_DOC_PHANTOM_NAME_MAX) len = OWL_DOC_PHANTOM_NAME_MAX - 1;
            memcpy(out[n], s, len);
            out[n][len] = '\0';
            n++;
        }
        p = e;
    }
    return n;
}

static bool set_contains(char set[][OWL_DOC_PHANTOM_NAME_MAX], int n, const char* name) {
    for (int i = 0; i < n; i++) if (strcmp(set[i], name) == 0) return true;
    return false;
}

/* Compare l'ensemble produit par le detecteur a l'ensemble attendu.
 * Egalite d'ensembles (ni plus, ni moins). Renvoie true si conforme. */
static bool compare_sets(const char* label,
                         OwlDocPhantomHit* got, int gotN,
                         char exp[][OWL_DOC_PHANTOM_NAME_MAX], int expN) {
    bool ok = true;
    for (int i = 0; i < gotN; i++) {
        if (!set_contains(exp, expN, got[i].name)) {
            fprintf(stderr, "  FAIL %s : flag INATTENDU '%s' (ligne %d)\n",
                    label, got[i].name, got[i].line);
            ok = false;
        }
    }
    for (int i = 0; i < expN; i++) {
        bool found = false;
        for (int k = 0; k < gotN; k++) if (strcmp(got[k].name, exp[i]) == 0) { found = true; break; }
        if (!found) {
            fprintf(stderr, "  FAIL %s : flag MANQUANT '%s' (attendu par l'oracle)\n",
                    label, exp[i]);
            ok = false;
        }
    }
    return ok;
}

static void process_fixture(const char* subdir, const char* name, const char* json) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/%s", DPL_FIX_DIR, subdir, name);
    char* buf = slurp(path);
    if (!buf) {
        g_fail++;
        fprintf(stderr, "  MISS %s/%s (lecture impossible — cas non examine)\n", subdir, name);
        return;
    }

    char exp[DPL_MAX_NAMES][OWL_DOC_PHANTOM_NAME_MAX];
    int expN = expected_names_for(json, name, exp, DPL_MAX_NAMES);
    if (expN < 0) {
        g_fail++;
        fprintf(stderr, "  ORPHELIN %s/%s : fixture sans entree dans EXPECTED.json\n",
                subdir, name);
        free(buf);
        return;
    }

    OwlDocPhantomHit got[OWL_DOC_PHANTOM_MAX_HITS];
    int gotN = owl_doc_phantom_detect(buf, got, OWL_DOC_PHANTOM_MAX_HITS);
    free(buf);

    char label[512];
    snprintf(label, sizeof label, "%s/%s", subdir, name);
    if (compare_sets(label, got, gotN, exp, expN)) g_pass++;
    else g_fail++;

    if (strcmp(subdir, "catch") == 0) g_catchCases++;
    else g_ignoreCases++;
}

static bool has_md_suffix(const char* n) {
    size_t len = strlen(n);
    return len > 3 && strcmp(n + len - 3, ".md") == 0;
}

/* Compte les entrees "<...>.md": dans l'oracle (les deux directions de
 * l'orphelin sont ainsi couvertes : fixture sans entree ci-dessus,
 * entree sans fixture ici). */
static int count_json_md_entries(const char* json) {
    int n = 0;
    for (const char* p = json; (p = strstr(p, ".md\"")) != NULL; p += 4) n++;
    return n;
}

static void scan_subdir(const char* subdir, const char* json) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", DPL_FIX_DIR, subdir);
    DIR* d = opendir(dir);
    if (!d) {
        g_fail++;
        fprintf(stderr, "doc-phantom-lint: CORPUS INTROUVABLE — %s absent "
                        "(cwd = racine du depot attendue).\n", dir);
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!has_md_suffix(e->d_name)) continue;
        process_fixture(subdir, e->d_name, json);
    }
    closedir(d);
}

int doc_phantom_lint_self_test_run(void) {
    g_pass = 0; g_fail = 0; g_catchCases = 0; g_ignoreCases = 0;

    char* json = slurp(DPL_EXPECTED);
    if (!json) {
        fprintf(stderr, "doc-phantom-lint: ORACLE INTROUVABLE — %s absent.\n", DPL_EXPECTED);
        return 1;
    }

    scan_subdir("catch",  json);
    scan_subdir("ignore", json);

    int jsonEntries = count_json_md_entries(json);
    int filesSeen = g_catchCases + g_ignoreCases;

    /* Non-vacuite : le corpus doit correspondre exactement a l'oracle. */
    if (filesSeen == 0) {
        g_fail++;
        fprintf(stderr, "doc-phantom-lint: CORPUS VIDE — 0 fixture iteree. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
    }
    if (jsonEntries != filesSeen) {
        g_fail++;
        fprintf(stderr, "doc-phantom-lint: DESACCORD oracle/corpus — %d entrees "
                        "dans EXPECTED.json mais %d fixtures sur disque (une "
                        "entree sans fixture ou l'inverse).\n", jsonEntries, filesSeen);
    }

    fprintf(stderr, "doc-phantom-lint: %d fixtures iterees (%d catch, %d ignore) — "
                    "%d pass, %d fail\n",
            filesSeen, g_catchCases, g_ignoreCases, g_pass, g_fail);
    free(json);
    return g_fail;
}
