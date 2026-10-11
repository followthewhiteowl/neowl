// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL compression stdlib.
 *
 * Phase 1 livrée dans ce fichier : buffer Compresse / Décompresse.
 * Backends archives (ZIP/TAR/TGZ/7z) arrivent dans des fichiers séparés
 * (compress_zip.c, compress_tar.c, compress_7z.c) et seront dispatchés
 * depuis les natives zip* (phase H du plan).
 *
 * Buffer API (canon) :
 *   Compresse(donnees [, type [, niveau]])  -> buffer
 *   Decompresse(bufferCompresse [, type])   -> buffer
 *
 * Types supportés (constantes compresse*) :
 *   compresseLZW          0  -- alias deflate brut (voir docs/VISION §5bis)
 *   compresseGZIP         1  -- deflate + header/footer gzip (RFC 1952)
 *   compresseAucun        2  -- store (passthrough, utile pour interop)
 *   compresseChaineCourte 3  -- alias deflate brut (optimisé courtes chaines)
 *
 * Divergence documentée : le défaut canon historique = compresseLZW.
 * miniz ne fait pas LZW. Notre compresseLZW est mappé sur deflate brut
 * (le canon moderne pousse compresseGZIP de toute façon).
 *
 * Defaults :
 *   Compresse(data)        -> gzip, niveau 6
 *   Decompresse(data)      -> auto-detect (magic 0x1F8B -> gzip, sinon raw deflate)
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#define OWL_ERRORS_SKIP_COMPILE_AWARE  /* éviter scanner.h vs windows.h winnt.h TokenType */
#include "owl_errors.h"
#include "memory.h"
#include "vm.h"
#include "object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define OWL_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <dirent.h>
#define OWL_MKDIR(p) mkdir(p, 0755)
#endif

/* Forward declarations for 7z backend functions */
void* owl_7z_open(const char* path, const char* mode);
void  owl_7z_close(void* state);
int   owl_7z_entry_count(void* state);
char* owl_7z_entry_name(void* state, int idx);
void* owl_7z_extract_to_mem(void* state, int idx, size_t* outLen);
int   owl_7z_extract_to_file(void* state, int idx, const char* outPath);
void* owl_7z_extract_by_name_to_mem(void* state, const char* name, size_t* outLen);

/* zipInfoFichier element codes — déclarés tôt car utilisés dans les
 * constantes globales plus bas. */
#define ZIP_INFO_NOM     0
#define ZIP_INFO_TAILLE  1
#define ZIP_INFO_TAILLEC 2
#define ZIP_INFO_DATE    3
#define ZIP_INFO_CRC     4
#define ZIP_INFO_ATTRIB  6

/* ================================================================
 * Constantes — catégorie "Compression"
 * ================================================================ */

/* compresse* — sentinelles centralisees. */
#include "sentinels.h"
#define COMP_LZW               OWL_COMPRESSE_LZW
#define COMP_GZIP              OWL_COMPRESSE_GZIP
#define COMP_AUCUN             OWL_COMPRESSE_AUCUN
#define COMP_CHAINE_COURTE     OWL_COMPRESSE_CHAINE_COURTE

#define ZIP_LEVEL_RAPIDE       1
#define ZIP_LEVEL_NORMAL       6
#define ZIP_LEVEL_MAX          9

#define C_COMP(nm, val, dfr, den) {nm, "Compression", INTEGER_VAL(val), dfr, den}
/* Variante avec nameEN explicite (invariant bilingue n°3,  lot A5).
 * Poser nameEN suffit : l'alias EN est enregistre runtime-only via Lot 0.
 * Cas EN==FR (zipFormat*) : re-set benin de la meme cle/valeur, pose quand
 * meme pour satisfaire la digue finale (nameEN obligatoire). */
#define C_COMP_EN(nm, val, dfr, den, nen) {nm, "Compression", INTEGER_VAL(val), dfr, den, nen}

/* Sentinelle pour zipExtraitFichier(<archive>, <entry>, zipEnMémoire). */
#define ZIP_EN_MEMOIRE_SENTINEL 65535

static const OwlConstDef COMP_CONSTS[] = {
    /* Algorithmes de compression buffer */
    C_COMP_EN("compresseLZW",        COMP_LZW,
           "Alias deflate (miniz ne fait pas LZW, voir VISION §5bis)",
           "Alias of deflate (miniz has no LZW backend)", "compressLZW"),
    C_COMP_EN("compresseGZIP",       COMP_GZIP,
           "Format gzip (deflate + header RFC 1952)",
           "gzip format (deflate + RFC 1952 header)", "compressGZIP"),
    C_COMP_EN("compresseAucun",      COMP_AUCUN,
           "Pas de compression (store)",
           "No compression (store)", "compressNone"),
    C_COMP_EN("compresseCha\xc3\xaeneCourte", COMP_CHAINE_COURTE,
           "Alias deflate, optimisé pour chaînes courtes",
           "Alias deflate, tuned for short strings", "compressShortString"),

    /* Niveaux de compression (0-9) */
    C_COMP_EN("zipCompressionRapide",   ZIP_LEVEL_RAPIDE,
           "Niveau de compression rapide (1)",
           "Fast compression level (1)", "zipFastCompression"),
    C_COMP_EN("zipCompressionNormale",  ZIP_LEVEL_NORMAL,
           "Niveau de compression par défaut (6)",
           "Default compression level (6)", "zipNormalCompression"),
    C_COMP_EN("zipCompressionMaximale", ZIP_LEVEL_MAX,
           "Niveau de compression maximale (9)",
           "Maximum compression level (9)", "zipMaxCompression"),

    /* Formats archive (pour zipCrée/zipOuvre). */
    C_COMP_EN("zipFormatZIP",   ZIP_FMT_ZIP,   "Archive ZIP standard",          "Standard ZIP", "zipFormatZIP"),
    C_COMP_EN("zipFormatCAB",   ZIP_FMT_CAB,   "Format d'archive Cabinet (.cab)", "Cabinet archive format (.cab)", "zipFormatCAB"),
    C_COMP_EN("zipFormatRAR",   ZIP_FMT_RAR,   "Format d'archive RAR (.rar)",     "RAR archive format (.rar)", "zipFormatRAR"),
    C_COMP_EN("zipFormatWDZ",   ZIP_FMT_WDZ,   "Archive WDZ (extension .wdz, moteur ZIP partagé, sentinel distinct pour préserver l'identification du format)", "WDZ archive (.wdz extension, ZIP engine shared, distinct sentinel preserves format identification)", "zipFormatWDZ"),
    C_COMP_EN("zipFormat7Z",    ZIP_FMT_7Z,    "Archive 7-Zip (lecture)",      "7-Zip archive (read)", "zipFormat7Z"),
    C_COMP_EN("zipFormatTAR",   ZIP_FMT_TAR,   "Archive TAR brute",             "Plain TAR", "zipFormatTAR"),
    C_COMP_EN("zipFormatTARGZ", ZIP_FMT_TARGZ, "TAR + gzip",                    "TAR + gzip", "zipFormatTARGZ"),

    /* Modes d'ouverture. */
    C_COMP_EN("zipLecture",           ZIP_MODE_READ,   "Mode lecture",          "Read mode",  "zipRead"),
    C_COMP_EN("zipEcriture",          ZIP_MODE_WRITE,  "Mode écriture",         "Write mode", "zipWrite"),
    C_COMP_EN("zipCr\xc3\xa9""ationSiInexistant", ZIP_MODE_CREATE, "Crée si absent", "Create if absent", "zipCreateIfNotFound"),

    /* Portion de chemin à conserver lors de l'ajout. */
    C_COMP_EN("zipAucun",   0, "Nom seul (basename)",            "Base name only", "zipNone"),
    C_COMP_EN("zipR\xc3\xa9""pertoire", 1, "Chemin relatif",        "Relative path", "zipDirectory"),
    C_COMP_EN("zipDisque",  2, "Chemin absolu",                  "Absolute path", "zipDrive"),

    /* Éléments pour zipInfoFichier. */
    C_COMP_EN("zipNom",                 ZIP_INFO_NOM,     "Nom de l'entrée",     "Entry name", "zipName"),
    C_COMP_EN("zipTaille",              ZIP_INFO_TAILLE,  "Taille décompressée", "Uncompressed size", "zipSize"),
    C_COMP_EN("zipTailleCompress\xc3\xa9""e", ZIP_INFO_TAILLEC, "Taille compressée", "Compressed size", "zipCompressedSize"),
    C_COMP_EN("zipDate",                ZIP_INFO_DATE,    "Date modification",   "Modification date", "zipDate"),
    C_COMP_EN("zipCRC",                 ZIP_INFO_CRC,     "CRC-32",              "CRC-32", "zipCRC"),
    C_COMP_EN("zipAttributs",           ZIP_INFO_ATTRIB,  "Attributs fichier",   "File attributes", "zipAttributes"),
    /* Sentinelle 'memoire' : cible pour zipExtraitFichier (retourne le buffer)
     * ET mode de gestion buffer pour zipAjouteFichier (sauve le buffer dans
     * l'archive). */
    C_COMP_EN("zipEnM\xc3\xa9moire", ZIP_EN_MEMOIRE_SENTINEL,
           "En mémoire : extrait vers un buffer, ou ajoute un buffer à l'archive.",
           "In-memory: extract to a buffer, or add a buffer to the archive.", "zipInMemory"),
};

/* ================================================================
 * Enums — modeling des familles de constantes Compression
 *
 * Modeling pur (aucun changement de comportement runtime) : ces OwlEnum
 * regroupent des constantes deja enregistrees dans COMP_CONSTS et cablent
 * les params concernes via .allowedBase, pour que le LSP propose la liste
 * stricte au lieu d'un 'entier' nu. Les noms de values doivent matcher
 * exactement les canon de COMP_CONSTS (self-test rule 5).
 *
 * force_small_int (rule 20) :
 *  - compresseType : values en zone sentinel (OWL_COMPRESSE_BASE, negatif),
 *    donc PAS besoin de force_small_int.
 *  - zipFormat / zipPortionChemin / zipInfo : valeurs entieres positives
 *    SUR lesquelles le runtime switche directement (fmt==ZIP_FMT_ZIP,
 *    element==ZIP_INFO_NOM, etc.). La valeur porte la semantique runtime
 *    => force_small_int=true assume (sentinel-er casserait le switch).
 *  - zipMode : ZIP_MODE_READ/WRITE/CREATE = 1/2/4, bitmask reel
 *    (mode & ZIP_MODE_WRITE) => combinable=true (rule 20 skip les bitmasks).
 */

static const char* const E_compresseType_values[] = {
    "compresseLZW", "compresseGZIP", "compresseAucun",
    "compresseCha\xc3\xaeneCourte", NULL
};
static const OwlEnum E_compresseType = {
    .name="compresseType", .nameAscii="compresseType", .nameEN="compressType",
    .category="Compression",
    .descFR="Algorithme retenu par le paramètre type de Compresse et Décompresse : alias deflate, gzip (deflate + en-tête) ou store (aucune compression).",
    .descEN="Algorithm selected by the type parameter of Compress and Uncompress: deflate alias, gzip (deflate + header) or store (no compression).",
    .values=E_compresseType_values, .combinable=false
};

static const char* const E_zipFormat_values[] = {
    "zipFormatZIP", "zipFormatCAB", "zipFormatRAR", "zipFormatWDZ",
    "zipFormat7Z", "zipFormatTAR", "zipFormatTARGZ", NULL
};
static const OwlEnum E_zipFormat = {
    .name="zipFormat", .nameAscii="zipFormat", .nameEN="zipFormat",
    .category="Compression",
    .descFR="Format d'archive choisi pour zipCrée ou imposé à zipOuvre : ZIP, WDZ, CAB, RAR, 7z, TAR et TAR.GZ.",
    .descEN="Archive format chosen for zipCreate or forced on zipOpen: ZIP, WDZ, CAB, RAR, 7z, TAR and TAR.GZ.",
    .values=E_zipFormat_values, .combinable=false, .force_small_int=true
};

static const char* const E_zipMode_values[] = {
    "zipLecture", "zipEcriture", "zipCr\xc3\xa9""ationSiInexistant", NULL
};
static const OwlEnum E_zipMode = {
    .name="zipMode", .nameAscii="zipMode", .nameEN="zipMode",
    .category="Compression",
    .descFR="Mode d'ouverture d'une archive : lecture, écriture ou création si absente. Les modes se combinent (écriture cumulable avec création).",
    .descEN="Archive open mode: read, write or create if absent. Modes combine (write can be combined with create).",
    .values=E_zipMode_values, .combinable=true
};

static const char* const E_zipPortionChemin_values[] = {
    "zipAucun", "zipR\xc3\xa9""pertoire", "zipDisque", "zipEnM\xc3\xa9moire", NULL
};
static const OwlEnum E_zipPortionChemin = {
    .name="zipPortionChemin", .nameAscii="zipPortionChemin", .nameEN="zipPathPortion",
    .category="Compression",
    .descFR="Portion de chemin conservée lors de l'ajout d'une entrée : nom seul, chemin relatif ou chemin absolu. La valeur en mémoire ajoute un buffer plutôt qu'un fichier disque.",
    .descEN="Path portion kept when adding an entry: base name only, relative path or absolute path. The in-memory value adds a buffer instead of a disk file.",
    .values=E_zipPortionChemin_values, .combinable=false, .force_small_int=true
};

static const char* const E_zipInfo_values[] = {
    "zipNom", "zipTaille", "zipTailleCompress\xc3\xa9""e",
    "zipDate", "zipCRC", "zipAttributs", NULL
};
static const OwlEnum E_zipInfo = {
    .name="zipInfo", .nameAscii="zipInfo", .nameEN="zipInfo",
    .category="Compression",
    .descFR="Métadonnée d'entrée interrogée par zipInfoFichier : nom, taille décompressée, taille compressée, date, CRC-32 ou attributs.",
    .descEN="Entry metadata queried by zipInfoFile: name, uncompressed size, compressed size, date, CRC-32 or attributes.",
    .values=E_zipInfo_values, .combinable=false, .force_small_int=true
};

static const OwlEnum* const COMP_ENUMS[] = {
    &E_compresseType, &E_zipFormat, &E_zipMode,
    &E_zipPortionChemin, &E_zipInfo
};

