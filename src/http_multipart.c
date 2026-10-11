// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "http_multipart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#ifdef _WIN32
#include <process.h>   /* _getpid */
#define OWL_MP_GETPID _getpid
#else
#include <unistd.h>    /* getpid */
#define OWL_MP_GETPID getpid
#endif

/* Recherche de motif dans un buffer BORNE (pas de dependance a memmem, absent
 * de certaines libc). Retourne l'index de la 1re occurrence >= from, ou -1. */
static long mp_find(const char* hay, size_t haylen,
                    const char* needle, size_t nlen, size_t from) {
    if (nlen == 0 || nlen > haylen) return -1;
    for (size_t i = from; i + nlen <= haylen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* Extrait le boundary de l'en-tete Content-Type (`...; boundary=xxx` ou
 * `boundary="xxx"`). Casse-insensible sur le nom du parametre. Retourne 0 si
 * trouve (non vide), -1 sinon. */
static int mp_extract_boundary(const char* ct, char* out, size_t cap) {
    for (const char* p = ct; *p; p++) {
        if (strncasecmp(p, "boundary", 8) != 0) continue;
        const char* q = p + 8;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        size_t n = 0;
        if (*q == '"') {
            q++;
            while (*q && *q != '"' && n + 1 < cap) out[n++] = *q++;
        } else {
            while (*q && *q != ';' && *q != ' ' && *q != '\t' &&
                   *q != '\r' && *q != '\n' && n + 1 < cap) {
                out[n++] = *q++;
            }
        }
        out[n] = '\0';
        return n > 0 ? 0 : -1;
    }
    return -1;
}

/* Copie bornee d'une tranche [s, s+len) en chaine possedee. */
static char* mp_dup_range(const char* s, size_t len) {
    char* d = (char*)malloc(len + 1);
    if (!d) return NULL;
    if (len > 0) memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

/* Extrait la valeur d'un parametre `key=` d'une ligne d'en-tete (Content-
 * Disposition). Gere la forme entre guillemets et la forme nue. Retourne une
 * chaine possedee, ou NULL si le parametre est absent. La recherche du nom de
 * parametre est casse-insensible et exige une frontiere gauche (`;`/espace) pour
 * ne pas confondre `name` et `filename`. */
static char* mp_disposition_param(const char* line, size_t len, const char* key) {
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen < len; i++) {
        /* Frontiere gauche : debut, ';' ou espace juste avant le nom. */
        if (i > 0) {
            char prev = line[i - 1];
            if (prev != ';' && prev != ' ' && prev != '\t') continue;
        }
        if (strncasecmp(line + i, key, klen) != 0) continue;
        size_t j = i + klen;
        while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
        if (j >= len || line[j] != '=') continue;
        j++;
        while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
        if (j < len && line[j] == '"') {
            j++;
            size_t start = j;
            while (j < len && line[j] != '"') j++;
            return mp_dup_range(line + start, j - start);
        }
        size_t start = j;
        while (j < len && line[j] != ';' && line[j] != ' ' && line[j] != '\t') j++;
        return mp_dup_range(line + start, j - start);
    }
    return NULL;
}

/* Analyse le bloc d'en-tetes d'une partie [h, h+len) (lignes CRLF, sans le
 * CRLFCRLF terminal). Renseigne name/filename/contentType (chaines possedees ou
 * NULL). */
static void mp_parse_part_headers(const char* h, size_t len,
                                  char** name, char** filename,
                                  char** contentType) {
    *name = NULL; *filename = NULL; *contentType = NULL;
    size_t i = 0;
    while (i < len) {
        long e = mp_find(h, len, "\r\n", 2, i);
        size_t lineEnd = (e < 0) ? len : (size_t)e;
        const char* line = h + i;
        size_t llen = lineEnd - i;
        if (strncasecmp(line, "Content-Disposition:", 20) == 0) {
            if (*name == NULL)     *name     = mp_disposition_param(line, llen, "name");
            if (*filename == NULL) *filename = mp_disposition_param(line, llen, "filename");
        } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
            const char* v = line + 13;
            size_t vlen = llen - 13;
            while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
            while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;
            if (*contentType == NULL && vlen > 0) *contentType = mp_dup_range(v, vlen);
        }
        if (e < 0) break;
        i = (size_t)e + 2;
    }
}

static int mp_append_part(OwlMultipartForm* out, int* cap,
                          char* name, char* filename, char* contentType,
                          const char* data, size_t dataLen) {
    if (out->count >= *cap) {
        int nc = *cap == 0 ? 8 : *cap * 2;
        OwlMultipartPart* np = (OwlMultipartPart*)realloc(
            out->parts, (size_t)nc * sizeof(OwlMultipartPart));
        if (!np) return -1;
        out->parts = np;
        *cap = nc;
    }
    OwlMultipartPart* p = &out->parts[out->count++];
    p->name        = name;
    p->filename    = filename;
    p->contentType = contentType;
    p->data        = data;
    p->dataLen     = dataLen;
    return 0;
}

