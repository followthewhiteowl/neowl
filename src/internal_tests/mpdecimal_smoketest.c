// SPDX-License-Identifier: AGPL-3.0-only
/* mpdecimal_smoketest.c — smoke test du vendoring mpdecimal 4.0.1.
 *
 * But : valider que la brique bignum decimal vendoree compile, linke et
 * fonctionne sur la toolchain OWL (host + cross zig), AVANT que le modele
 * de nombre v2 la consomme via un OBJ_NUMERIC wrappe au GC.
 *
 * Ici aucun wrapping OWL : la lib tourne sur malloc/free standard, etat
 * global fige une seule fois localement. C'est juste un sanity check du
 * vendoring, isole, sans framework de test (pattern zstd_smoketest).
 *
 * Verifs (exit code 0 si tout passe, !=0 + message sinon) :
 *   1. mpd_version() == "4.0.1"
 *   2. 1/3 a precision 50 : exactitude arbitraire (la ou un double se vautre)
 *   3. 0.1 + 0.2 == 0.3 EXACT (0.30000000000000004 en binaire flottant)
 *   4. 10^40 stocke exact (hors portee int64 et double)
 *   5. conversion vers int64 avec detection d'overflow via status flag
 *      (INT64_MAX round-trip OK ; 45 digits -> MPD_Invalid_operation leve)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "mpdecimal.h"

int main(void) {
    /* 1) Version : on attend exactement 4.0.1. */
    const char *ver = mpd_version();
    if (strcmp(ver, "4.0.1") != 0) {
        fprintf(stderr, "mpdecimal_smoketest: mpd_version()=%s, attendu 4.0.1\n", ver);
        return 1;
    }

    mpd_context_t ctx;
    mpd_init(&ctx, 50);                 /* 50 chiffres significatifs */

    mpd_t *one   = mpd_new(&ctx);
    mpd_t *three = mpd_new(&ctx);
    mpd_t *r     = mpd_new(&ctx);
    if (!one || !three || !r) {
        fprintf(stderr, "mpdecimal_smoketest: mpd_new a echoue (OOM ?)\n");
        return 2;
    }

    /* 2) 1/3 @ precision 50 : 0.333...3 (50 chiffres). */
    mpd_set_string(one,   "1", &ctx);
    mpd_set_string(three, "3", &ctx);
    mpd_div(r, one, three, &ctx);
    {
        char *s = mpd_to_sci(r, 1);
        /* 50 chiffres significatifs : "0." + 50x '3'. mpd_to_sci rend la
           forme decimale exacte attendue. */
        const char *expected = "0.33333333333333333333333333333333333333333333333333";
        if (strcmp(s, expected) != 0) {
            fprintf(stderr, "mpdecimal_smoketest: 1/3@50 = %s\n  attendu: %s\n", s, expected);
            mpd_free(s);
            return 3;
        }
        mpd_free(s);
    }

    /* 3) 0.1 + 0.2 == 0.3 EXACT. */
    {
        mpd_t *a = mpd_new(&ctx);
        mpd_t *b = mpd_new(&ctx);
        mpd_set_string(a, "0.1", &ctx);
        mpd_set_string(b, "0.2", &ctx);
        mpd_add(r, a, b, &ctx);
        char *s = mpd_to_sci(r, 1);
        if (strcmp(s, "0.3") != 0) {
            fprintf(stderr, "mpdecimal_smoketest: 0.1+0.2 = %s, attendu 0.3\n", s);
            mpd_free(s); mpd_del(a); mpd_del(b);
            return 4;
        }
        mpd_free(s); mpd_del(a); mpd_del(b);
    }

    /* 4) 10^40 stocke exact (> 10^38, hors int64/double). */
    {
        mpd_context_t big;
        mpd_maxcontext(&big);
        mpd_t *p    = mpd_new(&big);
        mpd_t *prod = mpd_new(&big);
        mpd_set_string(p, "100000000000000000000", &big);  /* 10^20 */
        mpd_mul(prod, p, p, &big);                          /* 10^40 */
        char *s = mpd_to_sci(prod, 1);
        /* maxcontext : exposant dans la plage -> forme entiere plate (1 + 40 zeros). */
        const char *expected = "10000000000000000000000000000000000000000";
        if (strcmp(s, expected) != 0) {
            fprintf(stderr, "mpdecimal_smoketest: 10^40 = %s\n  attendu: %s\n", s, expected);
            mpd_free(s); mpd_del(p); mpd_del(prod);
            return 5;
        }
        mpd_free(s); mpd_del(p); mpd_del(prod);
    }

    /* 5) conversion int64 + detection overflow via status flag. */
    {
        uint32_t status = 0;
        mpd_t *small = mpd_new(&ctx);
        mpd_set_i64(small, 9223372036854775807LL, &ctx);   /* INT64_MAX */
        int64_t back = mpd_qget_i64(small, &status);
        if (status != 0 || back != 9223372036854775807LL) {
            fprintf(stderr, "mpdecimal_smoketest: i64 round-trip INT64_MAX = %" PRId64
                            " status=0x%08x (attendu round-trip propre)\n", back, status);
            mpd_del(small);
            return 6;
        }

        /* 45 chiffres -> doit lever Invalid_operation, PAS de valeur fausse silencieuse. */
        mpd_context_t big;
        mpd_maxcontext(&big);
        mpd_t *huge = mpd_new(&big);
        mpd_set_string(huge, "123456789012345678901234567890123456789012345", &big);
        status = 0;
        (void)mpd_qget_i64(huge, &status);
        if (!(status & MPD_Invalid_operation)) {
            fprintf(stderr, "mpdecimal_smoketest: overflow i64 NON detecte (status=0x%08x)\n", status);
            mpd_del(small); mpd_del(huge);
            return 7;
        }
        mpd_del(small); mpd_del(huge);
    }

    mpd_del(one); mpd_del(three); mpd_del(r);

    printf("mpdecimal_smoketest OK (version %s, prec 50, 0.1+0.2=0.3 exact, 10^40 exact, overflow i64 detecte)\n", ver);
    return 0;
}
