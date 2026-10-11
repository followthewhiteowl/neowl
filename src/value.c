// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "value.h"
#include "memory.h"
#include "object.h"
#include "table.h"        /* table_get pour égalité UUID via field _bytes */
#include "owl_stdlib.h"   /* owl_value_is_uuid / owl_uuid_format_canonical */
#include "mpdecimal.h"    /* modele de nombre v2 : OBJ_NUMERIC bignum decimal */
#include <string.h>
#include <stdlib.h>       /* strtod for owl_real_to_str round-trip check */
#include <math.h>         /* isfinite, isnan */
#include <locale.h>       /* : localeconv pour normaliser le separateur decimal */
#include <stdarg.h>       /* owl_display_printf */

/* ================================================================
 * NUMERIC (bignum decimal, mpdecimal) — modele de nombre v2
 *
 * Deux contextes globaux, figes une fois par owl_numeric_init_context :
 *   - g_mpd_max : contexte exact (maxcontext, precision quasi illimitee).
 *     Sert aux additions/soustractions/multiplications et aux chargements
 *     (entier/reel/chaine -> mpd) qui ne doivent jamais arrondir.
 *   - g_mpd_ctx : precision de division = 50 chiffres, arrondi demi-pair.
 *     Sert a la division (÷) et au formatage par defaut.
 * Les deux ont traps=0 : les fonctions non-q (mpd_set_i64/string) ne
 * peuvent pas abort, elles posent juste un flag de status.
 * ================================================================ */

static mpd_context_t g_mpd_ctx;     /* division : 50 chiffres, HALF_EVEN */
static mpd_context_t g_mpd_max;     /* exact : +-*, chargements */
static bool g_mpd_ready = false;

void owl_numeric_init_context(void) {
    if (g_mpd_ready) return;
    mpd_init(&g_mpd_ctx, 50);
    g_mpd_ctx.round = MPD_ROUND_HALF_EVEN;
    g_mpd_ctx.traps = 0;
    mpd_maxcontext(&g_mpd_max);
    g_mpd_max.round = MPD_ROUND_HALF_EVEN;
    g_mpd_max.traps = 0;
    g_mpd_ready = true;
}

/* Charge n'importe quel nombre dans un mpd_t deja alloue, sans arrondir. */
static void load_value_mpd(Value v, mpd_t* out) {
    owl_numeric_init_context();
    uint32_t st = 0;
    if (IS_NUMERIC(v)) {
        mpd_qcopy(out, AS_NUMERIC(v)->dec, &st);
    } else if (IS_INTEGER(v)) {
        mpd_set_i64(out, AS_INTEGER(v), &g_mpd_max);
    } else if (IS_BOOL(v)) {
        mpd_set_i64(out, AS_BOOL(v) ? 1 : 0, &g_mpd_max);
    } else if (IS_NIL(v)) {
        mpd_set_i64(out, 0, &g_mpd_max);
    } else if (IS_CURRENCY(v)) {
        char b[40];
        currency_to_str(AS_CURRENCY(v), b, sizeof(b));
        mpd_set_string(out, b, &g_mpd_max);
    } else if (IS_REAL(v)) {
        char b[40];
        owl_real_to_str(AS_REAL(v), b, sizeof(b));  /* shortest round-trip décimal */
        mpd_set_string(out, b, &g_mpd_max);
    } else {
        mpd_set_i64(out, 0, &g_mpd_max);
    }
}

/* Stringifie un __int128 en décimal (buf >= 44). */
static void i128_to_str(__int128 v, char* buf) {
    bool neg = v < 0;
    unsigned __int128 u = neg ? (unsigned __int128)(-(v + 1)) + 1 : (unsigned __int128)v;
    char tmp[44];
    int n = 0;
    if (u == 0) tmp[n++] = '0';
    while (u > 0) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    int p = 0;
    if (neg) buf[p++] = '-';
    while (n > 0) buf[p++] = tmp[--n];
    buf[p] = '\0';
}

Value numeric_from_i64(int64_t v) {
    owl_numeric_init_context();
    ObjNumeric* r = obj_new_numeric();
    mpd_set_i64(r->dec, v, &g_mpd_max);
    return OBJ_VAL(r);
}

Value numeric_from_i128(__int128 v) {
    char buf[44];
    i128_to_str(v, buf);
    return numeric_from_string(buf);
}

Value numeric_from_string(const char* s) {
    owl_numeric_init_context();
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qset_string(r->dec, s, &g_mpd_max, &st);
    if (st & MPD_Conversion_syntax) {
        mpd_set_i64(r->dec, 0, &g_mpd_max);  /* invalide -> 0 */
    }
    return OBJ_VAL(r);
}

Value numeric_from_real(double v) {
    char buf[40];
    owl_real_to_str(v, buf, sizeof(buf));   /* "inf"/"-inf"/"nan" sur non-fini */
    return numeric_from_string(buf);
}

