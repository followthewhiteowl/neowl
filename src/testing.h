// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_testing_h
#define owl_testing_h

#include "common.h"

/* Register test/assertion native functions (called from vm_init). */
void testing_register_natives(void);

/* Verbosity levels:
 * 0 (compact)  : résumé par fichier uniquement (✓/✗), détails des FAIL
 * 1 (normal)   : idem + nom de chaque assertion qui FAIL
 * 2 (verbose)  : chaque assertion PASS/FAIL
 * -1 (quiet)   : seulement les erreurs, pas de résumé intermédiaire */
void testing_set_verbosity(int level);

/* Test mode flags (canon).
 * EnModeTestUnitaire() : le code tourne via `owl test` (test runner OWL).
 * Le runtime n'altere rien automatiquement — c'est au dev de detecter et
 * adapter (skip dialogs bloquantes, mocker, etc.). */
void testing_set_unitaire(bool active);
bool testing_is_unitaire(void);

/* Access assertion counters. */
int testing_get_pass_count(void);
int testing_get_fail_count(void);
void testing_reset(void);

/* Test runner options. */
typedef struct {
    const char* dir;          /* Directory to scan for test_*.owl files */
    const char* singleFile;   /* If non-NULL, only run this specific file (basename) */
    int verbosity;            /* -1=quiet, 0=compact, 1=normal, 2=verbose */
    bool failFast;            /* Stop at first test failure */
    const char* filter;       /* If non-NULL, only run tests matching this substring */
    bool jsonOutput;          /* Output results as JSON */
    bool strict;              /* --strict : warnings deviennent failures (exit 2) */
    bool listFiles;           /* --list-files : log fichiers découverts + compilés/exécutés (debug autodiscovery) */
} TestOptions;

/* Run test suites. Returns 0 if all pass, 1 if any fail. */
int run_tests_ex(const TestOptions* opts);

#endif
