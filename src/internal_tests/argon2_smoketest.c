// SPDX-License-Identifier: AGPL-3.0-only
/* argon2_smoketest.c — smoke test du vendoring argon2 20190702 (CC0 / Apache-2.0).
 *
 * But : valider que la brique de hachage de mot de passe vendoree compile,
 * linke et produit des tags CONFORMES sur la toolchain OWL (host gcc + cross
 * zig), AVANT que des natives OWL la consomment.
 *
 * Ici aucun wrapping OWL : la lib tourne sur son propre malloc/free, on ne
 * touche que l'API haut niveau (argon2id_hash_encoded / argon2id_hash_raw /
 * argon2id_verify). Sanity check isole, sans framework (pattern
 * zint_smoketest / mpdecimal_smoketest).
 *
 * Verifs (exit code 0 si tout passe, != 0 + message sinon) :
 *   1. KAT officiel argon2id v=19, t=2, m=65536, p=1, outlen=32 -> tag
 *      encode attendu. Vecteur repris tel quel du src/test.c upstream
 *      (non vendore) : preuve de conformite de l'implementation vendoree.
 *   2. KAT officiel avec p=2 : ARGON2_NO_THREADS ne doit PAS changer le tag
 *      (lanes traitees en serie et non en parallele, resultat identique).
 *      C'est ce qui justifie le define cote CMake.
 *   3. Parametres OWASP (m=19456 KiB, t=2, p=1) : hash + verify OK, et
 *      verify d'un mauvais mot de passe -> ARGON2_VERIFY_MISMATCH.
 *   4. Encodage refuse : chaine tronquee -> jamais ARGON2_OK.
 */

#include <stdio.h>
#include <string.h>
#include "argon2.h"

#define SALT "somesalt"

/* Vecteurs officiels upstream (src/test.c de phc-winner-argon2, non vendore) :
   argon2id v=19, pwd="password", salt="somesalt", outlen=32. */
static const char *KAT_M65536_T2_P1 =
    "$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHQ"
    "$CTFhFdXPJO1aFaMaO6Mm5c8y7cJHAph8ArZWb2GRPPc";
static const char *KAT_M256_T2_P2 =
    "$argon2id$v=19$m=256,t=2,p=2$c29tZXNhbHQ"
    "$bQk8UB/VmZZF4Oo79iDXuL5/0ttZwg2f/5U52iv1cDc";

static int check_kat(uint32_t m_cost, uint32_t lanes, const char *expected,
                     int base_code) {
    const char *pwd = "password";
    char enc[256];

    int rc = argon2id_hash_encoded(2, m_cost, lanes,
                                   pwd, strlen(pwd),
                                   SALT, strlen(SALT),
                                   32, enc, sizeof enc);
    if (rc != ARGON2_OK) {
        fprintf(stderr, "argon2_smoketest: hash_encoded(m=%u,p=%u) rc=%d (%s)\n",
                m_cost, lanes, rc, argon2_error_message(rc));
        return base_code + 1;
    }
    if (strcmp(enc, expected) != 0) {
        fprintf(stderr, "argon2_smoketest: KAT m=%u p=%u divergent\n"
                        "  attendu : %s\n  obtenu  : %s\n",
                m_cost, lanes, expected, enc);
        return base_code + 2;
    }
    rc = argon2id_verify(enc, pwd, strlen(pwd));
    if (rc != ARGON2_OK) {
        fprintf(stderr, "argon2_smoketest: verify(m=%u,p=%u) rc=%d (%s)\n",
                m_cost, lanes, rc, argon2_error_message(rc));
        return base_code + 3;
    }
    printf("  KAT argon2id m=%u t=2 p=%u OK\n", m_cost, lanes);
    return 0;
}

/* Parametres OWASP 2026 pour Argon2id : m=19456 KiB (19 MiB), t=2, p=1. */
static int check_owasp(void) {
    const char *pwd = "correct horse battery staple";
    const char *bad = "correct horse battery stapl";
    char enc[256];

    int rc = argon2id_hash_encoded(2, 19456, 1,
                                   pwd, strlen(pwd),
                                   SALT, strlen(SALT),
                                   32, enc, sizeof enc);
    if (rc != ARGON2_OK) {
        fprintf(stderr, "argon2_smoketest: OWASP hash rc=%d (%s)\n",
                rc, argon2_error_message(rc));
        return 31;
    }
    if (argon2id_verify(enc, pwd, strlen(pwd)) != ARGON2_OK) {
        fprintf(stderr, "argon2_smoketest: OWASP verify du bon mdp a echoue\n");
        return 32;
    }
    rc = argon2id_verify(enc, bad, strlen(bad));
    if (rc != ARGON2_VERIFY_MISMATCH) {
        fprintf(stderr, "argon2_smoketest: mauvais mdp accepte ou mauvais "
                        "code (rc=%d, attendu %d)\n", rc, ARGON2_VERIFY_MISMATCH);
        return 33;
    }
    printf("  OWASP m=19456 t=2 p=1 OK (verify + mismatch)\n");
    printf("  %s\n", enc);
    return 0;
}

/* Une chaine encodee tronquee ne doit jamais valider. */
static int check_bad_encoding(void) {
    const char *truncated = "$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHQ";
    int rc = argon2id_verify(truncated, "password", 8);
    if (rc == ARGON2_OK) {
        fprintf(stderr, "argon2_smoketest: encodage tronque VALIDE (faille)\n");
        return 41;
    }
    printf("  encodage tronque rejete (rc=%d)\n", rc);
    return 0;
}

int main(void) {
    int rc;

    rc = check_kat(65536, 1, KAT_M65536_T2_P1, 10);
    if (rc) return rc;

    /* p=2 sous ARGON2_NO_THREADS : meme tag que la version multi-thread. */
    rc = check_kat(256, 2, KAT_M256_T2_P2, 20);
    if (rc) return rc;

    rc = check_owasp();
    if (rc) return rc;

    rc = check_bad_encoding();
    if (rc) return rc;

    printf("argon2_smoketest OK (argon2id, ref.c, ARGON2_NO_THREADS)\n");
    return 0;
}
