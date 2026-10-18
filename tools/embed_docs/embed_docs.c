// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* embed_docs — outil de build : embarque docs/public_documentation/,
 * docs/public_templates/ et tests/scenarios/ dans le binaire owl via un
 * .c généré.
 *
 * Usage : embed_docs <repo_root> <output_c_file>
 *
 *   repo_root      : racine du repo OWL (contient docs/, tests/)
 *   output_c_file  : chemin du .c à générer (typiquement
 *                    build/embedded_docs.c)
 *
 * Le fichier produit définit :
 *   const OwlEmbeddedDoc EMBEDDED_PUBLIC_DOCS[]
 *   const OwlEmbeddedDoc EMBEDDED_SCENARIOS[]
 *   const OwlEmbeddedDoc EMBEDDED_TEMPLATES[]
 * (terminés par {NULL, ...} sentinel).
 *
 * Chaque entrée pointe sur un blob de bytes du fichier source (texte
 * brut). Le parsing au runtime se fait dans src/cli_doc.c — l'outil
 * de build n'interprète pas le contenu.
 *
 * Discipline :
 *   - Toolchain C-only (cf. CLAUDE.md). Aucune dépendance externe,
 *     juste libc et l'API filesystem POSIX/Windows portable.
 *   - Le walk est récursif pour tests/scenarios/<cat>/. Pour
 *     docs/public_documentation/, profondeur 1.
 *   - docs/public_documentation/ et docs/public_templates/ sont strictement
 *     flat : aucun sous-dossier n'est embarqué (recursive=0). C'est le
 *     contrat formel — les sous-dossiers (candidats/, drafts/, etc.) sont
 *     réservés à du contenu non publiable, exclu sans condition.
 *   - tests/scenarios/ est récursif mais ignore les fichiers et répertoires
 *     commençant par _ (drafts, brouillons).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #define PATH_SEP '/'
  #define stat _stat
#else
  #include <dirent.h>
  #include <unistd.h>
  #define PATH_SEP '/'
#endif

/* ------------------------------------------------------------------
 * Utilitaires fichier
 * ------------------------------------------------------------------ */

static char* read_whole_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = (long)got;
    return buf;
}

static int has_suffix(const char* s, const char* suf) {
    size_t ls = strlen(s), lsf = strlen(suf);
    if (lsf > ls) return 0;
    return strcmp(s + ls - lsf, suf) == 0;
}

/* Copie bornée d'une chaîne avec terminaison garantie. */
static void safe_strcpy(char* dst, size_t dst_size, const char* src) {
    if (dst_size == 0) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Un nom de fichier/dossier non-ASCII casse l'encodage du .c généré :
 * sous Windows, FindFirstFileA renvoie le nom en CP1252 (ex 0xE9 pour
 * 'é') et non en UTF-8, produisant un littéral C invalide ->
 * `-Werror,-Winvalid-source-encoding` opaque à la compile de
 * embedded_docs.c, sans lien apparent avec le changement.
 *
 * La convention des scénarios de test (et des docs/templates
 * embarqués) est un nommage ASCII pur. On refuse tôt, avec un message
 * actionnable, plutôt que de laisser fuiter l'octet fautif dans le .c
 * généré. `exit` fait échouer la custom command CMake -> build stoppé
 * proprement sur le vrai coupable. */
static void reject_non_ascii(const char* name) {
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        if (*p > 0x7F) {
            fprintf(stderr,
                "embed_docs: nom non-ASCII interdit : '%s'\n"
                "  -> renommer en ASCII pur. Les noms de fichiers et de "
                "dossiers embarqués (scénarios de test, docs, templates) "
                "doivent être ASCII : un accent y casse l'encodage du .c "
                "généré.\n", name);
            exit(2);
        }
    }
}

