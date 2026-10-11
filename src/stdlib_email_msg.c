// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Import de fichiers .msg Outlook (, Lot 5 chantier email) — lecture
 * SEULE. Native EmailImporteMSG(email, chemin) -> booleen : ouvre un conteneur
 * OLE2/CFBF, extrait les proprietes MAPI ([MS-OXMSG]/[MS-OXPROPS]) et remplit
 * une variable Email (expediteur, destinataires/Cc/Cci, sujet, corps texte,
 * corps HTML, pieces jointes).
 *
 * Le conteneur CFBF est lu par la machinerie durcie/fuzzee deja vendoree dans
 * libxls (vendor/libxls/include/libxls/ole.h : ole2_open_file / ole2_sopen /
 * ole2_read + FAT/mini-FAT). ole2_open_file APLATIT le repertoire dans
 * ole->files et perd la hierarchie (noms de sous-flux qui collisionnent entre
 * storages) ; on RELIT donc le flux de repertoire nous-memes (entrees PSS de
 * 128 o via ole2_sopen(ole, ole->dirstart, -1)), on reconstruit l'arbre des
 * storages, et on ouvre chaque flux cible par secteur de depart + taille. La
 * mini-FAT (SSAT/SSecID) necessaire aux petits flux est deja montee par
 * ole2_open_file (traitement de l'entree racine) : ole2_sopen la reutilise.
 *
 * Le corps HTML d'un vrai .msg Outlook n'est pas dans PidTagHtml (0x1013, ici
 * 8 octets junk) mais encapsule dans PidTagRtfCompressed (0x1009) : RTF
 * compresse LZFu ([MS-OXRTFCP]) puis HTML de-encapsule ([MS-OXRTFEX],
 * \fromhtml1). Les deux passes sont implementees ici. Fallback : PidTagHtml si
 * present et non-junk.
 *
 * Parsing de binaire NON fiable : toutes les allocations sont bornees, aucun
 * deref hors bornes, LZFu resistant a un en-tete corrompu. */

#include "stdlib_email.h"

#include "owl_registry.h"
#include "owl_stdlib.h"
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "vm.h"
#include "object.h"
#include "value.h"

#include "libxls/ole.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* Bornes de securite ------------------------------------------------------- */
#define MSG_MAX_FILE_SIZE   (256u * 1024u * 1024u)   /* 256 Mo */
#define MSG_MAX_STREAM      ( 64u * 1024u * 1024u)   /* 64 Mo par flux */
#define MSG_MAX_DIR_ENTRIES 1000000u
#define MSG_MAX_WALK_DEPTH  128
#define MSG_DIR_ENTRY_SIZE  128
#define MSG_NOSTREAM        0xFFFFFFFFu

/* Types MAPI de proprietes variables (stockees en sous-flux __substg1.0_). */
#define MAPI_T_UNICODE 0x001F   /* UTF-16LE */
#define MAPI_T_ANSI    0x001E   /* octets ANSI/ASCII */
#define MAPI_T_BINARY  0x0102
#define MAPI_T_LONG    0x0003   /* PT_LONG : stocke inline dans __properties */

/* Identifiants de proprietes (partie haute du tag). */
#define PID_SUBJECT         0x0037
#define PID_BODY            0x1000
#define PID_RTF_COMPRESSED  0x1009
#define PID_HTML            0x1013
#define PID_MESSAGE_ID      0x1035
#define PID_SENTREPR_EMAIL  0x0065
#define PID_EMAIL_ADDRESS   0x3003
#define PID_SMTP_ADDRESS    0x39FE
#define PID_RECIPIENT_TYPE  0x0C15
#define PID_ATTACH_LONGNAME 0x3707
#define PID_ATTACH_FILENAME 0x3704
#define PID_ATTACH_DATA     0x3701
#define PID_ATTACH_MIMETAG  0x370E

/* ============================================================
 * Lecteurs petit-boutistes (independants de l'alignement / endianness hote).
 * ============================================================ */

static uint16_t rd_le16(const unsigned char* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_le32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Encode un point de code Unicode en UTF-8. Renvoie le nombre d'octets ecrits
 * (1..4). dst doit avoir au moins 4 octets libres. */
static int utf8_encode(char* dst, uint32_t cp) {
    if (cp <= 0x7F) {
        dst[0] = (char)cp; return 1;
    } else if (cp <= 0x7FF) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        if (cp > 0x10FFFF) cp = 0xFFFD;
        dst[0] = (char)(0xF0 | (cp >> 18));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* UTF-16LE -> UTF-8 (chaine heap NUL-terminee). Gere les paires de substitution.
 * bytelen = nombre d'octets source. Renvoie NULL si allocation impossible. */
static char* utf16le_to_utf8(const unsigned char* p, size_t bytelen) {
    size_t nunits = bytelen / 2;
    /* Pire cas 4 octets UTF-8 par unite. */
    char* out = malloc(nunits * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < nunits; i++) {
        uint32_t u = rd_le16(p + i * 2);
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < nunits) {
            uint32_t lo = rd_le16(p + (i + 1) * 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                cp = u;   /* substitution isolee : recopie brute */
            }
        } else {
            cp = u;
        }
        o += (size_t)utf8_encode(out + o, cp);
    }
    out[o] = '\0';
    return out;
}

/* cp1252 -> point de code Unicode (plage 0x80..0x9F specifique ; 0xA0..0xFF =
 * Latin-1 identite). Utilise pour decoder les octets \'XX du RTF. */
static uint32_t cp1252_to_cp(unsigned char b) {
    static const uint16_t hi[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (b < 0x80) return b;
    if (b < 0xA0) return hi[b - 0x80];
    return b;
}

/* ============================================================
 * Repertoire CFBF : relecture + reconstruction de l'arbre.
 * ============================================================ */

typedef struct {
    char     name[64];   /* nom ASCII-decode, NUL-termine */
    uint8_t  type;       /* 0=vide 1=storage 2=stream 5=root */
    uint32_t left, right, child;
    uint32_t sstart;
    uint32_t size;
} MsgDirEntry;

/* Decode le nom d'une entree (UTF-16LE, plage ASCII) dans dst borne. bsize =
 * taille du nom en octets (terminateur inclus). */
static void msg_decode_name(const unsigned char* raw, uint16_t bsize,
                            char* dst, size_t dstcap) {
    size_t nchars = 0;
    if (bsize >= 2) nchars = (size_t)(bsize / 2) - 1;   /* retire le NUL */
    if (nchars > dstcap - 1) nchars = dstcap - 1;
    size_t o = 0;
    for (size_t i = 0; i < nchars; i++) {
        uint16_t u = rd_le16(raw + i * 2);
        dst[o++] = (u < 0x80) ? (char)u : '?';
    }
    dst[o] = '\0';
}

/* Relit le flux de repertoire et renvoie le tableau d'entrees (indexe par
 * position = id d'entree, pour resoudre left/right/child). */
static MsgDirEntry* msg_read_directory(OLE2* ole, size_t* out_count) {
    *out_count = 0;
    OLE2Stream* s = ole2_sopen(ole, ole->dirstart, (size_t)-1);
    if (!s) return NULL;

    size_t cap = 64, count = 0;
    MsgDirEntry* entries = malloc(cap * sizeof(MsgDirEntry));
    if (!entries) { ole2_fclose(s); return NULL; }

    unsigned char raw[MSG_DIR_ENTRY_SIZE];
    for (;;) {
        ssize_t n = ole2_read(raw, 1, MSG_DIR_ENTRY_SIZE, s);
        if (n != MSG_DIR_ENTRY_SIZE) break;
        if (count >= MSG_MAX_DIR_ENTRIES) break;
        if (count == cap) {
            size_t ncap = cap * 2;
            MsgDirEntry* ne = realloc(entries, ncap * sizeof(MsgDirEntry));
            if (!ne) { free(entries); ole2_fclose(s); return NULL; }
            entries = ne; cap = ncap;
        }
        MsgDirEntry* e = &entries[count];
        uint16_t bsize = rd_le16(raw + 64);
        msg_decode_name(raw, bsize, e->name, sizeof(e->name));
        e->type   = raw[66];
        e->left   = rd_le32(raw + 68);
        e->right  = rd_le32(raw + 72);
        e->child  = rd_le32(raw + 76);
        e->sstart = rd_le32(raw + 116);
        e->size   = rd_le32(raw + 120);
        count++;
        if (s->eof) break;
    }
    ole2_fclose(s);
    *out_count = count;
    return entries;
}

/* Liste d'index (enfants directs d'un storage). */
typedef struct { uint32_t* idx; size_t count, cap; } MsgIdxList;

static void idxlist_add(MsgIdxList* l, uint32_t v) {
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        uint32_t* ni = realloc(l->idx, ncap * sizeof(uint32_t));
        if (!ni) return;   /* on tronque silencieusement en cas d'OOM */
        l->idx = ni; l->cap = ncap;
    }
    l->idx[l->count++] = v;
}
static void idxlist_free(MsgIdxList* l) { free(l->idx); l->idx = NULL; l->count = l->cap = 0; }

/* Parcours en ordre de l'arbre rouge-noir des freres (left, moi, right), avec
 * garde de profondeur et bitmap anti-cycle sur un repertoire corrompu. */
static void msg_walk(const MsgDirEntry* entries, size_t count, uint32_t idx,
                     int depth, unsigned char* visited, MsgIdxList* out) {
    if (idx == MSG_NOSTREAM || idx >= count) return;
    if (depth > MSG_MAX_WALK_DEPTH) return;
    if (visited[idx]) return;
    visited[idx] = 1;
    msg_walk(entries, count, entries[idx].left, depth + 1, visited, out);
    idxlist_add(out, idx);
    msg_walk(entries, count, entries[idx].right, depth + 1, visited, out);
}

/* Enfants directs du storage d'index storageIdx. */
static MsgIdxList msg_children(const MsgDirEntry* entries, size_t count,
                               uint32_t storageIdx) {
    MsgIdxList l = {0};
    if (storageIdx >= count) return l;
    unsigned char* visited = calloc(count ? count : 1, 1);
    if (!visited) return l;
    msg_walk(entries, count, entries[storageIdx].child, 0, visited, &l);
    free(visited);
    return l;
}

/* Comparaison de nom insensible a la casse (noms de flux MAPI en hexa). */
static int msg_name_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}
static int msg_name_prefix(const char* s, const char* pfx) {
    while (*pfx) { if (*s != *pfx) return 0; s++; pfx++; }
    return 1;
}

/* Cherche un sous-flux __substg1.0_<TAG> parmi une liste d'enfants. */
static int msg_find_substg(const MsgDirEntry* entries, const MsgIdxList* kids,
                           uint32_t fullTag, uint32_t* out_idx) {
    char want[24];
    snprintf(want, sizeof(want), "__substg1.0_%08X", fullTag);
    for (size_t i = 0; i < kids->count; i++) {
        uint32_t ci = kids->idx[i];
        if (msg_name_ieq(entries[ci].name, want)) { *out_idx = ci; return 1; }
    }
    return 0;
}
static int msg_find_named(const MsgDirEntry* entries, const MsgIdxList* kids,
                          const char* name, uint32_t* out_idx) {
    for (size_t i = 0; i < kids->count; i++) {
        uint32_t ci = kids->idx[i];
        if (msg_name_ieq(entries[ci].name, name)) { *out_idx = ci; return 1; }
    }
    return 0;
}

/* Lit le contenu brut d'un flux (secteur de depart + taille). Renvoie un buffer
 * heap + longueur ; NULL si vide / trop gros / echec. */
static unsigned char* msg_read_stream(OLE2* ole, const MsgDirEntry* e,
                                      size_t* out_len) {
    *out_len = 0;
    if (e->size == 0 || e->size > MSG_MAX_STREAM) return NULL;
    OLE2Stream* s = ole2_sopen(ole, e->sstart, (size_t)e->size);
    if (!s) return NULL;
    unsigned char* buf = malloc(e->size);
    if (!buf) { ole2_fclose(s); return NULL; }
    ssize_t n = ole2_read(buf, 1, e->size, s);
    ole2_fclose(s);
    if (n <= 0) { free(buf); return NULL; }
    *out_len = (size_t)n;
    return buf;
}

/* Lit une propriete chaine (essaie Unicode 001F puis ANSI 001E). Renvoie une
 * chaine heap NUL-terminee (UTF-8) ou NULL si absente. */
static char* msg_read_string_prop(OLE2* ole, const MsgDirEntry* entries,
                                  const MsgIdxList* kids, uint32_t propId) {
    uint32_t idx;
    if (msg_find_substg(entries, kids, (propId << 16) | MAPI_T_UNICODE, &idx)) {
        size_t len = 0;
        unsigned char* raw = msg_read_stream(ole, &entries[idx], &len);
        if (raw) { char* s = utf16le_to_utf8(raw, len); free(raw); return s; }
        char* empty = malloc(1); if (empty) empty[0] = '\0'; return empty;
    }
    if (msg_find_substg(entries, kids, (propId << 16) | MAPI_T_ANSI, &idx)) {
        size_t len = 0;
        unsigned char* raw = msg_read_stream(ole, &entries[idx], &len);
        char* s = malloc(len + 1);
        if (s) { if (raw) memcpy(s, raw, len); s[len] = '\0'; }
        free(raw);
        return s;
    }
    return NULL;
}

/* Lit une propriete PT_LONG dans le flux __properties_version1.0 des enfants.
 * headerSize = taille de l'en-tete (8 pour recipient/attachment). */
static int msg_read_long_prop(OLE2* ole, const MsgDirEntry* entries,
                              const MsgIdxList* kids, uint32_t fullTag,
                              uint32_t headerSize, uint32_t* out_val) {
    uint32_t idx;
    if (!msg_find_named(entries, kids, "__properties_version1.0", &idx)) return 0;
    size_t len = 0;
    unsigned char* raw = msg_read_stream(ole, &entries[idx], &len);
    if (!raw) return 0;
    int found = 0;
    for (size_t off = headerSize; off + 16 <= len; off += 16) {
        uint32_t tag = rd_le32(raw + off);
        if (tag == fullTag) { *out_val = rd_le32(raw + off + 8); found = 1; break; }
    }
    free(raw);
    return found;
}

/* ============================================================
 * LZFu : decompression RTF ([MS-OXRTFCP]).
 * ============================================================ */

/* Dictionnaire initial standard (prebuf RTF), pre-charge dans l'anneau de 4096
 * octets ; la position d'ecriture demarre a sa longueur (207). */
static const char LZFU_INITDICT[] =
    "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}"
    "{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans SerifSymbolArial"
    "Times New RomanCourier{\\colortbl\\red0\\green0\\blue0\r\n"
    "\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";

#define LZFU_MAGIC_COMPRESSED 0x75465A4Cu   /* 'LZFu' */
#define LZFU_MAGIC_RAW        0x414C454Du   /* 'MELA' (non compresse) */

/* Decompresse un flux PidTagRtfCompressed. Renvoie le RTF brut (heap) +
 * longueur dans *out_len, ou NULL en cas d'en-tete corrompu / OOM. */
static char* lzfu_decompress(const unsigned char* in, size_t inlen, size_t* out_len) {
    *out_len = 0;
    if (inlen < 16) return NULL;
    uint32_t compSize = rd_le32(in + 0);
    uint32_t rawSize  = rd_le32(in + 4);
    uint32_t magic    = rd_le32(in + 8);
    /* compSize couvre tout ce qui suit son propre champ (rawSize+magic+crc+data). */
    size_t dataAvail = inlen - 16;
    size_t dataDecl  = (compSize >= 12) ? (size_t)(compSize - 12) : 0;
    size_t dataLen   = (dataDecl && dataDecl <= dataAvail) ? dataDecl : dataAvail;

    if (rawSize == 0 || rawSize > MSG_MAX_STREAM) return NULL;

    if (magic == LZFU_MAGIC_RAW) {
        /* Flux non compresse : les octets bruts suivent l'en-tete. */
        size_t n = dataLen < rawSize ? dataLen : rawSize;
        char* out = malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, in + 16, n);
        out[n] = '\0';
        *out_len = n;
        return out;
    }
    if (magic != LZFU_MAGIC_COMPRESSED) return NULL;

    const unsigned char* data = in + 16;
    unsigned char dict[4096];
    size_t initLen = sizeof(LZFU_INITDICT) - 1;   /* 207 */
    if (initLen > sizeof(dict)) initLen = sizeof(dict);
    memcpy(dict, LZFU_INITDICT, initLen);
    uint32_t wp = (uint32_t)initLen;

    char* out = malloc((size_t)rawSize + 1);
    if (!out) return NULL;
    size_t outpos = 0;
    size_t inpos = 0;
    int done = 0;

    while (!done && inpos < dataLen && outpos < rawSize) {
        unsigned char control = data[inpos++];
        for (int bit = 0; bit < 8 && !done; bit++) {
            if ((control & (1u << bit)) == 0) {
                /* Litteral. */
                if (inpos >= dataLen) { done = 1; break; }
                unsigned char c = data[inpos++];
                if (outpos < rawSize) out[outpos++] = (char)c;
                dict[wp & 0xFFF] = c; wp++;
            } else {
                /* Reference dictionnaire (2 octets, gros-boutiste). */
                if (inpos + 1 >= dataLen) { done = 1; break; }
                unsigned char b0 = data[inpos++];
                unsigned char b1 = data[inpos++];
                uint32_t offset = ((uint32_t)b0 << 4) | ((uint32_t)b1 >> 4);
                uint32_t length = (uint32_t)(b1 & 0x0F) + 2;
                if (offset == (wp & 0xFFF)) { done = 1; break; }  /* fin de flux */
                for (uint32_t k = 0; k < length && outpos < rawSize; k++) {
                    unsigned char c = dict[(offset + k) & 0xFFF];
                    out[outpos++] = (char)c;
                    dict[wp & 0xFFF] = c; wp++;
                }
            }
        }
    }
    out[outpos] = '\0';
    *out_len = outpos;
    return out;
}

