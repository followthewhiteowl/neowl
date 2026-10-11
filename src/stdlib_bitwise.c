// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "sentinels.h"
#include "object.h"
#include "net_le_buffer.h"
#include "net_be_buffer.h"

#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"

#include <stdint.h>
#include <string.h>

/* Helper: extract int64 from any numeric Value. */
static int64_t to_int64(Value v) {
    if (IS_INTEGER(v)) return AS_INTEGER(v);
    if (IS_REAL(v))    return (int64_t)AS_REAL(v);
    return 0;
}

static Value fn_et_binaire(int argCount, Value* args) {
    (void)argCount;
    return INTEGER_VAL(to_int64(args[0]) & to_int64(args[1]));
}

static Value fn_ou_binaire(int argCount, Value* args) {
    (void)argCount;
    return INTEGER_VAL(to_int64(args[0]) | to_int64(args[1]));
}

static Value fn_ou_exclusif_binaire(int argCount, Value* args) {
    (void)argCount;
    return INTEGER_VAL(to_int64(args[0]) ^ to_int64(args[1]));
}

static Value fn_non_binaire(int argCount, Value* args) {
    (void)argCount;
    return INTEGER_VAL(~to_int64(args[0]));
}

/* Note : les décalages binaires (bitDécaleGauche/Droite) ne sont PAS
 * exposés comme natives. Ils existent comme opérateurs scanner+compiler
 * (TOKEN_BIT_SHL/SHR + OP_BIT_SHL/SHR) et supportent les deux syntaxes
 * canon : infix `a bitDécaleGauche b` et call `bitDécaleGauche(a, b)`. */

/* ================================================================
 * Accès direct aux bits — extension OWL, équivalent à la syntaxe canon
 * `v[n]`. Famille de méthodes prefix sur entier :
 *   n.BitLit(pos)       → 0 ou 1
 *   n.BitActive(pos)    → n avec bit allumé
 *   n.BitDesactive(pos) → n avec bit éteint
 *   n.BitInverse(pos)   → n avec bit basculé (XOR)
 * Indexation base 1, LSB d'abord (pos=1 → poids 2^0). Hors range :
 *   - BitLit retourne 0 (alignement canon)
 *   - setters retournent n inchangé
 * ================================================================ */

/* Plage acceptée : 1..64 (entier 64 bits). Hors range = false. */
static bool bit_pos_valid(int64_t pos) {
    return pos >= 1 && pos <= 64;
}

static Value fn_bit_lit(int argCount, Value* args) {
    (void)argCount;
    int64_t v   = to_int64(args[0]);
    int64_t pos = to_int64(args[1]);
    if (!bit_pos_valid(pos)) return INTEGER_VAL(0);
    return INTEGER_VAL((int64_t)(((uint64_t)v >> (pos - 1)) & 1));
}

static Value fn_bit_active(int argCount, Value* args) {
    (void)argCount;
    int64_t v   = to_int64(args[0]);
    int64_t pos = to_int64(args[1]);
    if (!bit_pos_valid(pos)) return INTEGER_VAL(v);
    return INTEGER_VAL((int64_t)((uint64_t)v | ((uint64_t)1 << (pos - 1))));
}

static Value fn_bit_desactive(int argCount, Value* args) {
    (void)argCount;
    int64_t v   = to_int64(args[0]);
    int64_t pos = to_int64(args[1]);
    if (!bit_pos_valid(pos)) return INTEGER_VAL(v);
    return INTEGER_VAL((int64_t)((uint64_t)v & ~((uint64_t)1 << (pos - 1))));
}

static Value fn_bit_inverse(int argCount, Value* args) {
    (void)argCount;
    int64_t v   = to_int64(args[0]);
    int64_t pos = to_int64(args[1]);
    if (!bit_pos_valid(pos)) return INTEGER_VAL(v);
    return INTEGER_VAL((int64_t)((uint64_t)v ^ ((uint64_t)1 << (pos - 1))));
}

/* ================================================================
 * Plage / octet — débordement explicite.
 *
 * Famille hors-canon : `entier` reste l'unique type de calcul sûr ; tout
 * débordement est opt-in via un paramètre de mode (enum entierDébordement
 * pour les plages arbitraires, octetDébordement pour la conversion octet
 * 0-255 qui ajoute débordementOctetFaible = & 0xFF).
 *
 *   débordementErreur (défaut) : lève une exception runtime catchable
 *     (QUAND EXCEPTION DANS). Pas de retour sentinelle.
 *   débordementCyclique : min + modulo_positif(valeur - min, max - min + 1)
 *   débordementBorne    : clamp vers la borne violée la plus proche
 *   débordementOctetFaible : valeur & 0xFF (octet uniquement)
 * ================================================================ */

/* Promotion soft-error → exception (pose throwPending, repris par le
 * dispatch VM dès le retour de la native). Cf. ErreurPropage / vm.c. */
extern void vm_throw_from_erreur(const char* msg, int code);

/* modulo positif : résultat dans [0, m) même pour n négatif. m > 0. */
static int64_t pos_mod(int64_t n, int64_t m) {
    int64_t r = n % m;
    if (r < 0) r += m;
    return r;
}

/* Cœur partagé : applique le mode de débordement à `valeur` dans [min, max].
 * Retourne la valeur contrainte dans *out. Renvoie false si le mode est
 * débordementErreur ET la valeur est hors plage (le caller lève alors une
 * exception via le message fourni). min <= max garanti par le caller. */
static bool apply_overflow(int64_t valeur, int64_t min, int64_t max,
                           int64_t mode, int64_t* out) {
    if (valeur >= min && valeur <= max) { *out = valeur; return true; }
    if (mode == OWL_OVERFLOW_CYCLIQUE) {
        *out = min + pos_mod(valeur - min, max - min + 1);
        return true;
    }
    if (mode == OWL_OVERFLOW_BORNE) {
        *out = (valeur < min) ? min : max;
        return true;
    }
    if (mode == OWL_OVERFLOW_OCTET_FAIBLE) {
        /* Réservé octet : le caller ne le passe que pour la plage [0,255]. */
        *out = valeur & 0xFF;
        return true;
    }
    /* débordementErreur (ou toute autre valeur) : hors plage = exception. */
    return false;
}

