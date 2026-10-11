/*  (ST1 de l'umbrella , ) — parite CLI/viewer des
 * liens fiche->guide. Aujourd'hui `find_related_public_docs` (src/cli_doc.c)
 * n'alimente QUE le CLI (`owl explain` texte + JSON via `print_related_json`).
 * Le JSON par-symbole consomme par le viewer (`DATA[]`, genere par
 * `dump_doc_html` dans src/main.c via `owl --dump-doc-html <template>`) n'a
 * AUCUN emetteur pour ces liens (confirme : `cli_doc_emit_examples_json` et
 * `cli_doc_emit_neighbors_json` existent, aucun equivalent
 * `cli_doc_emit_public_docs_json` n'est appele dans dump_doc_html a ce jour).
 *
 * Ce scanner reutilise le meme pin que crossref_pin_scan.c
 * (RegexRemplace -> 05_operateurs, cf. ce fichier pour la justification du
 * choix) et verifie que l'objet JSON de RegexRemplace dans `DATA[]` (sortie
 * de `owl --dump-doc-html docs/reference/_owl-doc-template.html`) porte bien
 * le lien vers "05_operateurs" — pas le rendu HTML visuel (hors de portee
 * d'un scanner texte), seulement l'emission JSON consommee par le viewer.
 *
 * ROUGE attendu tant que l'emetteur n'existe pas : l'objet JSON de
 * RegexRemplace ne contient "05_operateurs" nulle part.
 *
 * Boite noire, calque explain_json_lang_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh.
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

/* Deduit la racine du repo depuis le chemin de owl.exe (.../build/owl.exe ->
 * ...) pour localiser le template HTML sans dependre du cwd du scanner. */
static void derive_repo_root(const char* owlBin, char* out, size_t outsz) {
    const char* marker = "/build/";
    const char* p = strstr(owlBin, marker);
    /* Prend la DERNIERE occurrence (chemin peut contenir "build" ailleurs). */
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

/* Isole l'objet JSON `{"kind":"native","name":"RegexRemplace",...}` dans
 * DATA[] : cherche le marqueur de nom, puis borne a l'accolade fermante de
 * fin d'objet correspondante (comptage de profondeur, naif mais suffisant :
 * pas de chaines contenant '{'/'}' non-echappees dans ce corpus desc). */
static char g_obj_buf[1 << 16];
static const char* find_native_object(const char* data, const char* name) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"name\":\"%s\"", name);
    const char* hit = strstr(data, needle);
    if (!hit) return NULL;
    /* Recule jusqu'a l'accolade ouvrante de l'objet. */
    const char* objStart = hit;
    while (objStart > data && *objStart != '{') objStart--;
    if (*objStart != '{') return NULL;
    /* Avance jusqu'a l'accolade fermante correspondante. */
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
    if (!strstr(out, "const DATA = [")) {
        fprintf(stderr, "FAIL: marqueur \"const DATA = [\" absent de la sortie --dump-doc-html "
                "(template introuvable ou format change ?)\n");
        free(out);
        return 1;
    }

    const char* obj = find_native_object(out, "RegexRemplace");
    if (!obj) {
        fprintf(stderr, "FAIL: objet JSON \"RegexRemplace\" introuvable dans DATA[]\n");
        fails++;
    } else if (!strstr(obj, "05_operateurs")) {
        fprintf(stderr, "FAIL: DATA[] : objet RegexRemplace ne porte pas le lien "
                "fiche->guide \"05_operateurs\" (parite viewer absente)\n");
        fails++;
    }
    free(out);

    printf("=== feat/2140 parite viewer JSON (DATA[] RegexRemplace -> 05_operateurs) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec) — emetteur JSON viewer pas encore branche\n",
               fails);
        return 1;
    }
    printf("RESULT: GREEN (DATA[] porte le lien fiche->guide, parite CLI/viewer retablie)\n");
    return 0;
}
