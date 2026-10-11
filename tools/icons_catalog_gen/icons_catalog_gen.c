// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* icons_catalog_gen — outil de build  Lot 2a-i.
 *
 * Genere `${CMAKE_BINARY_DIR}/icons_catalog_gen.c` qui materialise le
 * catalogue d'icones Iconify embarque dans owl.exe : table d'entrees
 * (nom, set_id, hash xxh3-64, offset, length, couleur_modifiable),
 * blob compresse zstd des bodies SVG concatenes, table des sets.
 *
 * Usage : icons_catalog_gen <vendor_iconify_dir> <output_c_file>
 *
 *   vendor_iconify_dir : repertoire des JSON sources (vendor/iconify-sets/).
 *   output_c_file      : chemin du .c a generer (build/icons_catalog_gen.c).
 *
 * Pipeline :
 *   1. Calcule SHA256 de la concatenation des 12 JSON sources.
 *   2. Si <output_c_file>.stamp existe avec SHA256 identique ET que
 *      <output_c_file> existe : skip silencieux (cache hit, ~0s).
 *   3. Parse en streaming chaque JSON (mini parser ad-hoc dedie a la
 *      structure ultra-reguliere Iconify : `"prefix":...`, `"icons":{...}`,
 *      chaque entree `"<name>":{"body":"<...>", ...}`). Pas de full JSON,
 *      juste detection de cles + lecture string echappee.
 *   4. Applique les filtres : fluent -24- only, line-md skip si body
 *      contient `<animate` (anim SMIL).
 *   5. Concatene les bodies en un blob raw (zone unique).
 *   6. Compresse zstd niveau 19.
 *   7. Calcule XXH64 (seed 0) par body wrappe avec son enveloppe svg
 *      (`<svg viewBox='0 0 W H'>BODY</svg>` ou W/H sont la taille
 *      par defaut du set, fallback 24x24). Choix XXH64 vs XXH3 : le
 *      header xxhash vendor zstd patche `XXH_NO_XXH3` qui masque XXH3 ;
 *      XXH64 reste expose et suffit pour le content-addressing.
 *   8. Emet le .c : tableau OWL_ICONS_CATALOG + blob OWL_ICONS_BLOB_ZSTD
 *      + table OWL_ICONS_SET_NAMES.
 *   9. Ecrit <output_c_file>.stamp avec le SHA256.
 *
 * Discipline :
 *   - Toolchain C-only (cf. CLAUDE.md). Dependances : libc + xxhash.h
 *     (header-only inline depuis vendor/zstd) + zstd compress (linked
 *     statique via les .c amalgames cote CMakeLists). SHA256 embed mini
 *     impl public domain (cf. plus bas).
 *   - Pas d'avalage 56 MB en RAM : le parser lit le fichier d'un coup
 *     mais reste contigu en memoire jusqu'a fin du set, puis libere.
 *     Worst-case pic ~12 MB (fluent.json). Le coordinator dit
 *     "streaming" mais en pratique avec 12 MB max c'est OK de lire d'un
 *     coup ; le surplus est temporaire (free apres parse).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>

/* memmem n'existe pas sur MinGW / certains BSD. On le redefinit nous-meme
 * en portable. Suffit pour les usages internes (sous-chaines courtes). */
static void* owl_memmem(const void* h, size_t hl, const void* n, size_t nl) {
    if (nl == 0) return (void*)h;
    if (hl < nl) return NULL;
    const unsigned char* hp = (const unsigned char*)h;
    const unsigned char* np = (const unsigned char*)n;
    for (size_t i = 0; i + nl <= hl; ++i) {
        if (hp[i] == np[0] && memcmp(hp + i, np, nl) == 0)
            return (void*)(hp + i);
    }
    return NULL;
}
#define memmem owl_memmem

/* xxhash : on link contre xxhash.c amalgame avec zstd_compress (cf.
 * CMakeLists.txt ZSTD_COMPRESS_SRCS). On utilise XXH64 (xxhash 64-bit
 * classique) plutot que XXH3_64bits : le header local zstd patche
 * `#define XXH_NO_XXH3` qui rend l'API XXH3 invisible, et le patcher
 * en consommateur n'est pas trivial (le define est conditionnel mais
 * `#ifndef`). XXH64 fait le job pour l'usage : content-addressing
 * stable, pas crypto. Le namespace upstream renomme en `ZSTD_XXH64`
 * — transparent grace aux macros du header. */
#include "xxhash.h"

/* zstd compress API : link via zstd_compress.c et co, amalgames dans
 * la cible CMake icons_catalog_gen (cf. CMakeLists.txt). */
#include "zstd.h"

/* ============================================================
 * SHA256 — mini impl public domain (suffisant pour cache build).
 * Source : adaptee de SHA-2 reference RFC 6234 (style B-Z), pas de
 * dependance externe. Pas crypto-sensible : on hash des sources
 * de build pour detection de changement, pas pour signature.
 * ============================================================ */

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} Sha256Ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x,n) (((x) >> (n)) | ((x) << (32-(n))))