/* ============================================================
 * De-encapsulation RTF -> HTML ([MS-OXRTFEX]).
 * ============================================================ */

typedef struct { char* buf; size_t len, cap; } HtmlBuf;
static void hb_reserve(HtmlBuf* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char* nb = realloc(b->buf, ncap);
    if (!nb) return;
    b->buf = nb; b->cap = ncap;
}
static void hb_putc(HtmlBuf* b, char c) { hb_reserve(b, 1); if (b->buf) b->buf[b->len++] = c; }
static void hb_put_cp(HtmlBuf* b, uint32_t cp) {
    char tmp[4]; int n = utf8_encode(tmp, cp);
    hb_reserve(b, (size_t)n);
    if (b->buf) { for (int i = 0; i < n; i++) b->buf[b->len++] = tmp[i]; }
}

/* Etat d'un groupe RTF (pile). */
typedef struct { int htmlrtf; int ignore; int uc; } RtfGroup;

/* Destinations RTF a ignorer integralement (contenu non-HTML). */
static int rtf_is_ignored_dest(const char* w) {
    static const char* const kill[] = {
        "fonttbl", "colortbl", "stylesheet", "generator", "mhtmltag",
        "pntext", "pntxta", "pntxtb", "listtable", "listoverridetable",
        "revtbl", "rsidtbl", "info", "xmlnstbl", "themedata", "colorschememapping",
        "datastore", "latentstyles", "author", "operator", "creatim", "revtim",
        "printim", "buptim", "template", "keywords", "comment", "subject",
        "title", "company", "manager", "category", "doccomm", "hlinkbase",
        "userprops", "fldrslt", "bkmkstart", "bkmkend", "pict", "object",
        "wgrffmtfilter", NULL
    };
    for (int i = 0; kill[i]; i++) if (strcmp(w, kill[i]) == 0) return 1;
    return 0;
}

