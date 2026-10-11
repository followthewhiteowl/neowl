// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_value_h
#define owl_value_h

#include "common.h"

/* Forward declaration - Obj is defined in object.h */
typedef struct Obj Obj;
typedef struct ObjString ObjString;

/* ================================================================
 * VALUE TYPE - Tagged union representing any OWL value
 * ================================================================ */

typedef enum {
    VAL_BOOL,
    VAL_NIL,
    VAL_INTEGER,
    VAL_REAL,
    VAL_CURRENCY,  /* Fixed-point: __int128 scaled by 10^6 (6 exact decimals) */
    VAL_OBJ,       /* heap-allocated: strings, arrays, numeric bignum, etc. */
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double real;
        __int128 currency;  /* value * 1,000,000 (borne ~±1.7×10^32) */
        Obj* obj;
    } as;
} Value;

/* --- Type checks --- */
#define IS_BOOL(value)     ((value).type == VAL_BOOL)
#define IS_NIL(value)      ((value).type == VAL_NIL)
#define IS_INTEGER(value)  ((value).type == VAL_INTEGER)
#define IS_REAL(value)     ((value).type == VAL_REAL)
#define IS_CURRENCY(value) ((value).type == VAL_CURRENCY)
#define IS_OBJ(value)      ((value).type == VAL_OBJ)
/* IS_NUMERIC / IS_NUMBER : definis dans object.h (dependent de OBJ_TYPE). */

/* --- Extract C value from OWL Value --- */
#define AS_BOOL(value)     ((value).as.boolean)
#define AS_INTEGER(value)  ((value).as.integer)
#define AS_REAL(value)     ((value).as.real)
#define AS_CURRENCY(value) ((value).as.currency)
#define AS_OBJ(value)      ((value).as.obj)

/* --- Wrap C value into OWL Value --- */
#define BOOL_VAL(value)     ((Value){VAL_BOOL,     {.boolean = (value)}})
#define NIL_VAL             ((Value){VAL_NIL,      {.integer = 0}})
#define INTEGER_VAL(value)  ((Value){VAL_INTEGER,   {.integer = (value)}})
#define REAL_VAL(value)     ((Value){VAL_REAL,      {.real = (value)}})
#define CURRENCY_VAL(value) ((Value){VAL_CURRENCY,  {.currency = (__int128)(value)}})
#define OBJ_VAL(object)     ((Value){VAL_OBJ,      {.obj = (Obj*)(object)}})

/* Currency scale: 1 unit = 1,000,000 internal */
#define CURRENCY_SCALE 1000000LL

/* ================================================================
 * NUMERIC (bignum decimal) API — modele de nombre v2.
 * Le type `numerique` est un OBJ_NUMERIC heap (mpd_t* via mpdecimal).
 * Toutes ces fonctions vivent dans value.c. Le contexte mpdecimal
 * global (precision division = 50, arrondi demi-pair) est initialise
 * une fois par owl_numeric_init_context() (appele depuis vm_init).
 * +-* sont exacts (maxcontext) ; / arrondit a 50 chiffres demi-pair.
 * ================================================================ */

/* Initialise les contextes mpdecimal globaux. Idempotent. */
void owl_numeric_init_context(void);

/* Constructeurs : renvoient une Value OBJ_NUMERIC fraiche. */
Value numeric_from_i64(int64_t v);
Value numeric_from_i128(__int128 v);
Value numeric_from_real(double v);       /* via la forme decimale shortest round-trip */
Value numeric_from_string(const char* s);/* parse exact ; 0 si invalide */

/* Coercions sortantes. numeric_to_double prend une Value IS_NUMERIC. */
double numeric_to_double(Value n);
/* Tronque vers zero en int64. *overflow (si non-NULL) = true si la magnitude
 * ne rentre pas dans int64 (mur cote ticket B ; ici juste le signal). */
int64_t numeric_to_i64(Value n, bool* overflow);
/* numerique -> __int128 mis a l'echelle monetaire (×10^6), arrondi demi-pair.
 * *overflow (si non-NULL) = true si la magnitude depasse la capacite monetaire
 * (int128 ×10^6, soit ±~1.7×10^32). */
__int128 numeric_to_currency_i128(Value n, bool* overflow);
/* Forme decimale plate (notation fixe, sans exposant), troncature trailing
 * zeros. Ecrit au plus bufSize-1 chars. Renvoie la longueur ecrite. */
int numeric_to_str(Value n, char* buf, int bufSize);
/* Formatage mpd_format (mini-langage Python, ex ".50f", "f"). Renvoie une
 * chaine allouee par mpdecimal a liberer via owl_numeric_free_str, ou NULL. */
char* numeric_format_alloc(Value n, const char* fmt);
void  owl_numeric_free_str(char* s);

/* Arithmetique exacte (+-*) / division 50 chiffres demi-pair. Les operandes
 * peuvent etre n'importe quel nombre (entier/reel/monetaire/numerique) ;
 * le resultat est toujours OBJ_NUMERIC. */
