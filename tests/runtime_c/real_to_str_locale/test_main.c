/*  — verrou d'independance a LC_NUMERIC de la serialisation d'un
 * reel (owl_real_to_str, src/value.c).
 *
 * Pourquoi un harness C dedie et pas un scenario .owl : aucune native OWL
 * ne permet de forcer LC_NUMERIC a une locale precise depuis un script (la
 * seule occurrence de setlocale() du runtime lit le locale DEJA actif du
 * processus/OS via setlocale(LC_NUMERIC, "")). Sur une machine dont le
 * locale OS utilise deja le point decimal, un scenario .owl qui declenche
 * ce chemin ne reproduirait jamais la fuite de virgule — faux negatif
 * garanti. Seul un harness qui appelle setlocale() lui-meme, dans son
 * propre processus, peut forcer la condition et prouver la regression.
 *
 * Le corps de owl_real_to_str n'est PAS recopie a la main ici : run.sh
 * l'EXTRAIT tel quel depuis src/value.c (bornage sur la signature exacte
 * de la fonction) et l'appende a la suite de ce fichier avant compilation.
 * Ce fichier ne contient donc que le driver (locale switch + assertions),
 * jamais une reimplementation parallele qui pourrait diverger du code reel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <locale.h>

/* Definition reelle appendee plus bas par run.sh (extraite de
 * src/value.c). */
int owl_real_to_str(double v, char* buf, int bufSize);

typedef struct {
    double value;
    const char* expected;   /* NULL = pas de comparaison exacte (ex: notation
                                scientifique dont le format d'exposant varie
                                d'un libc a l'autre) — seule l'absence de
                                virgule et la presence d'un point sont
                                verifiees dans ce cas. */
} case_t;

int main(void) {
    /* Essaie plusieurs noms de locale a separateur decimal virgule, pour la
     * portabilite Windows/MinGW (testes empiriquement disponibles sur ce
     * runtime UCRT) et POSIX/glibc (suffixe .UTF-8 usuel). Le premier qui
     * bascule reellement localeconv()->decimal_point vers "," est retenu. */
    const char* candidates[] = {
        "fr_FR", "fr_FR.UTF-8", "fr_FR.utf8", "French_France.1252",
        "de_DE", "de_DE.UTF-8", "German_Germany.1252",
        NULL
    };
    const char* picked = NULL;
    for (int i = 0; candidates[i]; i++) {
        char* r = setlocale(LC_NUMERIC, candidates[i]);
        if (!r) continue;
        struct lconv* lc = localeconv();
        if (lc && lc->decimal_point && lc->decimal_point[0] == ',') {
            picked = candidates[i];
            break;
        }
        setlocale(LC_NUMERIC, "C");
    }

    if (!picked) {
        printf("SKIP: aucune locale a separateur decimal virgule "
               "(fr_FR/de_DE/...) disponible sur cette machine — test non "
               "concluant, PAS une preuve de non-regression.\n");
        return 77;
    }

    printf("locale virgule retenue : %s (decimal_point=\"%s\")\n",
           picked, localeconv()->decimal_point);

    case_t cases[] = {
        { 3.14,    "3.14" },
        { 0.5,     "0.5" },
        { -2.75,   "-2.75" },
        { 1234.5,  "1234.5" },
        { 42.0,    "42" },
        { 1e-7,    NULL },
    };
    int nb = (int)(sizeof(cases) / sizeof(cases[0]));
    int fails = 0;

    for (int i = 0; i < nb; i++) {
        char buf[64];
        owl_real_to_str(cases[i].value, buf, sizeof(buf));
        printf("owl_real_to_str(%.10g) -> \"%s\"\n", cases[i].value, buf);

        if (strchr(buf, ',') != NULL) {
            fprintf(stderr,
                "FAIL: \"%s\" contient une virgule sous locale %s "
                "(le separateur decimal doit toujours etre '.', quel que "
                "soit LC_NUMERIC)\n", buf, picked);
            fails++;
            continue;
        }

        if (cases[i].expected) {
            if (strcmp(buf, cases[i].expected) != 0) {
                fprintf(stderr,
                    "FAIL: owl_real_to_str(%.10g) = \"%s\", attendu \"%s\"\n",
                    cases[i].value, buf, cases[i].expected);
                fails++;
            }
        } else if (strchr(buf, '.') == NULL &&
                   strchr(buf, 'e') == NULL && strchr(buf, 'E') == NULL) {
            fprintf(stderr,
                "FAIL: \"%s\" ne contient ni point decimal ni notation "
                "scientifique\n", buf);
            fails++;
        }
    }

    setlocale(LC_NUMERIC, "C");

    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s)) — fuite de la virgule locale "
               "dans owl_real_to_str sous %s\n", fails, picked);
        return 1;
    }
    printf("RESULT: GREEN (owl_real_to_str reste en point decimal sous "
           "%s)\n", picked);
    return 0;
}