/* ================================================================
 * Helpers
 * ================================================================ */

/* Extract bytes + length from a Value that is either chaîne or buffer.
 * Returns 1 on success, 0 on type mismatch. */
static int args_bytes(Value v, const unsigned char** out, int* outLen) {
    return value_as_bytes(v, out, outLen);
}

/* CRC32 (miniz helper) + little-endian write of 4 bytes. */
static void write_le32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Build a minimal gzip envelope around raw deflate data.
 * Format (RFC 1952) : 10-byte fixed header || deflate stream || CRC32 || ISIZE.
 * No FNAME / FEXTRA / FCOMMENT. OS = 0xFF (unknown). */
static ObjBuffer* wrap_gzip(const unsigned char* defl, int deflLen,
                             uint32_t crc, uint32_t isize) {
    int total = 10 + deflLen + 8;
    ObjBuffer* out = obj_buffer_empty(total);
    unsigned char header[10] = {
        0x1f, 0x8b,            /* magic */
        0x08,                  /* method = deflate */
        0x00,                  /* flags */
        0x00, 0x00, 0x00, 0x00,/* mtime = 0 */
        0x00,                  /* xflags */
        0xff                   /* OS = unknown */
    };
    obj_buffer_append(out, header, 10);
    obj_buffer_append(out, defl, deflLen);
    unsigned char footer[8];
    write_le32(footer, crc);
    write_le32(footer + 4, isize);
    obj_buffer_append(out, footer, 8);
    return out;
}

/* Parse gzip header; returns offset of deflate payload or -1 on error.
 * Same logic as stdlib_http.c:parse_gzip_header (kept private to avoid
 * cross-module coupling — identical behaviour). */
static int gzip_header_size(const unsigned char* in, int len) {
    if (len < 10) return -1;
    if (in[0] != 0x1f || in[1] != 0x8b) return -1;
    if (in[2] != 8) return -1;
    int flags = in[3];
    int p = 10;
    if (flags & 0x04) { /* FEXTRA */
        if (p + 2 > len) return -1;
        int xlen = in[p] | (in[p+1] << 8);
        p += 2 + xlen;
    }
    if (flags & 0x08) { while (p < len && in[p] != 0) p++; p++; }
    if (flags & 0x10) { while (p < len && in[p] != 0) p++; p++; }
    if (flags & 0x02) p += 2;
    if (p > len) return -1;
    return p;
}

/* ================================================================
 * Compresse(data [, type [, niveau]])
 * ================================================================ */

static Value fn_compresse(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "Compresse",
                              "native", "Compresse",
                              "arg", "données",
                              NULL);
        return NIL_VAL;
    }

    const unsigned char* src = NULL;
    int srcLen = 0;
    if (!args_bytes(args[0], &src, &srcLen)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Compresse",
                              "native", "Compresse",
                              "arg", "données",
                              "expected", "chaîne ou buffer",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }

    int type = COMP_GZIP; /* default */
    if (argCount >= 2) {
        if (!IS_INTEGER(args[1])) {
            owl_emit_native_error(E_NATIVE_COMPRESSION_INVALID_TYPE, "Compresse",
                                  "native", "Compresse",
                                  "value", "non-entier",
                                  NULL);
            return NIL_VAL;
        }
        type = (int)AS_INTEGER(args[1]);
    }

    int level = ZIP_LEVEL_NORMAL;
    if (argCount >= 3) {
        if (!IS_INTEGER(args[2])) {
            owl_emit_native_error(E_NATIVE_COMPRESSION_INVALID_LEVEL, "Compresse",
                                  "native", "Compresse",
                                  "value", "non-entier",
                                  NULL);
            return NIL_VAL;
        }
        level = (int)AS_INTEGER(args[2]);
        if (level < 0) level = 0;
        if (level > 9) level = 9;
    }

    /* compresseAucun : passthrough store. Même entrée = même sortie (buffer). */
    if (type == COMP_AUCUN) {
        ObjBuffer* out = obj_new_buffer(src, srcLen);
        owl_clear_error();
        return OBJ_VAL(out);
    }

    /* tdefl flags : niveau + éventuel wrapper zlib/gzip.
     * Pour compresseGZIP on écrit notre propre wrapper RFC 1952 à la main
     * (miniz n'expose pas un mode gzip one-shot direct ; seule la forme
     * stream le permet). Pour compresseLZW/ChaineCourte/défaut on sort du
     * deflate brut sans header zlib. */
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(level, -15, 0);
    /* window_bits négatif = raw deflate sans header zlib */

    size_t outLen = 0;
    void* heapOut = tdefl_compress_mem_to_heap(src, (size_t)srcLen, &outLen, flags);
    if (heapOut == NULL) {
        owl_emit_native_error(E_NATIVE_COMPRESSION_MEMORY_FAILURE, "Compresse",
                              "native", "Compresse",
                              NULL);
        return NIL_VAL;
    }

    ObjBuffer* buf;
    if (type == COMP_GZIP) {
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, src, (size_t)srcLen);
        buf = wrap_gzip((unsigned char*)heapOut, (int)outLen, crc, (uint32_t)srcLen);
    } else {
        /* LZW, ChaineCourte : raw deflate */
        buf = obj_new_buffer(heapOut, (int)outLen);
    }
    free(heapOut);

    owl_clear_error();
    return OBJ_VAL(buf);
}

/* ================================================================
 * Décompresse(bufferCompressé [, type])
 * ================================================================ */

static Value fn_decompresse(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "Décompresse",
                              "native", "Décompresse",
                              "arg", "données",
                              NULL);
        return NIL_VAL;
    }

    const unsigned char* src = NULL;
    int srcLen = 0;
    if (!args_bytes(args[0], &src, &srcLen)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Décompresse",
                              "native", "Décompresse",
                              "arg", "données",
                              "expected", "chaîne ou buffer",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }

    int type = -1; /* -1 = auto */
    if (argCount >= 2) {
        if (!IS_INTEGER(args[1])) {
            owl_emit_native_error(E_NATIVE_COMPRESSION_INVALID_TYPE, "Décompresse",
                                  "native", "Décompresse",
                                  "value", "non-entier",
                                  NULL);
            return NIL_VAL;
        }
        type = (int)AS_INTEGER(args[1]);
    }

    /* compresseAucun : passthrough. */
    if (type == COMP_AUCUN) {
        ObjBuffer* out = obj_new_buffer(src, srcLen);
        owl_clear_error();
        return OBJ_VAL(out);
    }

    /* Auto-detect or explicit GZIP : skip RFC 1952 header if magic matches. */
    const unsigned char* deflStart = src;
    int deflLen = srcLen;
    if (type == COMP_GZIP || (type == -1 && srcLen >= 2 && src[0] == 0x1f && src[1] == 0x8b)) {
        int hdr = gzip_header_size(src, srcLen);
        if (hdr < 0) {
            owl_emit_native_error(E_NATIVE_COMPRESSION_INVALID_HEADER, "Décompresse",
                                  "native", "Décompresse",
                                  NULL);
            return NIL_VAL;
        }
        deflStart = src + hdr;
        /* Derniers 8 octets = CRC32 + ISIZE, non vérifiés (on fait confiance). */
        deflLen = srcLen - hdr - 8;
        if (deflLen < 0) {
            owl_emit_native_error(E_NATIVE_COMPRESSION_TRUNCATED, "Décompresse",
                                  "native", "Décompresse",
                                  NULL);
            return NIL_VAL;
        }
    }

    /* Décompression raw deflate vers heap. */
    size_t outLen = 0;
    void* heapOut = tinfl_decompress_mem_to_heap(deflStart, (size_t)deflLen,
                                                  &outLen, 0 /* raw deflate */);
    if (heapOut == NULL) {
        /* Fallback : si type=auto et pas gzip, peut-être que l'entrée a
         * un header zlib (0x78 0x9C / 0x78 0xDA / 0x78 0x01). Réessayons
         * avec TINFL_FLAG_PARSE_ZLIB_HEADER. */
        if (type == -1 || type == COMP_LZW || type == COMP_CHAINE_COURTE) {
            heapOut = tinfl_decompress_mem_to_heap(src, (size_t)srcLen, &outLen,
                                                    TINFL_FLAG_PARSE_ZLIB_HEADER);
        }
        if (heapOut == NULL) {
            owl_emit_native_error(E_NATIVE_COMPRESSION_CORRUPT_DATA, "Décompresse",
                                  "native", "Décompresse",
                                  NULL);
            return NIL_VAL;
        }
    }

    ObjBuffer* out = obj_new_buffer(heapOut, (int)outLen);
    free(heapOut);
    owl_clear_error();
    return OBJ_VAL(out);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const OwlParam p_compresse1[] = {
    {"donn\xc3\xa9""es", "cha\xc3\xaene|buffer", "Données à compresser"},
    {NULL, NULL, NULL}
};
static const OwlParam p_compresse2[] = {
    {"donn\xc3\xa9""es", "cha\xc3\xaene|buffer", "Données à compresser"},
    {"type",             "entier",               "Algorithme (constante compresse*)", .allowedBase=&E_compresseType},
    {NULL, NULL, NULL}
};
static const OwlParam p_compresse3[] = {
    {"donn\xc3\xa9""es", "cha\xc3\xaene|buffer", "Données à compresser"},
    {"type",             "entier",               "Algorithme (constante compresse*)", .allowedBase=&E_compresseType},
    {"niveau",           "entier",               "Niveau de compression 0-9 (défaut 6)"},
    {NULL, NULL, NULL}
};
static const OwlParam p_decompresse1[] = {
    {"donn\xc3\xa9""es", "buffer", "Données compressées"},
    {NULL, NULL, NULL}
};
static const OwlParam p_decompresse2[] = {
    {"donn\xc3\xa9""es", "buffer", "Données compressées"},
    {"type",             "entier", "Algorithme (auto-détecté par défaut)", .allowedBase=&E_compresseType},
    {NULL, NULL, NULL}
};

static const OwlSignature sig_compresse[]   = {
    {1, p_compresse1, "buffer", "Données compressées"},
    {2, p_compresse2, "buffer", "Données compressées"},
    {3, p_compresse3, "buffer", "Données compressées"},
    {0}
};
static const OwlSignature sig_decompresse[] = {
    {1, p_decompresse1, "buffer", "Données décompressées"},
    {2, p_decompresse2, "buffer", "Données décompressées"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "Compresse",
        .nameEN     = "Compress",
        .category   = "Compression",
        .descFR     = "Compresse des données (chaîne ou buffer) avec gzip ou deflate.",
        .descEN     = "Compress data (string or buffer) using gzip or deflate.",
        .signatures = sig_compresse,
        .impl       = fn_compresse,
    },
    {
        .canonical      = "D\xc3\xa9" "compresse", /* Décompresse */
        .canonicalAscii = "Decompresse",
        .nameEN         = "Uncompress",
        .category       = "Compression",
        .descFR         = "Décompresse un buffer gzip ou deflate (auto-détecté).",
        .descEN         = "Decompress a gzip or deflate buffer (auto-detected).",
        .signatures     = sig_decompresse,
        .impl           = fn_decompresse,
    },
};

/* ================================================================
 * TAR / TAR.GZ BACKEND (POSIX ustar, maison)
 *
 * Format ustar documenté depuis 1988 : blocs de 512 octets, chaque
 * entrée = 1 bloc header + N blocs data (zero-padded). Fin = 2 blocs
 * entièrement zéros. Pour TGZ on enveloppe la séquence dans un flux
 * gzip via miniz. Ce backend n'a pas de random access → nous chargeons
 * toute l'archive en mémoire à l'ouverture en lecture. Acceptable
 * pour la Phase 1 (archives < 500 MB). Streaming viendra en Phase 2.
 * ================================================================ */

typedef struct {
    char*         name;        /* malloc'd, NUL-terminated */
    uint64_t      offset;      /* dans le data buffer, pointe sur le début data */
    uint64_t      size;        /* taille décompressée */
    uint64_t      mtime;       /* epoch */
    int           mode;        /* octal */
    int           typeflag;    /* '0'/'\0'=file, '5'=dir, '2'=symlink, '7'=reg-contiguous */
} TarEntry;

typedef struct {
    /* Reader side */
    unsigned char* data;       /* tout le contenu TAR décompressé (read mode) */
    size_t         dataLen;
    TarEntry*      entries;    /* tableau d'entrées parsées */
    int            entryCount;

    /* Writer side */
    FILE*          out;        /* NULL en mode read */
    bool           isGzipped;  /* TGZ (ZIP_FMT_TARGZ) */
    /* Buffer TAR brut écrit en mémoire avant le close (TGZ seulement —
     * pour le gzippage final). */
    unsigned char* writeBuf;
    size_t         writeLen;
    size_t         writeCap;
} TarState;

static void tar_write_octal(char* dst, int len, uint64_t val) {
    /* POSIX ustar: octal string, NUL-terminated (or NUL+space).
     * We emit right-aligned octal then trailing NUL. */
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%0*llo", len - 1, (unsigned long long)val);
    if (n > len - 1) n = len - 1;
    memset(dst, '0', len - 1);
    memcpy(dst + (len - 1 - n), tmp, n);
    dst[len - 1] = '\0';
}

static uint64_t tar_read_octal(const char* src, int len) {
    uint64_t val = 0;
    for (int i = 0; i < len; i++) {
        char c = src[i];
        if (c == 0 || c == ' ') break;
        if (c < '0' || c > '7') return 0;
        val = (val << 3) | (c - '0');
    }
    return val;
}

static void tar_fill_checksum(unsigned char* header512) {
    /* Compute the 8-byte chksum field by summing all bytes (treating
     * the chksum field itself as 8 spaces for the initial computation),
     * then write it back as 6 octal digits + NUL + space. */
    uint32_t sum = 0;
    for (int i = 0; i < 148; i++) sum += header512[i];
    for (int i = 0; i < 8;   i++) sum += ' ';
    for (int i = 156; i < 512; i++) sum += header512[i];
    char buf[8];
    snprintf(buf, sizeof(buf), "%06o", sum);
    memcpy(header512 + 148, buf, 6);
    header512[148 + 6] = '\0';
    header512[148 + 7] = ' ';
}

