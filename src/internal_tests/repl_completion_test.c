// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test de la complétion Tab du REPL.
 *
 * `repl_completion_cb` (src/main.c) alimentait la complétion Tab à partir de
 * 3 tableaux FIGÉS en dur (repl_keywords/repl_types/repl_natives, ~50 natives)
 * alors que le registre vivant en compte 600+. Conséquence : toute native
 * récente (dont DéclencheÉvénement/TriggerEvent) était introuvable à la
 * complétion, alors que `owl search`/`explain` la voyaient.
 *
 * Ce harnais exerce le chemin de génération des candidats — désormais branché
 * sur le registre vivant — via `repl_completion_enumerate`, qui découple la
 * production des candidats (registre) de leur consommation (owl_completion_add
 * en REPL, capture ici via un sink).
 *
 * vm_init() est requis : c'est lui qui peuple le registre (modules stdlib) et
 * vm.globals. Branché sur `owl --self-test-repl-completion` (cf src/main.c),
 * gate make all. Retour : 0 si tout passe, sinon le nombre d'échecs. */

#include "../vm.h"   /* vm_init / vm_free */

#include <stdio.h>
#include <string.h>

/* Exposé par src/main.c. Enumère les candidats de complétion pour
 * buf[0..bufLen) contre le registre vivant + mots-clés + globals de session. */
extern void repl_completion_enumerate(const char* buf, int bufLen,
                                      void (*sink)(const char* candidate, void* ctx),
                                      void* ctx);

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Capture des candidats émis pour une requête. */
#define CAP_MAX 4096
static char g_cands[CAP_MAX][256];
static int  g_cand_count = 0;

static void capture_sink(const char* candidate, void* ctx) {
    (void)ctx;
    if (g_cand_count < CAP_MAX) {
        snprintf(g_cands[g_cand_count], sizeof(g_cands[0]), "%s", candidate);
        g_cand_count++;
    }
}

/* Lance la complétion sur `buf` et remplit g_cands. */
static void complete(const char* buf) {
    g_cand_count = 0;
    repl_completion_enumerate(buf, (int)strlen(buf), capture_sink, NULL);
}

/* Un candidat exactement égal à `expected` a-t-il été émis ? */
static int has_exact(const char* expected) {
    for (int i = 0; i < g_cand_count; i++)
        if (strcmp(g_cands[i], expected) == 0) return 1;
    return 0;
}

/* Un candidat commençant par `prefix` a-t-il été émis ? */
static int has_prefix(const char* prefix) {
    size_t n = strlen(prefix);
    for (int i = 0; i < g_cand_count; i++)
        if (strncmp(g_cands[i], prefix, n) == 0) return 1;
    return 0;
}

int repl_completion_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    vm_init();

    /* 1. Native cible du ticket, forme ASCII sans-accent : `Declenche` doit
     * matcher le canonicalAscii "DeclencheEvenement" (+ suffixe "("). Absente
     * de l'ancienne liste figée -> c'est le coeur du fix. */
    complete("Declenche");
    CHECK(has_exact("DeclencheEvenement("),
          "prefixe ASCII 'Declenche' -> DeclencheEvenement(");

    /* 2. Forme EN : `Trigger` doit matcher le nameEN "TriggerEvent". */
    complete("Trigger");
    CHECK(has_exact("TriggerEvent("),
          "prefixe EN 'Trigger' -> TriggerEvent(");

    /* 3. Forme FR accentuée : `Déclenche` (UTF-8) doit matcher la forme
     * canonique accentuée "DéclencheÉvénement". Vérifie que la détection de
     * début de mot inclut bien les octets UTF-8. */
    complete("D\xc3\xa9""clenche");
    CHECK(has_exact("D\xc3\xa9""clenche\xc3\x89""v\xc3\xa9""nement("),
          "prefixe FR accentue 'Declenche' -> DeclencheEvenement canon (");

    /* 4. Native INTERNE (OWL_NAT_INTERNAL) NE doit PAS être proposée.
     * "__DebugFeuilleStyle" est un flag OWL_NAT_INTERNAL du module UI. */
    complete("__DebugFeuille");
    CHECK(g_cand_count == 0,
          "native interne __DebugFeuille absente de la completion");
    CHECK(!has_prefix("__Debug"),
          "aucun candidat prefixe __Debug (natives internes masquees)");

    /* 5. Reflet du registre vivant : une native quelconque hors ancienne liste
     * figée apparaît. `DateVersChaine` n'était pas dans repl_natives[]. */
    complete("DateVers");
    CHECK(has_prefix("DateVers"),
          "native hors liste figee (DateVers...) presente");

    /* 6. Type composé du registre : `httpReq` -> httpRequête (forme canon). */
    complete("httpReq");
    CHECK(has_prefix("httpReq"),
          "type composé httpReq... proposé (registre vivant)");

    /* 7. Type primitif : `entier` reste proposé (table primitifs). */
    complete("enti");
    CHECK(has_exact("entier"),
          "type primitif 'entier' proposé");

    /* 8. Mot-clé : `POU` -> POUR (source figée conservée, hors sujet natives). */
    complete("POU");
    CHECK(has_exact("POUR"),
          "mot-clé POUR proposé");

    /* 9. Préfixe avec contexte devant le mot : la partie précédant le mot
     * courant est bien conservée (ex. début d'expression). */
    complete("x = Declenche");
    CHECK(has_exact("x = DeclencheEvenement("),
          "prefixe conserve le contexte avant le mot courant");

    vm_free();

    fprintf(stdout, "repl-completion: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
