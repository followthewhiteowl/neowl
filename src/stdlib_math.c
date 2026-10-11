// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "sentinels.h"

#include <math.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <stdio.h>
#endif

/*  : skip section compile-aware d'owl_errors.h pour eviter la
 * collision TokenType (scanner.h vs winnt.h tire par bcrypt.h Win32). */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"

static int rng_seeded = 0;

/* OS-backed CSPRNG — fills buf with len random bytes. Returns 0 on success. */
static int csprng_bytes(unsigned char* buf, size_t len) {
#ifdef _WIN32
    NTSTATUS s = BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return (s == 0) ? 0 : -1;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
#endif
}

/* Unbiased uniform integer in [0, range-1] via rejection sampling.
 * Uses CSPRNG as source. range must be > 0. */
static uint64_t csprng_range(uint64_t range) {
    /* threshold = (2^64 mod range) = smallest value that creates bias */
    uint64_t threshold = (uint64_t)(-(int64_t)range) % range;
    uint64_t r;
    do {
        csprng_bytes((unsigned char*)&r, sizeof(r));
    } while (r < threshold);
    return r % range;
}

/* Helper: get double from any numeric Value. Pour le numerique (bignum) la
 * conversion est lossy au-dela de ~17 chiffres : les natives dont le resultat
 * doit preserver la magnitude (Abs, NumériqueTronque) passent par les helpers
 * numeric_* exacts, pas par ce raccourci. */
static double to_double(Value v) {
    if (IS_INTEGER(v)) return (double)AS_INTEGER(v);
    if (IS_REAL(v)) return AS_REAL(v);
    if (IS_NUMERIC(v)) return numeric_to_double(v);
    if (IS_CURRENCY(v)) return (double)AS_CURRENCY(v) / (double)CURRENCY_SCALE;
    return 0.0;
}

/* ================================================================
 * MATH FUNCTIONS
 * ================================================================ */

/* Abs(n) */
static Value fn_abs(int argCount, Value* args) {
    (void)argCount;
    if (IS_INTEGER(args[0])) {
        int64_t v = AS_INTEGER(args[0]);
        return INTEGER_VAL(v < 0 ? -v : v);
    }
    if (IS_REAL(args[0])) return REAL_VAL(fabs(AS_REAL(args[0])));
    if (IS_NUMERIC(args[0])) return numeric_abs(args[0]);  /* magnitude preservee */
    if (IS_CURRENCY(args[0])) {
        __int128 v = AS_CURRENCY(args[0]);
        return CURRENCY_VAL(v < 0 ? -v : v);
    }
    if (IS_DURATION(args[0])) {
        int64_t ms = AS_DURATION(args[0])->totalMs;
        return OBJ_VAL(obj_new_duration(ms < 0 ? -ms : ms));
    }
    return INTEGER_VAL(0);
}

/* Arrondi demi-pair (banker's) sur double : ties-to-even, sans dependre du
 * mode FPU courant. Les cas non-tie tombent dans les branches < / > 0.5 ;
 * seul un demi exact (representable : .5 / scaled .5) declenche la regle de
 * parite. round() de la libc reste utilise pour le demi-eloigne. */
static double round_half_even(double v) {
    double f = floor(v);
    double diff = v - f;
    if (diff < 0.5) return f;
    if (diff > 0.5) return f + 1.0;
    /* tie exact : on choisit l'entier pair. */
    return (fmod(f, 2.0) == 0.0) ? f : f + 1.0;
}

/* Arrondi(valeur [, decimales [, mode]]) — demi-pair par defaut, uniforme
 * reel + numerique. mode = arrondiDemiPair (defaut) | arrondiDemiEloigne. */
static Value fn_arrondi(int argCount, Value* args) {
    int decimals = 0;
    if (argCount >= 2) decimals = (int)to_double(args[1]);

    /* 3e arg optionnel : tie-rule. Defaut = demi-pair. Tout ce qui n'est pas
     * la sentinelle demi-eloigne retombe sur demi-pair (la validation enum
     * stricte est faite au compile-time via allowedBase). */
    bool halfAway = (argCount >= 3) &&
                    ((int64_t)to_double(args[2]) == OWL_ROUND_HALF_AWAY);
    OwlRoundMode rmode = halfAway ? OWL_RND_HALF_AWAY : OWL_RND_HALF_EVEN;

    /* Numerique : arrondi exact via mpdecimal (magnitude preservee). */
    if (IS_NUMERIC(args[0])) {
        if (decimals == 0) {
            Value r = numeric_round_int(args[0], rmode);
            bool overflow = false;
            int64_t i = numeric_to_i64(r, &overflow);
            if (!overflow) return INTEGER_VAL(i);
            return r;  /* magnitude > int64 : on garde le numerique */
        }
        return numeric_round_dec(args[0], decimals, rmode);
    }

    double val = to_double(args[0]);
    if (decimals == 0) {
        double r = halfAway ? round(val) : round_half_even(val);
        return INTEGER_VAL((int64_t)r);
    }
    double factor = pow(10.0, decimals);
    double scaled = val * factor;
    double r = halfAway ? round(scaled) : round_half_even(scaled);
    return REAL_VAL(r / factor);
}

/* Plancher numerique exact a N decimales : scale par 10^dec, floor (vers -inf,
 * exact mpdecimal), unscale. Pour dec=0 c'est numeric_floor direct. */
static Value numeric_floor_dec(Value a, int decimals) {
    if (decimals <= 0) return numeric_floor(a);
    Value p = numeric_from_i64((int64_t)pow(10.0, decimals));
    Value fl = numeric_floor(numeric_mul(a, p));
    bool dz = false;
    return numeric_div(fl, p, &dz);  /* p != 0 par construction */
}

/* ArrondiInférieur(n [, decimales]) — plancher vers -inf a N decimales.
 * Distinct de PartieEntière : accepte un nombre de decimales, retour numerique. */