/* De-encapsule un RTF \fromhtml en HTML (UTF-8). Renvoie une chaine heap ou
 * NULL. rtflen = longueur du RTF brut. */
static char* rtf_to_html(const char* rtf, size_t rtflen) {
    HtmlBuf out = {0};

    enum { MAXG = 256 };
    RtfGroup stack[MAXG];
    int sp = 0;
    stack[0].htmlrtf = 0; stack[0].ignore = 0; stack[0].uc = 1;

    int pendingStar = 0;   /* on vient de lire "\*" : la prochaine destination
                              inconnue devient un groupe ignore */
    size_t i = 0;
    while (i < rtflen) {
        char c = rtf[i];
        RtfGroup* g = &stack[sp];

        if (c == '{') {
            if (sp + 1 < MAXG) { sp++; stack[sp] = stack[sp - 1]; }
            i++;
            continue;
        }
        if (c == '}') {
            if (sp > 0) sp--;
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }  /* RTF ignore les fins de ligne brutes */

        if (c == '\\') {
            /* Symbole de controle ou mot de controle. */
            if (i + 1 >= rtflen) { i++; continue; }
            char n = rtf[i + 1];
            if (n == '\'') {
                /* \'XX : octet hexa. */
                if (i + 3 < rtflen) {
                    char h[3] = { rtf[i + 2], rtf[i + 3], '\0' };
                    unsigned v = (unsigned)strtoul(h, NULL, 16);
                    if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, cp1252_to_cp((unsigned char)v));
                    i += 4;
                } else { i += 2; }
                continue;
            }
            if (!( (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') )) {
                /* Symbole de controle : \\ \{ \} \~ \- \_ \* etc. */
                if (n == '\\' || n == '{' || n == '}') {
                    if (!g->htmlrtf && !g->ignore) hb_putc(&out, n);
                } else if (n == '~') {
                    if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x00A0);
                } else if (n == '*') {
                    pendingStar = 1;
                }
                i += 2;
                continue;
            }
            /* Mot de controle : lettres, parametre numerique optionnel, espace
             * delimiteur optionnel. */
            size_t j = i + 1;
            while (j < rtflen && ((rtf[j] >= 'a' && rtf[j] <= 'z') ||
                                  (rtf[j] >= 'A' && rtf[j] <= 'Z'))) j++;
            char word[32];
            size_t wl = j - (i + 1);
            if (wl >= sizeof(word)) wl = sizeof(word) - 1;
            memcpy(word, rtf + i + 1, wl);
            word[wl] = '\0';
            /* Parametre. */
            int hasParam = 0, neg = 0;
            long param = 0;
            size_t k = j;
            if (k < rtflen && rtf[k] == '-') { neg = 1; k++; }
            if (k < rtflen && rtf[k] >= '0' && rtf[k] <= '9') {
                hasParam = 1;
                while (k < rtflen && rtf[k] >= '0' && rtf[k] <= '9') {
                    param = param * 10 + (rtf[k] - '0'); k++;
                }
                if (neg) param = -param;
            }
            /* Espace delimiteur consomme. */
            if (k < rtflen && rtf[k] == ' ') k++;
            i = k;

            /* Interpretation. */
            if (strcmp(word, "htmlrtf") == 0) {
                g->htmlrtf = (hasParam && param == 0) ? 0 : 1;
                pendingStar = 0;
            } else if (strcmp(word, "htmltag") == 0) {
                /* Destination HTML : on la garde (contenu emis normalement). */
                pendingStar = 0;
            } else if (strcmp(word, "mhtmltag") == 0) {
                g->ignore = 1; pendingStar = 0;
            } else if (strcmp(word, "uc") == 0) {
                if (hasParam) g->uc = (int)param;
                pendingStar = 0;
            } else if (strcmp(word, "u") == 0) {
                long cp = param; if (cp < 0) cp += 65536;
                if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, (uint32_t)cp);
                /* Saute les caracteres de repli (uc). */
                int skip = g->uc;
                while (skip > 0 && i < rtflen) {
                    if (rtf[i] == '\\' && i + 1 < rtflen && rtf[i + 1] == '\'') {
                        i += 4;   /* \'XX */
                    } else if (rtf[i] == '{' || rtf[i] == '}') {
                        break;
                    } else {
                        i++;
                    }
                    skip--;
                }
                pendingStar = 0;
            } else if (strcmp(word, "par") == 0 || strcmp(word, "line") == 0 ||
                       strcmp(word, "br") == 0) {
                if (!g->htmlrtf && !g->ignore) hb_putc(&out, '\n');
                pendingStar = 0;
            } else if (strcmp(word, "tab") == 0) {
                if (!g->htmlrtf && !g->ignore) hb_putc(&out, '\t');
                pendingStar = 0;
            } else if (strcmp(word, "lquote") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x2018); pendingStar = 0; }
              else if (strcmp(word, "rquote") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x2019); pendingStar = 0; }
              else if (strcmp(word, "ldblquote") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x201C); pendingStar = 0; }
              else if (strcmp(word, "rdblquote") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x201D); pendingStar = 0; }
              else if (strcmp(word, "endash") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x2013); pendingStar = 0; }
              else if (strcmp(word, "emdash") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x2014); pendingStar = 0; }
              else if (strcmp(word, "bullet") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x2022); pendingStar = 0; }
              else if (strcmp(word, "nbsp") == 0) { if (!g->htmlrtf && !g->ignore) hb_put_cp(&out, 0x00A0); pendingStar = 0; }
              else {
                /* Mot de controle non gere. */
                if (pendingStar) {
                    /* Destination optionnelle inconnue : ignorer le groupe. */
                    g->ignore = 1;
                    pendingStar = 0;
                } else if (rtf_is_ignored_dest(word)) {
                    g->ignore = 1;
                }
                /* sinon : formatage RTF pur, aucune sortie. */
            }
            continue;
        }

        /* Texte litteral. */
        if (!g->htmlrtf && !g->ignore) hb_putc(&out, c);
        i++;
    }

    if (!out.buf) { out.buf = malloc(1); if (out.buf) out.buf[0] = '\0'; out.len = 0; }
    else out.buf[out.len] = '\0';
    return out.buf;
}

