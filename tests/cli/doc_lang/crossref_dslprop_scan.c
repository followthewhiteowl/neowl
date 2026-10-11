/*  (ST3 de l'umbrella ,  — decision dev "fidelite
 * complete") — extension du mecanisme de cross-reference declarative aux
 * FICHES DE PROPRIETE DSL (`OWL_XREF_DSL_PROP`, reserve par ST1/).
 *
 * Grief d'origine (, ticket support #324) : la propriete `Projet`
 * `ActiveAvertissementConventionDeNommage` / `EnableNamingConventionWarning`
 * n'a PAS de fiche `explain` autonome (confirme : `owl explain
 * ActiveAvertissementConventionDeNommage` -> "symbole ... inconnu" — c'est
 * un champ du type composite Projet, pas un symbole du registre). Sa SEULE
 * fiche navigable vit dans le viewer, comme entree `DSLPROPS` (w="Projet")
 * emise par `dump_doc_html` (src/main.c, autour du commentaire
 * " — champs du type composite Projet"). C'est LA que le lien vers
 * le guide des prefixes doit apparaitre — pas via `owl explain` (impossible,
 * pas de fiche standalone).
 *
 * Ce scanner isole l'objet JSON de cette entree dans `const DSLPROPS = [...]`
 * (sortie de `owl --dump-doc-html docs/reference/_owl-doc-template.html`) et
 * verifie qu'il porte un champ de lien fiche->guide vers
 * "12_prefixes_de_nommage" — au meme titre que `cli_doc_emit_public_docs_json`
 * le fait deja pour les natives/types dans `DATA[]` (cf.
 * crossref_viewer_parity_scan.c, ST1).
 *
 * Format JSON actuel de l'objet (avant extension), verifie au depot de ce
 * scanner :
 *   {"w":"Projet","n":"ActiveAvertissementConventionDeNommage",
 *    "en":"EnableNamingConventionWarning","t":"booléen",
 *    "descFR":"...","descEN":"..."}
 * — aucun champ "public_documentation" ni equivalent. ROUGE attendu tant que
 * l'emetteur DSLPROPS n'appelle pas le meme mecanisme de cross-ref pour les
 * champs Projet (extension attendue : `crossref_symbol_exists` doit aussi
 * resoudre un nom de propriete DSL via son catalogue, pas seulement les
 * registres natif/type/enum/constante).
 *
 * Boite noire, calque tests/cli/doc_lang/crossref_viewer_parity_scan.c (ST1).
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
    size_t cap = 1 << 20, len = 0; /* DSLPROPS est gros (tout le catalogue de props) */
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

/* Deduit la racine du repo depuis le chemin de owl.exe (.../build/owl.exe ->
 * ...) pour localiser le template HTML sans dependre du cwd du scanner. */
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

static char* run_dump_doc_html(const char* owlBin) {
    char root[1024];
    derive_repo_root(owlBin, root, sizeof(root));
    char cmdline[2048];
    snprintf(cmdline, sizeof(cmdline),
             "\"\"%s\" --dump-doc-html \"%s/docs/reference/_owl-doc-template.html\" 2>NUL\"",
             owlBin, root);
    return run_capture(cmdline);
}

/* Isole l'objet JSON de l'entree DSLPROPS "Projet"/"ActiveAvertissementConventionDeNommage"
 * (needle sur le couple w+n, dans l'ordre d'emission actuel de dump_doc_html)
 * en bornant a l'accolade fermante correspondante (comptage de profondeur). */
static char g_obj_buf[1 << 16];
static const char* find_dslprop_object(const char* data) {
    const char* needle = "\"w\":\"Projet\",\"n\":\"ActiveAvertissementConventionDeNommage\"";
    const char* hit = strstr(data, needle);
    if (!hit) return NULL;
    const char* objStart = hit;
    while (objStart > data && *objStart != '{') objStart--;
    if (*objStart != '{') return NULL;
    int depth = 0;
    const char* p = objStart;
    for (; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
    }
    size_t len = (size_t)(p - objStart);
    if (len >= sizeof(g_obj_buf)) len = sizeof(g_obj_buf) - 1;
    memcpy(g_obj_buf, objStart, len);
    g_obj_buf[len] = '\0';
    return g_obj_buf;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    char* out = run_dump_doc_html(owlBin);
    if (!out || !*out) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl --dump-doc-html <template>'\n");
        free(out);
        return 1;
    }
    if (!strstr(out, "const DSLPROPS = [")) {
        fprintf(stderr, "FAIL: marqueur \"const DSLPROPS = [\" absent de la sortie "
                "--dump-doc-html (template introuvable ou format change ?)\n");
        free(out);
        return 1;
    }

    const char* obj = find_dslprop_object(out);
    if (!obj) {
        fprintf(stderr, "FAIL: entree DSLPROPS \"Projet\"/"
                "\"ActiveAvertissementConventionDeNommage\" introuvable\n");
        fails++;
    } else if (!strstr(obj, "12_prefixes_de_nommage")) {
        fprintf(stderr, "FAIL: DSLPROPS : l'entree de la propriete "
                "ActiveAvertissementConventionDeNommage/EnableNamingConventionWarning "
                "ne porte pas le lien fiche->guide \"12_prefixes_de_nommage\"\n");
        fails++;
    }
    free(out);

    printf("=== feat/2142 cross-ref propriete DSL (Projet.ActiveAvertissementConventionDeNommage -> 12_prefixes_de_nommage) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — mecanisme DSL_PROP pas encore branche\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (fiche propriete DSL Projet porte le lien fiche->guide dans le viewer)\n");
    return 0;
}