static Value fn_entier_dans_plage(int argCount, Value* args) {
    int64_t valeur = to_int64(args[0]);
    int64_t min    = to_int64(args[1]);
    int64_t max    = to_int64(args[2]);
    int64_t mode   = (argCount >= 4) ? to_int64(args[3]) : OWL_OVERFLOW_ERREUR;
    if (min > max) {
        vm_throw_from_erreur("EntierDansPlage : min doit être inférieur ou égal à max.", 0);
        return NIL_VAL;
    }
    int64_t out;
    if (!apply_overflow(valeur, min, max, mode, &out)) {
        vm_throw_from_erreur("EntierDansPlage : valeur hors de la plage autorisée.", 0);
        return NIL_VAL;
    }
    return INTEGER_VAL(out);
}

static Value fn_entier_vers_octet(int argCount, Value* args) {
    int64_t valeur = to_int64(args[0]);
    int64_t mode   = (argCount >= 2) ? to_int64(args[1]) : OWL_OVERFLOW_ERREUR;
    int64_t out;
    if (!apply_overflow(valeur, 0, 255, mode, &out)) {
        vm_throw_from_erreur("EntierVersOctet : valeur hors de la plage 0-255.", 0);
        return NIL_VAL;
    }
    return INTEGER_VAL(out);
}

static Value fn_buffer_lit_octet(int argCount, Value* args) {
    (void)argCount;
    if (!IS_BUFFER(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "BufferLitOctet",
                              "arg", "buf",
                              "expected", "Buffer",
                              "actual", "autre type", NULL);
        return INTEGER_VAL(0);
    }
    ObjBuffer* buf = AS_BUFFER(args[0]);
    int64_t position = to_int64(args[1]);  /* 1-based */
    if (position < 1 || position > buf->length) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "BufferLitOctet",
                              "arg", "position", NULL);
        return INTEGER_VAL(0);
    }
    return INTEGER_VAL((int64_t)buf->bytes[position - 1]);
}

static Value fn_buffer_ecrit_octet(int argCount, Value* args) {
    if (!IS_BUFFER(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "BufferÉcritOctet",
                              "arg", "buf",
                              "expected", "Buffer",
                              "actual", "autre type", NULL);
        return NIL_VAL;
    }
    ObjBuffer* buf = AS_BUFFER(args[0]);
    int64_t position = to_int64(args[1]);  /* 1-based */
    int64_t valeur   = to_int64(args[2]);
    int64_t mode     = (argCount >= 4) ? to_int64(args[3]) : OWL_OVERFLOW_ERREUR;
    if (position < 1 || position > buf->length) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "BufferÉcritOctet",
                              "arg", "position", NULL);
        return NIL_VAL;
    }
    int64_t out;
    if (!apply_overflow(valeur, 0, 255, mode, &out)) {
        vm_throw_from_erreur("BufferÉcritOctet : valeur hors de la plage 0-255.", 0);
        return NIL_VAL;
    }
    buf->bytes[position - 1] = (unsigned char)(out & 0xFF);
    return NIL_VAL;
}

/* ================================================================
 * Scalaire -> buffer binaire (fondation marshalling, F1a).
 *
 * Generalisation du modele "sandwich" : la taille fixe et le boutisme
 * sont des arguments au moment de produire les octets, jamais des
 * attributs du type entier/reel. L'ordre des octets reutilise les
 * primitives owl_le_buffer_* / owl_be_buffer_*.
 * ================================================================ */

/* Tailles d'entier acceptees (complement a deux). */
static bool int_buffer_size_valid(int64_t n) {
    return n == 1 || n == 2 || n == 4 || n == 8;
}

/* Ecrit les `nbOctets` octets de poids faible de `u` dans le bon ordre,
 * puis materialise un buffer OWL. nbOctets ∈ {1,2,4,8}. */
static Value scalar_to_buffer(uint64_t u, int64_t nbOctets, bool big) {
    Value result;
    if (big) {
        OwlBeBuffer b;
        owl_be_buffer_init(&b, 8);
        switch (nbOctets) {
            case 1: owl_be_buffer_write_u8 (&b, (uint8_t)u);  break;
            case 2: owl_be_buffer_write_u16(&b, (uint16_t)u); break;
            case 4: owl_be_buffer_write_u32(&b, (uint32_t)u); break;
            case 8: owl_be_buffer_write_u64(&b, u);           break;
        }
        result = OBJ_VAL(obj_new_buffer(b.data, (int)b.length));
        owl_be_buffer_free(&b);
    } else {
        OwlLeBuffer b;
        owl_le_buffer_init(&b, 8);
        switch (nbOctets) {
            case 1: owl_le_buffer_write_u8 (&b, (uint8_t)u);  break;
            case 2: owl_le_buffer_write_u16(&b, (uint16_t)u); break;
            case 4: owl_le_buffer_write_u32(&b, (uint32_t)u); break;
            case 8: owl_le_buffer_write_u64(&b, u);           break;
        }
        result = OBJ_VAL(obj_new_buffer(b.data, (int)b.length));
        owl_le_buffer_free(&b);
    }
    return result;
}