/* ============================================================
 * Remplissage de la variable Email.
 * ============================================================ */

static int msg_is_email(ObjInstance* inst) {
    return inst && inst->structName != NULL &&
           strcmp(inst->structName->chars, "Email") == 0;
}

static ObjArray* msg_field_array(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_ARRAY(v)) return NULL;
    return AS_ARRAY(v);
}

static void msg_array_add_str(ObjInstance* e, const char* champ, const char* val) {
    ObjArray* arr = msg_field_array(e, champ);
    if (!arr) return;
    Value s = OBJ_VAL(obj_copy_string(val, (int)strlen(val)));
    vm_push(s);
    obj_array_push(arr, s);   /* helper canon : racine interne ; vm_push par surete */
    vm_pop();
}

/* Deduit un type MIME best-effort a partir de l'extension du nom de fichier. */
static const char* msg_mime_from_name(const char* name) {
    const char* dot = NULL;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return "application/octet-stream";
    if (msg_name_ieq(dot, ".pdf"))  return "application/pdf";
    if (msg_name_ieq(dot, ".txt"))  return "text/plain";
    if (msg_name_ieq(dot, ".html") || msg_name_ieq(dot, ".htm")) return "text/html";
    if (msg_name_ieq(dot, ".png"))  return "image/png";
    if (msg_name_ieq(dot, ".jpg") || msg_name_ieq(dot, ".jpeg")) return "image/jpeg";
    if (msg_name_ieq(dot, ".gif"))  return "image/gif";
    if (msg_name_ieq(dot, ".zip"))  return "application/zip";
    if (msg_name_ieq(dot, ".doc"))  return "application/msword";
    if (msg_name_ieq(dot, ".xls"))  return "application/vnd.ms-excel";
    return "application/octet-stream";
}