static void sha256_transform(Sha256Ctx* ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i, j;
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (uint32_t)data[j] << 24 | (uint32_t)data[j+1] << 16
             | (uint32_t)data[j+2] << 8 | (uint32_t)data[j+3];
    for (; i < 64; ++i) {
        uint32_t s0 = SHA256_ROTR(m[i-15],7) ^ SHA256_ROTR(m[i-15],18) ^ (m[i-15] >> 3);
        uint32_t s1 = SHA256_ROTR(m[i-2],17) ^ SHA256_ROTR(m[i-2],19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        uint32_t S1 = SHA256_ROTR(e,6) ^ SHA256_ROTR(e,11) ^ SHA256_ROTR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + SHA256_K[i] + m[i];
        uint32_t S0 = SHA256_ROTR(a,2) ^ SHA256_ROTR(a,13) ^ SHA256_ROTR(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx* ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(Sha256Ctx* ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen      );
    ctx->data[62] = (uint8_t)(ctx->bitlen >>  8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)(ctx->state[0] >> (24 - i*8));
        hash[i + 4]  = (uint8_t)(ctx->state[1] >> (24 - i*8));
        hash[i + 8]  = (uint8_t)(ctx->state[2] >> (24 - i*8));
        hash[i + 12] = (uint8_t)(ctx->state[3] >> (24 - i*8));
        hash[i + 16] = (uint8_t)(ctx->state[4] >> (24 - i*8));
        hash[i + 20] = (uint8_t)(ctx->state[5] >> (24 - i*8));
        hash[i + 24] = (uint8_t)(ctx->state[6] >> (24 - i*8));
        hash[i + 28] = (uint8_t)(ctx->state[7] >> (24 - i*8));
    }
}

static void sha256_hex(const uint8_t hash[32], char out[65]) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i*2]   = H[(hash[i] >> 4) & 0xF];
        out[i*2+1] = H[hash[i] & 0xF];
    }
    out[64] = '\0';
}

/* ============================================================
 * Sets vendores : 12 prefixes, ordre = priorite ticket V8.
 * ============================================================ */

typedef struct {
    const char* prefix;      /* nom du JSON sans .json. */
    int         default_dim; /* taille viewBox par defaut (W=H). */
    /* Filtre name : si non-NULL, l'entree doit contenir cette
     * sous-chaine pour etre retenue (fluent -> "-24-"). */
    const char* require_substr;
    /* Filtre body : si non-NULL, l'entree est rejetee si le body
     * contient cette sous-chaine (line-md -> "<animate"). */
    const char* reject_body_substr;
} IconSetDef;

/* Ordre = priorite resolution Lot 2a-ii. Pas d'editorial : c'est
 * fige par le ticket DSL Icone V8 §"Resolution compile-time". */
/* b — Defaults width/height extraits des champs `width`/`height`
 * top-level de chaque vendor/iconify-sets/*.json. Verifier avec :
 *   tail -5 vendor/iconify-sets/<set>.json | grep -E width
 * Correction de 3 valeurs erronees du commit initial (fluent 20 != 24,
 * devicon 128 != 32, logos 256 != 32). */
static const IconSetDef SETS[] = {
    { "material-symbols",   24, NULL,    NULL       },
    { "mdi",                24, NULL,    NULL       },
    { "fluent",             20, "-24-",  NULL       },
    { "vscode-icons",       32, NULL,    NULL       },
    { "logos",              256, NULL,   NULL       },
    { "devicon",            128, NULL,   NULL       },
    { "fluent-emoji-flat",  32, NULL,    NULL       },
    { "carbon",             32, NULL,    NULL       },
    { "game-icons",         512, NULL,   NULL       },
    { "simple-icons",       24, NULL,    NULL       },
    { "line-md",            24, NULL,    "<animate" },
    { "flat-color-icons",   48, NULL,    NULL       }
};
#define SETS_COUNT ((int)(sizeof(SETS) / sizeof(SETS[0])))

/* ============================================================
 * Mini parser JSON ad-hoc Iconify.
 * Structure ciblee :
 *   {
 *     "prefix": "...",
 *     "info": {...},
 *     "icons": {
 *       "<name>": {"body": "<...escaped...>", "width": N?, "height": N?},
 *       ...
 *     },
 *     "aliases": {...},
 *     "width": N?, "height": N?
 *   }
 * On ne lit QUE "icons" : skip recursif des autres objets via balance
 * d'accolades. Les strings sont decodees (\" \\ \n \r \t \uXXXX).
 * ============================================================ */

typedef struct {
    const char* p;     /* pointeur courant. */
    const char* end;
} JsonReader;

static void jr_skip_ws(JsonReader* r) {
    while (r->p < r->end) {
        char c = *r->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r->p++;
        else break;
    }
}

/* Decode une string JSON, alloue le resultat (NUL-terminated).
 * Renvoie NULL si erreur. *out_len = taille raw (sans NUL). */
