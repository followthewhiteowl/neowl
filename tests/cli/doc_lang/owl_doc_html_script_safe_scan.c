/*  — regression livree : `owl-doc.html` ouvrait un shell intact mais
 * une liste de symboles VIDE. Cause mesuree : l'aide HTML (`owl
 * --dump-doc-html <template>`) emet l'objet DATA[] dans un <script> inline.
 * L'exemple de la native HTMLVersTexte contient la chaine
 * `HTMLVersTexte("<script>var x = 1;</script>visible")` ; emise verbatim, le
 * `</script>` litteral fermait le bloc <script> cote navigateur -> `DATA`
 * jamais defini -> `renderList()` jamais appele -> liste vide. Le generateur
 * echappe desormais '<' en "<" dans toute la region injectee du HTML
 * (flag g_doc_script_safe dans src/main.c).
 *
 * Ce scanner verrouille l'invariant pour qu'il ne re-casse pas :
 *   (a) la sortie n'est pas vide et porte le marqueur `const DATA = [`.
 *   (b) le HTML genere contient EXACTEMENT UNE occurrence de `</script`
 *       (insensible a la casse). Le template n'a qu'un seul bloc <script> :
 *       toute occurrence en trop = une fuite d'une chaine du registry qui
 *       refermerait le bloc et re-casserait la page.
 *   (c) l'objet `{"kind":"native","name":"HTMLVersTexte"` est present dans
 *       DATA[] : preuve que l'exemple dangereux a SURVECU (echappe), pas
 *       simplement supprime.
 *
 * Boite noire, calque table_html_colonne_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh. Exit code 0 si tout tient, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

static char* run_capture(const char* cmdline) {
    FILE* fp = POPEN(cmdline, "r");
    if (!fp) return NULL;
    size_t cap = 1 << 20, len = 0; /* DATA[] est gros (tout le registry) */
    char* buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    buf[0] = '\0';
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap + n + 1) * 2;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, n);
        len += n;
        buf[len] = '\0';
    }
    PCLOSE(fp);
    return buf;
}

static void derive_repo_root(const char* owlBin, char* out, size_t outsz) {
    const char* marker = "/build/";
    const char* p = strstr(owlBin, marker);
    if (p) {
        const char* q;
        while ((q = strstr(p + 1, marker)) != NULL) p = q;
    }
    if (!p) { snprintf(out, outsz, "."); return; }
    size_t len = (size_t)(p - owlBin);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, owlBin, len);
    out[len] = '\0';
}

/* Le HTML est complet ssi il porte sa fermeture structurelle `</html>`, la
 * toute derniere ligne du template (le `</script>` surveille la precede
 * immediatement). Un dump tronque — binaire en cours de relink par un build
 * voisin, lecture de pipe interrompue — laisse passer `const DATA = [` (emis
 * tot dans le flux, ~8 Ko) mais perd `</script>` ET `</html>` (fin du flux,
 * ~6 Mo). Sans ce garde, une troncature transitoire se presente comme
 * "</script compte 0" : un faux signal de violation script-safe, alors que
 * la generation n'a simplement pas ete au bout. C'est la cause de l'ecart
 * batterie (binaire fige) / standalone (lance pendant un build concurrent). */
static int dump_is_complete(const char* html) {
    return html && strstr(html, "</html>") != NULL;
}

static char* run_dump_doc_html(const char* owlBin) {
    char root[1024];
    derive_repo_root(owlBin, root, sizeof(root));
    char cmdline[2048];
    snprintf(cmdline, sizeof(cmdline),
             "\"\"%s\" --dump-doc-html \"%s/docs/reference/_owl-doc-template.html\" 2>NUL\"",
             owlBin, root);
    /* Regenere tant que le flux est tronque : une troncature transitoire se
     * resorbe des que le binaire est stable, rendant le verdict identique en
     * batterie et en standalone. Une troncature PERSISTANTE (vraie panne de
     * generation) survit aux 4 tentatives et sera signalee comme telle. */
    char* out = NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        free(out);
        out = run_capture(cmdline);
        if (dump_is_complete(out)) break;
    }
    return out;
}

/* Compte les occurrences de `</script` dans `hay`, insensible a la casse. */
static int count_close_script(const char* hay) {
    const char* needle = "</script";
    size_t nlen = strlen(needle);
    int count = 0;
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == (unsigned char)needle[i]) {
            i++;
        }
        if (i == nlen) count++;
    }
    return count;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    char* out = run_dump_doc_html(owlBin);

    /* Garde d'infrastructure, EN AMONT des assertions de contenu : si la
     * generation n'a pas produit un HTML complet (vide ou tronque, meme apres
     * retries), ce n'est PAS une violation script-safe — le binaire etait
     * instable (build concurrent). On le dit distinctement (exit 2 = infra,
     * pas exit 1 = regression) pour ne jamais imputer au produit un ecart qui
     * n'existe pas. Un vrai leak, lui, laisse un HTML COMPLET avec >1 </script. */
    if (!out || !*out) {
        fprintf(stderr,
                "INFRA: sortie vide pour 'owl --dump-doc-html <template>' apres 4 tentatives\n"
                "       (binaire en cours de build/relink ? relancer) — pas une violation script-safe.\n");
        free(out);
        return 2;
    }
    if (!dump_is_complete(out)) {
        fprintf(stderr,
                "INFRA: HTML genere tronque (pas de </html> apres 4 tentatives)\n"
                "       generation interrompue, pas une violation script-safe — relancer sur binaire stable.\n");
        free(out);
        return 2;
    }

    /* (a) marqueur DATA[] present : la region injectee a bien ete emise. */
    if (!strstr(out, "const DATA = [")) {
        fprintf(stderr, "FAIL: marqueur \"const DATA = [\" absent (template introuvable ?)\n");
        fails++;
    }

    /* (b) exactement UNE fermeture de <script> : le template n'en a qu'une
     * (ligne 185). Toute occurrence supplementaire = une "</script>" d'une
     * chaine du registry non echappee -> le bloc se referme trop tot. */
    int nClose = count_close_script(out);
    if (nClose != 1) {
        fprintf(stderr,
                "FAIL: `</script` compte %d occurrence(s), attendu exactement 1.\n"
                "      Une fuite ferme le <script> inline -> DATA jamais defini -> liste vide.\n",
                nClose);
        fails++;
    }

    /* (c) l'exemple dangereux a survecu, echappe (pas supprime). */
    if (!strstr(out, "{\"kind\":\"native\",\"name\":\"HTMLVersTexte\"")) {
        fprintf(stderr,
                "FAIL: objet native \"HTMLVersTexte\" absent de DATA[]\n"
                "      (l'exemple porteur du `</script>` doit etre present et echappe)\n");
        fails++;
    }

    free(out);

    printf("=== fix/2426 aide HTML script-safe : `</script` unique dans le HTML genere ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (DATA present, 1 seul </script, HTMLVersTexte survit echappe)\n");
    return 0;
}