/* Traite un sous-storage recipient : email + type -> Destinataire/Cc/Cci. */
static void msg_handle_recipient(OLE2* ole, const MsgDirEntry* entries,
                                 size_t count, uint32_t recipIdx, ObjInstance* e) {
    MsgIdxList kids = msg_children(entries, count, recipIdx);

    char* email = msg_read_string_prop(ole, entries, &kids, PID_SMTP_ADDRESS);
    if (!email || email[0] == '\0') {
        free(email);
        email = msg_read_string_prop(ole, entries, &kids, PID_EMAIL_ADDRESS);
    }

    uint32_t rtype = 1;   /* defaut : To */
    msg_read_long_prop(ole, entries, &kids,
                       (PID_RECIPIENT_TYPE << 16) | MAPI_T_LONG, 8, &rtype);

    if (email && email[0]) {
        const char* field = "Destinataire";
        if (rtype == 2) field = "Cc";
        else if (rtype == 3) field = "Cci";
        msg_array_add_str(e, field, email);
    }
    free(email);
    idxlist_free(&kids);
}

/* Traite un sous-storage attachment : nom + contenu + type MIME -> ..Attache. */
static void msg_handle_attachment(OLE2* ole, const MsgDirEntry* entries,
                                  size_t count, uint32_t attIdx, ObjInstance* e) {
    MsgIdxList kids = msg_children(entries, count, attIdx);

    char* nom = msg_read_string_prop(ole, entries, &kids, PID_ATTACH_LONGNAME);
    if (!nom || nom[0] == '\0') {
        free(nom);
        nom = msg_read_string_prop(ole, entries, &kids, PID_ATTACH_FILENAME);
    }
    char* mime = msg_read_string_prop(ole, entries, &kids, PID_ATTACH_MIMETAG);

    size_t dlen = 0;
    unsigned char* data = NULL;
    uint32_t didx;
    if (msg_find_substg(entries, &kids, (PID_ATTACH_DATA << 16) | MAPI_T_BINARY, &didx)) {
        data = msg_read_stream(ole, &entries[didx], &dlen);
    }

    Value attV = owl_clone_global_template("emailAttache");
    if (IS_INSTANCE(attV)) {
        vm_push(attV);                          /* racine avant toute alloc */
        ObjInstance* att = AS_INSTANCE(attV);
        ObjBuffer* buf = (data && dlen > 0) ? obj_new_buffer(data, (int)dlen)
                                            : obj_buffer_empty(0);
        inst_field_set_value(att, "Contenu", OBJ_VAL(buf));   /* canon */
        inst_field_set_str(att, "Nom", nom ? nom : "");
        if (mime && mime[0]) inst_field_set_str(att, "ContentType", mime);
        else inst_field_set_str(att, "ContentType", msg_mime_from_name(nom ? nom : ""));
        ObjArray* arr = msg_field_array(e, "Attache");
        if (arr) obj_array_push(arr, attV);     /* attV racine sur la pile */
        vm_pop();
    }

    free(nom);
    free(mime);
    free(data);
    idxlist_free(&kids);
}

