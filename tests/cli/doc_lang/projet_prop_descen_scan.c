/*  — digue anti-franglais sur les fiches d'aide des proprietes du
 * bloc Projet (F1 -> owl-doc.html). Complement de projet_prop_scan.c :
 * celui-ci verrouille non pas la PRESENCE des proprietes (leur nom EN dans
 * DSLPROPS) mais la LANGUE de leur DESCRIPTION.
 *
 * Symptome couvert (support #253) : « francais dans l'aide anglaise ». Le
 * viewer choisit descEN quand la langue active est EN ; si le generateur
 * (`owl --dump-doc-html`) n'emet pas de descEN distincte, l'emetteur
 * doc_emit_desc replie descEN sur descFR et la fiche montre du FR en mode
 * EN. Ce harness verifie, pour chaque objet DSLPROPS `{"w":"Projet",...}`,
 * que descEN existe, est non vide, ET differe de descFR (sinon = repli
 * franglais silencieux). Source de verite : FIELDS_Projet
 * (src/stdlib_builtin_types.c), dont les 7 champs portent une descEN
 * genuine, distincte du FR.
 *
 * Mutation : retirer l'emission de descEN cote generateur ferait replier
 * doc_emit_desc sur descFR (descEN == descFR) -> ce test rougit. C'est ce
 * qui distingue un vrai EN d'un repli.
 *
 * Boite noire : compile a la volee par tests/cli/doc_lang/run.sh (calque
 * projet_prop_scan.c). Exit code 0 si les descriptions EN sont distinctes,
 * 1 sinon.
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

/* Extrait la valeur d'une cle chaine JSON `"<key>":"..."` dans [start,end).
 * Copie la valeur brute (avec sequences d'echappement telles quelles) dans
 * out. Retourne 1 si trouvee, 0 sinon. La comparaison brute suffit : FR et
 * EN sont echappes de la meme facon par le meme emetteur. */
static int extract_str(const char* start, const char* end,
                       const char* key, char* out, size_t outsz) {
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(start, needle);
    if (!p || p >= end) return 0;
    p += strlen(needle);
    size_t j = 0;
    while (p < end && *p) {
        if (*p == '\\' && p + 1 < end) {   /* copie la paire d'echappement */
            if (j + 2 < outsz) { out[j++] = p[0]; out[j++] = p[1]; }
            p += 2;
            continue;
        }
        if (*p == '"') break;              /* fin de la chaine */
        if (j + 1 < outsz) out[j++] = *p;
        p++;
    }
    out[j] = '\0';
    return 1;
}

static void derive_template(const char* owlBin, char* out, size_t outsz) {
    char root[1024];
    snprintf(root, sizeof(root), "%s", owlBin);
    for (int seg = 0; seg < 2; seg++) {   /* strip "owl.exe" puis "build" */
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
    const char* en;    /* forme EN attendue (nameEN de FIELDS_Projet) */
    const char* note;  /* nom FR de rappel */
} Expect;

/* Les 7 champs du type Projet, cle EN (src/stdlib_builtin_types.c). */
static const Expect EXPECTED[] = {
    { "Description",                    "Description" },
    { "Author",                        "Auteur" },
    { "License",                       "Licence" },
    { "Languages",                     "Langues" },
    { "Theme",                         "Theme" },
    { "StartupWindow",                 "FenetreDemarrage" },
    { "EnableNamingConventionWarning", "ActiveAvertissementConventionDeNommage" },
};
#define EXPECTED_COUNT ((int)(sizeof(EXPECTED)/sizeof(EXPECTED[0])))

/* Localise l'objet DSLPROPS `{"w":"Projet",...}` qui porte `"en":"<enForm>"`,
 * borne par le prochain `{"w":`. Retourne le pointeur de debut (start) et
 * ecrit end. NULL si absent. */
static const char* find_projet_obj(const char* json, const char* enForm,
                                   const char** endOut) {
    const char* WNEEDLE = "{\"w\":\"Projet\"";
    char en[96];
    snprintf(en, sizeof(en), "\"en\":\"%s\"", enForm);
    const char* p = json;
    while ((p = strstr(p, WNEEDLE)) != NULL) {
        const char* start = p;
        const char* bound = strstr(p + strlen(WNEEDLE), "{\"w\":\"");
        const char* end = bound ? bound : json + strlen(json);
        const char* hit = strstr(start, en);
        if (hit && hit < end) { *endOut = end; return start; }
        p += strlen(WNEEDLE);
    }
    return NULL;
}

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
        const char* end = NULL;
        const char* obj = find_projet_obj(out, EXPECTED[i].en, &end);
        if (!obj) {
            fprintf(stderr, "FAIL: propriete Projet \"%s\" (en=\"%s\") absente\n",
                    EXPECTED[i].note, EXPECTED[i].en);
            fails++;
            continue;
        }
        char descFR[4096] = {0}, descEN[4096] = {0};
        int hasFR = extract_str(obj, end, "descFR", descFR, sizeof(descFR));
        int hasEN = extract_str(obj, end, "descEN", descEN, sizeof(descEN));
        if (!hasEN || descEN[0] == '\0') {
            fprintf(stderr,
                    "FAIL: Projet.%s : descEN absente/vide (aide EN montrerait du FR)\n",
                    EXPECTED[i].note);
            fails++;
        } else if (hasFR && strcmp(descFR, descEN) == 0) {
            fprintf(stderr,
                    "FAIL: Projet.%s : descEN == descFR (repli franglais, pas une vraie "
                    "traduction EN)\n", EXPECTED[i].note);
            fails++;
        } else {
            printf("  OK  Projet.%-38s descEN distincte du FR\n", EXPECTED[i].note);
        }
    }
    free(out);

    printf("=== fix/1923 descriptions EN des proprietes du bloc Projet (anti-franglais) ===\n");
    printf("Proprietes verifiees : %d, echecs : %d\n", EXPECTED_COUNT, fails);
    if (fails > 0) {
        printf("RESULT: RED (%d) — l'aide EN des proprietes projet montrerait du FR.\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (%d proprietes projet avec descEN distincte)\n",
           EXPECTED_COUNT);
    return 0;
}