static Value fn_arrondi_inferieur(int argCount, Value* args) {
    int decimals = 0;
    if (argCount >= 2) decimals = (int)to_double(args[1]);

    if (IS_NUMERIC(args[0])) {
        Value fl = numeric_floor_dec(args[0], decimals);
        if (decimals <= 0) {
            bool overflow = false;
            int64_t i = numeric_to_i64(fl, &overflow);
            if (!overflow) return INTEGER_VAL(i);
        }
        return fl;  /* decimales > 0 ou magnitude > int64 : on garde le numerique */
    }

    double val = to_double(args[0]);
    if (decimals <= 0) return INTEGER_VAL((int64_t)floor(val));
    double factor = pow(10.0, decimals);
    return REAL_VAL(floor(val * factor) / factor);
}

/* ArrondiSupérieur(n [, decimales]) — plafond vers +inf a N decimales.
 * ceil(x) = -floor(-x) : reutilise la machinerie plancher pour l'exactitude. */
static Value fn_arrondi_superieur(int argCount, Value* args) {
    int decimals = 0;
    if (argCount >= 2) decimals = (int)to_double(args[1]);

    if (IS_NUMERIC(args[0])) {
        Value ce = numeric_neg(numeric_floor_dec(numeric_neg(args[0]), decimals));
        if (decimals <= 0) {
            bool overflow = false;
            int64_t i = numeric_to_i64(ce, &overflow);
            if (!overflow) return INTEGER_VAL(i);
        }
        return ce;
    }

    double val = to_double(args[0]);
    if (decimals <= 0) return INTEGER_VAL((int64_t)ceil(val));
    double factor = pow(10.0, decimals);
    return REAL_VAL(ceil(val * factor) / factor);
}

/* ArrondiAuMultiple(valeur, multiple [, mode]) — multiple le plus proche :
 * arrondi(valeur / multiple) * multiple. Departage demi-pair par defaut
 * (uniforme avec Arrondi) ; arrondiDemiEloigne reproduit le departage
 * demi-eloigne. La validation enum stricte du mode est faite au compile-time
 * via allowedBase sur signatures[0]. */
static Value fn_arrondi_multiple(int argCount, Value* args) {
    double mult = to_double(args[1]);
    if (mult == 0.0) {
        owl_emit_native_error(E_NATIVE_DIV_BY_ZERO, "ArrondiAuMultiple",
                              "native", "ArrondiAuMultiple", NULL);
        return INTEGER_VAL(0);
    }
    bool halfAway = (argCount >= 3) &&
                    ((int64_t)to_double(args[2]) == OWL_ROUND_HALF_AWAY);

    double q = to_double(args[0]) / mult;
    double r = halfAway ? round(q) : round_half_even(q);
    double res = r * mult;

    /* Entier si le resultat est entier (multiple entier) ; reel sinon
     * (multiple fractionnaire). */
    if (res == floor(res) && fabs(res) < 9.2e18)
        return INTEGER_VAL((int64_t)res);
    return REAL_VAL(res);
}

/* PartieEntière(n) — floor() per canon: PartieEntière(-3.5) = -4. */
static Value fn_partie_entiere(int argCount, Value* args) {
    (void)argCount;
    if (IS_NUMERIC(args[0])) {
        Value fl = numeric_floor(args[0]);
        bool overflow = false;
        int64_t r = numeric_to_i64(fl, &overflow);
        if (overflow) return fl;  /* magnitude > int64 : on garde le numerique */
        return INTEGER_VAL(r);
    }
    if (IS_CURRENCY(args[0])) {
        __int128 v = AS_CURRENCY(args[0]);
        __int128 q = v / CURRENCY_SCALE;
        if (v < 0 && v % CURRENCY_SCALE != 0) q -= 1;  /* floor pour negatifs */
        /* La partie entiere d'un monetaire int128 peut depasser int64 :
         * (int64_t)q serait un UB. On preserve la magnitude en numerique. */
        if (q >= (__int128)INT64_MIN && q <= (__int128)INT64_MAX)
            return INTEGER_VAL((int64_t)q);
        return numeric_from_i128(q);
    }
    double val = to_double(args[0]);
    return INTEGER_VAL((int64_t)floor(val));
}

/* NumériqueTronque(n) — truncation toward zero (OWL extension; the canon has
 * no native truncation, only floor via PartieEntière). NumériqueTronque(-3.5)
 * returns -3, not -4. Symmetric to C cast (int64_t) and Math.trunc() / Python int().
 * Non-static so stdlib_string.c can dispatch Tronque(numeric) here. */
Value fn_numerique_tronque(int argCount, Value* args);
Value fn_numerique_tronque(int argCount, Value* args) {
    (void)argCount;
    if (IS_INTEGER(args[0])) return args[0];
    if (IS_NUMERIC(args[0])) return numeric_trunc(args[0]);  /* magnitude preservee */
    if (IS_CURRENCY(args[0])) {
        __int128 q = AS_CURRENCY(args[0]) / CURRENCY_SCALE;  /* vers zero */
        /* (int64_t)q serait un UB au-dela d'int64 : on preserve la magnitude. */
        if (q >= (__int128)INT64_MIN && q <= (__int128)INT64_MAX)
            return INTEGER_VAL((int64_t)q);
        return numeric_from_i128(q);
    }
    double val = to_double(args[0]);
    return INTEGER_VAL((int64_t)val);  /* C cast = truncation toward zero */
}

/* Modulo(a, b) — native alias for the `%` operator. */
static Value fn_modulo(int argCount, Value* args) {
    (void)argCount;
    if (IS_INTEGER(args[0]) && IS_INTEGER(args[1])) {
        int64_t b = AS_INTEGER(args[1]);
        if (b == 0) {
            owl_emit_native_error(E_NATIVE_DIV_BY_ZERO, "Modulo",
                                  "native", "Modulo", NULL);
            return INTEGER_VAL(0);
        }
        return INTEGER_VAL(AS_INTEGER(args[0]) % b);
    }
    /* Numerique : reste exact via mpdecimal (preserve la magnitude). */
    if (IS_NUMERIC(args[0]) || IS_NUMERIC(args[1])) {
        bool divByZero = false;
        Value r = numeric_rem(args[0], args[1], &divByZero);
        if (divByZero) {
            owl_emit_native_error(E_NATIVE_DIV_BY_ZERO, "Modulo",
                                  "native", "Modulo", NULL);
            return INTEGER_VAL(0);
        }
        return r;
    }
    double a = to_double(args[0]);
    double b = to_double(args[1]);
    if (b == 0.0) {
        owl_emit_native_error(E_NATIVE_DIV_BY_ZERO, "Modulo",
                              "native", "Modulo", NULL);
        return INTEGER_VAL(0);
    }
    return REAL_VAL(fmod(a, b));
}