double numeric_to_double(Value n) {
    if (!IS_NUMERIC(n)) {
        if (IS_INTEGER(n))  return (double)AS_INTEGER(n);
        if (IS_CURRENCY(n)) return (double)AS_CURRENCY(n) / 1000000.0;
        if (IS_REAL(n))     return AS_REAL(n);
        return 0.0;
    }
    char* s = mpd_to_sci(AS_NUMERIC(n)->dec, 0);
    double d = s ? strtod(s, NULL) : 0.0;
    if (s) mpd_free(s);
    return d;
}

int64_t numeric_to_i64(Value n, bool* overflow) {
    owl_numeric_init_context();
    if (!IS_NUMERIC(n)) {
        if (overflow) *overflow = false;
        if (IS_INTEGER(n)) return AS_INTEGER(n);
        return (int64_t)numeric_to_double(n);
    }
    uint32_t st = 0;
    mpd_t* t = mpd_qnew();
    mpd_qtrunc(t, AS_NUMERIC(n)->dec, &g_mpd_max, &st);  /* troncature vers zéro */
    st = 0;
    int64_t r = mpd_qget_i64(t, &st);
    if (overflow) *overflow = (st & MPD_Invalid_operation) != 0;
    mpd_del(t);
    return r;
}

/* Parse une chaîne décimale entière (notation fixe, signe optionnel) en
 * __int128, détectant le débordement. Renvoie false si la magnitude dépasse
 * INT128_MAX (ou syntaxe invalide). */
static bool str_to_i128(const char* s, __int128* out) {
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;
    if (*s == '\0') return false;
    unsigned __int128 acc = 0;
    /* INT128_MAX = 2^127 - 1 */
    const unsigned __int128 MAXP = (((unsigned __int128)1) << 127) - 1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        unsigned __int128 d = (unsigned)(*s - '0');
        if (acc > (MAXP - d) / 10) return false;  /* acc*10 + d > INT128_MAX */
        acc = acc * 10 + d;
    }
    *out = neg ? -(__int128)acc : (__int128)acc;
    return true;
}

/* Conversion numérique -> __int128 mis à l'échelle monétaire (×10^6), arrondi
 * demi-pair. `overflow` signale que la magnitude dépasse la capacité d'un
 * monétaire (int128 ×10^6, soit ±~1.7×10^32). Sert au mur de fn_monetaire_coerce. */
__int128 numeric_to_currency_i128(Value n, bool* overflow) {
    owl_numeric_init_context();
    if (overflow) *overflow = false;
    mpd_t* t = mpd_qnew();
    load_value_mpd(n, t);
    uint32_t st = 0;
    mpd_t* scale = mpd_qnew();
    mpd_set_i64(scale, 1000000, &g_mpd_max);
    mpd_qmul(t, t, scale, &g_mpd_max, &st);     /* ×10^6 exact */
    mpd_del(scale);
    st = 0;
    mpd_qround_to_int(t, t, &g_mpd_ctx, &st);   /* g_mpd_ctx = HALF_EVEN */
    /* mpd entier -> texte fixe -> __int128 (mur si > INT128_MAX). */
    char* s = mpd_format(t, "f", &g_mpd_ctx);
    __int128 r = 0;
    bool ok = s ? str_to_i128(s, &r) : false;
    if (s) mpd_free(s);
    if (!ok && overflow) *overflow = true;
    mpd_del(t);
    return r;
}

void owl_numeric_free_str(char* s) {
    if (s) mpd_free(s);
}

char* numeric_format_alloc(Value n, const char* fmt) {
    owl_numeric_init_context();
    if (!IS_NUMERIC(n)) return NULL;
    mpd_t* d = AS_NUMERIC(n)->dec;
    mpd_t* tmp = NULL;
    /* "f" sans précision = forme canonique plate : on retire les zéros de
     * fin (1.50 -> 1.5, 1.0 -> 1) via reduce avant le rendu fixe. Un format
     * avec précision explicite (".50f") ne doit PAS être réduit. */
    if (strcmp(fmt, "f") == 0 && !mpd_isspecial(d)) {
        uint32_t st = 0;
        tmp = mpd_qnew();
        mpd_qcopy(tmp, d, &st);
        mpd_qreduce(tmp, tmp, &g_mpd_max, &st);
        d = tmp;
    }
    char* s = mpd_format(d, fmt, &g_mpd_ctx);
    if (tmp) mpd_del(tmp);
    return s;
}

int numeric_to_str(Value n, char* buf, int bufSize) {
    char* s = numeric_format_alloc(n, "f");
    if (!s) { if (bufSize > 0) buf[0] = '\0'; return 0; }
    int len = (int)strlen(s);
    if (len > bufSize - 1) len = bufSize - 1;
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    owl_numeric_free_str(s);
    return len;
}

Value numeric_add(Value a, Value b) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew(); mpd_t* mb = mpd_qnew();
    load_value_mpd(a, ma); load_value_mpd(b, mb);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qadd(r->dec, ma, mb, &g_mpd_max, &st);
    mpd_del(ma); mpd_del(mb);
    return OBJ_VAL(r);
}