/* Extrait le corps HTML : PidTagRtfCompressed (LZFu + de-encapsulation) sinon
 * PidTagHtml direct s'il est non-junk. */
static void msg_fill_html(OLE2* ole, const MsgDirEntry* entries,
                          const MsgIdxList* top, ObjInstance* e) {
    uint32_t idx;
    /* Chemin principal : RTF compresse -> HTML. */
    if (msg_find_substg(entries, top, (PID_RTF_COMPRESSED << 16) | MAPI_T_BINARY, &idx)) {
        size_t rlen = 0;
        unsigned char* raw = msg_read_stream(ole, &entries[idx], &rlen);
        if (raw) {
            size_t rtflen = 0;
            char* rtf = lzfu_decompress(raw, rlen, &rtflen);
            free(raw);
            if (rtf) {
                char* html = rtf_to_html(rtf, rtflen);
                free(rtf);
                if (html && html[0]) {
                    inst_field_set_str(e, "HTML", html);
                    free(html);
                    return;
                }
                free(html);
            }
        }
    }
    /* Fallback : PidTagHtml direct (binaire ou unicode), si non-junk. */
    char* h = msg_read_string_prop(ole, entries, top, PID_HTML);
    if (!h) {
        if (msg_find_substg(entries, top, (PID_HTML << 16) | MAPI_T_BINARY, &idx)) {
            size_t len = 0;
            unsigned char* raw = msg_read_stream(ole, &entries[idx], &len);
            if (raw && len > 16) {
                char* s = malloc(len + 1);
                if (s) { memcpy(s, raw, len); s[len] = '\0';
                         inst_field_set_str(e, "HTML", s); free(s); }
            }
            free(raw);
        }
        return;
    }
    if (strlen(h) > 16) inst_field_set_str(e, "HTML", h);
    free(h);
}

