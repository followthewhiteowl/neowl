// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* encoding_self_test.c - Garde d'hygiene encodage des fichiers source.
 *
 * Voir encoding_self_test.h. Suite de  : un .c reencode en
 * double-UTF-8 (mojibake) + BOM par un editeur avait pourri 4 tests LSP
 * pendant des jours sans etre detecte. Ce self-test rend ce type de
 * corruption impossible a rater : il casse `make` (exit != 0).
 *
 * 3 regles, blocage dur si violation :
 *   1. UTF-8 invalide (RFC 3629 strict).
 *   2. BOM UTF-8 (EF BB BF) en tete — sauf whitelist par chemin.
 *   3. Mojibake C3 83 : signature exacte du double-encodage de .
 *      En contenu FR/EN/code legitime, C3 83 ('A tilde', U+00C3) n'apparait
 *      jamais (0 occurrence sur tout le depot).
 *
 * Detection 100% byte-level : aucune dependance a la locale (cf. ,
 * ou iconv deraillait sur la locale du git-bash).
 */

#include "encoding_self_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ============================================================
 * Verdicts + detecteur (cœur testable in-memory)
 * ============================================================ */

typedef enum {
    ENC_OK = 0,
    ENC_NOT_UTF8,
    ENC_MOJIBAKE
} EncVerdict;