int owl_multipart_parse(const char* contentType,
                        const char* body, size_t bodyLen,
                        OwlMultipartForm* out) {
    out->parts = NULL;
    out->count = 0;
    if (contentType == NULL || body == NULL) return -1;

    char boundary[256];
    if (mp_extract_boundary(contentType, boundary, sizeof boundary) != 0) return -1;

    char dash[262];
    int dl = snprintf(dash, sizeof dash, "--%s", boundary);
    if (dl <= 0 || (size_t)dl >= sizeof dash) return -1;
    size_t dashLen = (size_t)dl;

    /* Delimiteur inter-parties : CRLF + "--" + boundary (le CRLF ferme le
     * contenu de la partie precedente). */
    char crlfDash[266];
    int cl = snprintf(crlfDash, sizeof crlfDash, "\r\n--%s", boundary);
    if (cl <= 0 || (size_t)cl >= sizeof crlfDash) return -1;
    size_t crlfDashLen = (size_t)cl;

    /* 1er boundary (un preambule est tolere avant lui). */
    long pos = mp_find(body, bodyLen, dash, dashLen, 0);
    if (pos < 0) return -1;
    size_t cur = (size_t)pos + dashLen;

    int cap = 0;
    while (cur <= bodyLen) {
        /* Boundary de cloture "--boundary--" -> fin propre. */
        if (cur + 2 <= bodyLen && body[cur] == '-' && body[cur + 1] == '-') break;

        /* CRLF apres le boundary (LF nu tolere). */
        if (cur + 2 <= bodyLen && body[cur] == '\r' && body[cur + 1] == '\n') {
            cur += 2;
        } else if (cur + 1 <= bodyLen && body[cur] == '\n') {
            cur += 1;
        } else {
            goto malformed;   /* rien d'exploitable apres le boundary */
        }

        long he = mp_find(body, bodyLen, "\r\n\r\n", 4, cur);
        if (he < 0) goto malformed;                 /* en-tetes non termines */
        size_t hdrStart     = cur;
        size_t hdrEnd       = (size_t)he;           /* exclusif du CRLFCRLF */
        size_t contentStart = (size_t)he + 4;

        long nb = mp_find(body, bodyLen, crlfDash, crlfDashLen, contentStart);
        if (nb < 0) goto malformed;                 /* pas de boundary terminal */
        size_t contentEnd = (size_t)nb;

        char *name, *filename, *ctype;
        mp_parse_part_headers(body + hdrStart, hdrEnd - hdrStart,
                              &name, &filename, &ctype);
        if (name == NULL) {
            /* Partie sans nom de champ : inexploitable pour le binding. */
            free(filename);
            free(ctype);
            goto malformed;
        }
        if (mp_append_part(out, &cap, name, filename, ctype,
                           body + contentStart, contentEnd - contentStart) != 0) {
            free(name); free(filename); free(ctype);
            goto malformed;
        }

        cur = (size_t)nb + 2 + dashLen;   /* passe le CRLF + "--boundary" */
    }
    return 0;

malformed:
    owl_multipart_free(out);
    return -1;
}

void owl_multipart_free(OwlMultipartForm* form) {
    if (form == NULL || form->parts == NULL) {
        if (form) { form->parts = NULL; form->count = 0; }
        return;
    }
    for (int i = 0; i < form->count; i++) {
        free(form->parts[i].name);
        free(form->parts[i].filename);
        free(form->parts[i].contentType);
    }
    free(form->parts);
    form->parts = NULL;
    form->count = 0;
}

int owl_multipart_write_temp(const char* data, size_t len,
                             char* outPath, size_t cap) {
    const char* dir = getenv("TMPDIR");
    if (dir == NULL || *dir == '\0') dir = getenv("TEMP");
    if (dir == NULL || *dir == '\0') dir = getenv("TMP");
    if (dir == NULL || *dir == '\0') dir = ".";

    /* Nom GENERE : pid + sequence process-globale + bits de l'adresse source.
     * Aucun octet ne vient du nom client (potentiellement hostile). Slash avant
     * (accepte sous Windows comme sous POSIX ; un separateur redondant si `dir`
     * en porte deja un est tolere par l'OS). */
    static unsigned long g_seq = 0;
    unsigned long seq = ++g_seq;
    unsigned long pid = (unsigned long)OWL_MP_GETPID();
    unsigned long salt = (unsigned long)(uintptr_t)data;

    int n = snprintf(outPath, cap, "%s/owl_upload_%lu_%lu_%lx.tmp",
                     dir, pid, seq, salt);
    if (n <= 0 || (size_t)n >= cap) return -1;

    FILE* f = fopen(outPath, "wb");
    if (f == NULL) return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(outPath);
        return -1;
    }
    fclose(f);
    return 0;
}

void owl_multipart_remove_temp(const char* path) {
    if (path != NULL && path[0] != '\0') remove(path);
}