/* basename sans extension */
static void stem(const char* path, char* out, size_t out_size) {
    const char* base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    if (n + 1 > out_size) n = out_size - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------
 * Walk dirs
 * ------------------------------------------------------------------ */

#define MAX_ENTRIES 1024

typedef struct {
    char path[1024];     /* relatif au repo_root, séparateur '/' */
    char slug[256];      /* identifiant C unique */
} Entry;

static int collect_dir(const char* repo_root, const char* rel_dir,
                       int recursive, const char* ext,
                       Entry* out, int* out_count, int max);

#ifdef _WIN32
static int collect_dir(const char* repo_root, const char* rel_dir,
                       int recursive, const char* ext,
                       Entry* out, int* out_count, int max) {
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s/%s/*", repo_root, rel_dir);
    /* Normalise slashes pour FindFirstFile */
    for (char* p = pattern; *p; p++) if (*p == '/') *p = '\\';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;
        reject_non_ascii(fd.cFileName);

        char rel_path[1024];
        snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* recursive=0 (public_documentation / public_templates) =
             * contrat flat strict : tout sous-dossier est skipped sans
             * condition (candidats/, drafts/, archived/, …). */
            if (recursive) {
                collect_dir(repo_root, rel_path, recursive, ext,
                            out, out_count, max);
            }
            continue;
        }
        if (!has_suffix(fd.cFileName, ext)) continue;
        if (*out_count >= max) {
            fprintf(stderr, "embed_docs: trop d'entrées (>%d)\n", max);
            FindClose(h);
            return 0;
        }
        safe_strcpy(out[*out_count].path, sizeof(out[0].path), rel_path);
        (*out_count)++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return 1;
}
#else
static int collect_dir(const char* repo_root, const char* rel_dir,
                       int recursive, const char* ext,
                       Entry* out, int* out_count, int max) {
    char abs_dir[2048];
    snprintf(abs_dir, sizeof(abs_dir), "%s/%s", repo_root, rel_dir);

    DIR* d = opendir(abs_dir);
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        reject_non_ascii(e->d_name);

        char rel_path[1024];
        snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, e->d_name);

        struct stat st;
        char abs_path[2048];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", repo_root, rel_path);
        if (stat(abs_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* recursive=0 (public_documentation / public_templates) =
             * contrat flat strict : tout sous-dossier est skipped sans
             * condition (candidats/, drafts/, archived/, …). */
            if (recursive) {
                collect_dir(repo_root, rel_path, recursive, ext,
                            out, out_count, max);
            }
            continue;
        }
        if (!has_suffix(e->d_name, ext)) continue;
        if (*out_count >= max) {
            fprintf(stderr, "embed_docs: trop d'entrées (>%d)\n", max);
            closedir(d);
            return 0;
        }
        safe_strcpy(out[*out_count].path, sizeof(out[0].path), rel_path);
        (*out_count)++;
    }
    closedir(d);
    return 1;
}
#endif

/* Tri lexicographique simple — produit un ordre déterministe quel que
 * soit l'OS, sinon les diffs de build deviennent du bruit. */
static int compare_entries(const void* a, const void* b) {
    const Entry* ea = (const Entry*)a;
    const Entry* eb = (const Entry*)b;
    return strcmp(ea->path, eb->path);
}

/* ------------------------------------------------------------------
 * Émission
 * ------------------------------------------------------------------ */

static void emit_blob(FILE* out, const char* slug,
                      const char* data, long len) {
    fprintf(out, "static const unsigned char BLOB_%s[] = {", slug);
    for (long i = 0; i < len; i++) {
        if ((i % 16) == 0) fprintf(out, "\n    ");
        fprintf(out, "0x%02X,", (unsigned char)data[i]);
    }
    fprintf(out, "\n};\n");
    fprintf(out, "static const long BLOB_%s_LEN = %ld;\n\n", slug, len);
}

/* strip_en : si vrai, le `name` exposé est le stem débarrassé de son
 * suffixe `.en` (variante linguistique appariée au stem FR). Le stem()
 * ne retire que l'extension `.md`, laissant `12_x.en` ; on coupe ici le
 * `.en` pour que la table EN s'apparie au stem FR par simple strcmp. */