/* Detecte le BOM UTF-8 (EF BB BF) en tete de buffer. */
static int encoding_has_bom(const unsigned char* b, size_t n) {
    return (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 1 : 0;
}

/* Scan octet du contenu : valide l'UTF-8 strict (RFC 3629) et detecte la
 * signature mojibake C3 83. Retourne le PREMIER verdict != OK, et renseigne
 * *outLine (1-based) + *outOff (offset absolu) du point fautif.
 *
 * La boucle ne revient en tete qu'aux frontieres de sequence (i avance par
 * sequence complete), donc tester C3 83 sur `c` ne touche jamais un octet de
 * continuation interne. */
static EncVerdict encoding_scan_content(const unsigned char* b, size_t n,
                                        size_t* outLine, size_t* outOff) {
    size_t i = 0;
    size_t line = 1;
    while (i < n) {
        unsigned char c = b[i];

        /* Mojibake : 'A tilde' (C3 83) en tete de sequence. U+00C3 est un
         * UTF-8 valide, donc cette regle DOIT passer avant la validation. */
        if (c == 0xC3 && i + 1 < n && b[i + 1] == 0x83) {
            if (outLine) *outLine = line;
            if (outOff) *outOff = i;
            return ENC_MOJIBAKE;
        }

        if (c == '\n') { line++; i++; continue; }

        if (c < 0x80) {
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2 octets U+0080..U+07FF. C0/C1 = overlong -> rejet. */
            if (c < 0xC2) goto bad;
            if (i + 1 >= n || (b[i + 1] & 0xC0) != 0x80) goto bad;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3 octets U+0800..U+FFFF, rejet overlong + surrogates. */
            if (i + 2 >= n ||
                (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80) goto bad;
            unsigned cp = ((unsigned)(c & 0x0F) << 12) |
                          ((unsigned)(b[i + 1] & 0x3F) << 6) |
                          (unsigned)(b[i + 2] & 0x3F);
            if (cp < 0x800) goto bad;                    /* overlong */
            if (cp >= 0xD800 && cp <= 0xDFFF) goto bad;  /* surrogate */
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4 octets U+10000..U+10FFFF. */
            if (i + 3 >= n ||
                (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80 ||
                (b[i + 3] & 0xC0) != 0x80) goto bad;
            unsigned cp = ((unsigned)(c & 0x07) << 18) |
                          ((unsigned)(b[i + 1] & 0x3F) << 12) |
                          ((unsigned)(b[i + 2] & 0x3F) << 6) |
                          (unsigned)(b[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) goto bad;
            i += 4;
        } else {
            goto bad;  /* continuation orpheline, C0/C1, ou 0xF8+ */
        }
        continue;
    bad:
        if (outLine) *outLine = line;
        if (outOff) *outOff = i;
        return ENC_NOT_UTF8;
    }
    return ENC_OK;
}

/* ============================================================
 * Reporter
 * ============================================================ */

typedef struct {
    int            count;
    int            scanned;
    FILE*          out;
    unsigned char* buf;   /* buffer de lecture reutilise entre fichiers */
    size_t         cap;   /* capacite courante de buf */
} EncReporter;

static void enc_report(EncReporter* r, const char* rule, const char* path,
                       size_t line, size_t off, const char* hint) {
    r->count++;
    fprintf(r->out, "  [%s] %s", rule, path);
    if (line > 0) fprintf(r->out, " (ligne %zu, offset %zu)", line, off);
    fprintf(r->out, "\n      -> %s\n", hint);
}

/* ============================================================
 * Non-regression in-memory : prouve que le detecteur attrape bien
 * une corruption (le test ne touche PAS le corpus scanne).
 * ============================================================ */

static void enc_expect(EncReporter* r, int ok, const char* what) {
    if (!ok) {
        r->count++;
        fprintf(r->out, "  [self_check] in-memory: %s\n", what);
    }
}

static void encoding_self_test_inmem(EncReporter* r) {
    size_t line, off;

    /* ASCII pur -> OK */
    {
        const unsigned char v[] = "bonjour\nmonde\n";
        enc_expect(r, encoding_scan_content(v, sizeof v - 1, &line, &off) == ENC_OK,
                   "ASCII valide doit etre OK");
    }
    /* UTF-8 accentue legitime (e accent aigu = C3 A9) -> OK */
    {
        const unsigned char v[] = { 'c', 'a', 'f', 0xC3, 0xA9, '\n' };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_OK,
                   "UTF-8 accentue valide doit etre OK");
    }
    /* Mojibake C3 83 -> MOJIBAKE */
    {
        const unsigned char v[] = { 'x', 0xC3, 0x83, 0xC2, 0xA9 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_MOJIBAKE,
                   "mojibake C3 83 doit etre rejete");
    }
    /* Latin-1 isole (0xE9 sans continuation) -> NOT_UTF8 */
    {
        const unsigned char v[] = { 'c', 'a', 'f', 0xE9 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "octet latin-1 isole doit etre rejete");
    }
    /* Continuation orpheline (0x80 en tete) -> NOT_UTF8 */
    {
        const unsigned char v[] = { 0x80, 0x41 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "continuation orpheline doit etre rejetee");
    }
    /* Surrogate U+D800 encode ED A0 80 -> NOT_UTF8 */
    {
        const unsigned char v[] = { 0xED, 0xA0, 0x80 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "surrogate UTF-8 doit etre rejete");
    }
    /* Overlong '/' encode C0 AF -> NOT_UTF8 */
    {
        const unsigned char v[] = { 0xC0, 0xAF };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "sequence overlong doit etre rejetee");
    }
    /* Overlong 3 octets E0 80 80 (encode U+0000 sur 3 octets) -> NOT_UTF8 */
    {
        const unsigned char v[] = { 0xE0, 0x80, 0x80 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "overlong 3 octets doit etre rejete");
    }
    /* 4 octets F4 90 80 80 (codepoint 0x110000 > U+10FFFF) -> NOT_UTF8 */
    {
        const unsigned char v[] = { 0xF4, 0x90, 0x80, 0x80 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "codepoint hors plage Unicode doit etre rejete");
    }
    /* Lead-byte seul en fin de buffer (C3 final, troncature) -> NOT_UTF8 */
    {
        const unsigned char v[] = { 'a', 0xC3 };
        enc_expect(r, encoding_scan_content(v, sizeof v, &line, &off) == ENC_NOT_UTF8,
                   "lead-byte tronque en fin de buffer doit etre rejete");
    }
    /* BOM present -> detecte */
    {
        const unsigned char v[] = { 0xEF, 0xBB, 0xBF, 'a' };
        enc_expect(r, encoding_has_bom(v, sizeof v) == 1,
                   "BOM EF BB BF doit etre detecte");
    }
    /* Pas de BOM -> non detecte */
    {
        const unsigned char v[] = { 'a', 'b', 'c' };
        enc_expect(r, encoding_has_bom(v, sizeof v) == 0,
                   "absence de BOM ne doit pas etre signalee");
    }
}

/* ============================================================
 * Whitelist BOM (table en dur, par chemin)
 * ============================================================ */

/* BOM intentionnel : fixture qui valide la tolerance du scanner aux
 * caracteres invisibles en tete de fichier. NE PAS reparer. */
static const char* ENC_BOM_WHITELIST[] = {
    "tests/runtime/scanner_invisible_carveout/test_invisible_carveout.owl",
    NULL
};

static int enc_bom_whitelisted(const char* path) {
    for (int i = 0; ENC_BOM_WHITELIST[i]; i++) {
        if (strstr(path, ENC_BOM_WHITELIST[i])) return 1;
    }
    return 0;
}

/* ============================================================
 * Scan filesystem
 * ============================================================ */

static int enc_has_scanned_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcmp(dot, ".owl") == 0 || strcmp(dot, ".c") == 0 ||
           strcmp(dot, ".h") == 0 || strcmp(dot, ".cpp") == 0 ||
           strcmp(dot, ".def") == 0 || strcmp(dot, ".md") == 0;
}

static void enc_scan_file(const char* path, EncReporter* r) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    r->scanned++;
    /* Lecture par blocs dans un buffer reutilise (pas de malloc/free par
     * fichier). On lit en entier — les sources OWL sont petites ; un .c
     * geant (>16 Mo, jamais le cas) serait simplement tronque au scan. */
    size_t nread = 0;
    for (;;) {
        if (nread + 65536 > r->cap) {
            size_t ncap = r->cap ? r->cap * 2 : 131072;
            while (ncap < nread + 65536) ncap *= 2;
            unsigned char* nb = (unsigned char*)realloc(r->buf, ncap);
            if (!nb) break;
            r->buf = nb;
            r->cap = ncap;
        }
        size_t got = fread(r->buf + nread, 1, 65536, f);
        nread += got;
        if (got < 65536) break;
        if (nread > 16 * 1024 * 1024) break;
    }
    fclose(f);
    unsigned char* buf = r->buf;

    if (encoding_has_bom(buf, nread) && !enc_bom_whitelisted(path)) {
        enc_report(r, "bom", path, 0, 0,
                   "BOM UTF-8 (EF BB BF) en tete. Casse shebangs/JSON strict. "
                   "Retirer le BOM (VSCode : Save with Encoding -> UTF-8 sans BOM).");
    }

    size_t line = 0, off = 0;
    EncVerdict v = encoding_scan_content(buf, nread, &line, &off);
    if (v == ENC_MOJIBAKE) {
        enc_report(r, "mojibake", path, line, off,
                   "Signature C3 83 ('A tilde') = double-encodage UTF-8. "
                   "Le fichier a ete reenregistre en mojibake. Restaurer "
                   "depuis git ou reconvertir l'encodage.");
    } else if (v == ENC_NOT_UTF8) {
        enc_report(r, "not-utf8", path, line, off,
                   "Octets non decodables en UTF-8 (latin-1/cp1252/UTF-16 ?). "
                   "Convertir en UTF-8 sans BOM.");
    }
}