static Value fn_entier_vers_buffer(int argCount, Value* args) {
    int64_t n        = to_int64(args[0]);
    int64_t nbOctets = to_int64(args[1]);
    int64_t boutisme = (argCount >= 3) ? to_int64(args[2]) : OWL_ENDIAN_LITTLE;

    if (!int_buffer_size_valid(nbOctets)) {
        vm_throw_from_erreur(
            "EntierVersBuffer : nombre d'octets invalide (attendu 1, 2, 4 ou 8).", 0);
        return NIL_VAL;
    }

    /* Debordement = exception : `n` doit etre representable sur nbOctets en
     * signe OU en non-signe, soit la plage [-2^(b-1), 2^b - 1] (b = 8*nbOctets).
     * Pour 8 octets tout int64 tient (signe), pas de borne a verifier. */
    if (nbOctets < 8) {
        int     bits        = (int)(8 * nbOctets);
        int64_t signed_min  = -(INT64_C(1) << (bits - 1));
        int64_t unsigned_max = (INT64_C(1) << bits) - 1;
        if (n < signed_min || n > unsigned_max) {
            vm_throw_from_erreur(
                "EntierVersBuffer : valeur hors plage pour le nombre d'octets demande.", 0);
            return NIL_VAL;
        }
    }

    /* (uint64_t)n donne directement le complement a deux sur 64 bits ; les
     * write_uN tronquent aux octets de poids faible. */
    return scalar_to_buffer((uint64_t)n, nbOctets, boutisme == OWL_ENDIAN_BIG);
}

static Value fn_reel_vers_buffer(int argCount, Value* args) {
    double  r        = IS_REAL(args[0]) ? AS_REAL(args[0]) : (double)to_int64(args[0]);
    int64_t nbOctets = to_int64(args[1]);
    int64_t boutisme = (argCount >= 3) ? to_int64(args[2]) : OWL_ENDIAN_LITTLE;

    if (nbOctets != 4 && nbOctets != 8) {
        vm_throw_from_erreur(
            "R\xc3\xa9""elVersBuffer : nombre d'octets invalide (attendu 4 ou 8).", 0);
        return NIL_VAL;
    }

    bool big = (boutisme == OWL_ENDIAN_BIG);
    if (nbOctets == 4) {
        float    f = (float)r;
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));        /* reinterpretation IEEE 754 binary32 */
        return scalar_to_buffer((uint64_t)bits, 4, big);
    }
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));            /* reinterpretation IEEE 754 binary64 */
    return scalar_to_buffer(bits, 8, big);
}

/* BufferVide(nbOctets) — reserve nbOctets octets initialises a zero (tampon
 * de sortie pour une operation externe). nbOctets == 0 => buffer vide valide ;
 * nbOctets < 0 => exception. obj_new_buffer(NULL, n) malloc sans zeroer, d'ou
 * le memset explicite. */
static Value fn_buffer_vide(int argCount, Value* args) {
    (void)argCount;
    int64_t n = to_int64(args[0]);
    if (n < 0) {
        vm_throw_from_erreur(
            "BufferVide : nombre d'octets negatif.", 0);
        return NIL_VAL;
    }
    ObjBuffer* b = obj_new_buffer(NULL, (int)n);
    if (n > 0 && b->bytes) memset(b->bytes, 0, (size_t)n);
    return OBJ_VAL(b);
}

/* ================================================================
 * Buffer binaire -> scalaire (fondation marshalling, F1c).
 *
 * Lecteurs symetriques des …VersBuffer : relire un bloc d'octets
 * (rempli par une operation externe ou construit par concatenation)
 * en valeurs typees, par position (base 1). Reutilise les primitives
 * de lecture owl_le_buffer_* / owl_be_buffer_* en les attachant sur la
 * sous-region [position, position+nbOctets-1].
 * ================================================================ */

/* Lit nbOctets (1/2/4/8) a `position` (base 1) dans buf, ordre `big`,
 * en complement a deux si `isSigned`. Renvoie false (sans ecrire *out) si
 * la tranche deborde du buffer ou si position < 1. Le caller a deja valide
 * nbOctets ∈ {1,2,4,8}. */
static bool buffer_read_int(ObjBuffer* buf, int64_t position, int64_t nbOctets,
                            bool big, bool isSigned, int64_t* out) {
    if (position < 1) return false;
    if (position - 1 + nbOctets > (int64_t)buf->length) return false;
    const uint8_t* p = buf->bytes + (position - 1);

    if (big) {
        OwlBeBuffer b;
        owl_be_buffer_attach_read(&b, p, (size_t)nbOctets);
        if (isSigned) {
            switch (nbOctets) {
                case 1: { uint8_t v; owl_be_buffer_read_u8 (&b, &v); *out = (int8_t)v;  break; }
                case 2: { int16_t v; owl_be_buffer_read_i16(&b, &v); *out = v;          break; }
                case 4: { int32_t v; owl_be_buffer_read_i32(&b, &v); *out = v;          break; }
                case 8: { int64_t v; owl_be_buffer_read_i64(&b, &v); *out = v;          break; }
            }
        } else {
            switch (nbOctets) {
                case 1: { uint8_t  v; owl_be_buffer_read_u8 (&b, &v); *out = v;           break; }
                case 2: { uint16_t v; owl_be_buffer_read_u16(&b, &v); *out = v;           break; }
                case 4: { uint32_t v; owl_be_buffer_read_u32(&b, &v); *out = v;           break; }
                case 8: { uint64_t v; owl_be_buffer_read_u64(&b, &v); *out = (int64_t)v;  break; }
            }
        }
    } else {
        OwlLeBuffer b;
        owl_le_buffer_attach_read(&b, p, (size_t)nbOctets);
        if (isSigned) {
            switch (nbOctets) {
                case 1: { uint8_t v; owl_le_buffer_read_u8 (&b, &v); *out = (int8_t)v;  break; }
                case 2: { int16_t v; owl_le_buffer_read_i16(&b, &v); *out = v;          break; }
                case 4: { int32_t v; owl_le_buffer_read_i32(&b, &v); *out = v;          break; }
                case 8: { int64_t v; owl_le_buffer_read_i64(&b, &v); *out = v;          break; }
            }
        } else {
            switch (nbOctets) {
                case 1: { uint8_t  v; owl_le_buffer_read_u8 (&b, &v); *out = v;           break; }
                case 2: { uint16_t v; owl_le_buffer_read_u16(&b, &v); *out = v;           break; }
                case 4: { uint32_t v; owl_le_buffer_read_u32(&b, &v); *out = v;           break; }
                case 8: { uint64_t v; owl_le_buffer_read_u64(&b, &v); *out = (int64_t)v;  break; }
            }
        }
    }
    return true;
}

