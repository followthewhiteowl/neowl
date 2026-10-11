// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Encodeurs MIME internes.
 *   - Quoted-Printable : RFC 2045 §6.7
 *   - Mots-encodes d'en-tete : RFC 2047 (Q et B, charset UTF-8 / ISO-8859-1)
 * C pur, aucune surface OWL. Base64 emprunte a mbedtls (deja lie via
 * auth_scram.c) — pas de reimplementation. */

#include "mime_codec.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/base64.h>

/* ================================================================
 * Buffer d'octets extensible (heap). NUL-terminated a la lecture.
 * ================================================================ */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
    int    oom;   /* colle a 1 des la premiere allocation ratee */
} Buf;

static void buf_init(Buf* b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = 0; }

static int buf_reserve(Buf* b, size_t extra) {
    if (b->oom) return 0;
    size_t need = b->len + extra + 1;   /* +1 pour le \0 final */
    if (need <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    char* nd = realloc(b->data, ncap);
    if (!nd) { b->oom = 1; free(b->data); b->data = NULL; return 0; }
    b->data = nd;
    b->cap = ncap;
    return 1;
}

static void buf_putc(Buf* b, char c) {
    if (!buf_reserve(b, 1)) return;
    b->data[b->len++] = c;
}

static void buf_put(Buf* b, const char* s, size_t n) {
    if (!buf_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void buf_puts(Buf* b, const char* s) { buf_put(b, s, strlen(s)); }

/* Finalise : renvoie une chaine heap NUL-terminee, ou NULL en cas d'OOM.
 * Transfere la propriete de data a l'appelant. */
static char* buf_finish(Buf* b, size_t* out_len) {
    if (b->oom) { free(b->data); return NULL; }
    if (!buf_reserve(b, 0)) { free(b->data); return NULL; }
    b->data[b->len] = '\0';
    if (out_len) *out_len = b->len;
    return b->data ? b->data : strdup("");   /* jamais NULL pour len 0 */
}

/* ================================================================
 * Helpers hexa / UTF-8
 * ================================================================ */

static const char HEXU[] = "0123456789ABCDEF";

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Longueur du caractere UTF-8 a partir de son octet de tete (tolerant :
 * un octet de continuation ou invalide compte pour 1). */
static size_t utf8_char_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ================================================================
 * Quoted-Printable — RFC 2045 §6.7
 * ================================================================ */

/* Ligne (hors CRLF) bornee a 76 colonnes ; on declenche le soft break des
 * que le prochain jeton depasserait 75, laissant la place au '=' terminal. */
#define QP_MAX_COL 75

static void qp_emit_hex(Buf* b, unsigned char c) {
    buf_putc(b, '=');
    buf_putc(b, HEXU[c >> 4]);
    buf_putc(b, HEXU[c & 0xF]);
}

char* qp_encode(const char* src, size_t len, size_t* out_len) {
    const unsigned char* s = (const unsigned char*)src;
    Buf b; buf_init(&b);
    size_t col = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = s[i];

        /* Retour de ligne dur : \r\n ou \n seul -> CRLF, remise a zero col. */
        if (c == '\r' && i + 1 < len && s[i + 1] == '\n') {
            buf_put(&b, "\r\n", 2); col = 0; i++; continue;
        }
        if (c == '\n') { buf_put(&b, "\r\n", 2); col = 0; continue; }

        int literal;
        if (c == ' ' || c == '\t') {
            /* Espace/tab litteral sauf en fin de ligne (avant CRLF ou EOF). */
            int eol = (i + 1 >= len) || s[i + 1] == '\r' || s[i + 1] == '\n';
            literal = !eol;
        } else if (c == '=') {
            literal = 0;
        } else {
            literal = (c >= 33 && c <= 126);   /* imprimable ASCII (hors '=') */
        }

        size_t tok = literal ? 1 : 3;
        if (col + tok > QP_MAX_COL) { buf_put(&b, "=\r\n", 3); col = 0; }

        if (literal) { buf_putc(&b, (char)c); col += 1; }
        else         { qp_emit_hex(&b, c);    col += 3; }
    }

    return buf_finish(&b, out_len);
}

char* qp_decode(const char* src, size_t len, size_t* out_len) {
    Buf b; buf_init(&b);

    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '=') {
            /* Soft break "=\r\n" ou "=\n" (tolerant) : disparait. */
            if (i + 2 < len && src[i + 1] == '\r' && src[i + 2] == '\n') { i += 2; continue; }
            if (i + 1 < len && src[i + 1] == '\n') { i += 1; continue; }
            /* "=XX" : deux chiffres hexa. */
            if (i + 2 < len) {
                int hi = hex_val(src[i + 1]), lo = hex_val(src[i + 2]);
                if (hi >= 0 && lo >= 0) { buf_putc(&b, (char)((hi << 4) | lo)); i += 2; continue; }
            }
            /* Tolerant : '=' isole/invalide conserve tel quel. */
            buf_putc(&b, '=');
        } else {
            buf_putc(&b, c);
        }
    }

    return buf_finish(&b, out_len);
}

