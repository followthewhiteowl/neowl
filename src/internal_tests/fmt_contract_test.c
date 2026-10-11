// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test fmt-contract.
 *
 * Le formateur tourne a l'enregistrement dans l'editeur. Son contrat est de
 * reecrire sans changer le sens ; le defaut qui a motive ce harnais le voyait
 * casser la compilation (`CAS =] ".pdf"` reecrit `CAS = ] ".pdf"`, deux
 * « Expression attendue »). Le defaut vivait depuis longtemps : rien ne
 * verifiait cette propriete, et il a fini decouvert par un agent qui passait
 * par la. fmt-stability compare a des snapshots — il ne voit qu'un ecart deja
 * fige dans un `.expected.owl`, donc rien d'une famille d'operateurs que le
 * corpus n'emploie pas encore.
 *
 * Deux proprietes verifiees sur chaque fichier du corpus, sans snapshot :
 *
 *   1. IDEMPOTENCE — `fmt(fmt(x)) == fmt(x)`, octet a octet. Un formateur qui
 *      derive a chaque passe fait diverger le fichier au fil des sauvegardes.
 *
 *   2. COMPILATION PRESERVEE — si `x` compile, `fmt(x)` doit compiler. C'est
 *      la propriete qui manquait. Conditionnelle par construction : le corpus
 *      contient des entrees qui ne compilent pas seules (fixtures de mise en
 *      forme pure, fichiers d'un projet cross-fichier). Elles sont comptees a
 *      part, jamais silencieuses.
 *
 * Corpus : les entrees du corpus formateur (tests/compiler/fmt-stability) et
 * les scenarios pedagogiques (tests/scenarios), parcourus recursivement.
 *
 * Branche sur `owl --self-test-fmt-contract` (cf src/main.c), gate `make all`.
 * Ce harnais compile — il ne peut donc PAS rejoindre fmt_stability_test.c, qui
 * est aussi linke dans owl-lsp-test.exe (backend catalogue, sans compilateur).
 *
 * Retour : nombre d'echecs (0 = tout passe). */

#include "compiler.h"
#include "formatter.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int g_pass = 0;
static int g_fail = 0;
/* Fichiers qui ne compilent pas seuls : la propriete 2 ne leur est pas
 * applicable. Comptes et affiches — un fichier ecarte doit se voir. */
static int g_non_compilables = 0;

static const char* const CORPUS_DIRS[] = {
    "tests/compiler/fmt-stability",
    "tests/scenarios",
};

/* Lit un fichier entier (NULL si absent). Caller free(). */
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

static bool has_suffix(const char* name, const char* suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

static bool is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

/* Compile `src` en isolant l'etat global partage entre deux compilations
 * successives dans le meme processus (registres de fenetres/etats, tables POO
 * persistantes du mode projet). Diagnostics captures = sortie muette. */
static bool source_compiles(const char* src, char* firstError, size_t cap) {
    if (firstError && cap > 0) firstError[0] = '\0';
    vm_windows_clear();
    vm_etats_clear();
    compiler_reset_poo_state();

    CompileDiagnostic errors[64];
    int errorCount = 0;
    CompileDiagnostic warnings[64];
    int warningCount = 0;
    compile_set_error_capture(errors, 64, &errorCount);
    compile_set_warning_capture(warnings, 64, &warningCount);

    compile(src);

    compile_set_error_capture(NULL, 0, NULL);
    compile_set_warning_capture(NULL, 0, NULL);
    compile_reset_parser_error_state();
    if (errorCount > 0 && firstError && cap > 0)
        snprintf(firstError, cap, "%s", errors[0].message);
    return errorCount == 0;
}

static void run_one(const char* path) {
    char* src = slurp(path);
    if (!src) return;

    char* once = owl_format(src);
    if (!once) {
        g_fail++;
        fprintf(stderr, "  FAIL fmt-contract/%s : owl_format a renvoye NULL\n", path);
        free(src);
        return;
    }

    char* twice = owl_format(once);
    if (!twice || strcmp(twice, once) != 0) {
        g_fail++;
        fprintf(stderr, "  FAIL fmt-contract/%s : NON IDEMPOTENT — la 2e passe "
                        "modifie encore le fichier (il derive a chaque "
                        "sauvegarde)\n", path);
    } else {
        g_pass++;
    }
    free(twice);

    if (!source_compiles(src, NULL, 0)) {
        /* Entree non compilable seule : la propriete 2 n'a pas de sens ici. */
        g_non_compilables++;
        free(once); free(src);
        return;
    }

    char why[1024];
    if (source_compiles(once, why, sizeof why)) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL fmt-contract/%s : LE FORMATEUR CASSE LA "
                        "COMPILATION — l'entree compile, sa sortie formatee "
                        "non. Le formateur reecrit sans changer le sens ; il "
                        "tourne a l'enregistrement, donc ce defaut detruit le "
                        "fichier de l'utilisateur pendant qu'il travaille.\n"
                        "         1re erreur apres formatage : %s\n",
                path, why);
    }

    free(once);
    free(src);
}

static void walk(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, n);
        if (is_dir(path)) { walk(path); continue; }
        if (!has_suffix(n, ".owl")) continue;
        /* Snapshots : deja couverts par fmt-stability, et apparies a une
         * entree deja parcourue. */
        if (has_suffix(n, ".expected.owl")) continue;
        /* Marqueur de frontiere : sa DSL est un chantier a part, le formateur
         * ne le traite pas. */
        if (strcmp(n, "projet.owl") == 0) continue;
        run_one(path);
    }
    closedir(d);
}

int fmt_contract_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    g_non_compilables = 0;

    for (size_t i = 0; i < sizeof CORPUS_DIRS / sizeof CORPUS_DIRS[0]; i++) {
        if (!is_dir(CORPUS_DIRS[i])) {
            g_fail++;
            fprintf(stderr, "fmt-contract: CORPUS INTROUVABLE — %s absent "
                            "(cwd = racine du depot attendue).\n",
                    CORPUS_DIRS[i]);
            continue;
        }
        walk(CORPUS_DIRS[i]);
    }

    fprintf(stderr, "fmt-contract: %d proprietes verifiees (%d pass, %d fail), "
                    "%d fichiers non compilables seuls (propriete "
                    "« compilation preservee » non applicable)\n",
            g_pass + g_fail, g_pass, g_fail, g_non_compilables);

    /* Garde ensemble vide : « 0 echec » sur un corpus vide n'est pas un
     * succes. */
    if (g_pass + g_fail == 0) {
        g_fail++;
        fprintf(stderr, "fmt-contract: CORPUS VIDE — aucun fichier examine. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
    }
    return g_fail;
}
