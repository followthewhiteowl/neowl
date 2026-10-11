// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#define _GNU_SOURCE
/*
 * OWL UUID stdlib.
 *
 * Type reifié (ObjInstance) :
 *   - UUID : 128 bits, format affichage canonique 8-4-4-4-12 (36 chars).
 *            Marker version (4 ou 7) + variant RFC appliqués sur les bytes.
 *
 * Natives :
 *   - DonneUUID([version])  → UUID (v4 RFC 4122 par défaut, v7 RFC 9562 si demandé)
 *
 * Méthode prefix :
 *   - id.VersChaîne([uuidFormat]) → chaîne
 *       uuidFormatStandard  (défaut) → 36 chars hex à tirets
 *       uuidFormatCompact            → 32 chars hex pur
 *       uuidFormatAccolades          → 38 chars legacy Microsoft {…}
 *
 * Enums :
 *   - uuidVersion : uuidV4, uuidV7
 *   - uuidFormat  : uuidFormatStandard, uuidFormatCompact, uuidFormatAccolades
 *
 * Source de randomness : owl_random_bytes (stdlib_crypto.c — CSPRNG OS-backed
 * via BCryptGenRandom Windows / /dev/urandom Linux/macOS).
 *
 * Storage interne : ObjInstance avec un seul field privé "_bytes" portant
 * un ObjBuffer de 16 octets. Pas de OBJ_UUID custom au niveau VM —
 * réutilise le mécanisme ObjInstance générique (pattern Chrono,
 * FichierDisque, etc.).
 *
 * Le hook côté vm.c::value_to_string délègue à owl_uuid_format_canonical
 * pour la coercion implicite UUID → chaîne (Trace, concat, Taille…).
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "sentinels.h"
#include "vm.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ================================================================
 * HELPERS
 * ================================================================ */

/* Récupère le buffer interne _bytes d'une instance UUID.
 * Retourne NULL si l'instance n'a pas le field ou s'il n'est pas un buffer. */
static ObjBuffer* uuid_get_bytes(ObjInstance* inst) {
    /*  lot 6 : root inst pendant obj_copy_string (sinon collecte
     * possible sous stress-GC, inst purement C-stack-local). */
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string("_bytes", 6);
    vm_pop();
    Value v;
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_BUFFER(v)) return NULL;
    return AS_BUFFER(v);
}

/* Vrai si v est une ObjInstance UUID (128 bits, 16 octets). */
static bool is_uuid128(Value v) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL) return false;
    return inst->structName->length == 4 && memcmp(inst->structName->chars, "UUID", 4) == 0;
}

/* Marque les bits version 4 + variant RFC 4122 sur un buffer 16 octets.
 * Doit être appelé après le tirage random pour produire un UUID v4 valide. */
static void apply_rfc4122_v4_marker(unsigned char b[16]) {
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);  /* version 4 dans le nibble haut de l'octet 6 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);  /* variant 10xx dans les bits hauts de l'octet 8 */
}

/* Marque les bits version 7 + variant RFC 9562 sur un buffer 16 octets.
 * Layout :
 *   octets 0..5  : timestamp epoch-ms en big-endian (48 bits)
 *   octet 6      : nibble haut = version 7, nibble bas = random
 *   octets 7..15 : random (variant déjà imposé sur l'octet 8)
 * Caller doit : (1) initialiser b[6..15] random, (2) écrire le timestamp
 * dans b[0..5], (3) appeler ce marker. */
static void apply_rfc9562_v7_marker(unsigned char b[16]) {
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x70);  /* version 7 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);  /* variant 10xx */
}

/* Timestamp epoch-ms (ms depuis 1970-01-01 UTC) en 48 bits low.
 * Sur Windows MSYS2 (timespec_get est dispo en C11), on s'en sert ; sinon
 * fallback time(NULL)*1000 (résolution seconde, suffisant si pas concurrent). */
static uint64_t epoch_millis(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
    }
    return (uint64_t)time(NULL) * 1000ULL;
}