static char* jr_read_string(JsonReader* r, size_t* out_len) {
    if (r->p >= r->end || *r->p != '"') return NULL;
    r->p++;
    /* Premier pass : taille majoree par taille raw entre quotes. */
    const char* start = r->p;
    while (r->p < r->end && *r->p != '"') {
        if (*r->p == '\\' && r->p + 1 < r->end) r->p += 2;
        else r->p++;
    }
    if (r->p >= r->end) return NULL;
    size_t raw = (size_t)(r->p - start);
    char* buf = (char*)malloc(raw + 1);
    if (!buf) return NULL;
    size_t k = 0;
    const char* s = start;
    while (s < r->p) {
        if (*s == '\\' && s + 1 < r->p) {
            char esc = s[1];
            switch (esc) {
                case '"':  buf[k++] = '"';  s += 2; break;
                case '\\': buf[k++] = '\\'; s += 2; break;
                case '/':  buf[k++] = '/';  s += 2; break;
                case 'n':  buf[k++] = '\n'; s += 2; break;
                case 'r':  buf[k++] = '\r'; s += 2; break;
                case 't':  buf[k++] = '\t'; s += 2; break;
                case 'b':  buf[k++] = '\b'; s += 2; break;
                case 'f':  buf[k++] = '\f'; s += 2; break;
                case 'u': {
                    if (s + 6 > r->p) { free(buf); return NULL; }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char hc = s[2 + i];
                        unsigned v;
                        if (hc >= '0' && hc <= '9') v = (unsigned)(hc - '0');
                        else if (hc >= 'a' && hc <= 'f') v = (unsigned)(hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') v = (unsigned)(hc - 'A' + 10);
                        else { free(buf); return NULL; }
                        cp = (cp << 4) | v;
                    }
                    s += 6;
                    /* Encode UTF-8. Pas de surrogate pairs gerees : les
                     * SVG Iconify n'utilisent pas de BMP-out chars. */
                    if (cp < 0x80) {
                        buf[k++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[k++] = (char)(0xC0 | (cp >> 6));
                        buf[k++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[k++] = (char)(0xE0 | (cp >> 12));
                        buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[k++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: buf[k++] = esc; s += 2; break;
            }
        } else {
            buf[k++] = *s++;
        }
    }
    buf[k] = '\0';
    r->p++; /* consomme " fermant. */
    if (out_len) *out_len = k;
    return buf;
}

/* Skip une valeur JSON sans interpretation. Necessaire pour passer
 * les champs autres que "icons" sans bloquer. */
static bool jr_skip_value(JsonReader* r);

static bool jr_skip_object(JsonReader* r) {
    if (r->p >= r->end || *r->p != '{') return false;
    r->p++;
    int depth = 1;
    while (r->p < r->end && depth > 0) {
        char c = *r->p;
        if (c == '"') {
            /* skip string */
            r->p++;
            while (r->p < r->end && *r->p != '"') {
                if (*r->p == '\\' && r->p + 1 < r->end) r->p += 2;
                else r->p++;
            }
            if (r->p < r->end) r->p++;
        } else if (c == '{' || c == '[') { depth++; r->p++; }
        else if (c == '}' || c == ']') { depth--; r->p++; }
        else r->p++;
    }
    return depth == 0;
}

static bool jr_skip_value(JsonReader* r) {
    jr_skip_ws(r);
    if (r->p >= r->end) return false;
    char c = *r->p;
    if (c == '"') {
        char* s = jr_read_string(r, NULL);
        if (!s) return false;
        free(s);
        return true;
    }
    if (c == '{' || c == '[') return jr_skip_object(r);
    /* number / true / false / null */
    while (r->p < r->end) {
        c = *r->p;
        if (c == ',' || c == '}' || c == ']' || c == ' ' ||
            c == '\t' || c == '\n' || c == '\r') break;
        r->p++;
    }
    return true;
}

/* ============================================================
 * Buffers dynamiques.
 * ============================================================ */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf* b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void buf_reserve(Buf* b, size_t need) {
    if (b->cap >= need) return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    char* nd = (char*)realloc(b->data, ncap);
    if (!nd) { fprintf(stderr, "icons_catalog_gen: OOM (buf %zu)\n", ncap); exit(2); }
    b->data = nd;
    b->cap = ncap;
}

static void buf_append(Buf* b, const void* src, size_t n) {
    buf_reserve(b, b->len + n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

/* ============================================================
 * Catalogue accumule.
 * ============================================================ */

typedef struct {
    char*    name;
    uint8_t  set_id;
    uint16_t width;          /* b : 0 = utilise default per-set */
    uint16_t height;         /* idem */
    uint64_t hash;
    uint32_t offset;
    uint32_t length;
    bool     couleur_modif;
} Entry;

typedef struct {
    Entry*  data;
    size_t  len;
    size_t  cap;
} Entries;

static void entries_push(Entries* e, Entry v) {
    if (e->len == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 256;
        e->data = (Entry*)realloc(e->data, e->cap * sizeof(Entry));
        if (!e->data) { fprintf(stderr, "icons_catalog_gen: OOM entries\n"); exit(2); }
    }
    e->data[e->len++] = v;
}

/* ============================================================
 * I/O fichier.
 * ============================================================ */

static char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* ============================================================
 * Parse d'un set.
 * Retourne nombre d'entrees retenues. line_md_anim_patterns est
 * un compteur des patterns d'animation rejetes pour reporting.
 * ============================================================ */

typedef struct {
    int total_in_json;
    int retained;
    int rejected_filter_name;
    int rejected_filter_body;
} SetStats;

/* Compteurs globaux des patterns SMIL detectes dans line-md, pour
 * report final dans le commit body. */
static int g_line_md_animate     = 0;
static int g_line_md_animatetx   = 0;
static int g_line_md_animatemot  = 0;
static int g_line_md_set_tag     = 0;

static void detect_anim_patterns(const char* body, size_t len) {
    /* Detection naive substring : suffit pour le reporting. */
    if (!body) return;
    if (memmem(body, len, "<animateTransform", 17)) g_line_md_animatetx++;
    else if (memmem(body, len, "<animateMotion", 14)) g_line_md_animatemot++;
    else if (memmem(body, len, "<animate", 8)) g_line_md_animate++;
    if (memmem(body, len, "<set ", 5)) g_line_md_set_tag++;
}

/* Cherche `"<key>"` (cle quotee) suivie de `:` dans le scope objet
 * courant ; renvoie true et place r->p juste apres le ':'. */
static bool jr_find_key(JsonReader* r, const char* key) {
    size_t klen = strlen(key);
    int depth = 0;
    while (r->p < r->end) {
        char c = *r->p;
        if (c == '"') {
            const char* qstart = r->p + 1;
            const char* p = qstart;
            while (p < r->end && *p != '"') {
                if (*p == '\\' && p + 1 < r->end) p += 2;
                else p++;
            }
            if (p >= r->end) return false;
            if (depth == 0 && (size_t)(p - qstart) == klen &&
                memcmp(qstart, key, klen) == 0) {
                r->p = p + 1;
                jr_skip_ws(r);
                if (r->p < r->end && *r->p == ':') { r->p++; jr_skip_ws(r); return true; }
                return false;
            }
            r->p = p + 1;
        } else if (c == '{' || c == '[') { depth++; r->p++; }
        else if (c == '}' || c == ']') {
            if (depth == 0) return false;
            depth--; r->p++;
        } else r->p++;
    }
    return false;
}

static int parse_set(const IconSetDef* def, int set_id,
                     const char* json, size_t json_len,
                     Entries* out, Buf* blob, SetStats* stats) {
    JsonReader r = { json, json + json_len };
    jr_skip_ws(&r);
    if (r.p >= r.end || *r.p != '{') {
        fprintf(stderr, "icons_catalog_gen: %s: pas un objet JSON\n", def->prefix);
        return -1;
    }
    r.p++;
    if (!jr_find_key(&r, "icons")) {
        fprintf(stderr, "icons_catalog_gen: %s: cle \"icons\" introuvable\n", def->prefix);
        return -1;
    }
    if (r.p >= r.end || *r.p != '{') return -1;
    r.p++;
    /* On itere les entrees de l'objet icons. */
    while (r.p < r.end) {
        jr_skip_ws(&r);
        if (r.p < r.end && *r.p == '}') { r.p++; break; }
        if (r.p < r.end && *r.p == ',') { r.p++; continue; }
        if (r.p >= r.end || *r.p != '"') break;
        char* name = jr_read_string(&r, NULL);
        if (!name) break;
        jr_skip_ws(&r);
        if (r.p >= r.end || *r.p != ':') { free(name); break; }
        r.p++;
        jr_skip_ws(&r);
        if (r.p >= r.end || *r.p != '{') { free(name); break; }
        /* Lit le sous-objet : chercher "body", "width", "height". */
        const char* sub_start = r.p;
        r.p++;
        char*  body     = NULL;
        size_t body_len = 0;
        uint16_t w_override = 0, h_override = 0;
        {
            JsonReader rsub = { r.p, r.end };
            if (jr_find_key(&rsub, "body")) {
                body = jr_read_string(&rsub, &body_len);
            }
            r.p = rsub.p;
        }
        /* b — Parse width/height per-entry. Independants : peuvent
         * etre avant OU apres body dans le sous-objet, donc on rescan from
         * sub_start. Si absent, reste 0 (= fallback default per-set au
         * runtime via OWL_ICONS_SET_VIEWBOX). */
        {
            JsonReader rsub = { sub_start + 1, r.end };
            if (jr_find_key(&rsub, "width")) {
                long v = strtol(rsub.p, NULL, 10);
                if (v > 0 && v < 65536) w_override = (uint16_t)v;
            }
        }
        {
            JsonReader rsub = { sub_start + 1, r.end };
            if (jr_find_key(&rsub, "height")) {
                long v = strtol(rsub.p, NULL, 10);
                if (v > 0 && v < 65536) h_override = (uint16_t)v;
            }
        }
        /* Skip jusqu'a fin du sous-objet. */
        JsonReader rfull = { sub_start, r.end };
        if (!jr_skip_object(&rfull)) { free(name); free(body); break; }
        r.p = rfull.p;

        stats->total_in_json++;
        if (!body) { free(name); continue; }

        /* Filtre nom. */
        if (def->require_substr && !strstr(name, def->require_substr)) {
            stats->rejected_filter_name++;
            free(name); free(body); continue;
        }
        /* Filtre body (anim line-md) + comptage des patterns. */
        if (def->reject_body_substr) {
            detect_anim_patterns(body, body_len);
            if (memmem(body, body_len, def->reject_body_substr,
                       strlen(def->reject_body_substr))) {
                stats->rejected_filter_body++;
                free(name); free(body); continue;
            }
        }

        /* Calcule XXH64 du body wrappe `<svg viewBox='0 0 D D'>BODY</svg>`.
         * Cout : 1 malloc/free par icone (~80 + body_len). Seed 0. */
        char wrap_pre[80];
        int wpre = snprintf(wrap_pre, sizeof(wrap_pre),
                            "<svg viewBox='0 0 %d %d'>", def->default_dim, def->default_dim);
        const char wrap_post[] = "</svg>";
        size_t wpost = sizeof(wrap_post) - 1;
        size_t wrap_total = (size_t)wpre + body_len + wpost;
        char* wrapped = (char*)malloc(wrap_total);
        if (!wrapped) { fprintf(stderr, "icons_catalog_gen: OOM wrap\n"); exit(2); }
        memcpy(wrapped, wrap_pre, (size_t)wpre);
        memcpy(wrapped + wpre, body, body_len);
        memcpy(wrapped + wpre + body_len, wrap_post, wpost);
        uint64_t hash = XXH64(wrapped, wrap_total, 0);
        free(wrapped);

        /* Heuristique couleur_modifiable : presence de "currentColor". */
        bool modif = memmem(body, body_len, "currentColor", 12) != NULL;

        /*  — un nom d'icone qui commence par un CHIFFRE (ex. "4x24",
         * "2a", "10mp") n'est PAS un identifiant OWL valide : `icones.4x24`
         * est intokenisable (le scanner lit `icones.4` + `x24`), le formatter
         * insere un espace, la completion propose de l'inexprimable. Decision
         * dev (, option prefixe) : on prefixe d'une lettre 'i' pour le
         * rendre referencable -> `icones.i4x24`. Le nom canon stocke est le
         * nom prefixe (catalogue + completion + runtime utilisent la meme
         * source, donc coherent). */
        if (name[0] >= '0' && name[0] <= '9') {
            size_t nlen = strlen(name);
            char* pref = (char*)malloc(nlen + 2);
            if (!pref) { fprintf(stderr, "icons_catalog_gen: OOM prefix\n"); exit(2); }
            pref[0] = 'i';
            memcpy(pref + 1, name, nlen + 1);  /* + NUL */
            free(name);
            name = pref;
        }

        /* Append au blob. */
        uint32_t off = (uint32_t)blob->len;
        buf_append(blob, body, body_len);

        Entry e;
        e.name          = name;        /* transfere ownership a out. */
        e.set_id        = (uint8_t)set_id;
        e.width         = w_override;  /* b : 0 = default per-set */
        e.height        = h_override;
        e.hash          = hash;
        e.offset        = off;
        e.length        = (uint32_t)body_len;
        e.couleur_modif = modif;
        entries_push(out, e);
        stats->retained++;
        free(body);
    }
    return 0;
}

/* ============================================================
 * Tri par (set_id, name) pour permettre binsearch en Lot 2a-ii.
 * ============================================================ */

static int entry_cmp(const void* a, const void* b) {
    const Entry* ea = (const Entry*)a;
    const Entry* eb = (const Entry*)b;
    if (ea->set_id != eb->set_id) return (int)ea->set_id - (int)eb->set_id;
    return strcmp(ea->name, eb->name);
}

/* ============================================================
 *  — Uniquification globale des noms.
 *
 * Apres `qsort(set_id, name)`, plusieurs entries peuvent partager le
 * meme `name` (ex. `youtube` apparait dans 3 sets Iconify differents).
 * Ces doublons rendent ambigu `icones.X` cote DSL OWL : le runtime
 * resolve toujours la 1ere occurrence, les 2e+ sont inaccessibles.
 *
 * Strategie : 1ere occurrence garde le nom canon (backward compat
 * preservee), 2e+ recoivent un suffix `-2`, `-3`, ..., `-N`. L'ordre
 * de la 1ere occurrence est determine par `qsort(set_id, name)` :
 * c'est l'entry du set le plus prioritaire (set_id le plus bas).
 *
 * Pourquoi suffix `-N` et pas `_N` :
 *   Le runtime `icones_normalize_name` (stdlib_icones.c) translate
 *   `_` → `-` au lookup pour permettre a l'utilisateur OWL (dont les
 *   identifiants ne supportent pas `-`) d'ecrire `icones.red_heart`
 *   et resoudre la `red-heart` (forme canon Iconify). Si on suffixait
 *   en `_N`, le translate transformerait `_2` en `-2` au lookup, mais
 *   le catalog stockerait `_2` → miss. Le suffix `-N` est stocke
 *   directement et l'utilisateur tape `icones.youtube_2` qui resout
 *   via translate vers `youtube-2`. Cohabite parfaitement avec le
 *   pattern existant.
 *
 * Risque collision : un id Iconify natif `youtube-2` existerait deja
 * dans le catalog → mon `youtube-2` suffixage serait un doublon. Mitige
 * par boucle de check : si le suffix `-N` est deja pris, incrementer
 * jusqu'a libre. En pratique, aucun nom suffixe natif ne collisionne
 * avec mes doublons (audit catalog 2026-06-09).
 *
 * Implementation : hashmap simple FNV-1a, ~50k buckets, O(n) average.
 * ============================================================ */

#define UNIQ_HASH_SIZE 65536u
#define UNIQ_HASH_MASK 0xFFFFu

typedef struct UniqNode {
    const char* name;  /* pointer vers le `name` de la 1ere entry */
    int count;         /* nombre d'occurrences vues */
    struct UniqNode* next;
} UniqNode;

static uint32_t uniq_fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

/* Renomme les entries 2e+ avec un meme `name`. Retourne le nombre
 * de doublons renommes (= entries->len - unique_names). */
static int uniquify_entry_names(Entries* entries) {
    UniqNode** table = (UniqNode**)calloc(UNIQ_HASH_SIZE, sizeof(UniqNode*));
    if (!table) {
        fprintf(stderr, "icons_catalog_gen: OOM uniquify table\n");
        exit(2);
    }
    int renamed = 0;
    for (size_t i = 0; i < entries->len; ++i) {
        Entry* e = &entries->data[i];
        uint32_t h = uniq_fnv1a(e->name) & UNIQ_HASH_MASK;
        UniqNode* n = table[h];
        while (n && strcmp(n->name, e->name) != 0) n = n->next;
        if (n) {
            /* Doublon : renommer avec suffix `-<count+1>`. La 1ere
             * occurrence (count=1) a deja garde son nom canon.
             *
             * Si le suffix collisionne avec un id Iconify natif deja
             * present (ex. `youtube-2` natif), incrementer jusqu'a
             * libre. */
            size_t old_len = strlen(e->name);
            char* new_name = NULL;
            do {
                if (new_name) free(new_name);
                n->count++;
                new_name = (char*)malloc(old_len + 16);
                if (!new_name) {
                    fprintf(stderr, "icons_catalog_gen: OOM rename\n");
                    exit(2);
                }
                snprintf(new_name, old_len + 16, "%s-%d", e->name, n->count);
                /* Check collision : ce nouveau name est-il deja dans la
                 * table (= existait deja comme nom Iconify natif ou
                 * comme suffix precedent) ? */
                uint32_t h2 = uniq_fnv1a(new_name) & UNIQ_HASH_MASK;
                UniqNode* coll = table[h2];
                while (coll && strcmp(coll->name, new_name) != 0) {
                    coll = coll->next;
                }
                if (coll == NULL) break; /* nom libre, on garde */
            } while (1);
            free(e->name);
            e->name = new_name;
            /* Inserer le nouveau nom dans la table pour eviter futurs
             * doublons. */
            UniqNode* node2 = (UniqNode*)malloc(sizeof(UniqNode));
            if (!node2) {
                fprintf(stderr, "icons_catalog_gen: OOM uniquify node2\n");
                exit(2);
            }
            node2->name = e->name;
            node2->count = 1;
            node2->next = table[uniq_fnv1a(e->name) & UNIQ_HASH_MASK];
            table[uniq_fnv1a(e->name) & UNIQ_HASH_MASK] = node2;
            renamed++;
        } else {
            UniqNode* node = (UniqNode*)malloc(sizeof(UniqNode));
            if (!node) {
                fprintf(stderr, "icons_catalog_gen: OOM uniquify node\n");
                exit(2);
            }
            node->name = e->name;
            node->count = 1;
            node->next = table[h];
            table[h] = node;
        }
    }
    /* Cleanup */
    for (uint32_t i = 0; i < UNIQ_HASH_SIZE; ++i) {
        UniqNode* n = table[i];
        while (n) {
            UniqNode* next = n->next;
            free(n);
            n = next;
        }
    }
    free(table);
    return renamed;
}

/* ============================================================
 * Emission C.
 * ============================================================ */

static void emit_byte_array(FILE* f, const char* name, const uint8_t* data, size_t n) {
    fprintf(f, "const unsigned char %s[%zu] = {\n", name, n);
    for (size_t i = 0; i < n; ++i) {
        if ((i & 15) == 0) fputs("    ", f);
        fprintf(f, "0x%02x,", data[i]);
        if ((i & 15) == 15 || i + 1 == n) fputc('\n', f);
        else fputc(' ', f);
    }
    fputs("};\n", f);
}

static void emit_c_string(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c >= 0x20 && c < 0x7F) fputc(c, f);
        else fprintf(f, "\\x%02x", c);
    }
    fputc('"', f);
}

static int emit_output(const char* out_path,
                       const Entries* entries,
                       const uint8_t* zstd_blob, size_t zstd_size,
                       size_t raw_size) {
    FILE* f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "icons_catalog_gen: cannot open %s for write\n", out_path);
        return -1;
    }
    fputs("/* AUTO-GENERATED by tools/icons_catalog_gen — DO NOT EDIT.\n", f);
    fputs(" * feat/0337 Lot 2a-i : catalogue d'icones Iconify embarque.\n", f);
    fputs(" * Regenerate via mingw32-make all (cache SHA256 sur sources JSON).\n", f);
    fputs(" */\n", f);
    fputs("#include \"icons_catalog.h\"\n\n", f);

    /* Table set names. */
    fputs("const char* const OWL_ICONS_SET_NAMES[] = {\n", f);
    for (int i = 0; i < SETS_COUNT; ++i) {
        fputs("    ", f);
        emit_c_string(f, SETS[i].prefix);
        fputs(",\n", f);
    }
    fputs("    (const char*)0\n};\n", f);
    fprintf(f, "const size_t OWL_ICONS_SET_COUNT = %d;\n\n", SETS_COUNT);

    /* b — Table viewbox per set (taille native du SVG).
     * Le resolver utilise cette valeur pour wrapper le body avec le
     * bon viewBox au lieu d'un hardcode 24x24 (qui faisait tronquer
     * les icones 32x32 / 48x48 / 512x512). */
    fputs("const int OWL_ICONS_SET_VIEWBOX[] = {\n", f);
    for (int i = 0; i < SETS_COUNT; ++i) {
        fprintf(f, "    %d,  /* %s */\n",
                SETS[i].default_dim, SETS[i].prefix);
    }
    fputs("    0\n};\n\n", f);

    /* Table d'entrees. b : width/height per-entry inseres apres
     * set_id (= 0 si l'icone heriterait du default per-set). */
    fprintf(f, "const OwlIconCatalogEntry OWL_ICONS_CATALOG[] = {\n");
    for (size_t i = 0; i < entries->len; ++i) {
        const Entry* e = &entries->data[i];
        fputs("    { ", f);
        emit_c_string(f, e->name);
        fprintf(f, ", %u, %u, %u, 0x%016llxULL, %uu, %uu, %s },\n",
                (unsigned)e->set_id,
                (unsigned)e->width,
                (unsigned)e->height,
                (unsigned long long)e->hash,
                (unsigned)e->offset,
                (unsigned)e->length,
                e->couleur_modif ? "true" : "false");
    }
    fputs("    { (const char*)0, 0, 0, 0, 0, 0, 0, false }\n};\n", f);
    fprintf(f, "const size_t OWL_ICONS_CATALOG_COUNT = %zu;\n\n", entries->len);

    /* Blob compresse + tailles.  — gardé sous OWL_ICONS_NO_BLOB :
     * owl.exe (resolver stdlib_icones) le compile ; owl-lsp passe
     * -DOWL_ICONS_NO_BLOB et ne référence que les noms (OWL_ICONS_CATALOG),
     * jamais le blob (~11,5 MB) → owl-lsp ~15 MB -> ~3 MB. Le linker ne peut
     * pas l'éliminer seul (donnée à linkage externe, --gc-sections PE ne mord
     * pas), d'où l'exclusion à la compilation. */
    fputs("#ifndef OWL_ICONS_NO_BLOB\n", f);
    emit_byte_array(f, "OWL_ICONS_BLOB_ZSTD", zstd_blob, zstd_size);
    fprintf(f, "const size_t OWL_ICONS_BLOB_ZSTD_SIZE = %zuu;\n", zstd_size);
    fprintf(f, "const size_t OWL_ICONS_BLOB_RAW_SIZE  = %zuu;\n", raw_size);
    fputs("#endif /* OWL_ICONS_NO_BLOB */\n", f);

    fclose(f);
    return 0;
}

/* ============================================================
 * Main.
 * ============================================================ */

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: icons_catalog_gen <vendor_iconify_dir> <output_c_file>\n");
        return 1;
    }
    const char* iconify_dir = argv[1];
    const char* out_path    = argv[2];

    /* Stamp path = <out_path>.stamp. */
    char stamp_path[1024];
    snprintf(stamp_path, sizeof(stamp_path), "%s.stamp", out_path);

    /*  : version de la LOGIQUE de génération, salée dans le hash du
     * cache. Le cache était keyé sur l'INPUT (JSON) seulement : un changement
     * du générateur (ex. préfixe des noms à chiffre initial) ne l'invalidait
     * PAS -> build incrémental reprenait l'ancien catalogue. Bumper cette
     * constante à chaque changement de logique de sortie. */
    static const char ICONS_GEN_VERSION[] = "v3-blob-ifndef-fix0592";

    /* Phase 1 : hash SHA256 du concat des 12 JSON sources + version logique. */
    Sha256Ctx hctx;
    sha256_init(&hctx);
    sha256_update(&hctx, (const uint8_t*)ICONS_GEN_VERSION,
                  sizeof(ICONS_GEN_VERSION));
    char* jsons[SETS_COUNT];
    size_t jsons_len[SETS_COUNT];
    for (int i = 0; i < SETS_COUNT; ++i) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.json", iconify_dir, SETS[i].prefix);
        size_t n = 0;
        char* data = read_file(path, &n);
        if (!data) {
            fprintf(stderr, "icons_catalog_gen: cannot read %s\n", path);
            return 1;
        }
        jsons[i] = data;
        jsons_len[i] = n;
        sha256_update(&hctx, (const uint8_t*)data, n);
        /* Inject prefix dans le hash pour invalider si le set list change. */
        sha256_update(&hctx, (const uint8_t*)SETS[i].prefix, strlen(SETS[i].prefix));
    }
    uint8_t hash[32];
    sha256_final(&hctx, hash);
    char hash_hex[65];
    sha256_hex(hash, hash_hex);

    /* Phase 2 : cache hit ? */
    if (file_exists(out_path) && file_exists(stamp_path)) {
        size_t sn = 0;
        char* sdata = read_file(stamp_path, &sn);
        if (sdata && sn >= 64 && memcmp(sdata, hash_hex, 64) == 0) {
            free(sdata);
            for (int i = 0; i < SETS_COUNT; ++i) free(jsons[i]);
            fprintf(stderr, "icons_catalog_gen: cache hit (%.16s...)\n", hash_hex);
            return 0;
        }
        free(sdata);
    }

    /* Phase 3 : parse + collecte. */
    Entries entries; entries.data = NULL; entries.len = 0; entries.cap = 0;
    Buf blob; buf_init(&blob);
    SetStats stats[SETS_COUNT];
    memset(stats, 0, sizeof(stats));
    for (int i = 0; i < SETS_COUNT; ++i) {
        if (parse_set(&SETS[i], i, jsons[i], jsons_len[i],
                      &entries, &blob, &stats[i]) != 0) {
            fprintf(stderr, "icons_catalog_gen: parse failed on %s\n",
                    SETS[i].prefix);
            return 1;
        }
        free(jsons[i]); jsons[i] = NULL;
        fprintf(stderr,
                "icons_catalog_gen: %-20s in=%6d retained=%6d "
                "filter_name=%5d filter_body=%5d\n",
                SETS[i].prefix, stats[i].total_in_json, stats[i].retained,
                stats[i].rejected_filter_name, stats[i].rejected_filter_body);
    }

    /* Phase 4 : tri par (set_id, name). Les offsets restent valides
     * (ils pointent dans le blob raw qui ne bouge pas). */
    qsort(entries.data, entries.len, sizeof(Entry), entry_cmp);

    /* Phase 4.5 : uniquification des noms. Les doublons
     * cross-sets (ex. `youtube` dans 3 sets) recoivent un suffix
     * `_2`, `_3`, ... La 1ere occurrence (= set_id le plus bas)
     * garde le nom canon. */
    {
        int renamed = uniquify_entry_names(&entries);
        fprintf(stderr,
                "icons_catalog_gen: uniquified %d duplicate names "
                "(suffix _N)\n", renamed);
    }

    /* Phase 5 : compression zstd niveau 19. */
    size_t bound = ZSTD_compressBound(blob.len);
    uint8_t* compressed = (uint8_t*)malloc(bound);
    if (!compressed) {
        fprintf(stderr, "icons_catalog_gen: OOM compressed (%zu)\n", bound);
        return 2;
    }
    size_t zsize = ZSTD_compress(compressed, bound, blob.data, blob.len, 19);
    if (ZSTD_isError(zsize)) {
        fprintf(stderr, "icons_catalog_gen: zstd error: %s\n",
                ZSTD_getErrorName(zsize));
        return 2;
    }

    /* Phase 6 : emit. */
    if (emit_output(out_path, &entries, compressed, zsize, blob.len) != 0) {
        return 1;
    }

    /* Phase 7 : ecriture stamp. */
    FILE* sf = fopen(stamp_path, "wb");
    if (sf) { fwrite(hash_hex, 1, 64, sf); fputc('\n', sf); fclose(sf); }

    /* Reporting final. */
    size_t total_retained = entries.len;
    fprintf(stderr,
            "icons_catalog_gen: TOTAL entries=%zu blob_raw=%zu blob_zstd=%zu "
            "ratio=%.1f%%\n",
            total_retained, blob.len, zsize,
            blob.len ? (100.0 * (double)zsize / (double)blob.len) : 0.0);
    fprintf(stderr,
            "icons_catalog_gen: line-md anim patterns: animate=%d "
            "animateTransform=%d animateMotion=%d set_tag=%d\n",
            g_line_md_animate, g_line_md_animatetx,
            g_line_md_animatemot, g_line_md_set_tag);

    /* Cleanup */
    for (size_t i = 0; i < entries.len; ++i) free(entries.data[i].name);
    free(entries.data);
    buf_free(&blob);
    free(compressed);
    return 0;
}