Value numeric_add(Value a, Value b);
Value numeric_sub(Value a, Value b);
Value numeric_mul(Value a, Value b);
Value numeric_div(Value a, Value b, bool* divByZero);
Value numeric_neg(Value a);
/* Comparaison signee : -1 / 0 / 1. Operandes = nombres quelconques. */
int   numeric_cmp(Value a, Value b);

/* Operations unaires exactes : resultat OBJ_NUMERIC, magnitude preservee
 * (jamais de transit par double). Operandes = nombres quelconques. */
Value numeric_trunc(Value a);                       /* troncature vers zero */
Value numeric_floor(Value a);                       /* arrondi vers -inf */
/* Regle de depart (tie-rule) pour les arrondis numeriques exacts. */
typedef enum {
    OWL_RND_HALF_EVEN = 0,   /* demi-pair (banker's) — defaut */
    OWL_RND_HALF_AWAY = 1     /* demi-eloigne (ties away from zero) */
} OwlRoundMode;
Value numeric_round_int(Value a, OwlRoundMode mode);               /* arrondi a l'entier */
Value numeric_round_dec(Value a, int decimals, OwlRoundMode mode); /* arrondi a N decimales */
Value numeric_abs(Value a);                         /* valeur absolue */
/* Reste de la division (signe du dividende, comme % / fmod). divByZero (si
 * non-NULL) signale b == 0 ; le retour vaut alors 0. */
Value numeric_rem(Value a, Value b, bool* divByZero);

int currency_to_str(__int128 v, char* buf, int bufSize);   /* defined in value.c */

/* Canonical real -> text serialization: shortest decimal form that
 * round-trips back to the same IEEE-754 double via strtod. Replaces all
 * legacy `%g` / `%.13g` / `%.17g` formatting sites for reals. Returns the
 * length written (excluding the NUL terminator). bufSize must be >= 32. */
int owl_real_to_str(double v, char* buf, int bufSize);
/* AS_NUMBER (coercion uniforme vers double) est defini dans object.h car il
 * depend de IS_NUMERIC (OBJ_TYPE). */

/* ================================================================
 * VALUE ARRAY - Dynamic array of values (used for constant pools)
 * ================================================================ */

typedef struct {
    int capacity;
    int count;
    Value* values;
} ValueArray;

void value_array_init(ValueArray* array);
void value_array_write(ValueArray* array, Value value);
void value_array_free(ValueArray* array);

/* ================================================================
 * VALUE OPERATIONS
 * ================================================================ */

/* ================================================================
 * AFFICHAGE — une seule logique, deux consommateurs
 * ================================================================
 *
 * La representation d'AFFICHAGE (celle que voit l'utilisateur : Trace, une
 * boite de message) est distincte de la coercion `= chaine`
 * (value_to_string) : ici Null se montre "Null" et un tableau montre son
 * contenu, la ou la coercion rend "" et "<object>".
 *
 * Un seul parcours produit les deux sorties, via ce puits d'ecriture :
 *   - out != NULL  : ecriture directe sur le flux (chemin historique de
 *                    value_print — meme octets, aucune allocation) ;
 *   - out == NULL  : accumulation dans un tampon malloc (croissance
 *                    geometrique, une allocation amortie).
 *
 * Le parcours ne retient jamais un pointeur GC entre deux ecritures : les
 * ObjString intermediaires (multilingue resolu, coercion d'un champ) sont
 * consommes immediatement, avant toute autre allocation.
 */
typedef struct {
    FILE*  out;       /* NULL => mode tampon */
    char*  data;      /* tampon NUL-termine (mode tampon) */
    size_t len;
    size_t cap;
    bool   failed;    /* allocation impossible : la sortie est tronquee */
    int    depth;     /* profondeur de recursion (tableaux, json) */
} OwlDisplaySink;

/* Au-dela, la structure est resumee par "..." plutot que de derouler la
 * pile C. Aucune donnee reelle n'atteint cette profondeur ; la garde
 * protege des structures cycliques ou pathologiques. */
#define OWL_DISPLAY_MAX_DEPTH 64

void owl_display_sink_init_buffer(OwlDisplaySink* sink);
void owl_display_write(OwlDisplaySink* sink, const char* s, size_t n);
void owl_display_putc(OwlDisplaySink* sink, char c);
void owl_display_printf(OwlDisplaySink* sink, const char* fmt, ...);

/* Ecrit la representation d'affichage de `value` dans le puits. En mode
 * tampon, `sink.data` est le resultat NUL-termine (le caller libere). */
void value_write_display(OwlDisplaySink* sink, Value value);

void value_print(Value value);
bool values_equal(Value a, Value b);

/* Implicit conversion: try to convert value to the target type.
 * Returns true on success, false if conversion impossible. */
bool value_to_bool(Value value, bool* result);
bool value_to_integer(Value value, int64_t* result);
bool value_to_real(Value value, double* result);

#endif
