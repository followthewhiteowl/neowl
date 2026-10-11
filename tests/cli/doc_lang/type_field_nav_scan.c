/*  — navigation F1 sur les champs de types composites (digue rouge).
 *
 * F1 dans l'editeur sur un champ de type compose (`MaxTokens`, `TopK`,
 * `TopP`, `Endpoint`, `MotDePasse`...) doit ouvrir owl-doc.html a l'ancre
 * `#<langue>/<mot>` et resoudre vers une fiche "membre" navigable. Cote
 * generateur (`owl --dump-doc-html`), ca implique qu'une entree PLATE de
 * type DSLPROPS (`{"w":"<Porteur>","n":"<ChampFR>","en":"<ChampEN>",...}`)
 * existe pour chaque champ, meme forme que celle deja emise pour les widgets
 * et pour le type Projet — mais AUJOURD'HUI seul Projet en
 * beneficie parmi les ~103 types composites. Ce harness ne cible PAS un nom
 * d'array precis (DSLPROPS vs un futur TYPEFIELDS) : il detecte la FORME
 * (entree plate demarree par une cle "w" ou "tp") independamment du nom de
 * variable JS qui la porte.
 *
 * Chaque champ verifie ici a ete choisi car il est ABSENT de toute fiche
 * top-level ET absent de toute entree plate aujourd'hui (verifie a la main
 * via --dump-doc-html) : ce ne sont PAS des cas de collision (feature #2),
 * uniquement des trous de navigation purs (feature #1). MotDePasse est
 * multi-porteur (AWS, ServeurBDD, FTP...) : verifie qu'au moins 2 types
 * distincts portent le champ, pour couvrir l'agregation "listant les types
 * porteurs" du critere d'acceptation.
 *
 * Boite noire : compile a la volee par tests/cli/doc_lang/run.sh (calque
 * projet_prop_scan.c). Exit code 0 si tout est present, 1 sinon (ROUGE
 * attendu tant que les champs de types composites hors Projet ne sont pas
 * emis dans une entree plate). */

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
    size_t cap = 262144, len = 0;
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

/* Trouve toutes les positions de debut d'entree plate `{"w":"` ou `{"tp":"`
 * dans `json`, triees par ordre d'apparition (elles le sont naturellement en
 * scannant lineairement). Retourne le nombre trouve, remplit `out` (deja
 * alloue par l'appelant, taille `cap`). */
static int find_flat_entry_starts(const char* json, const char** out, int cap) {
    int cnt = 0;
    const char* p = json;
    while (*p && cnt < cap) {
        const char* w = strstr(p, "{\"w\":\"");
        const char* tp = strstr(p, "{\"tp\":\"");
        const char* next = NULL;
        if (w && tp) next = (w < tp) ? w : tp;
        else if (w) next = w;
        else next = tp;
        if (!next) break;
        out[cnt++] = next;
        p = next + 1;
    }
    return cnt;
}

/* Compte les porteurs distincts (valeur de "w"/"tp") d'un champ `fieldFR`
 * dans les entrees plates. `carriers` recoit jusqu'a `maxCarriers` valeurs
 * (dedupliquees par simple recherche lineaire), retourne le compte. */
static int count_field_carriers(const char* json, const char* fieldFR,
                                 const char* fieldEN, char carriers[][96],
                                 int maxCarriers) {
    static const char* starts[8192];
    int nstarts = find_flat_entry_starts(json, starts, 8192);
    char nNeedle[128], enNeedle[128];
    snprintf(nNeedle, sizeof(nNeedle), "\"n\":\"%s\"", fieldFR);
    snprintf(enNeedle, sizeof(enNeedle), "\"en\":\"%s\"", fieldEN);

    int found = 0;
    for (int i = 0; i < nstarts; i++) {
        const char* start = starts[i];
        const char* bound = (i + 1 < nstarts) ? starts[i + 1] : (start + strlen(start));
        const char* hitN = strstr(start, nNeedle);
        const char* hitEn = strstr(start, enNeedle);
        if (!hitN || hitN >= bound) continue;
        if (!hitEn || hitEn >= bound) continue;

        /* extrait la valeur du porteur ("w" ou "tp"), juste apres le `{`
         * (chaque entree plate commence par l'une de ces deux formes). */
        char carrier[96] = {0};
        const char* key = NULL;
        if (strncmp(start, "{\"w\":\"", 6) == 0) key = start + 6;
        else if (strncmp(start, "{\"tp\":\"", 7) == 0) key = start + 7;
        if (key) {
            const char* endq = strchr(key, '"');
            if (endq && (size_t)(endq - key) < sizeof(carrier)) {
                memcpy(carrier, key, (size_t)(endq - key));
                carrier[endq - key] = '\0';
            }
        }
        if (carrier[0] == '\0') continue;

        int dup = 0;
        for (int c = 0; c < found; c++) if (strcmp(carriers[c], carrier) == 0) { dup = 1; break; }
        if (!dup && found < maxCarriers) {
            snprintf(carriers[found], 96, "%s", carrier);
            found++;
        }
    }
    return found;
}