Value numeric_sub(Value a, Value b) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew(); mpd_t* mb = mpd_qnew();
    load_value_mpd(a, ma); load_value_mpd(b, mb);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qsub(r->dec, ma, mb, &g_mpd_max, &st);
    mpd_del(ma); mpd_del(mb);
    return OBJ_VAL(r);
}

Value numeric_mul(Value a, Value b) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew(); mpd_t* mb = mpd_qnew();
    load_value_mpd(a, ma); load_value_mpd(b, mb);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qmul(r->dec, ma, mb, &g_mpd_max, &st);
    mpd_del(ma); mpd_del(mb);
    return OBJ_VAL(r);
}

Value numeric_div(Value a, Value b, bool* divByZero) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew(); mpd_t* mb = mpd_qnew();
    load_value_mpd(a, ma); load_value_mpd(b, mb);
    if (mpd_iszero(mb)) {
        if (divByZero) *divByZero = true;
        mpd_del(ma); mpd_del(mb);
        return numeric_from_i64(0);
    }
    if (divByZero) *divByZero = false;
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qdiv(r->dec, ma, mb, &g_mpd_ctx, &st);  /* 50 chiffres, demi-pair */
    mpd_del(ma); mpd_del(mb);
    return OBJ_VAL(r);
}

Value numeric_neg(Value a) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew();
    load_value_mpd(a, ma);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qcopy_negate(r->dec, ma, &st);
    mpd_del(ma);
    return OBJ_VAL(r);
}

int numeric_cmp(Value a, Value b) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew(); mpd_t* mb = mpd_qnew();
    load_value_mpd(a, ma); load_value_mpd(b, mb);
    uint32_t st = 0;
    int c = mpd_qcmp(ma, mb, &st);
    mpd_del(ma); mpd_del(mb);
    return c;
}

Value numeric_trunc(Value a) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew();
    load_value_mpd(a, ma);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qtrunc(r->dec, ma, &g_mpd_max, &st);   /* vers zero, exact */
    mpd_del(ma);
    return OBJ_VAL(r);
}

Value numeric_floor(Value a) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew();
    load_value_mpd(a, ma);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qfloor(r->dec, ma, &g_mpd_max, &st);   /* vers -inf, exact */
    mpd_del(ma);
    return OBJ_VAL(r);
}

/* Contexte d'arrondi derive de g_mpd_ctx avec la tie-rule choisie. HALF_EVEN
 * (defaut) reutilise le contexte global ; HALF_AWAY (= MPD_ROUND_HALF_UP en
 * vocabulaire mpdecimal) en est une copie locale au round demande. */
static mpd_context_t round_ctx(OwlRoundMode mode) {
    mpd_context_t c = g_mpd_ctx;
    c.round = (mode == OWL_RND_HALF_AWAY) ? MPD_ROUND_HALF_UP : MPD_ROUND_HALF_EVEN;
    return c;
}

Value numeric_round_int(Value a, OwlRoundMode mode) {
    owl_numeric_init_context();
    mpd_context_t ctx = round_ctx(mode);
    mpd_t* ma = mpd_qnew();
    load_value_mpd(a, ma);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qround_to_int(r->dec, ma, &ctx, &st);
    mpd_del(ma);
    return OBJ_VAL(r);
}

Value numeric_round_dec(Value a, int decimals, OwlRoundMode mode) {
    owl_numeric_init_context();
    mpd_context_t ctx = round_ctx(mode);
    mpd_t* ma = mpd_qnew();
    load_value_mpd(a, ma);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qrescale(r->dec, ma, (mpd_ssize_t)(-decimals), &ctx, &st);
    mpd_del(ma);
    return OBJ_VAL(r);
}

Value numeric_abs(Value a) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew();
    load_value_mpd(a, ma);
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qabs(r->dec, ma, &g_mpd_max, &st);
    mpd_del(ma);
    return OBJ_VAL(r);
}

Value numeric_rem(Value a, Value b, bool* divByZero) {
    owl_numeric_init_context();
    mpd_t* ma = mpd_qnew(); mpd_t* mb = mpd_qnew();
    load_value_mpd(a, ma); load_value_mpd(b, mb);
    if (mpd_iszero(mb)) {
        if (divByZero) *divByZero = true;
        mpd_del(ma); mpd_del(mb);
        return numeric_from_i64(0);
    }
    if (divByZero) *divByZero = false;
    ObjNumeric* r = obj_new_numeric();
    uint32_t st = 0;
    mpd_qrem(r->dec, ma, mb, &g_mpd_max, &st);  /* signe du dividende */
    mpd_del(ma); mpd_del(mb);
    return OBJ_VAL(r);
}

/* Format a currency (__int128 scaled by 10^6) to exact decimal string, trailing
 * zeros trimmed (154.490000 -> "154.49", 0 -> "0"). Shared by value_print and
 * JSON serialization so the decimal form stays canonical everywhere. */