/* Build a ustar header for a regular file entry. */
static void tar_build_header(unsigned char* hdr, const char* name,
                              uint64_t size, uint64_t mtime, int mode) {
    memset(hdr, 0, 512);
    size_t nl = strlen(name);
    if (nl > 99) nl = 99; /* ustar name field = 100 bytes, leave room for NUL */
    memcpy(hdr, name, nl);
    tar_write_octal((char*)hdr + 100, 8,  (uint64_t)(mode & 0777));
    tar_write_octal((char*)hdr + 108, 8,  0);          /* uid */
    tar_write_octal((char*)hdr + 116, 8,  0);          /* gid */
    tar_write_octal((char*)hdr + 124, 12, size);
    tar_write_octal((char*)hdr + 136, 12, mtime);
    hdr[156] = '0';                                     /* typeflag = regular file */
    memcpy(hdr + 257, "ustar", 5);                      /* magic */
    hdr[262] = '\0';
    memcpy(hdr + 263, "00", 2);                          /* version */
    tar_fill_checksum(hdr);
}

static void tar_buf_append(TarState* t, const void* data, size_t n) {
    if (t->writeLen + n > t->writeCap) {
        size_t nc = t->writeCap ? t->writeCap * 2 : 8192;
        while (nc < t->writeLen + n) nc *= 2;
        t->writeBuf = (unsigned char*)realloc(t->writeBuf, nc);
        t->writeCap = nc;
    }
    memcpy(t->writeBuf + t->writeLen, data, n);
    t->writeLen += n;
}

static void tar_write_block(TarState* t, const void* block512) {
    if (t->out)      fwrite(block512, 1, 512, t->out);
    else             tar_buf_append(t, block512, 512);
}

static int tar_add_entry(TarState* t, const char* name,
                          const unsigned char* data, size_t size) {
    unsigned char hdr[512];
    tar_build_header(hdr, name, (uint64_t)size, (uint64_t)time(NULL), 0644);
    tar_write_block(t, hdr);

    /* Data blocks, zero-padded to 512. */
    size_t remaining = size;
    const unsigned char* p = data;
    while (remaining >= 512) {
        tar_write_block(t, p);
        p += 512; remaining -= 512;
    }
    if (remaining > 0) {
        unsigned char pad[512] = {0};
        memcpy(pad, p, remaining);
        tar_write_block(t, pad);
    }
    return 0;
}

/* Écrit les deux blocs finaux de zéros + flush. */
static void tar_finalize(TarState* t) {
    unsigned char zero[512] = {0};
    tar_write_block(t, zero);
    tar_write_block(t, zero);
    if (t->out) { fflush(t->out); }
}

/* Parse the whole loaded buffer into entries[]. Returns count or -1. */
static int tar_parse_all(TarState* t) {
    t->entries    = NULL;
    t->entryCount = 0;
    int cap = 0;
    size_t i = 0;
    while (i + 512 <= t->dataLen) {
        unsigned char* hdr = t->data + i;
        /* Detect end-of-archive: first byte of name == 0 = empty block. */
        if (hdr[0] == 0) break;
        /* Validate ustar magic for safety (non-ustar tars sometimes slip
         * through; we still try to parse them). */
        uint64_t size = tar_read_octal((char*)hdr + 124, 12);
        i += 512;
        if (t->entryCount >= cap) {
            cap = cap ? cap * 2 : 16;
            t->entries = (TarEntry*)realloc(t->entries, cap * sizeof(TarEntry));
        }
        TarEntry* e = &t->entries[t->entryCount++];
        /* Name is NUL-terminated (or full 100 chars). */
        size_t nlen = 0;
        while (nlen < 100 && hdr[nlen]) nlen++;
        e->name = (char*)malloc(nlen + 1);
        memcpy(e->name, hdr, nlen);
        e->name[nlen] = '\0';
        e->offset = i;
        e->size   = size;
        e->mtime  = tar_read_octal((char*)hdr + 136, 12);
        e->mode   = (int)tar_read_octal((char*)hdr + 100, 8);
        e->typeflag = hdr[156];
        /* Skip data blocks (rounded up to 512). */
        size_t blocks = (size + 511) / 512;
        i += blocks * 512;
    }
    return t->entryCount;
}

/* 7z backend close — forwards to LZMA SDK via our wrapper. */
static void sevenz_backend_close(ObjZipArchive* z) {
    if (!z || !z->backend) return;
    owl_7z_close(z->backend);
}

static void tar_backend_close(ObjZipArchive* z) {
    if (!z || !z->backend) return;
    TarState* t = (TarState*)z->backend;
    if (t->out) {
        if (z->mode != ZIP_MODE_READ) tar_finalize(t);
        fclose(t->out);
        t->out = NULL;
    }
    /* TGZ finalize: gzip the in-memory TAR buffer and write to disk. */
    if (t->isGzipped && t->writeBuf && z->path) {
        size_t compressedLen = 0;
        mz_uint flags = tdefl_create_comp_flags_from_zip_params(z->compressionLevel, -15, 0);
        void* deflated = tdefl_compress_mem_to_heap(t->writeBuf, t->writeLen,
                                                      &compressedLen, flags);
        if (deflated) {
            FILE* f = fopen(z->path, "wb");
            if (f) {
                unsigned char gzhdr[10] = {0x1f,0x8b,0x08,0x00,0,0,0,0,0x00,0xff};
                fwrite(gzhdr, 1, 10, f);
                fwrite(deflated, 1, compressedLen, f);
                uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, t->writeBuf, t->writeLen);
                unsigned char foot[8];
                foot[0]=(crc)&0xff; foot[1]=(crc>>8)&0xff; foot[2]=(crc>>16)&0xff; foot[3]=(crc>>24)&0xff;
                uint32_t isize = (uint32_t)t->writeLen;
                foot[4]=(isize)&0xff; foot[5]=(isize>>8)&0xff; foot[6]=(isize>>16)&0xff; foot[7]=(isize>>24)&0xff;
                fwrite(foot, 1, 8, f);
                fclose(f);
            }
            free(deflated);
        }
    }
    for (int i = 0; i < t->entryCount; i++) free(t->entries[i].name);
    free(t->entries);
    free(t->data);
    free(t->writeBuf);
    free(t);
}

/* Open for writing: TAR directly streams to FILE*, TGZ buffers in mem. */
static int tar_open_writer(ObjZipArchive* z) {
    TarState* t = (TarState*)calloc(1, sizeof(TarState));
    if (!t) return -1;
    t->isGzipped = (z->format == ZIP_FMT_TARGZ);
    if (!t->isGzipped) {
        t->out = fopen(z->path, "wb");
        if (!t->out) { free(t); return -1; }
    }
    z->backend = t;
    z->open = true;
    return 0;
}

/* Open for reading: load whole file, decompress if TGZ, parse entries. */
static int tar_open_reader(ObjZipArchive* z) {
    TarState* t = (TarState*)calloc(1, sizeof(TarState));
    if (!t) return -1;
    t->isGzipped = (z->format == ZIP_FMT_TARGZ);

    FILE* f = fopen(z->path, "rb");
    if (!f) { free(t); return -1; }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* raw = (unsigned char*)malloc(sz);
    if (!raw || fread(raw, 1, sz, f) != sz) {
        free(raw); fclose(f); free(t); return -1;
    }
    fclose(f);

    if (t->isGzipped) {
        int hdr = gzip_header_size(raw, (int)sz);
        if (hdr < 0) { free(raw); free(t); return -1; }
        int deflLen = (int)sz - hdr - 8;
        if (deflLen < 0) { free(raw); free(t); return -1; }
        size_t outLen = 0;
        void* plain = tinfl_decompress_mem_to_heap(raw + hdr, (size_t)deflLen,
                                                     &outLen, 0);
        free(raw);
        if (!plain) { free(t); return -1; }
        t->data = (unsigned char*)plain;
        t->dataLen = outLen;
    } else {
        t->data = raw;
        t->dataLen = sz;
    }
    tar_parse_all(t);
    z->backend = t;
    z->open = true;
    return 0;
}

/* ================================================================
 * ZIP BACKEND (miniz mz_zip_* API)
 *
 * Couvre ZIP + WDZ (WDZ partage le moteur ZIP — sentinels distincts
 * mais le backend miniz traite les 2 identiquement).
 * Le backend est un `mz_zip_archive*` malloc'd, stocké dans
 * ObjZipArchive.backend. On passe par mz_zip_writer_* en mode write
 * ou mz_zip_reader_* en mode read. Switcher d'un mode à l'autre sans
 * fermer l'archive n'est pas autorisé (canon pareil).
 *
 * AES-256 : non implémenté en Phase 1 (miniz ne le fait pas, minizip-ng
 * ou mbedTLS-on-top sont des phases suivantes). zipMotDePasse lève
 * "format non chiffré" pour l'instant.
 * ================================================================ */

/* Forward decl of additional 7z reader backend functions (lives in src/compress_7z.c). */
void*       owl_7z_open_reader(const char* path);
int         owl_7z_num_files(void* state);
const char* owl_7z_filename(void* state, int index);
int         owl_7z_is_dir(void* state, int index);
int         owl_7z_find(void* state, const char* name);
int         owl_7z_extract(void* state, int index, unsigned char** out, size_t* out_len);
unsigned long long owl_7z_file_size(void* state, int index);

/* Forward decl of the AES ZIP writer (opaque handle lives in
 * src/compress_aes_zip.c). We can't include its struct because it's
 * defined privately to that file; we just treat it as void*. */
typedef struct AesZipWriter AesZipWriter;
AesZipWriter* aes_zip_writer_create(const char* path);
void aes_zip_writer_set_level(AesZipWriter* w, int level);
int  aes_zip_writer_add(AesZipWriter* w, const char* name,
                         const unsigned char* data, size_t data_len,
                         const char* password);
int  aes_zip_writer_finalize(AesZipWriter* w);
void aes_zip_writer_destroy(AesZipWriter* w);
int  aes_zip_extract_at_offset(const char* archive_path, unsigned local_offset,
                                const char* password,
                                unsigned char** out, size_t* out_len);

/* Discriminant on ObjZipArchive.backend: normally mz_zip_archive*, but
 * when encrypted writing is active we stash an AesZipWriter* instead
 * and set the is_aes_writer flag (abused via the lastError slot — no
 * existing use for it during an active writer session). We use a
 * dedicated bit: 0x80000000 marks "AES writer backend". */
#define AES_WRITER_MARKER 0x41455A57 /* "AEZW" */

static int is_aes_writer_backend(ObjZipArchive* z) {
    return z && z->lastError == AES_WRITER_MARKER;
}
static void mark_aes_writer(ObjZipArchive* z) { z->lastError = AES_WRITER_MARKER; }

static void zip_backend_close(ObjZipArchive* z) {
    if (!z || !z->backend) return;
    if (is_aes_writer_backend(z)) {
        AesZipWriter* w = (AesZipWriter*)z->backend;
        if (z->mode != ZIP_MODE_READ) {
            aes_zip_writer_finalize(w);
        }
        aes_zip_writer_destroy(w);
        z->lastError = 0;
        return;
    }
    mz_zip_archive* ar = (mz_zip_archive*)z->backend;
    if (z->mode == ZIP_MODE_READ) {
        mz_zip_reader_end(ar);
    } else {
        (void)mz_zip_writer_finalize_archive(ar);
        mz_zip_writer_end(ar);
    }
    free(ar);
}

/* Détecte le format à partir de l'extension du path. Fallback ZIP. */
static ZipArchiveFormat detect_format_from_path(const char* path) {
    if (!path) return ZIP_FMT_ZIP;
    size_t n = strlen(path);
    if (n >= 4 && strcasecmp(path + n - 4, ".zip") == 0) return ZIP_FMT_ZIP;
    if (n >= 4 && strcasecmp(path + n - 4, ".wdz") == 0) return ZIP_FMT_WDZ;
    if (n >= 4 && strcasecmp(path + n - 4, ".tar") == 0) return ZIP_FMT_TAR;
    if (n >= 4 && strcasecmp(path + n - 4, ".tgz") == 0) return ZIP_FMT_TARGZ;
    if (n >= 7 && strcasecmp(path + n - 7, ".tar.gz") == 0) return ZIP_FMT_TARGZ;
    if (n >= 3 && strcasecmp(path + n - 3, ".7z")  == 0) return ZIP_FMT_7Z;
    if (n >= 4 && strcasecmp(path + n - 4, ".cab") == 0) return ZIP_FMT_CAB;
    if (n >= 4 && strcasecmp(path + n - 4, ".rar") == 0) return ZIP_FMT_RAR;
    return ZIP_FMT_ZIP;
}

/* ================================================================
 * Callbacks de progression user-space
 *
 * Le canon passe une procédure optionnelle en dernier argument de
 * zipAjouteFichier/zipAjouteRépertoire/zipExtraitTout/etc. On appelle
 * cette procédure via vm_call_reentrant aux transitions d'entrée
 * (début ou fin de fichier). Signature canon :
 *
 *     PROCEDURE Prog(fichier: chaîne, pctGlobal: entier, pctFichier: entier)
 *     RENVOYER zipAnnuleOp  // -1 pour annuler l'opération
 *
 * Un retour < 0 (ou la sentinelle zipAnnuleOp = -1) demande l'annulation.
 * 0 ou toute autre valeur = continuer. Le rafraîchissement est par
 * fichier (pas par byte) en Phase 1 — suffisant pour 95% des UX.
 * ================================================================ */

/* Détecte si la valeur ressemble à une procédure/fonction callable. */
static bool is_callable_value(Value v) {
    if (IS_NIL(v)) return false;
    return IS_CALLABLE(v);
}

/* Invoque le callback de progression. Renvoie true si l'opération doit
 * être annulée (return < 0 du user). Si cb est NIL ou non callable,
 * no-op silencieux. Stack balance : on pousse exactement ce qu'on pop. */
static bool progress_call(Value cb, const char* fileName, int pctGlobal, int pctFile) {
    if (!is_callable_value(cb)) return false;

    vm_push(cb);
    vm_push(OBJ_VAL(obj_copy_string(fileName, (int)strlen(fileName))));
    vm_push(INTEGER_VAL(pctGlobal));
    vm_push(INTEGER_VAL(pctFile));

    /* vm_call_reentrant pousse le résultat sur la pile. Le callable est
     * consommé avec ses args par call_value → on pop juste le résultat. */
    InterpretResult r = vm_call_reentrant(cb, 3);
    if (r != INTERPRET_OK) {
        /* Erreur pendant le callback — on annule pour rester safe. */
        return true;
    }
    Value ret = vm_pop();
    if (IS_INTEGER(ret) && AS_INTEGER(ret) < 0) return true;
    return false;
}

