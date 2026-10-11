// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "memory.h"
#include "vm.h"
#include "gc.h"  /*  : gc_mark_object pour stdlib_string_mark_roots */
#include "scanner.h"
#include "sentinels.h"
#include "owl_errors.h"
#include "string_compare_opts.h"
#include <stdio.h>
#include <locale.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>

/* ChaîneCompare option flags — doc 3024011. Combinable with +.
 * Valeurs alignees byte-a-byte sur OWLCMP_* (cf. string_compare_opts.h)
 * pour permettre la factorisation du compare. */
#define CC_SANS_CASSE            OWLCMP_SANS_CASSE
#define CC_SANS_ACCENT           OWLCMP_SANS_ACCENT
#define CC_SANS_ESPACE           OWLCMP_SANS_ESPACE          /* outer whitespace only (trim) */
#define CC_SANS_ESPACE_INT       OWLCMP_SANS_ESPACE_INT      /* internal spaces too */
#define CC_SANS_PONCTUATION      OWLCMP_SANS_PONCTUATION     /* punctuation + all spaces */
#define CC_ORDRE_LEXICOGRAPHIQUE OWLCMP_ORDRE_LEXICOGRAPHIQUE
#define CC_RESPECTE_NUMERIQUE    OWLCMP_RESPECTE_NUMERIQUE
/* ChaîneFormate extra flags (cc_Majuscule / cc_Minuscule) — bits reserved
 * for the canon ChaîneFormate options set, combinable with other cc* flags.
 * NOTE: the doc does not publish the numeric values of these constants;
 * OWL picks 128/256 as the next free bits. Users should always use the
 * symbolic names (`ccMajuscule`, `ccMinuscule`) — never the raw integers.
 * If bytecode compatibility with external canon values becomes a concern
 * we'll realign here. */
#define CC_MAJUSCULE          128
#define CC_MINUSCULE          256

/* Search option flags pour Position, PositionOccurrence, ChaineSupprime,
 * ChaineOccurrence — doc. Bits combinables.
 * DepuisFin = bit dedie pour permettre MotComplet | SansCasse | DepuisFin. */
#define SEARCH_MOT_COMPLET  1   /* whole-word: surrounded by non-word chars */
#define SEARCH_SANS_CASSE   2   /* case-insensitive */
#define SEARCH_DEPUIS_FIN   4   /* recherche en arriere depuis la fin */

/* rangXxx — traversal mode constants for ExtraitChaîneEntre, PositionOccurrence.
 * Values start at 1000 to avoid collision with ordinary rank integers (1, 2, 3…). */
/* Valeurs sentinelles : zone INT32 edge, alignees sur canon. Voir
 * src/sentinels.h pour la rationale. */
#include "sentinels.h"
#define RANG_PREMIER   OWL_RANG_PREMIER
#define RANG_SUIVANT   OWL_RANG_SUIVANT
#define RANG_DERNIER   OWL_RANG_DERNIER
#define RANG_PRECEDENT OWL_RANG_PRECEDENT

/* EOT character (Caract(4)) */
#define EOT_CHAR "\x04"

/* Helper: extract int from Value (for position/length args) */
static int to_int(Value v) {
    if (IS_INTEGER(v)) return (int)AS_INTEGER(v);
    if (IS_REAL(v)) return (int)AS_REAL(v);
    return 0;
}

/* Coerce any Value to ObjString for the string natives (Majuscule, Gauche,
 * Taille, Remplace, etc.). The canon accepts numerics transparently:
 * `Majuscule(4) == "4"`, `Taille(42) == 2`, `Remplace(3.14, ".", ",")`.
 * We defer to vm.c's `value_to_string` which already encodes the canonical
 * canonical rendering for every Value type (int, real, bool, Null, Date,
 * Heure, DateHeure, Durée, Monétaire, Numérique) — keeping the coercion
 * logic in ONE place avoids drift between "value_to_string" used by
 * OP_CONCAT / ChaîneVersXxx and what the string stdlib sees. */
static ObjString* to_string(Value v) {
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        return obj_copy_string((const char*)b->bytes, b->length);
    }
    return value_to_string(v);
}

/* ================================================================
 * UTF-8 helpers — all string functions exposed at the string API
 * (Taille, Gauche, Droite, Milieu, Position) operate at the *codepoint*
 * level, matching the canon contract. Internal storage stays UTF-8.
 *
 * Width of a UTF-8 leading byte (0 = continuation byte, treated as 1
 * to make slicing tolerant on invalid input). */
static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  /* invalid leading byte: skip 1 byte to make progress */
}

/* Number of codepoints in a UTF-8 string of `len` bytes. */
static int utf8_cp_count(const char* s, int len) {
    int n = 0, i = 0;
    while (i < len) {
        i += utf8_seq_len((unsigned char)s[i]);
        n++;
    }
    return n;
}

/* Byte offset of the codepoint at index `cp` (0-based). Returns `len` if
 * cp >= cp_count. Plain-string callers use the cache-aware inline walk in
 * str_byte_at_cp; this helper serves the undetectable round-trip path
 * (cleartext temp). Always compiled so non-OWL_INDETECTABLE builds don't
 * break the build with -Werror=unused (function elided by linker if cold). */
__attribute__((unused))
static int utf8_byte_at_cp(const char* s, int len, int cp) {
    int i = 0, n = 0;
    while (i < len && n < cp) {
        i += utf8_seq_len((unsigned char)s[i]);
        n++;
    }
    return i;
}

/* Codepoint index of the byte at `byte_off` (used to convert Position
 * search results back to codepoints). */
static int utf8_cp_at_byte(const char* s, int len, int byte_off) {
    int i = 0, n = 0;
    while (i < len && i < byte_off) {
        i += utf8_seq_len((unsigned char)s[i]);
        n++;
    }
    return n;
}

/* ObjString-aware variants -- transparently unmask <indétectable> strings
 * so the UTF-8 parser sees cleartext bytes. Plain strings benefit from
 * three lazy caches on ObjString (ascii_state, cp_count, last_cp_index/
 * last_byte_offset) that turn O(n) walks into O(1) for ASCII and O(delta)
 * for incremental loops on multibyte. Caches are invalidated on mutation
 * (see OP_APPEND_IN_PLACE in src/vm.c). */

/* True if the string is entirely ASCII (no byte >= 0x80). Lazy detection,
 * cached on first call. Skipped on undetectable strings (chars are masked). */
static bool str_is_ascii(ObjString* s) {
    if (s->ascii_state != 0) return s->ascii_state == 1;
#ifdef OWL_INDETECTABLE
    if (s->undetectable) { s->ascii_state = 2; return false; }
#endif
    for (int i = 0; i < s->length; i++) {
        if ((unsigned char)s->chars[i] >= 0x80) {
            s->ascii_state = 2;
            return false;
        }
    }
    s->ascii_state = 1;
    return true;
}

static int str_cp_count(ObjString* s) {
    if (s->cp_count >= 0) return s->cp_count;
    if (str_is_ascii(s)) {
        s->cp_count = s->length;
        return s->cp_count;
    }
#ifdef OWL_INDETECTABLE
    if (s->undetectable) {
        char* clear = obj_string_unmask_dup(s);
        s->cp_count = utf8_cp_count(clear, s->length);
        obj_string_secure_free(clear, s->length);
        return s->cp_count;
    }
#endif
    s->cp_count = utf8_cp_count(s->chars, s->length);
    return s->cp_count;
}

static int str_byte_at_cp(ObjString* s, int cp) {
    if (cp <= 0) return 0;
    /* ASCII fast path: codepoint index == byte index. */
    if (str_is_ascii(s)) {
        return cp <= s->length ? cp : s->length;
    }
#ifdef OWL_INDETECTABLE
    if (s->undetectable) {
        /* Mask round-trip dominates; cache wouldn't pay off on the
         * temporary plaintext anyway. */
        char* clear = obj_string_unmask_dup(s);
        int off = utf8_byte_at_cp(clear, s->length, cp);
        obj_string_secure_free(clear, s->length);
        return off;
    }
#endif
    /* Incremental walk: start from cached position when the request is
     * ahead of (or at) the cache. The hot pattern is ascending iteration
     * `Milieu(s, i, k)` where each call advances by 1 codepoint. */
    int start_cp = 0, start_byte = 0;
    if (s->last_cp_index >= 0 && s->last_cp_index <= cp) {
        start_cp = s->last_cp_index;
        start_byte = s->last_byte_offset;
    }
    int byte = start_byte, n = start_cp;
    while (byte < s->length && n < cp) {
        byte += utf8_seq_len((unsigned char)s->chars[byte]);
        n++;
    }
    s->last_cp_index = n;
    s->last_byte_offset = byte;
    return byte;
}

static int str_cp_at_byte(ObjString* s, int byte_off) {
    if (str_is_ascii(s)) {
        return byte_off <= s->length ? byte_off : s->length;
    }
#ifdef OWL_INDETECTABLE
    if (s->undetectable) {
        char* clear = obj_string_unmask_dup(s);
        int n = utf8_cp_at_byte(clear, s->length, byte_off);
        obj_string_secure_free(clear, s->length);
        return n;
    }
#endif
    return utf8_cp_at_byte(s->chars, s->length, byte_off);
}

/* ================================================================
 * STRING FUNCTIONS
 * All positions are BASE 1 and operate on CODEPOINTS, not bytes.
 * ================================================================ */

/* Taille(s) / Length(s) - string codepoint count, or array/buffer/coll length */
static Value fn_taille(int argCount, Value* args) {
    (void)argCount;
    /*  — un nom nu de widget (ObjUiControlRef) passe en argument
     * scalaire ne transite pas par value_to_string ici (les branches
     * collection/primitif lisent args[0] direct). On deballe au chokepoint
     * pour resoudre ..defaultProperty avant de mesurer : Taille(SAI) ==
     * Taille(SAI..Valeur). Cible scalaire only — Taille consomme une valeur,
     * jamais un controle. Idempotent sur string/array/etc (no-op). */
    Value a0 = owl_instance_default_unwrap(args[0]);
    if (IS_STRING(a0)) {
        return INTEGER_VAL(str_cp_count(AS_STRING(a0)));
    }
    if (IS_BUFFER(a0)) return INTEGER_VAL(AS_BUFFER(a0)->length);
    if (IS_ARRAY(a0)) return INTEGER_VAL(AS_ARRAY(a0)->elements.count);
    if (IS_ASSOC_ARRAY(a0)) return INTEGER_VAL(AS_ASSOC_ARRAY(a0)->count);
    if (IS_COLLECTION(a0)) return INTEGER_VAL(AS_COLLECTION(a0)->elements.count);
    /*  lot C : ObjMultilingue se comporte comme la chaine de
     * sa langue courante runtime (Taille(m) = nb caracteres de
     * m[langueCourante]). Pas le count du storage interne — ce serait
     * une fuite d'implementation, l'utilisateur voit `m` comme une
     * chaine traduisible. */
    if (IS_MULTILINGUE(a0)) {
        Value resolved = multilingue_resolve(AS_MULTILINGUE(a0),
                                             owl_i18n_langue_courante(),
                                             owl_i18n_langue_source());
        if (IS_STRING(resolved)) return INTEGER_VAL(str_cp_count(AS_STRING(resolved)));
        return INTEGER_VAL(0);
    }
    if (IS_JSON(a0)) {
        ObjJson* j = AS_JSON(a0);
        switch (j->jtype) {
            case JSON_T_ARRAY:  return INTEGER_VAL(j->as.arrayVal.count);
            case JSON_T_OBJECT: return INTEGER_VAL(j->as.objectVal.count);
            case JSON_T_STRING: return INTEGER_VAL(j->as.strVal ? j->as.strVal->length : 0);
            default:            return INTEGER_VAL(0);
        }
    }
    /* Date/Time/DateTime: return length of string representation */
    if (IS_DATE(a0)) return INTEGER_VAL(8);        /* YYYYMMDD */
    if (IS_TIME(a0)) return INTEGER_VAL(8);        /* HHMMSSCC (canon spec) */
    if (IS_DATETIME(a0)) return INTEGER_VAL(14);   /* YYYYMMDDHHMMSS */
    /* Canon : Taille = longueur de la représentation chaîne. Pour entier,
     * réel, booléen, monétaire, numérique : cast vers string puis longueur.
     * Exemple : Taille(422) = 3, Taille(3.14) = 4, Taille(Vrai) = 1. */
    if (IS_INTEGER(a0) || IS_REAL(a0) || IS_BOOL(a0) ||
        IS_CURRENCY(a0) || IS_NUMERIC(a0)) {
        ObjString* s = value_to_string(a0);
        return INTEGER_VAL(str_cp_count(s));
    }
    /* NIL en context Taille = 0 (canon : Null = chaîne vide en cast). */
    if (IS_NIL(a0)) return INTEGER_VAL(0);
    return INTEGER_VAL(0);
}

/* Helper: count total elements in an array (recursive for nested 2D+). */
static int array_total_elements(ObjArray* arr) {
    int total = 0;
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (IS_ARRAY(v)) {
            total += array_total_elements(AS_ARRAY(v));
        } else {
            total++;
        }
    }
    return total;
}

/* Dimension(tab) — nombre TOTAL d'éléments (doc: tab 5×3 entiers → 15).
 * Dimension(tab, n) — redim tableau 1D.
 * Dimension(tab, r, c [, ...]) — redim multi-dim (nested).
 * Doc 3013022. */
static Value fn_dimension(int argCount, Value* args) {
    /* Forme 1: retourner le nombre total d'éléments */
    if (argCount == 1) {
        if (IS_ARRAY(args[0])) {
            ObjArray* arr = AS_ARRAY(args[0]);
            /* Si nested (chaque cellule extérieure est un array), retourner le
             * total récursif. Sinon, .Occurrence. */
            bool isNested = false;
            for (int i = 0; i < arr->elements.count; i++) {
                if (IS_ARRAY(arr->elements.values[i])) { isNested = true; break; }
            }
            if (isNested) return INTEGER_VAL(array_total_elements(arr));
            return INTEGER_VAL(arr->elements.count);
        }
        /* Non-array: fallback to fn_taille */
        return fn_taille(1, args);
    }

    /* Forme 2+: redim */
    if (!IS_ARRAY(args[0])) return NIL_VAL;
    ObjArray* arr = AS_ARRAY(args[0]);

    int newSize = argCount >= 2 && IS_INTEGER(args[1]) ? (int)AS_INTEGER(args[1]) : 0;
    if (newSize < 0) newSize = 0;

    /* Pour les tableaux 2D nested, le elementPrimKind du parent décrit le type
     * des cells leaf. On l'utilise pour fill les nouvelles cells/rows avec
     * la zero-value du type (canon : extension d'un tableau typé ne
     * produit jamais Null). */
    Value defaultLeaf = obj_array_default_for_kind(arr->elementPrimKind);

    /* Resize outer to newSize */
    int oldSize = arr->elements.count;
    if (newSize > oldSize) {
        for (int i = oldSize; i < newSize; i++) {
            if (argCount >= 3) {
                /* Each new row is itself a nested array — build with remaining dims */
                ObjArray* sub = obj_new_array();
                sub->elementPrimKind = arr->elementPrimKind;  /* propage type */
                vm_push(OBJ_VAL(sub));
                int innerSize = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2]) : 0;
                for (int k = 0; k < innerSize; k++) {
                    value_array_write(&sub->elements, defaultLeaf);
                }
                obj_array_push(arr, OBJ_VAL(sub));
                vm_pop();
            } else {
                value_array_write(&arr->elements, defaultLeaf);
            }
        }
    } else if (newSize < oldSize) {
        arr->elements.count = newSize;
    }

    /* If additional dim sizes provided, resize each row too */
    if (argCount >= 3) {
        int innerSize = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2]) : 0;
        if (innerSize < 0) innerSize = 0;
        for (int i = 0; i < arr->elements.count; i++) {
            Value v = arr->elements.values[i];
            ObjArray* row;
            if (IS_ARRAY(v)) {
                row = AS_ARRAY(v);
            } else {
                /* Cell was NIL (row added without sub-array); create one */
                row = obj_new_array();
                row->elementPrimKind = arr->elementPrimKind;
                arr->elements.values[i] = OBJ_VAL(row);
            }
            int rowOld = row->elements.count;
            if (innerSize > rowOld) {
                for (int k = rowOld; k < innerSize; k++) {
                    value_array_write(&row->elements, defaultLeaf);
                }
            } else if (innerSize < rowOld) {
                row->elements.count = innerSize;
            }
        }
    }

    return NIL_VAL;
}

/* Helper: substring extraction that propagates the undetectable flag.
 * For non-undetectable sources, equivalent to obj_copy_string. For
 * undetectable sources, unmasks the source into a temp, extracts the
 * range, hands off to the undetectable allocator, and scrubs the temp. */
static Value substring_propagate(ObjString* s, int offset, int n) {
    if (!s->undetectable) return OBJ_VAL(obj_copy_string(s->chars + offset, n));
    char* clear = obj_string_unmask_dup(s);
    ObjString* result = obj_copy_string_undetectable(clear + offset, n);
    obj_string_secure_free(clear, s->length);
    return OBJ_VAL(result);
}

/* Gauche(s, n) / Left(s, n) - first n characters */
static Value fn_gauche(int argCount, Value* args) {
    (void)argCount;
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    /*   : root s (fresh ObjString si args[0] non-string) car
     * substring_propagate fait obj_copy_string interne (alloc → GC) qui peut
     * collecter s et son chars buffer → memcpy lit poison 0xDD. */
    vm_push(OBJ_VAL(s));
    int n = to_int(args[1]);
    Value result;
    if (n <= 0) {
        result = OBJ_VAL(obj_copy_string("", 0));
    } else {
        int cpTotal = str_cp_count(s);
        if (n >= cpTotal) {
            result = OBJ_VAL(s);
        } else {
            int byteEnd = str_byte_at_cp(s, n);
            result = substring_propagate(s, 0, byteEnd);
        }
    }
    vm_pop(); /* s */
    return result;
}

/* Droite(s, n) / Right(s, n) - last n characters */
/* Droite(s [, n]) / Right(s [, n])
 * canon: si n omis, trim les espaces à gauche et retourne le reste.
 * Sinon, retourne les n derniers caractères. */
static Value fn_droite(int argCount, Value* args) {
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    /*   : root s pendant substring_propagate. Cf fn_gauche. */
    vm_push(OBJ_VAL(s));
    Value result;

    /* Forme 0-arg (trim espaces gauche) */
    if (argCount == 1) {
        int start = 0;
        while (start < s->length && s->chars[start] == ' ') start++;
        if (start == 0) result = OBJ_VAL(s);
        else result = substring_propagate(s, start, s->length - start);
    } else {
        /* Forme 1-arg (n derniers caractères) */
        int n = to_int(args[1]);
        if (n <= 0) result = OBJ_VAL(obj_copy_string("", 0));
        else {
            int cpTotal = str_cp_count(s);
            if (n >= cpTotal) result = OBJ_VAL(s);
            else {
                int byteStart = str_byte_at_cp(s, cpTotal - n);
                result = substring_propagate(s, byteStart, s->length - byteStart);
            }
        }
    }
    vm_pop(); /* s */
    return result;
}

/* Milieu(s, pos [, n]) / Middle(s, pos [, n]) — substring from codepoint pos
 * (base 1), n codepoints. Sans n : retourne tout depuis pos jusqu'a la fin
 * (canon). */
static Value fn_milieu(int argCount, Value* args) {
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    /*   : root s pendant substring_propagate. Cf fn_gauche. */
    vm_push(OBJ_VAL(s));
    Value result;
    int posCp = to_int(args[1]) - 1; /* base-1 to base-0, in codepoints */
    if (posCp < 0) posCp = 0;
    int cpTotal = str_cp_count(s);
    if (posCp >= cpTotal) {
        result = OBJ_VAL(obj_copy_string("", 0));
    } else {
        int nCp;
        if (argCount >= 3) {
            nCp = to_int(args[2]);
            if (nCp <= 0) { result = OBJ_VAL(obj_copy_string("", 0)); goto done; }
            if (posCp + nCp > cpTotal) nCp = cpTotal - posCp;
        } else {
            /* Sans n : jusqu'a la fin */
            nCp = cpTotal - posCp;
        }
        int byteStart = str_byte_at_cp(s, posCp);
        int byteEnd   = str_byte_at_cp(s, posCp + nCp);
        result = substring_propagate(s, byteStart, byteEnd - byteStart);
    }
done:
    vm_pop(); /* s */
    return result;
}

/* Position(s, search [, start]) - find codepoint position (base 1), 0 if not found.
 * Bytes-wise search is safe on UTF-8 (self-synchronizing), but start arg
 * and return value are codepoint-indexed per canon contract. */
/* Position(s, search [, start [, mode]]) -- canon.
 *   search = chaine OU tableau de chaines (forme array)
 *   mode = DepuisDebut (1, defaut) : recherche en avant a partir de start
 *   mode = DepuisFin   (SEARCH_DEPUIS_FIN = 4) : recherche en arriere
 *                       Si start = 0 (sentinelle) part de la fin.
 * Position retournee = base 1 codepoint, 0 si absent. */
static Value fn_position(int argCount, Value* args) {
    /*  — garde : forme minimale Position(s, recherche) = 2 args.
     * Sous-application (appel dynamique / hors validation) → pas de lecture
     * hors-borne de args[1]. */
    if (argCount < 2) return INTEGER_VAL(0);
    ObjString* haystack = to_string(args[0]);
    if (haystack == NULL) return INTEGER_VAL(0);
    int totalCp = str_cp_count(haystack);

    /* Mode : bit DepuisFin actif -> recherche en arriere */
    int mode = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) : 1;
    bool reverse = (mode & SEARCH_DEPUIS_FIN) != 0;

    /* Start (base 1) : si absent ou 0, defaut = 1 (forward) ou totalCp (reverse) */
    int startCp;
    if (argCount >= 3) {
        startCp = to_int(args[2]);
        if (startCp == 0) startCp = reverse ? totalCp : 1;
    } else {
        startCp = reverse ? totalCp : 1;
    }
    int startIdx = startCp - 1;
    if (startIdx < 0) startIdx = 0;

    /* Forme tableau de chercheurs : Position(s, [c1, c2, ...] [, start [, mode]]).
     * Renvoie la 1ere occurrence (ou la derniere si DepuisFin) parmi tous
     * les chercheurs combines. */
    if (IS_ARRAY(args[1])) {
        ObjArray* arr = AS_ARRAY(args[1]);
        if (arr->elements.count == 0) return INTEGER_VAL(0);
        /* Pour chaque chercheur, calculer sa position via une recursion sur
         * fn_position (forme chaine). Garder le min (forward) ou max (reverse). */
        int best = 0; /* 0 = absent */
        for (int i = 0; i < arr->elements.count; i++) {
            Value sub = arr->elements.values[i];
            if (!IS_STRING(sub)) continue;
            /* Construire un appel forme chaine recursive */
            Value subArgs[4];
            subArgs[0] = args[0];
            subArgs[1] = sub;
            int subN = 2;
            if (argCount >= 3) { subArgs[subN++] = args[2]; }
            if (argCount >= 4) { subArgs[subN++] = args[3]; }
            Value res = fn_position(subN, subArgs);
            if (!IS_INTEGER(res)) continue;
            int p = (int)AS_INTEGER(res);
            if (p == 0) continue;
            if (best == 0) best = p;
            else if (reverse) { if (p > best) best = p; }
            else              { if (p < best) best = p; }
        }
        return INTEGER_VAL(best);
    }

    /* Forme classique chaine */
    ObjString* needle = to_string(args[1]);
    if (needle == NULL) return INTEGER_VAL(0);
    if (needle->length == 0) return INTEGER_VAL(startIdx + 1);

    if (reverse) {
        if (startIdx >= totalCp) startIdx = totalCp - 1;
        for (int cp = startIdx; cp >= 0; cp--) {
            int byteOff = str_byte_at_cp(haystack, cp);
            int remaining = haystack->length - byteOff;
            if (remaining < needle->length) continue;
            if (memcmp(haystack->chars + byteOff, needle->chars, needle->length) == 0) {
                return INTEGER_VAL(cp + 1);
            }
        }
        return INTEGER_VAL(0);
    }

    int byteStart = str_byte_at_cp(haystack, startIdx);
    if (byteStart >= haystack->length) return INTEGER_VAL(0);
    const char* found = strstr(haystack->chars + byteStart, needle->chars);
    if (found == NULL) return INTEGER_VAL(0);
    int byteOff = (int)(found - haystack->chars);
    return INTEGER_VAL(str_cp_at_byte(haystack, byteOff) + 1);
}

/* Remplace(s, old, new) / Replace(s, old, new) - replace all occurrences */
/* Remplace(s, old, new) / Replace(s, old, new) — canon
 * Forme 1: Remplace(s, search, replacement [, options]) — remplace toutes occurrences
 * Forme 2: Remplace(s, position, length, replacement) — forme positionnelle
 */
static Value fn_remplace(int argCount, Value* args) {
    ObjString* src = to_string(args[0]);
    if (src == NULL) return args[0];

    /* Forme 2: positionnelle (args[1] = int position) */
    if (argCount >= 4 && IS_INTEGER(args[1])) {
        int pos = (int)AS_INTEGER(args[1]) - 1; /* base-1 to base-0 */
        int len = to_int(args[2]);
        ObjString* rep = to_string(args[3]);
        if (rep == NULL) rep = obj_copy_string("", 0);

        if (pos < 0) pos = 0;
        if (pos >= src->length || len <= 0) return args[0];
        if (pos + len > src->length) len = src->length - pos;

        int newLen = src->length - len + rep->length;
        char* buf = ALLOCATE(char, newLen + 1);
        memcpy(buf, src->chars, pos);
        memcpy(buf + pos, rep->chars, rep->length);
        memcpy(buf + pos + rep->length, src->chars + pos + len, src->length - pos - len);
        buf[newLen] = '\0';
        return OBJ_VAL(obj_take_string(buf, newLen));
    }

    /* Forme 1b: tableau de patterns à remplacer */
    if (IS_ARRAY(args[1])) {
        ObjArray* arr = AS_ARRAY(args[1]);
        ObjString* rep = to_string(args[2]);
        if (rep == NULL) rep = obj_copy_string("", 0);
        /*  : push rep si fresh (cas to_string sur args non-chaine
         * ou obj_copy_string(""). Sinon rep est rooted via args[2]). */
        vm_push(OBJ_VAL(rep));

        Value result = args[0];
        vm_push(result); /* root result fresh entre iterations */
        for (int i = 0; i < arr->elements.count; i++) {
            Value v = arr->elements.values[i];
            if (!IS_STRING(v)) continue;
            ObjString* pattern = AS_STRING(v);
            if (pattern->length == 0) continue;

            /* Apply fn_remplace recursively on current result */
            Value tempArgs[3] = { result, OBJ_VAL(pattern), OBJ_VAL(rep) };
            result = fn_remplace(3, tempArgs);
            /* Re-push : retire ancien result, push nouveau */
            vm_pop();
            vm_push(result);
        }
        vm_pop(); /* result */
        vm_pop(); /* rep */
        return result;
    }

    /* Forme 1a: search/replace simple */
    ObjString* old = to_string(args[1]);
    ObjString* rep = to_string(args[2]);
    if (old == NULL || rep == NULL) return args[0];

    if (old->length == 0) return args[0];

    /* Count occurrences first */
    int count = 0;
    const char* p = src->chars;
    while ((p = strstr(p, old->chars)) != NULL) {
        count++;
        p += old->length;
    }
    if (count == 0) return args[0];

    /* Build result */
    int newLen = src->length + count * (rep->length - old->length);
    char* buf = ALLOCATE(char, newLen + 1);
    char* dst = buf;
    p = src->chars;
    while (*p) {
        if (strncmp(p, old->chars, old->length) == 0) {
            memcpy(dst, rep->chars, rep->length);
            dst += rep->length;
            p += old->length;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, newLen));
}

/* Majuscule(s) / Upper(s) - uppercase */
/* UTF-8 aware case conversion for ASCII + Latin-1 Supplement.
 * - ASCII (0x00-0x7F) : standard toupper/tolower.
 * - Latin-1 two-byte sequences 0xC3 0xXX : flip XOR 0x20 on byte2 when in
 *   lowercase range [0xA0..0xBE] / uppercase range [0x80..0x9E], skipping
 *   the ÷ (0xB7) and × (0x97) math symbols that have no case mate.
 *   Sorry ß (0xC3 0x9F) — your uppercase is 2 chars (SS), out of v1 scope.
 * - Other bytes pass through unchanged.
 *
 * Note doc-vs-realite : une doc ancienne indique que les minuscules accentuees
 * doivent etre converties en majuscules NON accentuees ("pépère" -> "PEPERE").
 * Le comportement reel constate est different :
 *   Majuscule("pépère et caéïne") -> "PÉPÈRE ET CAÉÏNE" (preserve les accents)
 * Doc obsolete -> on garde XOR 0x20 simple, conforme au comportement reel. */
static void owl_case_convert(const char* src, int len, char* dst, bool toUpper) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) {
            dst[i] = toUpper ? (char)toupper(c) : (char)tolower(c);
            continue;
        }
        if (c == 0xC3 && i + 1 < len) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (toUpper && c2 >= 0xA0 && c2 <= 0xBE && c2 != 0xB7) {
                dst[i]     = (char)0xC3;
                dst[i + 1] = (char)(c2 - 0x20);
                i++;
                continue;
            }
            if (!toUpper && c2 >= 0x80 && c2 <= 0x9E && c2 != 0x97) {
                dst[i]     = (char)0xC3;
                dst[i + 1] = (char)(c2 + 0x20);
                i++;
                continue;
            }
        }
        dst[i] = src[i];
    }
}

static Value fn_majuscule(int argCount, Value* args) {
    (void)argCount;
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    char* buf = ALLOCATE(char, s->length + 1);
    owl_case_convert(s->chars, s->length, buf, true);
    buf[s->length] = '\0';
    return OBJ_VAL(obj_take_string(buf, s->length));
}

/* Minuscule(s) / Lower(s) - lowercase */
static Value fn_minuscule(int argCount, Value* args) {
    (void)argCount;
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    char* buf = ALLOCATE(char, s->length + 1);
    owl_case_convert(s->chars, s->length, buf, false);
    buf[s->length] = '\0';
    return OBJ_VAL(obj_take_string(buf, s->length));
}

/* SansEspace(s [, position]) — strip ASCII spaces (U+0020 only).
 * Position constants (canon):
 *   sscDroite=1     trim right
 *   sscGauche=2     trim left
 *   sscExterieur=3  trim left+right (default)
 *   sscInterieur=4  collapse internal runs to single space (kept on edges)
 *   sscTout=5       remove every space, edges and inside */
static Value fn_sansespace(int argCount, Value* args) {
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    int mode = OWL_SSC_EXTERIEUR; /* defaut canon */
    if (argCount >= 2) {
        if (IS_INTEGER(args[1])) mode = (int)AS_INTEGER(args[1]);
        else if (IS_REAL(args[1])) mode = (int)AS_REAL(args[1]);
    }
    int len = s->length;
    int start = 0, end = len;
    if (mode == OWL_SSC_GAUCHE || mode == OWL_SSC_EXTERIEUR) {
        while (start < end && s->chars[start] == ' ') start++;
    }
    if (mode == OWL_SSC_DROITE || mode == OWL_SSC_EXTERIEUR) {
        while (end > start && s->chars[end-1] == ' ') end--;
    }
    if (mode != OWL_SSC_INTERIEUR && mode != OWL_SSC_TOUT) {
        return OBJ_VAL(obj_copy_string(s->chars + start, end - start));
    }
    /* Internal modes: build a fresh buffer */
    char* buf = malloc((size_t)len);
    if (!buf) return NIL_VAL;
    int n = 0;
    if (mode == OWL_SSC_TOUT) {
        for (int i = 0; i < len; i++) {
            if (s->chars[i] != ' ') buf[n++] = s->chars[i];
        }
    } else {  /* sscInterieur: drop all internal spaces, keep left/right padding */
        int leftPad = 0, rightPad = 0;
        while (leftPad < len && s->chars[leftPad] == ' ') leftPad++;
        int rEnd = len;
        while (rEnd > leftPad && s->chars[rEnd-1] == ' ') { rEnd--; rightPad++; }
        for (int i = 0; i < leftPad; i++) buf[n++] = ' ';
        for (int i = leftPad; i < rEnd; i++) {
            if (s->chars[i] != ' ') buf[n++] = s->chars[i];
        }
        for (int i = 0; i < rightPad; i++) buf[n++] = ' ';
    }
    Value result = OBJ_VAL(obj_copy_string(buf, n));
    free(buf);
    return result;
}

extern int vm_count_controls_named(const char* name);

/* Occurrence(arg) / Occurrence(s, search) — multi-forme :
 *  - 1 arg tableau     : count des elements du tableau (extension OWL)
 *  - 1 arg chaine      : nombre de champs UI declares (canon)
 *                        inspection statique de vm.windows[].controls[],
 *                        ne necessite PAS d'avoir Ouvre() la fenetre
 *  - 2 args chaines    : count des occurrences de la sous-chaine (extension OWL,
 *                        equivalent a ChaineOccurrence canon) */
static Value fn_occurrence(int argCount, Value* args) {
    if (IS_ARRAY(args[0])) {
        (void)argCount;
        return INTEGER_VAL(AS_ARRAY(args[0])->elements.count);
    }
    /* Forme 1-arg ObjUiControl (apres Ouvre()) : extraire le nom puis count */
    if (argCount == 1 && IS_OBJ(args[0]) && OBJ_TYPE(args[0]) == OBJ_UI_CONTROL) {
        ObjUiControl* c = (ObjUiControl*)AS_OBJ(args[0]);
        return INTEGER_VAL(c->name ? vm_count_controls_named(c->name) : 0);
    }
    ObjString* hs = to_string(args[0]);
    if (hs == NULL) return INTEGER_VAL(0);

    /* Forme 1-arg chaine : champ UI declare (canon) */
    if (argCount == 1) {
        return INTEGER_VAL(vm_count_controls_named(hs->chars));
    }

    /* Forme 2-args : count substring (extension OWL) */
    ObjString* needle = to_string(args[1]);
    if (needle == NULL) return INTEGER_VAL(0);
    if (needle->length == 0) return INTEGER_VAL(0);

    int count = 0;
    const char* p = hs->chars;
    while ((p = strstr(p, needle->chars)) != NULL) {
        count++;
        p += needle->length;
    }
    return INTEGER_VAL(count);
}

/* ChaineVersEntier(s) / Val(s) - string to integer */
static Value fn_chaine_vers_entier(int argCount, Value* args) {
    (void)argCount;
    /*  — deballe un nom nu de widget (..defaultProperty) avant de
     * parser : ChaineVersEntier(SAI) == ChaineVersEntier(SAI..Valeur).
     * Cible scalaire only, idempotent sur les chaines (no-op). */
    Value v0 = owl_instance_default_unwrap(args[0]);
    if (!IS_STRING(v0)) return INTEGER_VAL(0);
    const char* s = AS_CSTRING(v0);
    char* end;
    long long val = strtoll(s, &end, 10);
    if (end == s) {
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "ChaineVersEntier",
                              "native", "ChaineVersEntier",
                              "value", s,
                              "expected", "entier",
                              NULL);
        return INTEGER_VAL(0);
    }
    owl_clear_error();
    return INTEGER_VAL((int64_t)val);
}

/* Val(s) — polymorphic numeric parse. Returns integer if the string is
 * a pure integer literal, real if it contains a decimal point or
 * exponent, 0 if the string doesn't start with a number. Matches
 * canon behaviour (Val("3.14") → 3.14 real, Val("42") → 42 integer,
 * Val("zero") → 0 integer). */
static Value fn_val(int argCount, Value* args) {
    (void)argCount;
    /*  — un nom nu de widget (ObjUiControlRef) passe en argument a
     * une native scalaire ne transite PAS par les opcodes deballeurs : on
     * deballe ici au chokepoint owl_instance_default_unwrap (resolveur VM +
     * path ObjInstance) pour resoudre ..defaultProperty avant de parser.
     * Sans ca Val(SAI) lisait l'objet controle au lieu de "42" -> 0.
     * Cible scalaire only : Val consomme une chaine, pas un controle. */
    Value v0 = owl_instance_default_unwrap(args[0]);
    if (!IS_STRING(v0)) return INTEGER_VAL(0);
    const char* s = AS_CSTRING(v0);
    /* Skip leading whitespace to mirror canon tolerance. */
    while (*s == ' ' || *s == '\t') s++;

    /* Detect if the numeric form is real (contains . or e/E/d/D in its prefix).
     * canon accepts D/d as Fortran-style exponent: "1D2" = 1×10² = 100. */
    const char* p = s;
    if (*p == '+' || *p == '-') p++;
    while (*p >= '0' && *p <= '9') p++;
    bool looksReal = (*p == '.' || *p == 'e' || *p == 'E' || *p == 'd' || *p == 'D');

    char* end;
    if (looksReal) {
        /* strtod doesn't understand D/d — substitute the first D→E in a temp buf. */
        char buf[128];
        int slen = (int)strlen(s);
        if (slen >= (int)sizeof(buf)) slen = (int)sizeof(buf) - 1;
        memcpy(buf, s, (size_t)slen);
        buf[slen] = '\0';
        for (int i = 0; i < slen; i++) {
            if (buf[i] == 'D' || buf[i] == 'd') { buf[i] = 'E'; break; }
        }
        double v = strtod(buf, &end);
        if (end == buf) { owl_clear_error(); return INTEGER_VAL(0); }
        owl_clear_error();
        return REAL_VAL(v);
    } else {
        long long v = strtoll(s, &end, 10);
        if (end == s) { owl_clear_error(); return INTEGER_VAL(0); }
        owl_clear_error();
        return INTEGER_VAL((int64_t)v);
    }
}

/* ChaîneVersNumérique(s) — parse exact vers numérique (bignum décimal).
 * Contrairement à Val (qui retombe sur entier/réel et tronque les grands
 * nombres), on conserve toute la précision : un littéral de 30 chiffres ou
 * un décimal long est exact. On lit le plus long préfixe numérique valide
 * (signe, chiffres, point, exposant e/E/d/D), comme Val tolère le bruit en
 * fin ; chaîne non numérique ou vide -> 0. */
static Value fn_chaine_vers_numerique(int argCount, Value* args) {
    (void)argCount;
    Value v0 = owl_instance_default_unwrap(args[0]);
    if (!IS_STRING(v0)) return numeric_from_i64(0);
    const char* s = AS_CSTRING(v0);
    while (*s == ' ' || *s == '\t') s++;
    char buf[160];
    int n = 0;
    const char* p = s;
    if (*p == '+' || *p == '-') { if (n < 159) buf[n++] = *p; p++; }
    bool anyDigit = false;
    while (*p >= '0' && *p <= '9') { if (n < 159) buf[n++] = *p; p++; anyDigit = true; }
    if (*p == '.') {
        if (n < 159) buf[n++] = '.'; p++;
        while (*p >= '0' && *p <= '9') { if (n < 159) buf[n++] = *p; p++; anyDigit = true; }
    }
    /* Exposant optionnel : e/E ou d/D (Fortran) suivi d'un signe et de chiffres. */
    if (anyDigit && (*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D')) {
        const char* q = p + 1;
        int savedN = n;
        char eb[40]; int en = 0;
        eb[en++] = 'E';
        if (*q == '+' || *q == '-') { eb[en++] = *q; q++; }
        bool expDigit = false;
        while (*q >= '0' && *q <= '9') { if (en < 39) eb[en++] = *q; q++; expDigit = true; }
        if (expDigit) {
            for (int i = 0; i < en && n < 159; i++) buf[n++] = eb[i];
        } else {
            n = savedN;  /* 'e' isolé : pas un exposant, on l'ignore */
        }
    }
    if (!anyDigit) return numeric_from_i64(0);
    buf[n] = '\0';
    return numeric_from_string(buf);
}

/* ChaineVersReel(s) / ValR(s) - string to real */
static Value fn_chaine_vers_reel(int argCount, Value* args) {
    (void)argCount;
    /*  — deballe un nom nu de widget (..defaultProperty) avant de
     * parser : ChaineVersReel(SAI) == ChaineVersReel(SAI..Valeur).
     * Cible scalaire only, idempotent sur les chaines (no-op). */
    Value v0 = owl_instance_default_unwrap(args[0]);
    if (!IS_STRING(v0)) return REAL_VAL(0.0);
    const char* s = AS_CSTRING(v0);
    char* end;
    double val = strtod(s, &end);
    if (end == s) {
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "ChaineVersReel",
                              "native", "ChaineVersReel",
                              "value", s,
                              "expected", "reel",
                              NULL);
        return REAL_VAL(0.0);
    }
    owl_clear_error();
    return REAL_VAL(val);
}

/* EstNumérique — validation STRICTE plein-chaîne (cf. ).
 * Divergence assumée du runtime canon laxiste (qui accepte "12a", "3plus2"…).
 * Contrat OWL : après trim des espaces de bord, la chaîne ENTIÈRE doit être
 * un nombre décimal valide. Grammaire :
 *     ws* [+-]? ( D+ ('.' D*)? | '.' D+ ) ws*
 * avec au moins un chiffre. Séparateur décimal = point (convention OWL).
 * Pas d'exposant, pas de préfixe 0x/0n/0m, pas de séparateur de milliers.
 * La virgule décimale n'est pas reconnue ("1,5" -> Faux). */
static bool string_is_strict_number(const char* s, int len) {
    int i = 0, j = len;
    while (i < j && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'
                     || s[i] == '\n' || s[i] == '\v' || s[i] == '\f')) i++;
    while (j > i && (s[j-1] == ' ' || s[j-1] == '\t' || s[j-1] == '\r'
                     || s[j-1] == '\n' || s[j-1] == '\v' || s[j-1] == '\f')) j--;
    if (i >= j) return false;
    if (s[i] == '+' || s[i] == '-') i++;
    bool anyDigit = false;
    while (i < j && s[i] >= '0' && s[i] <= '9') { i++; anyDigit = true; }
    /* Le point n'est un séparateur décimal que s'il est suivi d'un chiffre
     * (aligné sur la grammaire numérique du scanner OWL : "5." n'est pas un
     * réel, le point orphelin n'est pas consommé). */
    if (i < j && s[i] == '.' && i + 1 < j && s[i+1] >= '0' && s[i+1] <= '9') {
        i++;
        while (i < j && s[i] >= '0' && s[i] <= '9') { i++; anyDigit = true; }
    }
    if (!anyDigit) return false;
    return i == j;  /* toute la chaîne trimmée doit être consommée */
}

/* EstNumérique(v) — Vrai si v est déjà numérique, ou une chaîne dont le
 * contenu est intégralement un nombre décimal OWL valide (cf. ). */
static Value fn_est_numerique(int argCount, Value* args) {
    (void)argCount;
    Value v0 = owl_instance_default_unwrap(args[0]);
    if (IS_NUMBER(v0)) return BOOL_VAL(true);
    if (IS_STRING(v0)) {
        ObjString* s = AS_STRING(v0);
        return BOOL_VAL(string_is_strict_number(s->chars, s->length));
    }
    return BOOL_VAL(false);
}

/* fn_numerique_vers_chaine retire — la prefix method .VersChaine() / .ToString()
 * passe maintenant par fn_chaine_formate (cf. owl_numeric_to_string), aligné
 * sur le canon printf-like et compatible avec le type Numérique 2024. */

/* ================================================================
 * BASE64 ENCODE / DECODE
 * ================================================================ */

/* Formats base64 (param 'format' de ChaîneVersBase64 / Base64VersChaîne).
 * Doc 1000022258 (Encode) : constantes encodeBASE64 / encodeBASE64URL.
 * encodeBASE64SansRC non exposée : OWL n'insère jamais de RC, elle serait
 * un synonyme exact de encodeBASE64 (le décodeur, lui, tolère les RC en
 * entrée pour lire du base64 césuré produit par un tiers). */
#define ENCODE_BASE64      0
#define ENCODE_BASE64_URL  1

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
/* RFC 4648 §5 : alphabet URL/filename-safe, sans remplissage. */
static const char b64url_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Lit le param optionnel 'format'. Renvoie -1 (et pose l'erreur) si invalide. */
static int b64_format_arg(int argCount, Value* args, const char* nativeName) {
    if (argCount < 2) return ENCODE_BASE64;
    int fmt = IS_INTEGER(args[1]) ? (int)AS_INTEGER(args[1]) : -1;
    if (fmt != ENCODE_BASE64 && fmt != ENCODE_BASE64_URL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, nativeName, "format",
                              "attendu encodeBASE64 ou encodeBASE64URL", NULL);
        return -1;
    }
    return fmt;
}

static Value fn_string_to_base64(int argCount, Value* args) {
    ObjString* s = to_string(args[0]);   /*  : chaîne OU buffer (coerce lossless) */
    if (s == NULL) return NIL_VAL;
    int fmt = b64_format_arg(argCount, args, "ChaineVersBase64");
    if (fmt < 0) return NIL_VAL;
    int urlmode = (fmt == ENCODE_BASE64_URL);
    const char* table = urlmode ? b64url_table : b64_table;

    const unsigned char* src = (const unsigned char*)s->chars;
    int slen = s->length;
    int outLen = 4 * ((slen + 2) / 3);
    char* out = malloc(outLen + 1);
    int j = 0;
    for (int i = 0; i < slen; i += 3) {
        int a = src[i];
        int b = (i + 1 < slen) ? src[i + 1] : 0;
        int c = (i + 2 < slen) ? src[i + 2] : 0;
        out[j++] = table[(a >> 2) & 0x3F];
        out[j++] = table[((a & 3) << 4) | ((b >> 4) & 0xF)];
        if (i + 1 < slen)      out[j++] = table[((b & 0xF) << 2) | ((c >> 6) & 3)];
        else if (!urlmode)     out[j++] = '=';
        if (i + 2 < slen)      out[j++] = table[c & 0x3F];
        else if (!urlmode)     out[j++] = '=';
    }
    out[j] = '\0';
    ObjString* result = obj_copy_string(out, j);
    free(out);
    return OBJ_VAL(result);
}

/* Décodage strict : chaque alphabet refuse les caractères de l'autre. Ce
 * décodeur voit des jetons venus du réseau — pas de tolérance silencieuse,
 * pas de lecture hors tampon. */
static int b64_decode_char(char c, int urlmode) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (urlmode) {
        if (c == '-') return 62;
        if (c == '_') return 63;
    } else {
        if (c == '+') return 62;
        if (c == '/') return 63;
    }
    return -1;
}

static Value fn_base64_to_string(int argCount, Value* args) {
    ObjString* s = to_string(args[0]);   /*  : chaîne OU buffer (coerce) */
    if (s == NULL) return NIL_VAL;
    int fmt = b64_format_arg(argCount, args, "Base64VersChaine");
    if (fmt < 0) return NIL_VAL;
    int urlmode = (fmt == ENCODE_BASE64_URL);

    int slen = s->length;
    char* out = malloc((size_t)slen + 1);
    int j = 0;
    unsigned int acc = 0;
    int nbits = 0, ngroups = 0, npad = 0;
    const char* reason = NULL;

    for (int i = 0; i < slen && reason == NULL; i++) {
        char c = s->chars[i];
        if (!urlmode && (c == '\r' || c == '\n')) continue;  /* base64 césuré */
        if (c == '=') { npad++; continue; }
        if (npad > 0) { reason = "caractere apres le remplissage '='"; break; }
        int v = b64_decode_char(c, urlmode);
        if (v < 0) {
            reason = urlmode ? "caractere hors de l'alphabet base64 URL (A-Z a-z 0-9 - _)"
                             : "caractere hors de l'alphabet base64 (A-Z a-z 0-9 + /)";
            break;
        }
        acc = (acc << 6) | (unsigned int)v;
        nbits += 6;
        ngroups++;
        if (nbits >= 8) { nbits -= 8; out[j++] = (char)((acc >> nbits) & 0xFF); }
    }
    if (reason == NULL) {
        if (npad > 2)                             reason = "remplissage '=' surnumeraire";
        else if (ngroups % 4 == 1)                reason = "longueur invalide";
        else if (npad > 0 && (ngroups + npad) % 4 != 0) reason = "longueur invalide";
    }
    if (reason != NULL) {
        free(out);
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Base64VersChaine",
                              urlmode ? "base64 (encodeBASE64URL)" : "base64 (encodeBASE64)",
                              reason, NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    ObjString* result = obj_copy_string(out, j);
    free(out);
    return OBJ_VAL(result);
}

/* ================================================================
 * URL ENCODE / DECODE
 * ================================================================ */

/* URLEncode(s [, mode]) — Doc 3024040.
 * Modes (constantes encodeURL*) :
 *   encodeURLDepuisUTF8 (défaut, =0) : preserve unreserved + reserved URL
 *     separators (:/?#[]@ !$&'()*+,;=). Espace -> %20. Pour encoder une URL
 *     entière en gardant sa structure.
 *   encodeURLDepuisAnsi (=1) : alias de DepuisUTF8 en OWL (UTF-8 natif,
 *     pas de transcodage).
 *   encodeURLDepuisUnicode (=2) : alias de DepuisUTF8 en OWL.
 *   encodeURLParamètre (=3) : encode TOUT sauf unreserved (RFC 3986). Pour
 *     passer une URL entière en paramètre d'une autre URL. */
#define ENCODE_URL_DEPUIS_UTF8     0
#define ENCODE_URL_DEPUIS_ANSI     1
#define ENCODE_URL_DEPUIS_UNICODE  2
#define ENCODE_URL_PARAMETRE       3

static Value fn_url_encode(int argCount, Value* args) {
    int mode = ENCODE_URL_DEPUIS_UTF8;
    if (argCount >= 2 && IS_INTEGER(args[1])) mode = (int)AS_INTEGER(args[1]);

    ObjString* s = to_string(args[0]);   /*  : chaîne OU buffer (coerce) */
    if (s == NULL) return NIL_VAL;
    char* out = malloc(s->length * 3 + 1);
    int j = 0;
    for (int i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->chars[i];
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        bool keep;
        if (mode == ENCODE_URL_PARAMETRE) {
            keep = unreserved;
        } else {
            /* Modes standard : préserver les séparateurs RFC 3986 reserved
             * (gen-delims + sub-delims) qui structurent une URL valide.
             * L'espace est exclu (encodé en %20). */
            keep = unreserved
                || c == ':' || c == '/' || c == '?' || c == '#'
                || c == '[' || c == ']' || c == '@'
                || c == '!' || c == '$' || c == '&' || c == '\''
                || c == '(' || c == ')' || c == '*' || c == '+'
                || c == ',' || c == ';' || c == '=';
        }
        if (keep) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = "0123456789ABCDEF"[c >> 4];
            out[j++] = "0123456789ABCDEF"[c & 0xF];
        }
    }
    out[j] = '\0';
    ObjString* result = obj_copy_string(out, j);
    free(out);
    return OBJ_VAL(result);
}

static Value fn_url_decode(int argCount, Value* args) {
    (void)argCount;
    ObjString* s = to_string(args[0]);   /*  : chaîne OU buffer (coerce) */
    if (s == NULL) return NIL_VAL;
    char* out = malloc(s->length + 1);
    int j = 0;
    for (int i = 0; i < s->length; i++) {
        if (s->chars[i] == '%' && i + 2 < s->length) {
            char hex[3] = { s->chars[i + 1], s->chars[i + 2], '\0' };
            out[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (s->chars[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = s->chars[i];
        }
    }
    ObjString* result = obj_copy_string(out, j);
    free(out);
    return OBJ_VAL(result);
}

/* ================================================================
 * HEXA ENCODE / DECODE
 * ================================================================ */

static Value fn_string_to_hex(int argCount, Value* args) {
    (void)argCount;
    ObjString* s = to_string(args[0]);   /*  : chaîne OU buffer (coerce lossless) */
    if (s == NULL) return NIL_VAL;
    char* out = malloc(s->length * 2 + 1);
    for (int i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->chars[i];
        out[i * 2]     = "0123456789ABCDEF"[c >> 4];
        out[i * 2 + 1] = "0123456789ABCDEF"[c & 0xF];
    }
    out[s->length * 2] = '\0';
    ObjString* result = obj_copy_string(out, s->length * 2);
    free(out);
    return OBJ_VAL(result);
}

/* CRC-32/ISO-HDLC (poly reflechi 0xEDB88320 = miroir de 0x04C11DB7, init
 * 0xFFFFFFFF, entree+sortie reflechies, xorout 0xFFFFFFFF). C'est le CRC-32
 * canonique de zlib/zip/PNG. La valeur transportee via <CRC Partiel> est la
 * forme deja xoree (semantique zlib) : passer 0 = calcul frais, passer le
 * resultat d'un fragment precedent = chaînage (crc32(b, crc32(a)) ==
 * crc32(a & b)). Calcul sur les octets reels OWL (UTF-8 / buffer brut),
 * divergence numerique assumee vs le canon qui somme sur UTF-16LE — meme
 * logique que Taille (octets, pas caracteres). */
static uint32_t owl_crc32_update(uint32_t crc, const unsigned char* buf, size_t len) {
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static Value fn_calcule_crc32(int argCount, Value* args) {
    ObjString* s = to_string(args[0]);   /* chaîne OU buffer (octets reels) */
    if (s == NULL) return NIL_VAL;
    uint32_t crc = 0;
    if (argCount >= 2 && !IS_NIL(args[1])) crc = (uint32_t)(int64_t)AS_NUMBER(args[1]);
    crc = owl_crc32_update(crc, (const unsigned char*)s->chars, (size_t)s->length);
    return INTEGER_VAL((int64_t)crc);
}

static Value fn_hex_to_string(int argCount, Value* args) {
    (void)argCount;
    ObjString* s = to_string(args[0]);   /*  : chaîne OU buffer (coerce) */
    if (s == NULL) return NIL_VAL;
    int outLen = s->length / 2;
    char* out = malloc(outLen + 1);
    for (int i = 0; i < outLen; i++) {
        char hex[3] = { s->chars[i * 2], s->chars[i * 2 + 1], '\0' };
        out[i] = (char)strtol(hex, NULL, 16);
    }
    out[outLen] = '\0';
    ObjString* result = obj_copy_string(out, outLen);
    free(out);
    return OBJ_VAL(result);
}

/* ================================================================
 * CHARSET CODECS (ChaineVersBuffer / BufferVersChaine)
 * ================================================================
 *
 * Frontiere charset du modele sandwich : le texte (chaine) est de l'UTF-8
 * abstrait, l'encodage est une propriete des octets (buffer). Les codecs
 * ci-dessous transcodent codepoint-par-codepoint entre l'UTF-8 interne et
 * un codepage cible. Caractere non representable -> octet 63 ('?').
 * ---------------------------------------------------------------- */

/* Table Windows-1252 pour la zone 0x80-0x9F (32 entrees). Hors de cette
 * zone, Windows-1252 == Latin-1 (codepoint == octet). Les positions non
 * assignees (0x81, 0x8D, 0x8F, 0x90, 0x9D) se mappent sur le codepoint
 * egal a l'octet (auto-map round-trippable, comme WHATWG). */
static const unsigned int CP1252_HIGH[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, /* 80-87 */
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, /* 88-8F */
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, /* 90-97 */
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178  /* 98-9F */
};

/* Tables OEM/DOS pour la zone 0x80-0xFF (128 entrees chacune). Hors de
 * cette zone (0x00-0x7F), tout codepage DOS est identique a l'ASCII
 * (octet == codepoint). Sources officielles :
 * unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP850.TXT et CP437.TXT.
 * CP850 et CP437 partagent la zone accentuee 0x80-0xA7 mais divergent
 * au-dela (CP850 = lettres latines etendues europeennes ; CP437 = grec
 * et symboles maths). Codepoint non present a l'encode -> octet 63 ('?'). */
static const unsigned int CP850_HIGH[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80-87 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88-8F */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90-97 */
    0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192, /* 98-9F */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0-A7 */
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8-AF */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0, /* B0-B7 */
    0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510, /* B8-BF */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3, /* C0-C7 */
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4, /* C8-CF */
    0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE, /* D0-D7 */
    0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580, /* D8-DF */
    0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE, /* E0-E7 */
    0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4, /* E8-EF */
    0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8, /* F0-F7 */
    0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0  /* F8-FF */
};

static const unsigned int CP437_HIGH[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80-87 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88-8F */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90-97 */
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, /* 98-9F */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0-A7 */
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8-AF */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, /* B0-B7 */
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, /* B8-BF */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, /* C0-C7 */
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, /* C8-CF */
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, /* D0-D7 */
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, /* D8-DF */
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, /* E0-E7 */
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, /* E8-EF */
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, /* F0-F7 */
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0  /* F8-FF */
};

/* Decode le codepoint UTF-8 a la position *i, avance *i sur l'octet
 * suivant. Sequence invalide -> consomme 1 octet, renvoie 0xFFFD. */
static unsigned int codec_utf8_next(const char* s, int len, int* i) {
    unsigned char c = (unsigned char)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0 && *i + 1 < len) {
        unsigned int cp = ((c & 0x1F) << 6) | ((unsigned char)s[*i + 1] & 0x3F);
        *i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && *i + 2 < len) {
        unsigned int cp = ((c & 0x0F) << 12) |
                          (((unsigned char)s[*i + 1] & 0x3F) << 6) |
                          ((unsigned char)s[*i + 2] & 0x3F);
        *i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && *i + 3 < len) {
        unsigned int cp = ((c & 0x07) << 18) |
                          (((unsigned char)s[*i + 1] & 0x3F) << 12) |
                          (((unsigned char)s[*i + 2] & 0x3F) << 6) |
                          ((unsigned char)s[*i + 3] & 0x3F);
        *i += 4; return cp;
    }
    (*i)++; return 0xFFFD;
}

/* Encode un codepoint en UTF-8 dans out (max 4 octets). Renvoie le
 * nombre d'octets ecrits. */
static int codec_cp_to_utf8(unsigned int cp, unsigned char* out) {
    if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Mappe un codepoint vers un octet Latin-1 (ISO-8859-1). Non
 * representable (cp >= 256) -> 63 ('?'). */
static unsigned char codec_cp_to_latin1(unsigned int cp) {
    return (cp < 256) ? (unsigned char)cp : (unsigned char)'?';
}

/* Mappe un codepoint vers un octet Windows-1252. Non representable -> 63. */
static unsigned char codec_cp_to_cp1252(unsigned int cp) {
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return (unsigned char)cp;
    for (int k = 0; k < 32; k++) {
        if (CP1252_HIGH[k] == cp) return (unsigned char)(0x80 + k);
    }
    return (unsigned char)'?';
}

/* Decode un octet Windows-1252 en codepoint. */
static unsigned int codec_cp1252_to_cp(unsigned char b) {
    if (b >= 0x80 && b <= 0x9F) return CP1252_HIGH[b - 0x80];
    return b;  /* ASCII + zone Latin-1 0xA0-0xFF identique */
}

/* Mappe un codepoint vers un octet OEM/DOS via la table 0x80-0xFF fournie
 * (CP850_HIGH ou CP437_HIGH). cp < 0x80 -> passthrough ASCII ; sinon
 * recherche inverse dans la table ; absent -> 63 ('?'). */
static unsigned char codec_cp_to_oem(unsigned int cp, const unsigned int* high) {
    if (cp < 0x80) return (unsigned char)cp;
    for (int k = 0; k < 128; k++) {
        if (high[k] == cp) return (unsigned char)(0x80 + k);
    }
    return (unsigned char)'?';
}

/* Decode un octet OEM/DOS en codepoint via la table fournie. */
static unsigned int codec_oem_to_cp(unsigned char b, const unsigned int* high) {
    return (b < 0x80) ? b : high[b - 0x80];
}

/* Encode la chaine UTF-8 `s` (len octets) vers `enc` dans un buffer C
 * fraichement alloue (a free par l'appelant). *outLen recoit la taille. */
unsigned char* owl_codec_encode(const char* s, int len, int enc, int* outLen) {
    if (enc == (int)OWL_ENC_UTF8) {
        unsigned char* out = malloc(len > 0 ? len : 1);
        if (len > 0) memcpy(out, s, len);
        *outLen = len;
        return out;
    }
    /* UTF-16 : jusqu'a 4 octets par codepoint ; codepages 8 bits : 1 octet. */
    int cap = (enc == (int)OWL_ENC_UTF16LE || enc == (int)OWL_ENC_UTF16BE)
                  ? len * 4 + 4 : len + 1;
    unsigned char* out = malloc(cap);
    int j = 0, i = 0;
    while (i < len) {
        unsigned int cp = codec_utf8_next(s, len, &i);
        switch (enc) {
            case (int)OWL_ENC_LATIN1:
                out[j++] = codec_cp_to_latin1(cp);
                break;
            case (int)OWL_ENC_WINDOWS1252:
                out[j++] = codec_cp_to_cp1252(cp);
                break;
            case (int)OWL_ENC_CP850:
                out[j++] = codec_cp_to_oem(cp, CP850_HIGH);
                break;
            case (int)OWL_ENC_CP437:
                out[j++] = codec_cp_to_oem(cp, CP437_HIGH);
                break;
            case (int)OWL_ENC_UTF16LE:
            case (int)OWL_ENC_UTF16BE: {
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = '?';
                if (cp >= 0x10000) {
                    unsigned int v = cp - 0x10000;
                    unsigned int hi = 0xD800 + (v >> 10);
                    unsigned int lo = 0xDC00 + (v & 0x3FF);
                    if (enc == (int)OWL_ENC_UTF16LE) {
                        out[j++] = (unsigned char)(hi & 0xFF); out[j++] = (unsigned char)(hi >> 8);
                        out[j++] = (unsigned char)(lo & 0xFF); out[j++] = (unsigned char)(lo >> 8);
                    } else {
                        out[j++] = (unsigned char)(hi >> 8); out[j++] = (unsigned char)(hi & 0xFF);
                        out[j++] = (unsigned char)(lo >> 8); out[j++] = (unsigned char)(lo & 0xFF);
                    }
                } else {
                    if (enc == (int)OWL_ENC_UTF16LE) {
                        out[j++] = (unsigned char)(cp & 0xFF); out[j++] = (unsigned char)(cp >> 8);
                    } else {
                        out[j++] = (unsigned char)(cp >> 8); out[j++] = (unsigned char)(cp & 0xFF);
                    }
                }
                break;
            }
            default:
                out[j++] = codec_cp_to_latin1(cp);
                break;
        }
    }
    *outLen = j;
    return out;
}

/* Decode `len` octets de `bytes` selon `enc` vers de l'UTF-8 dans un
 * buffer C fraichement alloue (a free par l'appelant). */
unsigned char* owl_codec_decode(const unsigned char* bytes, int len, int enc, int* outLen) {
    if (enc == (int)OWL_ENC_UTF8) {
        unsigned char* out = malloc(len > 0 ? len : 1);
        if (len > 0) memcpy(out, bytes, len);
        *outLen = len;
        return out;
    }
    /* Pire cas UTF-8 : 3 octets par codepoint (BMP) ou par unite UTF-16. */
    unsigned char* out = malloc(len * 3 + 4);
    int j = 0;
    if (enc == (int)OWL_ENC_UTF16LE || enc == (int)OWL_ENC_UTF16BE) {
        int i = 0;
        while (i + 1 < len) {
            unsigned int u = (enc == (int)OWL_ENC_UTF16LE)
                ? (bytes[i] | (bytes[i + 1] << 8))
                : ((bytes[i] << 8) | bytes[i + 1]);
            i += 2;
            unsigned int cp = u;
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
                unsigned int lo = (enc == (int)OWL_ENC_UTF16LE)
                    ? (bytes[i] | (bytes[i + 1] << 8))
                    : ((bytes[i] << 8) | bytes[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            j += codec_cp_to_utf8(cp, out + j);
        }
    } else {
        for (int i = 0; i < len; i++) {
            unsigned int cp;
            if (enc == (int)OWL_ENC_WINDOWS1252)   cp = codec_cp1252_to_cp(bytes[i]);
            else if (enc == (int)OWL_ENC_CP850)    cp = codec_oem_to_cp(bytes[i], CP850_HIGH);
            else if (enc == (int)OWL_ENC_CP437)    cp = codec_oem_to_cp(bytes[i], CP437_HIGH);
            else                                   cp = bytes[i];  /* Latin-1 : octet == codepoint */
            j += codec_cp_to_utf8(cp, out + j);
        }
    }
    *outLen = j;
    return out;
}

/* Lit l'argument encodage optionnel ; defaut encUTF8. */
static int codec_arg_encoding(int argCount, Value* args, int idx) {
    if (argCount > idx && IS_INTEGER(args[idx])) return (int)AS_INTEGER(args[idx]);
    return (int)OWL_ENC_UTF8;
}

/* ================================================================
 * CHARACTER / ASCII
 * ================================================================ */

/* Caract(n) / Chr(n) - ASCII/Unicode code point to single character */
static Value fn_caract(int argCount, Value* args) {
    (void)argCount;
    int code = 0;
    if (IS_INTEGER(args[0])) code = (int)AS_INTEGER(args[0]);
    else if (IS_REAL(args[0])) code = (int)AS_REAL(args[0]);
    if (code < 0) code = 0;
    /* UTF-8 encode */
    char buf[4];
    int len = 0;
    if (code < 0x80) {
        buf[0] = (char)code;
        len = 1;
    } else if (code < 0x800) {
        buf[0] = (char)(0xC0 | (code >> 6));
        buf[1] = (char)(0x80 | (code & 0x3F));
        len = 2;
    } else if (code < 0x10000) {
        buf[0] = (char)(0xE0 | (code >> 12));
        buf[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (code & 0x3F));
        len = 3;
    } else {
        buf[0] = (char)(0xF0 | (code >> 18));
        buf[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (code & 0x3F));
        len = 4;
    }
    return OBJ_VAL(obj_copy_string(buf, len));
}

/* Asc(c) / CharCode(c) - first character to ASCII/Unicode code point */
static Value fn_asc(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return INTEGER_VAL(0);
    ObjString* s = AS_STRING(args[0]);
    if (s->length == 0) return INTEGER_VAL(0);
    unsigned char c = (unsigned char)s->chars[0];
    if (c < 0x80) return INTEGER_VAL(c);
    /* UTF-8 decode */
    int code = 0;
    if ((c & 0xE0) == 0xC0 && s->length >= 2) {
        code = ((c & 0x1F) << 6) | ((unsigned char)s->chars[1] & 0x3F);
    } else if ((c & 0xF0) == 0xE0 && s->length >= 3) {
        code = ((c & 0x0F) << 12) | (((unsigned char)s->chars[1] & 0x3F) << 6) |
               ((unsigned char)s->chars[2] & 0x3F);
    } else if ((c & 0xF8) == 0xF0 && s->length >= 4) {
        code = ((c & 0x07) << 18) | (((unsigned char)s->chars[1] & 0x3F) << 12) |
               (((unsigned char)s->chars[2] & 0x3F) << 6) |
               ((unsigned char)s->chars[3] & 0x3F);
    }
    return INTEGER_VAL(code);
}

/* CaractUnicode(code) - point de code Unicode -> caractere (encodage UTF-8).
 * Contrairement a Caract (qui clampe et n'encode que le point de code brut),
 * cette native valide strictement le domaine Unicode : hors [0, 0x10FFFF] ou
 * dans la zone surrogate [0xD800, 0xDFFF] -> erreur non fatale + chaine vide. */
static Value fn_caract_unicode(int argCount, Value* args) {
    (void)argCount;
    long long code = -1;
    if (IS_INTEGER(args[0])) code = AS_INTEGER(args[0]);
    else if (IS_REAL(args[0])) code = (long long)AS_REAL(args[0]);
    if (code < 0 || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF)) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "CaractUnicode",
                              "native", "CaractUnicode",
                              "arg", "code",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    char buf[4];
    int len = 0;
    if (code < 0x80) {
        buf[0] = (char)code;
        len = 1;
    } else if (code < 0x800) {
        buf[0] = (char)(0xC0 | (code >> 6));
        buf[1] = (char)(0x80 | (code & 0x3F));
        len = 2;
    } else if (code < 0x10000) {
        buf[0] = (char)(0xE0 | (code >> 12));
        buf[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (code & 0x3F));
        len = 3;
    } else {
        buf[0] = (char)(0xF0 | (code >> 18));
        buf[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (code & 0x3F));
        len = 4;
    }
    owl_clear_error();
    return OBJ_VAL(obj_copy_string(buf, len));
}

/* HexaVersEntier(s) - chaine hexadecimale -> entier.
 * Parse STRICT aligne sur le canon :
 *   - espaces de tete et de fin ignores ;
 *   - prefixe 0x/0X optionnel tolere ;
 *   - casse indifferente (a-f / A-F) ;
 *   - chaine vide (ou seulement des espaces) -> 0, SANS erreur ;
 *   - tout caractere non-hexa dans la partie significative -> erreur
 *     (soft-error ErreurDetectee=Vrai, retour 0). Pas de parse partiel :
 *     "1FZZ" est rejete (strtoull seul s'arreterait sans signal).
 * Depassement : le type entier OWL est un int64 signe (, contrat
 * strict, pas de wrap silencieux). Une valeur >= 2^63 (ex. "FF..FF" sur
 * 16 chiffres) ne se represente pas -> soft-error hors plage, retour 0
 * plutot qu'un wrap negatif silencieux. */
static bool hexint_is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}
static bool hexint_is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static Value fn_hexa_vers_entier(int argCount, Value* args) {
    (void)argCount;
    Value v0 = owl_instance_default_unwrap(args[0]);
    if (!IS_STRING(v0)) return INTEGER_VAL(0);
    const char* s = AS_CSTRING(v0);
    const char* p = s;
    const char* e = s + strlen(s);
    /* trim espaces de tete et de fin */
    while (p < e && hexint_is_ws((unsigned char)*p)) p++;
    while (e > p && hexint_is_ws((unsigned char)e[-1])) e--;
    /* prefixe 0x / 0X optionnel */
    if (e - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    /* partie significative vide -> 0 sans erreur */
    if (p == e) {
        owl_clear_error();
        return INTEGER_VAL(0);
    }
    /* validation stricte : tout caractere doit etre hexa */
    for (const char* q = p; q < e; q++) {
        if (!hexint_is_hex((unsigned char)*q)) {
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "HexaVersEntier",
                                  "native", "HexaVersEntier",
                                  "value", s,
                                  "expected", "chaîne hexadécimale",
                                  NULL);
            return INTEGER_VAL(0);
        }
    }
    errno = 0;
    unsigned long long val = strtoull(p, NULL, 16);
    if (errno == ERANGE || val > (unsigned long long)INT64_MAX) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "HexaVersEntier",
                              "native", "HexaVersEntier",
                              "arg", "s",
                              NULL);
        return INTEGER_VAL(0);
    }
    owl_clear_error();
    return INTEGER_VAL((int64_t)val);
}

/* ================================================================
 * STRING BUILD / FORMAT
 * ================================================================ */

/* ChaineConstruit(format, p1, p2, ...) / StringBuild(format, p1, p2, ...)
 * Replaces %1, %2, ... %N with corresponding parameters.
 * Canon: %1 = first param after format string. */
static Value fn_chaine_construit(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return NIL_VAL;
    ObjString* fmt = AS_STRING(args[0]);

    /* canon 2nd syntax: ChaîneConstruit(fmt, <tableau>) — the single array
     * argument is spread as positional parameters. */
    Value* effArgs = args + 1;
    int paramCount = argCount - 1;
    ObjArray* spreadArr = NULL;
    if (paramCount == 1 && IS_ARRAY(args[1])) {
        spreadArr = AS_ARRAY(args[1]);
        paramCount = spreadArr->elements.count;
        effArgs = spreadArr->elements.values;
    }

    /* Pre-stringify all params.
     *  : route les types non-primitifs (durée, date, heure, dateheure,
     * buffer, monétaire, numérique...) via value_to_string — même helper que
     * Trace(x) et OP_CONCAT, garantit un format unique. Avant ce fix, tous
     * les types hors STRING/INTEGER/REAL/BOOL tombaient sur "" → interpolation
     * vide.
     *
     * GC safety : value_to_string retourne un ObjString fresh non-rooté.
     * L'alloc finale (obj_copy_string sur `out`) et tout obj_copy_string
     * intermédiaire (param i+1) peuvent déclencher GC qui collecte l'ObjString
     * i précédent. Pattern canon (cf OP_ADD vm.c:2383+) : push chaque
     * ObjString temp sur la VM stack pour le rooter, pop tout à la fin. */
    char* paramStrs[16];
    int paramLens[16];
    bool paramOwned[16];   /* true = malloc'd (free à la fin), false = GC-managed ou littéral */
    int gcRoots = 0;
    if (paramCount > 16) paramCount = 16;

    for (int i = 0; i < paramCount; i++) {
        Value v = effArgs[i];
        if (IS_INTEGER(v)) {
            paramStrs[i] = malloc(32);
            paramLens[i] = snprintf(paramStrs[i], 32, "%lld", (long long)AS_INTEGER(v));
            paramOwned[i] = true;
        } else if (IS_REAL(v)) {
            paramStrs[i] = malloc(64);
            paramLens[i] = owl_real_to_str(AS_REAL(v), paramStrs[i], 64);
            paramOwned[i] = true;
        } else {
            /* STRING, BOOL, NIL, Date, Heure, DateHeure, Durée, Buffer,
             * Monétaire, Numérique, etc. → value_to_string fait tout. */
            ObjString* s = to_string(v);
            vm_push(OBJ_VAL(s));   /* root pendant la phase build */
            gcRoots++;
            paramStrs[i] = s->chars;
            paramLens[i] = s->length;
            paramOwned[i] = false;
        }
    }

    /* Build result */
    int cap = fmt->length * 2 + 256;
    char* out = malloc(cap);
    int olen = 0;

    for (int i = 0; i < fmt->length; i++) {
        if (fmt->chars[i] == '%' && i + 1 < fmt->length &&
            fmt->chars[i+1] >= '1' && fmt->chars[i+1] <= '9') {
            /* Parse param index: %1 through %16 */
            int idx = 0;
            int j = i + 1;
            while (j < fmt->length && fmt->chars[j] >= '0' && fmt->chars[j] <= '9') {
                idx = idx * 10 + (fmt->chars[j] - '0');
                j++;
            }
            idx--; /* %1 = index 0 */
            if (idx >= 0 && idx < paramCount) {
                while (olen + paramLens[idx] >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                memcpy(out + olen, paramStrs[idx], paramLens[idx]);
                olen += paramLens[idx];
            }
            i = j - 1; /* skip consumed chars */
        } else {
            if (olen + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
            out[olen++] = fmt->chars[i];
        }
    }

    ObjString* result = obj_copy_string(out, olen);

    /* Free malloc'd param strings (INTEGER / REAL path). Les autres types
     * (route value_to_string) sont GC-managed et ont été rootés via vm_push
     * pendant la phase build — les pop maintenant que `result` est alloué. */
    for (int i = 0; i < paramCount; i++) {
        if (paramOwned[i]) free(paramStrs[i]);
    }
    while (gcRoots-- > 0) vm_pop();
    free(out);
    return OBJ_VAL(result);
}

/* ================================================================
 * STRING PREDICATES
 * ================================================================ */

/* CommencePar(s, prefix) / StartsWith(s, prefix) */
/* Normalize a UTF-8 buffer per cc* flags (lowercase + strip accents).
 * Writes at most dstCap bytes into dst, returns number of bytes written. */
static int cc_normalize(const char* src, int len, char* dst, int dstCap, int flags) {
    if (len > dstCap) len = dstCap;
    memcpy(dst, src, len);
    int cur = len;
    if (flags & CC_SANS_CASSE) {
        char tmp[4096];
        int n = cur < (int)sizeof(tmp) ? cur : (int)sizeof(tmp);
        owl_case_convert(dst, n, tmp, false);
        memcpy(dst, tmp, n);
    }
    if ((flags & CC_SANS_ACCENT) || (flags & CC_ORDRE_LEXICOGRAPHIQUE)) {
        char tmp[4096];
        int n = scanner_strip_accents(dst, cur, tmp, (int)sizeof(tmp));
        if (n > dstCap) n = dstCap;
        memcpy(dst, tmp, n);
        cur = n;
    }
    return cur;
}

/* Longueur (octets) du prefixe de [s,sLen) dont la normalisation cc* egale la
 * needle DEJA normalisee (pnorm/pn), ou -1 si aucun prefixe ne matche.
 * La normalisation (strip accents) peut changer la longueur : on essaie des
 * fenetres croissantes de s jusqu'a egaler la longueur normalisee de la needle. */
static int cc_prefix_span(const char* s, int sLen, const char* pnorm, int pn, int flags) {
    char sbuf[4096];
    int maxTry = sLen < pn * 2 + 4 ? sLen : pn * 2 + 4;
    for (int take = pn; take <= maxTry; take++) {
        int sn = cc_normalize(s, take, sbuf, (int)sizeof(sbuf), flags);
        if (sn == pn && memcmp(sbuf, pnorm, (size_t)pn) == 0) return take;
        if (sn > pn) break;
    }
    return -1;
}

/* Check if s starts with p under cc* options. Normalization may change
 * byte length (accents), so we normalize the full p and enough of s to
 * cover p's normalized length. */
static bool cc_prefix_match(const char* s, int sLen, const char* p, int pLen, int flags) {
    if (flags == 0) return pLen <= sLen && memcmp(s, p, pLen) == 0;

    char pbuf[4096];
    int pn = cc_normalize(p, pLen, pbuf, (int)sizeof(pbuf), flags);
    if (pn == 0) return pLen == 0;

    return cc_prefix_span(s, sLen, pbuf, pn, flags) >= 0;
}

/* ChaîneCommencePar(s, prefix_or_array [, options])
 * Spec canon: returns the length of the matching element in CHARACTERS (codepoints)
 * to stay consistent with Taille(), or 0 if no match.
 * 2nd arg may be a single string OR an array of candidate prefixes. */
static Value fn_commence_par(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(0);  /*  garde sous-application */
    if (!IS_STRING(args[0])) return INTEGER_VAL(0);
    ObjString* s = AS_STRING(args[0]);
    int flags = 0;
    if (argCount >= 3 && IS_INTEGER(args[2])) flags = (int)AS_INTEGER(args[2]);

    if (IS_STRING(args[1])) {
        ObjString* prefix = AS_STRING(args[1]);
        if (cc_prefix_match(s->chars, s->length, prefix->chars, prefix->length, flags)) {
            return INTEGER_VAL(utf8_cp_count(prefix->chars, prefix->length));
        }
        return INTEGER_VAL(0);
    }
    if (IS_ARRAY(args[1])) {
        ObjArray* arr = AS_ARRAY(args[1]);
        for (int i = 0; i < arr->elements.count; i++) {
            Value v = arr->elements.values[i];
            if (!IS_STRING(v)) continue;
            ObjString* p = AS_STRING(v);
            if (cc_prefix_match(s->chars, s->length, p->chars, p->length, flags)) {
                return INTEGER_VAL(utf8_cp_count(p->chars, p->length));
            }
        }
        return INTEGER_VAL(0);
    }
    return INTEGER_VAL(0);
}

/* ChaîneFinitPar(s, suffix_or_array [, options]) — symmetrical */
static Value fn_finit_par(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(0);  /*  garde sous-application */
    if (!IS_STRING(args[0])) return INTEGER_VAL(0);
    ObjString* s = AS_STRING(args[0]);
    int flags = 0;
    if (argCount >= 3 && IS_INTEGER(args[2])) flags = (int)AS_INTEGER(args[2]);

    if (IS_STRING(args[1])) {
        ObjString* suffix = AS_STRING(args[1]);
        if (suffix->length > s->length) return INTEGER_VAL(0);
        const char* tail = s->chars + s->length - suffix->length;
        if (cc_prefix_match(tail, suffix->length, suffix->chars, suffix->length, flags)) {
            return INTEGER_VAL(utf8_cp_count(suffix->chars, suffix->length));
        }
        return INTEGER_VAL(0);
    }
    if (IS_ARRAY(args[1])) {
        ObjArray* arr = AS_ARRAY(args[1]);
        for (int i = 0; i < arr->elements.count; i++) {
            Value v = arr->elements.values[i];
            if (!IS_STRING(v)) continue;
            ObjString* p = AS_STRING(v);
            if (p->length > s->length) continue;
            const char* tail = s->chars + s->length - p->length;
            if (cc_prefix_match(tail, p->length, p->chars, p->length, flags)) {
                return INTEGER_VAL(utf8_cp_count(p->chars, p->length));
            }
        }
        return INTEGER_VAL(0);
    }
    return INTEGER_VAL(0);
}

/* Contient(s, search [, options]) / Contains(s, search [, options])
 * canon: 2 formes
 *   Forme 1: Contient(sous-chaîne [, MotComplet/SansCasse])
 *   Forme 2: Contient(tableau_de_chaînes [, options])
 * Retour: Vrai si la chaîne contient la sous-chaîne (ou une des chaînes du tableau).
 */
/* Forward decl: defined further down with ChaîneSupprime helpers (~ ligne 2942) */
static const char* find_with_opts(const char* hay, int haylen,
                                  const char* needle, int needlelen,
                                  int opts, int* matchLen);

static Value fn_contient(int argCount, Value* args) {
    if (argCount < 2) return BOOL_VAL(false);  /*  garde sous-application */
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    ObjString* s = AS_STRING(args[0]);
    int opts = (argCount >= 3 && IS_INTEGER(args[2])) ? (int)AS_INTEGER(args[2]) : 0;

    /* Forme 2: tableau de chaînes — l'option est globale, appliquée à tous
     * les candidats (doc 1410089824). */
    if (IS_ARRAY(args[1])) {
        ObjArray* arr = AS_ARRAY(args[1]);
        for (int i = 0; i < arr->elements.count; i++) {
            Value candidate = arr->elements.values[i];
            if (!IS_STRING(candidate)) continue;
            ObjString* search = AS_STRING(candidate);
            if (search->length == 0) return BOOL_VAL(true);
            int mlen;
            if (find_with_opts(s->chars, s->length,
                               search->chars, search->length, opts, &mlen) != NULL)
                return BOOL_VAL(true);
        }
        return BOOL_VAL(false);
    }

    /* Forme 1: chaîne simple */
    if (!IS_STRING(args[1])) return BOOL_VAL(false);
    ObjString* search = AS_STRING(args[1]);
    if (search->length == 0) return BOOL_VAL(true);
    int mlen;
    return BOOL_VAL(find_with_opts(s->chars, s->length,
                                   search->chars, search->length, opts, &mlen) != NULL);
}

/* ================================================================
 * STRING PADDING
 * ================================================================ */

/* Complet(s, n, car, sens) / Complete(s, n, char, direction)
 * Pads string s to length n with character car.
 * sens: 0 or omitted = right, 1 = left */
static Value fn_complet(int argCount, Value* args) {
    if (argCount < 2) return args[0];  /*  garde sous-application */
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* s = AS_STRING(args[0]);
    int n = to_int(args[1]);
    if (n <= 0) return OBJ_VAL(obj_copy_string("", 0));
    if (n < s->length) {
        /* Tronquer (doc 1410089823: "Alexandre".Complète(4) → "Alex") */
        return OBJ_VAL(obj_copy_string(s->chars, n));
    }
    if (n == s->length) return OBJ_VAL(s);

    char padChar = ' ';
    if (argCount >= 3 && IS_STRING(args[2]) && AS_STRING(args[2])->length > 0)
        padChar = AS_STRING(args[2])->chars[0];

    int padLeft = 0;
    if (argCount >= 4) padLeft = to_int(args[3]);

    int padLen = n - s->length;
    char* buf = malloc(n + 1);
    if (padLeft) {
        memset(buf, padChar, padLen);
        memcpy(buf + padLen, s->chars, s->length);
    } else {
        memcpy(buf, s->chars, s->length);
        memset(buf + s->length, padChar, padLen);
    }
    buf[n] = '\0';
    ObjString* result = obj_copy_string(buf, n);
    free(buf);
    return OBJ_VAL(result);
}

/* RepeteChaine(s, n) / RepeatString(s, n) - repeat string n times */
static Value fn_repete_chaine(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* s = AS_STRING(args[0]);
    int n = to_int(args[1]);
    if (n <= 0) return OBJ_VAL(obj_copy_string("", 0));
    int totalLen = s->length * n;
    char* buf = malloc(totalLen + 1);
    for (int i = 0; i < n; i++) {
        memcpy(buf + i * s->length, s->chars, s->length);
    }
    buf[totalLen] = '\0';
    ObjString* result = obj_copy_string(buf, totalLen);
    free(buf);
    return OBJ_VAL(result);
}

/* Inverse(s) / Reverse(s) — canon: returns the per-codepoint
 * complement to 65535 (XOR 0xFFFF) of each character. NOT a string reversal.
 * OWL strings are UTF-8, so we iterate codepoints, XOR each with 0xFFFF,
 * and re-encode. Codepoints above 0xFFFF are XORed with 0xFFFF too (only
 * the low 16 bits are flipped — matches canon Unicode behaviour). */
static int utf8_decode(const char* s, int len, int* cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)                        { *cp = c;                                            return 1; }
    if ((c & 0xE0) == 0xC0 && len >= 2)  { *cp = ((c & 0x1F) << 6) |  ((unsigned char)s[1] & 0x3F);  return 2; }
    if ((c & 0xF0) == 0xE0 && len >= 3)  { *cp = ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && len >= 4)  { *cp = ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) | (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F); return 4; }
    *cp = c;  /* invalid byte: pass through as-is */
    return 1;
}
static int utf8_encode(int cp, char* out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
/* Inverse(chaîne) — fonction globale. XOR chaque codepoint
 * Unicode avec 0xFFFF (mode Unicode, complement a 65535).
 * Itère codepoint par codepoint en UTF-8 puis réencode.
 *
 * NB : différent de ChaîneInverse qui inverse l'ordre des
 * caractères. La méthode prefix `chaîne.Inverse()` est mappée sur
 * fn_inverse_chaine (= ordre), conformément à la doc method 1000019901.
 *
 * Idempotence : f(f(c)) = c (XOR involution). Roundtrip garanti. */
Value fn_inverse_xor(int argCount, Value* args);
Value fn_inverse_xor(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* s = AS_STRING(args[0]);
    /* Worst case : chaque codepoint ASCII (1 byte) devient un codepoint
     * dans la zone U+FFxx → 3 bytes UTF-8. Donc taille max = 3 * length. */
    char* buf = (char*)malloc((size_t)s->length * 3 + 4);
    if (!buf) return NIL_VAL;
    int outLen = 0;
    int i = 0;
    while (i < s->length) {
        int cp = 0;
        int adv = utf8_decode(s->chars + i, s->length - i, &cp);
        if (adv <= 0) adv = 1;
        int xored = cp ^ 0xFFFF;
        outLen += utf8_encode(xored, buf + outLen);
        i += adv;
    }
    Value result = OBJ_VAL(obj_copy_string(buf, outLen));
    free(buf);
    return result;
}

Value fn_inverse_chaine(int argCount, Value* args);
Value fn_inverse_chaine(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* s = AS_STRING(args[0]);
    /* canon spec : inverse l'ordre des caractères.
     * On opère codepoint par codepoint (UTF-8 aware) pour que "café" devienne
     * "éfac" (4 caractères) et pas un mix de bytes. */
    char* buf = malloc((size_t)s->length + 1);
    if (!buf) return NIL_VAL;
    int outLen = 0;
    /* Collecte les positions de début de chaque codepoint, puis copie en ordre inverse. */
    int starts[4096];
    int count = 0;
    int i = 0;
    while (i < s->length && count < (int)(sizeof(starts) / sizeof(starts[0]))) {
        starts[count++] = i;
        int cp = 0;
        int adv = utf8_decode(s->chars + i, s->length - i, &cp);
        if (adv <= 0) adv = 1; /* defensive */
        i += adv;
    }
    for (int k = count - 1; k >= 0; k--) {
        int start = starts[k];
        int end = (k + 1 < count) ? starts[k + 1] : s->length;
        int len = end - start;
        memcpy(buf + outLen, s->chars + start, len);
        outLen += len;
    }
    Value result = OBJ_VAL(obj_copy_string(buf, outLen));
    free(buf);
    return result;
}

/* ================================================================
 * NumériqueVersChaîne — printf-like formatter (doc 3024024)
 * ================================================================
 *
 * Format string: "[<Cadrage>][<Signe>][<Zéro>][<Taille>][<.Précision>]<Type>[<Millier>]"
 *   <Cadrage>   '-' = left-align (default: right)
 *   <Signe>     '+' = force sign on positives
 *   <Zéro>      '0' = pad left with '0' (only with right-align)
 *   <Taille>    integer < 100 = total width (incl. sign, separators, decimals)
 *   <.Précision> '.N' or ',N' = decimal places (only valid for f/e). The
 *               separator char ('.' or ',') becomes the actual decimal sep
 *               in the output. Default precision for f/e = 6.
 *   <Type>      d|e|f|x|X|o (required). Bin (b) is FormatNumérique-only.
 *   <Millier>   'S' or 's' = activate locale thousands separator
 *
 * Locale: decimal & thousands separators read once via setlocale/localeconv
 * at first use. Override via the OwlConfigureLocale(decSep, thouSep) native.
 *
 * Two parameter shapes accepted (canon + extension):
 *   1) NumériqueVersChaîne(value, "mask")           — printf-like string mask
 *   2) NumériqueVersChaîne(value, formatNumStruct)  — FormatNumérique structured
 *   3) NumériqueVersChaîne(value)                   — defaults (no mask)
 *
 * Hex/oct on negatives diverges from the doc: the doc claims "8001"
 * for (-32767, "3X") but the real behaviour returns "001". OWL aligns on
 * the doc (sane 2's-complement-style) — divergence assumed in VISION §5bis.
 */

/* ---- Locale management ---- */
static char g_locale_dec_sep[8]  = ".";
static char g_locale_thou_sep[8] = " ";
static bool g_locale_initialized = false;

static void owl_locale_init(void) {
    if (g_locale_initialized) return;
    g_locale_initialized = true;
    /* : on lit les separateurs de l'OS pour l'AFFICHAGE localise
     * volontaire (NumériqueVersChaîne), mais on ne laisse PAS LC_NUMERIC
     * flippe au niveau du process — sinon la virgule fuiterait dans toute
     * serialisation reelle via %g (JSON, OOXML, xls...). On sauvegarde le
     * locale courant, on bascule le temps de lire localeconv(), puis on
     * restaure. setlocale(LC_NUMERIC, NULL) renvoie un buffer statique
     * potentiellement reutilise par le setlocale suivant -> on duplique. */
    const char* prev = setlocale(LC_NUMERIC, NULL);
    char prevCopy[128];
    prevCopy[0] = '\0';
    if (prev) snprintf(prevCopy, sizeof(prevCopy), "%s", prev);
    setlocale(LC_NUMERIC, "");
    struct lconv* lc = localeconv();
    if (lc && lc->decimal_point && lc->decimal_point[0]) {
        snprintf(g_locale_dec_sep, sizeof(g_locale_dec_sep), "%s", lc->decimal_point);
    }
    if (lc && lc->thousands_sep && lc->thousands_sep[0]) {
        snprintf(g_locale_thou_sep, sizeof(g_locale_thou_sep), "%s", lc->thousands_sep);
    }
    setlocale(LC_NUMERIC, prevCopy[0] ? prevCopy : "C");
}

/* OwlConfigureLocale(decSep, thouSep) — override locale separators
 * explicitly. Both arguments are strings (multi-byte safe for UTF-8). */
static Value fn_owl_configure_locale(int argCount, Value* args) {
    owl_locale_init();
    if (argCount >= 1 && IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        int n = s->length < (int)sizeof(g_locale_dec_sep) - 1
                ? s->length : (int)sizeof(g_locale_dec_sep) - 1;
        memcpy(g_locale_dec_sep, s->chars, (size_t)n);
        g_locale_dec_sep[n] = 0;
    }
    if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* s = AS_STRING(args[1]);
        int n = s->length < (int)sizeof(g_locale_thou_sep) - 1
                ? s->length : (int)sizeof(g_locale_thou_sep) - 1;
        memcpy(g_locale_thou_sep, s->chars, (size_t)n);
        g_locale_thou_sep[n] = 0;
    }
    return NIL_VAL;
}

/* ---- Internal format spec ---- */
typedef enum {
    WLNUM_TYPE_NONE = 0,
    WLNUM_TYPE_D,    /* decimal int */
    WLNUM_TYPE_F,    /* fixed-point */
    WLNUM_TYPE_E,    /* exponential */
    WLNUM_TYPE_X,    /* hex lower */
    WLNUM_TYPE_XU,   /* hex upper */
    WLNUM_TYPE_O,    /* octal */
    WLNUM_TYPE_B,    /* binary (FormatNumérique only) */
} WLNumType;

typedef enum {
    WLNUM_SIGN_NEG_ONLY = 0,  /* default: only show '-' on negatives */
    WLNUM_SIGN_FORCE,         /* '+' flag: always show sign */
    WLNUM_SIGN_PARENS,        /* fnsParenthèsesSiNégatif */
} WLNumSignMode;

typedef struct {
    bool          leftAlign;     /* '-' flag in mask */
    WLNumSignMode signMode;
    bool          zeroPad;       /* '0' flag */
    int           width;         /* 0 = auto */
    bool          hasPrecision;
    int           precision;
    char          decSep;        /* '.' or ',' from mask */
    WLNumType     type;
    bool          thousandSep;   /* 'S' / 's' suffix */

    /* Extended (FormatNumérique only) */
    char          fillChar;      /* default ' ' */
    bool          uppercase;     /* hex en majuscule via Majuscules */
    const char*   customDecSep;  /* override decimal sep multi-byte (UTF-8); NULL = use decSep */
    const char*   customThouSep; /* override thousand sep (UTF-8); NULL = locale */
    const char*   prefix;        /* custom prefix string; NULL = none */
    int           prefixLen;
    bool          prefixAuto;    /* fnpAuto: auto prefix (0x/0b/0) per notation */
    bool          expSignStrip;  /* fnsMoinsSiNégatif/Parenthèses: drop exponent sign (E only) */

    /* Parse status */
    bool          valid;
    const char*   errMsg;
} WLNumFormatSpec;

static void wl_format_spec_init(WLNumFormatSpec* s) {
    memset(s, 0, sizeof(*s));
    s->fillChar = ' ';
    s->decSep   = '.';
}

/* Banker's rounding (round half to even) — canon for type 'd'. */
static long long owl_banker_round(double v) {
    if (v >= 0) {
        double f = floor(v);
        double frac = v - f;
        if (frac < 0.5) return (long long)f;
        if (frac > 0.5) return (long long)(f + 1.0);
        long long fl = (long long)f;
        return (fl % 2 == 0) ? fl : fl + 1;
    } else {
        double f = ceil(v);
        double frac = f - v;
        if (frac < 0.5) return (long long)f;
        if (frac > 0.5) return (long long)(f - 1.0);
        long long fl = (long long)f;
        return (fl % 2 == 0) ? fl : fl - 1;
    }
}

/* Insert thousand separators in [bufStart..bufEnd) inside buf, returning
 * new total length. The sep is `sepOverride` if non-NULL else g_locale_thou_sep
 * (UTF-8, may be multi-byte). */
static int owl_insert_thousands_ex(char* buf, int len, int intStart, int intEnd,
                                   const char* sepOverride) {
    const char* sep = sepOverride ? sepOverride : g_locale_thou_sep;
    int sepLen = (int)strlen(sep);
    if (sepLen == 0) return len;
    int intLen = intEnd - intStart;
    if (intLen <= 3) return len;
    int firstGroup = intLen % 3;
    if (firstGroup == 0) firstGroup = 3;
    int nSeps = (intLen - firstGroup) / 3;
    if (nSeps <= 0) return len;
    /* Shift everything after intEnd to the right by nSeps*sepLen bytes,
     * then insert separators going left-to-right within the int zone. */
    int extra = nSeps * sepLen;
    memmove(buf + intEnd + extra, buf + intEnd, (size_t)(len - intEnd));
    /* Build new int zone in a temp, then copy back */
    char tmp[128];
    int t = 0;
    for (int i = 0; i < intLen; i++) {
        if (i > 0 && (i - firstGroup) % 3 == 0) {
            memcpy(tmp + t, sep, (size_t)sepLen);
            t += sepLen;
        }
        tmp[t++] = buf[intStart + i];
    }
    memcpy(buf + intStart, tmp, (size_t)t);
    return len + extra;
}

/* Format raw value to text (sign omitted, width not applied yet). */
static int owl_format_raw(double absv, const WLNumFormatSpec* spec,
                          char* raw, int rawSize) {
    int len = 0;
    switch (spec->type) {
        case WLNUM_TYPE_F: {
            int prec = spec->hasPrecision ? spec->precision : 6;
            len = snprintf(raw, (size_t)rawSize, "%.*f", prec, absv);
            break;
        }
        case WLNUM_TYPE_E: {
            int prec = spec->hasPrecision ? spec->precision : 6;
            len = snprintf(raw, (size_t)rawSize, "%.*e", prec, absv);
            if (len < 0) len = 0;
            if (len >= rawSize) len = rawSize - 1;
            raw[len] = 0;
            /* SigneExponentielle != fnsToujours -> drop the exponent sign
             * (the '+'/'-' right after the 'e'/'E'). Loses the exponent sign
             * info on purpose; the mantissa sign is handled separately. */
            if (spec->expSignStrip) {
                for (int i = 0; i < len; i++) {
                    if ((raw[i] == 'e' || raw[i] == 'E') && i + 1 < len &&
                        (raw[i + 1] == '+' || raw[i + 1] == '-')) {
                        memmove(raw + i + 1, raw + i + 2, (size_t)(len - i - 2));
                        len--;
                        raw[len] = 0;
                        break;
                    }
                }
            }
            break;
        }
        case WLNUM_TYPE_D: {
            long long iv = owl_banker_round(absv);
            len = snprintf(raw, (size_t)rawSize, "%lld", iv);
            break;
        }
        case WLNUM_TYPE_X: {
            long long iv = (long long)absv;
            len = snprintf(raw, (size_t)rawSize, "%llx", iv);
            break;
        }
        case WLNUM_TYPE_XU: {
            long long iv = (long long)absv;
            len = snprintf(raw, (size_t)rawSize, "%llX", iv);
            break;
        }
        case WLNUM_TYPE_O: {
            long long iv = (long long)absv;
            len = snprintf(raw, (size_t)rawSize, "%llo", iv);
            break;
        }
        case WLNUM_TYPE_B: {
            unsigned long long iv = (unsigned long long)absv;
            char tmp[80];
            int tlen = 0;
            if (iv == 0) tmp[tlen++] = '0';
            else while (iv > 0 && tlen < (int)sizeof(tmp)) {
                tmp[tlen++] = (char)('0' + (iv & 1));
                iv >>= 1;
            }
            for (int i = 0; i < tlen; i++) raw[i] = tmp[tlen - 1 - i];
            len = tlen;
            break;
        }
        default:
            return 0;
    }
    if (len < 0) len = 0;
    if (len >= rawSize) len = rawSize - 1;
    raw[len] = 0;

    /* Replace snprintf '.' with the spec's decSep when it's not '.' */
    if (spec->decSep != '.' &&
        (spec->type == WLNUM_TYPE_F || spec->type == WLNUM_TYPE_E)) {
        for (int i = 0; i < len; i++) {
            if (raw[i] == '.') raw[i] = spec->decSep;
        }
    }

    /* Insert thousand separators in the integer part */
    if (spec->thousandSep) {
        int intEnd = len;
        if (spec->type == WLNUM_TYPE_F || spec->type == WLNUM_TYPE_E) {
            for (int i = 0; i < len; i++) {
                if (raw[i] == spec->decSep) { intEnd = i; break; }
            }
        }
        int intStart = 0;
        if (raw[0] == '-' || raw[0] == '+') intStart = 1;
        len = owl_insert_thousands_ex(raw, len, intStart, intEnd, spec->customThouSep);
        raw[len] = 0;
    }
    return len;
}

/* Compute the auto-prefix string for fnpAuto according to the type. */
static const char* owl_auto_prefix(WLNumType type, bool uppercase) {
    switch (type) {
        case WLNUM_TYPE_X:  return "0x";
        case WLNUM_TYPE_XU: return uppercase ? "0X" : "0x";
        case WLNUM_TYPE_O:  return "0";
        case WLNUM_TYPE_B:  return "0b";
        default:            return "";
    }
}

/* Apply width / alignment / sign / parens / prefix. Writes to out (NUL-terminated). */
static int owl_format_apply_width(const char* raw, int rawLen,
                                  bool negative, const WLNumFormatSpec* spec,
                                  char* out, int outSize) {
    bool useParens = negative && spec->signMode == WLNUM_SIGN_PARENS;
    bool showSign  = !useParens && (negative || spec->signMode == WLNUM_SIGN_FORCE);
    char signChar  = negative ? '-' : '+';

    /* Resolve prefix : custom string > auto prefix > none */
    const char* prefix = NULL;
    int prefixLen = 0;
    if (spec->prefix && spec->prefixLen > 0) {
        prefix = spec->prefix;
        prefixLen = spec->prefixLen;
    } else if (spec->prefixAuto) {
        prefix = owl_auto_prefix(spec->type, spec->uppercase);
        prefixLen = (int)strlen(prefix);
    }

    int contentLen = rawLen + prefixLen + (showSign ? 1 : 0) + (useParens ? 2 : 0);

    /* Width is ignored if precision >= width (doc rule for f/e only — hex/oct/bin
     * ignore the precision field, so width must apply normally). */
    int width = spec->width;
    if (spec->hasPrecision && spec->precision >= width &&
        (spec->type == WLNUM_TYPE_F || spec->type == WLNUM_TYPE_E)) {
        width = 0;
    }

    int pad = (width > contentLen) ? (width - contentLen) : 0;
    char fill = spec->zeroPad && !spec->leftAlign ? '0' : spec->fillChar;

    int o = 0;
    if (spec->leftAlign) {
        if (showSign && o < outSize) out[o++] = signChar;
        if (useParens && o < outSize) out[o++] = '(';
        if (prefixLen > 0 && o + prefixLen <= outSize) {
            memcpy(out + o, prefix, (size_t)prefixLen); o += prefixLen;
        }
        int n = rawLen < outSize - o ? rawLen : outSize - o;
        memcpy(out + o, raw, (size_t)n); o += n;
        if (useParens && o < outSize) out[o++] = ')';
        for (int i = 0; i < pad && o < outSize; i++) out[o++] = ' ';
    } else if (fill == '0' && !useParens) {
        /* Zero-pad: sign first, then prefix, then zeros, then digits */
        if (showSign && o < outSize) out[o++] = signChar;
        if (prefixLen > 0 && o + prefixLen <= outSize) {
            memcpy(out + o, prefix, (size_t)prefixLen); o += prefixLen;
        }
        for (int i = 0; i < pad && o < outSize; i++) out[o++] = '0';
        int n = rawLen < outSize - o ? rawLen : outSize - o;
        memcpy(out + o, raw, (size_t)n); o += n;
    } else {
        /* Right-align with custom fill: pad first, then sign, prefix, digits */
        for (int i = 0; i < pad && o < outSize; i++) out[o++] = fill;
        if (showSign && o < outSize) out[o++] = signChar;
        if (useParens && o < outSize) out[o++] = '(';
        if (prefixLen > 0 && o + prefixLen <= outSize) {
            memcpy(out + o, prefix, (size_t)prefixLen); o += prefixLen;
        }
        int n = rawLen < outSize - o ? rawLen : outSize - o;
        memcpy(out + o, raw, (size_t)n); o += n;
        if (useParens && o < outSize) out[o++] = ')';
    }
    if (o < outSize) out[o] = 0;
    return o;
}

/* Parse a printf-like mask into a WLNumFormatSpec. */
static bool wl_format_parse_mask(const char* mask, int len, WLNumFormatSpec* spec) {
    wl_format_spec_init(spec);
    int i = 0;

    /* '-' (cadrage) */
    if (i < len && mask[i] == '-') { spec->leftAlign = true; i++; }

    /* '+' (signe) */
    if (i < len && mask[i] == '+') { spec->signMode = WLNUM_SIGN_FORCE; i++; }

    /* '0' (zéro flag) — must precede width digits */
    if (i < len && mask[i] == '0' &&
        i + 1 < len && mask[i+1] >= '0' && mask[i+1] <= '9') {
        spec->zeroPad = true; i++;
    }

    /* <Taille> */
    while (i < len && mask[i] >= '0' && mask[i] <= '9') {
        spec->width = spec->width * 10 + (mask[i] - '0');
        i++;
    }

    /* <.Précision> or <,Précision> */
    if (i < len && (mask[i] == '.' || mask[i] == ',')) {
        spec->decSep = mask[i];
        spec->hasPrecision = true;
        i++;
        while (i < len && mask[i] >= '0' && mask[i] <= '9') {
            spec->precision = spec->precision * 10 + (mask[i] - '0');
            i++;
        }
    }

    /* <Type> required */
    if (i >= len) {
        spec->errMsg = "type manquant"; return false;
    }
    switch (mask[i]) {
        case 'd': spec->type = WLNUM_TYPE_D;  break;
        case 'e': spec->type = WLNUM_TYPE_E;  break;
        case 'f': spec->type = WLNUM_TYPE_F;  break;
        case 'x': spec->type = WLNUM_TYPE_X;  break;
        case 'X': spec->type = WLNUM_TYPE_XU; break;
        case 'o': spec->type = WLNUM_TYPE_O;  break;
        default:
            spec->errMsg = "type invalide"; return false;
    }
    i++;

    /* <Millier> 'S' / 's' */
    if (i < len && (mask[i] == 'S' || mask[i] == 's')) {
        spec->thousandSep = true; i++;
    }

    if (i != len) { spec->errMsg = "caracteres en trop"; return false; }

    spec->valid = true;
    return true;
}

/* Format value using a parsed spec. Returns ObjString. */
static ObjString* wl_format_with_spec(double val, const WLNumFormatSpec* spec) {
    bool negative = val < 0;
    /* Special: -0.0 detection (doc says signbit() preserved on rounded-to-zero) */
    if (val == 0.0 && signbit(val)) negative = true;

    double absv = negative ? -val : val;
    char raw[256];
    int rawLen = owl_format_raw(absv, spec, raw, sizeof(raw));

    /* For type 'd' the raw contains an integer; rounding may have brought
     * a negative -> 0. Detection: if rawLen == 1 && raw[0] == '0' &&
     * negative, the doc says the '-' is NOT shown unless force-sign is on
     * (then '+'). Observed: -0.001 with "+5.2f" => "-0.00".
     * This means the negative flag is preserved even when rounded to zero
     * for type f/e. We keep `negative` as-is. */

    char out[512];
    int outLen = owl_format_apply_width(raw, rawLen, negative, spec, out, sizeof(out));
    return obj_copy_string(out, outLen);
}

/* Top-level dispatch: value -> string with optional mask/struct. */
static ObjString* owl_numeric_to_string(Value v, Value* maskArg, int hasMask) {
    owl_locale_init();

    /* String passthrough: canon spec says if value is a string, return as-is */
    if (IS_STRING(v)) return AS_STRING(v);

    /* Numeric value -> double (we'll handle VAL_NUMERIC precision-preserving
     * later in this same function for f/d types where it matters) */
    double val = 0;
    if      (IS_INTEGER(v))   val = (double)AS_INTEGER(v);
    else if (IS_REAL(v))      val = AS_REAL(v);
    /* CURRENCY est stocké int64 scaled par CURRENCY_SCALE (cf. value.h).
     * Sans cette division, les 3 branches masque (string, sentinel,
     * FormatNumérique) exposent l'entier scaled à l'utilisateur — bug
     * d'omission de  corrigé ici, aligne sur  « CURRENCY :
     * exact, 6 décimales scaled ». Caveat IEEE-754 : au-delà de 2^53
     * unités scaled (~9 quadrillions d'unités), la conversion double perd
     * des bits — acceptable, wl_format_with_spec opère en double quoi
     * qu'il arrive. */
    else if (IS_CURRENCY(v))  val = (double)AS_CURRENCY(v) / (double)CURRENCY_SCALE;
    else if (IS_NUMERIC(v))   val = numeric_to_double(v);
    else                      return obj_copy_string("", 0);

    /* No mask -> default formatting.
     *
     * Cf.  : chaque type sort dans sa forme la plus fidèle :
     *  - INTEGER  : décimal exact, pas de point.
     *  - NUMERIC  : numeric_to_str (rendu exact depuis __int128, sans
     *               passer par double — sinon perte de précision dès 16
     *               chiffres significatifs).
     *  - CURRENCY : currency_to_str (exact, 6 décimales scaled).
     *  - REAL     : owl_real_to_str (shortest round-trippable). */
    if (!hasMask || !maskArg) {
        /* NUMERIC : forme décimale plate exacte, peut dépasser un buf fixe pour
         * un bignum -> allocation mpdecimal. */
        if (IS_NUMERIC(v)) {
            char* s = numeric_format_alloc(v, "f");
            if (s == NULL) return obj_copy_string("0", 1);
            ObjString* r = obj_copy_string(s, (int)strlen(s));
            owl_numeric_free_str(s);
            return r;
        }
        char buf[64];
        int n;
        if      (IS_INTEGER(v))   n = snprintf(buf, sizeof(buf), "%lld", (long long)AS_INTEGER(v));
        else if (IS_CURRENCY(v))  n = currency_to_str(AS_CURRENCY(v), buf, sizeof(buf));
        else                      n = owl_real_to_str(val, buf, sizeof(buf));
        return obj_copy_string(buf, n);
    }

    /* Mask is a string: parse printf-like */
    if (IS_STRING(*maskArg)) {
        ObjString* mask = AS_STRING(*maskArg);
        WLNumFormatSpec spec;
        if (!wl_format_parse_mask(mask->chars, mask->length, &spec)) {
            /* Masque non reconnu : soft-error nommant le masque fautif au
             * lieu d'une chaîne vide muette (footgun). Le masque printf-like
             * s'écrit SANS le « % » (ex. ".2f") — l'indice le rappelle. */
            owl_emit_native_error(E_NATIVE_NUM_MASK_INVALID,
                                  "Num\xc3\xa9" "riqueVersCha\xc3\xae" "ne",
                                  "masque", mask->chars, NULL);
            return obj_copy_string("", 0);
        }
        /* NUMERIC + masque fixe/entier/expo "simple" : on formate exactement
         * depuis le bignum (mpdecimal) au lieu de passer par un double lossy.
         * Couvre la précision arbitraire (ex ".50f" = 50 décimales exactes) et
         * l'entier "d" sur un grand nombre. Les cas exotiques (séparateur de
         * milliers, séparateur décimal non '.', préfixe custom, signe entre
         * parenthèses, hex/oct/bin) retombent sur le formateur double. */
        bool simpleFixed = (spec.type == WLNUM_TYPE_D) ||
                           ((spec.type == WLNUM_TYPE_F || spec.type == WLNUM_TYPE_E)
                            && spec.hasPrecision);
        if (IS_NUMERIC(v) && simpleFixed && spec.decSep == '.' && !spec.thousandSep &&
            spec.customDecSep == NULL && spec.customThouSep == NULL &&
            spec.prefix == NULL && spec.signMode != WLNUM_SIGN_PARENS &&
            !spec.expSignStrip) {
            char mfmt[48];
            int p = 0;
            if (spec.leftAlign)              mfmt[p++] = '<';
            if (spec.signMode == WLNUM_SIGN_FORCE) mfmt[p++] = '+';
            if (spec.zeroPad && !spec.leftAlign)   mfmt[p++] = '0';
            if (spec.width > 0) p += snprintf(mfmt + p, sizeof(mfmt) - p, "%d", spec.width);
            int prec = spec.hasPrecision ? spec.precision
                                         : (spec.type == WLNUM_TYPE_D ? 0 : -1);
            if (prec >= 0) p += snprintf(mfmt + p, sizeof(mfmt) - p, ".%d", prec);
            mfmt[p++] = (spec.type == WLNUM_TYPE_E) ? 'e' : 'f';
            mfmt[p] = '\0';
            char* s = numeric_format_alloc(v, mfmt);
            if (s != NULL) {
                ObjString* r = obj_copy_string(s, (int)strlen(s));
                owl_numeric_free_str(s);
                return r;
            }
        }
        return wl_format_with_spec(val, &spec);
    }

    /* Mask is an integer sentinel (maskNombreSystème / maskMonétaireSystème)
     * -> apply locale-default formatting. canon exposes these as opaque values;
     * we encode them in the OWL_MASK_* sentinel zone (cf sentinels.h). */
    if (IS_INTEGER(*maskArg)) {
        long long sentinel = AS_INTEGER(*maskArg);
        WLNumFormatSpec spec;
        wl_format_spec_init(&spec);
        spec.type         = WLNUM_TYPE_F;
        spec.hasPrecision = true;
        spec.precision    = 2;
        spec.thousandSep  = true;
        ObjString* core = wl_format_with_spec(val, &spec);
        if (sentinel == OWL_MASK_MONETAIRE_SYSTEME) {
            struct lconv* lc = localeconv();
            const char* cur = (lc && lc->currency_symbol) ? lc->currency_symbol : "";
            if (cur && cur[0]) {
                char buf[256];
                int n = snprintf(buf, sizeof(buf), "%s %.*s",
                                 cur, core->length, core->chars);
                return obj_copy_string(buf, n);
            }
        }
        return core;
    }

    /* Mask is a FormatNumérique struct */
    if (IS_INSTANCE(*maskArg)) {
        ObjInstance* inst = AS_INSTANCE(*maskArg);
        if (inst->structName && strcmp(inst->structName->chars, "FormatNumerique") == 0) {
            WLNumFormatSpec spec;
            wl_format_spec_init(&spec);
            Value f;

            /* Notation -> WLNumType */
            ObjString* k_notation = obj_copy_string("Notation", 8);
            if (table_get(&inst->fields, k_notation, &f) && IS_INTEGER(f)) {
                long long n = AS_INTEGER(f);
                if      (n == OWL_FNN_BINAIRE)     spec.type = WLNUM_TYPE_B;
                else if (n == OWL_FNN_DECIMAL)     spec.type = WLNUM_TYPE_F;  /* déc = f */
                else if (n == OWL_FNN_EXPONENTIEL) spec.type = WLNUM_TYPE_E;
                else if (n == OWL_FNN_HEXADECIMAL) spec.type = WLNUM_TYPE_X;
                else if (n == OWL_FNN_OCTAL)       spec.type = WLNUM_TYPE_O;
                else                                spec.type = WLNUM_TYPE_F;
            } else {
                spec.type = WLNUM_TYPE_F;
            }

            /* Majuscules : pour hex, switch X minuscule -> XU majuscule */
            ObjString* k_maj = obj_copy_string("Majuscules", 10);
            if (table_get(&inst->fields, k_maj, &f) && IS_BOOL(f) && AS_BOOL(f)) {
                spec.uppercase = true;
                if (spec.type == WLNUM_TYPE_X) spec.type = WLNUM_TYPE_XU;
            }

            /* Alignement -> leftAlign */
            ObjString* k_align = obj_copy_string("Alignement", 10);
            if (table_get(&inst->fields, k_align, &f) && IS_INTEGER(f)) {
                spec.leftAlign = (AS_INTEGER(f) == OWL_FNC_GAUCHE);
            }

            /* Signe -> signMode */
            ObjString* k_signe = obj_copy_string("Signe", 5);
            if (table_get(&inst->fields, k_signe, &f) && IS_INTEGER(f)) {
                long long s = AS_INTEGER(f);
                if      (s == OWL_FNS_TOUJOURS)              spec.signMode = WLNUM_SIGN_FORCE;
                else if (s == OWL_FNS_PARENTHESES_SI_NEGATIF) spec.signMode = WLNUM_SIGN_PARENS;
                else                                          spec.signMode = WLNUM_SIGN_NEG_ONLY;
            }

            /* SigneExponentielle -> expSignStrip (exponential notation only) */
            ObjString* k_signexp = obj_copy_string("SigneExponentielle", 18);
            if (table_get(&inst->fields, k_signexp, &f) && IS_INTEGER(f)) {
                long long se = AS_INTEGER(f);
                spec.expSignStrip = (se == OWL_FNS_MOINS_SI_NEGATIF ||
                                     se == OWL_FNS_PARENTHESES_SI_NEGATIF);
            }

            /* ChiffresApresVirgule -> precision */
            ObjString* k_dec = obj_copy_string("ChiffresApresVirgule", 20);
            if (table_get(&inst->fields, k_dec, &f) && IS_INTEGER(f)) {
                spec.hasPrecision = true;
                spec.precision    = (int)AS_INTEGER(f);
            }

            /* LongueurMinimale -> width */
            ObjString* k_width = obj_copy_string("LongueurMinimale", 16);
            if (table_get(&inst->fields, k_width, &f) && IS_INTEGER(f)) {
                spec.width = (int)AS_INTEGER(f);
            }

            /* CaractereRemplissage -> fillChar */
            ObjString* k_fill = obj_copy_string("CaractereRemplissage", 20);
            if (table_get(&inst->fields, k_fill, &f) && IS_STRING(f) && AS_STRING(f)->length > 0) {
                spec.fillChar = AS_STRING(f)->chars[0];
            }

            /* SeparateurDecimal -> custom (single byte) */
            ObjString* k_decsep = obj_copy_string("SeparateurDecimal", 17);
            if (table_get(&inst->fields, k_decsep, &f) && IS_STRING(f) && AS_STRING(f)->length > 0) {
                spec.decSep = AS_STRING(f)->chars[0];
            }

            /* SeparateurMilliers -> thousand sep override (UTF-8 multi-byte ok) */
            ObjString* k_thousep = obj_copy_string("SeparateurMilliers", 18);
            if (table_get(&inst->fields, k_thousep, &f) && IS_STRING(f) && AS_STRING(f)->length > 0) {
                spec.thousandSep   = true;
                spec.customThouSep = AS_STRING(f)->chars;
            }

            /* Prefixe : canon = chaîne libre. Extension OWL pragmatique :
             * la constante fnpAuto (sentinel entier OWL_FNP_AUTO) déclenche le
             * préfixe automatique selon la notation (0x/0b/0). Cohérent avec le
             * reste du registry où les constantes sont des entiers.
             *   - entier == OWL_FNP_AUTO        -> préfixe auto
             *   - chaîne "@AUTO@" (marker hérité) -> préfixe auto
             *   - chaîne libre non vide          -> préfixe brut */
            ObjString* k_pre = obj_copy_string("Prefixe", 7);
            if (table_get(&inst->fields, k_pre, &f)) {
                if (IS_INTEGER(f) && AS_INTEGER(f) == OWL_FNP_AUTO) {
                    spec.prefixAuto = true;
                } else if (IS_STRING(f) && AS_STRING(f)->length > 0) {
                    ObjString* p = AS_STRING(f);
                    if (p->length == 6 && memcmp(p->chars, "@AUTO@", 6) == 0) {
                        spec.prefixAuto = true;
                    } else {
                        spec.prefix    = p->chars;
                        spec.prefixLen = p->length;
                    }
                }
            }

            return wl_format_with_spec(val, &spec);
        }
    }

    return obj_copy_string("", 0);
}

/* ChaîneFormate(<chaîne>, <options>) — canon spec.
 * Applies one or a combination of cc* transformations to a string:
 *   ccMajuscule / ccMinuscule : case conversion
 *   ccSansAccent              : strip accents
 *   ccSansEspace              : trim outer whitespace
 *   ccSansEspaceIntérieur     : strip internal whitespace
 *   ccSansPonctuationNiEspace : strip punctuation and all whitespace
 *   ccNormal (0)              : no-op passthrough
 * Flags are combinable (e.g. ccMajuscule + ccSansAccent). */
static Value fn_chaine_formate_options(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return args[0];
    ObjString* src = AS_STRING(args[0]);
    int flags = 0;
    if (argCount >= 2) {
        if (IS_INTEGER(args[1])) flags = (int)AS_INTEGER(args[1]);
        else if (IS_BOOL(args[1])) flags = AS_BOOL(args[1]) ? 0 : 0;
    }
    if (flags == 0) return args[0];

    /* Work in a heap buffer up to 2x source length (accent stripping may expand). */
    int bufCap = src->length * 2 + 16;
    char* buf = ALLOCATE(char, bufCap + 1);
    int bufLen = src->length;
    memcpy(buf, src->chars, src->length);
    buf[bufLen] = '\0';

    /* 1. Strip outer whitespace */
    if (flags & CC_SANS_ESPACE) {
        int start = 0, end = bufLen;
        while (start < end && (buf[start] == ' ' || buf[start] == '\t' ||
                               buf[start] == '\n' || buf[start] == '\r' ||
                               buf[start] == 11   || buf[start] == 12)) start++;
        while (end > start && (buf[end-1] == ' ' || buf[end-1] == '\t' ||
                               buf[end-1] == '\n' || buf[end-1] == '\r' ||
                               buf[end-1] == 11   || buf[end-1] == 12)) end--;
        if (start > 0) memmove(buf, buf + start, end - start);
        bufLen = end - start;
        buf[bufLen] = '\0';
    }
    /* 2. Strip internal whitespace */
    if (flags & CC_SANS_ESPACE_INT) {
        int w = 0;
        for (int i = 0; i < bufLen; i++) {
            char c = buf[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 11 || c == 12) continue;
            buf[w++] = c;
        }
        bufLen = w;
        buf[bufLen] = '\0';
    }
    /* 3. Strip punctuation + all whitespace */
    if (flags & CC_SANS_PONCTUATION) {
        int w = 0;
        for (int i = 0; i < bufLen; i++) {
            unsigned char c = (unsigned char)buf[i];
            /* Keep letters (incl UTF-8 high bytes) and digits */
            if ((c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                c >= 0x80) {  /* UTF-8 continuation / accented bytes */
                buf[w++] = (char)c;
            }
        }
        bufLen = w;
        buf[bufLen] = '\0';
    }
    /* 4. Strip accents */
    if (flags & CC_SANS_ACCENT) {
        char tmp[8192];
        int newLen = scanner_strip_accents(buf, bufLen, tmp, (int)sizeof(tmp));
        if (newLen <= bufCap) {
            memcpy(buf, tmp, newLen);
            bufLen = newLen;
            buf[bufLen] = '\0';
        }
    }
    /* 5. Case conversion (exclusive: Majuscule wins if both set).
     *    Uses the UTF-8 aware helper (handles ASCII + Latin-1 Supplement). */
    if (flags & CC_MAJUSCULE) {
        char* tmp = malloc(bufLen);
        owl_case_convert(buf, bufLen, tmp, true);
        memcpy(buf, tmp, bufLen);
        free(tmp);
    } else if (flags & CC_MINUSCULE) {
        char* tmp = malloc(bufLen);
        owl_case_convert(buf, bufLen, tmp, false);
        memcpy(buf, tmp, bufLen);
        free(tmp);
    }

    return OBJ_VAL(obj_take_string(buf, bufLen));
}

/* NumériqueVersChaîne(val [, masque|FormatNumérique]) — doc 3024024.
 * String passthrough when val is already a string (canon spec).
 * Mask can be: string (printf-like), integer sentinel (maskNombreSystème
 * / maskMonétaireSystème), or FormatNumérique struct. */
static Value fn_chaine_formate(int argCount, Value* args) {
    Value v = args[0];
    if (IS_STRING(v)) return v;  /* canon passthrough */
    Value* maskPtr = (argCount >= 2) ? &args[1] : NULL;
    return OBJ_VAL(owl_numeric_to_string(v, maskPtr, maskPtr ? 1 : 0));
}

/* ExtraitChaîne(source, rang, separateur) — canon-style token picker.
 *   ExtraitChaîne("a;b;c;d", 2, ";") -> "b"
 *   ExtraitChaîne(s, rangPremier, ";") -> first token, init traversal
 *   ExtraitChaîne(s, rangSuivant, ";") -> next token in traversal
 *   Rang hors limites -> EOT (Caract(4)) — canon.
 *
 * Le séparateur peut aussi être un tableau de chaînes (doc 3024017
 * "Il est possible d'utiliser un tableau de séparateurs"). Dans ce cas,
 * ExtraitChaîne extrait les sous-chaînes en utilisant n'importe lequel
 * des séparateurs (earliest match au scan). */
#define MAX_XC_SEPS 16
static ObjString*  g_xc_src = NULL;     /* source pour traversal rangSuivant/rangPrécédent */
static const char* g_xc_pos = NULL;     /* curseur de parcours : forward = début du prochain
                                         * segment (avance) ; reverse = borne droite exclusive
                                         * du segment à renvoyer (recule). NULL = parcours
                                         * inverse épuisé (avant le premier morceau). */
static bool        g_xc_reverse = false;/* direction du parcours en cours : évite qu'un
                                         * rangSuivant reprenne un parcours inverse (et vice versa). */
static char        g_xc_seps_buf[MAX_XC_SEPS][128]; /* copie locale des séparateurs */
static int         g_xc_seps_len[MAX_XC_SEPS];
static int         g_xc_sep_count = 0;

/* Cherche dans [from, end) la 1ere occurrence de N'IMPORTE QUEL séparateur.
 * Renvoie le pointeur sur le match (ou NULL) et écrit dans *outLen la taille
 * du séparateur matche. */
static const char* find_earliest_sep(const char* from, const char* end,
                                      const char (*seps)[128], const int* sepsLen,
                                      int sepCount, int* outLen) {
    const char* best = NULL;
    int bestLen = 0;
    for (int s = 0; s < sepCount; s++) {
        int sl = sepsLen[s];
        if (sl <= 0 || sl > (int)(end - from)) continue;
        for (const char* p = from; p + sl <= end; p++) {
            if (memcmp(p, seps[s], (size_t)sl) == 0) {
                if (best == NULL || p < best) {
                    best = p;
                    bestLen = sl;
                }
                break;  /* earliest match for this separator */
            }
        }
    }
    if (outLen) *outLen = bestLen;
    return best;
}

/* Cherche dans [from, end) la DERNIÈRE occurrence de n'importe quel séparateur.
 * Curseur inverse stateful : O(n) par appel, pas de tableau borné. */
static const char* find_last_sep(const char* from, const char* end,
                                 const char (*seps)[128], const int* sepsLen,
                                 int sepCount, int* outLen) {
    const char* last = NULL;
    int lastLen = 0;
    const char* p = from;
    while (p < end) {
        int mlen = 0;
        const char* m = find_earliest_sep(p, end, seps, sepsLen, sepCount, &mlen);
        if (m == NULL) break;
        last = m;
        lastLen = mlen;
        p = m + mlen;
    }
    if (outLen) *outLen = lastLen;
    return last;
}

/* Charge le 3e arg (string ou array de strings) dans le buffer global
 * g_xc_seps_buf. Retourne false si invalide. */
static bool load_xc_separators(Value sepArg) {
    g_xc_sep_count = 0;
    if (IS_STRING(sepArg)) {
        ObjString* s = AS_STRING(sepArg);
        if (s->length <= 0 || s->length >= 128) return false;
        memcpy(g_xc_seps_buf[0], s->chars, (size_t)s->length);
        g_xc_seps_buf[0][s->length] = '\0';
        g_xc_seps_len[0] = s->length;
        g_xc_sep_count = 1;
        return true;
    }
    if (IS_ARRAY(sepArg)) {
        ObjArray* arr = AS_ARRAY(sepArg);
        int n = arr->elements.count;
        if (n <= 0) return false;
        if (n > MAX_XC_SEPS) n = MAX_XC_SEPS;
        for (int i = 0; i < n; i++) {
            Value v = arr->elements.values[i];
            if (!IS_STRING(v)) return false;
            ObjString* s = AS_STRING(v);
            if (s->length <= 0 || s->length >= 128) continue;
            memcpy(g_xc_seps_buf[g_xc_sep_count], s->chars, (size_t)s->length);
            g_xc_seps_buf[g_xc_sep_count][s->length] = '\0';
            g_xc_seps_len[g_xc_sep_count] = s->length;
            g_xc_sep_count++;
        }
        return g_xc_sep_count > 0;
    }
    return false;
}

static Value fn_extrait_chaine(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
    ObjString* src = AS_STRING(args[0]);
    int rank = 1;
    if (IS_INTEGER(args[1])) rank = (int)AS_INTEGER(args[1]);
    else if (IS_REAL(args[1])) rank = (int)AS_REAL(args[1]);

    if (!load_xc_separators(args[2])) {
        return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
    }
    const char* end = src->chars + src->length;

    /* 4e arg optionnel : sens de parcours (DepuisDébut=0 neutre / DepuisFin=4,
     * bit combinable). Seul le bit DepuisFin nous intéresse ici. */
    int opts = 0;
    if (argCount >= 4) {
        if (IS_INTEGER(args[3])) opts = (int)AS_INTEGER(args[3]);
        else if (IS_REAL(args[3])) opts = (int)AS_REAL(args[3]);
    }
    bool depuisFin = (opts & SEARCH_DEPUIS_FIN) != 0;

    /* Traversal mode : rangPremier initialise, rangSuivant continue */
    if (rank == RANG_PREMIER) {
        g_xc_src = src;
        g_xc_reverse = false;
        int matchLen = 0;
        const char* m = find_earliest_sep(src->chars, end,
                                           g_xc_seps_buf, g_xc_seps_len,
                                           g_xc_sep_count, &matchLen);
        if (m == NULL) {
            g_xc_pos = end;
            return OBJ_VAL(obj_copy_string(src->chars, src->length));
        }
        g_xc_pos = m + matchLen;
        return OBJ_VAL(obj_copy_string(src->chars, (int)(m - src->chars)));
    }
    if (rank == RANG_SUIVANT) {
        if (g_xc_src != src || g_xc_pos == NULL || g_xc_reverse) {
            return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        }
        if (g_xc_pos > end) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        int matchLen = 0;
        const char* m = find_earliest_sep(g_xc_pos, end,
                                           g_xc_seps_buf, g_xc_seps_len,
                                           g_xc_sep_count, &matchLen);
        if (m == NULL) {
            /* Dernier segment, après quoi on signale fin via pos > end */
            int len = (int)(end - g_xc_pos);
            const char* p = g_xc_pos;
            g_xc_pos = end + 1;
            return OBJ_VAL(obj_copy_string(p, len));
        }
        const char* p = g_xc_pos;
        int len = (int)(m - p);
        g_xc_pos = m + matchLen;
        return OBJ_VAL(obj_copy_string(p, len));
    }

    /* Parcours à rebours : rangDernier initialise (dernier morceau), rangPrécédent
     * remonte vers le début. Curseur stateful sans tableau borné : g_xc_pos tient la
     * borne droite exclusive du prochain segment à renvoyer, on scanne en arrière via
     * find_last_sep. Le contenu de chaque morceau reste en ordre normal de lecture. */
    if (rank == RANG_DERNIER) {
        g_xc_src = src;
        g_xc_reverse = true;
        int matchLen = 0;
        const char* m = find_last_sep(src->chars, end,
                                      g_xc_seps_buf, g_xc_seps_len,
                                      g_xc_sep_count, &matchLen);
        if (m == NULL) {
            /* Aucun séparateur : la chaîne entière est le seul (et dernier) morceau. */
            g_xc_pos = NULL;
            return OBJ_VAL(obj_copy_string(src->chars, src->length));
        }
        const char* segStart = m + matchLen;
        g_xc_pos = m;  /* borne pour le rangPrécédent suivant = début de ce séparateur */
        return OBJ_VAL(obj_copy_string(segStart, (int)(end - segStart)));
    }
    if (rank == RANG_PRECEDENT) {
        if (g_xc_src != src || g_xc_pos == NULL || !g_xc_reverse) {
            return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        }
        const char* boundary = g_xc_pos;  /* borne droite exclusive du morceau courant */
        int matchLen = 0;
        const char* m = find_last_sep(src->chars, boundary,
                                      g_xc_seps_buf, g_xc_seps_len,
                                      g_xc_sep_count, &matchLen);
        if (m == NULL) {
            /* Plus de séparateur à gauche : ce morceau est le premier, parcours terminé. */
            g_xc_pos = NULL;
            return OBJ_VAL(obj_copy_string(src->chars, (int)(boundary - src->chars)));
        }
        const char* segStart = m + matchLen;
        g_xc_pos = m;
        return OBJ_VAL(obj_copy_string(segStart, (int)(boundary - segStart)));
    }

    if (rank < 1) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));

    /* DepuisFin : on compte les morceaux à rebours (rang 1 = dernier morceau).
     * Le contenu du morceau reste en ordre normal de lecture. Approche two-pass
     * sans plafond : passe 1 compte le nombre total de morceaux, passe 2 re-scanne
     * jusqu'à l'index cible (total - rang). O(2n), pas de tableau borné. */
    if (depuisFin) {
        int nseg = 1;  /* le dernier morceau (après le dernier séparateur) compte toujours */
        const char* p = src->chars;
        while (p < end) {
            int matchLen = 0;
            const char* m = find_earliest_sep(p, end,
                                               g_xc_seps_buf, g_xc_seps_len,
                                               g_xc_sep_count, &matchLen);
            if (m == NULL) break;
            nseg++;
            p = m + matchLen;
        }
        int idx = nseg - rank;
        if (idx < 0) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        int cur = 0;
        const char* segStart = src->chars;
        p = src->chars;
        while (p < end) {
            int matchLen = 0;
            const char* m = find_earliest_sep(p, end,
                                               g_xc_seps_buf, g_xc_seps_len,
                                               g_xc_sep_count, &matchLen);
            if (m == NULL) break;
            if (cur == idx) return OBJ_VAL(obj_copy_string(segStart, (int)(m - segStart)));
            cur++;
            p = m + matchLen;
            segStart = p;
        }
        /* Dernier morceau (index nseg-1) */
        if (cur == idx) return OBJ_VAL(obj_copy_string(segStart, (int)(end - segStart)));
        return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
    }

    /* Rang fixe : on scanne en avançant à chaque match du earliest separator. */
    int count = 1;
    const char* segStart = src->chars;
    const char* p = src->chars;
    while (p < end) {
        int matchLen = 0;
        const char* m = find_earliest_sep(p, end,
                                           g_xc_seps_buf, g_xc_seps_len,
                                           g_xc_sep_count, &matchLen);
        if (m == NULL) break;
        if (count == rank) {
            return OBJ_VAL(obj_copy_string(segStart, (int)(m - segStart)));
        }
        count++;
        p = m + matchLen;
        segStart = p;
    }
    if (count == rank) {
        return OBJ_VAL(obj_copy_string(segStart, (int)(end - segStart)));
    }
    return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
}

/* ================================================================
 * NEW STRING FUNCTIONS
 * ================================================================ */

/* ChaîneCompare(s1, s2 [, options]) → -1 / 0 / 1
 * Doc 3024011: ccNormal(0), ccSansCasse(1), ccSansAccent(2), ccSansEspace(4),
 * ccSansEspaceIntérieur(8), ccSansPonctuationNiEspace(16),
 * ccOrdreLexicographique(32), ccRespecteNumérique(64).
 * ccSansEspace = outer whitespace only (trim). ccSansEspaceIntérieur = inner spaces. */
static Value fn_chaine_compare(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(0);  /*  garde sous-application */
    ObjString* s1 = to_string(args[0]);
    ObjString* s2 = to_string(args[1]);
    if (!s1 || !s2) return INTEGER_VAL(0);

    int flags = 0;
    if (argCount >= 3) {
        if (IS_INTEGER(args[2]))   flags = (int)AS_INTEGER(args[2]);
        else if (IS_BOOL(args[2])) flags = AS_BOOL(args[2]) ? 0 : CC_SANS_CASSE;
    }
    /* Helper factorise dans string_compare_opts.c — meme code path utilise
     * par le comparateur de tri tableau (cf. stdlib_array.c). Les valeurs
     * CC_* sont alignees byte-a-byte sur OWLCMP_*. */
    return INTEGER_VAL(owl_string_compare_with_opts(s1->chars, s2->chars, flags));
}

/* Helper: split `s` using any-of separators (strings in `seps`).
 * Retourne un nouveau ObjArray contenant les segments.
 * Le caller est responsable de GC-protect le résultat. */
static ObjArray* split_string_anyof(const char* s, int slen,
                                    const char** seps, const int* sepLens,
                                    int sepCount) {
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));

    const char* p = s;
    const char* end = s + slen;
    const char* segStart = p;
    while (p < end) {
        int matched = -1, matchedLen = 0;
        for (int k = 0; k < sepCount; k++) {
            int spl = sepLens[k];
            if (spl > 0 && p + spl <= end &&
                memcmp(p, seps[k], (size_t)spl) == 0) {
                matched = k; matchedLen = spl; break;
            }
        }
        if (matched >= 0) {
            ObjString* part = obj_copy_string(segStart, (int)(p - segStart));
            obj_array_push(arr, OBJ_VAL(part));
            p += matchedLen;
            segStart = p;
        } else {
            p++;
        }
    }
    ObjString* last = obj_copy_string(segStart, (int)(end - segStart));
    obj_array_push(arr, OBJ_VAL(last));
    return arr;
}

/* Helper: extract separators from a Value (string or array of strings).
 * Fills seps[] and sepLens[] up to sepCount slots. Returns actual count.
 * Returns -1 on bad argument type. */
static int extract_separators(Value v, const char** seps, int* sepLens,
                              int maxCount) {
    int count = 0;
    if (IS_STRING(v)) {
        ObjString* sp = AS_STRING(v);
        if (count < maxCount) {
            seps[count] = sp->chars;
            sepLens[count] = sp->length;
            count++;
        }
    } else if (IS_ARRAY(v)) {
        ObjArray* sepArr = AS_ARRAY(v);
        for (int k = 0; k < sepArr->elements.count && count < maxCount; k++) {
            if (!IS_STRING(sepArr->elements.values[k])) continue;
            ObjString* sp = AS_STRING(sepArr->elements.values[k]);
            seps[count] = sp->chars;
            sepLens[count] = sp->length;
            count++;
        }
    } else {
        return -1;
    }
    return count;
}

/* ChaîneVersTableau(s [, sep [, sepColonne]]) / StringToArray — canon.
 * Forme 1D: split string into 1D array of strings (défaut sep = CRLF).
 * Forme 2D: split par sepLigne puis chaque ligne par sepColonne → nested array.
 * Les séparateurs peuvent être une chaîne ou un tableau de chaînes (any-of).
 * OWL divergence: returns new array (canon fills existing array by ref).
 *
 * ChaîneDécoupe (alias) utilise la même impl mais avec sémantique cascade
 * canon pour 2+ seps strings (split morceau par morceau, pas 2D). On distingue
 * via un flag `is2D` passé à la vraie impl. */
static Value fn_chaine_vers_tableau_impl(int argCount, Value* args, bool is2D);

static Value fn_chaine_vers_tableau(int argCount, Value* args) {
    return fn_chaine_vers_tableau_impl(argCount, args, true);
}
static Value fn_chaine_decoupe(int argCount, Value* args) {
    return fn_chaine_vers_tableau_impl(argCount, args, false);
}

static Value fn_chaine_vers_tableau_impl(int argCount, Value* args, bool is2D) {
    /* Forme 2D ChaîneVersTableau seulement: 3 args où args[1] et args[2]
     * sont string ou tableau */
    if (is2D && argCount >= 3 &&
        (IS_STRING(args[1]) || IS_ARRAY(args[1])) &&
        (IS_STRING(args[2]) || IS_ARRAY(args[2]))) {
        ObjString* src2d = to_string(args[0]);
        if (!src2d) {
            ObjArray* empty = obj_new_array();
            return OBJ_VAL(empty);
        }
        /* Extract line separators */
        const char* lineSeps[32]; int lineSepLens[32];
        int lineSepCount = extract_separators(args[1], lineSeps, lineSepLens, 32);
        if (lineSepCount <= 0) { lineSeps[0] = "\r\n"; lineSepLens[0] = 2; lineSepCount = 1; }
        /* Extract column separators */
        const char* colSeps[32]; int colSepLens[32];
        int colSepCount = extract_separators(args[2], colSeps, colSepLens, 32);
        if (colSepCount <= 0) { colSeps[0] = "\t"; colSepLens[0] = 1; colSepCount = 1; }

        /* Outer array */
        ObjArray* outer = obj_new_array();
        vm_push(OBJ_VAL(outer));

        /* Split src by line seps */
        ObjArray* lines = split_string_anyof(src2d->chars, src2d->length,
                                             lineSeps, lineSepLens, lineSepCount);
        /* lines est sur la stack (pushed by split_string_anyof) */

        /* Pour chaque ligne, re-split par col seps */
        for (int i = 0; i < lines->elements.count; i++) {
            Value lineV = lines->elements.values[i];
            if (!IS_STRING(lineV)) continue;
            ObjString* lineStr = AS_STRING(lineV);
            ObjArray* cols = split_string_anyof(lineStr->chars, lineStr->length,
                                                 colSeps, colSepLens, colSepCount);
            /* cols on stack */
            obj_array_push(outer, OBJ_VAL(cols));
            vm_pop(); /* cols */
        }
        vm_pop(); /* lines */
        vm_pop(); /* outer (was rooted while building) */
        return OBJ_VAL(outer);
    }
    /* Continuer avec la logique 1D existante */
    ObjString* src = to_string(args[0]);
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr)); /* GC root */
    if (!src) { vm_pop(); return OBJ_VAL(arr); }

    /* Default separator is CRLF (\r\n) per doc */
    static const char RC_SEP[] = "\r\n";
    const char* sepStr = RC_SEP;
    int sepLen = 2;
    bool splitByCodepoint = false;

    /* Collect all string separators from args[1..N]. Forme canon 2:
     * ChaîneDécoupe(s, sep1, sep2, ..., sepN [, options]) applies each
     * separator to the corresponding segment of the remaining string.
     * Forme canon 3: le 2e arg est un tableau [sep1, sep2, ...] → split sur n'importe
     * lequel des séparateurs (any-of). */
    const char* sepChain[32];
    int sepChainLens[32];
    int sepChainCount = 0;
    bool sepsFromArray = false; /* true if seps came from an array arg */
    for (int i = 1; i < argCount && sepChainCount < 32; i++) {
        if (IS_STRING(args[i])) {
            ObjString* sp = AS_STRING(args[i]);
            sepChain[sepChainCount] = sp->chars;
            sepChainLens[sepChainCount] = sp->length;
            sepChainCount++;
        } else if (IS_ARRAY(args[i]) && i == 1 && sepChainCount == 0) {
            /* Forme 3: tableau de séparateurs → split any-of */
            ObjArray* sepArr = AS_ARRAY(args[i]);
            for (int k = 0; k < sepArr->elements.count && sepChainCount < 32; k++) {
                if (!IS_STRING(sepArr->elements.values[k])) continue;
                ObjString* sp = AS_STRING(sepArr->elements.values[k]);
                sepChain[sepChainCount] = sp->chars;
                sepChainLens[sepChainCount] = sp->length;
                sepChainCount++;
            }
            sepsFromArray = true;
        } else {
            break; /* integer options arg stops the chain */
        }
    }

    /* Forme 3: any-of split avec tableau de séparateurs */
    if (sepsFromArray && sepChainCount > 0) {
        const char* p = src->chars;
        const char* end = src->chars + src->length;
        const char* segStart = p;
        while (p < end) {
            /* Try to match any separator at position p */
            int matched = -1;
            int matchedLen = 0;
            for (int k = 0; k < sepChainCount; k++) {
                int spl = sepChainLens[k];
                if (spl > 0 && p + spl <= end &&
                    memcmp(p, sepChain[k], (size_t)spl) == 0) {
                    matched = k;
                    matchedLen = spl;
                    break;
                }
            }
            if (matched >= 0) {
                ObjString* part = obj_copy_string(segStart, (int)(p - segStart));
                obj_array_push(arr, OBJ_VAL(part));
                p += matchedLen;
                segStart = p;
            } else {
                p++;
            }
        }
        /* Last segment */
        ObjString* last = obj_copy_string(segStart, (int)(end - segStart));
        obj_array_push(arr, OBJ_VAL(last));
        vm_pop();
        return OBJ_VAL(arr);
    }

    /* Forme 2 : plusieurs séparateurs successifs (sépare au 1er occurrence
     * de sep1, puis au 1er de sep2 dans ce qui reste, etc.). */
    if (sepChainCount >= 2) {
        const char* rem = src->chars;
        int remLen = src->length;
        for (int k = 0; k < sepChainCount; k++) {
            const char* sp = sepChain[k];
            int spl = sepChainLens[k];
            if (spl == 0) break;
            /* Find sp in [rem, rem+remLen) */
            const char* found = NULL;
            for (const char* q = rem; q + spl <= rem + remLen; q++) {
                if (memcmp(q, sp, (size_t)spl) == 0) { found = q; break; }
            }
            if (!found) break; /* keep remainder as last piece */
            ObjString* part = obj_copy_string(rem, (int)(found - rem));
            obj_array_push(arr, OBJ_VAL(part));
            rem = found + spl;
            remLen -= (int)((found + spl) - (rem - spl));
            remLen = (int)(src->chars + src->length - rem);
        }
        /* Dernier morceau = ce qui reste */
        ObjString* last = obj_copy_string(rem, remLen);
        obj_array_push(arr, OBJ_VAL(last));
        vm_pop();
        return OBJ_VAL(arr);
    }

    if (argCount >= 2) {
        ObjString* sep = to_string(args[1]);
        if (!sep || sep->length == 0) {
            splitByCodepoint = true;
        } else {
            sepStr = sep->chars;
            sepLen = sep->length;
        }
    }

    if (splitByCodepoint) {
        int i = 0;
        while (i < src->length) {
            int w = utf8_seq_len((unsigned char)src->chars[i]);
            ObjString* ch = obj_copy_string(src->chars + i, w);
            obj_array_push(arr, OBJ_VAL(ch));
            i += w;
        }
    } else {
        const char* p = src->chars;
        const char* end = src->chars + src->length;
        while (p <= end) {
            /* Also accept bare \n as separator (LF-only line endings) */
            /* Portable memmem: find sepStr in [p, end) */
            const char* foundCRLF = NULL;
            if (sepLen == 1) {
                foundCRLF = (const char*)memchr(p, sepStr[0], (size_t)(end - p));
            } else {
                for (const char* q = p; q + sepLen <= end; q++) {
                    if (memcmp(q, sepStr, (size_t)sepLen) == 0) { foundCRLF = q; break; }
                }
            }
            const char* foundLF   = (sepLen == 2 && sepStr[0] == '\r' && sepStr[1] == '\n')
                                  ? memchr(p, '\n', (size_t)(end - p)) : NULL;
            const char* found = foundCRLF;
            int foundSepLen = sepLen;
            if (foundLF && (!found || foundLF < found)) {
                /* bare \n found before \r\n: check it's not part of a \r\n */
                if (foundLF == p || *(foundLF - 1) != '\r') {
                    found = foundLF;
                    foundSepLen = 1;
                }
            }
            int len = found ? (int)(found - p) : (int)(end - p);
            ObjString* part = obj_copy_string(p, len);
            obj_array_push(arr, OBJ_VAL(part));
            if (!found) break;
            p = found + foundSepLen;
        }
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* TableauVersChaîne(tab [, sep1 [, sep2]]) / ArrayToString — join array
 * elements. Doc 1000017007.
 *  - 1D : sep1 = séparateur d'éléments (défaut CRLF)
 *  - 2D nested : sep1 = séparateur de ligne (défaut CRLF),
 *                sep2 = séparateur de colonne (défaut TAB)
 *
 * En OWL les tableaux 2D sont nested array-of-arrays (cf VISION §5bis), donc
 * un 2D = tableau dont chaque élément est un sous-tableau. La détection est
 * automatique : si le premier élément est un ObjArray, on traite en 2D.
 */
static Value fn_tableau_vers_chaine(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjArray* arr = AS_ARRAY(args[0]);

    bool is2d = arr->elements.count > 0 && IS_ARRAY(arr->elements.values[0]);

    ObjString* sep1 = (argCount >= 2 && IS_STRING(args[1])) ? to_string(args[1])
                    : obj_copy_string("\r\n", 2);
    ObjString* sep2 = (argCount >= 3 && IS_STRING(args[2])) ? to_string(args[2])
                    : obj_copy_string("\t", 1);
    if (!sep1) sep1 = obj_copy_string("\r\n", 2);
    if (!sep2) sep2 = obj_copy_string("\t", 1);

    /* Pour 2D : sep1 = inter-ligne, sep2 = inter-colonne.
     * Pour 1D : on n'utilise que sep1 (sep2 ignoré, conforme doc). */

    /* Calcul de la taille totale en deux passes (évite les realloc). */
    int total = 0;
    for (int i = 0; i < arr->elements.count; i++) {
        Value row = arr->elements.values[i];
        if (is2d && IS_ARRAY(row)) {
            ObjArray* rowArr = AS_ARRAY(row);
            for (int j = 0; j < rowArr->elements.count; j++) {
                ObjString* s = to_string(rowArr->elements.values[j]);
                if (s) total += s->length;
                if (j < rowArr->elements.count - 1) total += sep2->length;
            }
        } else {
            ObjString* s = to_string(row);
            if (s) total += s->length;
        }
        if (i < arr->elements.count - 1) total += sep1->length;
    }

    char* buf = ALLOCATE(char, total + 1);
    char* dst = buf;
    for (int i = 0; i < arr->elements.count; i++) {
        Value row = arr->elements.values[i];
        if (is2d && IS_ARRAY(row)) {
            ObjArray* rowArr = AS_ARRAY(row);
            for (int j = 0; j < rowArr->elements.count; j++) {
                ObjString* s = to_string(rowArr->elements.values[j]);
                if (s) { memcpy(dst, s->chars, s->length); dst += s->length; }
                if (j < rowArr->elements.count - 1) {
                    memcpy(dst, sep2->chars, sep2->length);
                    dst += sep2->length;
                }
            }
        } else {
            ObjString* s = to_string(row);
            if (s) { memcpy(dst, s->chars, s->length); dst += s->length; }
        }
        if (i < arr->elements.count - 1) {
            memcpy(dst, sep1->chars, sep1->length);
            dst += sep1->length;
        }
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, total));
}

/* ----------------------------------------------------------------
 * CSV RFC-4180 (quote-when-needed) — helpers partagés écriture/lecture.
 *
 * Divergence assumée vs canon : la doc décrit TableauVersCSV /
 * CSVVersTableau comme un join/split naïf (le vrai CSV canon passe par
 * HExporteCSV/HImporteCSV). OWL fait du RFC-4180 quote-when-needed : sur
 * données propres (sans guillemet / CR / LF / séparateur dans un champ) la
 * sortie reste octet-pour-octet identique au join naïf ; le quoting ne
 * s'active que dans la zone que la doc déclare elle-même non supportée.
 * ---------------------------------------------------------------- */

/* Un champ doit être quoté s'il contient un guillemet, un CR, un LF, ou
 * (en 2D uniquement) la sous-chaîne séparateur de colonne. En 1D le
 * séparateur n'est pas un délimiteur de cellule, donc on ne quote pas dessus. */
static bool csv_field_needs_quote(const char* s, int slen,
                                  const char* sep, int seplen,
                                  bool quoteOnSep) {
    for (int i = 0; i < slen; i++) {
        char c = s[i];
        if (c == '"' || c == '\r' || c == '\n') return true;
    }
    if (quoteOnSep && seplen > 0 && slen >= seplen) {
        for (int i = 0; i + seplen <= slen; i++) {
            if (memcmp(s + i, sep, (size_t)seplen) == 0) return true;
        }
    }
    return false;
}

/* Longueur émise pour ce champ : si quoté, +2 pour l'encadrement et +1 par
 * guillemet interne (doublement). Sinon longueur brute. */
static int csv_field_emit_len(const char* s, int slen, bool quoted) {
    if (!quoted) return slen;
    int extra = 2;
    for (int i = 0; i < slen; i++)
        if (s[i] == '"') extra++;
    return slen + extra;
}

/* Émet le champ dans dst, en quotant + doublant les guillemets si demandé.
 * Retourne le nouveau curseur. */
static char* csv_write_field(char* dst, const char* s, int slen, bool quoted) {
    if (!quoted) {
        memcpy(dst, s, slen);
        return dst + slen;
    }
    *dst++ = '"';
    for (int i = 0; i < slen; i++) {
        if (s[i] == '"') *dst++ = '"';
        *dst++ = s[i];
    }
    *dst++ = '"';
    return dst;
}

/* TableauVersCSV(tab [, sepColonne]) / ArrayToCSV — canon.
 * 1D: éléments séparés par CRLF ("\r\n").
 * 2D (premier élément est ObjArray): lignes séparées par CRLF, colonnes par sepColonne (défaut ";").
 * OWL: déclenche ErreurDetectée si arg[0] n'est pas un tableau.
 * RFC-4180 quote-when-needed : voir helpers csv_* ci-dessus. */
static Value fn_tableau_vers_csv(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TableauVersCSV",
                              "native", "TableauVersCSV",
                              "arg", "tableau",
                              "expected", "tableau",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    owl_clear_error();
    ObjArray* arr = AS_ARRAY(args[0]);

    bool is2d = arr->elements.count > 0 && IS_ARRAY(arr->elements.values[0]);

    static const char CRLF_SEP[] = "\r\n";
    ObjString* sep2 = (argCount >= 2 && IS_STRING(args[1])) ? AS_STRING(args[1])
                    : obj_copy_string(";", 1);
    if (!sep2) sep2 = obj_copy_string(";", 1);
    /*   : root sep2 — utilise via sep2->chars dans les
     * deux boucles. Sans push, les to_string() / obj_take_string() des
     * iterations peuvent GC entre l'alloc et la lecture → UAF. */
    vm_push(OBJ_VAL(sep2));

    /* En 1D le séparateur n'est pas un délimiteur de cellule : on ne quote
     * que sur guillemet/CR/LF. En 2D on quote aussi sur le séparateur. */
    int total = 0;
    for (int i = 0; i < arr->elements.count; i++) {
        Value row = arr->elements.values[i];
        if (is2d && IS_ARRAY(row)) {
            ObjArray* rowArr = AS_ARRAY(row);
            for (int j = 0; j < rowArr->elements.count; j++) {
                ObjString* s = to_string(rowArr->elements.values[j]);
                if (s) {
                    bool q = csv_field_needs_quote(s->chars, s->length,
                                                   sep2->chars, sep2->length, true);
                    total += csv_field_emit_len(s->chars, s->length, q);
                }
                if (j < rowArr->elements.count - 1) total += sep2->length;
            }
        } else {
            ObjString* s = to_string(row);
            if (s) {
                bool q = csv_field_needs_quote(s->chars, s->length,
                                               sep2->chars, sep2->length, false);
                total += csv_field_emit_len(s->chars, s->length, q);
            }
        }
        if (i < arr->elements.count - 1) total += 2; /* CRLF len */
    }

    char* buf = ALLOCATE(char, total + 1);
    char* dst = buf;
    for (int i = 0; i < arr->elements.count; i++) {
        Value row = arr->elements.values[i];
        if (is2d && IS_ARRAY(row)) {
            ObjArray* rowArr = AS_ARRAY(row);
            for (int j = 0; j < rowArr->elements.count; j++) {
                ObjString* s = to_string(rowArr->elements.values[j]);
                if (s) {
                    bool q = csv_field_needs_quote(s->chars, s->length,
                                                   sep2->chars, sep2->length, true);
                    dst = csv_write_field(dst, s->chars, s->length, q);
                }
                if (j < rowArr->elements.count - 1) {
                    memcpy(dst, sep2->chars, sep2->length);
                    dst += sep2->length;
                }
            }
        } else {
            ObjString* s = to_string(row);
            if (s) {
                bool q = csv_field_needs_quote(s->chars, s->length,
                                               sep2->chars, sep2->length, false);
                dst = csv_write_field(dst, s->chars, s->length, q);
            }
        }
        if (i < arr->elements.count - 1) {
            memcpy(dst, CRLF_SEP, 2);
            dst += 2;
        }
    }
    *dst = '\0';
    /*  : push result avant pop(sep2) pour pas perdre le retour. */
    ObjString* result = obj_take_string(buf, total);
    vm_push(OBJ_VAL(result));
    /* Stack: [..., sep2, result], pop dans l'ordre */
    Value ret = vm_pop();      /* result */
    vm_pop();                   /* sep2 */
    return ret;
}

/* Construit un ObjString depuis un champ CSV brut (slice entre délimiteurs).
 * Si le champ commence par '"' on le traite comme quoté : guillemets
 * d'encadrement retirés, '""' interne restitué en '"' unique. Sinon le champ
 * est restitué tel quel (espaces conservés, pas de trim — RFC-4180). */
static ObjString* csv_unquote_field(const char* s, int len) {
    if (len >= 1 && s[0] == '"') {
        /* Le buffer désescapé est au plus aussi long que la source. */
        char* buf = ALLOCATE(char, len + 1);
        int n = 0;
        int i = 1; /* saute le guillemet ouvrant */
        for (; i < len; i++) {
            if (s[i] == '"') {
                if (i + 1 < len && s[i + 1] == '"') {
                    buf[n++] = '"';
                    i++;
                } else {
                    /* guillemet fermant : on ignore le reste éventuel */
                    break;
                }
            } else {
                buf[n++] = s[i];
            }
        }
        buf[n] = '\0';
        return obj_take_string(buf, n);
    }
    return obj_copy_string(s, len);
}

/* CSVVersTableau(csv [, sepColonne]) / CSVToArray — canon.
 * Sans sepColonne: tableau 1D (chaque enregistrement = un champ).
 * Avec sepColonne: tableau 2D (lignes → colonnes).
 * OWL divergence: renvoie un nouveau tableau (canon modifie un tableau existant par référence).
 * Parsing RFC-4180 quote-aware : un séparateur ou un CR/LF situé à l'intérieur
 * d'un champ quoté ne coupe ni la colonne ni la ligne ; '""' interne restitue '"'.
 * Déclenche ErreurDetectée si arg[0] n'est pas une chaîne. */
static Value fn_csv_vers_tableau(int argCount, Value* args) {
    if (!IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "CSVVersTableau",
                              "native", "CSVVersTableau",
                              "arg", "csv",
                              "expected", "chaine",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_new_array());
    }
    owl_clear_error();
    ObjString* csv = AS_STRING(args[0]);
    bool do2D = (argCount >= 2 && IS_STRING(args[1]) && AS_STRING(args[1])->length > 0);

    ObjArray* outer = obj_new_array();
    vm_push(OBJ_VAL(outer));

    /* Entrée vide → tableau vide (0 ligne), pas de ligne fantôme. */
    if (csv->length == 0) {
        vm_pop();
        return OBJ_VAL(outer);
    }

    const char* sepChars = do2D ? AS_STRING(args[1])->chars : NULL;
    int         sepLen   = do2D ? AS_STRING(args[1])->length : 0;

    const char* p   = csv->chars;
    const char* end = csv->chars + csv->length;

    /* Ligne courante (2D) : ObjArray de colonnes, rooté pendant la construction. */
    ObjArray* curRow = NULL;
    const char* fieldStart = p;
    bool inQuotes = false;

    while (p <= end) {
        bool atEnd = (p == end);
        bool atRowSep = false;
        int  rowSepSkip = 0;
        bool atColSep = false;

        if (!atEnd && !inQuotes) {
            if (p[0] == '\r') {
                atRowSep = true;
                rowSepSkip = (p + 1 < end && p[1] == '\n') ? 2 : 1;
            } else if (p[0] == '\n') {
                atRowSep = true;
                rowSepSkip = 1;
            } else if (do2D && sepLen > 0 && p + sepLen <= end &&
                       memcmp(p, sepChars, (size_t)sepLen) == 0) {
                atColSep = true;
            }
        }

        if (!atEnd && !atRowSep && !atColSep) {
            if (p[0] == '"') inQuotes = !inQuotes;
            p++;
            continue;
        }

        /* Frontière de champ atteinte (séparateur, fin de ligne, ou EOF).
         *  ORDRE PUSH critique : curRow d'abord (s'il vient
         * d'etre cree), field ensuite. Sinon le vm_pop ci-dessous popperait
         * curRow (qui serait au TOP), laissant curRow unrooted pour les
         * iterations suivantes → GC le sweep → tout ecrit dans curRow
         * suivant lit 0xDD poison. Bug detecte via OWL_GC_POISON sur
         * test_csv_rfc4180. */
        if (do2D && curRow == NULL) {
            curRow = obj_new_array();
            vm_push(OBJ_VAL(curRow));
        }
        ObjString* field = csv_unquote_field(fieldStart, (int)(p - fieldStart));
        vm_push(OBJ_VAL(field));

        if (do2D) {
            obj_array_push(curRow, OBJ_VAL(field));
        } else {
            obj_array_push(outer, OBJ_VAL(field));
        }
        vm_pop(); /* field — TOP, curRow reste pour la suite de la rangee */

        if (atColSep) {
            p += sepLen;
            fieldStart = p;
            continue;
        }

        /* Fin de ligne ou EOF : on clôt la ligne courante (2D). */
        if (do2D && curRow != NULL) {
            obj_array_push(outer, OBJ_VAL(curRow));
            vm_pop(); /* curRow */
            curRow = NULL;
        }

        if (atEnd) break;

        p += rowSepSkip;
        fieldStart = p;
        /* Un CR/LF final ne doit pas produire une ligne vide fantôme. */
        if (p == end) break;
    }

    vm_pop(); /* outer */
    return OBJ_VAL(outer);
}

/* ChaîneInsère(s, insert, pos) / StringInsert — canon: (chaîne, chaîneAInsérer, position)
 * args: 0=source, 1=string_to_insert, 2=position (base 1) */
static Value fn_chaine_insere(int argCount, Value* args) {
    (void)argCount;
    ObjString* src = to_string(args[0]);
    ObjString* ins = to_string(args[1]);
    int cp = to_int(args[2]) - 1; /* 0-based */
    if (!src || !ins) return args[0];
    if (cp < 0) cp = 0;
    int bytePos = str_byte_at_cp(src, cp);
    if (bytePos > src->length) bytePos = src->length;
    int newLen = src->length + ins->length;
    char* buf = ALLOCATE(char, newLen + 1);
    memcpy(buf, src->chars, bytePos);
    memcpy(buf + bytePos, ins->chars, ins->length);
    memcpy(buf + bytePos + ins->length, src->chars + bytePos, src->length - bytePos);
    buf[newLen] = '\0';
    return OBJ_VAL(obj_take_string(buf, newLen));
}

/* ----------------------------------------------------------------
 * Search helpers — used by ChaîneSupprime, ChaîneOccurrence,
 * PositionOccurrence. Must be defined before the functions that use them.
 * ---------------------------------------------------------------- */

static int is_word_boundary(unsigned char c) {
    return c < 0x80 && !isalnum(c) && c != '_';
}

/* Find next occurrence of needle in hay[0..haylen), respecting opts flags.
 * Sets *matchLen = matched byte length. Returns pointer to match or NULL. */
static const char* find_with_opts(const char* hay, int haylen,
                                   const char* needle, int needlelen,
                                   int opts, int* matchLen) {
    if (needlelen == 0 || haylen < needlelen) return NULL;
    int sansCase = (opts & SEARCH_SANS_CASSE);
    int motComp  = (opts & SEARCH_MOT_COMPLET);

    if (sansCase) {
        /* SansCasse englobe l'accent-insensibilite (canon) : on delegue au
         * comparateur canonique casse+accent. Le span matche dans le hay peut
         * differer de needlelen (ex : "DEBUT" matche "DEBUT" via strip accents),
         * d'ou le calcul par cc_prefix_span. */
        int ccFlags = CC_SANS_CASSE | CC_SANS_ACCENT;
        char pbuf[4096];
        int pn = cc_normalize(needle, needlelen, pbuf, (int)sizeof(pbuf), ccFlags);
        if (pn == 0) return NULL;
        for (int i = 0; i + pn <= haylen; i++) {
            int span = cc_prefix_span(hay + i, haylen - i, pbuf, pn, ccFlags);
            if (span < 0) continue;
            if (motComp) {
                int before_ok = (i == 0 || is_word_boundary((unsigned char)hay[i-1]));
                int after_ok  = (i + span >= haylen ||
                                 is_word_boundary((unsigned char)hay[i + span]));
                if (!(before_ok && after_ok)) continue;
            }
            *matchLen = span;
            return hay + i;
        }
        return NULL;
    }

    /* Sensible a la casse : comparaison byte-exacte, whole-word optionnel. */
    *matchLen = needlelen;
    for (int i = 0; i <= haylen - needlelen; i++) {
        if (memcmp(hay + i, needle, (size_t)needlelen) != 0) continue;
        if (motComp) {
            int before_ok = (i == 0 || is_word_boundary((unsigned char)hay[i-1]));
            int after_ok  = (i + needlelen >= haylen ||
                             is_word_boundary((unsigned char)hay[i + needlelen]));
            if (!(before_ok && after_ok)) continue;
        }
        return hay + i;
    }
    return NULL;
}

/* ChaîneSupprime / StringDelete — two canon forms:
 *   Form 1: ChaîneSupprime(chaîne, sous-chaîne [, options]) → remove all occurrences of sous-chaîne
 *   Form 2: ChaîneSupprime(chaîne, position, nombre)        → delete nombre codepoints at position (base 1)
 * Dispatch: if args[1] is string → form 1; if args[1] is integer → form 2. */
static Value fn_chaine_supprime_chaine(int argCount, Value* args) {
    if (argCount < 2) return args[0];  /*  garde sous-application */
    ObjString* src = to_string(args[0]);
    if (!src) return args[0];

    /* Form 2: ChaîneSupprime(s, pos, len) — position is always an integer */
    if (argCount >= 3 && IS_INTEGER(args[1])) {
        int cp = to_int(args[1]) - 1; /* 0-based */
        int count = to_int(args[2]);
        if (cp < 0 || count <= 0) return args[0];
        int byteStart = str_byte_at_cp(src, cp);
        int byteEnd   = str_byte_at_cp(src, cp + count);
        if (byteStart >= src->length) return args[0];
        if (byteEnd > src->length) byteEnd = src->length;
        int newLen = src->length - (byteEnd - byteStart);
        char* buf = ALLOCATE(char, newLen + 1);
        memcpy(buf, src->chars, byteStart);
        memcpy(buf + byteStart, src->chars + byteEnd, src->length - byteEnd);
        buf[newLen] = '\0';
        return OBJ_VAL(obj_take_string(buf, newLen));
    }

    /* Form 1: ChaîneSupprime(s, sub [, options]) — remove all occurrences.
     * Doc: options = MotComplet (1) | SansCasse (2), combinable. */
    ObjString* sub = to_string(args[1]);
    if (!sub || sub->length == 0) return args[0];
    int opts = (argCount >= 3) ? to_int(args[2]) : 0;

    char* buf = ALLOCATE(char, src->length + 1);
    char* dst = buf;
    const char* p = src->chars;
    const char* end = src->chars + src->length;
    while (p < end) {
        int mlen;
        const char* hit = find_with_opts(p, (int)(end - p),
                                         sub->chars, sub->length, opts, &mlen);
        if (!hit) { memcpy(dst, p, end - p); dst += end - p; break; }
        memcpy(dst, p, hit - p); dst += hit - p;
        p = hit + mlen;
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, (int)(dst - buf)));
}

/* ChaîneRécupère(s, pos, n) / StringRetrieve — alias for Milieu */
static Value fn_chaine_recupere(int argCount, Value* args) {
    return fn_milieu(argCount, args);
}

/* ChaîneOccurrence(s, sub [, options]) / StringCount
 * Doc: options = MotComplet (1) | SansCasse (2).
 * Also accepts array of strings as 2nd arg. */
static Value fn_chaine_occurrence(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(0);  /*  garde sous-application */
    ObjString* hay = to_string(args[0]);
    if (!hay) return INTEGER_VAL(0);
    int opts = (argCount >= 3) ? to_int(args[2]) : 0;

    /* Array of search strings */
    if (argCount >= 2 && IS_ARRAY(args[1])) {
        ObjArray* needles = AS_ARRAY(args[1]);
        int total = 0;
        for (int ni = 0; ni < needles->elements.count; ni++) {
            ObjString* needle = to_string(needles->elements.values[ni]);
            if (!needle || needle->length == 0) continue;
            const char* p = hay->chars;
            int remaining = hay->length;
            int mlen;
            while ((find_with_opts(p, remaining, needle->chars, needle->length,
                                   opts, &mlen)) != NULL) {
                const char* hit = find_with_opts(p, remaining,
                                                 needle->chars, needle->length,
                                                 opts, &mlen);
                total++;
                int advance = (int)(hit - p) + mlen;
                p += advance; remaining -= advance;
            }
        }
        return INTEGER_VAL(total);
    }

    /* Single search string */
    ObjString* needle = to_string(args[1]);
    if (!needle || needle->length == 0) return INTEGER_VAL(0);

    int count = 0;
    const char* p = hay->chars;
    int remaining = hay->length;
    while (1) {
        int mlen;
        const char* hit = find_with_opts(p, remaining,
                                         needle->chars, needle->length,
                                         opts, &mlen);
        if (!hit) break;
        count++;
        int advance = (int)(hit - p) + mlen;
        p += advance; remaining -= advance;
    }
    return INTEGER_VAL(count);
}

/* ChaîneSupprimeDoublon(s [, sep [, comparaison]]) / StringDeleteDuplicate
 * Doc: removes ALL duplicates (not just consecutive). Default sep = TAB.
 * comparaison = cc* flags (combinable). Without sep: char-level, no options.
 * With sep: token-level, keeps first occurrence of each token. */
static Value fn_chaine_supprime_doublon(int argCount, Value* args) {
    ObjString* src = to_string(args[0]);
    if (!src) return OBJ_VAL(obj_copy_string("", 0));

    if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* sep = to_string(args[1]);
        int opts = (argCount >= 3) ? to_int(args[2]) : 0;
        int sansCase = (opts & CC_SANS_CASSE);
        int sansAccent = (opts & CC_SANS_ACCENT);

        /* Split by separator.  : push partsV pendant l'iteration
         * (ALLOCATE + memcpy peuvent declencher GC → parts collected). */
        Value splitArgs[2] = { args[0], args[1] };
        Value partsV = fn_chaine_vers_tableau(2, splitArgs);
        vm_push(partsV);
        ObjArray* parts = AS_ARRAY(partsV);

        /* Rebuild keeping only first occurrence of each token */
        char* buf = ALLOCATE(char, src->length * 2 + 1);
        char* dst = buf;
        bool first = true;
        for (int i = 0; i < parts->elements.count; i++) {
            ObjString* tok = to_string(parts->elements.values[i]);
            if (!tok) continue;
            /* Check if already seen (scan preceding accepted tokens) */
            bool seen = false;
            for (int j = 0; j < i && !seen; j++) {
                ObjString* prev = to_string(parts->elements.values[j]);
                /* prev may have been deduped already; check by comparing with tok */
                if (!prev) continue;
                char ta[256], tb[256];
                const char* ca = tok->chars;
                const char* cb = prev->chars;
                if (sansAccent) {
                    scanner_strip_accents(tok->chars, tok->length, ta, sizeof(ta));
                    scanner_strip_accents(prev->chars, prev->length, tb, sizeof(tb));
                    ca = ta; cb = tb;
                }
                int eq = sansCase ? strcasecmp(ca, cb) : strcmp(ca, cb);
                if (eq == 0) seen = true;
            }
            if (seen) continue;
            /* Add sep before all but first */
            if (!first && sep->length > 0) {
                memcpy(dst, sep->chars, sep->length); dst += sep->length;
            }
            memcpy(dst, tok->chars, tok->length); dst += tok->length;
            first = false;
        }
        *dst = '\0';
        vm_pop(); /* partsV */
        return OBJ_VAL(obj_take_string(buf, (int)(dst - buf)));
    }

    /* No separator or non-string arg → char-level: remove consecutive duplicate
     * codepoints (preserves original behaviour for no-sep case) */
    char* buf = ALLOCATE(char, src->length + 1);
    char* dst = buf;
    int i = 0;
    int prevStart = -1, prevLen = 0;
    while (i < src->length) {
        int w = utf8_seq_len((unsigned char)src->chars[i]);
        if (prevStart < 0 || prevLen != w ||
            memcmp(src->chars + prevStart, src->chars + i, (size_t)w) != 0) {
            memcpy(dst, src->chars + i, w);
            dst += w;
            prevStart = i;
            prevLen = w;
        }
        i += w;
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, (int)(dst - buf)));
}

/* ChaîneIncremente(chaîne [, position]) / StringIncrement — doc behavior:
 * increments the numeric part at end (default) or beginning of the string.
 * "ABC001" → "ABC002", "ABC999" → "ABC1000", "ABC" → "ABC1" (append "1" if no digits).
 * Doc 1000020773: DepuisFin(0), DepuisDebut(1).
 * Without position: end if ends with digit, beginning if begins with digit, else append 1. */
static Value fn_chaine_incremente(int argCount, Value* args) {
    ObjString* src = to_string(args[0]);
    if (!src || src->length == 0) return OBJ_VAL(obj_copy_string("1", 1));

    const char* s = src->chars;
    int len = src->length;

    /* Constantes (post-unification enum optionsRechercheChaîne) :
     *   DepuisDébut = 0  (valeur neutre, défaut combinable)
     *   DepuisFin   = SEARCH_DEPUIS_FIN (4)
     * Le mode est strictement bit-driven : on regarde le bit DepuisFin.
     * Sans argument explicite, auto-detect 3-way (fin de chiffres, début
     * de chiffres, sinon append "1" à la fin). */
    int pos_mode;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        pos_mode = (int)AS_INTEGER(args[1]);
    } else {
        bool ends_digit   = (len > 0 && s[len-1] >= '0' && s[len-1] <= '9');
        bool starts_digit = (len > 0 && s[0] >= '0' && s[0] <= '9');
        if      (ends_digit)   pos_mode = SEARCH_DEPUIS_FIN; /* fin */
        else if (starts_digit) pos_mode = 0;                 /* début */
        else                   pos_mode = SEARCH_DEPUIS_FIN; /* append "1" à la fin */
    }

    /* Mode "fin" si et seulement si le bit DepuisFin est positionné. */
    if (pos_mode & SEARCH_DEPUIS_FIN) {
        /* Find numeric run at end of string */
        int num_end = len;
        int num_start = num_end;
        while (num_start > 0 && s[num_start - 1] >= '0' && s[num_start - 1] <= '9') num_start--;
        int num_len = num_end - num_start; /* 0 = no digits at end */

        if (num_len == 0) {
            /* No digits: append "1" */
            char* buf = ALLOCATE(char, len + 2);
            memcpy(buf, s, len);
            buf[len] = '1'; buf[len + 1] = '\0';
            return OBJ_VAL(obj_take_string(buf, len + 1));
        }

        /* Increment numeric part, preserving zero-padding width if result fits. */
        long long num = 0;
        for (int i = num_start; i < num_end; i++) num = num * 10 + (s[i] - '0');
        num++;
        char numbuf[32];
        int digits = snprintf(numbuf, sizeof(numbuf), "%lld", num);
        /* Preserve leading-zero width only if result still fits (no carry overflow) */
        int nlen = (digits <= num_len) ? num_len : digits;
        if (digits < num_len) {
            /* Zero-pad to original width by shifting digits right and filling zeros */
            int pad = num_len - digits;
            memmove(numbuf + pad, numbuf, (size_t)digits + 1);
            for (int pi = 0; pi < pad; pi++) numbuf[pi] = '0';
        }
        int prefix_len = num_start;
        char* buf = ALLOCATE(char, prefix_len + nlen + 1);
        memcpy(buf, s, prefix_len);
        memcpy(buf + prefix_len, numbuf, nlen);
        buf[prefix_len + nlen] = '\0';
        return OBJ_VAL(obj_take_string(buf, prefix_len + nlen));
    } else {
        /* champDébut: find numeric run at beginning */
        int num_start = 0;
        int num_end = 0;
        while (num_end < len && s[num_end] >= '0' && s[num_end] <= '9') num_end++;
        int num_len = num_end - num_start;

        if (num_len == 0) {
            /* No digits at start: prepend "1" */
            char* buf = ALLOCATE(char, len + 2);
            buf[0] = '1';
            memcpy(buf + 1, s, len);
            buf[len + 1] = '\0';
            return OBJ_VAL(obj_take_string(buf, len + 1));
        }

        long long num = 0;
        for (int i = num_start; i < num_end; i++) num = num * 10 + (s[i] - '0');
        num++;
        char numbuf[32];
        int digits = snprintf(numbuf, sizeof(numbuf), "%lld", num);
        int nlen = (digits <= num_len) ? num_len : digits;
        if (digits < num_len) {
            int pad = num_len - digits;
            memmove(numbuf + pad, numbuf, (size_t)digits + 1);
            for (int pi = 0; pi < pad; pi++) numbuf[pi] = '0';
        }
        int suffix_len = len - num_end;
        char* buf = ALLOCATE(char, nlen + suffix_len + 1);
        memcpy(buf, numbuf, nlen);
        memcpy(buf + nlen, s + num_end, suffix_len);
        buf[nlen + suffix_len] = '\0';
        return OBJ_VAL(obj_take_string(buf, nlen + suffix_len));
    }
}

/* ExtraitChaîneEntre(chaîne, rang, séparateurDébut [, séparateurFin]) / ExtractStringBetween
 * canon signature: (Chaîne, Rang, SéparateurDébut [, SéparateurFin])
 * args: 0=source, 1=rang (base 1), 2=opening delimiter, 3=closing delimiter (optional, defaults to opening) */
/* ExtraitChaîneEntre(s, rang, sepDébut [, sepFin [, options]]) / ExtractStringBetween
 * Doc: rang = integer (1-based), or rangPremier/rangSuivant traversal constant.
 * Returns EOT when rang > number of occurrences found.
 * options: DepuisFin, MotComplet, SansCasse (combinable). */

/* Traversal state for rangPremier/rangSuivant mode */
static const char* g_entre_pos = NULL;
static const ObjString* g_entre_src = NULL;

/* Paire de delimiteurs (debut, fin) pour ExtraitChaineEntre.
 * Forme array : N pairs alignees ; forme single string : 1 pair. */
#define MAX_XCE_PAIRS 16
typedef struct {
    char deb[128];
    int  debLen;
    char fin[128];
    int  finLen;
    bool symmetric;  /* deb == fin (alors `|a|b|c|` rang=2 -> "b") */
} XcePair;

/* Charge args[2] (deb) et args[3] (fin) en pairs. Retourne le nombre de
 * pairs chargees (0 = invalide).
 *   - args[2] = string et args[3] = string (ou absent) -> 1 pair
 *   - args[2] = array de strings et args[3] = array meme cardinalite -> N pairs
 *   - les autres combinaisons sont rejetees. */
static int load_xce_pairs(int argCount, Value* args, XcePair* out) {
    Value vDeb = args[2];
    Value vFin = (argCount >= 4) ? args[3] : args[2];
    /* Si vFin est un options int (forme 4-args avec opts en 4eme arg, sans
     * sepFin explicite), on tombe back sur sepDeb. */
    if (!IS_STRING(vFin) && !IS_ARRAY(vFin)) vFin = vDeb;

    if (IS_STRING(vDeb) && IS_STRING(vFin)) {
        ObjString* d = AS_STRING(vDeb);
        ObjString* f = AS_STRING(vFin);
        if (d->length <= 0 || d->length >= 128) return 0;
        if (f->length <= 0 || f->length >= 128) return 0;
        memcpy(out[0].deb, d->chars, (size_t)d->length);
        out[0].deb[d->length] = '\0';
        out[0].debLen = d->length;
        memcpy(out[0].fin, f->chars, (size_t)f->length);
        out[0].fin[f->length] = '\0';
        out[0].finLen = f->length;
        out[0].symmetric = (d->length == f->length
                          && memcmp(d->chars, f->chars, (size_t)d->length) == 0);
        return 1;
    }

    if (IS_ARRAY(vDeb) && IS_ARRAY(vFin)) {
        ObjArray* aDeb = AS_ARRAY(vDeb);
        ObjArray* aFin = AS_ARRAY(vFin);
        int n = aDeb->elements.count;
        if (n != aFin->elements.count) return 0;
        if (n <= 0) return 0;
        if (n > MAX_XCE_PAIRS) n = MAX_XCE_PAIRS;
        int loaded = 0;
        for (int i = 0; i < n; i++) {
            Value vd = aDeb->elements.values[i];
            Value vf = aFin->elements.values[i];
            if (!IS_STRING(vd) || !IS_STRING(vf)) continue;
            ObjString* d = AS_STRING(vd);
            ObjString* f = AS_STRING(vf);
            if (d->length <= 0 || d->length >= 128) continue;
            if (f->length <= 0 || f->length >= 128) continue;
            memcpy(out[loaded].deb, d->chars, (size_t)d->length);
            out[loaded].deb[d->length] = '\0';
            out[loaded].debLen = d->length;
            memcpy(out[loaded].fin, f->chars, (size_t)f->length);
            out[loaded].fin[f->length] = '\0';
            out[loaded].finLen = f->length;
            out[loaded].symmetric = (d->length == f->length
                              && memcmp(d->chars, f->chars, (size_t)d->length) == 0);
            loaded++;
        }
        return loaded;
    }
    return 0;
}

/* Cherche dans [from, end) la 1ere occurrence d'une paire (deb,fin) parmi
 * les pairs : on prend le `deb` qui demarre le plus tot, puis on cherche
 * son `fin` dans le contenu restant.
 *
 *   *outInnerStart : debut du contenu (apres deb)
 *   *outInnerEnd   : fin du contenu (avant fin)
 *   *outAdvance    : ou continuer le scan (apres ce match)
 *
 * Retourne true si match trouve, false sinon. */
static bool xce_find_next(const char* from, const char* end,
                           const XcePair* pairs, int npairs, int opts,
                           const char** outInnerStart,
                           const char** outInnerEnd,
                           const char** outAdvance) {
    /* Le sens de parcours (DepuisFin) est gere par l'appelant ; ici on ne
     * retient que les options de matching des separateurs. */
    int findOpts = opts & (SEARCH_MOT_COMPLET | SEARCH_SANS_CASSE);
    const char* bestSd = NULL;
    const char* bestEd = NULL;
    int bestIdx = -1;
    int bestDebSpan = 0;
    int bestFinSpan = 0;

    for (int i = 0; i < npairs; i++) {
        int dlen;
        const char* sd = find_with_opts(from, (int)(end - from),
                                        pairs[i].deb, pairs[i].debLen, findOpts, &dlen);
        if (!sd) continue;
        const char* inner = sd + dlen;
        if (inner > end) continue;
        int flen;
        const char* ed = find_with_opts(inner, (int)(end - inner),
                                        pairs[i].fin, pairs[i].finLen, findOpts, &flen);
        if (!ed) continue;
        if (bestSd == NULL || sd < bestSd) {
            bestSd = sd;
            bestEd = ed;
            bestIdx = i;
            bestDebSpan = dlen;
            bestFinSpan = flen;
        }
    }
    if (bestSd == NULL) return false;
    *outInnerStart = bestSd + bestDebSpan;
    *outInnerEnd   = bestEd;
    *outAdvance    = pairs[bestIdx].symmetric
                    ? bestEd
                    : bestEd + bestFinSpan;
    return true;
}

static Value fn_extrait_chaine_entre(int argCount, Value* args) {
    if (argCount < 3) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));  /*  garde */
    ObjString* src = to_string(args[0]);
    if (!src) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));

    XcePair pairs[MAX_XCE_PAIRS];
    int npairs = load_xce_pairs(argCount, args, pairs);
    if (npairs == 0) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));

    int rang2 = to_int(args[1]); /* integer rank OR traversal constant */
    /* Options : si args[3] est un int, c'est la forme 4-args avec opts en
     * position 3 (sans sepFin). Sinon options en position 4. */
    int opts = 0;
    if (argCount >= 5) opts = to_int(args[4]);
    else if (argCount == 4 && IS_INTEGER(args[3])) opts = to_int(args[3]);
    bool depuisFin = (opts & SEARCH_DEPUIS_FIN);

    if (rang2 == RANG_DERNIER || rang2 == RANG_PRECEDENT) {
        depuisFin = true;
    }

    const char* end = src->chars + src->length;

    /* Traversal mode : rangPremier reset + find first, rangSuivant continue */
    if (rang2 == RANG_PREMIER) {
        g_entre_src = src;
        const char *iS, *iE, *adv;
        if (!xce_find_next(src->chars, end, pairs, npairs, opts, &iS, &iE, &adv)) {
            g_entre_pos = end;
            return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        }
        g_entre_pos = adv;
        return OBJ_VAL(obj_copy_string(iS, (int)(iE - iS)));
    }
    if (rang2 == RANG_SUIVANT) {
        if (g_entre_src != src || g_entre_pos == NULL || g_entre_pos >= end) {
            return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        }
        const char *iS, *iE, *adv;
        if (!xce_find_next(g_entre_pos, end, pairs, npairs, opts, &iS, &iE, &adv)) {
            g_entre_pos = end;
            return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        }
        g_entre_pos = adv;
        return OBJ_VAL(obj_copy_string(iS, (int)(iE - iS)));
    }
    if (rang2 == RANG_DERNIER) {
        rang2 = 1;
    }

    if (rang2 < 1) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));

    if (depuisFin) {
        /* Two-pass sans plafond : passe 1 compte les correspondances, passe 2
         * re-scanne jusqu'à l'index cible (total - rang2). O(2n), pas de tableau. */
        int mc = 0;
        const char* p = src->chars;
        while (p < end) {
            const char *iS, *iE, *adv;
            if (!xce_find_next(p, end, pairs, npairs, opts, &iS, &iE, &adv)) break;
            mc++;
            p = adv;
        }
        int idx = mc - rang2;
        if (idx < 0) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
        int cur = 0;
        p = src->chars;
        while (p < end) {
            const char *iS, *iE, *adv;
            if (!xce_find_next(p, end, pairs, npairs, opts, &iS, &iE, &adv)) break;
            if (cur == idx) return OBJ_VAL(obj_copy_string(iS, (int)(iE - iS)));
            cur++;
            p = adv;
        }
        return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
    }

    /* Forward scan for rank `rang2` */
    const char* p = src->chars;
    int found = 0;
    while (p < end) {
        const char *iS, *iE, *adv;
        if (!xce_find_next(p, end, pairs, npairs, opts, &iS, &iE, &adv)) break;
        found++;
        if (found == rang2) {
            return OBJ_VAL(obj_copy_string(iS, (int)(iE - iS)));
        }
        p = adv;
    }
    return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
}

/* ExtraitLigne(s, n) / ExtractLine — extract nth line (base 1).
 * Doc: returns EOT (Caract(4)) when n > number of lines. */
static Value fn_extrait_ligne(int argCount, Value* args) {
    (void)argCount;
    ObjString* src = to_string(args[0]);
    int n = to_int(args[1]);
    if (!src || n < 1) return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));

    const char* p = src->chars;
    int line = 1;
    while (p <= src->chars + src->length) {
        const char* lf = memchr(p, '\n', (size_t)(src->chars + src->length - p));
        int len = lf ? (int)(lf - p) : (int)(src->chars + src->length - p);
        /* Strip trailing \r for CRLF */
        if (len > 0 && p[len - 1] == '\r') len--;
        if (line == n) return OBJ_VAL(obj_copy_string(p, len));
        if (!lf) break;
        p = lf + 1;
        line++;
    }
    return OBJ_VAL(obj_copy_string(EOT_CHAR, 1));
}

/* PositionOccurrence traversal state */
static const char* g_posocc_pos = NULL;
static const ObjString* g_posocc_src = NULL;

/* Etat de parcours pour la forme array : positions pre-calculees triees +
 * index courant. Utilise par rangPremier / rangSuivant sur tableau.
 * Buffer alloue dynamiquement (int, hors GC), lifetime rangPremier->rangSuivant,
 * libere sur re-init rangPremier ET dans stdlib_string_reset_globals. */
static int* g_posocc_arr_positions = NULL;
static int g_posocc_arr_cap = 0;
static int g_posocc_arr_count = 0;
static int g_posocc_arr_idx = 0;
static const ObjString* g_posocc_arr_src = NULL;

/* PositionOccurrence(s, sub, rang [, options]) / PositionOccurrence
 * Doc: rang = integer (1-based) OR traversal constant (rangPremier/rangSuivant/etc).
 * options = MotComplet | SansCasse | DepuisFin (combinable).
 * Returns 0 when not found or traversal ended. */
static Value fn_position_occurrence(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(0);  /*  garde sous-application */
    ObjString* hay    = to_string(args[0]);
    int rang  = (argCount >= 3) ? to_int(args[2]) : 1;
    int opts  = (argCount >= 4) ? to_int(args[3]) : 0;
    if (!hay) return INTEGER_VAL(0);

    /* Forme tableau de chercheurs : PositionOccurrence(s, [c1,c2,...], N|rang*) */
    if (IS_ARRAY(args[1])) {
        ObjArray* arr = AS_ARRAY(args[1]);
        if (arr->elements.count == 0) return INTEGER_VAL(0);

        /* Mode rangSuivant : retourne la position pre-calculee suivante */
        if (rang == RANG_SUIVANT) {
            if (g_posocc_arr_src != hay || g_posocc_arr_idx >= g_posocc_arr_count) {
                return INTEGER_VAL(0);
            }
            int byteOff = g_posocc_arr_positions[g_posocc_arr_idx++];
            return INTEGER_VAL(str_cp_at_byte(hay, byteOff) + 1);
        }

        /* Pre-calcul : collecter toutes les positions et trier ascendant.
         * Buffer croissant (int, hors GC) — pas de plafond. */
        bool isTraversal = (rang == RANG_PREMIER);
        int* positions = NULL;
        int cap = 0;
        int count = 0;

        for (int i = 0; i < arr->elements.count; i++) {
            Value sub = arr->elements.values[i];
            if (!IS_STRING(sub)) continue;
            ObjString* nd = AS_STRING(sub);
            if (nd->length == 0) continue;
            const char* p = hay->chars;
            int rem = hay->length;
            int mlen;
            while (1) {
                const char* hit = find_with_opts(p, rem, nd->chars, nd->length, opts, &mlen);
                if (!hit) break;
                if (count + 1 > cap) {
                    int oldCap = cap;
                    cap = GROW_CAPACITY(cap);
                    positions = GROW_ARRAY(int, positions, oldCap, cap);
                }
                positions[count++] = (int)(hit - hay->chars);
                int adv = (int)(hit - p) + mlen;
                p += adv; rem -= adv;
            }
        }
        /* Tri ascendant (insertion sort) */
        for (int i = 1; i < count; i++) {
            int v = positions[i]; int j = i - 1;
            while (j >= 0 && positions[j] > v) { positions[j+1] = positions[j]; j--; }
            positions[j+1] = v;
        }

        if (isTraversal) {
            /* Transfert vers l'etat de parcours global (lifetime rangPremier
             * -> rangSuivant). Libere l'allocation precedente d'abord. */
            if (g_posocc_arr_positions) {
                FREE_ARRAY(int, g_posocc_arr_positions, g_posocc_arr_cap);
            }
            g_posocc_arr_positions = positions;
            g_posocc_arr_cap = cap;
            g_posocc_arr_count = count;
            g_posocc_arr_src = hay;
            g_posocc_arr_idx = 0;
            if (count == 0) return INTEGER_VAL(0);
            int byteOff = g_posocc_arr_positions[g_posocc_arr_idx++];
            return INTEGER_VAL(str_cp_at_byte(hay, byteOff) + 1);
        }

        /* Rang fixe : indexe puis libere le buffer local (tous les chemins). */
        int result = 0;
        if (rang >= 1 && rang <= count) {
            result = str_cp_at_byte(hay, positions[rang - 1]) + 1;
        }
        if (positions) FREE_ARRAY(int, positions, cap);
        return INTEGER_VAL(result);
    }

    ObjString* needle = to_string(args[1]);
    if (!needle || needle->length == 0) return INTEGER_VAL(0);

    /* Traversal mode */
    if (rang == RANG_PREMIER) {
        g_posocc_src = hay;
        /* Find first occurrence and set position past it */
        int mlen;
        const char* hit = find_with_opts(hay->chars, hay->length,
                                         needle->chars, needle->length, opts, &mlen);
        if (!hit) { g_posocc_pos = hay->chars + hay->length; return INTEGER_VAL(0); }
        g_posocc_pos = hit + mlen;
        return INTEGER_VAL(str_cp_at_byte(hay, (int)(hit - hay->chars)) + 1);
    } else if (rang == RANG_SUIVANT) {
        if (g_posocc_src != hay || g_posocc_pos == NULL) return INTEGER_VAL(0);
        int mlen;
        const char* remaining_start = g_posocc_pos;
        int remaining = (int)(hay->chars + hay->length - remaining_start);
        const char* hit = find_with_opts(remaining_start, remaining,
                                         needle->chars, needle->length, opts, &mlen);
        if (!hit) { g_posocc_pos = hay->chars + hay->length; return INTEGER_VAL(0); }
        g_posocc_pos = hit + mlen;
        int byteOff = (int)(hit - hay->chars);
        return INTEGER_VAL(str_cp_at_byte(hay, byteOff) + 1);
    } else if (rang == RANG_DERNIER) {
        /* Find last occurrence */
        const char* p = hay->chars;
        int rem = hay->length;
        const char* last = NULL;
        int mlen;
        while (1) {
            const char* hit = find_with_opts(p, rem, needle->chars, needle->length,
                                             opts, &mlen);
            if (!hit) break;
            last = hit;
            int adv = (int)(hit - p) + mlen;
            p += adv; rem -= adv;
        }
        if (!last) return INTEGER_VAL(0);
        g_posocc_src = hay;
        g_posocc_pos = last; /* rangPrécédent will step back */
        return INTEGER_VAL(str_cp_at_byte(hay, (int)(last - hay->chars)) + 1);
    } else if (rang == RANG_PRECEDENT) {
        /* Walk backwards from current position */
        if (g_posocc_src != hay || g_posocc_pos == NULL) return INTEGER_VAL(0);
        /* Collect all matches before current pos and return the last one */
        const char* p = hay->chars;
        int rem = (int)(g_posocc_pos - hay->chars);
        /* Step back by at least needle length so we don't re-find same match */
        if (rem > 0) rem--;
        const char* last = NULL;
        int mlen;
        while (1) {
            const char* hit = find_with_opts(p, rem, needle->chars, needle->length,
                                             opts, &mlen);
            if (!hit) break;
            last = hit;
            int adv = (int)(hit - p) + mlen;
            p += adv; rem -= adv;
        }
        if (!last) return INTEGER_VAL(0);
        g_posocc_pos = last;
        return INTEGER_VAL(str_cp_at_byte(hay, (int)(last - hay->chars)) + 1);
    }

    /* DepuisFin: nth from end — two-pass sans plafond : passe 1 compte les
     * occurrences, passe 2 re-scanne jusqu'à l'index cible (total - rang). */
    bool depuisFin = (opts & SEARCH_DEPUIS_FIN);
    if (depuisFin) {
        const char* p = hay->chars;
        int rem = hay->length;
        int mc = 0, mlen;
        while (1) {
            const char* hit = find_with_opts(p, rem, needle->chars, needle->length,
                                             opts & ~SEARCH_DEPUIS_FIN, &mlen);
            if (!hit) break;
            mc++;
            int adv = (int)(hit - p) + mlen;
            p += adv; rem -= adv;
        }
        int idx = mc - rang;
        if (idx < 0) return INTEGER_VAL(0);
        int cur = 0;
        p = hay->chars;
        rem = hay->length;
        while (1) {
            const char* hit = find_with_opts(p, rem, needle->chars, needle->length,
                                             opts & ~SEARCH_DEPUIS_FIN, &mlen);
            if (!hit) break;
            if (cur == idx) return INTEGER_VAL(str_cp_at_byte(hay, (int)(hit - hay->chars)) + 1);
            cur++;
            int adv = (int)(hit - p) + mlen;
            p += adv; rem -= adv;
        }
        return INTEGER_VAL(0);
    }

    if (rang < 1) return INTEGER_VAL(0);

    /* Forward: find rang-th occurrence */
    const char* p = hay->chars;
    int rem = hay->length;
    int found = 0, mlen;
    while (1) {
        const char* hit = find_with_opts(p, rem, needle->chars, needle->length,
                                         opts, &mlen);
        if (!hit) break;
        found++;
        if (found == rang) {
            return INTEGER_VAL(str_cp_at_byte(hay, (int)(hit - hay->chars)) + 1);
        }
        int adv = (int)(hit - p) + mlen;
        p += adv; rem -= adv;
    }
    return INTEGER_VAL(0);
}

/* PositionVersLigne(s, pos) / PositionToLine — codepoint position → line number (base 1) */
static Value fn_position_vers_ligne(int argCount, Value* args) {
    (void)argCount;
    ObjString* src = to_string(args[0]);
    int pos = to_int(args[1]); /* 1-based codepoint position */
    if (!src || pos < 1) return INTEGER_VAL(1);

    int bytePos = str_byte_at_cp(src, pos - 1);
    int line = 1;
    for (int i = 0; i < bytePos && i < src->length; i++) {
        if (src->chars[i] == '\n') line++;
    }
    return INTEGER_VAL(line);
}

/* TailleVersChaîne(n [, formatAffichage]) / LengthToString
 * Doc 1000017038: 2nd param is a unit constant (tailleO/Ko/Mo/Go/TO) not decimal count.
 * Result always has 2 decimals. Default: auto-select best unit. */
#define TAILLE_AUTO  0
#define TAILLE_O     1
#define TAILLE_KO    2
#define TAILLE_MO    3
#define TAILLE_GO    4
#define TAILLE_TO    5

static Value fn_taille_vers_chaine(int argCount, Value* args) {
    double n = IS_INTEGER(args[0]) ? (double)AS_INTEGER(args[0])
             : IS_REAL(args[0])    ? AS_REAL(args[0]) : 0.0;

    int unit = TAILLE_AUTO;
    if (argCount >= 2 && IS_INTEGER(args[1])) unit = (int)AS_INTEGER(args[1]);

    static const char* labels[] = { "o", "Ko", "Mo", "Go", "To" };
    double v;
    int u;

    if (unit == TAILLE_AUTO || unit == 0) {
        /* Auto: pick best unit */
        u = 0; v = n < 0 ? -n : n;
        while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
        if (n < 0) v = -v;
    } else {
        /* Force unit: tailleO=1, Ko=2, Mo=3, Go=4, To=5 → index 0..4 */
        u = unit - 1;
        if (u < 0) u = 0;
        if (u > 4) u = 4;
        v = n;
        for (int i = 0; i < u; i++) v /= 1024.0;
    }

    /* doc: 2 decimals, but no decimals when value is an exact integer */
    char buf[64];
    if (v == (double)(long long)v && v >= 0) {
        snprintf(buf, sizeof(buf), "%.0f %s", v, labels[u]);
    } else {
        snprintf(buf, sizeof(buf), "%.2f %s", v, labels[u]);
        for (char* p = buf; *p; p++) if (*p == '.') *p = ',';
    }
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}


/* TexteVersHTML(s) / TextToHTML — escape & < > " ' */
static Value fn_texte_vers_html(int argCount, Value* args) {
    /* Doc: optional 2nd param = alphabet constant (e.g. alphabetCourant).
     * OWL is UTF-8 internally so alphabet hint is a no-op — accepted for compat. */
    (void)argCount;
    ObjString* src = to_string(args[0]);
    if (!src) return OBJ_VAL(obj_copy_string("", 0));

    /* Count extra bytes needed (\r dropped, \n -> <BR>) */
    int extra = 0;
    for (int i = 0; i < src->length; i++) {
        char c = src->chars[i];
        if      (c == '&')  extra += 4; /* &amp; (5-1) */
        else if (c == '<')  extra += 3; /* &lt;  (4-1) */
        else if (c == '>')  extra += 3; /* &gt;  (4-1) */
        else if (c == '"')  extra += 5; /* &quot;(6-1) */
        else if (c == '\'') extra += 4; /* &#39; (5-1) */
        else if (c == '\n') extra += 3; /* <BR>  (4-1) */
        else if (c == '\r') extra -= 1; /* dropped — CRLF: the \r vanishes */
    }
    char* buf = ALLOCATE(char, src->length + extra + 1);
    char* dst = buf;
    for (int i = 0; i < src->length; i++) {
        char c = src->chars[i];
        if      (c == '&')  { memcpy(dst, "&amp;",  5); dst += 5; }
        else if (c == '<')  { memcpy(dst, "&lt;",   4); dst += 4; }
        else if (c == '>')  { memcpy(dst, "&gt;",   4); dst += 4; }
        else if (c == '"')  { memcpy(dst, "&quot;", 6); dst += 6; }
        else if (c == '\'') { memcpy(dst, "&#39;",  5); dst += 5; }
        else if (c == '\n') { memcpy(dst, "<BR>",   4); dst += 4; }
        else if (c == '\r') { /* drop — part of CRLF */ }
        else                { *dst++ = c; }
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, (int)(dst - buf)));
}

/* URLExtraitChemin(url, portionAExtraire) / URLExtractPath
 * Doc 1000020965: portionAExtraire is a combinable int constant (url* flags).
 * Multiple portions can be combined with +, result is the concatenated URL pieces.
 * Also accepts legacy string labels for backward compat. */
#define URL_PROTOCOLE       1
#define URL_UTILISATEUR     2
#define URL_MOT_DE_PASSE    4
#define URL_DOMAINE         8
#define URL_PORT           16
#define URL_CHEMIN_RESSOURCE 32
#define URL_NOM_RESSOURCE   64
#define URL_EXTENSION      128
#define URL_REQUETE        256
#define URL_FRAGMENT       512

static Value fn_url_extrait_chemin(int argCount, Value* args) {
    (void)argCount;
    ObjString* url = to_string(args[0]);
    if (!url) return OBJ_VAL(obj_copy_string("", 0));

    const char* s = url->chars;

    /* Parse all components */
    char scheme[64]="", user[256]="", password[256]="", host[256]="", port[16]="";
    char dirpath[1024]="", resname[256]="", ext[64]="", query[1024]="", fragment[256]="";

    const char* p = s;

    /* scheme:// */
    const char* schemeEnd = strstr(p, "://");
    if (schemeEnd) {
        int slen = (int)(schemeEnd - p); if (slen >= 63) slen = 63;
        memcpy(scheme, p, slen); scheme[slen] = '\0';
        p = schemeEnd + 3;
    }

    /* user:password@ */
    const char* at = strchr(p, '@');
    const char* firstSlash = strchr(p, '/');
    if (at && (!firstSlash || at < firstSlash)) {
        const char* colon = (const char*)memchr(p, ':', (size_t)(at - p));
        if (colon) {
            int ulen = (int)(colon - p); if (ulen >= 255) ulen = 255;
            memcpy(user, p, ulen); user[ulen] = '\0';
            int plen = (int)(at - colon - 1); if (plen >= 255) plen = 255;
            memcpy(password, colon + 1, plen); password[plen] = '\0';
        } else {
            int ulen = (int)(at - p); if (ulen >= 255) ulen = 255;
            memcpy(user, p, ulen); user[ulen] = '\0';
        }
        p = at + 1;
    }

    /* host:port */
    const char* pathStart = strchr(p, '/');
    const char* queryMark = strchr(p, '?');
    const char* fragMark  = strchr(p, '#');
    /* hostport ends at first of '/', '?', '#' */
    const char* hostEnd = pathStart;
    if (!hostEnd || (queryMark && queryMark < hostEnd)) hostEnd = queryMark;
    if (!hostEnd || (fragMark  && fragMark  < hostEnd)) hostEnd = fragMark;
    int hplen = hostEnd ? (int)(hostEnd - p) : (int)strlen(p);
    char hostport[272] = "";
    if (hplen >= 271) hplen = 271;
    memcpy(hostport, p, hplen); hostport[hplen] = '\0';
    char* colon = strchr(hostport, ':');
    if (colon) {
        int hlen = (int)(colon - hostport); if (hlen >= 255) hlen = 255;
        memcpy(host, hostport, hlen); host[hlen] = '\0';
        strncpy(port, colon + 1, 15); port[15] = '\0';
    } else {
        int hlen2 = (int)strlen(hostport); if (hlen2 > 255) hlen2 = 255;
        memcpy(host, hostport, hlen2); host[hlen2] = '\0';
    }

    /* path = dirpath + resname + ext */
    char fullpath[1024] = "";
    if (pathStart) {
        const char* pend = queryMark ? queryMark : (fragMark ? fragMark : pathStart + strlen(pathStart));
        int plen = (int)(pend - pathStart); if (plen >= 1023) plen = 1023;
        memcpy(fullpath, pathStart, plen); fullpath[plen] = '\0';
        /* split dir / name / ext */
        const char* lastSlash = strrchr(fullpath, '/');
        if (lastSlash) {
            int dlen = (int)(lastSlash - fullpath) + 1; /* include trailing / */
            if (dlen >= 1023) dlen = 1023;
            memcpy(dirpath, fullpath, dlen); dirpath[dlen] = '\0';
            const char* fname = lastSlash + 1;
            const char* dot = strrchr(fname, '.');
            if (dot) {
                int nlen = (int)(dot - fname); if (nlen >= 255) nlen = 255;
                memcpy(resname, fname, nlen); resname[nlen] = '\0';
                strncpy(ext, dot, 63); ext[63] = '\0'; /* includes the dot */
            } else {
                strncpy(resname, fname, 255); resname[255] = '\0';
            }
        } else {
            int fplen = (int)strlen(fullpath); if (fplen > 1023) fplen = 1023;
            memcpy(dirpath, fullpath, fplen); dirpath[fplen] = '\0';
        }
    }

    /* query (without '?') */
    if (queryMark) {
        const char* qend = fragMark ? fragMark : queryMark + strlen(queryMark);
        int qlen = (int)(qend - queryMark - 1); if (qlen >= 1023) qlen = 1023;
        memcpy(query, queryMark + 1, qlen); query[qlen] = '\0';
    }

    /* fragment (without '#') */
    if (fragMark) { strncpy(fragment, fragMark + 1, 255); fragment[255] = '\0'; }

    /* Determine which portions to extract */
    int mask = 0;
    if (IS_INTEGER(args[1])) {
        mask = (int)AS_INTEGER(args[1]);
    } else if (IS_STRING(args[1])) {
        /* Legacy string labels */
        const char* e = AS_CSTRING(args[1]);
        if      (strcasecmp(e,"scheme")==0   || strcasecmp(e,"protocole")==0) mask = URL_PROTOCOLE;
        else if (strcasecmp(e,"host")==0     || strcasecmp(e,"domaine")==0)   mask = URL_DOMAINE;
        else if (strcasecmp(e,"port")==0)                                      mask = URL_PORT;
        else if (strcasecmp(e,"path")==0)    mask = URL_CHEMIN_RESSOURCE | URL_NOM_RESSOURCE | URL_EXTENSION;
        else if (strcasecmp(e,"query")==0    || strcasecmp(e,"requete")==0)   mask = URL_REQUETE;
        else if (strcasecmp(e,"fragment")==0 || strcasecmp(e,"ancre")==0)     mask = URL_FRAGMENT;
        else if (strcasecmp(e,"user")==0     || strcasecmp(e,"utilisateur")==0) mask = URL_UTILISATEUR;
        else if (strcasecmp(e,"password")==0 || strcasecmp(e,"motdepasse")==0) mask = URL_MOT_DE_PASSE;
    }

    /* Build result by concatenating requested portions in URL order */
    char result[2048] = "";
    int rlen = 0;
#define APPEND(str) do { int l=(int)strlen(str); if(rlen+l<2047){memcpy(result+rlen,str,l);rlen+=l;result[rlen]='\0';} } while(0)
    if (mask & URL_PROTOCOLE) {
        APPEND(scheme);
        /* Add "://" separator only when other parts follow (building compound URL) */
        if (mask & (URL_UTILISATEUR | URL_MOT_DE_PASSE | URL_DOMAINE | URL_PORT |
                    URL_CHEMIN_RESSOURCE | URL_NOM_RESSOURCE | URL_EXTENSION |
                    URL_REQUETE | URL_FRAGMENT)) {
            APPEND("://");
        }
    }
    if (mask & URL_UTILISATEUR)     APPEND(user);
    if (mask & URL_MOT_DE_PASSE)    { if ((mask & URL_UTILISATEUR) && *password) APPEND(":"); APPEND(password); }
    if ((mask & (URL_UTILISATEUR | URL_MOT_DE_PASSE)) && *host && (mask & URL_DOMAINE)) APPEND("@");
    if (mask & URL_DOMAINE)         APPEND(host);
    if (mask & URL_PORT)            { if (*port) { if (mask & URL_DOMAINE) APPEND(":"); APPEND(port); } }
    if (mask & URL_CHEMIN_RESSOURCE) APPEND(dirpath);
    if (mask & URL_NOM_RESSOURCE)   APPEND(resname);
    if (mask & URL_EXTENSION)       APPEND(ext);
    if (mask & URL_REQUETE)         APPEND(query);
    if (mask & URL_FRAGMENT)        APPEND(fragment);
#undef APPEND

    return OBJ_VAL(obj_copy_string(result, rlen));
}

/* __BufferVersChaine — coerce implicite vers chaîne à l'assignement typé
 *   `s est une chaîne = <expr>` / `s est une chaîne? = <expr>`
 * Le nom vient du seul cas d'origine (Buffer → chaîne) ; la native est
 * devenue le coerce chaîne générique, injecté par le compilateur pour toute
 * cible `chaîne`, nullable ou non.
 *
 * - Buffer → chaîne : conformité au canon (cast type 28
 *   (buffer) → type 16 (chaîne) à l'assignement). OWL étant UTF-8 natif, on
 *   copie les bytes du buffer dans un ObjString (pas de transcodage).
 * - chaîne → chaîne : passe-through.
 * - Null → Null : préservé (c'est ce qui rend la native sûre sur une cible
 *   nullable, cf. ).
 * - tout le reste : représentation textuelle canon via value_to_string. */
static Value fn_buffer_vers_chaine_coerce(int argCount, Value* args) {
    (void)argCount;
    /*  +  — un widget DSL nu (ObjUiControlRef) ou une cellule
     * ZR headless materialisee (ObjUiControl) assigne a un slot chaine se
     * deballe sur sa prop par defaut. Symetrique de fn_entier_coerce /
     * fn_reel_coerce qui deballent deja au meme chokepoint. */
    Value v = owl_instance_default_unwrap(args[0]);
    if (IS_STRING(v)) return v;
    if (IS_NIL(v)) return v;  /* préserve Null pour le ??* coalesce */
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        return OBJ_VAL(obj_copy_string((const char*)b->bytes, b->length));
    }
    /* Canon : `s est une chaîne = <expr>` caste TOUT vers chaîne via la
     * représentation textuelle (entier → "42", réel → "3.14", bool → "1"/"0",
     * Date → "AAAAMMJJ", DateHeure → "AAAAMMJJHHMMSSFFF", etc.).
     * value_to_string délivre la repr canon pour chaque type. */
    ObjString* s = value_to_string(v);
    return s ? OBJ_VAL(s) : v;
}

/* __ChaineVersBuffer — coerce implicite à l'assignement typé
 *   `b est un Buffer = <Chaine>`
 * Symétrique de __BufferVersChaine. Le canon caste implicitement string →
 * Buffer à l'assignement (`b est un Buffer = "hello"`
 * produit un Buffer de bytes, pas une string stockée dans un slot Buffer).
 *
 * Comportement OWL :
 * - String → Buffer : alloue un nouvel ObjBuffer et copie les bytes UTF-8
 *   (pas de transcodage UTF-16, divergence Taille assumée vs canon — voir
 *   VISION.md §5bis).
 * - Buffer → Buffer : passe-through (déjà du bon type).
 * - Null → Null : préservé pour le coalesce ??*.
 * - Autres types (int, réel, bool, ...) : passe-through. canon ferait une
 *   sérialisation binaire (int32 LE / double LE) ; OWL refuse cette
 *   sémantique anti-portable (voir VISION §5bis "Buffer + primitive").
 *   Le passe-through laisse remonter l'erreur de type au site downstream
 *   plutôt qu'un cast silencieux divergent. */
static Value fn_chaine_vers_buffer_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_BUFFER(v)) return v;
    if (IS_NIL(v)) return v;
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        return OBJ_VAL(obj_new_buffer(s->chars, s->length));
    }
    return v;
}

/* ================================================================
 * Coerce implicite pour les types numériques + booléen à l'assignement
 * typé : `n est un entier = <expr>`, `r est un réel = <expr>`, etc.
 *
 * Symétrique de __DateCoerce/__BufferVersChaine, comble le trou typing.
 * Canon : conversion implicite vers le type déclaré.
 *
 *   entier  ← réel        : TRUNCATE (3.7 → 3, -3.7 → -3) — convention
 *                           C/C#/Java/Rust/Go/canon classique.
 *   entier  ← chaîne      : strtoll si parseable, sinon 0 silencieux
 *                           (canon : pas d'erreur).
 *   entier  ← booléen     : Vrai → 1, Faux → 0
 *   entier  ← Null        : 0
 *   réel    ← entier/bool : promote (5 → 5.0, Vrai → 1.0)
 *   réel    ← chaîne      : strtod si parseable, sinon 0.0 silencieux
 *   booléen ← entier/réel : !=0
 *   booléen ← chaîne      : non-vide → Vrai
 *   passthrough sinon (déjà du bon type, ou type non-supporté).
 * ================================================================ */

/* Promotion d'un mur de coercion (magnitude hors borne, valeur non finie)
 * en exception runtime rattrapable par QUAND EXCEPTION. Pose throwPending,
 * repris par le dispatch VM dès le retour de la native. Cf. vm.c. */
extern void vm_throw_from_erreur(const char* msg, int code);

/* Lève un mur de coercion via le registry bilingue (owl_errors.def). Le
 * message est rendu dans la langue active (FR par défaut, EN sous --lang en)
 * puis copié par capture_exception_context — le buffer thread-local du
 * render peut être réutilisé sans risque après l'appel. */
static void coerce_wall_throw(OwlErrorCode code) {
    const char* msg = owl_error_render_lang_is_en()
        ? owl_error_render_en(code, NULL)
        : owl_error_render_fr(code, NULL);
    vm_throw_from_erreur(msg, 0);
}

static Value fn_entier_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    /*  — Une instance composite avec defaultProperty (Couleur ...)
     * se deballe sur son field par defaut quand assignee a un slot scalaire. */
    v = owl_instance_default_unwrap(v);
    /*  — Trou de typage : `n est un entier = TypeEnum.cBlanc` stockait
     * l'ObjInstance enum dans le slot entier, violant la promesse "tout est
     * typé, pas de Variant". Option A : extraire le field `Valeur` quand le
     * RHS est une instance d'enum user-defined (ObjClass kind ENUM enregistree
     * en global). Les enums OWL garantissent un `Valeur` entier (cf.
     * __EnumVal vm.c). Combinaisons (CLASS_KIND_COMBINATION) explicitement
     * hors-scope du ticket : pas d'extraction de bitmask vers slot entier. */
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName != NULL) {
            Value classVal;
            if (table_get(&vm.globals, inst->structName, &classVal)
                && IS_CLASS(classVal)
                && AS_CLASS(classVal)->kind == CLASS_KIND_ENUM) {
                ObjString* valKey = obj_copy_string("Valeur", 6);
                Value extracted;
                if (table_get(&inst->fields, valKey, &extracted)
                    && IS_INTEGER(extracted)) {
                    return extracted;
                }
            }
        }
    }
    if (IS_INTEGER(v)) return v;
    if (IS_REAL(v)) {
        double d = AS_REAL(v);
        /* Mur : Infini/NaN n'a aucune image entière. `(int64_t)d` serait UB. */
        if (!isfinite(d)) {
            coerce_wall_throw(E_RT_REAL_TO_INT_NON_FINITE);
            return NIL_VAL;
        }
        /* Mur : magnitude hors [INT64_MIN, INT64_MAX]. `(int64_t)d` serait UB.
         * La coercion souple ne perd que les décimales, pas la magnitude. */
        if (d >= 9223372036854775808.0 || d < -9223372036854775808.0) {
            coerce_wall_throw(E_RT_REAL_TO_INT_OVERFLOW);
            return NIL_VAL;
        }
        return INTEGER_VAL((int64_t)d);  /* troncature vers zéro */
    }
    if (IS_NUMERIC(v)) {
        /* Coercion souple : troncature vers zéro (perte des décimales, OK).
         * Mur si la magnitude ne rentre pas dans int64 (exception, pas
         * troncature silencieuse aux 64 bits bas). */
        bool overflow = false;
        int64_t r = numeric_to_i64(v, &overflow);
        if (overflow) {
            coerce_wall_throw(E_RT_NUMERIC_TO_INT_OVERFLOW);
            return NIL_VAL;
        }
        return INTEGER_VAL(r);
    }
    if (IS_BOOL(v))    return INTEGER_VAL(AS_BOOL(v) ? 1 : 0);
    if (IS_NIL(v))     return INTEGER_VAL(0);
    if (IS_CURRENCY(v))return INTEGER_VAL((int64_t)(AS_CURRENCY(v) / 1000000));
    /* Cast Duree -> entier retourne les millisecondes (canon).
     * Symetrie avec `var est une duree = <entier>` qui
     * interprete deja l'entier en ms. Cf . */
    if (IS_DURATION(v)) return INTEGER_VAL(AS_DURATION(v)->totalMs);
    /* Cast Date -> entier : jours 1-based depuis le 01/01/1800 (canon
     * DateVersEntier). JDN inline (date_to_jdn est static
     * dans stdlib_date.c, formule Gregorienne standard). Cf. . */
    if (IS_DATE(v)) {
        ObjDate* d = AS_DATE(v);
        int y = d->year, m = d->month, dd = d->day;
        int a = (14 - m) / 12;
        int yy = y + 4800 - a;
        int mm = m + 12 * a - 3;
        int64_t jdn = dd + (153 * mm + 2) / 5 + 365 * yy + yy / 4
                        - yy / 100 + yy / 400 - 32045;
        /* epoch 01/01/1800 calcule avec la meme formule (= 2378497). */
        int64_t epoch_1800 = 2378497LL;
        return INTEGER_VAL(jdn - epoch_1800 + 1);
    }
    /* Cast Heure -> entier : centiemes de seconde depuis 00:00:00:00
     * (canon HeureVersEntier). Cf. . */
    if (IS_TIME(v)) {
        ObjTime* t = AS_TIME(v);
        int64_t cs = (int64_t)t->hour * 360000
                   + (int64_t)t->minute * 6000
                   + (int64_t)t->second * 100
                   + (int64_t)(t->ms / 10);
        return INTEGER_VAL(cs);
    }
    /* Cast DateHeure -> entier : pas de canon univoque. Le compile-time
     * check (compiler.c) rejette les cas statiques. Pour le path dynamique
     * (RHS non resoluble statiquement), on signale via ErreurDetectee et
     * on retourne 0. Cf. . */
    if (IS_DATETIME(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EntierCoerce",
                              "native", "EntierCoerce",
                              "arg", "valeur",
                              "expected", "Date|Heure|entier|reel|booleen|chaine",
                              "actual", "DateHeure",
                              NULL);
        return INTEGER_VAL(0);
    }
    if (IS_STRING(v)) {
        /* Parsing atoi-style du préfixe numérique valide (espaces de tête +
         * signe + chiffres), aligné sur Val(). On tronque au 1er non-chiffre :
         * "42test"->42, "3.99"->3, "  -3.9x"->-3, "1e3"->1 (base 10 s'arrête
         * sur 'e', pas de scientifique en entier), "0x1F"->0 (pas d'hexa).
         * Préfixe non numérique ou chaîne vide -> 0, silencieux (canon). */
        const char* s = AS_CSTRING(v);
        char* end;
        long long n = strtoll(s, &end, 10);
        if (end != s) return INTEGER_VAL((int64_t)n);
        return INTEGER_VAL(0);
    }
    return v;  /* passthrough non-géré */
}

static Value fn_reel_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    v = owl_instance_default_unwrap(v);  /*  */
    if (IS_REAL(v))    return v;
    if (IS_INTEGER(v)) return REAL_VAL((double)AS_INTEGER(v));
    if (IS_BOOL(v))    return REAL_VAL(AS_BOOL(v) ? 1.0 : 0.0);
    if (IS_NIL(v))     return REAL_VAL(0.0);
    if (IS_CURRENCY(v))return REAL_VAL((double)AS_CURRENCY(v) / 1000000.0);
    if (IS_STRING(v)) {
        /* Parsing du préfixe flottant valide (espaces + signe + décimale +
         * exposant), aligné sur Val(). On ignore la suite non numérique :
         * "42test"->42, "3.14xyz"->3.14, "-2.5e2"->-250. Préfixe non
         * numérique ou chaîne vide -> 0.0, silencieux (canon). */
        const char* s = AS_CSTRING(v);
        char* end;
        double d = strtod(s, &end);
        if (end != s) return REAL_VAL(d);
        return REAL_VAL(0.0);
    }
    return v;
}

static Value fn_booleen_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_BOOL(v))    return v;
    if (IS_INTEGER(v)) return BOOL_VAL(AS_INTEGER(v) != 0);
    if (IS_REAL(v))    return BOOL_VAL(AS_REAL(v) != 0.0);
    if (IS_NIL(v))     return BOOL_VAL(false);
    if (IS_CURRENCY(v))return BOOL_VAL(AS_CURRENCY(v) != 0);
    if (IS_STRING(v))  return BOOL_VAL(AS_STRING(v)->length != 0);
    return v;
}

/*  : helpers de cast double→entier saturant (anti-UB).
 *
 * Cast (int64_t)(double) ou (__int128)(double) est UB en C si la valeur
 * sort de la plage representable. zig-clang Debug trap dessus (UBSan),
 * Release produit un garbage silencieux qui pollue les Value stockees
 * et fait crasher la suite de tests plus tard a un point variable
 * (la valeur garbage est decode comme un Obj* invalide via NaN-boxing,
 * etc.). On sature a [INT64_MIN, INT64_MAX] / [INT128_MIN, INT128_MAX] ;
 * NaN -> 0 (canon strtod). */
static inline int64_t double_to_int64_saturate(double d) {
    if (d != d) return 0;                       /* NaN */
    if (d >=  9223372036854775808.0) return INT64_MAX;
    if (d <  -9223372036854775808.0) return INT64_MIN;
    return (int64_t)(d + (d >= 0 ? 0.5 : -0.5));
}

/* __MonetaireCoerce — `mo est un monétaire = <expr>` ; cast canon.
 * Stockage interne CURRENCY_VAL = __int128 × 10^6 (6 décimales fixes). */
static Value fn_monetaire_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_CURRENCY(v)) return v;
    if (IS_INTEGER(v))  return CURRENCY_VAL((__int128)AS_INTEGER(v) * 1000000);
    if (IS_REAL(v)) {
        double d = AS_REAL(v);
        return CURRENCY_VAL(double_to_int64_saturate(d * 1000000.0));
    }
    if (IS_NUMERIC(v)) {
        /* Coercion souple : arrondi demi-pair à 6 décimales (perte des
         * décimales au-delà, OK). Mur si la magnitude dépasse la capacité
         * monétaire (exception, pas troncature silencieuse). */
        bool overflow = false;
        __int128 scaled = numeric_to_currency_i128(v, &overflow);
        if (overflow) {
            coerce_wall_throw(E_RT_NUMERIC_TO_CURRENCY_OVERFLOW);
            return NIL_VAL;
        }
        return CURRENCY_VAL(scaled);
    }
    if (IS_BOOL(v))     return CURRENCY_VAL(AS_BOOL(v) ? 1000000 : 0);
    if (IS_NIL(v))      return CURRENCY_VAL(0);
    if (IS_STRING(v)) {
        const char* s = AS_CSTRING(v);
        char* end;
        double d = strtod(s, &end);
        if (end != s && *end == '\0') {
            return CURRENCY_VAL(double_to_int64_saturate(d * 1000000.0));
        }
        return CURRENCY_VAL(0);  /* parse fail → 0 silencieux (canon) */
    }
    return v;
}

/*  : __EnumCoerce(value, enumName) — coerce vers une constante
 * d'un enum registry pour les variables `var est un EnumXxx = <expr>`.
 *
 * Strategie :
 *   - IS_NIL          → premiere constante de l'enum (init defaut)
 *   - IS_INTEGER      → passthrough si valeur dans la plage sentinel de
 *                       l'enum cible, sinon NIL (le compile-time aurait du
 *                       refuser les literals ; le runtime traite les
 *                       expressions dynamiques)
 *   - IS_STRING       → resolution par nom canonical via
 *                       owl_enum_contains ; renvoie le sentinel int de la
 *                       constante matchee, sinon NIL
 *   - autre           → passthrough (laisse runtime gerer l'incoherence)
 *
 * Pas d'erreur fatale runtime : si la coerce echoue, on retourne NIL et
 * l'utilisateur voit Null dans la variable. Compile-time est la barriere
 * stricte pour les literals (le compilateur emet une erreur claire avant
 * meme d'arriver ici sur `var = 42`, `var = "GET"`, `var = sensXxx`). */
static Value fn_enum_coerce(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    Value v = args[0];
    if (!IS_STRING(args[1])) return v;  /* defensif : nom enum manquant */
    const char* enumName = AS_CSTRING(args[1]);
    const OwlEnum* e = owl_registry_enum_find(enumName);
    if (e == NULL) return v;

    /* IS_NIL : init defaut = premiere constante de l'enum. */
    if (IS_NIL(v)) {
        if (e->values && e->values[0]) {
            const OwlConstDef* cd = owl_registry_const_find(e->values[0]);
            if (cd != NULL) return cd->value;
        }
        return NIL_VAL;
    }

    /* IS_STRING : resolution par nom canonical. */
    if (IS_STRING(v)) {
        const char* s = AS_CSTRING(v);
        if (!owl_enum_contains_bilingual(e, s)) return NIL_VAL;
        const OwlConstDef* cd = owl_registry_const_find_bilingual(s);
        if (cd != NULL) return cd->value;
        return NIL_VAL;
    }

    /* IS_INTEGER : verifie via owl_enum_label_for_int (parcourt les
     * constantes de l'enum et matche par valeur). Si pas de match, NIL. */
    if (IS_INTEGER(v)) {
        const char* label = owl_enum_label_for_int(e, AS_INTEGER(v));
        if (label != NULL) return v;  /* valeur appartient bien a l'enum */
        return NIL_VAL;
    }

    /* Autre type : passthrough. */
    return v;
}

/* __NumeriqueCoerce — `n est un numérique = <expr>` ; cast canon.
 * Stockage = OBJ_NUMERIC bignum décimal (mpdecimal, précision arbitraire).
 * réel -> numérique passe par la forme décimale shortest round-trip (exacte) ;
 * chaîne -> parse exact mpdecimal ; inf/nan -> forme spéciale mpd (le mur de la
 * redescente est géré en aval, ticket B). */
static Value fn_numerique_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_NUMERIC(v))  return v;
    if (IS_INTEGER(v))  return numeric_from_i64(AS_INTEGER(v));
    if (IS_REAL(v))     return numeric_from_real(AS_REAL(v));  /* forme décimale exacte */
    if (IS_BOOL(v))     return numeric_from_i64(AS_BOOL(v) ? 1 : 0);
    if (IS_NIL(v))      return numeric_from_i64(0);
    if (IS_CURRENCY(v)) {
        char b[40]; currency_to_str(AS_CURRENCY(v), b, sizeof(b));
        return numeric_from_string(b);
    }
    if (IS_STRING(v)) {
        /* Parse exact : un littéral chaîne -> numérique conserve toute la
         * précision (plus de passage lossy par double). Invalide -> 0. */
        const char* s = AS_CSTRING(v);
        char* end;
        (void)strtod(s, &end);
        if (end != s && *end == '\0') return numeric_from_string(s);
        return numeric_from_i64(0);
    }
    return v;
}

/* MarkdownVersTexte / MarkdownVersHTML : implémentés dans stdlib_markdown.c
 * (wrapper md4c CommonMark + GFM). Voir stdlib_markdown_init(). */

/* ================================================================
 * REGISTRATION
 * ================================================================ */

/* ================================================================
 * BUFFER
 * ================================================================ */

/* __BufferNew() -- internal, creates empty buffer (used by compiler). */
static Value fn_buffer_new(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_buffer_empty(0));
}

/* ChaineVersBuffer(s [, encodage]) / StringToBuffer
 * Sans encodage (ou encUTF8) : octets UTF-8 bruts (retro-compatible).
 * Avec encodage : transcode les codepoints vers le codepage cible. */
static Value fn_chaine_vers_buffer(int argCount, Value* args) {
    int enc = codec_arg_encoding(argCount, args, 1);
    if (IS_BUFFER(args[0])) return args[0];  /* deja des octets */
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* s = AS_STRING(args[0]);

    /* Octets encodes : UTF-8 = octets bruts de la chaine (s reste roote en tant
     * qu'argument sur la pile), sinon transcodage malloc'd (owned, a liberer). */
    const unsigned char* enc_bytes;
    int encLen;
    unsigned char* owned = NULL;
    if (enc == (int)OWL_ENC_UTF8) {
        enc_bytes = (const unsigned char*)s->chars;
        encLen = s->length;
    } else {
        owned = owl_codec_encode(s->chars, s->length, enc, &encLen);
        enc_bytes = owned;
    }

    /* 3e arg optionnel : longueur fixe en octets. Plus court => padding de
     * zeros a droite ; egal => inchange ; plus long => exception (decision dev). */
    if (argCount >= 3 && IS_INTEGER(args[2])) {
        int64_t target = AS_INTEGER(args[2]);
        if (target < 0 || encLen > target) {
            free(owned);
            vm_throw_from_erreur(
                "ChaineVersBuffer : les octets encodes depassent la longueur demandee.", 0);
            return NIL_VAL;
        }
        ObjBuffer* b = obj_new_buffer(NULL, (int)target);
        if (target > 0 && b->bytes) {
            memset(b->bytes, 0, (size_t)target);
            if (encLen > 0) memcpy(b->bytes, enc_bytes, (size_t)encLen);
        }
        free(owned);
        return OBJ_VAL(b);
    }

    Value r = OBJ_VAL(obj_new_buffer(enc_bytes, encLen));
    free(owned);
    return r;
}

/* Tronque(source, n [, type]).
 * type = nombreDeCaractèresAConserver (default, false/0): keep n chars from left.
 * type = nombreDeCaractèresASupprimer (true/1): drop n chars from the right.
 * If n < 0: returns EOT (Caract(4)) and sets owl_set_error (non-fatal).
 * Source can be a string or a buffer (byte-level for buffers).
 *
 * Note: methods on integer/real dispatch to fn_numerique_tronque (different
 * sema). The dispatch happens at registration time. */
extern Value fn_numerique_tronque(int argCount, Value* args);
Value fn_numerique_tronque(int argCount, Value* args);  /* fwd from stdlib_math.c */
static Value fn_tronque(int argCount, Value* args) {
    /* Numeric dispatch: n.Tronque() and Tronque(n) → truncation toward zero. */
    if (IS_INTEGER(args[0]) || IS_REAL(args[0])) {
        return fn_numerique_tronque(argCount, args);
    }
    /*  garde : forme chaîne/buffer = 2 args minimum (source, nombre). */
    if (argCount < 2) return args[0];
    /* String/buffer truncation per canon */
    int64_t n = 0;
    if (IS_INTEGER(args[1])) n = AS_INTEGER(args[1]);
    else if (IS_REAL(args[1])) n = (int64_t)AS_REAL(args[1]);
    int dropMode = 0;
    if (argCount >= 3) {
        if (IS_BOOL(args[2])) dropMode = AS_BOOL(args[2]) ? 1 : 0;
        else if (IS_INTEGER(args[2])) {
            /* Compare a la constante nommee (sentinel) — pas de !=0 brut depuis
             * la migration sentinel : les deux valeurs sont non-nulles. */
            dropMode = (AS_INTEGER(args[2]) == OWL_TRONQUE_SUPPRIMER) ? 1 : 0;
        }
    }
    if (n < 0) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "Tronque",
                              "native", "Tronque",
                              "arg", "n",
                              NULL);
        return OBJ_VAL(obj_copy_string("\x04", 1));  /* EOT */
    }
    /* Buffer path */
    if (IS_BUFFER(args[0])) {
        ObjBuffer* b = AS_BUFFER(args[0]);
        int keep = dropMode ? (b->length - (int)n) : (int)n;
        if (keep < 0) keep = 0;
        if (keep > b->length) keep = b->length;
        return OBJ_VAL(obj_new_buffer((const char*)b->bytes, keep));
    }
    /* String path */
    ObjString* s = to_string(args[0]);
    if (s == NULL) return NIL_VAL;
    int keep = dropMode ? (s->length - (int)n) : (int)n;
    if (keep < 0) keep = 0;
    if (keep > s->length) keep = s->length;
    return OBJ_VAL(obj_copy_string(s->chars, keep));
}

/* BufferVersChaine(b [, encodage]) / BufferToString
 * Sans encodage (ou encUTF8) : octets interpretes comme UTF-8 (retro-
 * compatible). Avec encodage : decode selon le codepage vers l'UTF-8. */
static Value fn_buffer_vers_chaine(int argCount, Value* args) {
    int enc = codec_arg_encoding(argCount, args, 1);
    if (IS_STRING(args[0])) return args[0];  /* deja une chaine */
    if (!IS_BUFFER(args[0])) return NIL_VAL;
    ObjBuffer* b = AS_BUFFER(args[0]);

    /* Forme 4-arg : decode une tranche [position, longueur] (base 1) ; les
     * formes 1/2-arg decodent la totalite (retro-compatible). */
    const unsigned char* src = b->bytes;
    int srcLen = b->length;
    if (argCount >= 4) {
        int64_t position = IS_INTEGER(args[2]) ? AS_INTEGER(args[2]) : 0;
        int64_t longueur = IS_INTEGER(args[3]) ? AS_INTEGER(args[3]) : -1;
        if (position < 1 || longueur < 0 ||
            position - 1 + longueur > (int64_t)b->length) {
            vm_throw_from_erreur(
                "BufferVersChaine : tranche hors des limites du buffer.", 0);
            return NIL_VAL;
        }
        src    = b->bytes + (position - 1);
        srcLen = (int)longueur;
    }

    if (enc == (int)OWL_ENC_UTF8) {
        return OBJ_VAL(obj_copy_string((const char*)src, srcLen));
    }
    int outLen = 0;
    unsigned char* out = owl_codec_decode(src, srcLen, enc, &outLen);
    Value r = OBJ_VAL(obj_copy_string((const char*)out, outLen));
    free(out);
    return r;
}

/* ----------------------------------------------------------------
 * Helpers internes (préfixe __) pour le statement POUR TOUTE CHAÎNE /
 * POUR TOUTE POSITION. Le compiler émet des OP_CALL vers ces natives
 * pour pré-calculer segments + positions, puis itère le tableau
 * résultant comme un POUR TOUT classique. Pas exposés au catalog
 * LSP/doc utilisateur — usage compiler-only.
 * ---------------------------------------------------------------- */

/* __pourtoutChaineSegments(src, seps) → tableau 2D où chaque élément
 * est [sous_chaîne, position_1based_codepoint]. Le tableau est vide
 * si src est vide (POUR TOUTE CHAÎNE itère zéro fois sur chaîne vide).
 * Si seps est vide (chaîne "" ou tableau []) → ErreurDéclenche soft
 * et retourne un tableau vide pour que la boucle ne s'exécute pas. */
static Value fn_pourtout_chaine_segments(int argCount, Value* args) {
    (void)argCount;
    ObjArray* outer = obj_new_array();
    vm_push(OBJ_VAL(outer));

    ObjString* src = to_string(args[0]);
    if (src == NULL || src->length == 0) {
        vm_pop();
        return OBJ_VAL(outer);
    }

    /* Extract séparateurs : chaîne unique ou tableau de chaînes */
    const char* seps[16];
    int sepLens[16];
    int sepCount = extract_separators(args[1], seps, sepLens, 16);
    if (sepCount <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "POUR TOUTE CHAINE",
                              "native", "POUR TOUTE CHAINE",
                              NULL);
        vm_pop();
        return OBJ_VAL(outer);
    }
    /* Vérifier qu'au moins un séparateur a une longueur > 0 */
    int validSeps = 0;
    for (int i = 0; i < sepCount; i++) {
        if (sepLens[i] > 0) validSeps++;
    }
    if (validSeps == 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "POUR TOUTE CHAINE",
                              "native", "POUR TOUTE CHAINE",
                              NULL);
        vm_pop();
        return OBJ_VAL(outer);
    }

    const char* p = src->chars;
    const char* end = src->chars + src->length;
    const char* segStart = p;
    while (p < end) {
        int matched = -1, matchedLen = 0;
        for (int k = 0; k < sepCount; k++) {
            int spl = sepLens[k];
            if (spl > 0 && p + spl <= end &&
                memcmp(p, seps[k], (size_t)spl) == 0) {
                matched = k; matchedLen = spl; break;
            }
        }
        if (matched >= 0) {
            /* Build pair [seg_str, pos_1based_cp] */
            ObjArray* pair = obj_new_array();
            vm_push(OBJ_VAL(pair));
            ObjString* part = obj_copy_string(segStart, (int)(p - segStart));
            int posCp = str_cp_at_byte(src, (int)(segStart - src->chars)) + 1;
            obj_array_push(pair, OBJ_VAL(part));
            value_array_write(&pair->elements, INTEGER_VAL(posCp));
            obj_array_push(outer, OBJ_VAL(pair));
            vm_pop(); /* pair */
            p += matchedLen;
            segStart = p;
        } else {
            p++;
        }
    }
    /* Dernier segment (peut être vide si src se termine par sep) */
    ObjArray* pair = obj_new_array();
    vm_push(OBJ_VAL(pair));
    ObjString* last = obj_copy_string(segStart, (int)(end - segStart));
    int posCp = str_cp_at_byte(src, (int)(segStart - src->chars)) + 1;
    obj_array_push(pair, OBJ_VAL(last));
    value_array_write(&pair->elements, INTEGER_VAL(posCp));
    obj_array_push(outer, OBJ_VAL(pair));
    vm_pop(); /* pair */

    vm_pop(); /* outer */
    return OBJ_VAL(outer);
}

/* __pourtoutChainePositions(src, motif [, opts]) → tableau d'entiers
 * (positions 1-based codepoint de chaque occurrence non-chevauchante
 * de motif dans src). opts = MotComplet | SansCasse | DepuisFin.
 * Motif vide → ErreurDéclenche soft et retourne tableau vide. */
static Value fn_pourtout_chaine_positions(int argCount, Value* args) {
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));

    ObjString* hay = to_string(args[0]);
    ObjString* needle = to_string(args[1]);
    int opts = (argCount >= 3) ? to_int(args[2]) : 0;

    if (hay == NULL || needle == NULL || needle->length == 0) {
        if (needle == NULL || needle->length == 0) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "POUR TOUTE POSITION",
                                  "native", "POUR TOUTE POSITION",
                                  NULL);
        }
        vm_pop();
        return OBJ_VAL(arr);
    }

    /* Collecter toutes les positions (1-based codepoint) en ordre
     * d'apparition (forward), poussees directement dans le tableau GC
     * (croissance illimitee, pas de plafond). Inverse en place si DepuisFin. */
    const char* p = hay->chars;
    int rem = hay->length;
    int mlen;
    int searchOpts = opts & ~SEARCH_DEPUIS_FIN;
    while (1) {
        const char* hit = find_with_opts(p, rem, needle->chars, needle->length,
                                         searchOpts, &mlen);
        if (!hit) break;
        int byteOff = (int)(hit - hay->chars);
        value_array_write(&arr->elements, INTEGER_VAL(str_cp_at_byte(hay, byteOff) + 1));
        int adv = (int)(hit - p) + mlen;
        p += adv; rem -= adv;
    }

    if (opts & SEARCH_DEPUIS_FIN) {
        int n = arr->elements.count;
        for (int i = 0; i < n / 2; i++) {
            Value tmp = arr->elements.values[i];
            arr->elements.values[i] = arr->elements.values[n - 1 - i];
            arr->elements.values[n - 1 - i] = tmp;
        }
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* SansEspace position constants : valeurs sentinelles centralisees dans
 * sentinels.h (Phase 2 anti-collision). */

static const char* const E_sansEspaceCote_values[] = {
    "sscDroite", "sscGauche", "sscExt\xc3\xa9rieur",  /* sscExtérieur */
    "sscInt\xc3\xa9rieur",                              /* sscIntérieur */
    "sscTout", NULL
};
static const OwlEnum E_sansEspaceCote = {
    .name="sansEspaceCote", .nameAscii="sansEspaceCote", .nameEN="sscSide", .category="Chaines",
    .descFR="Position des espaces à supprimer pour SansEspace.",
    .descEN="Position constants for SansEspace.",
    .values=E_sansEspaceCote_values, .combinable=false
};
/* Tronque(s, n, type) — type constants (canon).
 * Documented as booléens but treated here as int 0/1 for registry consistency. */
static const char* const E_tronqueType_values[] = {
    "nombreDeCaract\xc3\xa8resAConserver",   /* nombreDeCaractèresAConserver */
    "nombreDeCaract\xc3\xa8resASupprimer",   /* nombreDeCaractèresASupprimer */
    NULL
};
static const OwlEnum E_tronqueType = {
    .name="tronqueType", .nameAscii="tronqueType", .nameEN="truncateType", .category="Chaines",
    .descFR="Type de troncature pour Tronque (caractères à conserver vs à supprimer).",
    .descEN="Truncation type for Tronque (chars to keep vs chars to drop).",
    .values=E_tronqueType_values, .combinable=false
};

/* chaîneCompare enum — doc 3024011 */
static const char* const E_chaineCompare_values[] = {
    "ccNormal", "ccSansCasse", "ccSansAccent", "ccSansEspace",
    "ccSansEspaceInt\xc3\xa9" "rieur", "ccSansPonctuationNiEspace",
    "ccOrdreLexicographique", "ccRespecteNum\xc3\xa9rique", NULL
};
static const OwlEnum E_chaineCompare = {
    .name="cha\xc3\xae" "neCompare", .nameAscii="chaineCompare", .nameEN="stringCompare", .category="Chaines",
    .descFR="Options de comparaison pour ChaîneCompare (combinables avec +).",
    .descEN="Comparison options for StringCompare (combinable with +).",
    .values=E_chaineCompare_values, .combinable=true
};

/* FormatNumérique field enums */
static const char* const E_fnAlignement_values[] = { "fncDroite", "fncGauche", NULL };
const OwlEnum E_fnAlignement = {
    .name="formatNum\xc3\xa9" "riqueAlignement", .nameAscii="formatNumeriqueAlignement", .nameEN="numberFormatAlignment",
    .category="FormatNum\xc3\xa9" "rique",
    .descFR="Alignement texte/remplissage pour FormatNumérique.Alignement.",
    .descEN="Text alignment / fill positioning for NumberFormat.Alignment.",
    .values=E_fnAlignement_values, .combinable=false
};
static const char* const E_fnNotation_values[] = {
    "fnnBinaire", "fnnD\xc3\xa9" "cimal", "fnnExponentiel",
    "fnnHexad\xc3\xa9" "cimal", "fnnOctal", NULL
};
const OwlEnum E_fnNotation = {
    .name="formatNum\xc3\xa9" "riqueNotation", .nameAscii="formatNumeriqueNotation", .nameEN="numberFormatNotation",
    .category="FormatNum\xc3\xa9" "rique",
    .descFR="Notation de sortie pour FormatNumérique.Notation.",
    .descEN="Output notation for NumberFormat.Notation.",
    .values=E_fnNotation_values, .combinable=false
};
static const char* const E_fnPrefixe_values[] = { "fnpAuto", NULL };
const OwlEnum E_fnPrefixe = {
    .name="formatNum\xc3\xa9" "riquePr\xc3\xa9" "fixe", .nameAscii="formatNumeriquePrefixe", .nameEN="numberFormatPrefix",
    .category="FormatNum\xc3\xa9" "rique",
    .descFR="Préfixe automatique pour FormatNumérique.Préfixe (sinon chaîne libre).",
    .descEN="Auto prefix marker for NumberFormat.Prefix (or free string).",
    .values=E_fnPrefixe_values, .combinable=false
};
static const char* const E_fnSigne_values[] = {
    "fnsMoinsSiN\xc3\xa9" "gatif", "fnsParenth\xc3\xa8" "sesSiN\xc3\xa9" "gatif",
    "fnsToujours", NULL
};
const OwlEnum E_fnSigne = {
    .name="formatNum\xc3\xa9" "riqueSigne", .nameAscii="formatNumeriqueSigne", .nameEN="numberFormatSign",
    .category="FormatNum\xc3\xa9" "rique",
    .descFR="Mode d'affichage du signe pour FormatNumérique.Signe.",
    .descEN="Sign display mode for NumberFormat.Sign.",
    .values=E_fnSigne_values, .combinable=false
};

/* chaineFormateOptions — sur-ensemble de chaineCompare incluant les
 * modificateurs de chaîne ccMajuscule / ccMinuscule (qui n'ont pas de
 * sens pour ChaîneCompare mais sont consommés par ChaîneFormate et
 * ChaîneDécoupe). Doc : ces 10 ccXxx forment la palette canon.
 *
 * Séparé de E_chaineCompare pour autocomplete LSP sémantiquement juste :
 * ChaîneCompare propose 8 valeurs (pas ccMajuscule/ccMinuscule),
 * ChaîneFormate propose les 10. */
static const char* const E_chaineFormateOptions_values[] = {
    "ccNormal", "ccSansCasse", "ccSansAccent", "ccSansEspace",
    "ccSansEspaceInt\xc3\xa9" "rieur", "ccSansPonctuationNiEspace",
    "ccOrdreLexicographique", "ccRespecteNum\xc3\xa9rique",
    "ccMajuscule", "ccMinuscule", NULL
};
const OwlEnum E_chaineFormateOptions = {
    .name="cha\xc3\xae" "neFormateOptions",
    .nameAscii="chaineFormateOptions", .nameEN="stringFormatOptions", .category="Chaines",
    .descFR="Options combinables pour ChaîneFormate et ChaîneDécoupe (sur-ensemble de chaineCompare incluant ccMajuscule / ccMinuscule).",
    .descEN="Combinable options for StringFormat and StringSplit (superset of chaineCompare including ccMajuscule / ccMinuscule).",
    .values=E_chaineFormateOptions_values, .combinable=true
};

/* optionsRechercheChaîne — options combinables OR pour les recherches
 * d'occurrence dans une chaîne. Doc 1510010 (PositionOccurrence) /
 * 1510018 (POUR TOUTE POSITION).
 *
 * Valeurs (toutes en bit-flags OR-clean) :
 *   DepuisDébut = 0 (neutre, défaut)
 *   MotComplet  = 1
 *   SansCasse   = 2 (englobe accent-insensibilité par doc canon)
 *   DepuisFin   = 4
 *
 * Note : la valeur DepuisDébut=0 a été harmonisée avec ce groupe ; le
 * comportement de ChaîneIncremente reste identique (auto-detect quand
 * argument absent, mode driven par bit DepuisFin quand explicite). */
static const char* const E_optionsRechercheChaine_values[] = {
    "DepuisD\xc3\xa9" "but", "MotComplet", "SansCasse", "DepuisFin", NULL
};
const OwlEnum E_optionsRechercheChaine = {
    .name="optionsRechercheCha\xc3\xae" "ne",
    .nameAscii="optionsRechercheChaine", .nameEN="stringSearchOptions",
    .category="Chaines",
    .descFR="Options combinables pour les recherches dans une chaîne (PositionOccurrence, Position, ChaîneOccurrence, Remplace, POUR TOUTE POSITION) et pour ChaîneIncremente (DepuisDébut / DepuisFin).",
    .descEN="Combinable options for string searches (PositionOccurrence, Position, StringOccurrence, Replace, FOR EACH POSITION) and for StringIncrement (FromBeginning / FromEnd).",
    .values=E_optionsRechercheChaine_values, .combinable=true
};

/* urlEncodeMode — mode d'encodage pour le param 'format' de URLEncode.
 * Valeurs entieres positives 0..3 a semantique runtime (dispatch direct
 * dans fn_url_encode), d'ou force_small_int=true. Non combinable :
 * un seul mode a la fois. Constantes enregistrees plus bas
 * (ENCODE_URL_CONSTS). Doc 3024040. */
static const char* const E_urlEncodeMode_values[] = {
    "encodeURLDepuisUTF8", "encodeURLDepuisAnsi",
    "encodeURLDepuisUnicode", "encodeURLParam\xc3\xa8tre",  /* encodeURLParamètre */
    NULL
};
static const OwlEnum E_urlEncodeMode = {
    .name="urlEncodeMode", .nameAscii="urlEncodeMode", .nameEN="urlEncodeMode", .category="Chaines",
    .descFR="Mode d'encodage pour le paramètre format de URLEncode (UTF-8 par défaut, ou encodage strict des caractères réservés).",
    .descEN="Encoding mode for the format parameter of URLEncode (UTF-8 by default, or strict encoding of reserved characters).",
    .values=E_urlEncodeMode_values, .combinable=false, .force_small_int=true
};

/* base64Format — alphabet pour le param 'format' de ChaîneVersBase64 /
 * Base64VersChaîne. Valeurs 0..1 a semantique runtime (dispatch direct),
 * d'ou force_small_int=true. Non combinable. Constantes enregistrees plus
 * bas (ENCODE_BASE64_CONSTS). Doc 1000022258. */
static const char* const E_base64Format_values[] = {
    "encodeBASE64", "encodeBASE64URL", NULL
};
static const OwlEnum E_base64Format = {
    .name="base64Format", .nameAscii="base64Format", .nameEN="base64Format", .category="Encodage",
    .descFR="Alphabet base64 pour le paramètre format de ChaîneVersBase64 / Base64VersChaîne (standard par défaut, ou variante URL-safe sans caractère de remplissage).",
    .descEN="Base64 alphabet for the format parameter of StringToBase64 / Base64ToString (standard by default, or URL-safe variant without padding).",
    .values=E_base64Format_values, .combinable=false, .force_small_int=true
};

/* tailleUnité — unité d'affichage pour le param 'formatAffichage' de
 * TailleVersChaîne. Valeurs 1..5 a semantique runtime (unit-1 = index
 * de label dans fn_taille_vers_chaine), d'ou force_small_int=true. Non
 * combinable. Constantes enregistrees plus bas (TAILLE_CONSTS). Note :
 * l'unite auto (absence de param) n'est pas une constante nommee dans
 * le canon, donc absente de l'enum. Doc 1000017038. */
static const char* const E_tailleUnite_values[] = {
    "tailleO", "tailleKo", "tailleMo", "tailleGo", "tailleTO", NULL
};
static const OwlEnum E_tailleUnite = {
    .name="tailleUnit\xc3\xa9", .nameAscii="tailleUnite", .nameEN="sizeUnit", .category="Chaines",
    .descFR="Unité d'affichage forçant le résultat de TailleVersChaîne (octets, Ko, Mo, Go ou To).",
    .descEN="Display unit forcing the result of LengthToString (bytes, KB, MB, GB or TB).",
    .values=E_tailleUnite_values, .combinable=false, .force_small_int=true
};

/* owlEncodage — codepage cible/source pour ChaîneVersBuffer /
 * BufferVersChaîne. Frontière charset du modèle sandwich UTF-8 : le texte
 * reste abstrait, l'encodage est une propriété des octets. Valeurs
 * mutuellement exclusives (un seul encodage à la fois), zone sentinelle. */
static const char* const E_owlEncodage_values[] = {
    "encUTF8", "encUTF16LE", "encUTF16BE", "encLatin1", "encWindows1252",
    "encCP850", "encCP437", NULL
};
const OwlEnum E_owlEncodage = {
    .name="owlEncodage", .nameAscii="owlEncodage", .nameEN="owlEncoding", .category="Chaines",
    .descFR="Encodage des octets pour ChaîneVersBuffer / BufferVersChaîne (UTF-8 par défaut, UTF-16 LE/BE, Latin-1, Windows-1252, codepages DOS CP850/CP437).",
    .descEN="Byte encoding for StringToBuffer / BufferToString (UTF-8 by default, UTF-16 LE/BE, Latin-1, Windows-1252, DOS codepages CP850/CP437).",
    .values=E_owlEncodage_values, .combinable=false
};

static const OwlEnum* const STRING_ENUMS[] = {
    &E_sansEspaceCote, &E_tronqueType, &E_chaineCompare,
    &E_fnAlignement, &E_fnNotation, &E_fnPrefixe, &E_fnSigne,
    &E_optionsRechercheChaine, &E_chaineFormateOptions,
    &E_urlEncodeMode, &E_base64Format, &E_tailleUnite, &E_owlEncodage
};

#define C_SSE(nm, val, dfr, den, nen) {nm, "Chaines", INTEGER_VAL(val), dfr, den, nen}
static const OwlConstDef STRING_SSE_CONSTS[] = {
    C_SSE("sscDroite",    OWL_SSC_DROITE,    "supprime les espaces à droite", "trim trailing spaces", "sscRight"),
    C_SSE("sscGauche",    OWL_SSC_GAUCHE,    "supprime les espaces à gauche", "trim leading spaces", "sscLeft"),
    C_SSE("sscExt\xc3\xa9rieur", OWL_SSC_EXTERIEUR,
        "supprime les espaces à gauche et à droite (défaut)",
        "trim leading and trailing spaces (default)", "sscOutside"),
    C_SSE("sscInt\xc3\xa9rieur", OWL_SSC_INTERIEUR,
        "supprime tous les espaces à l'intérieur de la chaîne",
        "remove all internal spaces", "sscInside"),
    C_SSE("sscTout",      OWL_SSC_TOUT,      "supprime tous les espaces (gauche + droite + intérieur)",
        "remove every space (left + right + internal)", "sscAll"),
};
/* STRING_SSE_ALIASES (sscExterieur/sscInterieur) supprime : auto-genere par
 * register_consts via accent-strip des canon SSE_CONSTS. */

#define C_TRQ(nm, val, dfr, den, nen) {nm, "Chaines", INTEGER_VAL(val), dfr, den, nen}
static const OwlConstDef STRING_TRQ_CONSTS[] = {
    C_TRQ("nombreDeCaract\xc3\xa8resAConserver", OWL_TRONQUE_CONSERVER,
        "défaut : <Nombre> = caractères à conserver dans le résultat",
        "default: <N> = chars to keep in result", "numberOfCharactersToStore"),
    C_TRQ("nombreDeCaract\xc3\xa8resASupprimer", OWL_TRONQUE_SUPPRIMER,
        "<Nombre> = caractères à supprimer en queue de chaîne",
        "<N> = chars to drop from the right", "numberOfCharactersToDelete"),
};
/* STRING_TRQ_ALIASES supprime : nombreDeCaracteresAConserver/ASupprimer
 * sont auto-genere par register_consts (accent-strip de ères). */

/* ccSansCasse etc. — ChaîneCompare option flags (combinable). Doc 3024011. */
#define C_CC(nm, val, dfr, den, nen) {nm, "Chaines", INTEGER_VAL(val), dfr, den, nen}
static const OwlConstDef STRING_CC_CONSTS[] = {
    /* ccNormal : forme EN canon identique au FR (ascii_identical) — posée
     * quand même (re-set runtime bénin, satisfait la digue nameEN). */
    C_CC("ccNormal",              0,                          "comparaison normale (défaut), similaire à l'opérateur '='", "normal comparison (default), similar to '=' operator", "ccNormal"),
    C_CC("ccSansCasse",           CC_SANS_CASSE,              "compare sans tenir compte de la casse", "compare case-insensitive", "ccIgnoreCase"),
    C_CC("ccSansAccent",          CC_SANS_ACCENT,             "compare sans tenir compte des accents", "compare accent-insensitive", "ccIgnoreAccent"),
    C_CC("ccSansEspace",          CC_SANS_ESPACE,             "compare sans tenir compte des espaces avant/après les chaînes", "compare ignoring leading/trailing spaces", "ccIgnoreSpace"),
    C_CC("ccSansEspaceInt\xc3\xa9rieur", CC_SANS_ESPACE_INT,  "compare sans tenir compte des espaces intérieurs", "compare ignoring internal spaces", "ccIgnoreInsideSpace"),
    C_CC("ccSansPonctuationNiEspace", CC_SANS_PONCTUATION,    "compare sans tenir compte de la ponctuation ni des espaces", "compare ignoring punctuation and spaces", "ccIgnorePunctuationAndSpace"),
    C_CC("ccOrdreLexicographique", CC_ORDRE_LEXICOGRAPHIQUE,  "comparaison lexicographique ('é' entre 'e' et 'f')", "lexicographic comparison", "ccLexicographicOrder"),
    C_CC("ccRespecteNum\xc3\xa9rique", CC_RESPECTE_NUMERIQUE, "comparaison tenant compte des numériques (\"10\" > \"9\")", "numeric-aware comparison", "ccRespectNumeric"),
    C_CC("ccMajuscule",           CC_MAJUSCULE,               "ChaîneFormate : passe la chaîne en majuscules", "StringFormat: uppercase", "ccUpCase"),
    C_CC("ccMinuscule",           CC_MINUSCULE,               "ChaîneFormate : passe la chaîne en minuscules", "StringFormat: lowercase", "ccLowCase"),
};
/* STRING_CC_ALIASES supprime : ccSansEspaceInterieur / ccRespecteNumerique
 * sont auto-generes par register_consts. */

/* DepuisFin / DepuisDébut — options combinables pour les recherches dans
 * une chaîne. Utilisées par PositionOccurrence, Position, ChaîneOccurrence,
 * ChaîneIncremente, Remplace et le statement POUR TOUTE POSITION ...
 * AVEC ... (doc 1510010 / 1510018).
 *
 * DepuisFin (4) = bit-flag combinable avec MotComplet(1) et SansCasse(2).
 * DepuisDébut (0) = valeur neutre (défaut), combine sans effet.
 * champFin/champDébut conservés en alias compat. */
#define C_CHAMP(nm, val, dfr, den, nen) {nm, "Chaines", INTEGER_VAL(val), dfr, den, nen}
static const OwlConstDef STRING_CHAMP_CONSTS[] = {
    C_CHAMP("DepuisFin",   SEARCH_DEPUIS_FIN,
        "recherche depuis la fin (bit combinable avec MotComplet/SansCasse)",
        "from-end search (combinable bit)", "FromEnd"),
    C_CHAMP("DepuisD\xc3\xa9" "but", 0,
        "recherche depuis le début (défaut, valeur neutre dans le masque combinable)",
        "from-start search (default, neutral value in combinable mask)", "FromBeginning"),
};

/* maskNombreSystème / maskMonétaireSystème — sentinels système pour
 * NumériqueVersChaîne (doc 3024024). */
#define C_MASK(nm, val, dfr, den, nen) {nm, "Chaines", INTEGER_VAL(val), dfr, den, nen}
static const OwlConstDef STRING_MASK_CONSTS[] = {
    C_MASK("maskNombreSyst\xc3\xa8me",     OWL_MASK_NOMBRE_SYSTEME,
           "Masque numérique selon les paramètres locale du système (séparateurs, décimales).",
           "System numeric mask (uses locale separators and decimals).", "maskSystemNumeral"),
    C_MASK("maskMon\xc3\xa9taireSyst\xc3\xa8me", OWL_MASK_MONETAIRE_SYSTEME,
           "Masque monétaire selon les paramètres locale du système (préfixe devise + séparateurs).",
           "System monetary mask (currency prefix + locale separators).", "maskSystemCurrency"),
};
/* STRING_MASK_ALIASES supprime : maskNombreSysteme / maskMonetaireSysteme
 * sont auto-generes par register_consts. */

/* fnc* / fnn* / fnp* / fns* — propriétés de FormatNumérique. */
#define C_FN(nm, val, dfr, den, nen) {nm, "FormatNum\xc3\xa9" "rique", INTEGER_VAL(val), dfr, den, nen}
static const OwlConstDef STRING_FN_CONSTS[] = {
    /* Alignement */
    C_FN("fncDroite", OWL_FNC_DROITE, "Justification à droite (défaut)", "Right alignment (default)", "nfaRight"),
    C_FN("fncGauche", OWL_FNC_GAUCHE, "Justification à gauche", "Left alignment", "nfaLeft"),
    /* Notation */
    C_FN("fnnBinaire",      OWL_FNN_BINAIRE,      "Notation binaire", "Binary notation", "nfnBinary"),
    C_FN("fnnD\xc3\xa9" "cimal",      OWL_FNN_DECIMAL,      "Notation décimale (défaut)", "Decimal notation (default)", "nfnDecimal"),
    C_FN("fnnExponentiel",  OWL_FNN_EXPONENTIEL,  "Notation exponentielle", "Exponential notation", "nfnExponential"),
    C_FN("fnnHexad\xc3\xa9" "cimal", OWL_FNN_HEXADECIMAL, "Notation hexadécimale", "Hexadecimal notation", "nfnHexadecimal"),
    C_FN("fnnOctal",        OWL_FNN_OCTAL,        "Notation octale", "Octal notation", "nfnOctal"),
    /* Préfixe */
    C_FN("fnpAuto",         OWL_FNP_AUTO,         "Préfixe automatique selon notation (0x, 0b, ...)", "Auto prefix per notation", "nfpAuto"),
    /* Signe */
    C_FN("fnsMoinsSiN\xc3\xa9" "gatif", OWL_FNS_MOINS_SI_NEGATIF,
         "Signe '-' affiché uniquement si négatif (défaut)", "Show '-' only on negatives (default)", "nfsMinusIfNegative"),
    C_FN("fnsParenth\xc3\xa8" "sesSiN\xc3\xa9" "gatif", OWL_FNS_PARENTHESES_SI_NEGATIF,
         "Négatifs entre parenthèses style comptable", "Negatives wrapped in parentheses (accounting)", "nfsParenthesesIfNegative"),
    C_FN("fnsToujours",     OWL_FNS_TOUJOURS,     "Signe toujours affiché ('+' ou '-')", "Always show sign", "nfsAlways"),
};
/* STRING_FN_ALIASES supprime : fnnDecimal/fnnHexadecimal/fnsMoinsSiNegatif/
 * fnsParenthesesSiNegatif sont auto-generes par register_consts. */
/* champFin / champDébut / champDebut sont des alias COMPAT (pas des
 * strips d'accents) → enregistres via owl_registry_register_ascii_alias
 * dans stdlib_string_init. */

void stdlib_string_init(void) {
    /* Internal: __BufferNew for `buf est un buffer` declarations */
    owl_define_native("__BufferNew", fn_buffer_new, 0);
    /* Internal: coerce Buffer → String à l'assignement `est une chaîne = <Buffer>` */
    owl_define_native("__BufferVersChaine", fn_buffer_vers_chaine_coerce, 1);
    /* Internal: coerce String → Buffer à l'assignement `est un Buffer = <Chaine>` */
    owl_define_native("__ChaineVersBuffer", fn_chaine_vers_buffer_coerce, 1);
    /* Internal: coerces vers types numériques + booléen pour les assignements
     * typés (`n est un entier = <expr>`, etc.). Comble le trou typing. */
    owl_define_native("__EntierCoerce",  fn_entier_coerce,  1);
    owl_define_native("__ReelCoerce",    fn_reel_coerce,    1);
    owl_define_native("__BooleenCoerce",   fn_booleen_coerce,   1);
    owl_define_native("__MonetaireCoerce", fn_monetaire_coerce, 1);
    owl_define_native("__NumeriqueCoerce", fn_numerique_coerce, 1);
    /* Internal: coerce vers une constante d'un enum registry pour les
     * variables `var est un EnumXxx = <expr>`. */
    owl_define_native("__EnumCoerce", fn_enum_coerce, 2);
    /* Internals: POUR TOUTE CHAÎNE / POUR TOUTE POSITION pre-iteration
     * helpers. Emitted by the compiler in foreach_statement (mode chaîne
     * et mode position). */
    owl_define_native("__pourtoutChaineSegments",  fn_pourtout_chaine_segments,  2);
    owl_define_native("__pourtoutChainePositions", fn_pourtout_chaine_positions, -1);

    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * pour les noms accentues. Les *_ALIASES[] tables qui contenaient les
     * versions ASCII des canons accentues (ccSansEspaceInterieur pour
     * ccSansEspaceIntérieur, etc.) sont devenues redondantes — supprimees. */
    owl_registry_register_enums(STRING_ENUMS,
        (int)(sizeof(STRING_ENUMS)/sizeof(STRING_ENUMS[0])));
    owl_registry_register_consts(STRING_SSE_CONSTS,
        (int)(sizeof(STRING_SSE_CONSTS)/sizeof(STRING_SSE_CONSTS[0])));
    owl_registry_register_consts(STRING_TRQ_CONSTS,
        (int)(sizeof(STRING_TRQ_CONSTS)/sizeof(STRING_TRQ_CONSTS[0])));
    owl_registry_register_consts(STRING_CHAMP_CONSTS,
        (int)(sizeof(STRING_CHAMP_CONSTS)/sizeof(STRING_CHAMP_CONSTS[0])));
    owl_registry_register_consts(STRING_CC_CONSTS,
        (int)(sizeof(STRING_CC_CONSTS)/sizeof(STRING_CC_CONSTS[0])));
    owl_registry_register_consts(STRING_MASK_CONSTS,
        (int)(sizeof(STRING_MASK_CONSTS)/sizeof(STRING_MASK_CONSTS[0])));
    owl_registry_register_consts(STRING_FN_CONSTS,
        (int)(sizeof(STRING_FN_CONSTS)/sizeof(STRING_FN_CONSTS[0])));
    /* Alias compat pour DepuisFin/DepuisDébut (anciens noms champFin/Début). */
    owl_registry_register_ascii_alias("DepuisFin",                "champFin");
    owl_registry_register_ascii_alias("DepuisD\xc3\xa9" "but",    "champD\xc3\xa9" "but");
    owl_registry_register_ascii_alias("DepuisD\xc3\xa9" "but",    "champDebut");
    /* url* constants (URLExtraitChemin portions) — doc 1000020965, combinables */
    {
        static const OwlConstDef URL_CONSTS[] = {
            {"urlProtocole",        "Chaines", INTEGER_VAL(URL_PROTOCOLE),       "protocole de l'URL (ex: \"http\")",    "URL protocol", "urlProtocol"},
            {"urlUtilisateur",      "Chaines", INTEGER_VAL(URL_UTILISATEUR),     "nom d'utilisateur dans l'URL",         "URL username", "urlUser"},
            {"urlMotDePasse",       "Chaines", INTEGER_VAL(URL_MOT_DE_PASSE),    "mot de passe dans l'URL",              "URL password", "urlPassword"},
            {"urlDomaine",          "Chaines", INTEGER_VAL(URL_DOMAINE),         "domaine de l'URL (ex: \"www.site.com\")", "URL domain", "urlDomain"},
            /* urlPort : forme EN canon identique au FR (re-set runtime bénin). */
            {"urlPort",             "Chaines", INTEGER_VAL(URL_PORT),            "port de l'URL (ex: \"80\")",           "URL port", "urlPort"},
            {"urlCheminRessource",  "Chaines", INTEGER_VAL(URL_CHEMIN_RESSOURCE),"chemin de la ressource (ex: \"/pages/\")", "resource path", "urlResourcePath"},
            {"urlNomRessource",     "Chaines", INTEGER_VAL(URL_NOM_RESSOURCE),   "nom de la ressource sans extension",   "resource name without extension", "urlResourceName"},
            {"urlExtensionRessource","Chaines",INTEGER_VAL(URL_EXTENSION),       "extension de la ressource (ex: \".htm\")", "resource extension", "urlResourceExtension"},
            {"urlRequ\xc3\xaate",   "Chaines", INTEGER_VAL(URL_REQUETE),         "requête de l'URL (après '?')",         "URL query string", "urlRequest"},
            /* urlFragment : forme EN canon identique au FR (re-set runtime bénin). */
            {"urlFragment",         "Chaines", INTEGER_VAL(URL_FRAGMENT),        "fragment de l'URL (après '#')",        "URL fragment/anchor", "urlFragment"},
        };
        /* register_consts auto-enregistre + auto-alias ASCII (urlRequete). */
        owl_registry_register_consts(URL_CONSTS,
            (int)(sizeof(URL_CONSTS)/sizeof(URL_CONSTS[0])));

        /* encodeURL* constants (URLEncode mode) — doc 3024040 */
        static const OwlConstDef ENCODE_URL_CONSTS[] = {
            {"encodeURLDepuisUTF8",    "Chaines", INTEGER_VAL(ENCODE_URL_DEPUIS_UTF8),
             "URLEncode : input UTF-8, préserve séparateurs URL (défaut).",
             "URLEncode: UTF-8 input, preserves URL separators (default).", "encodeURLFromUTF8"},
            {"encodeURLDepuisAnsi",    "Chaines", INTEGER_VAL(ENCODE_URL_DEPUIS_ANSI),
             "URLEncode : input ANSI (alias UTF-8 en OWL).",
             "URLEncode: ANSI input (alias UTF-8 in OWL).", "encodeURLFromAnsi"},
            {"encodeURLDepuisUnicode", "Chaines", INTEGER_VAL(ENCODE_URL_DEPUIS_UNICODE),
             "URLEncode : input Unicode (alias UTF-8 en OWL).",
             "URLEncode: Unicode input (alias UTF-8 in OWL).", "encodeURLFromUnicode"},
            {"encodeURLParam\xc3\xa8tre", "Chaines", INTEGER_VAL(ENCODE_URL_PARAMETRE),
             "URLEncode : encode TOUS les caractères réservés (pour passer une URL en paramètre).",
             "URLEncode: encode ALL reserved characters (for passing URL as parameter).", "encodeURLParameter"},
        };
        owl_registry_register_consts(ENCODE_URL_CONSTS,
            (int)(sizeof(ENCODE_URL_CONSTS)/sizeof(ENCODE_URL_CONSTS[0])));

        /* encodeBASE64* constants — doc 1000022258 (noms identiques FR/EN) */
        static const OwlConstDef ENCODE_BASE64_CONSTS[] = {
            {"encodeBASE64",    "Encodage", INTEGER_VAL(ENCODE_BASE64),
             "base64 standard : alphabet '+' et '/', complété par '=' (défaut).",
             "Standard base64: '+' and '/' alphabet, padded with '=' (default).", "encodeBASE64"},
            {"encodeBASE64URL", "Encodage", INTEGER_VAL(ENCODE_BASE64_URL),
             "base64 URL-safe : alphabet '-' et '_', sans caractère de remplissage.",
             "URL-safe base64: '-' and '_' alphabet, without padding.", "encodeBASE64URL"},
        };
        owl_registry_register_consts(ENCODE_BASE64_CONSTS,
            (int)(sizeof(ENCODE_BASE64_CONSTS)/sizeof(ENCODE_BASE64_CONSTS[0])));
    }
    /* MotComplet / SansCasse — search option flags for ChaîneSupprime, ChaîneOccurrence,
     * PositionOccurrence. Doc: identical constant names. */
    {
        static const OwlConstDef SEARCH_CONSTS[] = {
            {"MotComplet", "Chaines", INTEGER_VAL(SEARCH_MOT_COMPLET),
             "recherche le mot complet (entouré de séparateurs/ponctuation)",
             "search for complete word (surrounded by non-word chars)", "WholeWord"},
            {"SansCasse",  "Chaines", INTEGER_VAL(SEARCH_SANS_CASSE),
             "recherche sans tenir compte de la casse ni des accents",
             "case- and accent-insensitive search", "IgnoreCase"},
        };
        owl_registry_register_consts(SEARCH_CONSTS,
            (int)(sizeof(SEARCH_CONSTS)/sizeof(SEARCH_CONSTS[0])));
    }
    /* rangXxx — traversal mode constants for ExtraitChaîneEntre, PositionOccurrence */
    {
        static const OwlConstDef RANG_CONSTS[] = {
            {"rangPremier",   "Chaines", INTEGER_VAL(RANG_PREMIER),
             "début de parcours (depuis le début de la chaîne)", "begin traversal from start", "firstRank"},
            {"rangSuivant",   "Chaines", INTEGER_VAL(RANG_SUIVANT),
             "occurrence suivante dans le parcours", "next occurrence in traversal", "nextRank"},
            {"rangDernier",   "Chaines", INTEGER_VAL(RANG_DERNIER),
             "début de parcours inversé (depuis la fin)", "begin traversal from end", "lastRank"},
            {"rangPr\xc3\xa9" "c\xc3\xa9" "dent", "Chaines", INTEGER_VAL(RANG_PRECEDENT),
             "occurrence précédente dans le parcours inversé", "previous occurrence in reverse traversal", "previousRank"},
        };
        /* register_consts auto-enregistre + auto-alias ASCII (rangPrecedent). */
        owl_registry_register_consts(RANG_CONSTS,
            (int)(sizeof(RANG_CONSTS)/sizeof(RANG_CONSTS[0])));
    }
    /* alphabetCourant — TexteVersHTML optional 2nd param (no-op in OWL) */
    {
        static const OwlConstDef ALPHA_CONSTS[] = {
            {"alphabetCourant", "Chaines", INTEGER_VAL(0),
             "alphabet courant (no-op en OWL UTF-8)",
             "current alphabet (no-op in OWL UTF-8)", "charsetCurrent"},
        };
        owl_registry_register_consts(ALPHA_CONSTS, 1);
    }
    /* taille* constants (TailleVersChaîne unit) — doc 1000017038 */
    {
        static const OwlConstDef TAILLE_CONSTS[] = {
            {"tailleO",  "Chaines", INTEGER_VAL(TAILLE_O),  "octets",        "bytes", "sizeB"},
            {"tailleKo", "Chaines", INTEGER_VAL(TAILLE_KO), "kilo-octets",   "kilobytes", "sizeKB"},
            {"tailleMo", "Chaines", INTEGER_VAL(TAILLE_MO), "méga-octets",   "megabytes", "sizeMB"},
            {"tailleGo", "Chaines", INTEGER_VAL(TAILLE_GO), "giga-octets",   "gigabytes", "sizeGB"},
            {"tailleTO", "Chaines", INTEGER_VAL(TAILLE_TO), "téra-octets",   "terabytes", "sizeTB"},
        };
        owl_registry_register_consts(TAILLE_CONSTS,
            (int)(sizeof(TAILLE_CONSTS)/sizeof(TAILLE_CONSTS[0])));
    }
    /* enc* constants (owlEncodage) — codepage pour ChaîneVersBuffer /
     * BufferVersChaîne. Noms techniques identiques FR/EN (re-set EN bénin). */
    {
        static const OwlConstDef ENC_CONSTS[] = {
            {"encUTF8",        "Chaines", INTEGER_VAL(OWL_ENC_UTF8),
             "encodage UTF-8 (défaut, octets internes inchangés)",
             "UTF-8 encoding (default, internal bytes unchanged)", "encUTF8"},
            {"encUTF16LE",     "Chaines", INTEGER_VAL(OWL_ENC_UTF16LE),
             "encodage UTF-16 petit-boutiste (2 octets par codepoint BMP)",
             "UTF-16 little-endian encoding (2 bytes per BMP codepoint)", "encUTF16LE"},
            {"encUTF16BE",     "Chaines", INTEGER_VAL(OWL_ENC_UTF16BE),
             "encodage UTF-16 grand-boutiste (2 octets par codepoint BMP)",
             "UTF-16 big-endian encoding (2 bytes per BMP codepoint)", "encUTF16BE"},
            {"encLatin1",      "Chaines", INTEGER_VAL(OWL_ENC_LATIN1),
             "encodage Latin-1 / ISO-8859-1 (Europe occidentale, 1 octet)",
             "Latin-1 / ISO-8859-1 encoding (Western Europe, 1 byte)", "encLatin1"},
            {"encWindows1252", "Chaines", INTEGER_VAL(OWL_ENC_WINDOWS1252),
             "encodage Windows-1252 (Latin-1 + zone 0x80-0x9F : €, œ, guillemets courbes…)",
             "Windows-1252 encoding (Latin-1 plus the 0x80-0x9F range: €, œ, curly quotes…)", "encWindows1252"},
            {"encCP850",       "Chaines", INTEGER_VAL(OWL_ENC_CP850),
             "encodage CP850 (codepage DOS/OEM Europe de l'Ouest, console et exports historiques)",
             "CP850 encoding (DOS/OEM Western Europe codepage, console and legacy exports)", "encCP850"},
            {"encCP437",       "Chaines", INTEGER_VAL(OWL_ENC_CP437),
             "encodage CP437 (codepage DOS/OEM US, console historique et art ASCII)",
             "CP437 encoding (DOS/OEM US codepage, legacy console and ASCII art)", "encCP437"},
        };
        owl_registry_register_consts(ENC_CONSTS,
            (int)(sizeof(ENC_CONSTS)/sizeof(ENC_CONSTS[0])));
    }

    {
        static const OwlParam p_s[]     = { {"s", "chaîne", "source"}, {0} };
        static const OwlParam p_s_n[]   = { {"s", "chaîne", "source"}, {"n", "entier", "nombre de caractères"}, {0} };
        static const OwlParam p_s_ij[]  = { {"s", "chaîne", "source"}, {"position", "entier", "début (base 1)"}, {"longueur", "entier", "nb caractères"}, {0} };
        static const OwlParam p_s_q[]   = { {"s", "chaîne", "source"}, {"recherche", "chaîne", "sous-chaîne"}, {0} };
        /* Variante avec options combinables (Position, ChaîneOccurrence). */
        static const OwlParam p_s_q_opts[] = {
            {"s", "chaîne", "source"},
            {"recherche", "chaîne", "sous-chaîne"},
            {"options", "entier", "MotComplet + SansCasse + DepuisFin (combinable, optionnel)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        static const OwlParam p_s_oo[]  = { {"s", "chaîne", "source"}, {"ancien", "chaîne", "à remplacer"}, {"nouveau", "chaîne", "remplaçant"}, {0} };
        /* Variante avec options combinables (Remplace 4 args). */
        static const OwlParam p_s_oo_opts[] = {
            {"s", "chaîne", "source"},
            {"ancien", "chaîne", "à remplacer"},
            {"nouveau", "chaîne", "remplaçant"},
            {"options", "entier", "MotComplet + SansCasse + DepuisFin (combinable, optionnel)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        /* ChaîneIncremente (s [, mode]) — mode = DepuisDébut (défaut, début) ou DepuisFin (fin). */
        static const OwlParam p_s_incr[] = {
            {"s", "chaîne", "source"},
            {"mode", "entier", "DepuisDébut (défaut auto-détect ou début explicite) ou DepuisFin (incrémente la partie numérique fin)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        static const OwlParam p_nf[]    = { {"n", "numérique", "valeur"}, {"masque", "chaîne|entier|FormatNumérique", "masque printf-like, sentinel maskNombreSystème, ou struct FormatNumérique"}, {0} };
        static const OwlParam p_ex[]    = { {"s", "chaîne", "source"}, {"rang", "entier", "rang (base 1)"}, {"séparateur", "chaîne", "séparateur"}, {0} };
        static const OwlParam p_ex4[]   = { {"s", "chaîne", "source"}, {"rang", "entier", "rang (base 1)"}, {"séparateur", "chaîne", "séparateur"}, {"sens", "entier", "DepuisDébut (défaut) ou DepuisFin — compte les morceaux depuis la fin (optionnel)", NULL, &E_optionsRechercheChaine}, {0} };
        static const OwlParam p_rep[]   = { {"s", "chaîne", "motif"}, {"n", "entier", "nb répétitions"}, {0} };
        static const OwlParam p_code[]  = { {"code", "entier", "code ASCII/Unicode"}, {0} };

        static const OwlSignature sig_taille[] = { {1, p_s,    "entier", "longueur en caractères"}, {0} };
        static const OwlSignature sig_dimension[] = { {-1, p_s,  "entier", "taille ou NIL (si redim)"}, {0} };
        static const OwlSignature sig_left[]   = { {2, p_s_n,  "chaîne", "n premiers"}, {0} };
        /* Droite(s [, n]) — sans n : trim des espaces à gauche. */
        static const OwlParam p_droite[]       = { {"s", "chaîne", "source"}, {"n", "entier", "nombre de caractères (optionnel)"}, {0} };
        static const OwlSignature sig_droite[] = {
            {1, p_droite, "chaîne", "trim des espaces à gauche"},
            {2, p_droite, "chaîne", "n derniers caractères"},
            {0}
        };
        static const OwlParam p_s_pos[]        = { {"chaîne", "chaîne", "source"}, {"position", "entier", "début (base 1)"}, {0} };
        static const OwlSignature sig_mid[]    = {
            { 3, p_s_ij,  "chaîne", "sous-chaîne (n caractères)" },
            { 2, p_s_pos, "chaîne", "depuis position jusqu'à la fin" },
            {0}
        };
        /* Position(s, recherche [, départ [, option]]) — doc 3024027 :
         * 2 à 4 args. arg[2] = position de départ (entier), arg[3] = option
         * combinable.  : formes discrètes au lieu de -1 fourre-tout. */
        static const OwlParam p_s_q_dep[] = {
            {"s", "chaîne", "source"},
            {"recherche", "chaîne|tableau", "sous-chaîne (ou tableau de sous-chaînes)"},
            {"départ", "entier", "position de départ (base 1)"},
            {0}
        };
        static const OwlParam p_s_q_dep_opts[] = {
            {"s", "chaîne", "source"},
            {"recherche", "chaîne|tableau", "sous-chaîne (ou tableau de sous-chaînes)"},
            {"départ", "entier", "position de départ (base 1)"},
            {"options", "entier", "MotComplet + SansCasse + DepuisFin (combinable, optionnel)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        static const OwlSignature sig_find[]   = {
            {2, p_s_q,          "entier", "position base 1 ou 0"},
            {3, p_s_q_dep,      "entier", "position base 1 ou 0 (recherche depuis 'départ')"},
            {4, p_s_q_dep_opts, "entier", "position base 1 ou 0 (avec option)"},
            {0}
        };
        static const OwlSignature sig_repl[]   = {
            {3, p_s_oo,      "chaîne", "chaîne modifiée"},
            {4, p_s_oo_opts, "chaîne", "chaîne modifiée (avec options)"},
            {0}
        };
        static const OwlSignature sig_s_to_s[] = { {1, p_s,    "chaîne", "chaîne transformée"}, {0} };
        /*  — encodeurs/décodeurs de format (base64/hex) : acceptent
         * chaîne OU buffer (coercition to_string, lossless au niveau octet).
         * Signature explicite pour la découvrabilité via owl explain. */
        static const OwlParam p_enc_data[] = { {"données", "chaîne | buffer", "chaîne ou buffer"}, {0} };
        static const OwlSignature sig_str_or_buf[] = { {1, p_enc_data, "chaîne", "résultat encodé/décodé"}, {0} };
        /* sCalculeCrc32(tampon [, crcPartiel]) — CRC-32 des octets reels. */
        static const OwlParam p_crc32[] = {
            {"tampon", "chaîne | buffer", "chaîne ou buffer dont on somme les octets"},
            {"crcPartiel", "entier", "CRC déjà obtenu à reprendre pour le chaînage (défaut 0 = calcul frais)"},
            {0}
        };
        static const OwlSignature sig_crc32[] = {
            {1, p_crc32, "entier", "CRC-32 des octets fournis"},
            {2, p_crc32, "entier", "CRC-32 enchaîné sur un CRC partiel"},
            {0}
        };
        /* Base64 : format d'alphabet optionnel (standard / URL-safe). */
        static const OwlParam p_b64[] = {
            {"données", "chaîne | buffer", "chaîne ou buffer"},
            {"format", "entier", "encodeBASE64 (défaut) ou encodeBASE64URL", .allowedBase=&E_base64Format},
            {0}
        };
        static const OwlSignature sig_b64[] = {
            {1, p_b64, "chaîne", "résultat encodé/décodé (alphabet standard)"},
            {2, p_b64, "chaîne", "résultat encodé/décodé (alphabet explicite)"},
            {0}
        };
        /* URLEncode(url [, format]). */
        static const OwlParam p_urlenc[]       = {
            {"url", "chaîne", "URL source"},
            {"format", "entier", "mode encodeURL* (défaut UTF-8, optionnel)", .allowedBase=&E_urlEncodeMode},
            {0}
        };
        static const OwlSignature sig_urlenc[] = {
            {1, p_urlenc, "chaîne", "URL encodée (mode défaut)"},
            {2, p_urlenc, "chaîne", "URL encodée (mode encodeURL* explicite)"},
            {0}
        };
        static const OwlParam p_s_fmt[] = {
            {"chaîne",  "chaîne",  "chaîne à formater"},
            {"options", "entier",  "ccMajuscule / ccMinuscule / ccSansAccent / ccSansEspace / ccSansEspaceIntérieur / ccSansPonctuationNiEspace (combinables)",
             NULL, &E_chaineFormateOptions},
            {0}
        };
        static const OwlSignature sig_s_s[]     = { {2, p_s_fmt, "chaîne", "chaîne formatée"}, {0} };
        static const OwlParam p_sse[] = {
            {"chaîne",   "chaîne", "chaîne source"},
            {"position", "entier", "sscDroite/sscGauche/sscExtérieur (défaut)/sscIntérieur/sscTout",
             .allowedBase=&E_sansEspaceCote},
            {0}
        };
        static const OwlSignature sig_sse[]    = {
            {1, p_sse, "chaîne", "espaces extérieurs supprimés (défaut)"},
            {2, p_sse, "chaîne", "espaces supprimés selon la position"},
            {0}
        };
        static const OwlParam p_trq[] = {
            {"source", "chaîne",  "chaîne ou buffer à tronquer"},
            {"nombre", "entier",  "nombre de caractères (selon type)"},
            {"type",   "booléen", "nombreDeCaractèresAConserver (défaut) ou nombreDeCaractèresASupprimer",
             .allowedBase=&E_tronqueType},
            {0}
        };
        /* Tronque(n) sur un numérique = troncation vers zéro (1 arg, extension OWL).
         * Tronque(source, nombre [, type]) sur chaîne/buffer (2 ou 3 args). */
        static const OwlSignature sig_trq[]    = {
            {1, p_trq, "chaîne", "troncation numérique vers zéro (forme numérique)"},
            {2, p_trq, "chaîne", "chaîne/buffer tronqué"},
            {3, p_trq, "chaîne", "chaîne/buffer tronqué (type de troncature)"},
            {0}
        };
        static const OwlSignature sig_cnt[]    = {
            {1, p_s_q, "entier", "nombre de champs UI déclarés"},
            {2, p_s_q, "entier", "nombre d'occurrences d'une sous-chaîne"},
            {0}
        };
        static const OwlSignature sig_cnt_opts[] = {
            {2, p_s_q_opts, "entier", "nombre d'occurrences"},
            {3, p_s_q_opts, "entier", "nombre d'occurrences (avec options)"},
            {0}
        };
        static const OwlSignature sig_s_to_n[] = { {1, p_s,    "numérique", "valeur numérique"}, {0} };
        static const OwlParam p_estnum[] = { {"valeur", "chaîne|numérique", "valeur à tester"}, {0} };
        static const OwlSignature sig_estnum[] = { {1, p_estnum, "booléen", "Vrai si numérique ou chaîne entièrement numérique"}, {0} };
        static const OwlSignature sig_n_to_s[] = {
            {1, p_nf, "chaîne", "représentation par défaut"},
            {2, p_nf, "chaîne", "représentation formatée (masque)"},
            {0}
        };
        static const OwlSignature sig_ex[]     = {
            {3, p_ex,   "chaîne", "partie extraite"},
            {4, p_ex4,  "chaîne", "partie extraite (sens de parcours)"},
            {0}
        };
        /* Complète(s, largeur [, c [, sens]]) — sens (4e arg) = padLeft (extension OWL). */
        static const OwlParam p_pad4[]      = { {"s", "chaîne", "source"}, {"largeur", "entier", "largeur cible"}, {"c", "chaîne", "caractère de remplissage (optionnel)"}, {"sens", "entier", "0 = pad à droite (défaut), 1 = pad à gauche (optionnel)"}, {0} };
        static const OwlSignature sig_pad[]    = {
            {2, p_pad4, "chaîne", "chaîne complétée (espaces, à droite)"},
            {3, p_pad4, "chaîne", "chaîne complétée (caractère de remplissage)"},
            {4, p_pad4, "chaîne", "chaîne complétée (caractère et sens)"},
            {0}
        };
        static const OwlSignature sig_rep[]    = { {2, p_rep,  "chaîne", "motif répété"}, {0} };
        static const OwlSignature sig_c[]      = { {1, p_code, "chaîne", "caractère"}, {0} };
        static const OwlSignature sig_asc[]    = { {1, p_s,    "entier", "code du premier caractère"}, {0} };
        static const OwlParam p_hexstr[]       = { {"chaîneHexa", "chaîne", "chaîne hexadécimale à convertir"}, {0} };
        static const OwlSignature sig_hexint[] = { {1, p_hexstr, "entier", "valeur entière convertie"}, {0} };
        static const OwlParam p_pred[] = {
            {"chaîne",   "chaîne", "chaîne source"},
            {"motif",    "chaîne|tableau de chaînes", "chaîne (ou tableau de candidats) à chercher"},
            {"options",  "entier", "ccSansCasse, ccSansAccent, ... (combinables, optionnel)",
             NULL, &E_chaineCompare},
            {0}
        };
        static const OwlSignature sig_pred[]   = {
            {2, p_pred, "entier", "taille du match ou 0"},
            {3, p_pred, "entier", "taille du match ou 0 (avec options)"},
            {0}
        };
        /*  : Contient est une fonction de RECHERCHE — ses options sont
         * MotComplet / SansCasse (E_optionsRechercheChaine), pas les ccXxx de
         * comparaison. Param + signature dédiés pour isoler Contient de ses
         * soeurs CommencePar/FinitPar (qui partagent p_pred/sig_pred et dont le
         * rewire éventuel reste à suivre). */
        static const OwlParam p_pred_search[] = {
            {"chaîne",   "chaîne", "chaîne source"},
            {"motif",    "chaîne|tableau de chaînes", "chaîne (ou tableau de candidats) à chercher"},
            {"options",  "entier", "MotComplet, SansCasse, ... (combinables, optionnel)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        static const OwlSignature sig_pred_search[] = {
            {2, p_pred_search, "entier", "taille du match ou 0"},
            {3, p_pred_search, "entier", "taille du match ou 0 (avec options)"},
            {0}
        };
        static const OwlSignature sig_build[]  = { {-1, p_s,   "chaîne", "chaîne construite"}, {0} };
        /* ChaîneVersBuffer(s [, encodage [, longueur]]) — frontière charset (modèle sandwich).
         * Un tableau de params par arité : le renderer explain/LSP affiche tout le
         * tableau jusqu'au {0}, donc chaque signature doit pointer sur ses propres params. */
        static const OwlParam p_cvb1[] = {
            {"s", "chaîne", "chaîne source"},
            {0}
        };
        static const OwlParam p_cvb2[] = {
            {"s", "chaîne", "chaîne source"},
            {"encodage", "owlEncodage", "encodage cible (défaut encUTF8)", &E_owlEncodage},
            {0}
        };
        static const OwlParam p_cvb3[] = {
            {"s", "chaîne", "chaîne source"},
            {"encodage", "owlEncodage", "encodage cible (défaut encUTF8)", &E_owlEncodage},
            {"longueur", "entier", "longueur fixe du buffer en octets (padding de zéros à droite, exception si dépassement)"},
            {0}
        };
        static const OwlSignature sig_cvb[]    = {
            {1, p_cvb1, "buffer", "octets UTF-8 de la chaîne (défaut)"},
            {2, p_cvb2, "buffer", "octets de la chaîne dans l'encodage demandé"},
            {3, p_cvb3, "buffer", "octets calés à une longueur fixe (padding de zéros ou exception)"},
            {0}
        };
        /* BufferVersChaîne(buf [, encodage]) — symétrique, un tableau de params par arité. */
        static const OwlParam p_bvc1[] = {
            {"buffer", "buffer", "buffer source"},
            {0}
        };
        static const OwlParam p_bvc2[] = {
            {"buffer", "buffer", "buffer source"},
            {"encodage", "owlEncodage", "encodage source (défaut encUTF8)", &E_owlEncodage},
            {0}
        };
        static const OwlParam p_bvc4[] = {
            {"buffer", "buffer", "buffer source"},
            {"encodage", "owlEncodage", "encodage source (défaut encUTF8)", &E_owlEncodage},
            {"position", "entier", "position de lecture dans le buffer (base 1)"},
            {"longueur", "entier", "nombre d'octets à décoder à partir de la position"},
            {0}
        };
        static const OwlSignature sig_bvc[]    = {
            {1, p_bvc1, "chaîne", "chaîne décodée en UTF-8 (défaut)"},
            {2, p_bvc2, "chaîne", "chaîne décodée depuis l'encodage demandé"},
            {4, p_bvc4, "chaîne", "chaîne décodée depuis une tranche du buffer (position, longueur)"},
            {0}
        };

        /* Params for new functions */
        static const OwlParam p_s_s[]       = { {"s1","chaîne","première chaîne"}, {"s2","chaîne","seconde chaîne"}, {"options","entier","ccSansCasse + ccSansAccent + ... (optionnel)", NULL, &E_chaineCompare}, {0} };
        static const OwlParam p_s_sep[]     = { {"s","chaîne","source"}, {"sep","chaîne","séparateur"}, {0} };
        static const OwlParam p_s_ins_p[]   = { {"s","chaîne","source"}, {"insert","chaîne","chaîne à insérer"}, {"pos","entier","position base 1"}, {0} };
        static const OwlParam p_s_p_n[]     = { {"s","chaîne","source"}, {"pos","entier","position base 1"}, {"n","entier","nb caractères"}, {0} };
        static const OwlParam p_s_r_sep[]   = {
            {"s","chaîne","source"},
            {"rang","entier","occurrence (base 1)"},
            {"sepDebut","chaîne","séparateur début"},
            {"sepFin","chaîne","séparateur fin (optionnel, = sepDebut si absent)"},
            {"options","entier","options de recherche (combinable, optionnel)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        static const OwlParam p_s_line[]    = { {"s","chaîne","source"}, {"n","entier","numéro de ligne base 1"}, {0} };
        static const OwlParam p_s_q_n[]     = {
            {"s","chaîne","source"},
            {"sub","chaîne","sous-chaîne"},
            {"n","entier","rang de l'occurrence"},
            {"options","entier","MotComplet + SansCasse + DepuisFin (combinable, optionnel)",
             NULL, &E_optionsRechercheChaine},
            {0}
        };
        /* TailleVersChaîne(taille [, format]) — arg[1] est l'unité d'affichage
         * (tailleAuto/tailleO/tailleKo/...), pas un nombre de décimales. */
        static const OwlParam p_sz_unit[]   = {
            {"taille","entier","taille en octets"},
            {"formatAffichage","entier","unité d'affichage forcée : tailleO/tailleKo/tailleMo/tailleGo/tailleTO (unité auto par défaut, optionnel)",
             .allowedBase=&E_tailleUnite},
            {0}
        };
        static const OwlParam p_s_url[]     = { {"url","chaîne","URL source"}, {"element","chaîne","composant : scheme/host/port/path/query/fragment"}, {0} };
        static const OwlParam p_csv_in[]    = { {"csv","cha\xc3\xaene","cha\xc3\xaene au format CSV"}, {"sepColonne","cha\xc3\xaene","s\xc3\xa9parateur de colonnes (optionnel, active mode 2D)"}, {0} };
        static const OwlParam p_tab_csv[]   = { {"tab","tableau","tableau 1D ou 2D"}, {"sepColonne","cha\xc3\xae" "ne","s\xc3\xa9" "parateur de colonnes (optionnel, d\xc3\xa9" "faut \";\")"}, {0} };

        /* TableauVersChaîne(tab [, sepLigne [, sepColonne]]) — forme 2D. */
        static const OwlParam p_tab_sep2[]  = { {"tab","tableau","tableau source"}, {"sepLigne","chaîne","séparateur de ligne (1D: séparateur d'éléments, défaut CRLF)"}, {"sepColonne","chaîne","séparateur de colonne (2D, défaut TAB)"}, {0} };
        /* TexteVersHTML(s [, alphabet]) — 2e arg accepté pour compat (no-op UTF-8). */
        static const OwlParam p_s_html[]    = { {"s","chaîne","texte source"}, {"alphabet","entier","alphabet utilisé (optionnel, no-op en UTF-8)"}, {0} };
        /* ChaîneSupprimeDoublon(s [, sep [, comparaison]]). */
        static const OwlParam p_s_sep_cmp[] = { {"s","chaîne","source"}, {"sep","chaîne","séparateur (optionnel)"}, {"comparaison","entier","options de comparaison ccSansCasse/ccSansAccent (combinable, optionnel)", NULL, &E_chaineCompare}, {0} };

        /*  — formes discrètes (validation arité compile-time). */
        static const OwlSignature sig_cmp[]    = {
            {2, p_s_s, "entier", "-1 / 0 / 1"},
            {3, p_s_s, "entier", "-1 / 0 / 1 (avec options)"},
            {0}
        };
        static const OwlSignature sig_split[]  = { {-1, p_s_sep,  "tableau", "tableau de sous-chaînes"}, {0} };
        static const OwlSignature sig_join[]   = {
            {1, p_tab_sep2, "chaîne", "éléments concaténés (séparateur défaut CRLF)"},
            {2, p_tab_sep2, "chaîne", "éléments joints par le séparateur"},
            {3, p_tab_sep2, "chaîne", "join 2D (sepLigne défaut CRLF, sepColonne défaut TAB)"},
            {0}
        };
        static const OwlSignature sig_csv_in[] = {
            {1, p_csv_in, "tableau", "tableau 1D (pas de séparateur de colonnes)"},
            {2, p_csv_in, "tableau", "tableau 2D (avec séparateur de colonnes)"},
            {0}
        };
        static const OwlSignature sig_csv_out[]= {
            {1, p_tab_csv, "cha\xc3\xae" "ne", "CSV (séparateur de colonnes défaut \";\")"},
            {2, p_tab_csv, "cha\xc3\xae" "ne", "CSV (séparateur de colonnes explicite)"},
            {0}
        };
        static const OwlSignature sig_ins[]    = { {3,  p_s_ins_p,"chaîne",  "chaîne avec insertion"}, {0} };
        static const OwlSignature sig_del[]    = {
            {2, p_s_p_n, "chaîne", "chaîne modifiée (sous-chaîne supprimée)"},
            {3, p_s_p_n, "chaîne", "chaîne modifiée (avec options ou position+longueur)"},
            {0}
        };
        static const OwlSignature sig_retr[]   = { {3,  p_s_p_n,  "chaîne",  "sous-chaîne"}, {0} };
        static const OwlSignature sig_between[]= {
            {3, p_s_r_sep, "chaîne", "chaîne extraite"},
            {4, p_s_r_sep, "chaîne", "chaîne extraite (avec séparateur de fin ou options)"},
            {5, p_s_r_sep, "chaîne", "chaîne extraite (avec séparateur de fin et options)"},
            {0}
        };
        static const OwlSignature sig_line[]   = { {2,  p_s_line, "chaîne",  "ligne extraite"}, {0} };
        static const OwlSignature sig_posocc[] = {
            {2, p_s_q_n, "entier", "position base 1 ou 0"},
            {3, p_s_q_n, "entier", "position base 1 ou 0 (avec rang)"},
            {4, p_s_q_n, "entier", "position base 1 ou 0 (avec rang et options)"},
            {0}
        };
        static const OwlSignature sig_p2l[]    = { {2,  p_s_line, "entier",  "numéro de ligne"}, {0} };
        static const OwlSignature sig_sz2s[]   = {
            {1, p_sz_unit, "chaîne", "taille formatée (unité auto)"},
            {2, p_sz_unit, "chaîne", "taille formatée (unité forcée)"},
            {0}
        };
        static const OwlSignature sig_s2s[]    = {
            {1, p_s_html, "chaîne", "texte échappé HTML"},
            {2, p_s_html, "chaîne", "texte échappé HTML (alphabet accepté pour compat)"},
            {0}
        };
        static const OwlSignature sig_url[]    = { {2,  p_s_url,  "chaîne",  "composant extrait"}, {0} };
        static const OwlSignature sig_incr[]   = {
            {1, p_s_incr, "chaîne", "chaîne incrémentée"},
            {2, p_s_incr, "chaîne", "chaîne incrémentée (position explicite)"},
            {0}
        };
        static const OwlSignature sig_dedup[]  = {
            {1, p_s_sep_cmp, "chaîne", "chaîne sans doublons (niveau caractère)"},
            {2, p_s_sep_cmp, "chaîne", "chaîne sans doublons (niveau token, séparateur)"},
            {3, p_s_sep_cmp, "chaîne", "chaîne sans doublons (avec options de comparaison)"},
            {0}
        };

        static const OwlNativeDef DEFS[] = {
            { .canonical = "Taille", .nameEN = "Length", .category = "Chaines",
              .descFR = "Longueur d'une chaîne ou nombre d'éléments d'un tableau.",
              .descEN = "Length of a string or count of array elements.",
              .signatures = sig_taille, .impl = fn_taille },

            /* Dimension(tab) — taille d'un tableau (== Occurrence pour nested OWL).
             * Dimension(tab, r [, c [, ...]]) — redimensionne le tableau (nested).
             * Doc 3013022. */
            { .canonical = "Dimension", .nameEN = "Dimension", .category = "Tableaux",
              .descFR = "Taille ou redimensionne un tableau (1D ou multi-dim nested).",
              .descEN = "Size or resize an array (1D or multi-dim nested).",
              .signatures = sig_dimension, .impl = fn_dimension },

            { .canonical      = "Cha\xc3\xae" "neVersBuffer",      /* ChaîneVersBuffer */
              .canonicalAscii = "ChaineVersBuffer",
              .nameEN = "StringToBuffer", .category = "Chaines",
              .descFR = "Convertit une chaîne en buffer d'octets selon l'encodage cible (UTF-8 par défaut).",
              .descEN = "Convert a string to a byte buffer in the target encoding (UTF-8 by default).",
              .signatures = sig_cvb, .impl = fn_chaine_vers_buffer },
            { .canonical      = "BufferVersCha\xc3\xae" "ne",      /* BufferVersChaîne */
              .canonicalAscii = "BufferVersChaine",
              .nameEN = "BufferToString", .category = "Chaines",
              .descFR = "Décode un buffer d'octets en chaîne selon l'encodage fourni (UTF-8 par défaut).",
              .descEN = "Decode a byte buffer into a string using the given encoding (UTF-8 by default).",
              .signatures = sig_bvc, .impl = fn_buffer_vers_chaine },

            { .canonical = "Gauche", .nameEN = "Left",   .category = "Chaines",
              .descFR = "Retourne les n premiers caractères d'une chaîne.",
              .descEN = "Return the first n characters.",
              .signatures = sig_left, .impl = fn_gauche },
            { .canonical = "Droite", .nameEN = "Right",  .category = "Chaines",
              .descFR = "Retourne les n derniers caractères d'une chaîne (ou trim espaces si n omis).",
              .descEN = "Return the last n characters (or trim left spaces if n omitted).",
              .signatures = sig_droite, .impl = fn_droite },
            { .canonical = "Milieu", .nameEN = "Middle", .category = "Chaines",
              .descFR = "Extrait une sous-chaîne à partir d'une position (base 1).",
              .descEN = "Extract a substring starting at a position (base 1).",
              .signatures = sig_mid, .impl = fn_milieu },

            { .canonical = "Position", .nameEN = "Position", .category = "Chaines",
              .descFR = "Position d'une sous-chaîne (base 1, 0 si absente).",
              .descEN = "Position of a substring (base 1, 0 if absent).",
              .signatures = sig_find, .impl = fn_position },
            { .canonical = "Remplace", .nameEN = "Replace", .category = "Chaines",
              .descFR = "Remplace toutes les occurrences d'une sous-chaîne.",
              .descEN = "Replace all occurrences of a substring.",
              .signatures = sig_repl, .impl = fn_remplace },
            { .canonical = "Majuscule", .nameEN = "Upper", .category = "Chaines",
              .descFR = "Convertit en majuscules.",
              .descEN = "Convert to uppercase.",
              .signatures = sig_s_to_s, .impl = fn_majuscule },
            { .canonical = "Minuscule", .nameEN = "Lower", .category = "Chaines",
              .descFR = "Convertit en minuscules.",
              .descEN = "Convert to lowercase.",
              .signatures = sig_s_to_s, .impl = fn_minuscule },
            { .canonical = "SansEspace", .nameEN = "NoSpace", .category = "Chaines",
              .descFR = "Supprime les espaces (U+0020 uniquement) selon la position : sscDroite, sscGauche, sscExtérieur (défaut), sscIntérieur, sscTout.",
              .descEN = "Strip ASCII spaces (U+0020) per position constant.",
              .signatures = sig_sse, .impl = fn_sansespace },

            { .canonical = "Occurrence", .nameEN = "Count", .category = "Chaines",
              .descFR = "Compte le nombre d'occurrences d'une sous-chaîne.",
              .descEN = "Count occurrences of a substring.",
              .signatures = sig_cnt, .impl = fn_occurrence },

            /* Tronque(s, n [, type]).
             * Polymorphique : sur entier/réel, dispatch vers NumériqueTronque
             * (truncation vers zéro). Sur chaîne/buffer, comportement canon. */
            { .canonical = "Tronque", .nameEN = "Truncate", .category = "Chaines",
              .descFR = "Coupe la fin d'une chaîne ou d'un buffer. Sur un numérique, se ramène à NumériqueTronque (troncation vers zéro).",
              .descEN = "Drops the trailing end of a string or buffer. On a numeric, reduces to NumericTruncate (truncation toward zero).",
              .signatures = sig_trq, .impl = fn_tronque },

            { .canonical      = "Cha\xc3\xae" "neVersEntier",      /* ChaîneVersEntier */
              .canonicalAscii = "ChaineVersEntier",
              .nameEN = "StringToInteger", .category = "Chaines",
              .descFR = "Convertit une chaîne en entier strict.",
              .descEN = "Convert a string to a strict integer.",
              .signatures = sig_s_to_n, .impl = fn_chaine_vers_entier },

            /* Val(s) — canon-standard polymorphic numeric parser:
             * returns a real if the string contains a decimal, integer otherwise. */
            { .canonical = "Val", .nameEN = "Val", .category = "Chaines",
              .descFR = "Convertit une chaîne en numérique (entier ou réel selon le contenu).",
              .descEN = "Convert a string to a number (integer or real based on content).",
              .signatures = sig_s_to_n, .impl = fn_val },

            /* ChaîneVersNumérique — identique à Val (doc 1410087852), alias officiel. */
            { .canonical      = "Cha\xc3\xae" "neVersNum\xc3\xa9" "rique",  /* ChaîneVersNumérique */
              .canonicalAscii = "ChaineVersNumerique",
              .nameEN = "StringToNum", .category = "Chaines",
              .descFR = "Renvoie la valeur numérique exacte d'une chaîne (précision arbitraire).",
              .descEN = "Return the exact numeric value of a string (arbitrary precision).",
              .signatures = sig_s_to_n, .impl = fn_chaine_vers_numerique },
            { .canonical      = "Cha\xc3\xae" "neVersR\xc3\xa9" "el",  /* ChaîneVersRéel */
              .canonicalAscii = "ChaineVersReel",
              .nameEN = "ValR", .category = "Chaines",
              .descFR = "Convertit une chaîne en réel.",
              .descEN = "Convert a string to real.",
              .signatures = sig_s_to_n, .impl = fn_chaine_vers_reel },
            /* EstNumérique — règle stricte plein-chaîne (cf. ). */
            { .canonical      = "EstNum\xc3\xa9" "rique",  /* EstNumérique */
              .canonicalAscii = "EstNumerique",
              .nameEN = "IsNumeric", .category = "Chaines",
              .descFR = "Indique si la valeur est numérique. Sur une chaîne, la règle est stricte : après suppression des espaces de bord, la totalité de la chaîne doit former un nombre décimal valide (signe optionnel, chiffres, séparateur décimal point). Un caractère non numérique, un espace interne ou une virgule décimale renvoient Faux.",
              .descEN = "Tells whether the value is numeric. On a string, the rule is strict: after trimming edge whitespace, the whole string must form a valid decimal number (optional sign, digits, dot decimal separator). Any non-numeric character, inner space, or comma decimal separator returns False.",
              .signatures = sig_estnum, .impl = fn_est_numerique },
            { .canonical      = "Num\xc3\xa9" "riqueVersCha\xc3\xae" "ne", /* NumériqueVersChaîne */
              .canonicalAscii = "NumeriqueVersChaine",
              .nameEN = "NumToString", .category = "Chaines",
              .descFR = "Convertit un nombre en chaîne. Le masque chaîne est printf-like mais s'écrit SANS le « % » (ex. « .2f » pour deux décimales, « .0f », « 2f »). Un masque entier désigne un format système (sentinel maskNombreSystème / maskMonétaireSystème), pas un nombre de décimales. Accepte aussi une struct FormatNumérique, ou aucun argument pour la forme par défaut. Un masque chaîne non reconnu lève une erreur.",
              .descEN = "Convert a number to string. The string mask is printf-like but written WITHOUT the '%' (e.g. '.2f' for two decimals, '.0f', '2f'). An integer mask selects a system format (sentinel maskNumberSystem / maskCurrencySystem), not a decimal count. Also accepts a NumberFormat struct, or no argument for the default form. An unrecognized string mask raises an error.",
              .signatures = sig_n_to_s, .impl = fn_chaine_formate },
            { .canonical      = "OwlConfigureLocale",
              .nameEN          = "OwlConfigureLocale", .category = "Chaines",
              .descFR          = "Override des séparateurs décimal/milliers utilisés par NumériqueVersChaîne (extension OWL pour cas où la locale système n'est pas la bonne).",
              .descEN          = "Override decimal/thousand separators used by NumToString (OWL extension for when system locale is not what you want).",
              .signatures      = sig_s_s, .impl = fn_owl_configure_locale },
            { .canonical      = "Cha\xc3\xae" "neFormate",         /* ChaîneFormate */
              .canonicalAscii = "ChaineFormate",
              .nameEN = "StringFormat", .category = "Chaines",
              .descFR = "Formate une chaîne selon des options (ccMajuscule, ccSansAccent, ...).",
              .descEN = "Format a string with options (ccMajuscule, ccSansAccent, ...).",
              .signatures = sig_s_s, .impl = fn_chaine_formate_options },

            { .canonical      = "ExtraitCha\xc3\xae" "ne",          /* ExtraitChaîne */
              .canonicalAscii = "ExtraitChaine",
              .nameEN = "ExtractString", .category = "Chaines",
              .descFR = "Extrait une portion entre séparateurs.",
              .descEN = "Extract a segment between separators.",
              .signatures = sig_ex, .impl = fn_extrait_chaine },
            { .canonical      = "Cha\xc3\xae" "neInverse",         /* ChaîneInverse */
              .canonicalAscii = "ChaineInverse",
              .nameEN = "StringReverse", .category = "Chaines",
              .descFR = "Inverse l'ordre des caractères d'une chaîne (\"ABC\" -> \"CBA\").",
              .descEN = "Reverses the order of characters in a string (\"ABC\" -> \"CBA\").",
              .signatures = sig_s_to_s, .impl = fn_inverse_chaine },

            { .canonical      = "Cha\xc3\xae" "neVersBase64",       /* ChaîneVersBase64 */
              .canonicalAscii = "ChaineVersBase64",
              .nameEN = "StringToBase64", .category = "Encodage",
              .descFR = "Encode une chaîne ou un buffer en Base64. 2e arg = format : encodeBASE64 (défaut, alphabet '+/' complété par '=') ou encodeBASE64URL (alphabet '-_' sans remplissage, transmissible dans une URL).",
              .descEN = "Encode a string or buffer as Base64. 2nd arg = format: encodeBASE64 (default, '+/' alphabet padded with '=') or encodeBASE64URL ('-_' alphabet without padding, safe in a URL).",
              .signatures = sig_b64, .impl = fn_string_to_base64 },
            { .canonical      = "Base64VersCha\xc3\xae" "ne",       /* Base64VersChaîne */
              .canonicalAscii = "Base64VersChaine",
              .nameEN = "Base64ToString", .category = "Encodage",
              .descFR = "Décode du Base64. 2e arg = format : encodeBASE64 (défaut) ou encodeBASE64URL. Chaque alphabet refuse les caractères de l'autre ; entrée invalide -> erreur et chaîne vide.",
              .descEN = "Decode Base64. 2nd arg = format: encodeBASE64 (default) or encodeBASE64URL. Each alphabet rejects the other's characters; invalid input -> error and empty string.",
              .signatures = sig_b64, .impl = fn_base64_to_string },

            { .canonical = "URLEncode", .nameEN = "URLEncode", .category = "Encodage",
              .descFR = "Encode une chaîne pour URL (percent-encoding). 2e arg = mode encodeURL* (défaut: préserve séparateurs URL ; encodeURLParamètre: encode tout).",
              .descEN = "URL percent-encode a string. 2nd arg = encodeURL* mode (default preserves URL separators; encodeURLParametre encodes everything).",
              .signatures = sig_urlenc, .impl = fn_url_encode },
            { .canonical = "URLDecode", .nameEN = "URLDecode", .category = "Encodage",
              .descFR = "Décode une chaîne URL (percent-decoding).",
              .descEN = "URL percent-decode a string.",
              .signatures = sig_s_to_s, .impl = fn_url_decode },

            { .canonical      = "Cha\xc3\xae" "neVersHexa",         /* ChaîneVersHexa */
              .canonicalAscii = "ChaineVersHexa",
              .nameEN = "StringToHex", .category = "Encodage",
              .descFR = "Convertit en représentation hexadécimale.",
              .descEN = "Convert to hexadecimal representation.",
              .signatures = sig_str_or_buf, .impl = fn_string_to_hex },
            { .canonical      = "HexaVersCha\xc3\xae" "ne",         /* HexaVersChaîne */
              .canonicalAscii = "HexaVersChaine",
              .nameEN = "HexToString", .category = "Encodage",
              .descFR = "Décode d'hexadécimal.",
              .descEN = "Decode from hexadecimal.",
              .signatures = sig_str_or_buf, .impl = fn_hex_to_string },

            { .canonical = "sCalculeCrc32", .nameEN = "sComputeCrc32", .category = "Encodage",
              .descFR = "Calcule le CRC-32 (somme de contrôle sur 32 bits, variante utilisée par zip, zlib et PNG) des octets d'une chaîne ou d'un buffer. Un CRC partiel optionnel permet d'enchaîner le calcul sur des fragments successifs (omis ou nul : calcul frais).",
              .descEN = "Computes the CRC-32 (32-bit checksum, the variant used by zip, zlib and PNG) of the bytes of a string or buffer. An optional partial CRC chains the computation over successive fragments (omitted or zero: fresh computation).",
              .signatures = sig_crc32, .impl = fn_calcule_crc32 },

            { .canonical = "Caract", .nameEN = "Charact", .category = "Chaines",
              .descFR = "Caractère depuis un code numérique.",
              .descEN = "Character from a numeric code.",
              .signatures = sig_c, .impl = fn_caract },
            { .canonical = "Asc", .nameEN = "Asc", .category = "Chaines",
              .descFR = "Code numérique du premier caractère d'une chaîne.",
              .descEN = "Numeric code of the first character of a string.",
              .signatures = sig_asc, .impl = fn_asc },
            { .canonical = "CaractUnicode", .nameEN = "CharactUnicode", .category = "Chaines",
              .descFR = "Caractère correspondant à un point de code Unicode. Un point de code hors plage ou de zone surrogate signale une erreur.",
              .descEN = "Character for a given Unicode code point. An out-of-range or surrogate code point reports an error.",
              .signatures = sig_c, .impl = fn_caract_unicode },
            { .canonical = "HexaVersEntier", .nameEN = "HexaToInt", .category = "Chaines",
              .descFR = "Convertit une chaîne hexadécimale en entier. La casse des chiffres est indifférente.",
              .descEN = "Converts a hexadecimal string to an integer. Digit case is irrelevant.",
              .signatures = sig_hexint, .impl = fn_hexa_vers_entier },

            { .canonical      = "Cha\xc3\xae" "neConstruit",        /* ChaîneConstruit */
              .canonicalAscii = "ChaineConstruit",
              .nameEN = "StringBuild", .category = "Chaines",
              .descFR = "Construit une chaîne en substituant les marqueurs positionnels %1, %2, … par les arguments correspondants (base 1). Un marqueur sans argument donne une chaîne vide ; %% reste littéral (non échappé).",
              .descEN = "Builds a string by substituting positional markers %1, %2, … with the matching arguments (1-based). A marker with no argument yields an empty string; %% stays literal (not escaped).",
              .signatures = sig_build, .impl = fn_chaine_construit },

            { .canonical = "CommencePar", .nameEN = "StartsWith", .category = "Chaines",
              .descFR = "Vrai si la chaîne commence par le préfixe donné.",
              .descEN = "True if the string starts with the given prefix.",
              .signatures = sig_pred, .impl = fn_commence_par },
            { .canonical = "FinitPar",    .nameEN = "EndsWith",   .category = "Chaines",
              .descFR = "Vrai si la chaîne se termine par le suffixe donné.",
              .descEN = "True if the string ends with the given suffix.",
              .signatures = sig_pred, .impl = fn_finit_par },
            { .canonical = "Contient",    .nameEN = "Contains",   .category = "Chaines",
              .descFR = "Vrai si la chaîne contient la sous-chaîne.",
              .descEN = "True if the string contains the substring.",
              .signatures = sig_pred_search, .impl = fn_contient },

            { .canonical = "Compl\xc3\xa8" "te", .canonicalAscii = "Complete", .nameEN = "Complete", .category = "Chaines",
              .descFR = "Complète une chaîne à la largeur voulue (pad droit).",
              .descEN = "Pad a string to the desired width (right pad).",
              .signatures = sig_pad, .impl = fn_complet },
            { .canonical      = "R\xc3\xa9" "p\xc3\xa8" "te",  /* Répète — canon */
              .canonicalAscii = "Repete",
              .nameEN = "RepeatString", .category = "Chaines",
              .descFR = "Répète une chaîne n fois.",
              .descEN = "Repeat a string n times.",
              .signatures = sig_rep, .impl = fn_repete_chaine },

            /* === New string functions === */
            { .canonical      = "Cha\xc3\xae" "neCompare", .canonicalAscii = "ChaineCompare",
              .nameEN = "StringCompare", .category = "Chaines",
              .descFR = "Compare deux chaînes. Retourne -1, 0 ou 1.",
              .descEN = "Compare two strings. Returns -1, 0 or 1.",
              .signatures = sig_cmp, .impl = fn_chaine_compare },

            { .canonical      = "Cha\xc3\xae" "neVersTableau", .canonicalAscii = "ChaineVersTableau",
              .nameEN = "StringToArray", .category = "Chaines",
              .descFR = "Découpe une chaîne en tableau selon un séparateur.",
              .descEN = "Split a string into an array using a separator.",
              .signatures = sig_split, .impl = fn_chaine_vers_tableau },

            { .canonical      = "Cha\xc3\xae" "neD\xc3\xa9" "coupe", .canonicalAscii = "ChaineDecoupe",
              .nameEN = "StringSplit", .category = "Chaines",
              .descFR = "Découpe une chaîne en segments selon un ou plusieurs séparateurs.",
              .descEN = "Split a string into segments using one or more separators.",
              .signatures = sig_split, .impl = fn_chaine_decoupe },

            { .canonical      = "TableauVersCha\xc3\xae" "ne", .canonicalAscii = "TableauVersChaine",
              .nameEN = "ArrayToString", .category = "Chaines",
              .descFR = "Concatène les éléments d'un tableau avec un séparateur.",
              .descEN = "Join array elements with a separator.",
              .signatures = sig_join, .impl = fn_tableau_vers_chaine },

            { .canonical      = "TableauVersCSV", .canonicalAscii = "TableauVersCSV",
              .nameEN = "ArrayToCSV", .category = "Chaines",
              .descFR = "Convertit un tableau 1D ou 2D en cha\xc3\xae" "ne CSV (lignes s\xc3\xa9" "par\xc3\xa9" "es par CRLF, colonnes par \";\" par d\xc3\xa9" "faut). Quoting automatique conforme RFC-4180 : un champ contenant le s\xc3\xa9" "parateur, un guillemet ou un retour-ligne est encadr\xc3\xa9 de guillemets et ses guillemets internes doubl\xc3\xa9" "s ; les champs simples restent inchang\xc3\xa9" "s.",
              .descEN = "Convert a 1D or 2D array to a CSV string (rows separated by CRLF, columns by \";\" by default). Automatic RFC-4180 quoting: a field containing the separator, a quote or a line break is wrapped in quotes with its inner quotes doubled; simple fields stay unchanged.",
              .signatures = sig_csv_out, .impl = fn_tableau_vers_csv },

            { .canonical      = "CSVVersTableau", .canonicalAscii = "CSVVersTableau",
              .nameEN = "CSVToArray", .category = "Chaines",
              .descFR = "Convertit une cha\xc3\xaene CSV en tableau 1D (sans sepColonne) ou 2D (avec sepColonne). Parsing conforme RFC-4180 : comprend les champs encadr\xc3\xa9" "s de guillemets, un s\xc3\xa9" "parateur ou un retour-ligne \xc3\xa0 l'int\xc3\xa9" "rieur ne coupe pas le champ. OWL: renvoie un nouveau tableau.",
              .descEN = "Convert a CSV string to a 1D array (no colSep) or 2D array (with colSep). RFC-4180 compliant parsing: understands quoted fields, a separator or line break inside a quoted field does not split it. OWL: returns a new array.",
              .signatures = sig_csv_in, .impl = fn_csv_vers_tableau },

            { .canonical      = "Cha\xc3\xae" "neIns\xc3\xa8" "re", .canonicalAscii = "ChaineInsere",
              .nameEN = "StringInsert", .category = "Chaines",
              .descFR = "Insère une chaîne à la position donnée (base 1). Syntaxe : ChaîneInsère(source, chaîneAInsérer, position).",
              .descEN = "Insert a string at the given position (base 1). Syntax: StringInsert(source, stringToInsert, position).",
              .signatures = sig_ins, .impl = fn_chaine_insere },

            { .canonical      = "Cha\xc3\xae" "neSupprime", .canonicalAscii = "ChaineSupprime",
              .nameEN = "StringDelete", .category = "Chaines",
              .descFR = "Supprime une sous-chaîne (toutes occurrences) ou n caractères à la position donnée. Forme 1 : ChaîneSupprime(s, sous-chaîne). Forme 2 : ChaîneSupprime(s, position, n).",
              .descEN = "Remove all occurrences of a substring, or delete n chars at a position. Form 1: StringDelete(s, substring). Form 2: StringDelete(s, position, n).",
              .signatures = sig_del, .impl = fn_chaine_supprime_chaine },

            { .canonical      = "Cha\xc3\xae" "neR\xc3\xa9" "cup\xc3\xa8" "re", .canonicalAscii = "ChaineRecupere",
              .nameEN = "StringRetrieve", .category = "Chaines",
              .descFR = "Extrait n caractères à la position donnée (alias Milieu).",
              .descEN = "Extract n characters at position (alias for Middle).",
              .signatures = sig_retr, .impl = fn_chaine_recupere },

            { .canonical      = "Cha\xc3\xae" "neOccurrence", .canonicalAscii = "ChaineOccurrence",
              .nameEN = "StringCount", .category = "Chaines",
              .descFR = "Nombre d'occurrences d'une sous-chaîne.",
              .descEN = "Number of occurrences of a substring.",
              .signatures = sig_cnt_opts, .impl = fn_chaine_occurrence },

            { .canonical      = "Cha\xc3\xae" "neSupprimeDoublon", .canonicalAscii = "ChaineSupprimeDoublon",
              .nameEN = "StringDeleteDuplicate", .category = "Chaines",
              .descFR = "Supprime les doublons consécutifs dans une chaîne.",
              .descEN = "Remove consecutive duplicate characters or tokens.",
              .signatures = sig_dedup, .impl = fn_chaine_supprime_doublon },

            { .canonical      = "Cha\xc3\xae" "neIncr\xc3\xa9" "mente", .canonicalAscii = "ChaineIncremente",
              .nameEN = "StringIncrement", .category = "Chaines",
              .descFR = "Incrémente la partie numérique d'une chaîne (\"ABC001\" → \"ABC002\"). Position : DepuisFin (défaut) ou DepuisDébut.",
              .descEN = "Increment the numeric part of a string (\"ABC001\" → \"ABC002\"). Position: FromEnd (default) or FromBeginning.",
              .signatures = sig_incr, .impl = fn_chaine_incremente },

            { .canonical      = "Cha\xc3\xae" "neCommencePar", .canonicalAscii = "ChaineCommencePar",
              .nameEN = "StringStartsWith", .category = "Chaines",
              .descFR = "Vrai si la chaîne commence par le préfixe donné.",
              .descEN = "True if the string starts with the given prefix.",
              .signatures = sig_pred, .impl = fn_commence_par },

            { .canonical      = "Cha\xc3\xae" "neFinitPar", .canonicalAscii = "ChaineFinitPar",
              .nameEN = "StringEndsWith", .category = "Chaines",
              .descFR = "Vrai si la chaîne se termine par le suffixe donné.",
              .descEN = "True if the string ends with the given suffix.",
              .signatures = sig_pred, .impl = fn_finit_par },

            { .canonical      = "ExtraitCha\xc3\xae" "neEntre", .canonicalAscii = "ExtraitChaineEntre",
              .nameEN = "ExtractStringBetween", .category = "Chaines",
              .descFR = "Extrait la n-ième sous-chaîne entre deux marqueurs. Syntaxe : ExtraitChaîneEntre(chaîne, rang, séparateurDébut [, séparateurFin]).",
              .descEN = "Extract the n-th substring between two delimiters. Syntax: ExtractStringBetween(string, rank, startDelimiter [, endDelimiter]).",
              .signatures = sig_between, .impl = fn_extrait_chaine_entre },

            { .canonical = "ExtraitLigne", .nameEN = "ExtractLine", .category = "Chaines",
              .descFR = "Extrait la n-ième ligne d'une chaîne multilignes (base 1).",
              .descEN = "Extract the n-th line from a multiline string (base 1).",
              .signatures = sig_line, .impl = fn_extrait_ligne },

            { .canonical = "PositionOccurrence", .nameEN = "PositionOccurrence", .category = "Chaines",
              .descFR = "Position de la n-ième occurrence d'une sous-chaîne (base 1).",
              .descEN = "Position of the n-th occurrence of a substring (base 1).",
              .signatures = sig_posocc, .impl = fn_position_occurrence },

            { .canonical      = "PositionVersLigne", .nameEN = "PositionToLine", .category = "Chaines",
              .descFR = "Retourne le numéro de ligne correspondant à une position caractère.",
              .descEN = "Return the line number for a character position.",
              .signatures = sig_p2l, .impl = fn_position_vers_ligne },

            { .canonical      = "TailleVersCha\xc3\xae" "ne", .canonicalAscii = "TailleVersChaine",
              .nameEN = "LengthToString", .category = "Chaines",
              .descFR = "Formate une taille en octets en chaîne lisible (\"1,50 Ko\").",
              .descEN = "Format a byte count as a human-readable string (\"1.50 KB\").",
              .signatures = sig_sz2s, .impl = fn_taille_vers_chaine },

            { .canonical = "TexteVersHTML", .nameEN = "TextToHTML", .category = "Chaines",
              .descFR = "Échappe les caractères HTML spéciaux (& < > \" ').",
              .descEN = "Escape HTML special characters (& < > \" ').",
              .signatures = sig_s2s, .impl = fn_texte_vers_html },

            { .canonical      = "URLExtraitChemin", .nameEN = "URLExtractPath", .category = "Chaines",
              .descFR = "Extrait un composant d'une URL (scheme, host, port, path, query, fragment).",
              .descEN = "Extract a component from a URL (scheme, host, port, path, query, fragment).",
              .signatures = sig_url, .impl = fn_url_extrait_chemin },

        };
        owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    }

    /* === Extension methods on strings: s.Majuscule(), s.VersEntier(), etc. === */
    owl_register_method(METHOD_TYPE_STRING, "Taille",      fn_taille,     1);
    owl_register_method(METHOD_TYPE_STRING, "Length",       fn_taille,     1);
    owl_register_method(METHOD_TYPE_STRING, "Gauche",      fn_gauche,     2);
    owl_register_method(METHOD_TYPE_STRING, "Left",        fn_gauche,     2);
    owl_register_method(METHOD_TYPE_STRING, "Droite",      fn_droite,     -1);
    owl_register_method(METHOD_TYPE_STRING, "Right",       fn_droite,     -1);
    /*  — binding -1 : la native a plusieurs formes ({2,3}), la
     * validation d'arité passe par les signatures (compile-time) + l'impl. */
    owl_register_method(METHOD_TYPE_STRING, "Milieu",      fn_milieu,    -1);
    owl_register_method(METHOD_TYPE_STRING, "Middle",      fn_milieu,    -1);
    owl_register_method(METHOD_TYPE_STRING, "Position",    fn_position,  -1);
    owl_register_method(METHOD_TYPE_STRING, "Find",        fn_position,  -1);
    owl_register_method(METHOD_TYPE_STRING, "Remplace",    fn_remplace,   -1);
    owl_register_method(METHOD_TYPE_STRING, "Replace",     fn_remplace,  -1); /*  : formes {3,4} */
    owl_register_method(METHOD_TYPE_STRING, "Majuscule",   fn_majuscule,  1);
    owl_register_method(METHOD_TYPE_STRING, "Upper",       fn_majuscule,  1);
    owl_register_method(METHOD_TYPE_STRING, "Minuscule",   fn_minuscule,  1);
    owl_register_method(METHOD_TYPE_STRING, "Lower",       fn_minuscule,  1);
    owl_register_method(METHOD_TYPE_STRING, "SansEspace",  fn_sansespace, -1);
    owl_register_method(METHOD_TYPE_STRING, "Trim",        fn_sansespace, -1);
    owl_register_method(METHOD_TYPE_STRING, "Tronque",     fn_tronque,    -1);
    owl_register_method(METHOD_TYPE_STRING, "Truncate",    fn_tronque,    -1);
    owl_register_method(METHOD_TYPE_STRING, "Occurrence",  fn_occurrence, -1);
    owl_register_method(METHOD_TYPE_STRING, "Count",       fn_occurrence, -1);
    /* Prefix-stripped conversion aliases */
    owl_register_method(METHOD_TYPE_STRING, "VersEntier",  fn_chaine_vers_entier, 1);
    owl_register_method(METHOD_TYPE_STRING, "ToInteger",   fn_chaine_vers_entier, 1);
    owl_register_method(METHOD_TYPE_STRING, "Val",         fn_val, 1);
    owl_register_method(METHOD_TYPE_STRING, "VersReel",    fn_chaine_vers_reel,   1);
    owl_register_method(METHOD_TYPE_STRING, "ToReal",      fn_chaine_vers_reel,   1);
    /* Encoding */
    owl_register_method(METHOD_TYPE_STRING, "VersBase64",  fn_string_to_base64,   1);
    owl_register_method(METHOD_TYPE_STRING, "ToBase64",    fn_string_to_base64,   1);
    owl_register_method(METHOD_TYPE_STRING, "VersHexa",    fn_string_to_hex,      1);
    owl_register_method(METHOD_TYPE_STRING, "ToHex",       fn_string_to_hex,      1);
    owl_register_method(METHOD_TYPE_STRING, "URLEncode",   fn_url_encode,         1);
    owl_register_method(METHOD_TYPE_STRING, "URLDecode",   fn_url_decode,         1);

    /* VersChaine on numeric types (NumeriqueVersChaine) */
    owl_register_method(METHOD_TYPE_INTEGER, "VersChaine", fn_chaine_formate, 1);
    owl_register_method(METHOD_TYPE_INTEGER, "ToString",   fn_chaine_formate, 1);
    owl_register_method(METHOD_TYPE_REAL,    "VersChaine", fn_chaine_formate, 1);
    owl_register_method(METHOD_TYPE_REAL,    "ToString",   fn_chaine_formate, 1);

    /* Taille/Length also works on arrays and collections */
    owl_register_method(METHOD_TYPE_ARRAY,            "Taille", fn_taille, 1);
    owl_register_method(METHOD_TYPE_ARRAY,            "Length", fn_taille, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "Taille", fn_taille, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "Length", fn_taille, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "Taille", fn_taille, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "Length", fn_taille, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST,  "Taille", fn_taille, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST,  "Length", fn_taille, 1);

    /* New string extension methods */
    /*  — bindings -1 : ces natives ont des formes {2,3} (options
     * optionnel). L'arité est validée par les signatures (compile-time) +
     * la garde impl, pas par l'arité figée du binding. */
    owl_register_method(METHOD_TYPE_STRING, "CommencePar",   fn_commence_par,   -1);
    owl_register_method(METHOD_TYPE_STRING, "StartsWith",    fn_commence_par,   -1);
    owl_register_method(METHOD_TYPE_STRING, "FinitPar",      fn_finit_par,      -1);
    owl_register_method(METHOD_TYPE_STRING, "EndsWith",      fn_finit_par,      -1);
    owl_register_method(METHOD_TYPE_STRING, "Contient",      fn_contient,       -1);
    owl_register_method(METHOD_TYPE_STRING, "Contains",      fn_contient,       -1);
    owl_register_method(METHOD_TYPE_STRING, "Complet",       fn_complet,        -1);
    owl_register_method(METHOD_TYPE_STRING, "Complete",      fn_complet,        -1);
    owl_register_method(METHOD_TYPE_STRING, "Repete",        fn_repete_chaine,   2);
    owl_register_method(METHOD_TYPE_STRING, "RepeatString", fn_repete_chaine,   2);
    owl_register_method(METHOD_TYPE_STRING, "Inverse",       fn_inverse_chaine,  1);
    owl_register_method(METHOD_TYPE_STRING, "Reverse",       fn_inverse_chaine,  1);
    owl_register_method(METHOD_TYPE_STRING, "Decoupe",       fn_chaine_decoupe, -1);
    owl_register_method(METHOD_TYPE_STRING, "Split",         fn_chaine_decoupe, -1);
    owl_register_method(METHOD_TYPE_STRING, "Asc",           fn_asc,             1);
    owl_register_method(METHOD_TYPE_STRING, "CharCode",      fn_asc,             1);

    /* CSV methods on arrays */
    owl_register_method(METHOD_TYPE_ARRAY, "VersCSV",        fn_tableau_vers_csv, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "ToCSV",          fn_tableau_vers_csv, -1);

    /*  : enregistre g_xc_src (curseur XML traversal) au registre
     * unique des racines persistantes. */
    owl_register_persistent_obj_root((Obj**)&g_xc_src);
}

void stdlib_string_reset_globals(void) {
    g_xc_src = NULL;
    g_xc_pos = NULL;
    g_xc_reverse = false;

    /* Etat de parcours PositionOccurrence forme tableau : libere le buffer
     * dynamique et remet tout a zero (evite fuite + curseur perime). */
    if (g_posocc_arr_positions) {
        FREE_ARRAY(int, g_posocc_arr_positions, g_posocc_arr_cap);
    }
    g_posocc_arr_positions = NULL;
    g_posocc_arr_cap = 0;
    g_posocc_arr_count = 0;
    g_posocc_arr_idx = 0;
    g_posocc_arr_src = NULL;
}