int currency_to_str(__int128 v, char* buf, int bufSize) {
    int neg = v < 0; if (neg) v = -v;
    __int128 intPart = v / CURRENCY_SCALE;
    int64_t decPart = (int64_t)(v % CURRENCY_SCALE);   /* < 10^6 : tient en int64 */
    int decDigits = 6;
    while (decDigits > 0 && decPart % 10 == 0) { decPart /= 10; decDigits--; }
    char ibuf[44];
    i128_to_str(intPart, ibuf);   /* intPart >= 0 ici */
    if (decDigits == 0)
        return snprintf(buf, bufSize, "%s%s", neg ? "-" : "", ibuf);
    return snprintf(buf, bufSize, "%s%s.%0*lld",
                    neg ? "-" : "", ibuf, decDigits, (long long)decPart);
}

/* Shortest round-trippable double -> text.
 *
 * Mimics Python `repr(float)` / `json.dumps` behavior: produce the
 * shortest decimal text such that strtod(text) == v bit-for-bit. We use
 * the "precision ladder" approach (Steele & White inspired but without
 * vendoring Ryū / Grisu): try %.15g, verify round-trip; if not exact,
 * try %.16g; final fallback %.17g (guaranteed round-trip for any finite
 * double per IEEE-754).
 *
 * 15 digits suffices for the vast majority of decimals a human would
 * write (lat/lon, prices, pi to full double). 16-17 digits cover the
 * edge cases where the binary rounding of the input requires more
 * precision to round-trip.
 *
 * NaN/Inf are reported as their textual form ("nan", "inf", "-inf");
 * these are not valid JSON numbers — JSON serialization sites must
 * filter upstream if strict RFC 8259 compliance is required. */
int owl_real_to_str(double v, char* buf, int bufSize) {
    if (bufSize < 32) {
        /* Defensive: caller should always provide >= 32. */
        if (bufSize > 0) buf[0] = '\0';
        return 0;
    }
    if (isnan(v)) {
        int n = snprintf(buf, bufSize, "nan");
        return n;
    }
    if (!isfinite(v)) {
        int n = snprintf(buf, bufSize, v < 0 ? "-inf" : "inf");
        return n;
    }
    /* Try 15, 16, 17 significant digits — first one that round-trips wins. */
    for (int prec = 15; prec <= 17; prec++) {
        int n = snprintf(buf, bufSize, "%.*g", prec, v);
        double parsed = strtod(buf, NULL);
        /* Bit-for-bit comparison (memcmp): handles -0.0 vs 0.0 too. */
        if (memcmp(&parsed, &v, sizeof(double)) == 0) {
            /* : la serialisation interne doit rester locale-independante
             * (JSON, OOXML <v>, xls...). snprintf/strtod tournent tous deux dans
             * LC_NUMERIC courant (coherents entre eux, round-trip valide), mais
             * si le process est sous une locale a virgule decimale (fr_FR/de_DE),
             * %g emet "3,14". On normalise le separateur decimal vers '.' apres
             * coup. %g n'emet qu'UN separateur decimal et aucun separateur de
             * milliers -> un seul remplacement suffit ; e-notation (1e-07) n'a
             * pas de separateur, rien a faire. */
            struct lconv* lc = localeconv();
            char sep = (lc && lc->decimal_point) ? lc->decimal_point[0] : '.';
            if (sep && sep != '.') {
                char* p = strchr(buf, sep);
                if (p) *p = '.';
            }
            return n;
        }
    }
    /* Shouldn't reach here: %.17g round-trips any finite double. */
    return (int)strlen(buf);
}

void value_array_init(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void value_array_write(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = GROW_CAPACITY(oldCapacity);
        array->values = GROW_ARRAY(Value, array->values,
                                   oldCapacity, array->capacity);
    }
    array->values[array->count] = value;
    array->count++;
}

void value_array_free(ValueArray* array) {
    FREE_ARRAY(Value, array->values, array->capacity);
    value_array_init(array);
}

/* ================================================================
 * AFFICHAGE — puits d'ecriture partage (cf. value.h)
 * ================================================================ */

void owl_display_sink_init_buffer(OwlDisplaySink* sink) {
    sink->out = NULL;
    sink->data = NULL;
    sink->len = 0;
    sink->cap = 0;
    sink->failed = false;
    sink->depth = 0;
}

/* Garantit `extra` octets libres en plus du NUL final. Croissance
 * geometrique : la composition d'un message reste amortie a une allocation
 * meme quand chaque argument ecrit separement. */
static bool sink_reserve(OwlDisplaySink* sink, size_t extra) {
    if (sink->len + extra + 1 <= sink->cap) return true;
    size_t want = sink->cap ? sink->cap : 64;
    while (want < sink->len + extra + 1) want *= 2;
    char* grown = (char*)realloc(sink->data, want);
    if (grown == NULL) { sink->failed = true; return false; }
    sink->data = grown;
    sink->cap = want;
    return true;
}

void owl_display_write(OwlDisplaySink* sink, const char* s, size_t n) {
    if (sink->out != NULL) { fwrite(s, 1, n, sink->out); return; }
    if (!sink_reserve(sink, n)) return;
    memcpy(sink->data + sink->len, s, n);
    sink->len += n;
    sink->data[sink->len] = '\0';
}

void owl_display_putc(OwlDisplaySink* sink, char c) {
    owl_display_write(sink, &c, 1);
}

