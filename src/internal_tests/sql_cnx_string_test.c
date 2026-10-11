// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test des assembleurs de chaines de connexion SQL (src/sql_cnx_string.c).
 *
 * Aucun serveur HFSQL n'est disponible en integration continue : la seule
 * chose verifiable est la chaine produite, et elle n'est verifiable que parce
 * que l'assemblage a ete sorti dans une fonction pure. Les cas ci-dessous
 * assertent la chaine EXACTE, pas une sous-chaine.
 *
 * Le format lui-meme (Server Name / Server Port) vient d'une documentation
 * externe du driver, pas d'une source executable ici : ces tests verrouillent
 * la coherence interne et la regle de preseance du port, pas la conformite du
 * driver. */

#include "sql_cnx_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Compare la chaine assemblee a l'attendu, octet pour octet. */
static void check_cnx(const char* serveur, int64_t port_champ,
                      const char* base, const char* user, const char* pwd,
                      const char* attendu, const char* label) {
    char* got = owl_sql_build_hfsql_cnx_string(serveur, port_champ,
                                               base, user, pwd);
    if (got == NULL) {
        g_fail++;
        fprintf(stderr, "  FAIL: %s -> NULL\n", label);
        return;
    }
    if (strcmp(got, attendu) == 0) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL: %s\n    attendu : %s\n    obtenu  : %s\n",
                label, attendu, got);
    }
    free(got);
}

/* ================================================================
 * Les cinq cas de preseance du port
 * ================================================================ */

/* 1. Port explicite seul : il doit apparaitre dans la chaine. */
static void test_port_explicite_seul(void) {
    check_cnx("monserveur", 4901, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4901;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port explicite 4901 pris en compte");
}

/* 2. Port explicite contre port dans Serveur : l'explicite gagne. */
static void test_port_explicite_gagne(void) {
    check_cnx("monserveur:4902", 4901, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4901;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port explicite l'emporte sur celui de Serveur");
}

/* 3. Pas de port explicite : il se deduit de Serveur. */
static void test_port_deduit_du_serveur(void) {
    check_cnx("monserveur:4902", 0, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4902;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port deduit de Serveur (4902)");
}

/* 4. Ni l'un ni l'autre : defaut HFSQL 4900 (comportement historique). */
static void test_port_defaut_4900(void) {
    check_cnx("monserveur", 0, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4900;"
              "Database=madb;UID=moi;PWD=secret;",
              "Defaut HFSQL 4900");
}

/* 5. Port hors bornes : ignore en silence, comme chez les trois autres
 *    connecteurs reseau. La source de repli reste Serveur, puis 4900. */
static void test_port_hors_bornes_ignore(void) {
    check_cnx("monserveur", 70000, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4900;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port 70000 ignore -> defaut 4900");
    check_cnx("monserveur", 65536, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4900;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port 65536 ignore (borne haute exclue)");
    check_cnx("monserveur:4902", -1, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4902;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port negatif ignore -> repli sur Serveur");
    check_cnx("monserveur", 1, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=1;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port 1 accepte (borne basse incluse)");
    check_cnx("monserveur", 65535, "madb", "moi", "secret",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=65535;"
              "Database=madb;UID=moi;PWD=secret;",
              "Port 65535 accepte (borne haute incluse)");
}

/* ================================================================
 * Segments optionnels — non-regression de l'assemblage
 * ================================================================ */

static void test_segments_optionnels(void) {
    check_cnx("", 4901, "madb", "", "",
              "DRIVER={HFSQL};Database=madb;",
              "Serveur vide -> ni Server Name ni Server Port");
    check_cnx("monserveur", 4901, NULL, NULL, NULL,
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4901;",
              "Champs NULL omis");
    check_cnx("monserveur", 0, "madb", "moi", "",
              "DRIVER={HFSQL};Server Name=monserveur;Server Port=4900;"
              "Database=madb;UID=moi;",
              "Mot de passe vide omis");
    check_cnx("[::1]", 4901, "madb", NULL, NULL,
              "DRIVER={HFSQL};Server Name=::1;Server Port=4901;Database=madb;",
              "IPv6 crochete : crochets retires, port explicite conserve");
}

/* ================================================================
 * Brique partagee : resolution hote/port
 * ================================================================ */

static void check_resolve(const char* serveur, int64_t port_champ,
                          int port_defaut,
                          const char* hote_attendu, int port_attendu,
                          const char* label) {
    char hote[256];
    int port = owl_sql_resolve_host_port(serveur, port_champ, port_defaut,
                                         hote, sizeof(hote));
    if (port == port_attendu && strcmp(hote, hote_attendu) == 0) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL: %s\n    attendu : %s / %d\n"
                        "    obtenu  : %s / %d\n",
                label, hote_attendu, port_attendu, hote, port);
    }
}

static void test_resolve_host_port(void) {
    check_resolve("srv", 0, 5432, "srv", 5432, "resolve: defaut backend");
    check_resolve("srv:6000", 0, 5432, "srv", 6000, "resolve: port dans serveur");
    check_resolve("srv:6000", 7000, 5432, "srv", 7000, "resolve: champ prioritaire");
    check_resolve("srv", 7000, 5432, "srv", 7000, "resolve: champ seul");
    check_resolve("srv", 0, 4900, "srv", 4900, "resolve: defaut HFSQL");
    check_resolve("srv:abc", 0, 5432, "srv:abc", 5432,
                  "resolve: suffixe non numerique n'est pas un port");
    check_resolve("[::1]:6000", 0, 5432, "::1", 6000, "resolve: IPv6 crochete");
    check_resolve("", 0, 4900, "", 4900, "resolve: serveur vide");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int sql_cnx_string_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_port_explicite_seul();
    test_port_explicite_gagne();
    test_port_deduit_du_serveur();
    test_port_defaut_4900();
    test_port_hors_bornes_ignore();
    test_segments_optionnels();
    test_resolve_host_port();

    printf("sql_cnx_string self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