static void msg_fill_email(OLE2* ole, const MsgDirEntry* entries, size_t count,
                           ObjInstance* e) {
    /* Racine = premiere entree de type 5 (sinon index 0). */
    uint32_t rootIdx = 0;
    for (size_t i = 0; i < count; i++) if (entries[i].type == PS_USER_ROOT) { rootIdx = (uint32_t)i; break; }

    MsgIdxList top = msg_children(entries, count, rootIdx);

    /* En-tetes simples. */
    char* s;
    if ((s = msg_read_string_prop(ole, entries, &top, PID_SENTREPR_EMAIL))) {
        inst_field_set_str(e, "Expediteur", s); free(s);
    }
    if ((s = msg_read_string_prop(ole, entries, &top, PID_SUBJECT))) {
        inst_field_set_str(e, "Sujet", s); free(s);
    }
    if ((s = msg_read_string_prop(ole, entries, &top, PID_BODY))) {
        inst_field_set_str(e, "Message", s); free(s);
    }
    if ((s = msg_read_string_prop(ole, entries, &top, PID_MESSAGE_ID))) {
        inst_field_set_str(e, "MessageID", s); free(s);
    }

    msg_fill_html(ole, entries, &top, e);

    /* Sous-storages : recipients puis attachments. */
    for (size_t i = 0; i < top.count; i++) {
        uint32_t ci = top.idx[i];
        const MsgDirEntry* c = &entries[ci];
        if (c->type != PS_USER_STORAGE) continue;
        if (msg_name_prefix(c->name, "__recip_version1.0_"))
            msg_handle_recipient(ole, entries, count, ci, e);
        else if (msg_name_prefix(c->name, "__attach_version1.0_"))
            msg_handle_attachment(ole, entries, count, ci, e);
    }

    idxlist_free(&top);
}