/* PartieDécimale(n) */
static Value fn_partie_decimale(int argCount, Value* args) {
    (void)argCount;
    /* Partie fractionnaire signee : x - tronque(x) (vers zero). */
    if (IS_NUMERIC(args[0])) {
        Value frac = numeric_sub(args[0], numeric_trunc(args[0]));
        return REAL_VAL(numeric_to_double(frac));
    }
    if (IS_CURRENCY(args[0])) {
        int64_t frac = (int64_t)(AS_CURRENCY(args[0]) % CURRENCY_SCALE);  /* garde le signe, < 10^6 */
        return REAL_VAL((double)frac / (double)CURRENCY_SCALE);
    }
    double val = to_double(args[0]);
    return REAL_VAL(val - (double)(int64_t)val);
}

/* InitHasard([graine]) — seed the PRNG.
 * Sans argument : graine fixe (comportement canon: séquence déterministe reproductible).
 * Avec argument : graine explicite. */
static Value fn_init_hasard(int argCount, Value* args) {
    unsigned int seed = 42;  /* canon default fixed seed */
    if (argCount >= 1) seed = (unsigned int)(int64_t)to_double(args[0]);
    srand(seed);
    rng_seeded = 1;
    return NIL_VAL;
}

/* Hasard() → réel [0,1]  /  Hasard(max) → entier [1,max]  /  Hasard(min,max) → entier [min,max]
 * Uses time-seeded PRNG (non-deterministic, non-crypto). For crypto use HasardSécurisé. */
static Value fn_hasard(int argCount, Value* args) {
    if (!rng_seeded) {
        srand((unsigned int)time(NULL));
        rng_seeded = 1;
    }
    if (argCount == 0) {
        return REAL_VAL((double)rand() / ((double)RAND_MAX + 1.0));
    }
    if (argCount == 1) {
        int64_t max = (int64_t)to_double(args[0]);
        if (max < 1) max = 1;
        return INTEGER_VAL(1 + (int64_t)(rand() % (int)max));
    }
    int64_t min = (int64_t)to_double(args[0]);
    int64_t max = (int64_t)to_double(args[1]);
    if (min > max) { int64_t t = min; min = max; max = t; }
    if (min == max) return INTEGER_VAL(min);
    int64_t range = max - min + 1;
    return INTEGER_VAL(min + (int64_t)(rand() % (int)range));
}

/* HasardSécurisé(min, max) — cryptographically secure, unbiased.
 * HasardSécurisé() → réel [0,1[ from 64-bit CSPRNG.
 * HasardSécurisé(max) → entier [1, max].
 * HasardSécurisé(min, max) → entier [min, max]. */
static Value fn_hasard_securise(int argCount, Value* args) {
    if (argCount == 0) {
        uint64_t r;
        csprng_bytes((unsigned char*)&r, sizeof(r));
        return REAL_VAL((double)(r >> 11) * (1.0 / (double)(UINT64_C(1) << 53)));
    }
    if (argCount == 1) {
        int64_t max = (int64_t)to_double(args[0]);
        if (max < 1) max = 1;
        return INTEGER_VAL(1 + (int64_t)csprng_range((uint64_t)max));
    }
    int64_t min = (int64_t)to_double(args[0]);
    int64_t max = (int64_t)to_double(args[1]);
    if (min > max) { int64_t t = min; min = max; max = t; }
    if (min == max) return INTEGER_VAL(min);
    uint64_t range = (uint64_t)(max - min + 1);
    return INTEGER_VAL(min + (int64_t)csprng_range(range));
}

/* HasardSécuriséOctets(n) — n octets cryptographiquement sûrs (CSPRNG OS) dans
 * un buffer. Réponse honnête au besoin « jeton/clé de 256 bits » :
 * HasardSécuriséOctets(32). n < 0 → soft-error arg invalide ; n == 0 → buffer
 * vide valide.  (spin-off  : pas de faux « UUID 256 bits »,
 * un vrai générateur de bytes crypto de taille arbitraire). */
static Value fn_hasard_securise_octets(int argCount, Value* args) {
    (void)argCount;
    int64_t n = (int64_t)to_double(args[0]);
    if (n < 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "HasardSecuriseOctets",
                              "nbOctets", "doit etre >= 0", NULL);
        return NIL_VAL;
    }
    ObjBuffer* b = obj_new_buffer(NULL, (int)n);
    if (n > 0 && b && b->bytes) {
        if (csprng_bytes(b->bytes, (size_t)n) != 0) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "HasardSecuriseOctets",
                                  "entropie", "source systeme indisponible", NULL);
            return NIL_VAL;
        }
    }
    return OBJ_VAL(b);
}

/* Racine(n) — square root */
static Value fn_racine(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(sqrt(to_double(args[0])));
}

/* Puissance(base, exp) */
static Value fn_puissance(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(pow(to_double(args[0]), to_double(args[1])));
}

/* Trigonometry — canon takes/returns DEGREES, not radians.
 * Doc 3050062 (Cosinus), 3050063 (Sinus), 3050064 (Tang),
 * 3050071 (ArcCosinus), 3050072 (ArcSinus), 3050073 (ArcTang),
 * 1000019879 (ArcTang2). */
#ifndef OWL_DEG2RAD
#  define OWL_DEG2RAD (3.14159265358979323846 / 180.0)
#endif
#ifndef OWL_RAD2DEG
#  define OWL_RAD2DEG (180.0 / 3.14159265358979323846)
#endif

static Value fn_cosinus(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(cos(to_double(args[0]) * OWL_DEG2RAD));
}

static Value fn_sinus(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(sin(to_double(args[0]) * OWL_DEG2RAD));
}