/* Hex encode lowercase, écrit 2*len caractères dans out. Retourne nb chars écrits. */
static int hex_encode_lower(const unsigned char* in, int len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    return len * 2;
}

/* Formatte 16 octets en `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (36 chars).
 * Écrit dans out (37 chars min incluant nul implicite). Retourne 36. */
static int format_uuid128_dashes(const unsigned char b[16], char out[37]) {
    int n = 0;
    n += hex_encode_lower(b,      4, out + n);    /* 8  */
    out[n++] = '-';
    n += hex_encode_lower(b + 4,  2, out + n);    /* 12 */
    out[n++] = '-';
    n += hex_encode_lower(b + 6,  2, out + n);    /* 16 */
    out[n++] = '-';
    n += hex_encode_lower(b + 8,  2, out + n);    /* 20 */
    out[n++] = '-';
    n += hex_encode_lower(b + 10, 6, out + n);    /* 32 */
    out[n] = '\0';
    return n;  /* 36 */
}

/* Construit une nouvelle instance UUID (128 bits) avec un buffer _bytes
 * initialisé via le pointeur `bytes` (16 octets). Si bytes == NULL, le
 * buffer est rempli de zéros (UUID nul "00000000-...-000000000000"). */
static ObjInstance* uuid_new_instance(const unsigned char* bytes) {
    ObjString* name = obj_copy_string("UUID", 4);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_pop();  /* name retenu par inst->structName */

    vm_push(OBJ_VAL(inst));

    unsigned char zeros[16] = {0};
    const unsigned char* src = bytes ? bytes : zeros;
    ObjBuffer* buf = obj_new_buffer(src, 16);
    vm_push(OBJ_VAL(buf));

    /*  : delegue au canon inst_field_set_value. */
    inst_field_set_value(inst, "_bytes", OBJ_VAL(buf));

    vm_pop();  /* buf */
    vm_pop();  /* inst */
    return inst;
}

/* ================================================================
 * HOOKS EXPOSÉS (consommés par vm.c::value_to_string)
 * ================================================================ */

bool owl_value_is_uuid(Value v) {
    return is_uuid128(v);
}

ObjString* owl_uuid_format_canonical(Value v) {
    if (!is_uuid128(v)) return NULL;
    ObjBuffer* b = uuid_get_bytes(AS_INSTANCE(v));
    if (b == NULL || b->length != 16) return NULL;
    char out[40];
    int n = format_uuid128_dashes(b->bytes, out);
    return obj_copy_string(out, n);
}

/* ================================================================
 * NATIVES
 * ================================================================ */

/* DonneUUID([version]) — défaut = uuidV4 si argCount == 0.
 *
 * Validation manuelle défensive du param version : le mécanisme allowedBase
 * runtime n'est pas encore branché côté coerce d'argument, donc on check ici
 * que la valeur est bien OWL_UUID_V4 ou OWL_UUID_V7. Tout autre entier
 * (42, OWL_HTTP_GET, etc.) → ErreurDétectée + retour UUID nul.
 *
 * Le compile-check enum_violation_error couvre le cas des identifiants nommés
 * appartenant à un autre enum (DonneUUID(httpGet) attrapé compile-time via
 * E_uuidVersion sur le param). Cette validation runtime attrape les entiers
 * bruts qui passent la compile. */