/* Helper format-non-supporté pour les natives zip*. Le caller passe son
 * nom (utilisé pour le slot {native}) et l'archive concernée. Retourne
 * une Value selon le mapping figé — Faux par défaut pour les
 * ops booléennes, l'extracteur retourne Null lui-même. */
static void emit_format_unsupported(const char* native_name, ZipArchiveFormat fmt) {
    const char* name = "?";
    switch (fmt) {
        case ZIP_FMT_CAB: name = "CAB"; break;
        case ZIP_FMT_RAR: name = "RAR"; break;
        case ZIP_FMT_7Z:  name = "7z";  break;
        case ZIP_FMT_TAR: name = "TAR"; break;
        case ZIP_FMT_TARGZ: name = "TGZ"; break;
        default: break;
    }
    owl_emit_native_error(E_NATIVE_ZIP_UNSUPPORTED_FORMAT, native_name,
                          "native", native_name,
                          "format", name,
                          NULL);
}

/* ================================================================
 * zipCrée(archive, chemin [, format])
 * ================================================================ */

/* zipCrée(chemin [, format]) — retourne une zipArchive ouverte.
 * Usage canon OWL :
 *     arch est une zipArchive = zipCrée("test.zip")
 *
 * (Variant a été retiré d'OWL — le type cible doit être explicite.) */
static Value fn_zip_cree(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipCrée", "chemin", 0, IS_STRING(args[0]), "chaîne", NIL_VAL);
    const char* path = AS_CSTRING(args[0]);

    ZipArchiveFormat fmt = detect_format_from_path(path);
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        fmt = (ZipArchiveFormat)AS_INTEGER(args[1]);
    }
    /* Phase 1 : ZIP/WDZ + TAR/TGZ supportés, CAB/RAR/7z non. */
    if (fmt != ZIP_FMT_ZIP && fmt != ZIP_FMT_WDZ &&
        fmt != ZIP_FMT_TAR && fmt != ZIP_FMT_TARGZ) {
        emit_format_unsupported("zipCrée", fmt);
        return NIL_VAL;
    }

    ObjZipArchive* z = obj_new_zip_archive(fmt, path, ZIP_MODE_CREATE);
    if (fmt == ZIP_FMT_TAR || fmt == ZIP_FMT_TARGZ) {
        if (tar_open_writer(z) != 0) {
            owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipCrée",
                                  "native", "zipCrée",
                                  "path", path,
                                  "detail", "création impossible",
                                  NULL);
            /* GC reprendra z */
            return NIL_VAL;
        }
    } else {
        mz_zip_archive* ar = (mz_zip_archive*)calloc(1, sizeof(mz_zip_archive));
        if (!ar || !mz_zip_writer_init_file(ar, path, 0)) {
            free(ar);
            owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipCrée",
                                  "native", "zipCrée",
                                  "path", path,
                                  "detail", "création impossible",
                                  NULL);
            return NIL_VAL;
        }
        z->backend = ar;
        z->open    = true;
    }
    owl_clear_error();
    return OBJ_VAL(z);
}

/* ================================================================
 * zipOuvre(archive, chemin [, mode])
 * ================================================================ */

static Value fn_zip_ouvre(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipOuvre", "chemin", 0, IS_STRING(args[0]), "chaîne", NIL_VAL);
    const char* path = AS_CSTRING(args[0]);
    int mode = ZIP_MODE_READ;
    if (argCount >= 2 && IS_INTEGER(args[1])) mode = (int)AS_INTEGER(args[1]);

    ZipArchiveFormat fmt = detect_format_from_path(path);
    if (fmt != ZIP_FMT_ZIP && fmt != ZIP_FMT_WDZ &&
        fmt != ZIP_FMT_TAR && fmt != ZIP_FMT_TARGZ) {
        emit_format_unsupported("zipOuvre", fmt);
        return NIL_VAL;
    }

    ZipArchiveMode omode = (mode & ZIP_MODE_WRITE) ? ZIP_MODE_WRITE : ZIP_MODE_READ;
    ObjZipArchive* z = obj_new_zip_archive(fmt, path, omode);

    if (fmt == ZIP_FMT_TAR || fmt == ZIP_FMT_TARGZ) {
        int rc = (omode == ZIP_MODE_READ) ? tar_open_reader(z) : tar_open_writer(z);
        if (rc != 0) {
            owl_emit_native_error(E_NATIVE_ZIP_FILE_NOT_FOUND, "zipOuvre",
                                  "native", "zipOuvre",
                                  "path", path,
                                  NULL);
            return NIL_VAL;
        }
        if (omode == ZIP_MODE_READ) {
            z->fileCount = ((TarState*)z->backend)->entryCount;
        }
    } else {
        mz_zip_archive* ar = (mz_zip_archive*)calloc(1, sizeof(mz_zip_archive));
        if (omode == ZIP_MODE_READ) {
            if (!ar || !mz_zip_reader_init_file(ar, path, 0)) {
                free(ar);
                owl_emit_native_error(E_NATIVE_ZIP_FILE_NOT_FOUND, "zipOuvre",
                                      "native", "zipOuvre",
                                      "path", path,
                                      NULL);
                return NIL_VAL;
            }
        } else {
            if (!ar || !mz_zip_writer_init_file(ar, path, 0)) {
                free(ar);
                owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipOuvre",
                                      "native", "zipOuvre",
                                      "path", path,
                                      "detail", "écriture impossible",
                                      NULL);
                return NIL_VAL;
            }
        }
        z->backend = ar;
        z->open    = true;
        if (omode == ZIP_MODE_READ) {
            z->fileCount = (int)mz_zip_reader_get_num_files(ar);
        }
    }
    owl_clear_error();
    return OBJ_VAL(z);
}

/* ================================================================
 * zipFerme(archive)
 * ================================================================ */

static Value fn_zip_ferme(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipFerme", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", BOOL_VAL(false));
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    obj_zip_archive_close(z);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Ajout d'une entrée depuis un buffer en mémoire.
 * Coeur partagé par la SYNTAXE 3 canon de zipAjouteFichier :
 *     zipAjouteFichier(<Archive>, <Buffer>, zipEnMémoire, <Chemin>)
 * Gère ZIP/WDZ (avec bascule writer AES si mot de passe) + TAR/TGZ.
 * Le caller a déjà validé l'archive ouverte en écriture.
 * ================================================================ */

static Value zip_add_buffer(ObjZipArchive* z, const char* name,
                            const unsigned char* data, int dataLen) {
    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        TarState* t = (TarState*)z->backend;
        tar_add_entry(t, name, data, (size_t)dataLen);
        z->fileCount++;
        owl_clear_error();
        return BOOL_VAL(true);
    }

    if (z->format != ZIP_FMT_ZIP && z->format != ZIP_FMT_WDZ) {
        emit_format_unsupported("zipAjouteFichier", z->format);
        return BOOL_VAL(false);
    }

    /* Password defined + backend still miniz + no entries written yet →
     * switch to the AES writer. This covers the canonical flow:
     *     a = zipCrée(path) ; zipMotDePasse(a, pwd) ; zipAjouteFichier(...)
     * If entries already exist, refuse — mixing encrypted + plain entries
     * isn't supported in Phase 2b. */
    if (z->password && !is_aes_writer_backend(z)) {
        if (z->fileCount > 0) {
            owl_emit_native_error(E_NATIVE_ZIP_WRITE_FAILURE, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "detail", "zipMotDePasse doit être défini avant le premier ajout",
                                  NULL);
            return BOOL_VAL(false);
        }
        /* Tear down the miniz writer and replace it with AES writer over
         * the same path. mz_zip_writer_init_file already opened the file
         * but hasn't written a central directory yet, so recreate cleanly. */
        mz_zip_archive* old = (mz_zip_archive*)z->backend;
        mz_zip_writer_end(old);
        free(old);
        z->backend = NULL;

        AesZipWriter* w = aes_zip_writer_create(z->path);
        if (!w) {
            owl_emit_native_error(E_NATIVE_ZIP_WRITE_FAILURE, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "detail", "échec bascule writer AES",
                                  NULL);
            return BOOL_VAL(false);
        }
        aes_zip_writer_set_level(w, z->compressionLevel);
        z->backend = w;
        mark_aes_writer(z);
    }

    if (is_aes_writer_backend(z)) {
        AesZipWriter* w = (AesZipWriter*)z->backend;
        if (aes_zip_writer_add(w, name, data, (size_t)dataLen, z->password) != 0) {
            char detail[256];
            snprintf(detail, sizeof(detail), "échec chiffrement '%s'", name);
            owl_emit_native_error(E_NATIVE_ZIP_WRITE_FAILURE, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "detail", detail,
                                  NULL);
            return BOOL_VAL(false);
        }
        z->fileCount++;
        owl_clear_error();
        return BOOL_VAL(true);
    }

    mz_zip_archive* ar = (mz_zip_archive*)z->backend;
    mz_uint flags = (mz_uint)(z->compressionLevel & 0x0F);
    if (!mz_zip_writer_add_mem(ar, name, data, (size_t)dataLen, flags)) {
        char detail[256];
        snprintf(detail, sizeof(detail), "échec ajout '%s'", name);
        owl_emit_native_error(E_NATIVE_ZIP_WRITE_FAILURE, "zipAjouteFichier",
                              "native", "zipAjouteFichier",
                              "detail", detail,
                              NULL);
        return BOOL_VAL(false);
    }
    z->fileCount++;
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * zipAjouteFichier — 2 familles de syntaxes canon :
 *   - disque : zipAjouteFichier(<Archive>, <Chemin> [, <PortionChemin>])
 *   - buffer : zipAjouteFichier(<Archive>, <Buffer>, zipEnMémoire, <Chemin>)
 *              (SYNTAXE 3 : format WDZ/ZIP, contenu déjà en mémoire)
 * Dispatch sur la présence de zipEnMémoire en 3e argument.
 * ================================================================ */

static Value fn_zip_ajoute_fichier(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipAjouteFichier", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", BOOL_VAL(false));
    OWL_REQUIRE_ARG_PRESENT("zipAjouteFichier", "cheminFichier", 1, BOOL_VAL(false));
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode == ZIP_MODE_READ) {
        owl_emit_native_error(E_NATIVE_ZIP_ARCHIVE_NOT_OPEN_WRITE, "zipAjouteFichier",
                              "native", "zipAjouteFichier",
                              NULL);
        return BOOL_VAL(false);
    }

    /* SYNTAXE 3 : ajout depuis un buffer.
     *   args[1] = contenu (buffer/chaîne), args[2] = zipEnMémoire,
     *   args[3] = chemin à conserver dans l'archive. */
    if (argCount >= 4 && IS_INTEGER(args[2])
        && AS_INTEGER(args[2]) == ZIP_EN_MEMOIRE_SENTINEL) {
        if (!IS_STRING(args[3])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "arg", "cheminAConserver",
                                  "expected", "chaîne",
                                  "actual", "autre type",
                                  NULL);
            return BOOL_VAL(false);
        }
        const unsigned char* data = NULL;
        int dataLen = 0;
        if (!value_as_bytes(args[1], &data, &dataLen)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "arg", "fichierAAjouter",
                                  "expected", "buffer ou chaîne",
                                  "actual", "autre type",
                                  NULL);
            return BOOL_VAL(false);
        }
        return zip_add_buffer(z, AS_CSTRING(args[3]), data, dataLen);
    }

    /* Syntaxes disque : args[1] doit être le chemin du fichier. */
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "zipAjouteFichier",
                              "native", "zipAjouteFichier",
                              "arg", "fichierAAjouter",
                              "expected", "chemin (chaîne) ou buffer + zipEnMémoire",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* filePath = AS_CSTRING(args[1]);
    int portion = 1;
    if (argCount >= 3 && IS_INTEGER(args[2])) portion = (int)AS_INTEGER(args[2]);

    const char* stored = filePath;
    if (portion == 0) {
        const char* bn = strrchr(filePath, '/');
        const char* bn2 = strrchr(filePath, '\\');
        if (bn2 && (!bn || bn2 > bn)) bn = bn2;
        if (bn) stored = bn + 1;
    }

    /* Normalise le nom stocke pour la compat zip canonique :
     *  - les zip standards interdisent les paths absolus (leading '/' ou C:)
     *  - le separateur d'archive est '/' (jamais '\\')
     * Sans ca, `mz_zip_writer_add_file` echoue silencieusement sous Linux
     * pour un stored qui commence par '/' (cf zipAjouteFichier(..., zipDisque)
     * sur un fRepTemp() POSIX). */
    char storedNorm[2048];
    {
        const char* src = stored;
        /* Strip leading drive letter "C:" */
        if (src[0] && src[1] == ':') src += 2;
        /* Strip all leading separators */
        while (*src == '/' || *src == '\\') src++;
        size_t k = 0;
        while (*src && k + 1 < sizeof(storedNorm)) {
            storedNorm[k++] = (*src == '\\') ? '/' : *src;
            src++;
        }
        storedNorm[k] = '\0';
        stored = storedNorm;
    }

    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        FILE* f = fopen(filePath, "rb");
        if (!f) {
            owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "path", filePath,
                                  "detail", "échec lecture",
                                  NULL);
            return BOOL_VAL(false);
        }
        fseek(f, 0, SEEK_END);
        size_t sz = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* data = (unsigned char*)malloc(sz);
        if (!data || fread(data, 1, sz, f) != sz) {
            free(data); fclose(f);
            owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipAjouteFichier",
                                  "native", "zipAjouteFichier",
                                  "path", filePath,
                                  "detail", "erreur de lecture",
                                  NULL);
            return BOOL_VAL(false);
        }
        fclose(f);
        TarState* t = (TarState*)z->backend;
        tar_add_entry(t, stored, data, sz);
        free(data);
        z->fileCount++;
        owl_clear_error();
        return BOOL_VAL(true);
    }

    if (z->format != ZIP_FMT_ZIP && z->format != ZIP_FMT_WDZ) {
        emit_format_unsupported("zipAjouteFichier", z->format);
        return BOOL_VAL(false);
    }
    mz_zip_archive* ar = (mz_zip_archive*)z->backend;
    mz_uint flags = (mz_uint)(z->compressionLevel & 0x0F);
    if (!mz_zip_writer_add_file(ar, stored, filePath, NULL, 0, flags)) {
        owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipAjouteFichier",
                              "native", "zipAjouteFichier",
                              "path", filePath,
                              "detail", "échec lecture",
                              NULL);
        return BOOL_VAL(false);
    }
    z->fileCount++;
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * zipNbFichier(archive)
 * ================================================================ */