static void derive_template(const char* owlBin, char* out, size_t outsz) {
    char root[1024];
    snprintf(root, sizeof(root), "%s", owlBin);
    for (int seg = 0; seg < 2; seg++) {
        char* slash = NULL;
        for (char* q = root; *q; q++) if (*q == '/' || *q == '\\') slash = q;
        if (!slash) { root[0] = '\0'; break; }
        *slash = '\0';
    }
    if (root[0])
        snprintf(out, outsz, "%s/docs/reference/_owl-doc-template.html", root);
    else
        snprintf(out, outsz, "docs/reference/_owl-doc-template.html");
}

typedef struct {
    const char* fr;
    const char* en;
    int minCarriers;   /* nombre minimal de types porteurs distincts attendu */
} FieldExpect;

/* 5 champs de types composites choisis pour leur absence totale (aujourd'hui)
 * de toute fiche/propriete top-level — trous purs, pas des collisions. */
static const FieldExpect EXPECTED[] = {
    { "MaxTokens",   "MaxTokens",   1 },  /* OptionsGénération (IA) */
    { "TopK",        "TopK",        1 },  /* OptionsGénération (IA) */
    { "TopP",        "TopP",        1 },  /* OptionsGénération (IA) */
    { "Endpoint",    "Endpoint",    1 },  /* config AWS */
    { "MotDePasse",  "Password",    2 },  /* multi-porteur : BDD, FTP, AWS... */
};
#define EXPECTED_COUNT ((int)(sizeof(EXPECTED)/sizeof(EXPECTED[0])))

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    char tpl[1200];
    derive_template(owlBin, tpl, sizeof(tpl));

    char cmdline[3072];
    snprintf(cmdline, sizeof(cmdline),
             "\"\"%s\" --dump-doc-html \"%s\" 2>NUL\"", owlBin, tpl);
    char* out = run_capture(cmdline);
    if (!out || !*out) {
        fprintf(stderr, "ERREUR: sortie vide (dump-doc-html non execute) : %s\n",
                cmdline);
        free(out);
        printf("RESULT: RED — dump-doc-html sans sortie (template: %s)\n", tpl);
        return 1;
    }

    int fails = 0;
    for (int i = 0; i < EXPECTED_COUNT; i++) {
        char carriers[16][96];
        int nc = count_field_carriers(out, EXPECTED[i].fr, EXPECTED[i].en, carriers, 16);
        if (nc >= EXPECTED[i].minCarriers) {
            printf("  OK  champ %-12s (en=%-10s) : %d porteur(s) [", EXPECTED[i].fr, EXPECTED[i].en, nc);
            for (int c = 0; c < nc; c++) printf("%s%s", c ? "," : "", carriers[c]);
            printf("]\n");
        } else {
            fprintf(stderr,
                    "FAIL: champ \"%s\" (en=\"%s\") absent de toute entree plate "
                    "navigable (%d porteur(s) trouve(s), %d attendu(s) min)\n",
                    EXPECTED[i].fr, EXPECTED[i].en, nc, EXPECTED[i].minCarriers);
            fails++;
        }
    }
    free(out);

    printf("=== feat/1866 champs de types composites navigables F1 (dump-doc-html) ===\n");
    printf("Champs verifies : %d, echecs : %d\n", EXPECTED_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d absent(s)) — attendu tant que dump_doc_html n'emet "
               "pas les champs des types composites (hors Projet) dans une entree "
               "plate navigable.\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (%d champs emis, formes FR+EN conformes, agregation multi-porteur ok)\n",
           EXPECTED_COUNT);
    return 0;
}
