// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_gen_assets.c — , .
 *
 * Implementation de `owl --gen-icon-assets <out-dir>` : dump le
 * catalogue Iconify embarque (47728 icones) sous forme PACKEE (un blob
 * unique + un index JSON), pret a packager dans le VSIX du plugin
 * VSCode designer panel.
 *
 *  : on n'explose plus le catalogue en 47k fichiers loose
 * `svg/<cat>/<nom>.svg` (install VSCode fragile — l'antivirus scanne
 * 47k fichiers). On emet le format packe deja present en memoire : le
 * blob raw decompresse tel quel + l'index enrichi des offsets. Le
 * plugin compose `<svg viewBox="0 0 w h">{body}</svg>` a la demande.
 *
 * Structure output :
 *   <out-dir>/icons.blob   (bodies SVG concatenes, decompresses)
 *   <out-dir>/catalog.json (index : n/c/k/w/h + o=offset, l=length)
 *   <out-dir>/fr_en.json   (mapping bilingue)
 *   <out-dir>/.stamp
 *
 * Contrat plugin : body de l'icone = icons.blob[o : o+l] (fragment
 * `<path .../>...` sans enveloppe `<svg>`). Le plugin wrap lui-meme.
 *
 * Categorisation OWL : 6 categories (ui/fichiers/marques/emoji/jeu/
 * classiques) au lieu d'exposer les 12 noms de sets Iconify (regle 7
 * OWL : pas de marques tierces user-facing).
 *
 * Cache : si <out-dir>/.stamp existe avec hash identique au contenu
 * source (blob_size + catalog_count + mapping_count + version + tag de
 * format), skip silent. Sinon regen complet.
 */

#include "cli_gen_assets.h"

#include "common.h"           /* OWL_VERSION */
#include "icons_catalog.h"    /* OWL_ICONS_CATALOG, OWL_ICONS_SET_VIEWBOX */
#include "icons_canon.h"      /* OWL_ICONS_FR_EN_MAPPING */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define MKDIR(p) _mkdir(p)
#else
  #include <unistd.h>
  #define MKDIR(p) mkdir(p, 0755)
#endif

/* ============================================================
 * Categories OWL-native (copie locale, dupliquee depuis cli_doc.c).
 * 6 entrees, dette acceptable V1. Si besoin, factoriser plus tard
 * dans src/icons_category.h partage.
 * ============================================================ */
typedef struct {
    const char* owl_name;
    signed char set_ids[8];   /* set_id Iconify membres ; -1 = end */
} OwlIconCategoryDef;

static const OwlIconCategoryDef GEN_CATEGORIES[] = {
    /* set_id : 0=material-symbols, 1=mdi, 2=fluent, 3=vscode-icons,
     *          4=logos, 5=devicon, 6=fluent-emoji-flat, 7=carbon,
     *          8=game-icons, 9=simple-icons, 10=line-md, 11=flat-color-icons */
    { "ui",         {0, 1, 2, 7, 10, -1, -1, -1} },
    { "fichiers",   {3, -1, -1, -1, -1, -1, -1, -1} },
    { "marques",    {4, 5, 9, -1, -1, -1, -1, -1} },
    { "emoji",      {6, -1, -1, -1, -1, -1, -1, -1} },
    { "jeu",        {8, -1, -1, -1, -1, -1, -1, -1} },
    { "classiques", {11, -1, -1, -1, -1, -1, -1, -1} },
};
#define GEN_CATEGORIES_COUNT \
    ((int)(sizeof(GEN_CATEGORIES) / sizeof(GEN_CATEGORIES[0])))

static const char* gen_set_id_to_category(int set_id) {
    for (int c = 0; c < GEN_CATEGORIES_COUNT; c++) {
        const signed char* ids = GEN_CATEGORIES[c].set_ids;
        for (int i = 0; i < 8 && ids[i] >= 0; i++) {
            if (ids[i] == set_id) return GEN_CATEGORIES[c].owl_name;
        }
    }
    return "?";
}

/* ============================================================
 * mkdir -p portable (cree les parents si necessaire).
 * ============================================================ */
