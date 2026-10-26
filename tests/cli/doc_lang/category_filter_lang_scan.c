/*  — digue sur la localisation des libelles de categorie dans la
 * doc d'aide HTML (`owl --dump-doc-html`).
 *
 * Support #389 / volet #265 : la rangee de filtres par categorie (Affichage,
 * Binaire, Buffer...) reste en francais en mode EN, alors que la rangee des
 * types (Champ, Constante, Evenement...) est deja traduite. La table
 * bilingue existe deja (`src/owl_categories.def`, resolue par
 * `owl_category_resolve()`) mais n'est branchee ni sur le dump ni sur le
 * viewer.
 *
 * Exigences verifiees sur le HTML reellement produit par owl.exe :
 *
 *   1. Le dump embarque un champ `catEN` a cote de `cat`, avec une forme EN
 *      reelle (pas une invention) issue de `owl_categories.def` — on ancre
 *      sur deux paires connues : Chaines/Strings et Fichiers/Files.
 *
 *   2. Le viewer rend la categorie de facon language-aware : un helper
 *      miroir de `dispName`/`dispDesc` (meme convention : lang==="en" avec
 *      fallback FR si pas de forme EN), applique aux trois sites qui
 *      affichaient jusqu'ici `d.cat` brut : la rangee de filtres
 *      (`buildFilters`), le tag categorie d'une fiche, et les en-tetes de
 *      groupe de la liste (`renderList`). La cle interne de filtrage/
 *      groupement (attribut `data-cat`, `activeCats`) reste la forme FR —
 *      seul l'AFFICHAGE est localise (decision ticket).
 *
 * Aucune de ces sous-chaines n'existe dans le template actuel : ni le champ
 * `catEN` (absent du dump), ni le helper `catLabel` (le viewer ne connait
 * que `d.cat`). C'est le contrat d'implementation attendu, pas une
 * tautologie sur de l'existant.
 *
 * Principe (calque sur widget_member_sort_lang_scan.c / list_scroll_reset_scan.c) :
 * on invoque le VRAI binaire owl.exe (chemin en argv[1]) en `--dump-doc-html`,
 * on capture le HTML (qui embarque le JS du viewer verbatim), et on verifie
 * ces sous-chaines. Le chemin du template est derive du chemin de l'exe.
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

/* POSITIF : emission du champ catEN dans le dump (deux paires reelles du
 * registre, pour exclure un "catEN" vide ou une invention). */
typedef struct { const char* needle; const char* note; } Expect;
static const Expect POSITIVE_DUMP[] = {
    { "\"catEN\":\"Strings\"",
      "catEN emis pour la categorie Chaines (forme EN du registre)" },
    { "\"catEN\":\"Files\"",
      "catEN emis pour la categorie Fichiers (forme EN du registre)" },
};
#define POSITIVE_DUMP_COUNT ((int)(sizeof(POSITIVE_DUMP)/sizeof(POSITIVE_DUMP[0])))