/* ================================================================
 * RFC 2047 — mots-encodes d'en-tete
 * ================================================================ */

/* Un mot-encode complet "=?UTF-8?X?...?=" ne doit pas depasser 75 car.
 * Overhead fixe "=?UTF-8?Q?" (10) + "?=" (2) = 12 -> charge utile <= 63. */
#define EW_MAX      75
#define EW_OVERHEAD 12
#define EW_PAYLOAD  (EW_MAX - EW_OVERHEAD)   /* 63 */

static int is_ascii_printable_only(const unsigned char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] < 32 || s[i] > 126) return 0;
    }
    return 1;
}

/* Q-encode d'un caractere ASCII/octet dans un mot d'en-tete (RFC 2047 §4.2).
 * Renvoie le nombre de caracteres produits (1 ou 3) et ecrit dans out. */
static size_t q_char_len(unsigned char c) {
    if (c == ' ') return 1;                     /* -> '_' */
    if (c == '=' || c == '?' || c == '_') return 3;
    if (c > 32 && c < 127) return 1;            /* imprimable ASCII sur */
    return 3;                                   /* controle / non-ASCII */
}

static void q_emit_char(Buf* b, unsigned char c) {
    if (c == ' ') { buf_putc(b, '_'); return; }
    if (c == '=' || c == '?' || c == '_' || c <= 32 || c >= 127) {
        buf_putc(b, '=');
        buf_putc(b, HEXU[c >> 4]);
        buf_putc(b, HEXU[c & 0xF]);
        return;
    }
    buf_putc(b, (char)c);
}

/* Estime le cout (en caracteres de charge utile) de Q vs B pour choisir. */
static int choose_b_encoding(const unsigned char* s, size_t len) {
    size_t qcost = 0;
    for (size_t i = 0; i < len; i++) qcost += q_char_len(s[i]);
    size_t bcost = 4 * ((len + 2) / 3);
    return bcost < qcost;   /* B seulement s'il est strictement plus compact */
}

/* Emet les mots-encodes B, en decoupant sur des frontieres UTF-8 pour tenir
 * dans EW_PAYLOAD caracteres base64 par mot. */
static void emit_b_words(Buf* b, const unsigned char* s, size_t len, int* first) {
    /* base64: 3 octets -> 4 car. Budget octets = (EW_PAYLOAD/4)*3. */
    size_t max_bytes = (EW_PAYLOAD / 4) * 3;
    size_t i = 0;
    while (i < len) {
        size_t chunk = 0;
        /* Accumule des caracteres UTF-8 entiers sans depasser max_bytes. */
        while (i + chunk < len) {
            size_t cl = utf8_char_len(s[i + chunk]);
            if (i + chunk + cl > len) cl = len - (i + chunk);   /* tolerant */
            if (chunk + cl > max_bytes) break;
            chunk += cl;
        }
        if (chunk == 0) chunk = (len - i);   /* garde-fou (caractere > budget) */

        unsigned char b64[128];
        size_t olen = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64), &olen, s + i, chunk) != 0) {
            b->oom = 1; return;
        }
        if (!*first) buf_put(b, "\r\n ", 3);
        *first = 0;
        buf_puts(b, "=?UTF-8?B?");
        buf_put(b, (const char*)b64, olen);
        buf_puts(b, "?=");
        i += chunk;
    }
}

