/*  — digue sur la recherche d'un guide par son titre EN dans la doc
 * d'aide HTML (`owl --dump-doc-html`).
 *
 * Support #389, regression signalee apres 1.5.0 : le guide "Prefixes de
 * nommage" est bien rendu en anglais (titre + contenu, infra ), mais
 * il reste introuvable par recherche sur son titre anglais ("naming") — seul
 * "Prefixe[s] de nommage" (FR) le fait remonter. Cause : l'entree de
 * navigation DATA[] kind "reference" de ce guide (emise par la boucle sur
 * EMBEDDED_PUBLIC_DOCS, cf. src/main.c) porte name/ascii/cat/doc mais PAS de
 * champ "en" — contrairement aux fiches de reference (operateurs, variables
 * globales...) qui en portent un. Le viewer indexe la recherche sur
 * [d.name, d.ascii, d.en] (_owl-doc-template.html) : sans "en", le titre EN
 * du guide (deja present dans GUIDES[stem].titleEN depuis ) n'est
 * jamais cherchable.
 *
 * Exigences verifiees sur le HTML reellement produit par owl.exe :
 *
 *   1. L'entree de navigation du guide "12_prefixes_de_nommage" (qui a une
 *      variante EN reelle, titleEN="Naming prefixes") porte un champ "en"
 *      dont la valeur est ce titre EN reel. Verifie en reel sur le dump
 *      actuel (GUIDES["12_prefixes_de_nommage"].titleEN="Naming prefixes") —
 *      pas une invention.
 *
 *   2. Garde positive de coherence sur un SECOND guide independant
 *      ("00_apercu_du_langage", qui a aussi une variante EN reelle,
 *      titleEN="OWL — Language overview") : son entree de navigation porte
 *      elle aussi un champ "en" dont la valeur est son vrai titre EN — pas
 *      vide, pas fabrique. Preuve que le mecanisme est GENERAL (pas code en
 *      dur sur le seul stem de l'attente #1).
 *
 *      Note (corrige apres premiere version de ce scanner) : il n'existe
 *      AUCUN guide public FR-only — les 42 guides ont tous une variante
 *      `.en.md`. L'ancienne attente #2 ("guide sans variante EN ne doit pas
 *      recevoir de champ en") etait donc insatisfiable par toute impl
 *      correcte ; elle a ete remplacee par cette garde positive sur un
 *      second stem.
 *
 * Absent aujourd'hui (verifie rouge sur le binaire courant) : l'entree de
 * navigation de "12_prefixes_de_nommage" ne porte aucune cle "en" du tout,
 * donc l'attente #1 echoue pour la bonne raison (feature non implementee).
 *
 * Principe (calque sur category_filter_lang_scan.c / guide_lang_scan.c) :
 * on invoque le VRAI binaire owl.exe (chemin en argv[1]) en `--dump-doc-html`,
 * on capture le HTML, on extrait la fenetre texte de l'entree de navigation
 * du stem vise (bornee par la derniere occurrence de `{"kind":"reference"`
 * avant son `,"doc":"<stem>"}` de fermeture), puis on verifie des
 * sous-chaines dans cette fenetre. L'implementer est libre de l'ordre des
 * champs JSON emis.
 *
 * Ce n'est PAS un self-test compile dans owl.exe : harness boite noire
 * compile a la volee par tests/cli/doc_lang/run.sh.
 *
 * Exit code : 0 si toutes les attentes tiennent, 1 sinon.
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

/* Derive le chemin du template a partir du chemin de l'exe :
 * <racine>/build/owl.exe -> <racine>/docs/reference/_owl-doc-template.html. */
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

/* Extrait la fenetre texte de l'entree DATA[] kind "reference" dont la cle
 * "doc" vaut `stem` : cherche la fermeture `,"doc":"<stem>"}` puis remonte a
 * la DERNIERE occurrence precedente de `{"kind":"reference"` pour bornerner
 * le debut de l'objet. Les entrees de navigation sont plates (aucune valeur
 * n'embarque d'accolade), donc cette delimitation simple suffit — pas besoin
 * d'un comptage de profondeur comme pour les objets GUIDES (markdown brut). */
static char* extract_nav_entry(const char* out, const char* stem) {
    char tail[300];
    snprintf(tail, sizeof(tail), ",\"doc\":\"%s\"}", stem);
    const char* end = strstr(out, tail);
    if (!end) return NULL;
    end += strlen(tail);

    const char* marker = "{\"kind\":\"reference\"";
    const char* start = NULL;
    const char* p = out;
    while (p < end) {
        const char* f = strstr(p, marker);
        if (!f || f >= end) break;
        start = f;
        p = f + 1;
    }
    if (!start) return NULL;

    size_t len = (size_t)(end - start);
    char* win = (char*)malloc(len + 1);
    if (!win) return NULL;
    memcpy(win, start, len);
    win[len] = '\0';
    return win;
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

    /* 1. Guide AVEC variante EN : entree de navigation cherchable par le
     *    titre EN reel (titleEN="Naming prefixes" dans GUIDES, verifie sur
     *    le dump reel avant ce scanner). */
    {
        const char* stem = "12_prefixes_de_nommage";
        char* win = extract_nav_entry(out, stem);
        if (!win) {
            fprintf(stderr, "FAIL: entree de navigation introuvable — %s\n", stem);
            fails++;
        } else {
            if (strstr(win, "\"en\":\"Naming prefixes\"")) {
                printf("  OK  present (entree de navigation de %s porte "
                       "\"en\":\"Naming prefixes\")\n", stem);
            } else {
                fprintf(stderr,
                        "FAIL: attendu absent — entree de navigation de %s "
                        "sans champ \"en\" (titre EN reel du guide, deja "
                        "present dans GUIDES[%s].titleEN)\n", stem, stem);
                fails++;
            }
            free(win);
        }
    }

    /* 2. Garde positive de coherence sur un second guide independant : son
     *    entree de navigation porte un champ "en" dont la valeur est son
     *    vrai titre EN (GUIDES["00_apercu_du_langage"].titleEN =
     *    "OWL — Language overview"). Ancre sur le prefixe "OWL " et le
     *    suffixe "Language overview" pour esquiver le piege d'encodage du
     *    tiret cadratin (U+2014) sans affaiblir la preuve : champ present,
     *    non vide, = vrai titre, pas une fabrication. Prouve en plus que le
     *    mecanisme est general (pas code en dur sur le seul stem de #1). */
    {
        const char* stem = "00_apercu_du_langage";
        char* win = extract_nav_entry(out, stem);
        if (!win) {
            fprintf(stderr, "FAIL: entree de navigation introuvable — %s\n", stem);
            fails++;
        } else {
            if (strstr(win, "\"en\":\"OWL ") && strstr(win, "Language overview\"")) {
                printf("  OK  present (entree de navigation de %s porte "
                       "\"en\":\"OWL \" ... \"Language overview\")\n", stem);
            } else {
                fprintf(stderr,
                        "FAIL: attendu absent — entree de navigation de %s "
                        "sans champ \"en\" (titre EN reel du guide, deja "
                        "present dans GUIDES[%s].titleEN)\n", stem, stem);
                fails++;
            }
            free(win);
        }
    }

    free(out);

    printf("=== fix/2283 recherche d'un guide par son titre EN (dump-doc-html) ===\n");
    printf("Attentes : 2 positives, echecs : %d\n", fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s))\n", fails);
        return 1;
    }
    printf("RESULT: GREEN\n");
    return 0;
}