static Value fn_tang(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(tan(to_double(args[0]) * OWL_DEG2RAD));
}

static Value fn_arc_cosinus(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(acos(to_double(args[0])) * OWL_RAD2DEG);
}

static Value fn_arc_sinus(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(asin(to_double(args[0])) * OWL_RAD2DEG);
}

static Value fn_arc_tang(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(atan(to_double(args[0])) * OWL_RAD2DEG);
}

static Value fn_arc_tang2(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(atan2(to_double(args[0]), to_double(args[1])) * OWL_RAD2DEG);
}

/* Log(n) — canon: decimal (base 10) logarithm. */
static Value fn_log_dec(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(log10(to_double(args[0])));
}

/* Ln(n) — canon: natural logarithm (base e). */
static Value fn_ln(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(log(to_double(args[0])));
}

/* Exp(n) — canon: exponential (e^n). */
static Value fn_exp(int argCount, Value* args) {
    (void)argCount;
    return REAL_VAL(exp(to_double(args[0])));
}

/* Comparaison Min/Max sur 2 valeurs. isMin=true -> retourne la plus petite. */
static Value pick_min_max(Value a, Value b, bool isMin) {
    /* Strings : comparaison lexicographique. */
    if (IS_STRING(a) && IS_STRING(b)) {
        ObjString* sa = AS_STRING(a);
        ObjString* sb = AS_STRING(b);
        int n = sa->length < sb->length ? sa->length : sb->length;
        int cmp = memcmp(sa->chars, sb->chars, (size_t)n);
        if (cmp == 0) cmp = sa->length - sb->length;
        bool aWins = isMin ? (cmp < 0) : (cmp > 0);
        return aWins ? a : b;
    }
    /* Numerique : preserve INTEGER si les 2 sont integers. */
    if (IS_INTEGER(a) && IS_INTEGER(b)) {
        int64_t ia = AS_INTEGER(a), ib = AS_INTEGER(b);
        bool aWins = isMin ? (ia < ib) : (ia > ib);
        return aWins ? a : b;
    }
    double da = to_double(a), db = to_double(b);
    bool aWins = isMin ? (da < db) : (da > db);
    return aWins ? a : b;
}

/* Min/Max — variadic + tableau (canon).
 *   Min(a, b)               -> 2 args
 *   Min(a, b, c, ...)       -> N args (N >= 2)
 *   Min(tableau)            -> reduction sur tableau */
static Value fn_min_max_impl(int argCount, Value* args, bool isMin) {
    if (argCount == 1 && IS_ARRAY(args[0])) {
        ObjArray* arr = AS_ARRAY(args[0]);
        if (arr->elements.count == 0) return NIL_VAL;
        Value best = arr->elements.values[0];
        for (int i = 1; i < arr->elements.count; i++) {
            best = pick_min_max(best, arr->elements.values[i], isMin);
        }
        return best;
    }
    if (argCount < 2) return argCount == 1 ? args[0] : NIL_VAL;
    Value best = args[0];
    for (int i = 1; i < argCount; i++) {
        best = pick_min_max(best, args[i], isMin);
    }
    return best;
}

static Value fn_min(int argCount, Value* args) {
    return fn_min_max_impl(argCount, args, true);
}

static Value fn_max(int argCount, Value* args) {
    return fn_min_max_impl(argCount, args, false);
}

/* ================================================================
 * Registry declarations — Math family (canonical names).
 * ================================================================ */

/* modeArrondi — tie-rule pour Arrondi. 2 valeurs, bilingue, sentinel-zone. */
static const char* const E_modeArrondi_values[] = {
    "arrondiDemiPair", "arrondiDemi\xc3\x89loign\xc3\xa9",
    NULL
};
static const OwlEnum E_modeArrondi = {
    .name="modeArrondi", .nameEN="roundingMode", .category="Maths",
    .descFR="Règle d'arrondi des valeurs à mi-chemin pour Arrondi : demi-pair (banker's) ou demi-éloigné.",
    .descEN="Tie-breaking rule for Round on midpoint values: half-even (banker's) or half-away-from-zero.",
    .values=E_modeArrondi_values, .combinable=false
};
static const OwlEnum* const MATH_ENUMS[] = { &E_modeArrondi };

static const OwlConstDef MATH_CONSTS[] = {
    {"arrondiDemiPair",                  "Maths", INTEGER_VAL(OWL_ROUND_HALF_EVEN),
        "arrondit les valeurs à mi-chemin vers l'entier pair le plus proche (défaut, sans biais cumulé)",
        "rounds midpoint values to the nearest even integer (default, no cumulative bias)", "roundHalfEven"},
    {"arrondiDemi\xc3\x89loign\xc3\xa9", "Maths", INTEGER_VAL(OWL_ROUND_HALF_AWAY),
        "arrondit les valeurs à mi-chemin en s'éloignant de zéro",
        "rounds midpoint values away from zero", "roundHalfAwayFromZero"},
};

static const OwlParam p_n[]        = { {"n", "numérique", "valeur"}, {0} };
static const OwlParam p_round[]    = { {"n", "numérique", "valeur"}, {"décimales", "entier", "nombre de décimales (optionnel, défaut 0)"}, {0} };
static const OwlParam p_round3[]   = {
    {"n",         "numérique", "valeur"},
    {"décimales", "entier",    "nombre de décimales"},
    {"mode",      "entier",    "règle d'arrondi des valeurs à mi-chemin", .allowedBase=&E_modeArrondi},
    {0}
};
static const OwlParam p_minmax[]   = { {"a", "numérique", "première valeur"}, {"b", "numérique", "deuxième valeur"}, {0} };
static const OwlParam p_hasard[]   = { {"min", "entier", "borne inférieure"}, {"max", "entier", "borne supérieure"}, {0} };
static const OwlParam p_init_hasard[] = { {"graine", "entier", "graine (optionnel, défaut fixe = séquence reproductible)"}, {0} };
static const OwlParam p_puis[]     = { {"base", "numérique", "base"}, {"exposant", "numérique", "exposant"}, {0} };

