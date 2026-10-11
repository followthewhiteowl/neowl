/*  (ST3 de l'umbrella , ) — back-link contenu
 *  : le guide `12_prefixes_de_nommage.md` decrit la propriete
 * `ActiveAvertissementConventionDeNommage` du bloc `est un Projet` (section
 * "Verification automatique (opt-in projet)") mais ne renvoie jamais vers la
 * fiche `Projet` elle-meme (confirme au depot : `grep -n "Projet"
 * docs/public_documentation/12_prefixes_de_nommage.md` ne remonte que des
 * occurrences dans des blocs `est un Projet` d'exemple ```owl, jamais un
 * renvoi explicite type "owl explain Projet"). Convention deja etablie
 * ailleurs dans les guides publics pour ce genre de renvoi (grep sur
 * docs/public_documentation, plusieurs occurrences, ex.
 * `08_exceptions.md:481`, `14_sql_connecteurs.md:19`) : mention inline
 * `` `owl explain <Symbole>` ``.
 *
 * Ce scanner verrouille qu'un renvoi vers la fiche `Projet` apparait dans le
 * rendu CLI du guide embarque (`owl learn 12_prefixes_de_nommage`), sous la
 * forme conventionnelle `owl explain Projet`.
 *
 * ROUGE attendu tant que le renvoi n'est pas ajoute : la sortie ne contient
 * jamais "explain Projet".
 *
 * Boite noire, calque tests/cli/doc_lang/crossref_pin_scan.c (ST1).
 * Compile a la volee par tests/cli/doc_lang/run.sh.
 * Exit code : 0 si toutes les assertions tiennent, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    size_t cap = 65536, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    buf[0] = '\0';
    char chunk[4096];
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

static char* run_owl(const char* owlBin, const char* args) {
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" %s 2>NUL\"", owlBin, args);
    return run_capture(cmdline);
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    char* out = run_owl(owlBin, "learn 12_prefixes_de_nommage");
    if (!out || !*out) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl learn 12_prefixes_de_nommage'\n");
        free(out);
        return 1;
    }

    if (!strstr(out, "explain Projet")) {
        fprintf(stderr, "FAIL: guide 12_prefixes_de_nommage : aucun renvoi "
                "\"explain Projet\" trouve (back-link manquant vers la fiche "
                "Projet)\n");
        fails++;
    }
    free(out);

    printf("=== feat/2142 back-link guide->fiche (12_prefixes_de_nommage -> Projet) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — back-link pas encore ajoute au guide\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (guide 12_prefixes_de_nommage renvoie vers la fiche Projet)\n");
    return 0;
}
