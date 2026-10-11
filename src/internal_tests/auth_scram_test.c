// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test SCRAM-SHA-256 client.
 *
 * Vecteurs de référence : RFC 7677 section 3 (exemple complet avec
 * username/password connus). Comme tous les paramètres (nonce, salt,
 * iteration count) sont fixes, on peut vérifier byte-par-byte chaque
 * étape sans dépendance réseau.
 *
 *   user = "user"
 *   pass = "pencil"
 *   client-nonce = "rOprNGfwEbeRWgbNEkqO"
 *   salt (b64)   = "W22ZaJ0SNY7soEsUEjb6gQ=="
 *   iter         = 4096
 *
 * Sorties attendues :
 *   ClientProof  (b64) = "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="
 *   ServerSig    (b64) = "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="
 */

#include "auth_scram.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define SCRAM_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

#define SCRAM_CHECK_STR(got, expected, label) do { \
    const char* _g = (const char*)(got); \
    bool _ok = _g && strcmp(_g, (expected)) == 0; \
    if (_ok) { g_pass++; } \
    else { g_fail++; \
        fprintf(stderr, "  FAIL: %s\n    got: '%s'\n    exp: '%s'\n", \
                label, _g ? _g : "(null)", (expected)); } \
} while (0)

/* RFC 7677 §3 — vecteur de test officiel. */
static const char* RFC_USER          = "user";
static const char* RFC_PASS          = "pencil";
static const char* RFC_CLIENT_NONCE  = "rOprNGfwEbeRWgbNEkqO";
static const char* RFC_SERVER_FIRST  = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
static const char* RFC_CLIENT_FINAL_EXPECTED =
    "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
    "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
static const char* RFC_SERVER_FINAL  = "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

static void test_rfc7677_vector(void) {
    OwlScramSha256 s;
    owl_scram_init(&s, RFC_USER, RFC_PASS);

    /* Étape 1 : client-first */
    char first[256];
    bool ok = owl_scram_client_first(&s, RFC_CLIENT_NONCE, first, sizeof(first));
    SCRAM_CHECK(ok, "client_first OK");
    SCRAM_CHECK_STR(first, "n,,n=user,r=rOprNGfwEbeRWgbNEkqO", "client-first message");

    /* Étape 2 : client-final */
    char final[512];
    ok = owl_scram_client_final(&s, RFC_SERVER_FIRST, final, sizeof(final));
    SCRAM_CHECK(ok, "client_final OK");
    SCRAM_CHECK_STR(final, RFC_CLIENT_FINAL_EXPECTED, "client-final message + ClientProof");

    /* Étape 3 : verify server-final */
    ok = owl_scram_verify_server_final(&s, RFC_SERVER_FINAL);
    SCRAM_CHECK(ok, "verify_server_final OK (ServerSignature matches)");
}

static void test_replay_protection(void) {
    /* Si le serveur retourne une combined-nonce qui ne commence pas par
     * notre client-nonce, c'est probablement un replay attack. Rejeter. */
    OwlScramSha256 s;
    owl_scram_init(&s, RFC_USER, RFC_PASS);
    char first[256];
    owl_scram_client_first(&s, RFC_CLIENT_NONCE, first, sizeof(first));

    /* Server tente de renvoyer un nonce sans notre prefix. */
    const char* bad_server_first =
        "r=ATTACKERnonceXXXXXXXX,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    char final[256];
    bool ok = owl_scram_client_final(&s, bad_server_first, final, sizeof(final));
    SCRAM_CHECK(!ok, "client_final rejette nonce non-prefixe (replay protection)");
    SCRAM_CHECK(strstr(owl_scram_last_error(&s), "client-nonce") != NULL,
                 "last_error mentionne client-nonce");
}

static void test_wrong_server_sig(void) {
    /* Server final avec signature alteree → verify doit echouer. */
    OwlScramSha256 s;
    owl_scram_init(&s, RFC_USER, RFC_PASS);
    char first[256], final[512];
    owl_scram_client_first(&s, RFC_CLIENT_NONCE, first, sizeof(first));
    owl_scram_client_final(&s, RFC_SERVER_FIRST, final, sizeof(final));

    /* Sig alteree (1 char modifie). */
    const char* bad_sf = "v=AAAATRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";
    bool ok = owl_scram_verify_server_final(&s, bad_sf);
    SCRAM_CHECK(!ok, "verify rejette signature alteree");
}

static void test_random_nonce_generation(void) {
    /* Sans override, owl_scram_client_first doit generer un nonce
     * de >= 16 chars, ASCII printable, different a chaque appel. */
    OwlScramSha256 s1, s2;
    owl_scram_init(&s1, "alice", "pw");
    owl_scram_init(&s2, "alice", "pw");
    char m1[256], m2[256];
    SCRAM_CHECK(owl_scram_client_first(&s1, NULL, m1, sizeof(m1)),
                 "client_first auto-nonce 1 OK");
    SCRAM_CHECK(owl_scram_client_first(&s2, NULL, m2, sizeof(m2)),
                 "client_first auto-nonce 2 OK");
    SCRAM_CHECK(strlen(s1.client_nonce) >= 16, "nonce >= 16 chars");
    SCRAM_CHECK(strcmp(s1.client_nonce, s2.client_nonce) != 0,
                 "deux nonces successifs different");
    /* Verifie ASCII printable + pas de ',' ou '=' qui casseraient le format. */
    for (size_t i = 0; i < strlen(s1.client_nonce); i++) {
        char c = s1.client_nonce[i];
        if (c == ',' || c == '=' || c < 0x21 || c > 0x7E) {
            SCRAM_CHECK(false, "nonce contient un caractere illegal");
            return;
        }
    }
    SCRAM_CHECK(true, "nonce ne contient pas ',' ou '=' ou non-printable");
}

int auth_scram_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_rfc7677_vector();
    test_replay_protection();
    test_wrong_server_sig();
    test_random_nonce_generation();

    printf("auth_scram self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