static const OwlSignature sig_abs[]          = { {1, p_n,      "numérique", "|n|"}, {0} };
/*  — formes d'arité discrètes (ex-{-1} masqué), bornes alignées sur l'IMPL.
 * Arrondi(n [, décimales [, mode]]) : 1, 2 ou 3 args. La forme 3-arg (mode
 * contraint) est en premier : le compilateur lit signatures[0].params pour
 * câbler la contrainte OwlEnum du paramètre mode (allowedBase strict). */
static const OwlSignature sig_arrondi[]      = {
    {3, p_round3, "numérique", "n arrondi selon la règle de mi-chemin donnée"},
    {1, p_round,  "numérique", "n arrondi à l'entier le plus proche (demi-pair)"},
    {2, p_round,  "numérique", "n arrondi au nombre de décimales donné (demi-pair)"},
    {0}
};
/* ArrondiInférieur / ArrondiSupérieur(n [, décimales]) : 1 ou 2 args. */
static const OwlSignature sig_floor[]        = {
    {1, p_round, "numérique", "n arrondi à l'entier inférieur ou égal (vers -∞)"},
    {2, p_round, "numérique", "n arrondi à la décimale inférieure (vers -∞)"},
    {0}
};
static const OwlSignature sig_ceil[]         = {
    {1, p_round, "numérique", "n arrondi à l'entier supérieur ou égal (vers +∞)"},
    {2, p_round, "numérique", "n arrondi à la décimale supérieure (vers +∞)"},
    {0}
};
/* ArrondiAuMultiple(valeur, multiple [, mode]) : 2 ou 3 args. La forme 3-arg
 * (mode contraint) est en premier : le compilateur lit signatures[0].params
 * pour câbler la contrainte OwlEnum du paramètre mode (allowedBase strict). */
static const OwlParam p_multiple2[] = {
    {"valeur",   "numérique", "valeur à arrondir"},
    {"multiple", "numérique", "multiple cible"},
    {0}
};
static const OwlParam p_multiple3[] = {
    {"valeur",   "numérique", "valeur à arrondir"},
    {"multiple", "numérique", "multiple cible"},
    {"mode",     "entier",    "règle d'arrondi des valeurs à mi-chemin", .allowedBase=&E_modeArrondi},
    {0}
};
static const OwlSignature sig_multiple[]     = {
    {3, p_multiple3, "numérique", "valeur arrondie au multiple le plus proche selon la règle donnée"},
    {2, p_multiple2, "numérique", "valeur arrondie au multiple le plus proche (demi-pair)"},
    {0}
};
static const OwlSignature sig_pe[]           = { {1, p_n,      "entier",    "partie entière"}, {0} };
static const OwlSignature sig_pd[]           = { {1, p_n,      "réel",      "partie décimale"}, {0} };
static const OwlParam p_mod[]                = { {"a", "numérique", "dividende"}, {"b", "numérique", "diviseur"}, {0} };
static const OwlSignature sig_mod[]          = { {2, p_mod,    "numérique", "a mod b"}, {0} };
/* Hasard() | Hasard(max) | Hasard(min, max) : 0, 1 ou 2 args (doc canon). */
static const OwlSignature sig_hasard[]       = {
    {0, p_hasard, "réel",   "réel dans [0,1["},
    {1, p_hasard, "entier", "entier dans [1,max]"},
    {2, p_hasard, "entier", "entier dans [min,max]"},
    {0}
};
/* InitHasard() | InitHasard(graine) : 0 ou 1 arg. La forme sans argument
 * (graine fixe reproductible) est une extension OWL ; la doc n'expose que
 * la forme à 1 argument. */