static Value fn_buffer_vers_entier(int argCount, Value* args) {
    if (!IS_BUFFER(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "BufferVersEntier",
                              "arg", "buffer",
                              "expected", "Buffer",
                              "actual", "autre type", NULL);
        return NIL_VAL;
    }
    ObjBuffer* buf   = AS_BUFFER(args[0]);
    int64_t position = to_int64(args[1]);                                  /* base 1 */
    int64_t nbOctets = to_int64(args[2]);
    int64_t boutisme = (argCount >= 4) ? to_int64(args[3]) : OWL_ENDIAN_LITTLE;
    bool    isSigned = true;
    if (argCount >= 5 && IS_BOOL(args[4])) isSigned = AS_BOOL(args[4]);

    if (!int_buffer_size_valid(nbOctets)) {
        vm_throw_from_erreur(
            "BufferVersEntier : nombre d'octets invalide (attendu 1, 2, 4 ou 8).", 0);
        return NIL_VAL;
    }
    int64_t result;
    if (!buffer_read_int(buf, position, nbOctets,
                         boutisme == OWL_ENDIAN_BIG, isSigned, &result)) {
        vm_throw_from_erreur(
            "BufferVersEntier : lecture hors des limites du buffer.", 0);
        return NIL_VAL;
    }
    return INTEGER_VAL(result);
}

static Value fn_buffer_vers_reel(int argCount, Value* args) {
    if (!IS_BUFFER(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "BufferVersR\xc3\xa9""el",
                              "arg", "buffer",
                              "expected", "Buffer",
                              "actual", "autre type", NULL);
        return NIL_VAL;
    }
    ObjBuffer* buf   = AS_BUFFER(args[0]);
    int64_t position = to_int64(args[1]);                                  /* base 1 */
    int64_t nbOctets = to_int64(args[2]);
    int64_t boutisme = (argCount >= 4) ? to_int64(args[3]) : OWL_ENDIAN_LITTLE;

    if (nbOctets != 4 && nbOctets != 8) {
        vm_throw_from_erreur(
            "BufferVersR\xc3\xa9""el : nombre d'octets invalide (attendu 4 ou 8).", 0);
        return NIL_VAL;
    }
    /* Les bits IEEE 754 transitent comme entier non-signe de meme largeur,
     * puis reinterpretation memcpy vers float/double. */
    int64_t bits;
    if (!buffer_read_int(buf, position, nbOctets,
                         boutisme == OWL_ENDIAN_BIG, false, &bits)) {
        vm_throw_from_erreur(
            "BufferVersR\xc3\xa9""el : lecture hors des limites du buffer.", 0);
        return NIL_VAL;
    }
    if (nbOctets == 4) {
        uint32_t u = (uint32_t)bits;
        float    f;
        memcpy(&f, &u, sizeof(f));              /* reinterpretation IEEE 754 binary32 */
        return REAL_VAL((double)f);
    }
    uint64_t u = (uint64_t)bits;
    double   d;
    memcpy(&d, &u, sizeof(d));                  /* reinterpretation IEEE 754 binary64 */
    return REAL_VAL(d);
}

/* ================================================================
 * Registry declarations — bitwise operations.
 * Canonical names: ETBinaire / OUBinaire /
 * OUExclusifBinaire / NONBinaire (doc TraitementDonnees/Donnees/Conversions).
 * ================================================================ */

static const OwlParam p_ab[] = {
    {"a", "entier", "premier opérande"},
    {"b", "entier", "deuxième opérande"},
    {0}
};
static const OwlParam p_a[] = {
    {"a", "entier", "valeur"},
    {0}
};

static const OwlSignature sig_et[]  = { {2, p_ab, "entier", "ET bit-à-bit de a et b"},  {0} };
static const OwlSignature sig_ou[]  = { {2, p_ab, "entier", "OU bit-à-bit de a et b"},  {0} };
static const OwlSignature sig_xor[] = { {2, p_ab, "entier", "OU exclusif bit-à-bit"},   {0} };
static const OwlSignature sig_not[] = { {1, p_a,  "entier", "complément à un de a"},    {0} };

static const OwlParam p_bit_read[] = {
    {"valeur",   "entier", "entier source"},
    {"position", "entier", "position du bit (1..64, base 1, LSB d'abord)"},
    {0}
};
static const OwlSignature sig_bit_read[] = {
    {2, p_bit_read, "entier", "0 ou 1 (0 si position hors range)"},
    {0}
};
static const OwlSignature sig_bit_write[] = {
    {2, p_bit_read, "entier", "nouvelle valeur (entier inchangé si position hors range)"},
    {0}
};