static void enc_skip_dir(const char* name, int* skip) {
    *skip = (strcmp(name, ".git") == 0 || strcmp(name, "build") == 0 ||
             strcmp(name, "vendor") == 0 || strcmp(name, "node_modules") == 0 ||
             strcmp(name, "_local") == 0);
}

static void enc_scan_dir(const char* dirPath, EncReporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        /* d_type evite un stat() par entree quand le type est connu (le cas
         * sur MinGW/Linux). Fallback stat() uniquement si DT_UNKNOWN. */
        int isDir, isReg;
#ifdef DT_DIR
        if (e->d_type == DT_DIR)      { isDir = 1; isReg = 0; }
        else if (e->d_type == DT_REG) { isDir = 0; isReg = 1; }
        else
#endif
        {
            char probe[2048];
            snprintf(probe, sizeof(probe), "%s/%s", dirPath, e->d_name);
            struct stat st;
            if (stat(probe, &st) != 0) continue;
            isDir = S_ISDIR(st.st_mode);
            isReg = S_ISREG(st.st_mode);
        }

        if (isReg) {
            /* Filtre extension AVANT de construire le chemin / ouvrir. */
            if (!enc_has_scanned_ext(e->d_name)) continue;
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
            enc_scan_file(full, r);
        } else if (isDir) {
            int skip = 0;
            enc_skip_dir(e->d_name, &skip);
            if (!skip) {
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
                enc_scan_dir(full, r);
            }
        }
    }
    closedir(d);
}

/* ============================================================
 * Entree publique
 * ============================================================ */

int encoding_self_test_run(FILE* out) {
    EncReporter r = { 0, 0, out, NULL, 0 };

    encoding_self_test_inmem(&r);
    int inmemViolations = r.count;
    if (inmemViolations > 0) {
        fprintf(out, "encoding self-test: %d echec(s) in-memory (detecteur casse)\n",
                inmemViolations);
    }

    static const char* ROOTS[] = { "tests", "src", "lsp",
                                   "docs/public_documentation", NULL };
    for (int i = 0; ROOTS[i]; i++) enc_scan_dir(ROOTS[i], &r);

    /* Garde ensemble vide : ce controle conclut "0 violation" en ne
     * trouvant rien — verdict indistinguable de "je n'ai ouvert aucun
     * fichier". Les racines sont relatives au cwd : lance ailleurs qu'a la
     * racine du depot, il rendait vert en n'ayant rien lu. */
    if (r.scanned == 0) {
        fprintf(out, "encoding self-test: CORPUS VIDE — 0 fichier scanne "
                     "(racines relatives au cwd : racine du depot attendue). "
                     "Un controle qui n'examine rien ne prouve rien.\n");
        free(r.buf);
        return 1;
    }

    if (r.count == 0) {
        fprintf(out, "encoding self-test: OK (%d fichiers scannes, 0 violation)\n",
                r.scanned);
    } else {
        fprintf(out, "encoding self-test: %d violation(s) (%d fichiers scannes)\n",
                r.count, r.scanned);
    }
    free(r.buf);
    return r.count;
}
