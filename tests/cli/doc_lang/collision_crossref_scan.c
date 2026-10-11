/*  — cross-ref des collisions nom-propriete / fiche-existante.
 *
 * Diagnostic verifie a la main (owl --dump-doc-html) : pour `Style`,
 * `Couleur`, `Grisé` (normalise sans accent : `Grise`), une fiche
 * "byName" existe DEJA (type Style, type Couleur, const Grisé) ET une
 * entree membre (propriete DSL de widget, agregee dans DSLPROPS) existe
 * DEJA AUSSI (Style : 18 widgets, Couleur : 9, Grisé : 16). Cote resolution
 * (`resolve()` dans le template, non modifie ici), `byName` gagne toujours
 * sur `byProp` : F1 sur ces mots ouvre la fiche existante et MASQUE
 * silencieusement l'aspect propriete, sans aucun renvoi.
 *
 * Ce harness verifie DEUX choses :
 *  1. Precondition (deja vraie aujourd'hui, VERT) : la fiche ET l'entree
 *     membre coexistent bien dans le DATA — non-regression sur le
 *     diagnostic qui justifie ce ticket.
 *  2. Renvoi (ROUGE aujourd'hui) : la fiche existante porte, quelque part
 *     dans son objet JSON, une indication textuelle de son statut membre
 *     ("Également"/"aussi" + "propriete"/"widgets", ou l'anglais
 *     equivalent). Scan heuristique multi-motifs (volontairement permissif)
 *     car la forme exacte du renvoi est du ressort de l'implementeur —
 *     n'importe quelle formulation contenant un des motifs suffit a faire
 *     passer ce test. Si l'implementation choisit une formulation qui
 *     n'emploie AUCUN des motifs ci-dessous, ce test doit etre etendu (pas
 *     contourne) : signaler au coordinator plutot que d'assouplir.
 *
 * Boite noire, calque projet_prop_scan.c. Le point 2 est un test de
 * l'OUTCOME (presence d'une info de renvoi), pas de l'implementation
 * (aucun nom de cle/array n'est impose). */

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

/* Trouve la fiche top-level `{"kind":"...","name":"<name>"` et retourne un
 * pointeur sur son debut, bornee jusqu'a la prochaine occurrence de
 * `{"kind":"` (ou fin de chaine). Ecrit la borne dans *boundOut. NULL si
 * aucune fiche trouvee. */
static const char* find_fiche_slice(const char* json, const char* name, const char** boundOut) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"name\":\"%s\"", name);
    const char* p = json;
    while ((p = strstr(p, "{\"kind\":\"")) != NULL) {
        const char* nextKind = strstr(p + 1, "{\"kind\":\"");
        const char* bound = nextKind ? nextKind : (p + strlen(p));
        const char* hit = strstr(p, needle);
        if (hit && hit < bound) {
            /* verifie que "name" appartient bien a CETTE fiche (juste apres
             * "kind":"...", pas une coincidence dans une desc plus loin) */
            *boundOut = bound;
            return p;
        }
        p = bound;
    }
    return NULL;
}

/* Compte les entrees membres plates (`{"w":"..."` / `{"tp":"..."`) portant
 * n:"<name>" (accent-insensible n'est pas necessaire ici : les valeurs
 * testees sont passees telles qu'observees dans le DATA aujourd'hui). */
static int count_member_entries(const char* json, const char* nameInDslprops) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"n\":\"%s\"", nameInDslprops);
    int count = 0;
    const char* p = json;
    while ((p = strstr(p, needle)) != NULL) { count++; p += strlen(needle); }
    return count;
}

static const char* CROSSREF_MARKERS[] = {
    "galement", "Also a prop", "also a prop", "Also a member", "also a member",
    "aussi une propri", "aussi un champ", "field of", "propriete d'interface",
    "propri\xc3\xa9t\xc3\xa9 d'interface", "seeAlso", "alsoProp", "crossRef", "xref",
};
#define CROSSREF_MARKERS_COUNT ((int)(sizeof(CROSSREF_MARKERS)/sizeof(CROSSREF_MARKERS[0])))

