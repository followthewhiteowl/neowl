// SPDX-License-Identifier: AGPL-3.0-only
/* multilingue_print_test.c —  : affichage d'une chaineMultilingue
 * resout la langue courante au lieu de dumper le dict brut.
 *
 * Execute via `owl --self-test-multilingue-print` (cf main.c). Verrouille le
 * chemin `object_print` (Trace -> value_print -> object_print case
 * OBJ_MULTILINGUE) que le harness de scenarios .owl ne peut PAS capturer
 * (stdout non capture par le runner ; Info/Erreur early-return en headless).
 *
 * Pattern : vm_interpret(source) (cf. vm_dispatch_test.c) + capture stdout via
 * dup/dup2/fichier temp (cf. cli_doc_test.c::capture_call).
 *
 * **TEST-FIRST (digue rouge-avant-vert)** : pose en commit ROUGE avant l'impl.
 * Sur le code actuel, Trace(m) dumpe `{"fr-FR": "Salut", "en-US": "Hi"}` ;
 * l'assertion "sortie == Salut, PAS le dict" echoue -> preuve que le test
 * capte le vrai bug. L'impl (object.c) le passe au vert.
 *
 * Cas couverts :
 *   1. Langue defaut (fr-FR)  -> Trace(m) imprime "Salut" (pas le dict)
 *   2. Apres Langue("en-US")  -> Trace(m) imprime "Hi"
 *   3. Fallback : langue courante absente du dict -> langue source ("fr-FR")
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../vm.h"
#include "fixture_unique.h"

#ifdef _WIN32
  #include <io.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

static int  g_pass = 0;
static int  g_fail = 0;
static char g_buf[16 * 1024];

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Capture stdout dans g_buf le temps d'un vm_interpret(source). Pattern
 * dup/dup2/fichier temp (cf. cli_doc_test.c::capture_call) — portable
 * MinGW/MSVCRT, evite freopen() qui invalide le FILE* d'origine. */
static InterpretResult capture_interpret(const char* source) {
    /* Nom propre au processus : a nom constant, deux executions
     * simultanees se tronquaient la capture -> "defaut_resout_fr" rouge alors
     * que object_print n'a rien fait de mal. */
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_multilingue_print_test_", ".tmp");
    g_buf[0] = '\0';

    fflush(stdout);
    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return INTERPRET_RUNTIME_ERROR;

    int tmp_fd = owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (tmp_fd < 0) { owl_close(saved_fd); return INTERPRET_RUNTIME_ERROR; }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd); owl_close(saved_fd);
        return INTERPRET_RUNTIME_ERROR;
    }
    owl_close(tmp_fd);

    InterpretResult res = vm_interpret(source);
    fflush(stdout);

    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    FILE* r = fopen(TMP, "rb");
    if (r) {
        size_t n = fread(g_buf, 1, sizeof(g_buf) - 1, r);
        g_buf[n] = '\0';
        fclose(r);
    }
    owl_unlink(TMP);
    return res;
}

/* "Salut" et "Hi" en UTF-8 sont ASCII pur — pas d'echappement requis.
 * Le dict brut commence par `{"` : sa presence = bug pas corrige. */
static const char* SRC_DEFAUT =
    "Langue(\"fr-FR\")\n"
    "m est une cha\xc3\xae" "neMultilingue = {langueFran\xc3\xa7" "ais: \"Salut\", langueAnglais: \"Hi\"}\n"
    "Trace(m)\n";

static const char* SRC_EN =
    "Langue(\"en-US\")\n"
    "m est une cha\xc3\xae" "neMultilingue = {langueFran\xc3\xa7" "ais: \"Salut\", langueAnglais: \"Hi\"}\n"
    "Trace(m)\n";

/* Langue courante "de-DE" absente du dict -> fallback langue source fr-FR. */
static const char* SRC_FALLBACK =
    "Langue(\"de-DE\")\n"
    "m est une cha\xc3\xae" "neMultilingue = {langueFran\xc3\xa7" "ais: \"Salut\", langueAnglais: \"Hi\"}\n"
    "Trace(m)\n";

static void test_defaut_resout_langue_courante(void) {
    InterpretResult res = capture_interpret(SRC_DEFAUT);
    CHECK(res == INTERPRET_OK, "defaut_interpret_ok");
    /* Le bug : dump du dict brut. Le contrat : la resolution. */
    CHECK(strstr(g_buf, "{\"") == NULL, "defaut_pas_de_dict_brut");
    CHECK(strstr(g_buf, "Salut") != NULL, "defaut_resout_fr");
    CHECK(strstr(g_buf, "Hi") == NULL, "defaut_pas_autre_langue");
}

static void test_apres_switch_langue(void) {
    InterpretResult res = capture_interpret(SRC_EN);
    CHECK(res == INTERPRET_OK, "en_interpret_ok");
    CHECK(strstr(g_buf, "{\"") == NULL, "en_pas_de_dict_brut");
    CHECK(strstr(g_buf, "Hi") != NULL, "en_resout_en");
}

static void test_fallback_langue_source(void) {
    InterpretResult res = capture_interpret(SRC_FALLBACK);
    CHECK(res == INTERPRET_OK, "fallback_interpret_ok");
    CHECK(strstr(g_buf, "{\"") == NULL, "fallback_pas_de_dict_brut");
    /* de-DE absent -> retombe sur la langue source fr-FR -> "Salut". */
    CHECK(strstr(g_buf, "Salut") != NULL, "fallback_vers_source");
}

int multilingue_print_test_main(void) {
    fprintf(stderr, "\n==== MULTILINGUE PRINT TESTS (fix/0640) ====\n");

    test_defaut_resout_langue_courante();
    test_apres_switch_langue();
    test_fallback_langue_source();

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "multilingue-print: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }

    return g_fail;
}
