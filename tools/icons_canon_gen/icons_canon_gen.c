// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icons_canon_gen — outil de build .
 *
 * Parse `tools/icons_canon_fr_en/mapping.def` (table FR -> [EN+]) et
 * genere `${CMAKE_BINARY_DIR}/icons_canon_gen.c` qui materialise les
 * tables dans owl.exe :
 *
 *   typedef struct {
 *       const char* fr;
 *       const char* const* en;
 *       int en_count;
 *   } OwlIconCanonEntry;
 *
 *   extern const OwlIconCanonEntry OWL_ICONS_FR_EN_MAPPING[];
 *   extern const int OWL_ICONS_FR_EN_COUNT;
 *
 * La table est triee par `fr` ASC pour bsearch.
 *
 * Format mapping.def :
 *   - Lignes vides et commencant par # ignorees.
 *   - Une entree par ligne : `<fr_lowercase_ascii>: <en1>, <en2>, ...`
 *   - Pas de doublon FR (verifie par l'outil, abort si conflit).
 *
 * Usage : icons_canon_gen <mapping_def_path> <output_c_path>
 *
 * Discipline : toolchain C-only, libc seule, pas d'allocation deraisonnable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------
 * Storage parse
 * ------------------------------------------------------------------ */

#define MAX_ENTRIES   5000   /* marge confort vs ~3100 actuel */
#define MAX_EN_PER_FR 16     /* synonymes EN par terme FR */

typedef struct {
    char fr[64];
    char en[MAX_EN_PER_FR][48];
    int  en_count;
} Entry;

static Entry  g_entries[MAX_ENTRIES];
static int    g_entry_count = 0;

/* ------------------------------------------------------------------
 * Parse helpers
 * ------------------------------------------------------------------ */

static char* read_whole_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    *out_len = sz;
    return buf;
}