static Value fn_zip_nb_fichier(int argCount, Value* args) {
    if (argCount < 1 || !IS_ZIP_ARCHIVE(args[0])) return INTEGER_VAL(0);
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open) return INTEGER_VAL(0);
    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        return INTEGER_VAL(z->fileCount);
    }
    if (z->mode == ZIP_MODE_READ) {
        mz_zip_archive* ar = (mz_zip_archive*)z->backend;
        return INTEGER_VAL((int64_t)mz_zip_reader_get_num_files(ar));
    }
    return INTEGER_VAL(z->fileCount);
}

/* ================================================================
 * zipListeFichier(archive) -> chaîne LF-séparée
 * ================================================================ */

static Value fn_zip_liste_fichier(int argCount, Value* args) {
    if (argCount < 1 || !IS_ZIP_ARCHIVE(args[0])) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode != ZIP_MODE_READ) {
        owl_emit_native_error(E_NATIVE_ZIP_ARCHIVE_NOT_OPEN_READ, "zipListeFichier",
                              "native", "zipListeFichier",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    size_t cap = 1024, len = 0;
    char* buf = (char*)malloc(cap);

    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        TarState* t = (TarState*)z->backend;
        for (int i = 0; i < t->entryCount; i++) {
            size_t nl = strlen(t->entries[i].name);
            if (len + nl + 2 > cap) { while (len + nl + 2 > cap) cap *= 2; buf = realloc(buf, cap); }
            if (len > 0) buf[len++] = '\n';
            memcpy(buf + len, t->entries[i].name, nl);
            len += nl;
        }
    } else {
        mz_zip_archive* ar = (mz_zip_archive*)z->backend;
        mz_uint n = mz_zip_reader_get_num_files(ar);
        for (mz_uint i = 0; i < n; i++) {
            char name[512];
            if (mz_zip_reader_get_filename(ar, i, name, sizeof(name)) == 0) continue;
            size_t nl = strlen(name);
            if (len + nl + 2 > cap) { while (len + nl + 2 > cap) cap *= 2; buf = realloc(buf, cap); }
            if (len > 0) buf[len++] = '\n';
            memcpy(buf + len, name, nl);
            len += nl;
        }
    }

    ObjString* out = obj_copy_string(buf, (int)len);
    free(buf);
    owl_clear_error();
    return OBJ_VAL(out);
}

/* ================================================================
 * zipExtraitFichier(archive, indexOuNom [, destination])
 *   destination : chemin disque (chaîne) OU constante zipEnMémoire (→ buffer)
 *   (sentinelle ZIP_EN_MEMOIRE_SENTINEL definie en haut du fichier)
 * ================================================================ */

static Value fn_zip_extrait_fichier(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipExtraitFichier", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", NIL_VAL);
    OWL_REQUIRE_ARG_PRESENT("zipExtraitFichier", "indexOuNom", 1, NIL_VAL);
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode != ZIP_MODE_READ) {
        owl_emit_native_error(E_NATIVE_ZIP_ARCHIVE_NOT_OPEN_READ, "zipExtraitFichier",
                              "native", "zipExtraitFichier",
                              NULL);
        return NIL_VAL;
    }

    /* Destination : en mémoire (sentinelle) OU chemin disque. */
    bool toMem = true;
    const char* destPath = NULL;
    if (argCount >= 3) {
        if (IS_INTEGER(args[2]) && AS_INTEGER(args[2]) == ZIP_EN_MEMOIRE_SENTINEL) {
            toMem = true;
        } else if (IS_STRING(args[2])) {
            toMem = false;
            destPath = AS_CSTRING(args[2]);
        } else {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "zipExtraitFichier",
                                  "native", "zipExtraitFichier",
                                  "arg", "destination",
                                  "expected", "chaîne ou zipEnMémoire",
                                  "actual", "autre type",
                                  NULL);
            return NIL_VAL;
        }
    }

    /* TAR/TGZ backend — lookup dans TarState.entries. */
    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        TarState* t = (TarState*)z->backend;
        int idx = -1;
        if (IS_INTEGER(args[1])) {
            int64_t i1 = AS_INTEGER(args[1]);
            if (i1 < 1 || i1 > t->entryCount) {
                char idxStr[32], maxStr[32];
                snprintf(idxStr, sizeof(idxStr), "%lld", (long long)i1);
                snprintf(maxStr, sizeof(maxStr), "%d", t->entryCount);
                owl_emit_native_error(E_NATIVE_ZIP_INDEX_OUT_OF_BOUNDS, "zipExtraitFichier",
                                      "native", "zipExtraitFichier",
                                      "index", idxStr,
                                      "max", maxStr,
                                      NULL);
                return NIL_VAL;
            }
            idx = (int)i1 - 1;
        } else if (IS_STRING(args[1])) {
            const char* name = AS_CSTRING(args[1]);
            for (int i = 0; i < t->entryCount; i++) {
                if (strcmp(t->entries[i].name, name) == 0) { idx = i; break; }
            }
            if (idx < 0) {
                owl_emit_native_error(E_NATIVE_ZIP_ENTRY_NOT_FOUND, "zipExtraitFichier",
                                      "native", "zipExtraitFichier",
                                      "name", name,
                                      NULL);
                return NIL_VAL;
            }
        }
        TarEntry* e = &t->entries[idx];
        if (toMem) {
            ObjBuffer* b = obj_new_buffer(t->data + e->offset, (int)e->size);
            owl_clear_error();
            return OBJ_VAL(b);
        } else {
            FILE* f = fopen(destPath, "wb");
            if (!f) {
                owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipExtraitFichier",
                                      "native", "zipExtraitFichier",
                                      "path", destPath,
                                      "detail", "écriture impossible",
                                      NULL);
                return NIL_VAL;
            }
            fwrite(t->data + e->offset, 1, (size_t)e->size, f);
            fclose(f);
            owl_clear_error();
            return BOOL_VAL(true);
        }
    }

    /* ZIP/WDZ backend — miniz reader. */
    mz_zip_archive* ar = (mz_zip_archive*)z->backend;
    int idx = -1;
    if (IS_INTEGER(args[1])) {
        int64_t i1 = AS_INTEGER(args[1]);
        if (i1 < 1) {
            char idxStr[32], maxStr[32];
            snprintf(idxStr, sizeof(idxStr), "%lld", (long long)i1);
            snprintf(maxStr, sizeof(maxStr), "%u", mz_zip_reader_get_num_files(ar));
            owl_emit_native_error(E_NATIVE_ZIP_INDEX_OUT_OF_BOUNDS, "zipExtraitFichier",
                                  "native", "zipExtraitFichier",
                                  "index", idxStr,
                                  "max", maxStr,
                                  NULL);
            return NIL_VAL;
        }
        idx = (int)i1 - 1;
    } else if (IS_STRING(args[1])) {
        idx = mz_zip_reader_locate_file(ar, AS_CSTRING(args[1]), NULL, 0);
        if (idx < 0) {
            owl_emit_native_error(E_NATIVE_ZIP_ENTRY_NOT_FOUND, "zipExtraitFichier",
                                  "native", "zipExtraitFichier",
                                  "name", AS_CSTRING(args[1]),
                                  NULL);
            return NIL_VAL;
        }
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "zipExtraitFichier",
                              "native", "zipExtraitFichier",
                              "arg", "indexOuNom",
                              "expected", "entier ou chaîne",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    if ((mz_uint)idx >= mz_zip_reader_get_num_files(ar)) {
        char idxStr[32], maxStr[32];
        snprintf(idxStr, sizeof(idxStr), "%d", idx + 1);
        snprintf(maxStr, sizeof(maxStr), "%u", mz_zip_reader_get_num_files(ar));
        owl_emit_native_error(E_NATIVE_ZIP_INDEX_OUT_OF_BOUNDS, "zipExtraitFichier",
                              "native", "zipExtraitFichier",
                              "index", idxStr,
                              "max", maxStr,
                              NULL);
        return NIL_VAL;
    }

    /* AES-2 entry detection: method 99 + extra field 0x9901. miniz
     * parses the central dir for us but refuses to decompress; we take
     * over with our own AE-2 reader. */
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(ar, (mz_uint)idx, &st) &&
        st.m_method == 99) {
        if (!z->password) {
            owl_emit_native_error(E_NATIVE_ZIP_ENCRYPTION_REQUIRED, "zipExtraitFichier",
                                  "native", "zipExtraitFichier",
                                  NULL);
            return NIL_VAL;
        }
        unsigned char* pt = NULL;
        size_t pt_len = 0;
        int rc = aes_zip_extract_at_offset(z->path, (unsigned)st.m_local_header_ofs,
                                             z->password, &pt, &pt_len);
        if (rc != 0) {
            if (rc == -1) {
                owl_emit_native_error(E_NATIVE_ZIP_INVALID_PASSWORD, "zipExtraitFichier",
                                      "native", "zipExtraitFichier",
                                      NULL);
            } else {
                owl_emit_native_error(E_NATIVE_ZIP_CORRUPT_ARCHIVE, "zipExtraitFichier",
                                      "native", "zipExtraitFichier",
                                      "detail", "HMAC/corruption sur entrée chiffrée",
                                      NULL);
            }
            return NIL_VAL;
        }
        if (toMem) {
            ObjBuffer* b = obj_new_buffer(pt, (int)pt_len);
            free(pt);
            owl_clear_error();
            return OBJ_VAL(b);
        } else {
            FILE* f = fopen(destPath, "wb");
            if (!f) {
                free(pt);
                owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipExtraitFichier",
                                      "native", "zipExtraitFichier",
                                      "path", destPath,
                                      "detail", "écriture impossible",
                                      NULL);
                return NIL_VAL;
            }
            fwrite(pt, 1, pt_len, f);
            fclose(f);
            free(pt);
            owl_clear_error();
            return BOOL_VAL(true);
        }
    }

    if (toMem) {
        size_t outLen = 0;
        void* data = mz_zip_reader_extract_to_heap(ar, (mz_uint)idx, &outLen, 0);
        if (!data) {
            owl_emit_native_error(E_NATIVE_ZIP_CORRUPT_ARCHIVE, "zipExtraitFichier",
                                  "native", "zipExtraitFichier",
                                  "detail", "échec extraction (corrompu ?)",
                                  NULL);
            return NIL_VAL;
        }
        ObjBuffer* b = obj_new_buffer(data, (int)outLen);
        free(data);
        owl_clear_error();
        return OBJ_VAL(b);
    } else {
        if (!mz_zip_reader_extract_to_file(ar, (mz_uint)idx, destPath, 0)) {
            owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipExtraitFichier",
                                  "native", "zipExtraitFichier",
                                  "path", destPath,
                                  "detail", "écriture impossible",
                                  NULL);
            return NIL_VAL;
        }
        owl_clear_error();
        return BOOL_VAL(true);
    }
}

/* ================================================================
 * zipChercheFichier(archive, nom) -> index 1-based ou 0 si absent
 * ================================================================ */

static Value fn_zip_cherche_fichier(int argCount, Value* args) {
    if (argCount < 2 || !IS_ZIP_ARCHIVE(args[0]) || !IS_STRING(args[1])) {
        return INTEGER_VAL(0);
    }
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode != ZIP_MODE_READ) return INTEGER_VAL(0);
    const char* name = AS_CSTRING(args[1]);

    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        TarState* t = (TarState*)z->backend;
        for (int i = 0; i < t->entryCount; i++) {
            if (strcmp(t->entries[i].name, name) == 0) return INTEGER_VAL(i + 1);
        }
        return INTEGER_VAL(0);
    }

    mz_zip_archive* ar = (mz_zip_archive*)z->backend;
    int idx = mz_zip_reader_locate_file(ar, name, NULL, 0);
    return INTEGER_VAL(idx < 0 ? 0 : (int64_t)(idx + 1));
}

/* ================================================================
 * zipNiveauCompression(archive, niveau)
 * ================================================================ */

static Value fn_zip_niveau_compression(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipNiveauCompression", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", BOOL_VAL(false));
    OWL_REQUIRE_ARG("zipNiveauCompression", "niveau", 1, IS_INTEGER(args[1]),
                    "entier", BOOL_VAL(false));
    int n = (int)AS_INTEGER(args[1]);
    if (n < 0 || n > 9) {
        char nStr[32];
        snprintf(nStr, sizeof(nStr), "%d", n);
        owl_emit_native_error(E_NATIVE_COMPRESSION_INVALID_LEVEL, "zipNiveauCompression",
                              "native", "zipNiveauCompression",
                              "value", nStr,
                              NULL);
        return BOOL_VAL(false);
    }
    AS_ZIP_ARCHIVE(args[0])->compressionLevel = n;
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * zipMotDePasse(archive, motDePasse) -> code
 *
 * Stocke le mot de passe sur l'ObjZipArchive. Les primitives WinZip
 * AES-2 sont dans src/compress_ae2.c ; l'intégration complète dans le
 * writer/reader ZIP arrive en Phase 2b (nécessite un writer ZIP
 * custom qui émet method=99 + extra field 0x9901). Pour l'instant on
 * accepte la définition du password (utile pour préparer une archive)
 * mais l'ajout d'entrées chiffrées lève 23. Décrypte des archives
 * AES-2 existantes déjà supporté via owl_ae2_decrypt_buffer (native
 * interne __AE2Decrypt).
 * ================================================================ */