void owl_display_printf(OwlDisplaySink* sink, const char* fmt, ...) {
    va_list ap;
    if (sink->out != NULL) {
        va_start(ap, fmt);
        vfprintf(sink->out, fmt, ap);
        va_end(ap);
        return;
    }
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (!sink_reserve(sink, (size_t)n)) return;
    va_start(ap, fmt);
    vsnprintf(sink->data + sink->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sink->len += (size_t)n;
}

void value_print(Value value) {
    OwlDisplaySink sink;
    owl_display_sink_init_buffer(&sink);
    sink.out = stdout;
    value_write_display(&sink, value);
}

void value_write_display(OwlDisplaySink* sink, Value value) {
    switch (value.type) {
        case VAL_BOOL:
            /* Canon: bool in textual context is "1"/"0", not the keyword.
             * Cf. spec OWL_SPEC.md "Null = 0 = Faux = (zero-value)". */
            owl_display_printf(sink, "%s", AS_BOOL(value) ? "1" : "0");
            break;
        case VAL_NIL:
            /* Display lisible (Trace, value_print direct). Distinct du cast
             * coercitif value_to_string(NIL) = "" : ici on veut que
             * l'utilisateur VOIE le Null au lieu d'une ligne vide ambiguë. */
            owl_display_printf(sink, "Null");
            break;
        case VAL_INTEGER:
            owl_display_printf(sink, "%lld", (long long)AS_INTEGER(value));
            break;
        case VAL_REAL: {
            /* Canonical shortest round-trippable form. Cf.  +
             *  — toutes les sorties réel→texte transitent par
             * owl_real_to_str pour garantir cohérence et round-trip. */
            char buf[40];
            owl_real_to_str(AS_REAL(value), buf, sizeof(buf));
            owl_display_printf(sink, "%s", buf);
            break;
        }
        case VAL_CURRENCY: {
            char buf[32];
            currency_to_str(AS_CURRENCY(value), buf, sizeof(buf));
            owl_display_printf(sink, "%s", buf);
            break;
        }
        case VAL_OBJ:
            object_write_display(sink, value);
            break;
    }
}

bool values_equal(Value a, Value b) {
    /* Sémantique OWL : Null = "absence de valeur". Aucune valeur typée n'est
     * égale à Null par cast — seul Null = Null est vrai. C'est la sémantique
     * naturelle qui permet aux types nullables (`?` postfix) de distinguer
     * réellement "non-affecté" de "zero-value affectée".
     *
     * Divergence assumée vs canon classique qui faisait
     * Null = 0 → VRAI. Le canon date d'avant l'introduction du `?` (canon
     * 29/2024) et n'a pas été repensé pour ce cas — la zero-value rule
     * brute casse l'utilité du `?` sur entier/booléen/réel. Pour tester
     * "valeur égale à zéro", utiliser le littéral direct (`v = 0`,
     * `b = Faux`, etc.) ; pour tester "non-affecté", utiliser `EstNul(v)`
     * ou `v = Null` (sur var nullable). */
    if (IS_NIL(a) && IS_NIL(b)) return true;
    if (IS_NIL(a) || IS_NIL(b)) return false;

    /* Same type: direct comparison */
    if (a.type == b.type) {
        switch (a.type) {
            case VAL_BOOL:     return AS_BOOL(a) == AS_BOOL(b);
            case VAL_INTEGER:  return AS_INTEGER(a) == AS_INTEGER(b);
            case VAL_REAL:     return AS_REAL(a) == AS_REAL(b);
            case VAL_CURRENCY: return AS_CURRENCY(a) == AS_CURRENCY(b);
            case VAL_OBJ:
                /* Reference equality first */
                if (AS_OBJ(a) == AS_OBJ(b)) return true;
                /* Numeric bignum: compare by decimal value (not by repr). */
                if (IS_NUMERIC(a) && IS_NUMERIC(b)) return numeric_cmp(a, b) == 0;
                /* For enum/combination instances: compare by Valeur field */
                if (IS_INSTANCE(a) && IS_INSTANCE(b)) {
                    ObjInstance* ia = AS_INSTANCE(a);
                    ObjInstance* ib = AS_INSTANCE(b);
                    ObjString* valKey = obj_copy_string("Valeur", 6);
                    Value va, vb;
                    if (table_get(&ia->fields, valKey, &va) &&
                        table_get(&ib->fields, valKey, &vb)) {
                        return values_equal(va, vb);
                    }
                }
                /* Date/Time/DateTime/Duration equality */
                if (IS_DATE(a) && IS_DATE(b)) {
                    ObjDate *da = AS_DATE(a), *db = AS_DATE(b);
                    return da->year == db->year && da->month == db->month && da->day == db->day;
                }
                if (IS_TIME(a) && IS_TIME(b)) {
                    ObjTime *ta = AS_TIME(a), *tb = AS_TIME(b);
                    return ta->hour == tb->hour && ta->minute == tb->minute &&
                           ta->second == tb->second && ta->ms == tb->ms;
                }
                if (IS_DATETIME(a) && IS_DATETIME(b)) {
                    ObjDateTime *da = AS_DATETIME(a), *db = AS_DATETIME(b);
                    return da->year == db->year && da->month == db->month &&
                           da->day == db->day && da->hour == db->hour &&
                           da->minute == db->minute && da->second == db->second;
                }
                if (IS_DURATION(a) && IS_DURATION(b)) {
                    return AS_DURATION(a)->totalMs == AS_DURATION(b)->totalMs;
                }
                /* String <-> String: interned strings compare by reference
                 * (handled above). Undetectable strings bypass interning, so
                 * equal-content undetectable strings have different addresses.
                 * Compare by cleartext content via unmask helpers. */
                if (IS_STRING(a) && IS_STRING(b)) {
                    ObjString* sa = AS_STRING(a);
                    ObjString* sb = AS_STRING(b);
                    if (sa->length != sb->length) return false;
                    if (!sa->undetectable && !sb->undetectable) {
                        /* Both interned in plain table -- ref equality already
                         * handled above; if we reach here, they differ. */
                        return false;
                    }
                    char* ca = obj_string_unmask_dup(sa);
                    char* cb = obj_string_unmask_dup(sb);
                    bool eq = (memcmp(ca, cb, (size_t)sa->length) == 0);
                    obj_string_secure_free(ca, sa->length);
                    obj_string_secure_free(cb, sb->length);
                    return eq;
                }
                /* Date/Time/DateTime <-> String: compare via canonical format AAAAMMJJ[HHMMSS] */
                if (IS_DATE(a) && IS_STRING(b)) {
                    ObjDate* d = AS_DATE(a);
                    ObjString* s = AS_STRING(b);
                    char buf[16];
                    int n = snprintf(buf, sizeof(buf), "%04d%02d%02d", d->year, d->month, d->day);
                    return s->length == n && memcmp(s->chars, buf, n) == 0;
                }
                if (IS_STRING(a) && IS_DATE(b)) return values_equal(b, a);
                if (IS_TIME(a) && IS_STRING(b)) {
                    /* canonical: HHMMSSCC (8 chars, cs = centièmes). */
                    ObjTime* t = AS_TIME(a);
                    ObjString* s = AS_STRING(b);
                    char buf[16];
                    int n = snprintf(buf, sizeof(buf), "%02d%02d%02d%02d",
                                     t->hour, t->minute, t->second, t->cs);
                    return s->length == n && memcmp(s->chars, buf, n) == 0;
                }
                if (IS_STRING(a) && IS_TIME(b)) return values_equal(b, a);
                if (IS_DATETIME(a) && IS_STRING(b)) {
                    /* Canonical: AAAAMMJJHHMMSSFFF (17 chars). */
                    ObjDateTime* dt = AS_DATETIME(a);
                    ObjString* s = AS_STRING(b);
                    char buf[24];
                    int n = snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d%03d",
                                     dt->year, dt->month, dt->day,
                                     dt->hour, dt->minute, dt->second, dt->ms);
                    return s->length == n && memcmp(s->chars, buf, n) == 0;
                }
                if (IS_STRING(a) && IS_DATETIME(b)) return values_equal(b, a);
                /* UUID ↔ UUID : égalité par contenu du field privé `_bytes`
                 * (les UUID sont opaques pour le code OWL, leur égalité repose
                 * uniquement sur les bytes random + marker RFC).
                 *
                 * NOTE : pas de dépendance circulaire avec stdlib_uuid —
                 * on lit directement le field `_bytes` (convention privée
                 * documentée dans stdlib_uuid.c), pas besoin de hook
                 * externe ici. */
                if (IS_INSTANCE(a) && IS_INSTANCE(b)) {
                    ObjInstance* ia = AS_INSTANCE(a);
                    ObjInstance* ib = AS_INSTANCE(b);
                    bool aIsUuid = ia->structName != NULL
                        && ia->structName->length == 4
                        && memcmp(ia->structName->chars, "UUID", 4) == 0;
                    bool bIsUuid = ib->structName != NULL
                        && ib->structName->length == 4
                        && memcmp(ib->structName->chars, "UUID", 4) == 0;
                    if (aIsUuid && bIsUuid) {
                        ObjString* k = obj_copy_string("_bytes", 6);
                        Value va, vb;
                        if (!table_get(&ia->fields, k, &va)) return false;
                        if (!table_get(&ib->fields, k, &vb)) return false;
                        if (!IS_BUFFER(va) || !IS_BUFFER(vb)) return false;
                        ObjBuffer* ba = AS_BUFFER(va);
                        ObjBuffer* bb = AS_BUFFER(vb);
                        return ba->length == bb->length &&
                               memcmp(ba->bytes, bb->bytes, (size_t)ba->length) == 0;
                    }
                }
                /* UUID ↔ String : compare via forme canonique (36 chars
                 * dash-formatted). Permet `dbgVérifieEgalité(monUUID,
                 * "00000000-...")` et `SI monUUID = "31aad81b-..."`. Hook
                 * owl_uuid_* exposé par stdlib_uuid via owl_stdlib.h. */
                if (IS_INSTANCE(a) && IS_STRING(b) && owl_value_is_uuid(a)) {
                    ObjString* canon = owl_uuid_format_canonical(a);
                    if (canon == NULL) return false;
                    ObjString* s = AS_STRING(b);
                    return canon->length == s->length &&
                           memcmp(canon->chars, s->chars, (size_t)s->length) == 0;
                }
                if (IS_STRING(a) && IS_INSTANCE(b) && owl_value_is_uuid(b)) {
                    return values_equal(b, a);
                }
                /* Icone <-> Icone : égalité par CONTENU, pas par référence
                 *. Chaque accès `icones.<nom>` fabrique un ObjIcone
                 * frais, donc la ref-equality plus haut ne suffit pas. Deux
                 * Icône sont égales ssi même source (hash catalogue si entrée
                 * catalogue, sinon SVG brut byte-à-byte) ET mêmes props de
                 * rendu (Couleur / Rotation / Forme). couleur_fond, valide et
                 * couleur_modifiable sont dérivés, hors contrat d'égalité. */
                if (IS_ICONE(a) && IS_ICONE(b)) {
                    ObjIcone* ia = AS_ICONE(a);
                    ObjIcone* ib = AS_ICONE(b);
                    if (ia->catalog_hash != ib->catalog_hash) return false;
                    if (ia->catalog_hash == 0) {
                        /* Custom (disque / inline) : compare le SVG brut. */
                        ObjString* sa = ia->svg_raw;
                        ObjString* sb = ib->svg_raw;
                        if (sa == NULL || sb == NULL) {
                            if (sa != sb) return false;
                        } else if (sa->length != sb->length ||
                                   memcmp(sa->chars, sb->chars,
                                          (size_t)sa->length) != 0) {
                            return false;
                        }
                    }
                    return ia->couleur == ib->couleur &&
                           ia->rotation == ib->rotation &&
                           ia->forme == ib->forme;
                }
                /* Buffer ↔ String: égalité byte-à-byte (canon : Buffer et
                 * String de même contenu sont considérés égaux). */
                if (IS_BUFFER(a) && IS_STRING(b)) {
                    ObjBuffer* buf = AS_BUFFER(a);
                    ObjString* s = AS_STRING(b);
                    if (buf->length != s->length) return false;
                    if (s->undetectable) {
                        char* clear = obj_string_unmask_dup(s);
                        bool eq = (memcmp(buf->bytes, clear, (size_t)buf->length) == 0);
                        obj_string_secure_free(clear, s->length);
                        return eq;
                    }
                    return memcmp(buf->bytes, s->chars, (size_t)buf->length) == 0;
                }
                if (IS_STRING(a) && IS_BUFFER(b)) return values_equal(b, a);
                /* Buffer ↔ Buffer: byte-wise */
                if (IS_BUFFER(a) && IS_BUFFER(b)) {
                    ObjBuffer* ba = AS_BUFFER(a);
                    ObjBuffer* bb = AS_BUFFER(b);
                    return ba->length == bb->length &&
                           memcmp(ba->bytes, bb->bytes, (size_t)ba->length) == 0;
                }
                /*  : ObjMultilingue <-> String. Une chaineMultilingue
                 * dans un contexte d'egalite scalaire compare sur la langue
                 * courante (resolve via langue source en fallback). Permet
                 * `BTN..Libelle = "Aide"` (coerce auto -> ObjMultilingue cote
                 * slot UI MULTILINGUE) de rester compatible avec
                 * `dbgVerifieEgalite(BTN..Libelle, "Aide")` cote test. */
                if (IS_MULTILINGUE(a) && IS_STRING(b)) {
                    Value resolved = multilingue_resolve(AS_MULTILINGUE(a),
                                                         owl_i18n_langue_courante(),
                                                         owl_i18n_langue_source());
                    if (IS_STRING(resolved)) {
                        ObjString* sa = AS_STRING(resolved);
                        ObjString* sb = AS_STRING(b);
                        return sa->length == sb->length &&
                               memcmp(sa->chars, sb->chars, (size_t)sa->length) == 0;
                    }
                    return false;
                }
                if (IS_STRING(a) && IS_MULTILINGUE(b)) return values_equal(b, a);
                return false;
            case VAL_NIL:     return true;
        }
    }

    /* Cross-type implicit conversions (canon compatible) */
    /* Bool <-> Integer: Vrai=1, Faux=0 */
    if (IS_BOOL(a) && IS_INTEGER(b)) return (AS_BOOL(a) ? 1 : 0) == AS_INTEGER(b);
    if (IS_INTEGER(a) && IS_BOOL(b)) return AS_INTEGER(a) == (AS_BOOL(b) ? 1 : 0);
    /* Bool <-> Real */
    if (IS_BOOL(a) && IS_REAL(b)) return (AS_BOOL(a) ? 1.0 : 0.0) == AS_REAL(b);
    if (IS_REAL(a) && IS_BOOL(b)) return AS_REAL(a) == (AS_BOOL(b) ? 1.0 : 0.0);
    /* Numeric cross-type: compare exactement via mpdecimal (l'opérande non
     * numérique est promu sans perte — entier/monétaire exacts, réel via sa
     * forme décimale shortest round-trip). */
    if ((IS_NUMERIC(a) || IS_NUMERIC(b)) &&
        (IS_INTEGER(a) || IS_INTEGER(b) || IS_REAL(a) || IS_REAL(b) ||
         IS_CURRENCY(a) || IS_CURRENCY(b) || IS_NUMERIC(a) || IS_NUMERIC(b))) {
        return numeric_cmp(a, b) == 0;
    }
    /* Integer <-> Real */
    if (IS_NUMBER(a) && IS_NUMBER(b)) return AS_NUMBER(a) == AS_NUMBER(b);
    /* String <-> Integer: try to parse string as number */
    if (IS_STRING(a) && IS_INTEGER(b)) {
        int64_t v; if (value_to_integer(a, &v)) return v == AS_INTEGER(b);
    }
    if (IS_INTEGER(a) && IS_STRING(b)) {
        int64_t v; if (value_to_integer(b, &v)) return AS_INTEGER(a) == v;
    }
    /* String <-> Real */
    if (IS_STRING(a) && IS_REAL(b)) {
        double v; if (value_to_real(a, &v)) return v == AS_REAL(b);
    }
    if (IS_REAL(a) && IS_STRING(b)) {
        double v; if (value_to_real(b, &v)) return AS_REAL(a) == v;
    }
    /* String <-> Bool: "" = Faux, non-empty = Vrai */
    if (IS_STRING(a) && IS_BOOL(b)) return (AS_STRING(a)->length > 0) == AS_BOOL(b);
    if (IS_BOOL(a) && IS_STRING(b)) return AS_BOOL(a) == (AS_STRING(b)->length > 0);
    /* Durée <-> Integer: compare en ms (totalMs) */
    if (IS_DURATION(a) && IS_INTEGER(b)) return AS_DURATION(a)->totalMs == AS_INTEGER(b);
    if (IS_INTEGER(a) && IS_DURATION(b)) return AS_INTEGER(a) == AS_DURATION(b)->totalMs;

    return false;
}