static const OwlSignature sig_init_hasard[]  = {
    {0, p_init_hasard, "", "initialise avec une graine fixe (séquence reproductible)"},
    {1, p_init_hasard, "", "initialise avec la graine donnée"},
    {0}
};
/* HasardSécurisé() | (max) | (min, max) : 0, 1 ou 2 args. */
static const OwlSignature sig_hasard_sec[]   = {
    {0, p_hasard, "réel",   "réel dans [0,1[ (crypto-safe)"},
    {1, p_hasard, "entier", "entier dans [1,max] (crypto-safe)"},
    {2, p_hasard, "entier", "entier dans [min,max] (crypto-safe)"},
    {0}
};
/*  — HasardSécuriséOctets(n) → buffer de n octets CSPRNG. */
static const OwlParam p_hasard_octets[] = {
    {"nbOctets", "entier", "nombre d'octets à générer (≥ 0)"}, {0}
};
static const OwlSignature sig_hasard_sec_octets[] = {
    {1, p_hasard_octets, "buffer", "buffer de nbOctets octets crypto-sûrs"},
    {0}
};
static const OwlSignature sig_racine[]    = { {1, p_n,      "réel",      "√n"}, {0} };
static const OwlSignature sig_puis[]      = { {2, p_puis,   "réel",      "base^exposant"}, {0} };
static const OwlSignature sig_log[]       = { {1, p_n,      "réel",      "log₁₀(n)"}, {0} };
static const OwlSignature sig_ln[]        = { {1, p_n,      "réel",      "ln(n)"}, {0} };
static const OwlSignature sig_exp[]       = { {1, p_n,      "réel",      "e^n"}, {0} };
static const OwlParam p_minmax_tab[] = { {"tableau", "tableau", "tableau de valeurs"}, {0} };
static const OwlSignature sig_minmax[]    = {
    { -1, p_minmax,     "numérique", "plus petit/grand sur 2+ args" },
    {  1, p_minmax_tab, "numérique", "plus petit/grand sur tableau" },
    {0}
};
static const OwlParam p_angle[]           = { {"angle", "réel", "angle en degrés"}, {0} };
static const OwlParam p_ratio[]           = { {"ratio", "réel", "valeur dans [-1,+1]"}, {0} };
static const OwlSignature sig_trig[]      = { {1, p_angle, "réel", "valeur trigonométrique"}, {0} };
static const OwlSignature sig_arctrig[]   = { {1, p_ratio, "réel", "angle en degrés"}, {0} };
static const OwlParam p_yx[]              = { {"y", "réel", "ordonnée"}, {"x", "réel", "abscisse"}, {0} };
static const OwlSignature sig_atan2[]     = { {2, p_yx, "réel", "angle (degrés) du vecteur (x,y)"}, {0} };

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "Abs",
        .nameEN     = "Abs",
        .category   = "Maths",
        .descFR     = "Valeur absolue.",
        .descEN     = "Absolute value.",
        .signatures = sig_abs,
        .impl       = fn_abs,
    },
    {
        .canonical  = "Arrondi",
        .nameEN     = "Round",
        .category   = "Maths",
        .descFR     = "Arrondit un nombre au nombre de décimales spécifié (défaut : 0).",
        .descEN     = "Round to the specified number of decimals (default: 0).",
        .signatures = sig_arrondi,
        .impl       = fn_arrondi,
    },
    {
        .canonical      = "ArrondiInf\xc3\xa9rieur",  /* ArrondiInférieur */
        .canonicalAscii = "ArrondiInferieur",
        .nameEN         = "RoundFloor",
        .category       = "Maths",
        .descFR         = "Arrondit vers le bas (plancher, vers -∞) au nombre de décimales spécifié (défaut : 0).",
        .descEN         = "Round down (floor, toward -∞) to the specified number of decimals (default: 0).",
        .signatures     = sig_floor,
        .impl           = fn_arrondi_inferieur,
    },
    {
        .canonical      = "ArrondiSup\xc3\xa9rieur",  /* ArrondiSupérieur */
        .canonicalAscii = "ArrondiSuperieur",
        .nameEN         = "RoundCeil",
        .category       = "Maths",
        .descFR         = "Arrondit vers le haut (plafond, vers +∞) au nombre de décimales spécifié (défaut : 0).",
        .descEN         = "Round up (ceiling, toward +∞) to the specified number of decimals (default: 0).",
        .signatures     = sig_ceil,
        .impl           = fn_arrondi_superieur,
    },
    {
        .canonical      = "ArrondiAuMultiple",
        .nameEN         = "RoundToMultiple",
        .category       = "Maths",
        .descFR         = "Arrondit une valeur au multiple le plus proche. Départage des valeurs à mi-chemin : demi-pair par défaut, ajustable via le 3ᵉ argument.",
        .descEN         = "Round a value to the nearest multiple. Half-way ties resolved half-even by default, adjustable via the 3rd argument.",
        .signatures     = sig_multiple,
        .impl           = fn_arrondi_multiple,
    },
    {
        .canonical     = "PartieEnti\xc3\xa8re",  /* PartieEntière */
        .canonicalAscii= "PartieEntiere",
        .nameEN        = "IntegerPart",
        .category      = "Maths",
        .descFR        = "Partie entière d'un nombre (floor : arrondi vers -∞). PartieEntière(-3.5) = -4.",
        .descEN        = "Integer part of a number (floor: rounds toward -∞). IntegerPart(-3.5) = -4.",
        .signatures    = sig_pe,
        .impl          = fn_partie_entiere,
    },
    {
        .canonical     = "Num\xc3\xa9" "riqueTronque",  /* NumériqueTronque */
        .canonicalAscii= "NumeriqueTronque",
        .nameEN        = "NumericTruncate",
        .category      = "Maths",
        .descFR        = "Tronque la partie décimale d'un nombre vers zéro (NumériqueTronque(-3.5) = -3). Complément de PartieEntière (floor).",
        .descEN        = "Truncate decimal part toward zero (NumericTruncate(-3.5) = -3). Complement to IntegerPart (floor).",
        .signatures    = sig_pe,
        .impl          = fn_numerique_tronque,
    },
    {
        .canonical     = "Modulo",
        .nameEN        = "Modulo",
        .category      = "Maths",
        .descFR        = "Reste de la division entière de a par b (équivalent à a % b).",
        .descEN        = "Remainder of integer division (equivalent to a % b).",
        .signatures    = sig_mod,
        .impl          = fn_modulo,
    },
    {
        .canonical     = "PartieD\xc3\xa9""cimale",  /* PartieDécimale */
        .canonicalAscii= "PartieDecimale",
        .nameEN        = "DecimalPart",
        .category      = "Maths",
        .descFR        = "Partie décimale d'un nombre.",
        .descEN        = "Decimal part of a number.",
        .signatures    = sig_pd,
        .impl          = fn_partie_decimale,
    },
    {
        .canonical  = "InitHasard",
        .nameEN     = "InitRandom",
        .category   = "Maths",
        .descFR     = "Initialise le générateur pseudo-aléatoire. Sans argument : graine fixe (séquence reproductible). Avec argument : graine explicite.",
        .descEN     = "Initialize the pseudo-random generator. Without argument: fixed seed (reproducible sequence). With argument: explicit seed.",
        .signatures = sig_init_hasard,
        .impl       = fn_init_hasard,
    },
    {
        .canonical  = "Hasard",
        .nameEN     = "Random",
        .category   = "Maths",
        .descFR     = "Nombre aléatoire (PRNG, non-crypto). Sans argument : réel [0,1[. Un argument : entier [1,max]. Deux arguments : entier [min,max]. Utiliser HasardSécurisé pour la cryptographie.",
        .descEN     = "Random number (PRNG, non-crypto). No arg: real [0,1[. One arg: integer [1,max]. Two args: integer [min,max]. Use SecureRandom for cryptography.",
        .signatures = sig_hasard,
        .impl       = fn_hasard,
    },
    {
        .canonical      = "HasardS\xc3\xa9""curis\xc3\xa9",  /* HasardSécurisé */
        .canonicalAscii = "HasardSecurise",
        .nameEN         = "SecureRandom",
        .category       = "Maths",
        .descFR         = "Nombre aléatoire cryptographiquement sûr (CSPRNG, sans biais modulo). Sans argument : réel [0,1[. Un argument : entier [1,max]. Deux arguments : entier [min,max].",
        .descEN         = "Cryptographically secure random number (CSPRNG, no modulo bias). No arg: real [0,1[. One arg: integer [1,max]. Two args: integer [min,max].",
        .signatures     = sig_hasard_sec,
        .impl           = fn_hasard_securise,
    },
    {
        .canonical      = "HasardS\xc3\xa9""curis\xc3\xa9Octets",  /* HasardSécuriséOctets */
        .canonicalAscii = "HasardSecuriseOctets",
        .nameEN         = "SecureRandomBytes",
        .category       = "Maths",
        .descFR         = "Buffer de n octets cryptographiquement sûrs (CSPRNG, source système). Taille arbitraire : HasardSécuriséOctets(32) = 256 bits (clé/jeton), (16) = sel 128 bits. Encoder en texte transmissible via ChaîneVersBase64 / ChaîneVersHexa (qui acceptent un buffer). n = 0 → buffer vide ; n < 0 → erreur.",
        .descEN         = "Buffer of n cryptographically secure bytes (OS CSPRNG). Arbitrary size: SecureRandomBytes(32) = 256-bit key/token, (16) = 128-bit salt. Encode to a transmissible string via StringToBase64 / StringToHex (which accept a buffer). n = 0 → empty buffer; n < 0 → error.",
        .signatures     = sig_hasard_sec_octets,
        .impl           = fn_hasard_securise_octets,
    },
    {
        .canonical  = "Racine",
        .nameEN     = "Root",
        .category   = "Maths",
        .descFR     = "Racine carrée.",
        .descEN     = "Square root.",
        .signatures = sig_racine,
        .impl       = fn_racine,
    },
    {
        .canonical  = "Puissance",
        .nameEN     = "Power",
        .category   = "Maths",
        .descFR     = "base élevée à la puissance exposant.",
        .descEN     = "base raised to the power of exposant.",
        .signatures = sig_puis,
        .impl       = fn_puissance,
    },
    {
        .canonical  = "Cosinus",
        .nameEN     = "Cos",
        .category   = "Maths",
        .descFR     = "Cosinus de l'angle (en degrés).",
        .descEN     = "Cosine of the angle (degrees).",
        .signatures = sig_trig,
        .impl       = fn_cosinus,
    },
    {
        .canonical  = "Sinus",
        .nameEN     = "Sin",
        .category   = "Maths",
        .descFR     = "Sinus de l'angle (en degrés).",
        .descEN     = "Sine of the angle (degrees).",
        .signatures = sig_trig,
        .impl       = fn_sinus,
    },
    {
        .canonical  = "Tang",
        .nameEN     = "Tangent",
        .category   = "Maths",
        .descFR     = "Tangente de l'angle (en degrés).",
        .descEN     = "Tangent of the angle (degrees).",
        .signatures = sig_trig,
        .impl       = fn_tang,
    },
    {
        .canonical  = "ArcCosinus",
        .nameEN     = "ArcCos",
        .category   = "Maths",
        .descFR     = "Arc cosinus (résultat en degrés).",
        .descEN     = "Inverse cosine (result in degrees).",
        .signatures = sig_arctrig,
        .impl       = fn_arc_cosinus,
    },
    {
        .canonical  = "ArcSinus",
        .nameEN     = "ArcSin",
        .category   = "Maths",
        .descFR     = "Arc sinus (résultat en degrés).",
        .descEN     = "Inverse sine (result in degrees).",
        .signatures = sig_arctrig,
        .impl       = fn_arc_sinus,
    },
    {
        .canonical  = "ArcTang",
        .nameEN     = "ArcTan",
        .category   = "Maths",
        .descFR     = "Arc tangente (résultat en degrés).",
        .descEN     = "Inverse tangent (result in degrees).",
        .signatures = sig_arctrig,
        .impl       = fn_arc_tang,
    },
    {
        .canonical  = "ArcTang2",
        .nameEN     = "ArcTan2",
        .category   = "Maths",
        .descFR     = "Arc tangente 2 du vecteur (x, y) — résultat en degrés.",
        .descEN     = "Inverse tangent of vector (x, y) — result in degrees.",
        .signatures = sig_atan2,
        .impl       = fn_arc_tang2,
    },
    {
        .canonical  = "Log",
        .nameEN     = "Log",
        .category   = "Maths",
        .descFR     = "Logarithme décimal (base 10). Pour le log népérien utiliser Ln.",
        .descEN     = "Decimal (base 10) logarithm. For natural log use Ln.",
        .signatures = sig_log,
        .impl       = fn_log_dec,
    },
    {
        .canonical  = "Ln",
        .nameEN     = "Ln",
        .category   = "Maths",
        .descFR     = "Logarithme népérien (base e).",
        .descEN     = "Natural (base e) logarithm.",
        .signatures = sig_ln,
        .impl       = fn_ln,
    },
    {
        .canonical  = "Exp",
        .nameEN     = "Exp",
        .category   = "Maths",
        .descFR     = "Exponentielle (e^n).",
        .descEN     = "Exponential (e^n).",
        .signatures = sig_exp,
        .impl       = fn_exp,
    },
    {
        .canonical  = "Min",
        .nameEN     = "Min",
        .category   = "Maths",
        .descFR     = "Plus petite des deux valeurs.",
        .descEN     = "Smaller of two values.",
        .signatures = sig_minmax,
        .impl       = fn_min,
    },
    {
        .canonical  = "Max",
        .nameEN     = "Max",
        .category   = "Maths",
        .descFR     = "Plus grande des deux valeurs.",
        .descEN     = "Larger of two values.",
        .signatures = sig_minmax,
        .impl       = fn_max,
    },
};