static int emit_section(FILE* out, const char* repo_root,
                        const char* section_name,
                        const Entry* entries, int count, int strip_en) {
    /* Émet d'abord les blobs, puis la table de pointeurs. */
    for (int i = 0; i < count; i++) {
        long len = 0;
        char abs_path[2048];
        snprintf(abs_path, sizeof(abs_path), "%s/%s",
                 repo_root, entries[i].path);
        char* data = read_whole_file(abs_path, &len);
        if (!data) {
            fprintf(stderr, "embed_docs: impossible de lire %s\n", abs_path);
            return 0;
        }
        emit_blob(out, entries[i].slug, data, len);
        free(data);
    }

    /* Table */
    fprintf(out, "const OwlEmbeddedDoc EMBEDDED_%s[] = {\n", section_name);
    for (int i = 0; i < count; i++) {
        char file_stem[256];
        stem(entries[i].path, file_stem, sizeof(file_stem));
        if (strip_en && has_suffix(file_stem, ".en"))
            file_stem[strlen(file_stem) - 3] = '\0';
        fprintf(out, "    { \"%s\", \"%s\", BLOB_%s, BLOB_%s_LEN },\n",
                file_stem, entries[i].path,
                entries[i].slug, entries[i].slug);
    }
    fprintf(out, "    { NULL, NULL, NULL, 0 }\n};\n\n");
    fprintf(out, "const int EMBEDDED_%s_COUNT = %d;\n\n",
            section_name, count);
    return 1;
}

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: embed_docs <repo_root> <output.c>\n");
        return 1;
    }
    const char* repo_root = argv[1];
    const char* out_path  = argv[2];

    /* Collecte public_documentation (profondeur 1). Les variantes
     * linguistiques `NN_slug.en.md` sont ramassées ici puis partitionnées :
     * elles n'alimentent PAS la table FR (sinon elles apparaîtraient comme
     * fiches autonomes dans `owl learn`, le count, les related-docs...), mais
     * une table EN parallèle appariée au stem FR côté main.c. */
    static Entry public_docs_all[MAX_ENTRIES];
    int n_all = 0;
    collect_dir(repo_root, "docs/public_documentation", 0, ".md",
                public_docs_all, &n_all, MAX_ENTRIES);
    qsort(public_docs_all, (size_t)n_all, sizeof(Entry), compare_entries);

    static Entry public_docs[MAX_ENTRIES];
    static Entry public_docs_en[MAX_ENTRIES];
    int n_pubdocs = 0, n_pubdocs_en = 0;
    for (int i = 0; i < n_all; i++) {
        if (has_suffix(public_docs_all[i].path, ".en.md"))
            public_docs_en[n_pubdocs_en++] = public_docs_all[i];
        else
            public_docs[n_pubdocs++] = public_docs_all[i];
    }

    /* Collecte scénarios (récursif) */
    static Entry scenarios[MAX_ENTRIES];
    int n_scen = 0;
    collect_dir(repo_root, "tests/scenarios", 1, ".owl",
                scenarios, &n_scen, MAX_ENTRIES);
    qsort(scenarios, (size_t)n_scen, sizeof(Entry), compare_entries);

    /* Collecte templates (profondeur 1, .owl) — squelettes prêts à
     * l'emploi exposés via `owl template <nom>`. */
    static Entry templates[MAX_ENTRIES];
    int n_tpl = 0;
    collect_dir(repo_root, "docs/public_templates", 0, ".owl",
                templates, &n_tpl, MAX_ENTRIES);
    qsort(templates, (size_t)n_tpl, sizeof(Entry), compare_entries);

    /* Génère slugs uniques (base 0..N). raw[] est volontairement plus
     * petit que slug[] pour garantir qu'aucune troncature n'a lieu. */
    for (int i = 0; i < n_pubdocs; i++) {
        char raw[128];
        stem(public_docs[i].path, raw, sizeof(raw));
        snprintf(public_docs[i].slug, sizeof(public_docs[i].slug),
                 "D_%s", raw);
        /* sanitize : remplace tout char non identifiant C */
        for (char* p = public_docs[i].slug; *p; p++) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
                *p = '_';
        }
    }
    for (int i = 0; i < n_pubdocs_en; i++) {
        char raw[128];
        stem(public_docs_en[i].path, raw, sizeof(raw));
        snprintf(public_docs_en[i].slug, sizeof(public_docs_en[i].slug),
                 "DEN_%s", raw);
        for (char* p = public_docs_en[i].slug; *p; p++) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
                *p = '_';
        }
    }
    for (int i = 0; i < n_scen; i++) {
        char raw[128];
        stem(scenarios[i].path, raw, sizeof(raw));
        snprintf(scenarios[i].slug, sizeof(scenarios[i].slug),
                 "S_%s_%d", raw, i);
        for (char* p = scenarios[i].slug; *p; p++) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
                *p = '_';
        }
    }
    for (int i = 0; i < n_tpl; i++) {
        char raw[128];
        stem(templates[i].path, raw, sizeof(raw));
        snprintf(templates[i].slug, sizeof(templates[i].slug),
                 "T_%s", raw);
        for (char* p = templates[i].slug; *p; p++) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
                *p = '_';
        }
    }

    /* Émission */
    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "embed_docs: impossible d'ouvrir %s en écriture\n",
                out_path);
        return 1;
    }

    fprintf(out,
        "/* Fichier généré automatiquement par tools/embed_docs.\n"
        " * NE PAS ÉDITER À LA MAIN — sera écrasé au prochain build.\n"
        " * Sources : docs/public_documentation/ (.md) + tests/scenarios/ (.owl)\n"
        " *           + docs/public_templates/ (.owl).\n"
        " */\n\n"
        "#include \"embedded_docs.h\"\n\n");

    if (!emit_section(out, repo_root, "PUBLIC_DOCS",
                      public_docs, n_pubdocs, 0)) {
        fclose(out);
        return 1;
    }
    if (!emit_section(out, repo_root, "PUBLIC_DOCS_EN",
                      public_docs_en, n_pubdocs_en, 1)) {
        fclose(out);
        return 1;
    }
    if (!emit_section(out, repo_root, "SCENARIOS", scenarios, n_scen, 0)) {
        fclose(out);
        return 1;
    }
    if (!emit_section(out, repo_root, "TEMPLATES", templates, n_tpl, 0)) {
        fclose(out);
        return 1;
    }

    fclose(out);
    fprintf(stderr, "embed_docs: %d public_doc(s) (+%d variante(s) EN) + "
                    "%d scénario(s) + %d template(s) -> %s\n",
            n_pubdocs, n_pubdocs_en, n_scen, n_tpl, out_path);
    return 0;
}