bool value_to_bool(Value value, bool* result) {
    switch (value.type) {
        case VAL_BOOL:    *result = AS_BOOL(value); return true;
        case VAL_NIL:     *result = false; return true;
        case VAL_INTEGER: *result = AS_INTEGER(value) != 0; return true;
        case VAL_REAL:     *result = AS_REAL(value) != 0.0; return true;
        case VAL_CURRENCY: *result = AS_CURRENCY(value) != 0; return true;
        case VAL_OBJ:
            /* numérique nul -> Faux ; tout autre objet non-nil -> Vrai. */
            if (IS_NUMERIC(value)) { *result = !mpd_iszero(AS_NUMERIC(value)->dec); return true; }
            *result = true; return true;
    }
    return false;
}

bool value_to_integer(Value value, int64_t* result) {
    switch (value.type) {
        case VAL_INTEGER:  *result = AS_INTEGER(value); return true;
        case VAL_REAL:     *result = (int64_t)AS_REAL(value); return true;
        case VAL_BOOL:     *result = AS_BOOL(value) ? 1 : 0; return true;
        case VAL_CURRENCY: *result = (int64_t)(AS_CURRENCY(value) / CURRENCY_SCALE); return true;
        case VAL_OBJ:
            if (IS_NUMERIC(value)) {
                /* Conversion souple : troncature vers zéro. Le mur strict sur
                 * magnitude hors int64 (exception) vit dans la coercion entier
                 * (fn_entier_coerce) ; ici on renvoie la valeur tronquée. */
                *result = numeric_to_i64(value, NULL); return true;
            }
            if (IS_STRING(value)) {
                const char* str = AS_CSTRING(value);
                char* end;
                long long val = strtoll(str, &end, 10);
                if (end != str && *end == '\0') {
                    *result = (int64_t)val;
                    return true;
                }
            }
            return false;
        /* Canon : Null en context numérique = 0. */
        case VAL_NIL: *result = 0; return true;
    }
    return false;
}

bool value_to_real(Value value, double* result) {
    switch (value.type) {
        case VAL_REAL:     *result = AS_REAL(value); return true;
        case VAL_INTEGER:  *result = (double)AS_INTEGER(value); return true;
        case VAL_BOOL:     *result = AS_BOOL(value) ? 1.0 : 0.0; return true;
        case VAL_CURRENCY: *result = (double)AS_CURRENCY(value) / CURRENCY_SCALE; return true;
        case VAL_OBJ:
            if (IS_NUMERIC(value)) { *result = numeric_to_double(value); return true; }
            if (IS_STRING(value)) {
                const char* str = AS_CSTRING(value);
                char* end;
                double val = strtod(str, &end);
                if (end != str && *end == '\0') {
                    *result = val;
                    return true;
                }
            }
            return false;
        /* Canon : Null en context numérique = 0. */
        case VAL_NIL: *result = 0.0; return true;
    }
    return false;
}