void stdlib_math_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
    owl_registry_register_enums(MATH_ENUMS,
        (int)(sizeof(MATH_ENUMS) / sizeof(MATH_ENUMS[0])));
    owl_registry_register_consts(MATH_CONSTS,
        (int)(sizeof(MATH_CONSTS) / sizeof(MATH_CONSTS[0])));

    /* === Extension methods on numbers: n.Abs(), n.Arrondi(2), etc. === */
    owl_register_method(METHOD_TYPE_INTEGER, "Abs",                         fn_abs,              1);
    owl_register_method(METHOD_TYPE_INTEGER, "Arrondi",                     fn_arrondi,         -1);
    owl_register_method(METHOD_TYPE_INTEGER, "Round",                       fn_arrondi,         -1);
    owl_register_method(METHOD_TYPE_INTEGER, "ArrondiInf\xc3\xa9rieur",     fn_arrondi_inferieur, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "ArrondiInferieur",            fn_arrondi_inferieur, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "RoundFloor",                  fn_arrondi_inferieur, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "ArrondiSup\xc3\xa9rieur",     fn_arrondi_superieur, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "ArrondiSuperieur",            fn_arrondi_superieur, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "RoundCeil",                   fn_arrondi_superieur, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "ArrondiAuMultiple",           fn_arrondi_multiple, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "RoundToMultiple",             fn_arrondi_multiple, -1);
    owl_register_method(METHOD_TYPE_INTEGER, "PartieEnti\xc3\xa8re",        fn_partie_entiere,   1);
    owl_register_method(METHOD_TYPE_INTEGER, "PartieEntiere",               fn_partie_entiere,   1);
    owl_register_method(METHOD_TYPE_INTEGER, "IntegerPart",                 fn_partie_entiere,   1);
    owl_register_method(METHOD_TYPE_INTEGER, "PartieD\xc3\xa9""cimale",     fn_partie_decimale,  1);
    owl_register_method(METHOD_TYPE_INTEGER, "PartieDecimale",              fn_partie_decimale,  1);
    owl_register_method(METHOD_TYPE_INTEGER, "DecimalPart",                 fn_partie_decimale,  1);
    owl_register_method(METHOD_TYPE_INTEGER, "Tronque",                     fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_INTEGER, "Truncate",                    fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_INTEGER, "Num\xc3\xa9" "riqueTronque",  fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_INTEGER, "NumeriqueTronque",            fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_INTEGER, "Racine",                      fn_racine,           1);
    owl_register_method(METHOD_TYPE_INTEGER, "Root",                        fn_racine,           1);
    owl_register_method(METHOD_TYPE_INTEGER, "Puissance",                   fn_puissance,        2);
    owl_register_method(METHOD_TYPE_INTEGER, "Power",                       fn_puissance,        2);
    owl_register_method(METHOD_TYPE_INTEGER, "Log",                         fn_log_dec,          1);
    owl_register_method(METHOD_TYPE_INTEGER, "Ln",                          fn_ln,               1);
    owl_register_method(METHOD_TYPE_INTEGER, "Exp",                         fn_exp,              1);
    owl_register_method(METHOD_TYPE_INTEGER, "Min",                         fn_min,              2);
    owl_register_method(METHOD_TYPE_INTEGER, "Max",                         fn_max,              2);

    /* Min/Max sur tableau : terminaux d'une chaine .Filtre.Transforme.Min */
    owl_register_method(METHOD_TYPE_ARRAY,   "Min",                         fn_min,              1);
    owl_register_method(METHOD_TYPE_ARRAY,   "Max",                         fn_max,              1);

    owl_register_method(METHOD_TYPE_REAL,    "Abs",                         fn_abs,              1);
    owl_register_method(METHOD_TYPE_DURATION,"Abs",                         fn_abs,              1);
    owl_register_method(METHOD_TYPE_REAL,    "Arrondi",                     fn_arrondi,         -1);
    owl_register_method(METHOD_TYPE_REAL,    "Round",                       fn_arrondi,         -1);
    owl_register_method(METHOD_TYPE_REAL,    "ArrondiInf\xc3\xa9rieur",     fn_arrondi_inferieur, -1);
    owl_register_method(METHOD_TYPE_REAL,    "ArrondiInferieur",            fn_arrondi_inferieur, -1);
    owl_register_method(METHOD_TYPE_REAL,    "RoundFloor",                  fn_arrondi_inferieur, -1);
    owl_register_method(METHOD_TYPE_REAL,    "ArrondiSup\xc3\xa9rieur",     fn_arrondi_superieur, -1);
    owl_register_method(METHOD_TYPE_REAL,    "ArrondiSuperieur",            fn_arrondi_superieur, -1);
    owl_register_method(METHOD_TYPE_REAL,    "RoundCeil",                   fn_arrondi_superieur, -1);
    owl_register_method(METHOD_TYPE_REAL,    "ArrondiAuMultiple",           fn_arrondi_multiple, -1);
    owl_register_method(METHOD_TYPE_REAL,    "RoundToMultiple",             fn_arrondi_multiple, -1);
    owl_register_method(METHOD_TYPE_REAL,    "PartieEnti\xc3\xa8re",        fn_partie_entiere,   1);
    owl_register_method(METHOD_TYPE_REAL,    "PartieEntiere",               fn_partie_entiere,   1);
    owl_register_method(METHOD_TYPE_REAL,    "IntegerPart",                 fn_partie_entiere,   1);
    owl_register_method(METHOD_TYPE_REAL,    "PartieD\xc3\xa9""cimale",     fn_partie_decimale,  1);
    owl_register_method(METHOD_TYPE_REAL,    "PartieDecimale",              fn_partie_decimale,  1);
    owl_register_method(METHOD_TYPE_REAL,    "DecimalPart",                 fn_partie_decimale,  1);
    owl_register_method(METHOD_TYPE_REAL,    "Tronque",                     fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_REAL,    "Truncate",                    fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_REAL,    "Num\xc3\xa9" "riqueTronque",  fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_REAL,    "NumeriqueTronque",            fn_numerique_tronque, 1);
    owl_register_method(METHOD_TYPE_REAL,    "Racine",                      fn_racine,           1);
    owl_register_method(METHOD_TYPE_REAL,    "Root",                        fn_racine,           1);
    owl_register_method(METHOD_TYPE_REAL,    "Puissance",                   fn_puissance,        2);
    owl_register_method(METHOD_TYPE_REAL,    "Power",                       fn_puissance,        2);
    owl_register_method(METHOD_TYPE_REAL,    "Log",                         fn_log_dec,          1);
    owl_register_method(METHOD_TYPE_REAL,    "Ln",                          fn_ln,               1);
    owl_register_method(METHOD_TYPE_REAL,    "Exp",                         fn_exp,              1);
    owl_register_method(METHOD_TYPE_REAL,    "Min",                         fn_min,              2);
    owl_register_method(METHOD_TYPE_REAL,    "Max",                         fn_max,              2);
}