/*  — support #389, regression apres 1.5.0 : la table bilingue de
 * `owl_categories.def` ne couvre QUE les categories portees par des
 * natives/mots-cles/limites (regle 56 du self-test) ; les categories
 * portees uniquement par des constantes/enums, et les pseudo-categories
 * doc litterales, en sont absentes -> `owl_category_resolve()` retombe sur
 * le FR (catEN == cat). Enumere sur le dump reel (`--dump-doc-html`),
 * cat/catEN identiques pour : Affichage, Clavier, Apprentissage automatique,
 * Primitifs, Reference, FormatNombre, FormatNumerique, FormatMonetaire,
 * Temporel, Constantes diverses (hors identites legitimes deja anglaises :
 * Guide, Interface, PDF, Multimedia, HTTP, JSON, Buffer, Chrono... —
 * celles-ci gardent sciemment catEN == cat, non couvertes ici).
 *
 * Paires EN proposees :
 *   - Affichage/Clavier/Apprentissage automatique/Primitifs/Reference :
 *     ancres fournies par le coordinator ticket  (Display, Keyboard,
 *     Machine Learning, Primitives, Reference sans accent).
 *   - FormatNumerique -> NumberFormat, FormatMonetaire -> CurrencyFormat :
 *     formes EN DEJA canon dans le registre (nameEN des types homonymes,
 *     src/stdlib_builtin_types.c), pas une invention.
 *   - FormatNombre -> NumberDisplayFormat : distinct de NumberFormat
 *     (reserve a FormatNumerique) pour eviter la collision EN->FR documentee
 *     dans le commentaire du type FormatNombre (meme fichier).
 *   - Temporel -> Temporal : terme deja employe comme concept interne
 *     (`docs/internal_documentation/temporal_family_canon.md`,
 *     `isTemporal` dans compiler.c), pas une invention isolee.
 *   - Constantes diverses -> Miscellaneous Constants : traduction directe,
 *     aucun precedent registre trouve — PROPOSITION a valider par le
 *     coordinator (signale dans le rapport test_writer, pas une certitude
 *     au meme niveau que les autres paires de cette liste).
 *
 * Absentes aujourd'hui (verifie rouge sur le binaire courant, 0 occurrence
 * de chacune de ces 10 paires `catEN`) : attentes rouges, pas des
 * tautologies sur de l'existant. */
static const Expect POSITIVE_DUMP_2283[] = {
    { "\"cat\":\"Affichage\",\"catEN\":\"Display\"",
      "fix/2283 : catEN=Display pour la categorie Affichage" },
    { "\"cat\":\"Clavier\",\"catEN\":\"Keyboard\"",
      "fix/2283 : catEN=Keyboard pour la categorie Clavier" },
    { "\"cat\":\"Apprentissage automatique\",\"catEN\":\"Machine Learning\"",
      "fix/2283 : catEN=Machine Learning pour la categorie Apprentissage automatique" },
    { "\"cat\":\"Primitifs\",\"catEN\":\"Primitives\"",
      "fix/2283 : catEN=Primitives pour la categorie Primitifs" },
    { "\"cat\":\"R\xc3\xa9" "f\xc3\xa9rence\",\"catEN\":\"Reference\"",
      "fix/2283 : catEN=Reference (sans accent) pour la categorie Reference" },
    { "\"cat\":\"FormatNum\xc3\xa9rique\",\"catEN\":\"NumberFormat\"",
      "fix/2283 : catEN=NumberFormat pour la categorie FormatNumerique (nameEN deja canon)" },
    { "\"cat\":\"FormatNombre\",\"catEN\":\"NumberDisplayFormat\"",
      "fix/2283 : catEN=NumberDisplayFormat pour la categorie FormatNombre (nameEN deja canon)" },
    { "\"cat\":\"FormatMon\xc3\xa9taire\",\"catEN\":\"CurrencyFormat\"",
      "fix/2283 : catEN=CurrencyFormat pour la categorie FormatMonetaire (nameEN deja canon)" },
    { "\"cat\":\"Temporel\",\"catEN\":\"Temporal\"",
      "fix/2283 : catEN=Temporal pour la categorie Temporel" },
    { "\"cat\":\"Constantes diverses\",\"catEN\":\"Miscellaneous Constants\"",
      "fix/2283 : catEN=Miscellaneous Constants pour la categorie Constantes diverses (PROPOSITION a valider)" },
};
#define POSITIVE_DUMP_2283_COUNT ((int)(sizeof(POSITIVE_DUMP_2283)/sizeof(POSITIVE_DUMP_2283[0])))

/* NON-REGRESSION  : identites assumees par le ticket (Guide,
 * Interface, PDF, Multimedia — deja des mots anglais, aucune traduction
 * voulue). Deja vertes aujourd'hui ; ce n'est pas une attente sur une
 * feature non-implementee, c'est un filet pour qu'une impl trop zelee
 * n'invente pas une forme EN differente dessus. */