static int fiche_has_crossref(const char* start, const char* bound) {
    size_t len = (size_t)(bound - start);
    char* slice = (char*)malloc(len + 1);
    if (!slice) return 0;
    memcpy(slice, start, len);
    slice[len] = '\0';
    int found = 0;
    for (int i = 0; i < CROSSREF_MARKERS_COUNT && !found; i++) {
        if (strstr(slice, CROSSREF_MARKERS[i])) found = 1;
    }
    free(slice);
    return found;
}

typedef struct {
    const char* ficheName;      /* nom porte par la fiche existante (byName) */
    const char* memberName;     /* nom porte par l'entree membre (byProp) — meme
                                  * mot normalise, forme brute observee dans DATA */
} Collision;

/* 3 des 17 collisions listees au ticket, verifiees a la main : fiche ET
 * entree membre coexistent deja aujourd'hui dans le DATA. */
static const Collision COLLISIONS[] = {
    { "Style",   "Style" },
    { "Couleur", "Couleur" },
    { "Gris\xc3\xa9",  "Gris\xc3\xa9" },
};
#define COLLISIONS_COUNT ((int)(sizeof(COLLISIONS)/sizeof(COLLISIONS[0])))

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    char tpl[1200];
    derive_template(owlBin, tpl, sizeof(tpl));

    char cmdline[3072];
    snprintf(cmdline, sizeof(cmdline),
             "\"\"%s\" --dump-doc-html \"%s\" 2>NUL\"", owlBin, tpl);
    char* out = run_capture(cmdline);
    if (!out || !*out) {
        fprintf(stderr, "ERREUR: sortie vide (dump-doc-html non execute) : %s\n", cmdline);
        free(out);
        printf("RESULT: RED — dump-doc-html sans sortie (template: %s)\n", tpl);
        return 1;
    }

    int precondFails = 0, crossrefFails = 0;
    for (int i = 0; i < COLLISIONS_COUNT; i++) {
        const char* bound = NULL;
        const char* fiche = find_fiche_slice(out, COLLISIONS[i].ficheName, &bound);
        int nMember = count_member_entries(out, COLLISIONS[i].memberName);

        if (!fiche || nMember == 0) {
            fprintf(stderr, "FAIL precondition: \"%s\" — fiche=%s, entrees membre=%d "
                    "(les deux doivent deja coexister, cf. diagnostic ticket)\n",
                    COLLISIONS[i].ficheName, fiche ? "oui" : "non", nMember);
            precondFails++;
            continue;
        }
        printf("  OK  precondition \"%s\" : fiche presente + %d entree(s) membre\n",
               COLLISIONS[i].ficheName, nMember);

        int hasXref = fiche_has_crossref(fiche, bound);
        if (hasXref) {
            printf("  OK  renvoi trouve dans la fiche \"%s\"\n", COLLISIONS[i].ficheName);
        } else {
            fprintf(stderr, "FAIL: fiche \"%s\" ne porte aucun renvoi vers son aspect "
                    "propriete/membre (aucun motif heuristique trouve)\n",
                    COLLISIONS[i].ficheName);
            crossrefFails++;
        }
    }
    free(out);

    printf("=== feat/1866 cross-ref collisions (dump-doc-html) ===\n");
    printf("Collisions verifiees : %d, preconditions en echec : %d, renvois manquants : %d\n",
           COLLISIONS_COUNT, precondFails, crossrefFails);
    if (precondFails > 0) {
        printf("RESULT: RED (precondition invalide — diagnostic ticket a revalider)\n");
        return 1;
    }
    if (crossrefFails > 0) {
        printf("RESULT: RED (%d renvoi(s) manquant(s)) — attendu tant que la fiche "
               "existante n'affiche pas le renvoi \"aussi une propriete de...\"\n",
               crossrefFails);
        return 1;
    }
    printf("RESULT: GREEN (preconditions + renvois presents)\n");
    return 0;
}
