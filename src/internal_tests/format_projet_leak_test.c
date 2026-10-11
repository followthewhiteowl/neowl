// SPDX-License-Identifier: AGPL-3.0-only
/* format_projet_leak_test.c —  : verrou anti-fuite de l'etat FORMAT
 * PROJET entre projets compiles dans le MEME process.
 *
 * Execute via `owl --self-test-format-projet-leak` (cf main.c). Tourne sous
 * `make all` (batterie run_all_checks.sh), la ou la suite runtime `owl test`
 * — qui heberge le verrou d'integration a_/b_ — ne tourne PAS.
 *
 * Contexte du bug : les captures compile-time du bloc `est un Projet`
 * (FormatDate/Heure/Duree via g_projet_meta_fields[], FormatNombre/Monetaire)
 * vivent dans des globals statiques du compilateur. compiler_projet_boot_langue
 * _reset() les remet a zero et DOIT etre appele a chaque frontiere de projet
 * (project_run, test runner, serveur de check LSP). Un projet posant un format
 * ne doit pas leguer ce format au projet suivant qui n'en declare pas.
 *
 * Ce que ce harnais verrouille, en un seul process :
 *   1. Capture : un projet posant les 5 formats les rend lisibles par les
 *      getters compiler_projet_format_*.
 *   2. EXHAUSTIVITE du reset : apres compiler_projet_boot_langue_reset(), les 5
 *      formats retombent au defaut (linchpin du fix — si un futur format projet
 *      s'ajoute sans etre reset, ce test rougit).
 *   3. Non-residu cross-projet : apres reset, un 2e projet ne posant qu'un
 *      FormatDate different ne voit AUCUN residu des formats du 1er.
 *
 * Le verrou d'INTEGRATION (le test runner appelle bien le reset a la frontiere)
 * vit dans tests/runtime/projet_format_date_fuite_cross_projet/ (paire a_/b_,
 * sous `owl test`). Ce harnais-ci verrouille le CONTRAT du reset lui-meme.
 *
 * Retourne 0 si tout passe, sinon le nombre d'echecs. */

#include <stdio.h>
#include <string.h>

#include "../vm.h"
#include "../compiler.h"
#include "../prescan.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* str NULL-safe egalite. */
static bool streq(const char* a, const char* b) {
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

/* Compile une source de projet et roote la fonction produite pour la duree de
 * la lecture des getters (les ObjString captures vivent dans le pool de
 * constantes du chunk). Retourne la fonction (rootee : le caller depile), NULL
 * si la compile echoue. */
static ObjFunction* compile_projet(const char* source) {
    SymbolTable* symbols = symtable_create();
    prescan_file(symbols, source, 0);
    ObjFunction* fn = compile_in_project(source, "projet.owl", symbols, 0);
    symtable_destroy(symbols);
    if (fn != NULL) vm_push(OBJ_VAL(fn));  /* root pendant la lecture des getters */
    return fn;
}

/* Projet A : pose les 5 formats. "Durée"/"FormatDurée"/"FormatMonétaire" en
 * UTF-8 (e accent aigu = \xc3\xa9). */
static const char* SRC_PROJET_A =
    "AppFuiteA est un Projet\n"
    "\tPROPRIETES\n"
    "\t\tLangues         = [langueFran\xc3\xa7\x61is]\n"
    "\t\tFormatDate      = \"MM/JJ/AAAA\"\n"
    "\t\tFormatHeure     = \"HH-MM\"\n"
    "\t\tFormatDur\xc3\xa9""e     = \"HH:MM\"\n"
    "\t\tFormatNombre    = {ChiffresApr\xc3\xa8sVirgule = 3}\n"
    "\t\tFormatMon\xc3\xa9taire = {Symbole = \"$\", ChiffresApr\xc3\xa8sVirgule = 2}\n"
    "\tFIN\n"
    "FIN\n";

/* Projet B : ne pose QU'un FormatDate, distinct de A. */
static const char* SRC_PROJET_B =
    "AppFuiteB est un Projet\n"
    "\tPROPRIETES\n"
    "\t\tLangues    = [langueFran\xc3\xa7\x61is]\n"
    "\t\tFormatDate = \"AAAA-MM-JJ\"\n"
    "\tFIN\n"
    "FIN\n";

int format_projet_leak_self_test_run(void) {
    fprintf(stderr, "\n==== FORMAT PROJET LEAK SELF-TESTS (fix/2157) ====\n");

    /* --- 1. Capture : projet A rend ses 5 formats lisibles. --- */
    ObjFunction* fnA = compile_projet(SRC_PROJET_A);
    CHECK(fnA != NULL, "projet_A_compile");
    if (fnA != NULL) {
        CHECK(streq(compiler_projet_format_date(),  "MM/JJ/AAAA"), "A_format_date");
        CHECK(streq(compiler_projet_format_heure(), "HH-MM"),      "A_format_heure");
        CHECK(streq(compiler_projet_format_duree(), "HH:MM"),      "A_format_duree");

        int chiffres = -1;
        bool hasNb = compiler_projet_format_nombre(&chiffres, NULL, 0, NULL, 0);
        CHECK(hasNb && chiffres == 3, "A_format_nombre");

        char sym[16] = "";
        bool hasMn = compiler_projet_format_monetaire(sym, sizeof sym, NULL,
                                                      NULL, 0, NULL, 0, NULL, NULL);
        CHECK(hasMn && streq(sym, "$"), "A_format_monetaire");

        vm_pop();  /* fnA */
    }

    /* --- 2. Exhaustivite du reset : tout retombe au defaut. --- */
    compiler_projet_boot_langue_reset();
    CHECK(compiler_projet_format_date()  == NULL, "reset_date_null");
    CHECK(compiler_projet_format_heure() == NULL, "reset_heure_null");
    CHECK(compiler_projet_format_duree() == NULL, "reset_duree_null");
    CHECK(!compiler_projet_format_nombre(NULL, NULL, 0, NULL, 0), "reset_nombre_unset");
    CHECK(!compiler_projet_format_monetaire(NULL, 0, NULL, NULL, 0, NULL, 0, NULL, NULL),
          "reset_monetaire_unset");

    /* --- 3. Non-residu cross-projet : B (FormatDate seul) apres reset ne voit
     *        aucun residu des formats de A. C'est la fuite du ticket, cablee au
     *        niveau du contrat de capture. --- */
    ObjFunction* fnB = compile_projet(SRC_PROJET_B);
    CHECK(fnB != NULL, "projet_B_compile");
    if (fnB != NULL) {
        CHECK(streq(compiler_projet_format_date(), "AAAA-MM-JJ"), "B_format_date_propre");
        CHECK(compiler_projet_format_heure() == NULL, "B_heure_sans_residu_A");
        CHECK(compiler_projet_format_duree() == NULL, "B_duree_sans_residu_A");
        CHECK(!compiler_projet_format_nombre(NULL, NULL, 0, NULL, 0), "B_nombre_sans_residu_A");
        CHECK(!compiler_projet_format_monetaire(NULL, 0, NULL, NULL, 0, NULL, 0, NULL, NULL),
              "B_monetaire_sans_residu_A");
        vm_pop();  /* fnB */
    }

    /* Reset final : ne pas laisser d'etat pour le reste du process. */
    compiler_projet_boot_langue_reset();

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide : un controle qui n'examine rien ne prouve rien. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "format-projet-leak: CORPUS VIDE — 0 cas execute.\n");
        return 1;
    }
    return g_fail;
}