/* Note : pas d'ancrage sur l'adjacence "cat":X","catEN":Y — certains sites
 * (widgets) intercalent d'autres champs (ex. "widgetkind") entre les deux,
 * cf. "cat":"Interface","widgetkind":"bloc","catEN":"Interface". On ancre
 * donc sur la valeur catEN seule : suffisant pour detecter une traduction
 * non voulue introduite sur une identite assumee. */
static const Expect NONREG_IDENTITY_2283[] = {
    { "\"catEN\":\"Guide\"",
      "fix/2283 non-regression : Guide reste catEN=Guide (identite assumee)" },
    { "\"catEN\":\"Interface\"",
      "fix/2283 non-regression : Interface reste catEN=Interface (identite assumee)" },
    { "\"catEN\":\"PDF\"",
      "fix/2283 non-regression : PDF reste catEN=PDF (identite assumee)" },
    { "\"catEN\":\"Multimedia\"",
      "fix/2283 non-regression : Multimedia reste catEN=Multimedia (identite assumee)" },
};
#define NONREG_IDENTITY_2283_COUNT ((int)(sizeof(NONREG_IDENTITY_2283)/sizeof(NONREG_IDENTITY_2283[0])))

/* POSITIF : viewer language-aware (helper + 3 sites d'affichage). */
static const Expect POSITIVE_VIEWER[] = {
    { "function catLabel(cat,catEN){ return lang===\"en\"&&catEN?catEN:cat; }",
      "helper de libelle categorie language-aware (miroir dispName/dispDesc)" },
    { "catLabel(d.cat,d.catEN)",
      "tag categorie d'une fiche localise via catLabel" },
    { "catLabel(cat,groupEN[cat])",
      "en-tete de groupe de la liste localise via catLabel" },
    { "catLabel(c,pairs.get(c))",
      "chip de la rangee de filtres categorie localisee via catLabel" },
};
#define POSITIVE_VIEWER_COUNT ((int)(sizeof(POSITIVE_VIEWER)/sizeof(POSITIVE_VIEWER[0])))

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

    for (int i = 0; i < POSITIVE_DUMP_COUNT; i++) {
        if (strstr(out, POSITIVE_DUMP[i].needle)) {
            printf("  OK  present (%s)\n", POSITIVE_DUMP[i].note);
        } else {
            fprintf(stderr, "FAIL: attendu absent — %s\n", POSITIVE_DUMP[i].note);
            fails++;
        }
    }

    for (int i = 0; i < POSITIVE_VIEWER_COUNT; i++) {
        if (strstr(out, POSITIVE_VIEWER[i].needle)) {
            printf("  OK  present (%s)\n", POSITIVE_VIEWER[i].note);
        } else {
            fprintf(stderr, "FAIL: attendu absent — %s\n", POSITIVE_VIEWER[i].note);
            fails++;
        }
    }

    for (int i = 0; i < POSITIVE_DUMP_2283_COUNT; i++) {
        if (strstr(out, POSITIVE_DUMP_2283[i].needle)) {
            printf("  OK  present (%s)\n", POSITIVE_DUMP_2283[i].note);
        } else {
            fprintf(stderr, "FAIL: attendu absent — %s\n", POSITIVE_DUMP_2283[i].note);
            fails++;
        }
    }

    for (int i = 0; i < NONREG_IDENTITY_2283_COUNT; i++) {
        if (strstr(out, NONREG_IDENTITY_2283[i].needle)) {
            printf("  OK  present (%s)\n", NONREG_IDENTITY_2283[i].note);
        } else {
            fprintf(stderr, "FAIL: regression — %s\n", NONREG_IDENTITY_2283[i].note);
            fails++;
        }
    }

    free(out);

    printf("=== fix/2236+2283 localisation des libelles de categorie (dump-doc-html) ===\n");
    printf("Attentes : %d positives, echecs : %d\n",
           POSITIVE_DUMP_COUNT + POSITIVE_VIEWER_COUNT + POSITIVE_DUMP_2283_COUNT + NONREG_IDENTITY_2283_COUNT,
           fails);
    if (fails > 0) {
        printf("RESULT: RED (%d ecart(s))\n", fails);
        return 1;
    }
    printf("RESULT: GREEN\n");
    return 0;
}