/* ============================================================
 * Native EmailImporteMSG(email, chemin) -> booleen.
 * ============================================================ */

static Value fn_email_importe_msg(int argCount, Value* args) {
    (void)argCount;
    if (!IS_INSTANCE(args[0]) || !msg_is_email(AS_INSTANCE(args[0]))) {
        const char* actual = "<non-structuré>";
        if (IS_INSTANCE(args[0]) && AS_INSTANCE(args[0])->structName != NULL)
            actual = AS_INSTANCE(args[0])->structName->chars;
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailImporteMSG",
                              "native", "EmailImporteMSG", "arg", "1",
                              "expected", "Email", "actual", actual, NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailImporteMSG",
                              "native", "EmailImporteMSG", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* e = AS_INSTANCE(args[0]);
    const char* chemin = AS_CSTRING(args[1]);

    /* Ouverture + garde-fous magic / taille avant de confier a libxls. */
    FILE* f = fopen(chemin, "rb");
    if (f == NULL) {
        owl_emit_native_error(E_NATIVE_IO_OPEN_FAILURE, "EmailImporteMSG",
                              "native", "EmailImporteMSG",
                              "path", chemin, "detail", "fichier introuvable", NULL);
        return BOOL_VAL(false);
    }
    unsigned char magic[8];
    size_t mg = fread(magic, 1, sizeof(magic), f);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fclose(f);

    static const unsigned char OLE2_MAGIC[8] =
        { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    if (mg < 8 || memcmp(magic, OLE2_MAGIC, 8) != 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "EmailImporteMSG",
                              "native", "EmailImporteMSG",
                              "path", chemin, "detail", "format non reconnu (pas un conteneur OLE2)", NULL);
        return BOOL_VAL(false);
    }
    if (fsz < 0 || (unsigned long)fsz > MSG_MAX_FILE_SIZE) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "EmailImporteMSG",
                              "native", "EmailImporteMSG",
                              "path", chemin, "detail", "fichier trop volumineux", NULL);
        return BOOL_VAL(false);
    }

    OLE2* ole = ole2_open_file(chemin);
    if (ole == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "EmailImporteMSG",
                              "native", "EmailImporteMSG",
                              "path", chemin, "detail", "conteneur OLE2 illisible", NULL);
        return BOOL_VAL(false);
    }

    size_t count = 0;
    MsgDirEntry* entries = msg_read_directory(ole, &count);
    if (entries == NULL || count == 0) {
        free(entries);
        ole2_close(ole);
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "EmailImporteMSG",
                              "native", "EmailImporteMSG",
                              "path", chemin, "detail", "repertoire du conteneur vide ou illisible", NULL);
        return BOOL_VAL(false);
    }

    msg_fill_email(ole, entries, count, e);   /* e rootee (args[0] sur la pile VM) */

    free(entries);
    ole2_close(ole);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * Definition native + init.
 * ============================================================ */

static const OwlParam P_IMPORTE_MSG[] = {
    {"email",  "Email",  "message à remplir à partir du fichier .msg"},
    {"chemin", "chaîne", "nom et chemin du fichier .msg à importer"},
    {0}
};
static const OwlSignature SIG_IMPORTE_MSG[] = {
    {2, P_IMPORTE_MSG, "booléen",
     "vrai si le fichier .msg a été lu et l'Email rempli, faux sinon"},
    {0}
};

static const OwlNativeDef DEFS_EMAIL_MSG[] = {
    { .canonical      = "EmailImporteMSG",
      .canonicalAscii = NULL,
      .nameEN         = "EmailImportMSG",
      .category       = "Email",
      .descFR         = "Importe un fichier .msg Outlook (lecture seule) et remplit un Email à partir de son contenu : expéditeur, destinataires, copies, sujet, corps texte et HTML, pièces jointes.",
      .descEN         = "Imports an Outlook .msg file (read-only) and fills an Email from its content: sender, recipients, copies, subject, text and HTML body, attachments.",
      .signatures     = SIG_IMPORTE_MSG,
      .impl           = fn_email_importe_msg },
};

void stdlib_email_msg_init(void) {
    owl_registry_register_module(DEFS_EMAIL_MSG,
        (int)(sizeof(DEFS_EMAIL_MSG) / sizeof(DEFS_EMAIL_MSG[0])));
}
