/*  — verrou de la detection de collision d'offset sentinelle.
 *
 * OWL_SENTINEL_OFFSET(n) = INT32_MIN + n*0x100 est une fonction pure de n :
 * deux bases '#define OWL_X_BASE OWL_SENTINEL_OFFSET(n)' au meme n produisent
 * des valeurs int32 identiques (collision bit-a-bit entre familles de
 * constantes). La regle de self-test no_duplicate_sentinel_offset scanne le
 * source C pour la detecter. Le coeur de detection vit dans
 * owl_sentinel_scan_duplicates (src/registry_self_test.c).
 *
 * Pourquoi un harnais C dedie et pas un scenario .owl : la detection opere
 * sur du TEXTE source C, pas sur le registre runtime. Aucune native OWL ne
 * donne acces a cette fonction. On ne peut pas non plus prouver "la regle
 * attrape une collision" en fabriquant une vraie collision dans src/ (elle
 * ferait rougir --self-test-registry pour de bon). Le harnais alimente la
 * fonction avec des buffers SYNTHETIQUES et verifie qu'elle rapporte (ou non)
 * les doublons attendus.
 *
 * Le corps d'owl_sentinel_scan_duplicates n'est PAS recopie ici : run.sh
 * l'EXTRAIT tel quel depuis src/registry_self_test.c (bornage sur la
 * signature exacte) et l'appende a la suite de ce fichier avant compilation.
 * Ce fichier ne contient que le driver + la copie du contrat (struct/macro)
 * que la fonction extraite reference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Contrat partage avec src/registry_self_test.c. Copie ici car l'extraction
 * ne prend que le corps de la fonction, pas le typedef qui la precede. Si le
 * contrat derive cote source, la compilation du harnais casse bruyamment
 * (exit 2 infra) — pas de faux vert silencieux. */
#define OWL_SENTINEL_MAX_DUP 64
typedef struct {
    int count;
    int overflow;
    int offset[OWL_SENTINEL_MAX_DUP];
    char nameA[OWL_SENTINEL_MAX_DUP][96];
    char nameB[OWL_SENTINEL_MAX_DUP][96];
} OwlSentinelDupReport;

/* Definition reelle appendee plus bas par run.sh (extraite du source). */
void owl_sentinel_scan_duplicates(const char* text, OwlSentinelDupReport* rep);

static int fails = 0;

static int has_dup(const OwlSentinelDupReport* rep, int offset,
                   const char* a, const char* b) {
    for (int i = 0; i < rep->count; i++) {
        if (rep->offset[i] != offset) continue;
        /* L'ordre premier/second depend de l'ordre d'apparition dans le texte. */
        if ((strcmp(rep->nameA[i], a) == 0 && strcmp(rep->nameB[i], b) == 0) ||
            (strcmp(rep->nameA[i], b) == 0 && strcmp(rep->nameB[i], a) == 0))
            return 1;
    }
    return 0;
}

static void check_count(const char* label, const OwlSentinelDupReport* rep,
                        int expected) {
    if (rep->count != expected) {
        fprintf(stderr, "FAIL [%s]: count=%d, attendu %d\n",
                label, rep->count, expected);
        fails++;
    } else {
        printf("OK   [%s]: count=%d\n", label, rep->count);
    }
}

int main(void) {
    OwlSentinelDupReport rep;

    /* 1. Collision cross-famille (la classe du bug : OWL_CONTINENT_BASE vs
     *    OWL_LOG_BASE tous deux a l'offset 108). La regle DOIT la rapporter. */
    {
        const char* text =
            "#define OWL_LOG_BASE    OWL_SENTINEL_OFFSET(108)\n"
            "#define OWL_CONTINENT_BASE   OWL_SENTINEL_OFFSET(108)\n";
        owl_sentinel_scan_duplicates(text, &rep);
        check_count("collision 108", &rep, 1);
        if (!has_dup(&rep, 108, "OWL_LOG_BASE", "OWL_CONTINENT_BASE")) {
            fprintf(stderr, "FAIL [collision 108]: doublon 108 (LOG/CONTINENT) "
                            "non rapporte\n");
            fails++;
        } else {
            printf("OK   [collision 108]: doublon 108 LOG/CONTINENT rapporte\n");
        }
    }

    /* 2. Pas de faux positif : la ligne de DEFINITION de la macro elle-meme
     *    (OWL_SENTINEL_OFFSET(n), parenthese sans chiffre) et un commentaire
     *    mentionnant un offset libre ne doivent pas etre comptes. */
    {
        const char* text =
            "#define OWL_SENTINEL_OFFSET(n)    ((int32_t)(INT32_MIN + ((n) * 0x100)))\n"
            "/* OWL_SENTINEL_OFFSET(5) reste libre pour reuse futur. */\n"
            "#define OWL_SSC_BASE         OWL_SENTINEL_OFFSET(1)\n";
        owl_sentinel_scan_duplicates(text, &rep);
        check_count("macro def + commentaire", &rep, 0);
    }

    /* 3. Offsets distincts : aucun doublon. */
    {
        const char* text =
            "#define OWL_A_BASE  OWL_SENTINEL_OFFSET(10)\n"
            "#define OWL_B_BASE  OWL_SENTINEL_OFFSET(11)\n"
            "#define OWL_C_BASE  OWL_SENTINEL_OFFSET(12)\n";
        owl_sentinel_scan_duplicates(text, &rep);
        check_count("offsets distincts", &rep, 0);
    }

    /* 4. Meme nom repete au meme offset (p.ex. inclusion double) : pas un
     *    doublon de FAMILLE, ne doit pas etre rapporte. */
    {
        const char* text =
            "#define OWL_A_BASE  OWL_SENTINEL_OFFSET(7)\n"
            "#define OWL_A_BASE  OWL_SENTINEL_OFFSET(7)\n";
        owl_sentinel_scan_duplicates(text, &rep);
        check_count("meme nom repete", &rep, 0);
    }

    /* 5. Deux doublons independants dans le meme texte. */
    {
        const char* text =
            "#define OWL_X_BASE  OWL_SENTINEL_OFFSET(40)\n"
            "#define OWL_Y_BASE  OWL_SENTINEL_OFFSET(40)\n"
            "#define OWL_Z_BASE  OWL_SENTINEL_OFFSET(41)\n"
            "#define OWL_W_BASE  OWL_SENTINEL_OFFSET(41)\n";
        owl_sentinel_scan_duplicates(text, &rep);
        check_count("deux doublons", &rep, 2);
        if (!has_dup(&rep, 40, "OWL_X_BASE", "OWL_Y_BASE")) {
            fprintf(stderr, "FAIL [deux doublons]: doublon 40 manquant\n");
            fails++;
        }
        if (!has_dup(&rep, 41, "OWL_Z_BASE", "OWL_W_BASE")) {
            fprintf(stderr, "FAIL [deux doublons]: doublon 41 manquant\n");
            fails++;
        }
    }

    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s)) — la detection de collision "
               "d'offset sentinelle ne se comporte pas comme attendu\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (owl_sentinel_scan_duplicates detecte les "
           "collisions d'offset et ignore les non-collisions)\n");
    return 0;
}