static Value fn_donne_uuid(int argCount, Value* args) {
    int64_t version = OWL_UUID_V4;  /* défaut */

    if (argCount >= 1) {
        Value v = args[0];
        if (!IS_INTEGER(v)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "DonneUUID",
                                  "native", "DonneUUID",
                                  "arg", "version",
                                  "expected", "uuidV4 ou uuidV7",
                                  "actual", "non entier",
                                  NULL);
            ObjInstance* nul = uuid_new_instance(NULL);
            return OBJ_VAL(nul);
        }
        version = AS_INTEGER(v);
        if (version != OWL_UUID_V4 && version != OWL_UUID_V7) {
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "DonneUUID",
                                  "native", "DonneUUID",
                                  "arg", "version",
                                  NULL);
            ObjInstance* nul = uuid_new_instance(NULL);
            return OBJ_VAL(nul);
        }
    }

    unsigned char bytes[16];
    if (owl_random_bytes(bytes, 16) != 0) {
        owl_emit_native_error(E_NATIVE_CRYPTO_INIT_FAILURE, "DonneUUID",
                              "native", "DonneUUID",
                              "detail", "source de randomness indisponible",
                              NULL);
        return NIL_VAL;
    }

    if (version == OWL_UUID_V7) {
        /* Layout RFC 9562 :
         *   octets 0..5  : timestamp epoch-ms big-endian (48 bits)
         *   octet 6      : 0x70 | (nibble random 4 bits)
         *   octet 7      : random (8 bits) — contribue à l'ordre intra-ms
         *   octet 8      : 0x80 | variant (6 bits random)
         *   octets 9..15 : random
         *
         * Garantie monotonicité intra-ms : on maintient un compteur 12 bits
         * sur (b[6]&0x0F)<<8 | b[7], incrémenté à chaque appel pendant la
         * même milliseconde. À nouveau ms, on recharge depuis le random.
         * Sans ça, deux UUID v7 générés dans le même ms ne sont pas sortables
         * (les bits random les plus significatifs ne sont pas ordonnés). */
        static uint64_t s_last_ms      = 0;
        static uint16_t s_last_counter = 0;
        uint64_t ms = epoch_millis() & 0x0000FFFFFFFFFFFFULL;  /* 48 bits */

        uint16_t counter;
        if (ms == s_last_ms) {
            /* Même ms : incrément monotone. Si 12 bits saturent (4095 UUID
             * dans la même ms — extrêmement improbable), on force ms+1 pour
             * garantir l'ordre lexicographique strict. */
            if (s_last_counter >= 0x0FFF) {
                ms++;
                counter = (uint16_t)(((bytes[6] & 0x0F) << 8) | bytes[7]);
            } else {
                counter = s_last_counter + 1;
            }
        } else {
            counter = (uint16_t)(((bytes[6] & 0x0F) << 8) | bytes[7]);
        }
        s_last_ms      = ms;
        s_last_counter = counter;

        bytes[0] = (unsigned char)((ms >> 40) & 0xFF);
        bytes[1] = (unsigned char)((ms >> 32) & 0xFF);
        bytes[2] = (unsigned char)((ms >> 24) & 0xFF);
        bytes[3] = (unsigned char)((ms >> 16) & 0xFF);
        bytes[4] = (unsigned char)((ms >>  8) & 0xFF);
        bytes[5] = (unsigned char)( ms        & 0xFF);
        bytes[6] = (unsigned char)((counter >> 8) & 0x0F);   /* nibble haut écrasé par v7 marker */
        bytes[7] = (unsigned char)( counter       & 0xFF);
        apply_rfc9562_v7_marker(bytes);
    } else {
        apply_rfc4122_v4_marker(bytes);
    }

    ObjInstance* inst = uuid_new_instance(bytes);
    owl_clear_error();
    return OBJ_VAL(inst);
}

/* UUID..VersChaîne([format]) — sérialise l'UUID dans le format demandé.
 *
 * Signature méthode prefix (receiver inclus dans args) :
 *   args[0] = receiver (ObjInstance UUID)
 *   args[1] = format (entier, optionnel, défaut uuidFormatStandard)
 *
 * 3 formats :
 *   uuidFormatStandard  → "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars)
 *   uuidFormatCompact   → "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"     (32 chars hex pur)
 *   uuidFormatAccolades → "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (38 chars)
 *
 * Validation manuelle défensive du param format : si entier hors enum
 * uuidFormat → ErreurDétectée + retour "". Le compile-check enum_violation_error
 * gère les identifiants nommés via E_uuidFormat sur le methodBinding param. */