static Value fn_zip_mot_de_passe(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipMotDePasse", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", BOOL_VAL(false));
    OWL_REQUIRE_ARG_PRESENT("zipMotDePasse", "motDePasse", 1, BOOL_VAL(false));
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (z->password) {
        memset(z->password, 0, strlen(z->password));
        free(z->password);
        z->password = NULL;
    }
    if (IS_STRING(args[1])) {
        const char* pw = AS_CSTRING(args[1]);
        size_t n = strlen(pw);
        if (n > 0) {
            z->password = (char*)malloc(n + 1);
            if (z->password) { memcpy(z->password, pw, n + 1); }
        }
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Natives internes pour exposer les primitives AE-2 aux tests et
 * aux utilisateurs avancés. En attendant l'intégration complète dans
 * le writer ZIP, ça permet de chiffrer/déchiffrer des buffers au
 * format WinZip AE-2 (salt|verifier|ct|hmac). Préfixe __ = API
 * non-stable, pourrait disparaître quand zipMotDePasse est full-wired.
 * ================================================================ */

extern int owl_ae2_encrypt(const unsigned char* pt, size_t pt_len,
                            const char* password, size_t pw_len,
                            unsigned char** out, size_t* out_len);
extern int owl_ae2_decrypt(const unsigned char* in, size_t in_len,
                            const char* password, size_t pw_len,
                            unsigned char** out_pt, size_t* out_len);

static Value fn_ae2_encrypt(int argCount, Value* args) {
    OWL_REQUIRE_ARG_PRESENT("__AE2Encrypt", "données", 0, NIL_VAL);
    OWL_REQUIRE_ARG("__AE2Encrypt", "motDePasse", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    const unsigned char* pt = NULL;
    int pt_len = 0;
    if (!value_as_bytes(args[0], &pt, &pt_len)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "__AE2Encrypt",
                              "native", "__AE2Encrypt",
                              "arg", "données",
                              "expected", "chaîne ou buffer",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    const char* pw = AS_CSTRING(args[1]);
    unsigned char* out = NULL;
    size_t out_len = 0;
    if (owl_ae2_encrypt(pt, (size_t)pt_len, pw, strlen(pw), &out, &out_len) < 0) {
        owl_emit_native_error(E_NATIVE_ZIP_CORRUPT_ARCHIVE, "__AE2Encrypt",
                              "native", "__AE2Encrypt",
                              "detail", "échec de chiffrement",
                              NULL);
        return NIL_VAL;
    }
    ObjBuffer* b = obj_new_buffer(out, (int)out_len);
    free(out);
    owl_clear_error();
    return OBJ_VAL(b);
}

static Value fn_ae2_decrypt(int argCount, Value* args) {
    OWL_REQUIRE_ARG_PRESENT("__AE2Decrypt", "données", 0, NIL_VAL);
    OWL_REQUIRE_ARG("__AE2Decrypt", "motDePasse", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    const unsigned char* ct = NULL;
    int ct_len = 0;
    if (!value_as_bytes(args[0], &ct, &ct_len)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "__AE2Decrypt",
                              "native", "__AE2Decrypt",
                              "arg", "données",
                              "expected", "buffer",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    const char* pw = AS_CSTRING(args[1]);
    unsigned char* pt = NULL;
    size_t pt_len = 0;
    int rc = owl_ae2_decrypt(ct, (size_t)ct_len, pw, strlen(pw), &pt, &pt_len);
    if (rc != 0) {
        if (rc == -1) {
            owl_emit_native_error(E_NATIVE_ZIP_INVALID_PASSWORD, "__AE2Decrypt",
                                  "native", "__AE2Decrypt",
                                  NULL);
        } else if (rc == -2) {
            owl_emit_native_error(E_NATIVE_ZIP_CORRUPT_ARCHIVE, "__AE2Decrypt",
                                  "native", "__AE2Decrypt",
                                  "detail", "HMAC invalide (données corrompues ou altérées)",
                                  NULL);
        } else {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "__AE2Decrypt",
                                  "native", "__AE2Decrypt",
                                  NULL);
        }
        return NIL_VAL;
    }
    ObjBuffer* b = obj_new_buffer(pt, (int)pt_len);
    free(pt);
    owl_clear_error();
    return OBJ_VAL(b);
}

/* ================================================================
 * zipExtraitTout(archive [, destination]) -> entier (code d'erreur)
 *   destination : répertoire disque. Par défaut = répertoire courant.
 * ================================================================ */

static Value fn_zip_extrait_tout(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipExtraitTout", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", BOOL_VAL(false));
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode != ZIP_MODE_READ) {
        owl_emit_native_error(E_NATIVE_ZIP_ARCHIVE_NOT_OPEN_READ, "zipExtraitTout",
                              "native", "zipExtraitTout",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* destDir = ".";
    if (argCount >= 2 && IS_STRING(args[1])) destDir = AS_CSTRING(args[1]);

    /* Callback optionnel : dernier argument non-string/entier interprété
     * comme procédure. Canon = position fixe, ici on accepte
     * l'un des deux derniers args comme callable. */
    Value progressCb = NIL_VAL;
    for (int i = argCount - 1; i >= 1; i--) {
        if (is_callable_value(args[i])) { progressCb = args[i]; break; }
    }

    /* Crée récursivement les composants de destDir si absent. `/c/tmp/...`
     * ou `C:\...` demandé par l'user peut avoir plusieurs niveaux
     * manquants — on marche le path et mkdir à chaque séparateur. */
    {
        char d[1024];
        snprintf(d, sizeof(d), "%s", destDir);
        for (char* p = d + 1; *p; p++) {
            if (*p == '/' || *p == '\\') {
                char sep = *p; *p = '\0';
                OWL_MKDIR(d);
                *p = sep;
            }
        }
        OWL_MKDIR(d); /* niveau final */
    }

    char path[1024];
    int rc = 0;

    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        TarState* t = (TarState*)z->backend;
        int total = t->entryCount;
        for (int i = 0; i < total && rc == 0; i++) {
            TarEntry* e = &t->entries[i];
            int pct = total > 0 ? (int)((int64_t)(i + 1) * 100 / total) : 100;
            if (progress_call(progressCb, e->name, pct, 0)) { rc = -1; break; }
            snprintf(path, sizeof(path), "%s/%s", destDir, e->name);
            for (char* p = path + strlen(destDir) + 1; *p; p++) {
                if (*p == '/' || *p == '\\') {
                    char sep = *p; *p = '\0';
                    OWL_MKDIR(path);
                    *p = sep;
                }
            }
            if (e->typeflag == '5') continue;
            FILE* f = fopen(path, "wb");
            if (!f) { rc = 2; break; }
            fwrite(t->data + e->offset, 1, (size_t)e->size, f);
            fclose(f);
            if (progress_call(progressCb, e->name, pct, 100)) { rc = -1; break; }
        }
    } else {
        mz_zip_archive* ar = (mz_zip_archive*)z->backend;
        mz_uint n = mz_zip_reader_get_num_files(ar);
        for (mz_uint i = 0; i < n && rc == 0; i++) {
            char name[512];
            if (mz_zip_reader_get_filename(ar, i, name, sizeof(name)) == 0) continue;
            int pct = n > 0 ? (int)((int64_t)(i + 1) * 100 / n) : 100;
            if (progress_call(progressCb, name, pct, 0)) { rc = -1; break; }
            snprintf(path, sizeof(path), "%s/%s", destDir, name);
            for (char* p = path + strlen(destDir) + 1; *p; p++) {
                if (*p == '/' || *p == '\\') {
                    char sep = *p; *p = '\0';
                    OWL_MKDIR(path);
                    *p = sep;
                }
            }
            size_t nl = strlen(name);
            if (nl > 0 && (name[nl-1] == '/' || name[nl-1] == '\\')) continue;
            if (!mz_zip_reader_extract_to_file(ar, i, path, 0)) {
                rc = 2;
                break;
            }
            if (progress_call(progressCb, name, pct, 100)) { rc = -1; break; }
        }
    }

    if (rc == -1) {
        /* Annulé par l'utilisateur via le callback. ErreurInfo() mentionne
         * l'annulation ; le retour Faux signale un échec partiel (mapping
         * figé ). */
        owl_emit_native_error(E_NATIVE_ZIP_OPERATION_CANCELED, "zipExtraitTout",
                              "native", "zipExtraitTout",
                              NULL);
        return BOOL_VAL(false);
    }
    if (rc != 0) {
        char detail[256];
        snprintf(detail, sizeof(detail), "échec extraction vers '%s'", destDir);
        owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipExtraitTout",
                              "native", "zipExtraitTout",
                              "path", destDir,
                              "detail", detail,
                              NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * zipInfoFichier(archive, indexOuNom, élément) -> selon élément
 *   élément : zipNom, zipTaille, zipTailleCompressée, zipDate, zipCRC
 * ================================================================ */

static Value zip_info_fichier_impl(int argCount, Value* args, const char* nativeName) {
    OWL_REQUIRE_ARG(nativeName, "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", NIL_VAL);
    OWL_REQUIRE_ARG_PRESENT(nativeName, "indexOuNom", 1, NIL_VAL);
    OWL_REQUIRE_ARG(nativeName, "élément", 2, IS_INTEGER(args[2]),
                    "entier", NIL_VAL);
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode != ZIP_MODE_READ) {
        owl_emit_native_error(E_NATIVE_ZIP_ARCHIVE_NOT_OPEN_READ, nativeName,
                              NULL);
        return NIL_VAL;
    }
    int element = (int)AS_INTEGER(args[2]);

    /* Résoudre l'index (1-based user → 0-based interne). */
    int idx = -1;
    if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
        TarState* t = (TarState*)z->backend;
        if (IS_INTEGER(args[1])) {
            int64_t i1 = AS_INTEGER(args[1]);
            if (i1 < 1 || i1 > t->entryCount) return NIL_VAL;
            idx = (int)i1 - 1;
        } else if (IS_STRING(args[1])) {
            for (int i = 0; i < t->entryCount; i++) {
                if (strcmp(t->entries[i].name, AS_CSTRING(args[1])) == 0) { idx = i; break; }
            }
        }
        if (idx < 0) {
            owl_emit_native_error(E_NATIVE_ZIP_ENTRY_NOT_FOUND, nativeName,
                                  "name", IS_STRING(args[1]) ? AS_CSTRING(args[1]) : "?",
                                  NULL);
            return NIL_VAL;
        }
        TarEntry* e = &t->entries[idx];
        switch (element) {
            case ZIP_INFO_NOM:     return OBJ_VAL(obj_copy_string(e->name, (int)strlen(e->name)));
            case ZIP_INFO_TAILLE:  return INTEGER_VAL((int64_t)e->size);
            case ZIP_INFO_TAILLEC: return INTEGER_VAL((int64_t)e->size); /* TAR ne compresse pas */
            case ZIP_INFO_DATE:    return INTEGER_VAL((int64_t)e->mtime);
            case ZIP_INFO_CRC:     return INTEGER_VAL(0); /* TAR n'a pas de CRC par entrée */
            case ZIP_INFO_ATTRIB:  return INTEGER_VAL((int64_t)e->mode);
            default:               return NIL_VAL;
        }
    }

    mz_zip_archive* ar = (mz_zip_archive*)z->backend;
    if (IS_INTEGER(args[1])) {
        int64_t i1 = AS_INTEGER(args[1]);
        if (i1 < 1 || (mz_uint)(i1 - 1) >= mz_zip_reader_get_num_files(ar)) return NIL_VAL;
        idx = (int)i1 - 1;
    } else if (IS_STRING(args[1])) {
        idx = mz_zip_reader_locate_file(ar, AS_CSTRING(args[1]), NULL, 0);
    }
    if (idx < 0) {
        owl_emit_native_error(E_NATIVE_ZIP_ENTRY_NOT_FOUND, nativeName,
                              "name", IS_STRING(args[1]) ? AS_CSTRING(args[1]) : "?",
                              NULL);
        return NIL_VAL;
    }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(ar, (mz_uint)idx, &st)) return NIL_VAL;
    switch (element) {
        case ZIP_INFO_NOM:
            return OBJ_VAL(obj_copy_string(st.m_filename, (int)strlen(st.m_filename)));
        case ZIP_INFO_TAILLE:  return INTEGER_VAL((int64_t)st.m_uncomp_size);
        case ZIP_INFO_TAILLEC: return INTEGER_VAL((int64_t)st.m_comp_size);
        case ZIP_INFO_DATE:    return INTEGER_VAL((int64_t)st.m_time);
        case ZIP_INFO_CRC:     return INTEGER_VAL((int64_t)st.m_crc32);
        case ZIP_INFO_ATTRIB:  return INTEGER_VAL((int64_t)st.m_external_attr);
        default:               return NIL_VAL;
    }
}

/* ================================================================
 * zipTailleFichier(archive, indexOuNom [, type]) -> entier
 *   type : 0=décompressée (défaut), 1=compressée
 * ================================================================ */

static Value fn_zip_info_fichier(int argCount, Value* args) {
    return zip_info_fichier_impl(argCount, args, "zipInfoFichier");
}

static Value fn_zip_taille_fichier(int argCount, Value* args) {
    if (argCount < 2 || !IS_ZIP_ARCHIVE(args[0])) return INTEGER_VAL(0);
    int element = ZIP_INFO_TAILLE;
    if (argCount >= 3 && IS_INTEGER(args[2]) && AS_INTEGER(args[2]) == 1) {
        element = ZIP_INFO_TAILLEC;
    }
    Value shim[3] = { args[0], args[1], INTEGER_VAL(element) };
    return zip_info_fichier_impl(3, shim, "zipTailleFichier");
}

/* ================================================================
 * zipExisteFichier(archive, nom) -> booléen
 * ================================================================ */

static Value fn_zip_existe_fichier(int argCount, Value* args) {
    Value r = fn_zip_cherche_fichier(argCount, args);
    return BOOL_VAL(IS_INTEGER(r) && AS_INTEGER(r) > 0);
}

/* ================================================================
 * zipExiste(chemin) -> booléen
 *   Vérifie qu'un fichier d'archive existe sur disque.
 * ================================================================ */

static Value fn_zip_existe(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return BOOL_VAL(false);
    FILE* f = fopen(AS_CSTRING(args[0]), "rb");
    if (!f) return BOOL_VAL(false);
    fclose(f);
    return BOOL_VAL(true);
}

/* ================================================================
 * zipNbPartie(archive) -> entier
 * zipEstMulti(archive) -> booléen
 *   Multi-volume : Phase 1 = toujours 1 partie / non-multi.
 * ================================================================ */

static Value fn_zip_nb_partie(int argCount, Value* args) {
    (void)argCount; (void)args;
    return INTEGER_VAL(1);
}
static Value fn_zip_est_multi(int argCount, Value* args) {
    (void)argCount; (void)args;
    return BOOL_VAL(false);
}

/* ================================================================
 * zipAjouteRépertoire(archive, cheminRépertoire [, sousRép [, portionChemin]])
 *   Ajoute récursivement tous les fichiers d'un répertoire.
 * ================================================================ */

#ifdef _WIN32
#include <windows.h>
/* Walk a directory recursively and call add_file on each file. The relative
 * path stored in the archive is computed from `rootLen` (length of rootDir
 * including trailing separator). */
static int walk_and_add_win(ObjZipArchive* z, const char* rootDir, int rootLen,
                             const char* currentDir) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", currentDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s\\%s", currentDir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_and_add_win(z, rootDir, rootLen, full);
        } else {
            const char* rel = full + rootLen;
            /* Normalise les séparateurs vers '/' pour l'archive. */
            char relNorm[1024];
            size_t i = 0;
            while (rel[i] && i < sizeof(relNorm) - 1) {
                relNorm[i] = (rel[i] == '\\') ? '/' : rel[i];
                i++;
            }
            relNorm[i] = '\0';

            if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
                FILE* f = fopen(full, "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                size_t sz = (size_t)ftell(f);
                fseek(f, 0, SEEK_SET);
                unsigned char* data = (unsigned char*)malloc(sz);
                if (data && fread(data, 1, sz, f) == sz) {
                    tar_add_entry((TarState*)z->backend, relNorm, data, sz);
                    z->fileCount++;
                }
                free(data); fclose(f);
            } else {
                mz_zip_archive* ar = (mz_zip_archive*)z->backend;
                mz_uint flags = (mz_uint)(z->compressionLevel & 0x0F);
                if (mz_zip_writer_add_file(ar, relNorm, full, NULL, 0, flags)) {
                    z->fileCount++;
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}
#else
#include <dirent.h>
#include <sys/stat.h>
static int walk_and_add_posix(ObjZipArchive* z, const char* rootDir, int rootLen,
                               const char* currentDir) {
    DIR* d = opendir(currentDir);
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", currentDir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_and_add_posix(z, rootDir, rootLen, full);
        } else if (S_ISREG(st.st_mode)) {
            const char* rel = full + rootLen;
            if (z->format == ZIP_FMT_TAR || z->format == ZIP_FMT_TARGZ) {
                FILE* f = fopen(full, "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                size_t sz = (size_t)ftell(f);
                fseek(f, 0, SEEK_SET);
                unsigned char* data = (unsigned char*)malloc(sz);
                if (data && fread(data, 1, sz, f) == sz) {
                    tar_add_entry((TarState*)z->backend, rel, data, sz);
                    z->fileCount++;
                }
                free(data); fclose(f);
            } else {
                mz_zip_archive* ar = (mz_zip_archive*)z->backend;
                mz_uint flags = (mz_uint)(z->compressionLevel & 0x0F);
                if (mz_zip_writer_add_file(ar, rel, full, NULL, 0, flags)) {
                    z->fileCount++;
                }
            }
        }
    }
    closedir(d);
    return 0;
}
#endif

static Value fn_zip_ajoute_repertoire(int argCount, Value* args) {
    OWL_REQUIRE_ARG("zipAjouteRépertoire", "archive", 0, IS_ZIP_ARCHIVE(args[0]),
                    "zipArchive", BOOL_VAL(false));
    OWL_REQUIRE_ARG("zipAjouteRépertoire", "répertoire", 1, IS_STRING(args[1]),
                    "chaîne", BOOL_VAL(false));
    ObjZipArchive* z = AS_ZIP_ARCHIVE(args[0]);
    if (!z->open || z->mode == ZIP_MODE_READ) {
        owl_emit_native_error(E_NATIVE_ZIP_ARCHIVE_NOT_OPEN_WRITE, "zipAjouteRépertoire",
                              "native", "zipAjouteRépertoire",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* root = AS_CSTRING(args[1]);
    int rootLen = (int)strlen(root);
    /* +1 pour inclure le séparateur trailing dans le stripping. */
    if (rootLen > 0 && (root[rootLen-1] == '/' || root[rootLen-1] == '\\')) {
        /* ok tel quel */
    } else {
        rootLen++; /* le walker ajoute un '\\' via snprintf */
    }
#ifdef _WIN32
    int rc = walk_and_add_win(z, root, rootLen, root);
#else
    int rc = walk_and_add_posix(z, root, rootLen, root);
#endif
    if (rc != 0) {
        owl_emit_native_error(E_NATIVE_ZIP_IO_FAILURE, "zipAjouteRépertoire",
                              "native", "zipAjouteRépertoire",
                              "path", root,
                              "detail", "répertoire introuvable",
                              NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Params / Signatures pour toutes les natives archive
 * ================================================================ */

static const OwlParam p_arch[]        = { {"archive", "zipArchive", "Archive"}, {NULL,NULL,NULL} };
static const OwlParam p_path_f1[]     = { {"chemin",  "cha\xc3\xaene", "Chemin disque"}, {NULL,NULL,NULL} };
static const OwlParam p_path_f[]      = { {"chemin",  "cha\xc3\xaene", "Chemin disque"},
                                           {"format",  "entier",        "Constante zipFormat*", .allowedBase=&E_zipFormat}, {NULL,NULL,NULL} };
static const OwlParam p_path_m1[]     = { {"chemin",  "cha\xc3\xaene", "Chemin disque"}, {NULL,NULL,NULL} };
static const OwlParam p_path_m[]      = { {"chemin",  "cha\xc3\xaene", "Chemin disque"},
                                           {"mode",    "entier",        "Constante zipLecture/zipEcriture", .allowedBase=&E_zipMode}, {NULL,NULL,NULL} };
/* zipAjouteFichier — formes disque (2-4) et buffer (4-5). */
static const OwlParam p_addfile2[]    = { {"archive", "zipArchive", "Archive"},
                                           {"fichier", "cha\xc3\xaene|buffer", "Chemin disque, ou buffer (avec zipEnMémoire)"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_addfile3[]    = { {"archive", "zipArchive", "Archive"},
                                           {"fichier", "cha\xc3\xaene|buffer", "Chemin disque, ou buffer (avec zipEnMémoire)"},
                                           {"portionChemin", "entier", "Constante zipAucun/Répertoire/Disque, ou zipEnMémoire pour un buffer", .allowedBase=&E_zipPortionChemin},
                                           {NULL,NULL,NULL} };
static const OwlParam p_addfile4[]    = { {"archive", "zipArchive", "Archive"},
                                           {"fichier", "cha\xc3\xaene|buffer", "Chemin disque, ou buffer (avec zipEnMémoire)"},
                                           {"portionChemin", "entier", "Constante zipAucun/Répertoire/Disque, ou zipEnMémoire pour un buffer", .allowedBase=&E_zipPortionChemin},
                                           {"chemin", "cha\xc3\xaene", "Chemin à conserver dans l'archive (forme buffer)"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_addfile5[]    = { {"archive", "zipArchive", "Archive"},
                                           {"fichier", "cha\xc3\xaene|buffer", "Chemin disque, ou buffer (avec zipEnMémoire)"},
                                           {"portionChemin", "entier", "Constante zipAucun/Répertoire/Disque, ou zipEnMémoire pour un buffer", .allowedBase=&E_zipPortionChemin},
                                           {"chemin", "cha\xc3\xaene", "Chemin à conserver dans l'archive (forme buffer)"},
                                           {"progression", "procédure", "Procédure de progression optionnelle"},
                                           {NULL,NULL,NULL} };
/* zipExtraitFichier — 2-4 (archive, indexOuNom, destination, progression). */
static const OwlParam p_extf2[]       = { {"archive", "zipArchive", "Archive"},
                                           {"indexOuNom", "entier|cha\xc3\xaene", "Index 1-based ou nom"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_extf3[]       = { {"archive", "zipArchive", "Archive"},
                                           {"indexOuNom", "entier|cha\xc3\xaene", "Index 1-based ou nom"},
                                           {"destination", "cha\xc3\xaene|entier", "Chemin disque ou zipEnMémoire"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_extf4[]       = { {"archive", "zipArchive", "Archive"},
                                           {"indexOuNom", "entier|cha\xc3\xaene", "Index 1-based ou nom"},
                                           {"destination", "cha\xc3\xaene|entier", "Chemin disque ou zipEnMémoire"},
                                           {"progression", "procédure", "Procédure de progression optionnelle"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_arch_name[]   = { {"archive", "zipArchive", "Archive"},
                                           {"nom",     "cha\xc3\xaene", "Nom d'entrée"}, {NULL,NULL,NULL} };
static const OwlParam p_arch_niveau[] = { {"archive", "zipArchive", "Archive"},
                                           {"niveau",  "entier", "Niveau 0-9"}, {NULL,NULL,NULL} };
static const OwlParam p_arch_pwd[]    = { {"archive", "zipArchive", "Archive"},
                                           {"motDePasse", "cha\xc3\xaene", "Mot de passe"}, {NULL,NULL,NULL} };
/* zipExtraitTout — 1-5 (archive [, destination [, options] [, procédure] [, gestion]]). */
static const OwlParam p_extall1[]     = { {"archive", "zipArchive", "Archive"}, {NULL,NULL,NULL} };
static const OwlParam p_extall2[]     = { {"archive", "zipArchive", "Archive"},
                                           {"destination", "cha\xc3\xaene", "Répertoire cible"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_extall3[]     = { {"archive", "zipArchive", "Archive"},
                                           {"destination", "cha\xc3\xaene", "Répertoire cible"},
                                           {"options", "entier|proc\xc3\xa9""dure", "Options ou procédure de progression"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_extall4[]     = { {"archive", "zipArchive", "Archive"},
                                           {"destination", "cha\xc3\xaene", "Répertoire cible"},
                                           {"options", "entier", "Options d'extraction"},
                                           {"progression", "proc\xc3\xa9""dure", "Procédure de progression"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_extall5[]     = { {"archive", "zipArchive", "Archive"},
                                           {"destination", "cha\xc3\xaene", "Répertoire cible"},
                                           {"options", "entier", "Options d'extraction"},
                                           {"progression", "proc\xc3\xa9""dure", "Procédure de progression"},
                                           {"gestion", "entier", "Gestion de la barre de progression"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_arch_idx_el[] = { {"archive", "zipArchive", "Archive"},
                                           {"indexOuNom", "entier|cha\xc3\xaene", "Index 1-based ou nom"},
                                           {"\xc3\xa9l\xc3\xa9""ment", "entier", "Constante zipNom/Taille/...", .allowedBase=&E_zipInfo},
                                           {NULL,NULL,NULL} };
/* zipTailleFichier — 2-3 (archive, indexOuNom [, type]). */
static const OwlParam p_taille2[]     = { {"archive", "zipArchive", "Archive"},
                                           {"indexOuNom", "entier|cha\xc3\xaene", "Index ou nom"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_taille3[]     = { {"archive", "zipArchive", "Archive"},
                                           {"indexOuNom", "entier|cha\xc3\xaene", "Index ou nom"},
                                           {"type", "entier", "0=décompressée, 1=compressée"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_path_simple[] = { {"chemin", "cha\xc3\xaene", "Chemin"}, {NULL,NULL,NULL} };
/* zipAjouteRépertoire — 2-6 (archive, répertoire [, sousRép [, option [, portion [, progression]]]]). */
static const OwlParam p_adddir2[]     = { {"archive", "zipArchive", "Archive"},
                                           {"r\xc3\xa9pertoire", "cha\xc3\xaene", "Répertoire à ajouter récursivement"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_adddir3[]     = { {"archive", "zipArchive", "Archive"},
                                           {"r\xc3\xa9pertoire", "cha\xc3\xaene", "Répertoire à ajouter récursivement"},
                                           {"sousR\xc3\xa9pertoire", "bool\xc3\xa9""en", "Inclure les sous-répertoires"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_adddir4[]     = { {"archive", "zipArchive", "Archive"},
                                           {"r\xc3\xa9pertoire", "cha\xc3\xaene", "Répertoire à ajouter récursivement"},
                                           {"sousR\xc3\xa9pertoire", "bool\xc3\xa9""en", "Inclure les sous-répertoires"},
                                           {"option", "entier", "Option de mémorisation du chemin"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_adddir5[]     = { {"archive", "zipArchive", "Archive"},
                                           {"r\xc3\xa9pertoire", "cha\xc3\xaene", "Répertoire à ajouter récursivement"},
                                           {"sousR\xc3\xa9pertoire", "bool\xc3\xa9""en", "Inclure les sous-répertoires"},
                                           {"option", "entier", "Option de mémorisation du chemin"},
                                           {"portionChemin", "entier", "Portion du chemin à ignorer", .allowedBase=&E_zipPortionChemin},
                                           {NULL,NULL,NULL} };
static const OwlParam p_adddir6[]     = { {"archive", "zipArchive", "Archive"},
                                           {"r\xc3\xa9pertoire", "cha\xc3\xaene", "Répertoire à ajouter récursivement"},
                                           {"sousR\xc3\xa9pertoire", "bool\xc3\xa9""en", "Inclure les sous-répertoires"},
                                           {"option", "entier", "Option de mémorisation du chemin"},
                                           {"portionChemin", "entier", "Portion du chemin à ignorer", .allowedBase=&E_zipPortionChemin},
                                           {"progression", "proc\xc3\xa9""dure", "Procédure de progression"},
                                           {NULL,NULL,NULL} };
static const OwlParam p_ae2[]         = { {"donn\xc3\xa9""es", "buffer|cha\xc3\xaene", "Données"},
                                           {"motDePasse", "cha\xc3\xaene", "Mot de passe UTF-8"},
                                           {NULL,NULL,NULL} };

static const OwlSignature sig_cree[]     = { {1, p_path_f1, "zipArchive", "Archive ouverte en écriture"},
                                              {2, p_path_f,  "zipArchive", "Archive ouverte en écriture"}, {0} };
static const OwlSignature sig_ouvre[]    = { {1, p_path_m1, "zipArchive", "Archive ouverte"},
                                              {2, p_path_m,  "zipArchive", "Archive ouverte"}, {0} };
static const OwlSignature sig_ferme[]    = { {1,  p_arch,       "bool\xc3\xa9""en", "Vrai si fermé"}, {0} };
static const OwlSignature sig_addfile[]  = { {2, p_addfile2, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {3, p_addfile3, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {4, p_addfile4, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {5, p_addfile5, "bool\xc3\xa9""en", "Vrai si ajouté"}, {0} };
static const OwlSignature sig_nb[]       = { {1,  p_arch,       "entier", "Nombre de fichiers"}, {0} };
static const OwlSignature sig_liste[]    = { {1,  p_arch,       "cha\xc3\xaene", "Noms LF-séparés"}, {0} };
static const OwlSignature sig_extrait[]  = { {2, p_extf2, "buffer|bool\xc3\xa9""en", "Buffer extrait (mémoire) ou Vrai (disque), Null si échec"},
                                              {3, p_extf3, "buffer|bool\xc3\xa9""en", "Buffer extrait (mémoire) ou Vrai (disque), Null si échec"},
                                              {4, p_extf4, "buffer|bool\xc3\xa9""en", "Buffer extrait (mémoire) ou Vrai (disque), Null si échec"}, {0} };
static const OwlSignature sig_cherche[]  = { {2,  p_arch_name,  "entier", "Index 1-based, 0 si absent"}, {0} };
static const OwlSignature sig_niveau[]   = { {2,  p_arch_niveau, "bool\xc3\xa9""en", "Vrai si niveau accepté"}, {0} };
static const OwlSignature sig_pwd[]      = { {2,  p_arch_pwd,   "bool\xc3\xa9""en", "Vrai si défini"}, {0} };
static const OwlSignature sig_extout[]   = { {1, p_extall1, "bool\xc3\xa9""en", "Vrai si extrait, Faux si échec ou annulation"},
                                              {2, p_extall2, "bool\xc3\xa9""en", "Vrai si extrait, Faux si échec ou annulation"},
                                              {3, p_extall3, "bool\xc3\xa9""en", "Vrai si extrait, Faux si échec ou annulation"},
                                              {4, p_extall4, "bool\xc3\xa9""en", "Vrai si extrait, Faux si échec ou annulation"},
                                              {5, p_extall5, "bool\xc3\xa9""en", "Vrai si extrait, Faux si échec ou annulation"}, {0} };
static const OwlSignature sig_info[]     = { {3,  p_arch_idx_el, "chaîne|entier", "Valeur demandée (selon zfXxx : chaîne pour Nom/Date, entier pour Taille/CRC...)"}, {0} };
static const OwlSignature sig_taille[]   = { {2, p_taille2, "entier", "Taille en octets"},
                                              {3, p_taille3, "entier", "Taille en octets"}, {0} };
static const OwlSignature sig_existe[]   = { {1,  p_path_simple, "bool\xc3\xa9""en", "Vrai si l'archive existe"}, {0} };
static const OwlSignature sig_existe_f[] = { {2,  p_arch_name,  "bool\xc3\xa9""en", "Vrai si l'entrée existe"}, {0} };
static const OwlSignature sig_multi[]    = { {1,  p_arch,       "bool\xc3\xa9""en|entier", "Nombre de parties (multi-volume)"}, {0} };
static const OwlSignature sig_adddir[]   = { {2, p_adddir2, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {3, p_adddir3, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {4, p_adddir4, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {5, p_adddir5, "bool\xc3\xa9""en", "Vrai si ajouté"},
                                              {6, p_adddir6, "bool\xc3\xa9""en", "Vrai si ajouté"}, {0} };
static const OwlSignature sig_ae2[]      = { {2,  p_ae2,        "buffer", "Résultat"}, {0} };

/* methodBindings — re-exposition des opérations zipXxx en méthodes sur le
 * receveur zipArchive (formes courtes, calquées sur sqlConnexion). Même impl,
 * deux syntaxes : zipXxx(arch, ...) (fonction libre canon) et arch.Mmm(...).
 * Arity = total INCLUANT le receveur (= arity de la signature, archive 1er
 * param). -1 = variadique. */
static const OwlMethodBinding MB_ZFERME[]   = { {"zipArchive","Ferme",1},  {"zipArchive","Close",1},  {NULL,NULL,0} };
/* .Ajoute() = LE binding d'ajout unique sur zipAjouteFichier : couvre les
 * syntaxes disque (chemin) ET buffer (zipEnMémoire). Arité variable. */
static const OwlMethodBinding MB_ZADD[]      = { {"zipArchive","Ajoute",-1}, {"zipArchive","Add",-1},   {NULL,NULL,0} };
static const OwlMethodBinding MB_ZADDDIR[]   = { {"zipArchive","AjouteRepertoire",-1}, {"zipArchive","AddDirectory",-1}, {NULL,NULL,0} };
static const OwlMethodBinding MB_ZNB[]       = { {"zipArchive","NbFichier",1}, {"zipArchive","FileCount",1}, {NULL,NULL,0} };
static const OwlMethodBinding MB_ZLISTE[]    = { {"zipArchive","Liste",1},  {"zipArchive","List",1},   {NULL,NULL,0} };
static const OwlMethodBinding MB_ZEXTF[]     = { {"zipArchive","Extrait",-1}, {"zipArchive","Extract",-1}, {NULL,NULL,0} };
static const OwlMethodBinding MB_ZEXTALL[]   = { {"zipArchive","ExtraitTout",-1}, {"zipArchive","ExtractAll",-1}, {NULL,NULL,0} };
static const OwlMethodBinding MB_ZCHERCHE[]  = { {"zipArchive","Cherche",2}, {"zipArchive","Find",2},   {NULL,NULL,0} };
static const OwlMethodBinding MB_ZNIVEAU[]   = { {"zipArchive","NiveauCompression",2}, {"zipArchive","CompressionLevel",2}, {NULL,NULL,0} };
static const OwlMethodBinding MB_ZPWD[]      = { {"zipArchive","MotDePasse",2}, {"zipArchive","Password",2}, {NULL,NULL,0} };
static const OwlMethodBinding MB_ZINFO[]     = { {"zipArchive","Info",3},   {NULL,NULL,0} };
static const OwlMethodBinding MB_ZTAILLE[]   = { {"zipArchive","Taille",-1}, {"zipArchive","Size",-1},  {NULL,NULL,0} };
static const OwlMethodBinding MB_ZEXISTEF[]  = { {"zipArchive","ExisteFichier",2}, {"zipArchive","FileExists",2}, {NULL,NULL,0} };

/* Le nom du paramètre macro `impl_fn` évite la collision avec le designator
 * `.impl` (le préprocesseur sinon remplace les deux). */
#define NAT_ARCH(nm_canon, nm_ascii, nm_en, impl_fn, descfr, descen, sig) \
    { .canonical = (nm_canon), .canonicalAscii = (nm_ascii), .nameEN = (nm_en), \
      .category = "Compression", .descFR = (descfr), .descEN = (descen), \
      .signatures = (sig), .impl = (impl_fn) }
#define NAT_ARCH_MB(nm_canon, nm_ascii, nm_en, impl_fn, descfr, descen, sig, mb) \
    { .canonical = (nm_canon), .canonicalAscii = (nm_ascii), .nameEN = (nm_en), \
      .category = "Compression", .descFR = (descfr), .descEN = (descen), \
      .signatures = (sig), .impl = (impl_fn), .methodBindings = (mb) }

static const OwlNativeDef ARCHIVE_DEFS[] = {
    NAT_ARCH("zipCr\xc3\xa9""e", "zipCree", "zipCreate", fn_zip_cree,
        "Crée une nouvelle archive (ZIP/WDZ).",
        "Create a new archive (ZIP/WDZ).", sig_cree),
    NAT_ARCH("zipOuvre", "zipOuvre", "zipOpen", fn_zip_ouvre,
        "Ouvre une archive existante en lecture ou écriture.",
        "Open an existing archive for reading or writing.", sig_ouvre),
    NAT_ARCH_MB("zipFerme", "zipFerme", "zipClose", fn_zip_ferme,
        "Ferme l'archive et libère les ressources.",
        "Close the archive and release resources.", sig_ferme, MB_ZFERME),
    NAT_ARCH_MB("zipAjouteFichier", "zipAjouteFichier", "zipAddFile", fn_zip_ajoute_fichier,
        "Ajoute une entrée : fichier disque, ou buffer en mémoire (zipEnMémoire).",
        "Add an entry: a disk file, or an in-memory buffer (zipInMemory).", sig_addfile, MB_ZADD),
    NAT_ARCH_MB("zipNbFichier", "zipNbFichier", "zipNbFile", fn_zip_nb_fichier,
        "Nombre d'entrées dans l'archive.",
        "Number of entries in the archive.", sig_nb, MB_ZNB),
    NAT_ARCH_MB("zipListeFichier", "zipListeFichier", "zipListFile", fn_zip_liste_fichier,
        "Liste des noms d'entrées (séparées par \\n).",
        "List of entry names (newline-separated).", sig_liste, MB_ZLISTE),
    NAT_ARCH_MB("zipExtraitFichier", "zipExtraitFichier", "zipExtractFile", fn_zip_extrait_fichier,
        "Extrait une entrée vers le disque ou en mémoire.",
        "Extract an entry to disk or to memory.", sig_extrait, MB_ZEXTF),
    NAT_ARCH_MB("zipChercheFichier", "zipChercheFichier", "zipFindFile", fn_zip_cherche_fichier,
        "Cherche une entrée par nom (index 1-based, 0 si absent).",
        "Find an entry by name (1-based index, 0 if absent).", sig_cherche, MB_ZCHERCHE),
    NAT_ARCH_MB("zipNiveauCompression", "zipNiveauCompression", "zipCompressionLevel", fn_zip_niveau_compression,
        "Définit le niveau de compression (0-9).",
        "Set compression level (0-9).", sig_niveau, MB_ZNIVEAU),
    NAT_ARCH_MB("zipMotDePasse", "zipMotDePasse", "zipPassword", fn_zip_mot_de_passe,
        "Définit le mot de passe (AES-256, partiel).",
        "Set the password (AES-256, partial).", sig_pwd, MB_ZPWD),
    NAT_ARCH_MB("zipExtraitTout", "zipExtraitTout", "zipExtractAll", fn_zip_extrait_tout,
        "Extrait toutes les entrées vers un répertoire.",
        "Extract every entry to a directory.", sig_extout, MB_ZEXTALL),
    NAT_ARCH_MB("zipInfoFichier", "zipInfoFichier", "zipInfoFile", fn_zip_info_fichier,
        "Métadonnées d'une entrée (nom, taille, date, CRC, ...).",
        "Entry metadata (name, size, date, CRC, ...).", sig_info, MB_ZINFO),
    NAT_ARCH_MB("zipTailleFichier", "zipTailleFichier", "zipFileSize", fn_zip_taille_fichier,
        "Taille d'une entrée (décompressée par défaut).",
        "Entry size (uncompressed by default).", sig_taille, MB_ZTAILLE),
    NAT_ARCH_MB("zipExisteFichier", "zipExisteFichier", "zipFileExists", fn_zip_existe_fichier,
        "Vrai si l'entrée existe dans l'archive.",
        "True if the entry exists in the archive.", sig_existe_f, MB_ZEXISTEF),
    NAT_ARCH("zipExiste", "zipExiste", "zipExist", fn_zip_existe,
        "Vrai si le fichier archive existe sur disque.",
        "True if the archive file exists on disk.", sig_existe),
    NAT_ARCH("zipNbPartie", "zipNbPartie", "zipNbPart", fn_zip_nb_partie,
        "Nombre de parties (multi-volume, 1 actuellement).",
        "Number of parts (multi-volume, 1 currently).", sig_multi),
    NAT_ARCH("zipEstMulti", "zipEstMulti", "zipIsMulti", fn_zip_est_multi,
        "Vrai si l'archive est multi-volume (Faux pour le moment).",
        "True if the archive is multi-volume (False for now).", sig_multi),
    NAT_ARCH_MB("zipAjouteR\xc3\xa9""pertoire", "zipAjouteRepertoire", "zipAddDirectory",
        fn_zip_ajoute_repertoire,
        "Ajoute récursivement tous les fichiers d'un répertoire.",
        "Recursively add all files from a directory.", sig_adddir, MB_ZADDDIR),
    /* APIs internes (prefix __) — masquees du LSP/--vscode-data via OWL_NAT_INTERNAL. */
    { .canonical="__AE2Encrypt", .canonicalAscii="__AE2Encrypt", .nameEN="__AE2Encrypt",
      .category="Compression", .flags=OWL_NAT_INTERNAL,
      .descFR="Chiffre un buffer en WinZip AES-2 (API interne).",
      .descEN="Encrypt a buffer with WinZip AES-2 (internal API).",
      .signatures=sig_ae2, .impl=fn_ae2_encrypt },
    { .canonical="__AE2Decrypt", .canonicalAscii="__AE2Decrypt", .nameEN="__AE2Decrypt",
      .category="Compression", .flags=OWL_NAT_INTERNAL,
      .descFR="Dechiffre un buffer WinZip AES-2 (API interne).",
      .descEN="Decrypt a WinZip AES-2 buffer (internal API).",
      .signatures=sig_ae2, .impl=fn_ae2_decrypt },
};

void stdlib_compression_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_module(ARCHIVE_DEFS,
        (int)(sizeof(ARCHIVE_DEFS)/sizeof(ARCHIVE_DEFS[0])));
    owl_registry_register_consts(COMP_CONSTS,
        (int)(sizeof(COMP_CONSTS)/sizeof(COMP_CONSTS[0])));
    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * (zipEnMemoire pour zipEnMémoire, etc.). */
    owl_registry_register_enums(COMP_ENUMS,
        (int)(sizeof(COMP_ENUMS)/sizeof(COMP_ENUMS[0])));

    /* Backend close hooks — le GC appelle ces callbacks quand un
     * ObjZipArchive est collecté sans que l'user ait appelé zipFerme. */
    owl_zip_register_backend_close(ZIP_FMT_ZIP,   zip_backend_close);
    owl_zip_register_backend_close(ZIP_FMT_WDZ,   zip_backend_close);
    owl_zip_register_backend_close(ZIP_FMT_TAR,   tar_backend_close);
    owl_zip_register_backend_close(ZIP_FMT_TARGZ, tar_backend_close);
    owl_zip_register_backend_close(ZIP_FMT_7Z,    sevenz_backend_close);
}