static void trim(char* s) {
    /* Trim leading + trailing whitespace in-place. */
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int qsort_cmp_entry(const void* a, const void* b) {
    const Entry* ea = (const Entry*)a;
    const Entry* eb = (const Entry*)b;
    return strcmp(ea->fr, eb->fr);
}

/* ------------------------------------------------------------------
 * Parse mapping.def
 * ------------------------------------------------------------------ */

static int parse_mapping(const char* def_path) {
    long sz = 0;
    char* buf = read_whole_file(def_path, &sz);
    if (!buf) {
        fprintf(stderr, "icons_canon_gen: cannot read %s\n", def_path);
        return 0;
    }

    int line_no = 0;
    char* p = buf;
    while (*p) {
        line_no++;
        /* Extract one line. */
        char* nl = strchr(p, '\n');
        char line[1024];
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        /* Strip CR. */
        size_t lnz = strlen(line);
        if (lnz > 0 && line[lnz - 1] == '\r') line[--lnz] = '\0';

        trim(line);

        if (line[0] == '\0' || line[0] == '#') continue;

        /* Split on first ':'. */
        char* colon = strchr(line, ':');
        if (!colon) {
            fprintf(stderr, "icons_canon_gen: line %d malformed (no ':'): %s\n",
                    line_no, line);
            free(buf);
            return 0;
        }
        *colon = '\0';
        char* fr = line;
        char* en_list = colon + 1;
        trim(fr);
        trim(en_list);

        if (g_entry_count >= MAX_ENTRIES) {
            fprintf(stderr, "icons_canon_gen: too many entries (>%d)\n",
                    MAX_ENTRIES);
            free(buf);
            return 0;
        }
        Entry* e = &g_entries[g_entry_count];
        if (strlen(fr) >= sizeof(e->fr)) {
            fprintf(stderr, "icons_canon_gen: line %d FR too long: %s\n",
                    line_no, fr);
            free(buf);
            return 0;
        }
        strcpy(e->fr, fr);
        e->en_count = 0;

        /* Split en_list on ','. */
        char* tok = en_list;
        while (*tok) {
            char* comma = strchr(tok, ',');
            char en_buf[256];
            size_t enlen = comma ? (size_t)(comma - tok) : strlen(tok);
            if (enlen >= sizeof(en_buf)) enlen = sizeof(en_buf) - 1;
            memcpy(en_buf, tok, enlen);
            en_buf[enlen] = '\0';
            trim(en_buf);
            if (en_buf[0] != '\0') {
                if (e->en_count >= MAX_EN_PER_FR) {
                    fprintf(stderr,
                            "icons_canon_gen: line %d too many EN tokens (>%d): %s\n",
                            line_no, MAX_EN_PER_FR, fr);
                    free(buf);
                    return 0;
                }
                if (strlen(en_buf) >= sizeof(e->en[0])) {
                    fprintf(stderr,
                            "icons_canon_gen: line %d EN token too long: %s\n",
                            line_no, en_buf);
                    free(buf);
                    return 0;
                }
                strcpy(e->en[e->en_count++], en_buf);
            }
            if (!comma) break;
            tok = comma + 1;
        }

        if (e->en_count == 0) {
            fprintf(stderr,
                    "icons_canon_gen: line %d FR '%s' has no EN tokens\n",
                    line_no, fr);
            free(buf);
            return 0;
        }
        g_entry_count++;
    }

    free(buf);

    /* Sort by FR ASC for bsearch. */
    qsort(g_entries, (size_t)g_entry_count, sizeof(Entry), qsort_cmp_entry);

    /* Check for duplicates. */
    for (int i = 1; i < g_entry_count; i++) {
        if (strcmp(g_entries[i - 1].fr, g_entries[i].fr) == 0) {
            fprintf(stderr,
                    "icons_canon_gen: duplicate FR key '%s'\n",
                    g_entries[i].fr);
            return 0;
        }
    }

    return 1;
}

/* ------------------------------------------------------------------
 * Emit C
 * ------------------------------------------------------------------ */

static int emit_c(const char* out_path) {
    FILE* f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "icons_canon_gen: cannot write %s\n", out_path);
        return 0;
    }

    fprintf(f, "/* AUTO-GENERATED by tools/icons_canon_gen — DO NOT EDIT.\n");
    fprintf(f, " * Source : tools/icons_canon_fr_en/mapping.def (feat/0343).\n");
    fprintf(f, " * Regenere automatiquement par CMake si mapping.def change.\n");
    fprintf(f, " */\n\n");
    fprintf(f, "#include <stddef.h>\n");
    fprintf(f, "#include \"icons_canon.h\"\n\n");

    /* Emit per-entry EN arrays. */
    for (int i = 0; i < g_entry_count; i++) {
        const Entry* e = &g_entries[i];
        fprintf(f, "static const char* const k_en_%d[] = { ", i);
        for (int j = 0; j < e->en_count; j++) {
            fprintf(f, "\"%s\", ", e->en[j]);
        }
        fprintf(f, "NULL };\n");
    }

    fprintf(f, "\nconst OwlIconCanonEntry OWL_ICONS_FR_EN_MAPPING[] = {\n");
    for (int i = 0; i < g_entry_count; i++) {
        const Entry* e = &g_entries[i];
        fprintf(f, "    { \"%s\", k_en_%d, %d },\n",
                e->fr, i, e->en_count);
    }
    fprintf(f, "};\n\n");

    fprintf(f, "const int OWL_ICONS_FR_EN_COUNT = %d;\n", g_entry_count);

    fclose(f);
    fprintf(stderr, "icons_canon_gen: emitted %d FR entries -> %s\n",
            g_entry_count, out_path);
    return 1;
}

/* ------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <mapping.def> <out.c>\n", argv[0]);
        return 1;
    }
    const char* def_path = argv[1];
    const char* out_path = argv[2];

    if (!parse_mapping(def_path)) return 2;
    if (!emit_c(out_path)) return 3;

    return 0;
}