static Value fn_uuid_vers_chaine(int argCount, Value* args) {
    if (argCount < 1 || !is_uuid128(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "UUID.VersChaine",
                              "native", "UUID.VersChaine",
                              "arg", "receveur",
                              "expected", "UUID",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    int64_t format = OWL_UUID_FORMAT_STANDARD;
    if (argCount >= 2) {
        Value f = args[1];
        if (!IS_INTEGER(f)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "UUID.VersChaine",
                                  "native", "UUID.VersChaine",
                                  "arg", "format",
                                  "expected", "constante uuidFormatXxx",
                                  "actual", "non entier",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
        format = AS_INTEGER(f);
        if (format != OWL_UUID_FORMAT_STANDARD &&
            format != OWL_UUID_FORMAT_COMPACT &&
            format != OWL_UUID_FORMAT_ACCOLADES) {
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "UUID.VersChaine",
                                  "native", "UUID.VersChaine",
                                  "arg", "format",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
    }

    ObjBuffer* b = uuid_get_bytes(AS_INSTANCE(args[0]));
    if (b == NULL || b->length != 16) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "UUID.VersChaine",
                              "native", "UUID.VersChaine",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    owl_clear_error();
    if (format == OWL_UUID_FORMAT_COMPACT) {
        char out[33];
        int n = hex_encode_lower(b->bytes, 16, out);
        out[n] = '\0';
        return OBJ_VAL(obj_copy_string(out, n));
    }
    if (format == OWL_UUID_FORMAT_ACCOLADES) {
        char out[40];
        int n = 0;
        out[n++] = '{';
        n += format_uuid128_dashes(b->bytes, out + n);
        out[n++] = '}';
        out[n] = '\0';
        return OBJ_VAL(obj_copy_string(out, n));
    }
    /* uuidFormatStandard (défaut) */
    char out[40];
    int n = format_uuid128_dashes(b->bytes, out);
    return OBJ_VAL(obj_copy_string(out, n));
}

/* ================================================================
 * COERCION : chaîne → UUID
 * ================================================================ */

/* Décode un caractère hex en valeur 0..15. Retourne -1 si invalide. */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Parse une chaîne UUID en 16 octets, tolère :
 *   - accolades `{...}` optionnelles autour
 *   - tirets `-` optionnels n'importe où
 *   - majuscules ou minuscules
 * Retourne 0 sur succès (out[16] rempli), -1 sur format invalide (count
 * différent de 32 chars hex, caractère non-hex…). */
static int parse_uuid_hex(const char* in, int in_len, unsigned char out[16]) {
    int start = 0;
    int end = in_len;
    /* Trim accolades optionnelles */
    if (start < end && in[start] == '{') start++;
    if (end > start && in[end - 1] == '}') end--;

    const int need = 32;  /* 16 octets * 2 chars hex */
    int gotChars = 0;
    int byteIdx = 0;
    int hi = -1;
    for (int i = start; i < end; i++) {
        char c = in[i];
        if (c == '-') continue;  /* séparateurs tolérés */
        int d = hex_digit(c);
        if (d < 0) return -1;
        gotChars++;
        if (gotChars > need) return -1;
        if (hi < 0) {
            hi = d;
        } else {
            out[byteIdx++] = (unsigned char)((hi << 4) | d);
            hi = -1;
        }
    }
    if (gotChars != need) return -1;
    if (hi != -1) return -1;  /* nombre impair, ne devrait pas arriver */
    return 0;
}

/* __UUIDCoerce(v) — coercion implicite à l'affectation `MonU est un UUID = expr`.
 *   Le compilateur l'appelle quand declaredType == TOKEN_UUID.
 *
 * Comportement :
 *   - Pass-through si v est déjà une instance UUID (cohérent avec
 *     __FichierDisqueCoerce qui pass-through si déjà une instance
 *     FichierDisque).
 *   - String → parse hex (avec ou sans tirets/accolades) vers 16 bytes,
 *     wrap en ObjInstance UUID avec _bytes initialisé. Format invalide
 *     → ErreurDétectée=Vrai + retour UUID nul (cohérent avec ChaineVersEntier
 *     qui retourne 0 sur input non parsable).
 *   - Null → ErreurDétectée + UUID nul.
 *   - Autre → ErreurDétectée + UUID nul. */
/* Parser de coerce vers UUID — enregistre via le registry generique.
 * Le compilo emet __InstanceCoerce(v, "UUID") qui dispatch ici. */
static Value parse_to_uuid(Value v) {
    /* Pass-through si déjà UUID — sans toucher à ErreurDétectée.
     * La coerce est un no-op dans ce cas : si DonneUUID() ou autre a déjà
     * positionné/effacé l'erreur, on respecte cet état. Effacer ici masquerait
     * une erreur signalée juste avant (ex: DonneUUID(42) qui retourne UUID nul
     * + ErreurDétectée=Vrai — la coerce subséquente ne doit PAS réécraser). */
    if (is_uuid128(v)) {
        return v;
    }

    /* String → parse hex */
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        unsigned char buf[16];
        if (parse_uuid_hex(s->chars, s->length, buf) == 0) {
            ObjInstance* inst = uuid_new_instance(buf);
            owl_clear_error();
            return OBJ_VAL(inst);
        }
        /* Chaîne mal formée : UUID nul + erreur signalée. Comportement
         * symétrique de ChaineVersEntier("abc") = 0 — pas de crash, juste
         * une valeur par défaut prévisible que l'appelant peut tester. */
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "UUID",
                              "native", "UUID",
                              "value", s->chars,
                              "expected", "UUID hex (8-4-4-4-12)",
                              NULL);
        ObjInstance* nul = uuid_new_instance(NULL);
        return OBJ_VAL(nul);
    }

    /* Null ou type non géré → UUID nul + erreur. */
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "UUID",
                          "native", "UUID",
                          "arg", "source",
                          "expected", "chaine UUID hex",
                          "actual", "type non geré",
                          NULL);
    ObjInstance* nul = uuid_new_instance(NULL);
    return OBJ_VAL(nul);
}

/* ================================================================
 * REGISTRY DECLARATIONS
 * ================================================================ */

/* Enum uuidVersion — version RFC pour DonneUUID(). 2 valeurs sentinel-zone
 * (impossible de tomber dessus par accident avec un entier littéral 4/7). */
static const char* const E_uuidVersion_values[] = {
    "uuidV4", "uuidV7", NULL
};

const OwlEnum E_uuidVersion = {
    .name        = "uuidVersion",
    .nameAscii   = "uuidVersion",
    .nameEN      = "uuidVersion",
    .category    = "UUID",
    .descFR      = "Version d'UUID à générer : v4 (16 octets aléatoires, RFC 4122) ou v7 (timestamp epoch-ms en préfixe + aléatoire, RFC 9562, sortable lexicographiquement).",
    .descEN      = "UUID version to generate: v4 (16 random bytes, RFC 4122) or v7 (epoch-ms timestamp prefix + random, RFC 9562, lexicographically sortable).",
    .values      = E_uuidVersion_values,
    .combinable  = false,
};

/* Enum uuidFormat — format de sérialisation pour UUID..VersChaîne. 3 valeurs
 * sentinel-zone. */
static const char* const E_uuidFormat_values[] = {
    "uuidFormatStandard", "uuidFormatCompact", "uuidFormatAccolades", NULL
};

const OwlEnum E_uuidFormat = {
    .name        = "uuidFormat",
    .nameAscii   = "uuidFormat",
    .nameEN      = "uuidFormat",
    .category    = "UUID",
    .descFR      = "Format de sérialisation d'un UUID : standard (36 chars à tirets, RFC), compact (32 hex pur, sans séparateur) ou accolades (38 chars legacy Microsoft, format { xxx-...-xxx }).",
    .descEN      = "UUID serialization format: standard (36 chars with dashes, RFC), compact (32 hex characters, no separator) or braces (38 chars Microsoft legacy, { xxx-...-xxx } format).",
    .values      = E_uuidFormat_values,
    .combinable  = false,
};

#define C_UUID(nm, val, fr, en, nen) {nm, "UUID", INTEGER_VAL(val), fr, en, nen}

static const OwlConstDef UUID_CONSTS[] = {
    /* uuidVersion — paramètre de DonneUUID(version). EN == FR (codes RFC). */
    C_UUID("uuidV4",                OWL_UUID_V4,
        "version 4 : 16 octets aléatoires (RFC 4122). Défaut de DonneUUID().",
        "version 4: 16 random bytes (RFC 4122). DonneUUID() default.", "uuidV4"),
    C_UUID("uuidV7",                OWL_UUID_V7,
        "version 7 : timestamp epoch-ms (48 bits) + aléatoire (RFC 9562). Sortable lexicographiquement par date de création — idéal comme clé primaire BDD ou identifiant de log.",
        "version 7: epoch-ms timestamp (48 bits) + random (RFC 9562). Lexicographically sortable by creation date — ideal as a database primary key or log identifier.", "uuidV7"),

    /* uuidFormat — paramètre de UUID..VersChaîne(format). */
    C_UUID("uuidFormatStandard",    OWL_UUID_FORMAT_STANDARD,
        "format canonique RFC : 36 caractères hex à tirets (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx). Défaut.",
        "RFC canonical format: 36 hex characters with dashes (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx). Default.", "uuidFormatStandard"),
    C_UUID("uuidFormatCompact",     OWL_UUID_FORMAT_COMPACT,
        "format compact : 32 caractères hex sans séparateur. Utile en URL ou comme clé compacte.",
        "compact format: 32 hex characters without separator. Useful in URLs or as a compact key.", "uuidFormatCompact"),
    C_UUID("uuidFormatAccolades",   OWL_UUID_FORMAT_ACCOLADES,
        "format legacy Microsoft : 38 caractères, encadré par { } avec tirets ({xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}). Attendu par certaines API Windows.",
        "Microsoft legacy format: 38 characters, enclosed in { } with dashes ({xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}). Expected by some Windows APIs.", "uuidFormatBraces"),
};
#undef C_UUID

/* Signatures natives */
static const OwlParam p_donne_uuid_v[] = {
    {"version", "entier", "version d'UUID à générer (uuidV4 par défaut, uuidV7 pour les UUID timestampés sortables)", &E_uuidVersion, NULL},
    {0}
};

/* Formes discrètes : DonneUUID() (v4 par défaut) ou DonneUUID(version).
 * version est optionnelle mais bornée — l'impl ne lit jamais au-delà de args[0].
 * La forme avec param (porteuse de l'enum E_uuidVersion) est listée AVANT la
 * forme 0-arg : le self-test enum_unreferenced (rule 18) itère les signatures
 * avec `s->params` comme terminateur et s'arrêterait sur un `{0, NULL}` placé
 * en tête, masquant l'enum. L'ordre n'affecte pas la validation d'arité
 * compile-time (qui itère sur `s->returnType`). */
static const OwlSignature sig_donne_uuid[] = {
    {1, p_donne_uuid_v, "UUID", "identifiant unique 128 bits dans la version demandée (v4 aléatoire ou v7 timestampé)"},
    {0, NULL,          "UUID", "identifiant unique 128 bits v4 aléatoire (RFC 4122)"},
    {0}
};

/* Méthode UUID..VersChaîne([format]) — receiver inclus dans l'arity.
 * Formes discrètes : receiver seul (format standard par défaut) ou
 * receiver + format. L'impl ne lit jamais au-delà de args[1]. */
static const OwlParam p_uuid_recv[] = {
    {"uuid", "UUID", "l'UUID à sérialiser"},
    {0}
};
static const OwlParam p_uuid_recv_fmt[] = {
    {"uuid", "UUID", "l'UUID à sérialiser"},
    {"format", "entier", "format de sérialisation (uuidFormatStandard par défaut, uuidFormatCompact, uuidFormatAccolades)", &E_uuidFormat, NULL},
    {0}
};
static const OwlSignature sig_uuid_vers_chaine[] = {
    {1, p_uuid_recv,     "chaîne", "représentation textuelle au format standard (36 chars à tirets)"},
    {2, p_uuid_recv_fmt, "chaîne", "représentation textuelle de l'UUID dans le format demandé"},
    {0}
};

/* Bindings prefix : id.VersChaine() / id.VersChaîne() / id.ToString().
 * receiver "UUID" mappe vers METHOD_TYPE_UUID via receiver_type_from_name. */
static const OwlMethodBinding MB_UUID_VERS_CHAINE[] = {
    {"UUID", "VersChaine",  -1},   /* ASCII (scanner strippe les accents) */
    {"UUID", "VersCha\xc3\xaene", -1}, /* canonical accentué (VersChaîne) — pour autocomplete LSP/formatter */
    {"UUID", "ToString",    -1},   /* alias EN */
    {NULL, NULL, 0}
};

static const OwlNativeDef DEFS_UUID[] = {
    {
        .canonical  = "DonneUUID",
        .nameEN     = "GetUUID",
        .category   = "UUID",
        .descFR     = "Génère un identifiant unique 128 bits (taille fixe : pas de variante 256 bits). Sans argument : UUID v4 aléatoire (RFC 4122). Avec uuidV7 : UUID v7 timestampé (RFC 9562, sortable lexicographiquement). Format d'affichage par défaut : 36 caractères hex à tirets sans accolades.",
        .descEN     = "Generate a 128-bit unique identifier (fixed size: no 256-bit variant). Without argument: random UUID v4 (RFC 4122). With uuidV7: timestamped UUID v7 (RFC 9562, lexicographically sortable). Default display format: 36 hex characters with dashes, no braces.",
        .signatures = sig_donne_uuid,
        .impl       = fn_donne_uuid,
    },
    /* Méthode prefix .VersChaine() exposée via methodBindings. Le canonical
     * __UUIDVersChaine est marqué INTERNAL : ne pollue pas l'autocomplete global
     * ni la doc --doc/--vscode-data. Seul l'usage `id.VersChaine([format])`
     * est exposé à l'utilisateur. */
    {
        .canonical      = "__UUIDVersChaine",
        .category       = "UUID",
        .descFR         = "Sérialise un UUID dans le format choisi (standard à tirets, compact hex pur, ou accolades). Exposé uniquement via la méthode prefix .VersChaîne().",
        .descEN         = "Serialize a UUID in the chosen format (standard dashes, compact hex, or braces). Exposed only via the prefix method .ToString().",
        .signatures     = sig_uuid_vers_chaine,
        .impl           = fn_uuid_vers_chaine,
        .flags          = OWL_NAT_INTERNAL,
        .methodBindings = MB_UUID_VERS_CHAINE,
    },
};

/* ================================================================
 * INIT
 * ================================================================ */

/* NOTE : le type UUID (FIELDS_*, OwlTypeDef, build_*_template,
 * register_global_instance) est déclaré dans stdlib_builtin_types.c pour
 * suivre le pattern Chrono/FichierDisque. stdlib_uuid.c ne gère que les
 * natives + les enums + les constantes. */
void stdlib_uuid_init(void) {
    owl_registry_register_module(DEFS_UUID, (int)(sizeof(DEFS_UUID) / sizeof(DEFS_UUID[0])));

    static const OwlEnum* UUID_ENUMS[] = { &E_uuidVersion, &E_uuidFormat };
    owl_registry_register_enums(UUID_ENUMS, (int)(sizeof(UUID_ENUMS) / sizeof(UUID_ENUMS[0])));

    owl_registry_register_consts(UUID_CONSTS, (int)(sizeof(UUID_CONSTS) / sizeof(UUID_CONSTS[0])));

    /* Enregistre le parser de coerce dans le registre central. Le compilo
     * emettra __InstanceCoerce(v, "UUID") qui dispatch ici. */
    owl_register_instance_coerce("UUID", parse_to_uuid);
}