/* Emet les mots-encodes Q, decoupage sur frontieres UTF-8, <= EW_PAYLOAD
 * caracteres de charge utile par mot. */
static void emit_q_words(Buf* b, const unsigned char* s, size_t len, int* first) {
    size_t i = 0;
    while (i < len) {
        if (!*first) buf_put(b, "\r\n ", 3);
        *first = 0;
        buf_puts(b, "=?UTF-8?Q?");
        size_t used = 0;
        while (i < len) {
            size_t cl = utf8_char_len(s[i]);
            if (i + cl > len) cl = len - i;   /* tolerant */
            /* Cout Q de ce caractere entier. */
            size_t qc = 0;
            for (size_t k = 0; k < cl; k++) qc += q_char_len(s[i + k]);
            if (used + qc > EW_PAYLOAD) break;
            for (size_t k = 0; k < cl; k++) q_emit_char(b, s[i + k]);
            used += qc;
            i += cl;
        }
        buf_puts(b, "?=");
        /* Si aucun caractere n'a pu tenir (mot vide), evite la boucle infinie. */
        if (used == 0 && i < len) {
            size_t cl = utf8_char_len(s[i]);
            if (i + cl > len) cl = len - i;
            /* Rouvre un mot dedie pour ce caractere trop gros (cas theorique). */
            for (size_t k = 0; k < cl; k++) q_emit_char(b, s[i + k]);
            i += cl;
        }
    }
}

char* rfc2047_encode(const char* src, size_t len, size_t* out_len) {
    const unsigned char* s = (const unsigned char*)src;

    /* Valeur pur-ASCII imprimable : laissee telle quelle. */
    if (is_ascii_printable_only(s, len)) {
        Buf b; buf_init(&b);
        buf_put(&b, src, len);
        return buf_finish(&b, out_len);
    }

    Buf b; buf_init(&b);
    int first = 1;
    if (choose_b_encoding(s, len)) emit_b_words(&b, s, len, &first);
    else                           emit_q_words(&b, s, len, &first);
    return buf_finish(&b, out_len);
}

/* ---------------- decode ---------------- */