static const OwlMethodBinding MB_BIT_LIT[] = {
    {"entier", "BitLit",      2},
    {"entier", "BitRead",     2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_BIT_ACTIVE[] = {
    {"entier", "BitActive",   2},
    {"entier", "BitEnable",   2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_BIT_DESACTIVE[] = {
    {"entier", "BitDesactive", 2},
    {"entier", "BitDisable",   2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_BIT_INVERSE[] = {
    {"entier", "BitInverse",  2},
    {"entier", "BitFlip",     2},
    {NULL, NULL, 0}
};

/* -- Débordement : 4 constantes partagées + 2 enums ---- */

/* Enum plage : pas de débordementOctetFaible (concept 8 bits). */
static const char* const E_entierDebordement_values[] = {
    "d\xc3\xa9""bordementErreur", "d\xc3\xa9""bordementCyclique",
    "d\xc3\xa9""bordementBorne",
    NULL
};
static const OwlEnum E_entierDebordement = {
    .name="entierD\xc3\xa9""bordement", .nameAscii="entierDebordement",
    .nameEN="intOverflow", .category="Conversion",
    .descFR="Mode de débordement de EntierDansPlage : erreur, repli cyclique ou clamp.",
    .descEN="Overflow mode for IntegerInRange: error, cyclic wrap or clamp.",
    .values=E_entierDebordement_values, .combinable=false
};

/* Enum octet : ajoute débordementOctetFaible (& 0xFF). */
static const char* const E_octetDebordement_values[] = {
    "d\xc3\xa9""bordementErreur", "d\xc3\xa9""bordementCyclique",
    "d\xc3\xa9""bordementBorne", "d\xc3\xa9""bordementOctetFaible",
    NULL
};
static const OwlEnum E_octetDebordement = {
    .name="octetD\xc3\xa9""bordement", .nameAscii="octetDebordement",
    .nameEN="byteOverflow", .category="Conversion",
    .descFR="Mode de débordement de EntierVersOctet / BufferÉcritOctet : erreur, cyclique, clamp ou 8 bits faibles.",
    .descEN="Overflow mode for IntegerToByte / BufferWriteByte: error, cyclic, clamp or low 8 bits.",
    .values=E_octetDebordement_values, .combinable=false
};

static const OwlConstDef DEBORDEMENT_CONSTS[] = {
    {"d\xc3\xa9""bordementErreur",      "Conversion", INTEGER_VAL(OWL_OVERFLOW_ERREUR),       "lève une exception si la valeur est hors plage (défaut)", "raises an exception if the value is out of range (default)", "overflowError"},
    {"d\xc3\xa9""bordementCyclique",    "Conversion", INTEGER_VAL(OWL_OVERFLOW_CYCLIQUE),     "repli modulo positif à l'intérieur de la plage",          "positive modulo wrap inside the range", "overflowWrap"},
    {"d\xc3\xa9""bordementBorne",       "Conversion", INTEGER_VAL(OWL_OVERFLOW_BORNE),        "clamp vers la borne violée la plus proche",                "clamps to the nearest violated bound", "overflowClamp"},
    {"d\xc3\xa9""bordementOctetFaible", "Conversion", INTEGER_VAL(OWL_OVERFLOW_OCTET_FAIBLE), "garde les 8 bits faibles (valeur & 0xFF, octet uniquement)", "keeps the low 8 bits (value & 0xFF, byte only)", "overflowLowByte"},
};

static const OwlEnum* const DEBORDEMENT_ENUMS[] = {
    &E_entierDebordement, &E_octetDebordement
};

/* -- Boutisme : ordre des octets en sortie ------------- */

static const char* const E_boutisme_values[] = {
    "boutismePetit", "boutismeGros",
    NULL
};
static const OwlEnum E_boutisme = {
    .name="boutisme", .nameAscii="boutisme", .nameEN="endianness",
    .category="Conversion",
    .descFR="Ordre des octets pour EntierVersBuffer / RéelVersBuffer : petit-boutiste (poids faible en premier, défaut) ou grand-boutiste (poids fort en premier).",
    .descEN="Byte order for IntegerToBuffer / RealToBuffer: little-endian (least significant byte first, default) or big-endian (most significant byte first).",
    .values=E_boutisme_values, .combinable=false
};

static const OwlConstDef BOUTISME_CONSTS[] = {
    {"boutismePetit", "Conversion", INTEGER_VAL(OWL_ENDIAN_LITTLE), "octets de poids faible en premier (petit-boutiste, défaut)", "least significant byte first (little-endian, default)", "littleEndian"},
    {"boutismeGros",  "Conversion", INTEGER_VAL(OWL_ENDIAN_BIG),    "octets de poids fort en premier (grand-boutiste)",          "most significant byte first (big-endian)", "bigEndian"},
};

static const OwlEnum* const BOUTISME_ENUMS[] = {
    &E_boutisme
};

static const OwlParam p_dans_plage3[] = {
    {"valeur", "entier", "valeur à contraindre"},
    {"min",    "entier", "borne basse incluse"},
    {"max",    "entier", "borne haute incluse"},
    {0}
};
static const OwlParam p_dans_plage4[] = {
    {"valeur", "entier", "valeur à contraindre"},
    {"min",    "entier", "borne basse incluse"},
    {"max",    "entier", "borne haute incluse"},
    {"mode",   "entier", "mode de débordement", .allowedBase=&E_entierDebordement},
    {0}
};
/* Forme 4-arg (avec mode contraint) en premier : le compilateur lit
 * signatures[0].params pour câbler la contrainte OwlEnum du paramètre mode. */
static const OwlSignature sig_dans_plage[] = {
    {4, p_dans_plage4, "entier", "valeur contrainte selon le mode de débordement"},
    {3, p_dans_plage3, "entier", "valeur contrainte (exception si hors plage en mode erreur)"},
    {0}
};

static const OwlParam p_vers_octet1[] = {
    {"valeur", "entier", "valeur à convertir en octet"},
    {0}
};
static const OwlParam p_vers_octet2[] = {
    {"valeur", "entier", "valeur à convertir en octet"},
    {"mode",   "entier", "mode de débordement", .allowedBase=&E_octetDebordement},
    {0}
};
static const OwlSignature sig_vers_octet[] = {
    {2, p_vers_octet2, "entier", "octet 0-255 selon le mode de débordement"},
    {1, p_vers_octet1, "entier", "octet 0-255 (exception si hors plage en mode erreur)"},
    {0}
};

static const OwlParam p_lit_octet[] = {
    {"buf",      "buffer", "buffer source"},
    {"position", "entier", "position de l'octet (base 1)"},
    {0}
};
static const OwlSignature sig_lit_octet[] = {
    {2, p_lit_octet, "entier", "octet 0-255 (erreur si position hors buffer)"},
    {0}
};

static const OwlParam p_ecrit_octet3[] = {
    {"buf",      "buffer", "buffer cible"},
    {"position", "entier", "position de l'octet (base 1)"},
    {"valeur",   "entier", "valeur d'octet à écrire (0-255)"},
    {0}
};
static const OwlParam p_ecrit_octet4[] = {
    {"buf",      "buffer", "buffer cible"},
    {"position", "entier", "position de l'octet (base 1)"},
    {"valeur",   "entier", "valeur d'octet à écrire"},
    {"mode",     "entier", "mode de débordement", .allowedBase=&E_octetDebordement},
    {0}
};
static const OwlSignature sig_ecrit_octet[] = {
    {4, p_ecrit_octet4, "rien", "écrit l'octet selon le mode de débordement"},
    {3, p_ecrit_octet3, "rien", "écrit l'octet (exception si hors 0-255 en mode erreur)"},
    {0}
};

static const OwlMethodBinding MB_DANS_PLAGE[] = {
    {"entier", "DansPlage", -1},
    {"entier", "InRange",   -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_VERS_OCTET[] = {
    {"entier", "VersOctet", -1},
    {"entier", "ToByte",    -1},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_LIT_OCTET[] = {
    {"buffer", "LitOctet",  2},
    {"buffer", "ReadByte",  2},
    {NULL, NULL, 0}
};
static const OwlMethodBinding MB_ECRIT_OCTET[] = {
    {"buffer", "\xc3\x89""critOctet", -1},  /* ÉcritOctet */
    {"buffer", "EcritOctet",          -1},  /* alias sans accent */
    {"buffer", "WriteByte",           -1},
    {NULL, NULL, 0}
};

/* -- Scalaire -> buffer -------------------------------- */

/* Forme 3-arg (boutisme contraint) en premier : le compilateur lit
 * signatures[0].params pour cabler la contrainte OwlEnum du parametre. */
static const OwlParam p_entier_vers_buffer3[] = {
    {"valeur",   "entier", "entier à convertir en octets"},
    {"nbOctets", "entier", "taille en octets (1, 2, 4 ou 8)"},
    {"boutisme", "entier", "ordre des octets (défaut boutismePetit)", .allowedBase=&E_boutisme},
    {0}
};
static const OwlParam p_entier_vers_buffer2[] = {
    {"valeur",   "entier", "entier à convertir en octets"},
    {"nbOctets", "entier", "taille en octets (1, 2, 4 ou 8)"},
    {0}
};
static const OwlSignature sig_entier_vers_buffer[] = {
    {3, p_entier_vers_buffer3, "buffer", "buffer des octets de la valeur, ordre selon le boutisme"},
    {2, p_entier_vers_buffer2, "buffer", "buffer des octets de la valeur (petit-boutiste par défaut)"},
    {0}
};

static const OwlParam p_reel_vers_buffer3[] = {
    {"valeur",   "réel",   "réel à convertir en octets IEEE 754"},
    {"nbOctets", "entier", "taille en octets (4 = simple précision, 8 = double)"},
    {"boutisme", "entier", "ordre des octets (défaut boutismePetit)", .allowedBase=&E_boutisme},
    {0}
};
static const OwlParam p_reel_vers_buffer2[] = {
    {"valeur",   "réel",   "réel à convertir en octets IEEE 754"},
    {"nbOctets", "entier", "taille en octets (4 = simple précision, 8 = double)"},
    {0}
};
static const OwlSignature sig_reel_vers_buffer[] = {
    {3, p_reel_vers_buffer3, "buffer", "buffer IEEE 754 de la valeur, ordre selon le boutisme"},
    {2, p_reel_vers_buffer2, "buffer", "buffer IEEE 754 de la valeur (petit-boutiste par défaut)"},
    {0}
};

static const OwlParam p_buffer_vide[] = {
    {"nbOctets", "entier", "nombre d'octets à réserver (tous initialisés à zéro)"},
    {0}
};
static const OwlSignature sig_buffer_vide[] = {
    {1, p_buffer_vide, "buffer", "buffer de nbOctets octets initialisés à zéro"},
    {0}
};

/* -- Buffer -> scalaire -------------------------------- */

/* Tableaux par arité : la forme la plus riche (boutisme contraint) en
 * premier pour câbler la contrainte OwlEnum du paramètre boutisme. */
static const OwlParam p_buffer_vers_entier5[] = {
    {"buffer",   "buffer", "buffer source"},
    {"position", "entier", "position de lecture (base 1)"},
    {"nbOctets", "entier", "nombre d'octets à lire (1, 2, 4 ou 8)"},
    {"boutisme", "entier", "ordre des octets (défaut boutismePetit)", .allowedBase=&E_boutisme},
    {"signé",    "booléen", "Vrai (défaut) = complément à deux ; Faux = non-signé"},
    {0}
};
static const OwlParam p_buffer_vers_entier4[] = {
    {"buffer",   "buffer", "buffer source"},
    {"position", "entier", "position de lecture (base 1)"},
    {"nbOctets", "entier", "nombre d'octets à lire (1, 2, 4 ou 8)"},
    {"boutisme", "entier", "ordre des octets (défaut boutismePetit)", .allowedBase=&E_boutisme},
    {0}
};
static const OwlParam p_buffer_vers_entier3[] = {
    {"buffer",   "buffer", "buffer source"},
    {"position", "entier", "position de lecture (base 1)"},
    {"nbOctets", "entier", "nombre d'octets à lire (1, 2, 4 ou 8)"},
    {0}
};
static const OwlSignature sig_buffer_vers_entier[] = {
    {5, p_buffer_vers_entier5, "entier", "valeur entière lue (signe selon le paramètre signé)"},
    {4, p_buffer_vers_entier4, "entier", "valeur entière lue (complément à deux par défaut)"},
    {3, p_buffer_vers_entier3, "entier", "valeur entière lue (petit-boutiste, signé par défaut)"},
    {0}
};

static const OwlParam p_buffer_vers_reel4[] = {
    {"buffer",   "buffer", "buffer source"},
    {"position", "entier", "position de lecture (base 1)"},
    {"nbOctets", "entier", "nombre d'octets à lire (4 = simple précision, 8 = double)"},
    {"boutisme", "entier", "ordre des octets (défaut boutismePetit)", .allowedBase=&E_boutisme},
    {0}
};
static const OwlParam p_buffer_vers_reel3[] = {
    {"buffer",   "buffer", "buffer source"},
    {"position", "entier", "position de lecture (base 1)"},
    {"nbOctets", "entier", "nombre d'octets à lire (4 = simple précision, 8 = double)"},
    {0}
};
static const OwlSignature sig_buffer_vers_reel[] = {
    {4, p_buffer_vers_reel4, "réel", "réel IEEE 754 lu, ordre selon le boutisme"},
    {3, p_buffer_vers_reel3, "réel", "réel IEEE 754 lu (petit-boutiste par défaut)"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "ETBinaire",
        .nameEN     = "BinaryAND",
        .category   = "Binaire",
        .descFR     = "ET logique (bit à bit) entre deux entiers.",
        .descEN     = "Bitwise AND between two integers.",
        .signatures = sig_et,
        .impl       = fn_et_binaire,
    },
    {
        .canonical  = "OUBinaire",
        .nameEN     = "BinaryOR",
        .category   = "Binaire",
        .descFR     = "OU logique (bit à bit) entre deux entiers.",
        .descEN     = "Bitwise OR between two integers.",
        .signatures = sig_ou,
        .impl       = fn_ou_binaire,
    },
    {
        .canonical  = "OUExclusifBinaire",
        .nameEN     = "BinaryXOR",
        .category   = "Binaire",
        .descFR     = "OU exclusif (XOR) bit à bit entre deux entiers.",
        .descEN     = "Bitwise XOR between two integers.",
        .signatures = sig_xor,
        .impl       = fn_ou_exclusif_binaire,
    },
    {
        .canonical  = "NONBinaire",
        .nameEN     = "BinaryNOT",
        .category   = "Binaire",
        .descFR     = "NON logique (complément à un) — inversion de tous les bits.",
        .descEN     = "Bitwise NOT (one's complement).",
        .signatures = sig_not,
        .impl       = fn_non_binaire,
    },
    /* Accès direct aux bits (extension OWL équivalente à canon `v[n]`). */
    {
        .canonical      = "BitLit",
        .nameEN         = "BitRead",
        .category       = "Binaire",
        .descFR         = "Lit la valeur du bit à la position indiquée (base 1, LSB d'abord). Retourne 0 ou 1.",
        .descEN         = "Reads the bit value at the given position (1-based, LSB first). Returns 0 or 1.",
        .signatures     = sig_bit_read,
        .impl           = fn_bit_lit,
        .methodBindings = MB_BIT_LIT,
    },
    {
        .canonical      = "BitActive",
        .nameEN         = "BitEnable",
        .category       = "Binaire",
        .descFR         = "Renvoie l'entier avec le bit à la position indiquée mis à 1 (base 1, LSB d'abord).",
        .descEN         = "Returns the integer with the bit at the given position set to 1 (1-based, LSB first).",
        .signatures     = sig_bit_write,
        .impl           = fn_bit_active,
        .methodBindings = MB_BIT_ACTIVE,
    },
    {
        .canonical      = "BitDesactive",
        .nameEN         = "BitDisable",
        .category       = "Binaire",
        .descFR         = "Renvoie l'entier avec le bit à la position indiquée mis à 0 (base 1, LSB d'abord).",
        .descEN         = "Returns the integer with the bit at the given position cleared to 0 (1-based, LSB first).",
        .signatures     = sig_bit_write,
        .impl           = fn_bit_desactive,
        .methodBindings = MB_BIT_DESACTIVE,
    },
    {
        .canonical      = "BitInverse",
        .nameEN         = "BitFlip",
        .category       = "Binaire",
        .descFR         = "Renvoie l'entier avec le bit à la position indiquée basculé (XOR avec 2^(pos-1)).",
        .descEN         = "Returns the integer with the bit at the given position toggled (XOR with 2^(pos-1)).",
        .signatures     = sig_bit_write,
        .impl           = fn_bit_inverse,
        .methodBindings = MB_BIT_INVERSE,
    },
    /* Plage / octet — débordement explicite. */
    {
        .canonical      = "EntierDansPlage",
        .nameEN         = "IntegerInRange",
        .category       = "Conversion",
        .descFR         = "Contraint un entier à la plage [min, max] inclusive. Mode par défaut : lève une exception si la valeur est hors plage. Modes alternatifs : repli cyclique ou clamp.",
        .descEN         = "Constrains an integer to the inclusive [min, max] range. Default mode raises an exception when out of range. Alternate modes: cyclic wrap or clamp.",
        .signatures     = sig_dans_plage,
        .impl           = fn_entier_dans_plage,
        .methodBindings = MB_DANS_PLAGE,
    },
    {
        .canonical      = "EntierVersOctet",
        .nameEN         = "IntegerToByte",
        .category       = "Conversion",
        .descFR         = "Convertit un entier en valeur d'octet (0-255). Mode par défaut : lève une exception si la valeur est hors plage. Modes alternatifs : cyclique, clamp ou conservation des 8 bits faibles.",
        .descEN         = "Converts an integer to a byte value (0-255). Default mode raises an exception when out of range. Alternate modes: cyclic, clamp or keep the low 8 bits.",
        .signatures     = sig_vers_octet,
        .impl           = fn_entier_vers_octet,
        .methodBindings = MB_VERS_OCTET,
    },
    {
        .canonical      = "BufferLitOctet",
        .nameEN         = "BufferReadByte",
        .category       = "Buffer",
        .descFR         = "Lit l'octet à la position indiquée (base 1) d'un buffer et renvoie sa valeur en entier 0-255. Erreur si la position est hors du buffer.",
        .descEN         = "Reads the byte at the given 1-based position of a buffer and returns its value as an integer 0-255. Error if the position is outside the buffer.",
        .signatures     = sig_lit_octet,
        .impl           = fn_buffer_lit_octet,
        .methodBindings = MB_LIT_OCTET,
    },
    {
        .canonical      = "Buffer\xc3\x89""critOctet",
        .canonicalAscii = "BufferEcritOctet",
        .nameEN         = "BufferWriteByte",
        .category       = "Buffer",
        .descFR         = "Écrit un octet (valeur 0-255) à la position indiquée (base 1) d'un buffer. Mode par défaut : lève une exception si la valeur est hors plage. Erreur si la position est hors du buffer.",
        .descEN         = "Writes a byte (value 0-255) at the given 1-based position of a buffer. Default mode raises an exception when the value is out of range. Error if the position is outside the buffer.",
        .signatures     = sig_ecrit_octet,
        .impl           = fn_buffer_ecrit_octet,
        .methodBindings = MB_ECRIT_OCTET,
    },
    /* Scalaire -> buffer binaire (fondation marshalling, ). */
    {
        .canonical  = "EntierVersBuffer",
        .nameEN     = "IntegerToBuffer",
        .category   = "Conversion",
        .descFR     = "Convertit un entier en buffer binaire de taille fixe (1, 2, 4 ou 8 octets), en complément à deux. L'ordre des octets est petit-boutiste par défaut ou grand-boutiste. Lève une exception si la valeur ne tient pas dans la taille demandée ou si la taille n'est pas 1, 2, 4 ou 8.",
        .descEN     = "Converts an integer to a fixed-size binary buffer (1, 2, 4 or 8 bytes), in two's complement. Byte order is little-endian by default or big-endian. Raises an exception if the value does not fit the requested size or if the size is not 1, 2, 4 or 8.",
        .signatures = sig_entier_vers_buffer,
        .impl       = fn_entier_vers_buffer,
    },
    {
        .canonical  = "R\xc3\xa9""elVersBuffer",
        .canonicalAscii = "ReelVersBuffer",
        .nameEN     = "RealToBuffer",
        .category   = "Conversion",
        .descFR     = "Convertit un réel en buffer binaire IEEE 754 : 4 octets en simple précision, 8 octets en double précision. L'ordre des octets est petit-boutiste par défaut ou grand-boutiste. Lève une exception si la taille n'est pas 4 ou 8.",
        .descEN     = "Converts a real to an IEEE 754 binary buffer: 4 bytes for single precision, 8 bytes for double precision. Byte order is little-endian by default or big-endian. Raises an exception if the size is not 4 or 8.",
        .signatures = sig_reel_vers_buffer,
        .impl       = fn_reel_vers_buffer,
    },
    {
        .canonical  = "BufferVide",
        .nameEN     = "EmptyBuffer",
        .category   = "Buffer",
        .descFR     = "Réserve un buffer de nbOctets octets tous initialisés à zéro, destiné à recevoir le résultat d'une opération qui le remplit. nbOctets égal à 0 retourne un buffer vide valide. Lève une exception si nbOctets est négatif.",
        .descEN     = "Reserves a buffer of nbOctets bytes all initialised to zero, intended to receive the result of an operation that fills it. A nbOctets of 0 returns a valid empty buffer. Raises an exception if nbOctets is negative.",
        .signatures = sig_buffer_vide,
        .impl       = fn_buffer_vide,
    },
    /* Buffer -> scalaire binaire (fondation marshalling, ). */
    {
        .canonical  = "BufferVersEntier",
        .nameEN     = "BufferToInteger",
        .category   = "Conversion",
        .descFR     = "Lit un entier sur 1, 2, 4 ou 8 octets à la position indiquée (base 1) d'un buffer. En complément à deux par défaut (signé), ou non-signé si le dernier paramètre vaut Faux. L'ordre des octets est petit-boutiste par défaut ou grand-boutiste. Lève une exception si la position ou la taille sort des limites du buffer, ou si la taille n'est pas 1, 2, 4 ou 8.",
        .descEN     = "Reads an integer over 1, 2, 4 or 8 bytes at the given 1-based position of a buffer. Two's complement by default (signed), or unsigned when the last parameter is False. Byte order is little-endian by default or big-endian. Raises an exception if the position or size is outside the buffer, or if the size is not 1, 2, 4 or 8.",
        .signatures = sig_buffer_vers_entier,
        .impl       = fn_buffer_vers_entier,
    },
    {
        .canonical  = "BufferVersR\xc3\xa9""el",
        .canonicalAscii = "BufferVersReel",
        .nameEN     = "BufferToReal",
        .category   = "Conversion",
        .descFR     = "Lit un réel IEEE 754 sur 4 octets (simple précision) ou 8 octets (double précision) à la position indiquée (base 1) d'un buffer. L'ordre des octets est petit-boutiste par défaut ou grand-boutiste. Lève une exception si la position ou la taille sort des limites du buffer, ou si la taille n'est pas 4 ou 8.",
        .descEN     = "Reads an IEEE 754 real over 4 bytes (single precision) or 8 bytes (double precision) at the given 1-based position of a buffer. Byte order is little-endian by default or big-endian. Raises an exception if the position or size is outside the buffer, or if the size is not 4 or 8.",
        .signatures = sig_buffer_vers_reel,
        .impl       = fn_buffer_vers_reel,
    },
};

void stdlib_bitwise_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
    owl_registry_register_enums(DEBORDEMENT_ENUMS,
        (int)(sizeof(DEBORDEMENT_ENUMS) / sizeof(DEBORDEMENT_ENUMS[0])));
    owl_registry_register_consts(DEBORDEMENT_CONSTS,
        (int)(sizeof(DEBORDEMENT_CONSTS) / sizeof(DEBORDEMENT_CONSTS[0])));
    owl_registry_register_enums(BOUTISME_ENUMS,
        (int)(sizeof(BOUTISME_ENUMS) / sizeof(BOUTISME_ENUMS[0])));
    owl_registry_register_consts(BOUTISME_CONSTS,
        (int)(sizeof(BOUTISME_CONSTS) / sizeof(BOUTISME_CONSTS[0])));
}