static int ensure_dir(const char* path) {
    /* Tente direct. Si reussit OU EEXIST : OK. */
    if (MKDIR(path) == 0) return 0;
    if (errno == EEXIST) return 0;
    /* Sinon, decompose le path et cree les parents. */
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    /* Skip drive letter Windows (C:\ ou C:/). */
    size_t i = 0;
    if (len >= 3 && tmp[1] == ':' && (tmp[2] == '\\' || tmp[2] == '/')) {
        i = 3;
    } else if (tmp[0] == '/' || tmp[0] == '\\') {
        i = 1;
    }
    for (; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0') {
                if (MKDIR(tmp) != 0 && errno != EEXIST) {
                    tmp[i] = saved;
                    /* Cannot create : silent skip, on retentera direct */
                }
            }
            tmp[i] = saved;
        }
    }
    /* Tente une derniere fois. */
    if (MKDIR(path) == 0 || errno == EEXIST) return 0;
    return -1;
}

/* ============================================================
 * Stamp cache : hash heuristique grossier sur le contenu source.
 * Pas SHA256 cryptographique, juste un ID stable suffisant pour
 * detecter une regeneration necessaire.
 * ============================================================ */
static unsigned long long compute_source_hash(void) {
    unsigned long long h = 1469598103934665603ULL;  /* FNV-1a seed */
    const unsigned long long fnv_p = 1099511628211ULL;
    /* Tag de format : change des que la structure output change. Invalide
     * les .stamp de l'ancien format loose svg/. */
    for (const char* p = "packed-blob-v1"; *p; p++) {
        h ^= (unsigned long long)(unsigned char)*p;
        h *= fnv_p;
    }
    /* Incorpore OWL_VERSION (changement de version = regen). */
    for (const char* p = OWL_VERSION; *p; p++) {
        h ^= (unsigned long long)(unsigned char)*p;
        h *= fnv_p;
    }
    /* Incorpore le count catalogue + mapping (changement = regen). */
    unsigned long long params[3] = {
        (unsigned long long)OWL_ICONS_CATALOG_COUNT,
        (unsigned long long)OWL_ICONS_FR_EN_COUNT,
        (unsigned long long)OWL_ICONS_BLOB_RAW_SIZE,
    };
    for (int i = 0; i < 3; i++) {
        for (int b = 0; b < 8; b++) {
            h ^= (params[i] >> (b * 8)) & 0xFF;
            h *= fnv_p;
        }
    }
    return h;
}

static bool stamp_matches(const char* stamp_path, unsigned long long want_hash) {
    FILE* f = fopen(stamp_path, "rb");
    if (!f) return false;
    unsigned long long got = 0;
    bool ok = (fread(&got, sizeof(got), 1, f) == 1);
    fclose(f);
    return ok && got == want_hash;
}

static bool stamp_write(const char* stamp_path, unsigned long long hash) {
    FILE* f = fopen(stamp_path, "wb");
    if (!f) return false;
    size_t n = fwrite(&hash, sizeof(hash), 1, f);
    fclose(f);
    return n == 1;
}

/* ============================================================
 * Write helpers
 * ============================================================ */

/* Ecrit le blob raw decompresse tel quel en UN fichier. Les offsets de
 * OWL_ICONS_CATALOG pointent dedans (body = icons.blob[o : o+l]), ils
 * restent valides. Le plugin compose le `<svg>` a la volee. */
static bool emit_icons_blob(const char* out_dir,
                            const unsigned char* blob, size_t blob_size) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/icons.blob", out_dir);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = (fwrite(blob, 1, blob_size, f) == blob_size);
    fclose(f);
    return ok;
}

/* Escape un nom de fichier (kebab-case Iconify) pour JSON. Les noms
 * Iconify ne contiennent jamais de char a echapper (kebab-case ASCII
 * pur). On copie tel quel pour V1, ajoute escape complet si besoin. */