/* Ecrit un code point Unicode en UTF-8 dans le buffer (pour ISO-8859-1). */
static void utf8_emit_cp(Buf* b, unsigned int cp) {
    if (cp < 0x80) {
        buf_putc(b, (char)cp);
    } else if (cp < 0x800) {
        buf_putc(b, (char)(0xC0 | (cp >> 6)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(b, (char)(0xE0 | (cp >> 12)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Vrai si le charset (insensible a la casse) est ISO-8859-1 / Latin1. */
static int charset_is_latin1(const char* cs, size_t n) {
    static const char* names[] = { "iso-8859-1", "latin1", "latin-1" };
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        size_t ln = strlen(names[k]);
        if (ln != n) continue;
        int eq = 1;
        for (size_t i = 0; i < n; i++) {
            char a = cs[i]; if (a >= 'A' && a <= 'Z') a += 32;
            if (a != names[k][i]) { eq = 0; break; }
        }
        if (eq) return 1;
    }
    return 0;
}

/* Decode la charge utile d'un mot-encode (enc = 'Q'/'q' ou 'B'/'b') vers b,
 * en tenant compte du charset (UTF-8 par defaut, ISO-8859-1 tolere). */
static void decode_payload(Buf* b, const char* text, size_t tlen,
                           char enc, int latin1) {
    if (enc == 'B' || enc == 'b') {
        unsigned char* raw = malloc(tlen + 4);
        if (!raw) { b->oom = 1; return; }
        size_t olen = 0;
        if (mbedtls_base64_decode(raw, tlen + 4, &olen,
                                  (const unsigned char*)text, tlen) == 0) {
            if (latin1) for (size_t i = 0; i < olen; i++) utf8_emit_cp(b, raw[i]);
            else        buf_put(b, (const char*)raw, olen);
        }
        free(raw);
    } else {
        /* Q-decoding : '_' -> espace, '=XX' -> octet, reste litteral. */
        for (size_t i = 0; i < tlen; i++) {
            char c = text[i];
            if (c == '_') {
                if (latin1) utf8_emit_cp(b, 0x20); else buf_putc(b, ' ');
            } else if (c == '=' && i + 2 < tlen) {
                int hi = hex_val(text[i + 1]), lo = hex_val(text[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    unsigned char v = (unsigned char)((hi << 4) | lo);
                    if (latin1) utf8_emit_cp(b, v); else buf_putc(b, (char)v);
                    i += 2;
                } else {
                    buf_putc(b, '=');
                }
            } else {
                if (latin1) utf8_emit_cp(b, (unsigned char)c); else buf_putc(b, c);
            }
        }
    }
}

/* Tente de parser un mot-encode a partir de src[i]. Renvoie 1 et avance *pi
 * jusqu'apres "?=" si succes ; 0 sinon (i inchange). */
static int try_encoded_word(Buf* b, const char* src, size_t len, size_t* pi) {
    size_t i = *pi;
    if (!(i + 1 < len && src[i] == '=' && src[i + 1] == '?')) return 0;
    size_t cs = i + 2;                          /* debut charset */
    size_t p = cs;
    while (p < len && src[p] != '?') p++;
    if (p >= len) return 0;
    size_t cs_len = p - cs;
    size_t enc_pos = p + 1;
    if (enc_pos >= len) return 0;
    char enc = src[enc_pos];
    if (enc_pos + 1 >= len || src[enc_pos + 1] != '?') return 0;
    if (enc != 'Q' && enc != 'q' && enc != 'B' && enc != 'b') return 0;
    size_t text = enc_pos + 2;                  /* debut charge utile */
    size_t t = text;
    while (t + 1 < len && !(src[t] == '?' && src[t + 1] == '=')) t++;
    if (t + 1 >= len || !(src[t] == '?' && src[t + 1] == '=')) return 0;
    size_t t_len = t - text;

    int latin1 = charset_is_latin1(src + cs, cs_len);
    decode_payload(b, src + text, t_len, enc, latin1);
    *pi = t + 2;                                /* apres "?=" */
    return 1;
}

char* rfc2047_decode(const char* src, size_t len, size_t* out_len) {
    Buf b; buf_init(&b);
    Buf ws; buf_init(&ws);      /* espaces en attente entre deux jetons */
    int last_was_word = 0;

    size_t i = 0;
    while (i < len) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            buf_putc(&ws, c);
            i++;
            continue;
        }
        /* Jeton non-blanc : mot-encode ? */
        size_t j = i;
        Buf tmp; buf_init(&tmp);
        if (try_encoded_word(&tmp, src, len, &j)) {
            /* Repli entre deux mots-encodes adjacents -> l'espace disparait. */
            if (!last_was_word && ws.len) buf_put(&b, ws.data, ws.len);
            ws.len = 0;
            if (tmp.oom) b.oom = 1;
            else if (tmp.data) buf_put(&b, tmp.data, tmp.len);
            free(tmp.data);
            i = j;
            last_was_word = 1;
        } else {
            free(tmp.data);
            /* Texte litteral : on restitue les blancs en attente. */
            if (ws.len) buf_put(&b, ws.data, ws.len);
            ws.len = 0;
            buf_putc(&b, c);
            i++;
            last_was_word = 0;
        }
    }
    /* Blancs de fin (jamais suivis d'un mot) : restitues. */
    if (ws.len) buf_put(&b, ws.data, ws.len);
    free(ws.data);

    return buf_finish(&b, out_len);
}