static void json_emit_string(FILE* f, const char* s) {
    fputc('"', f);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static bool emit_catalog_json(const char* out_dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/catalog.json", out_dir);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "{\n");
    fprintf(f, "  \"owl_version\": \"%s\",\n", OWL_VERSION);
    fprintf(f, "  \"total\": %zu,\n", OWL_ICONS_CATALOG_COUNT);
    fprintf(f, "  \"categories\": [");
    for (int c = 0; c < GEN_CATEGORIES_COUNT; c++) {
        if (c > 0) fputc(',', f);
        fputc('"', f);
        fputs(GEN_CATEGORIES[c].owl_name, f);
        fputc('"', f);
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"icons\": [\n");
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[i];
        const char* cat = gen_set_id_to_category(e->set_id);
        int vw = (e->width  > 0) ? e->width  : OWL_ICONS_SET_VIEWBOX[e->set_id];
        int vh = (e->height > 0) ? e->height : OWL_ICONS_SET_VIEWBOX[e->set_id];
        if (vw <= 0) vw = 24;
        if (vh <= 0) vh = 24;
        fprintf(f, "    {\"n\":");
        json_emit_string(f, e->name);
        fprintf(f, ",\"c\":\"%s\",\"k\":%s,\"w\":%d,\"h\":%d,\"o\":%u,\"l\":%u}",
                cat, e->couleur_modifiable ? "true" : "false", vw, vh,
                (unsigned)e->offset, (unsigned)e->length);
        if (i + 1 < OWL_ICONS_CATALOG_COUNT) fputc(',', f);
        fputc('\n', f);
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

static bool emit_fr_en_json(const char* out_dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/fr_en.json", out_dir);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "{\n");
    fprintf(f, "  \"owl_version\": \"%s\",\n", OWL_VERSION);
    fprintf(f, "  \"total\": %d,\n", OWL_ICONS_FR_EN_COUNT);
    fprintf(f, "  \"mapping\": [\n");
    for (int i = 0; i < OWL_ICONS_FR_EN_COUNT; i++) {
        const OwlIconCanonEntry* m = &OWL_ICONS_FR_EN_MAPPING[i];
        fprintf(f, "    {\"fr\":");
        json_emit_string(f, m->fr);
        fprintf(f, ",\"en\":[");
        for (int j = 0; j < m->en_count; j++) {
            if (j > 0) fputc(',', f);
            json_emit_string(f, m->en[j]);
        }
        fprintf(f, "]}");
        if (i + 1 < OWL_ICONS_FR_EN_COUNT) fputc(',', f);
        fputc('\n', f);
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

/* ============================================================
 * Main entry
 * ============================================================ */
int cli_gen_icon_assets(const char* out_dir) {
    if (out_dir == NULL || out_dir[0] == '\0') {
        fprintf(stderr, "owl --gen-icon-assets : <out-dir> requis.\n");
        fprintf(stderr, "  Ex : owl --gen-icon-assets ../owl-integration/vscode/icons-catalog/\n");
        return 64;
    }

    /* Cache stamp. */
    unsigned long long want_hash = compute_source_hash();
    char stamp_path[1024];
    snprintf(stamp_path, sizeof(stamp_path), "%s/.stamp", out_dir);
    if (stamp_matches(stamp_path, want_hash)) {
        fprintf(stderr, "gen-icon-assets: cache hit (%016llx) — skip\n",
                want_hash);
        return 0;
    }

    /* Cree out_dir. Plus de sous-dossiers svg/ : format packe. */
    if (ensure_dir(out_dir) != 0) {
        fprintf(stderr, "gen-icon-assets: cannot create %s\n", out_dir);
        return 73;
    }

    /* Decompresse le blob. */
    const unsigned char* blob = owl_icones_blob_raw_ensure();
    if (blob == NULL) {
        fprintf(stderr, "gen-icon-assets: blob decompression KO\n");
        return 74;
    }

    /* Emet le blob raw tel quel en UN fichier. Les offsets/length de
     * OWL_ICONS_CATALOG (exportes dans catalog.json ci-dessous) reperent
     * chaque body : body = icons.blob[o : o+l]. */
    if (!emit_icons_blob(out_dir, blob, OWL_ICONS_BLOB_RAW_SIZE)) {
        fprintf(stderr, "gen-icon-assets: emit icons.blob KO\n");
        return 74;
    }

    /* Emet catalog.json (index enrichi o/l) + fr_en.json. */
    if (!emit_catalog_json(out_dir)) {
        fprintf(stderr, "gen-icon-assets: emit catalog.json KO\n");
        return 75;
    }
    if (!emit_fr_en_json(out_dir)) {
        fprintf(stderr, "gen-icon-assets: emit fr_en.json KO\n");
        return 75;
    }

    /* Stamp. */
    stamp_write(stamp_path, want_hash);

    fprintf(stderr, "gen-icon-assets: %zu icones / %d categories packees "
                    "dans %s (icons.blob %.1f MB + catalog.json)\n",
            OWL_ICONS_CATALOG_COUNT, GEN_CATEGORIES_COUNT, out_dir,
            (double)OWL_ICONS_BLOB_RAW_SIZE / (1024.0 * 1024.0));
    return 0;
}
