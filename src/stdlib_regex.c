// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL regex stdlib: PCRE2-backed regular expressions, canon.
 *
 * Functions:
 *   RegexVerifie(subject, pattern, [flags])           -> bool     (tests match)
 *   RegexCherche(source, format, &tab[, ...])         -> entier   (count, fills tab)
 *   RegexRemplace(subj, pat, repl, [flags])           -> chaine   (global replace)
 *
 * Flags string: any combination of:
 *   'i' - case-insensitive (PCRE2_CASELESS)
 *   'm' - multiline (^ and $ match at newlines)
 *   's' - dotall (. matches newline)
 *   'x' - extended (whitespace and comments ignored)
 *   'u' - UTF-8 mode
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#ifndef PCRE2_STATIC
#define PCRE2_STATIC
#endif
#include "pcre2.h"

/* ================================================================
 * HELPERS
 * ================================================================ */

/* Parse flags string -> PCRE2 options. Returns 0 on unknown flag. */
static uint32_t parse_flags(const char* s, int len, int* ok) {
    uint32_t opts = 0;
    *ok = 1;
    for (int i = 0; i < len; i++) {
        switch (s[i]) {
            case 'i': case 'I': opts |= PCRE2_CASELESS;  break;
            case 'm': case 'M': opts |= PCRE2_MULTILINE; break;
            case 's': case 'S': opts |= PCRE2_DOTALL;    break;
            case 'x': case 'X': opts |= PCRE2_EXTENDED;  break;
            case 'u': case 'U': opts |= PCRE2_UTF;       break;
            default: *ok = 0; return 0;
        }
    }
    return opts;
}

/* Compile a pattern. Caller must pcre2_code_free(). NULL on error. Sets the
 * soft error state with a descriptive PCRE2 message on failure. */
static pcre2_code* compile_pattern(ObjString* pat, uint32_t opts) {
    int err_code;
    PCRE2_SIZE err_off;
    pcre2_code* re = pcre2_compile((PCRE2_SPTR)pat->chars, (PCRE2_SIZE)pat->length,
                                   opts, &err_code, &err_off, NULL);
    if (re == NULL) {
        PCRE2_UCHAR msgbuf[256];
        pcre2_get_error_message(err_code, msgbuf, sizeof(msgbuf));
        char detail[384];
        snprintf(detail, sizeof(detail), "pattern invalide a la position %d: %s",
                 (int)err_off, (const char*)msgbuf);
        owl_emit_native_error(E_NATIVE_REGEX_ERROR, "RegEx",
                              "native", "RegEx",
                              "detail", detail,
                              NULL);
    }
    return re;
}

/* Extract optional flags arg at position `idx` if present and a string.
 * Returns PCRE2 options. Sets *ok=0 on parse error. */
static uint32_t read_flags_arg(int argCount, Value* args, int idx, int* ok) {
    *ok = 1;
    if (argCount <= idx) return 0;
    if (!IS_STRING(args[idx])) return 0;
    ObjString* f = AS_STRING(args[idx]);
    return parse_flags(f->chars, f->length, ok);
}

/* ================================================================
 * RegexVerifie - boolean test, out-params variadic pour groupes capturés
 *
 * Signatures canon :
 *   RegexVerifie(s, motif)                  -> bool
 *   RegexVerifie(s, motif, &g1 [, &g2, ...]) -> bool, remplit groupes
 *
 * Les flags inline restent dans le pattern (ex: "(?i)abc"). Cf VISION
 * §5bis "Out-params variadic".
 * ================================================================ */

static Value fn_regex(int argCount, Value* args) {
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);

    ObjString* subj = AS_STRING(args[0]);
    pcre2_code* re = compile_pattern(AS_STRING(args[1]), 0);
    if (!re) return BOOL_VAL(false);
    owl_clear_error();

    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)subj->chars, (PCRE2_SIZE)subj->length,
                         0, 0, md, NULL);

    bool matched = (rc > 0);

    /* Out-params : groupes capturés -> args[2..] (1-based PCRE: groupe k
     * dans ov[2k], ov[2k+1]). Si pas de match, args[i] reçoit "". */
    if (argCount > 2) {
        PCRE2_SIZE* ov = matched ? pcre2_get_ovector_pointer(md) : NULL;
        for (int i = 2; i < argCount; i++) {
            int k = i - 1;  /* groupe 1, 2, 3... */
            ObjString* captured;
            if (matched && k < rc) {
                PCRE2_SIZE s = ov[2 * k], e = ov[2 * k + 1];
                if (s == PCRE2_UNSET || e == PCRE2_UNSET) {
                    captured = obj_copy_string("", 0);
                } else {
                    captured = obj_copy_string(subj->chars + s, (int)(e - s));
                }
            } else {
                captured = obj_copy_string("", 0);
            }
            args[i] = OBJ_VAL(captured);
        }
    }

    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return BOOL_VAL(matched);
}

/* ================================================================
 * RegexCherche(source, format, &tabFull [, &tabGrp1, &tabGrp2, ..., &tabGrpN])
 *   - Remplit tabFull avec tous les matchs complets.
 *   - Si plus de tableaux passes : tabGrpK recoit les K-emes groupes
 *     captures (parentheses dans le pattern), pour chaque match.
 *   - Retourne le nombre total de matchs.
 * Doc 1000026274 (v27+).
 * ================================================================ */

static int fill_matches_with_groups(ObjString* subj, ObjString* pat,
                                     ObjArray** outArrays, int outCount) {
    /* outArrays[0] = matchs complets, outArrays[1..N] = groupes 1..N */
    for (int i = 0; i < outCount; i++) outArrays[i]->elements.count = 0;

    pcre2_code* re = compile_pattern(pat, 0);
    if (!re) return 0;

    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
    int total = 0;
    PCRE2_SIZE offset = 0;
    while (offset <= (PCRE2_SIZE)subj->length) {
        int rc = pcre2_match(re, (PCRE2_SPTR)subj->chars,
                             (PCRE2_SIZE)subj->length,
                             offset, 0, md, NULL);
        if (rc <= 0) break;
        PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);

        /* Match complet (groupe 0) -> outArrays[0] */
        if (outCount >= 1) {
            PCRE2_SIZE s = ov[0], e = ov[1];
            Value v = OBJ_VAL(obj_copy_string(subj->chars + s, (int)(e - s)));
            vm_push(v);
            value_array_write(&outArrays[0]->elements, v);
            vm_pop();
        }

        /* Groupes captures 1..N -> outArrays[1..N]. PCRE2 retourne dans
         * ov[2k], ov[2k+1] le k-eme groupe (k=0 = match complet). */
        for (int k = 1; k < outCount; k++) {
            if (k >= rc) {
                /* Groupe non capture par ce match : push "" */
                Value v = OBJ_VAL(obj_copy_string("", 0));
                vm_push(v);
                value_array_write(&outArrays[k]->elements, v);
                vm_pop();
                continue;
            }
            PCRE2_SIZE s = ov[2*k], e = ov[2*k + 1];
            Value v;
            if (s == PCRE2_UNSET || e == PCRE2_UNSET) {
                v = OBJ_VAL(obj_copy_string("", 0));
            } else {
                v = OBJ_VAL(obj_copy_string(subj->chars + s, (int)(e - s)));
            }
            vm_push(v);
            value_array_write(&outArrays[k]->elements, v);
            vm_pop();
        }

        total++;
        PCRE2_SIZE e = ov[1];
        offset = (e == offset) ? e + 1 : e;
    }
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return total;
}

static Value fn_regex_cherche(int argCount, Value* args) {
    if (argCount < 3) return INTEGER_VAL(0);
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return INTEGER_VAL(0);
    /* args[2..N] doivent etre des tableaux. Au moins 1 (matchs complets). */
    int outCount = argCount - 2;
    if (outCount > 32) outCount = 32;  /* safety cap */
    ObjArray* outs[32];
    for (int i = 0; i < outCount; i++) {
        if (!IS_ARRAY(args[2 + i])) return INTEGER_VAL(0);
        outs[i] = AS_ARRAY(args[2 + i]);
    }
    return INTEGER_VAL(fill_matches_with_groups(
        AS_STRING(args[0]), AS_STRING(args[1]), outs, outCount));
}

/* ================================================================
 * RegExRemplace - global substitution
 * Replacement supports $0, $1 ... $N backrefs (PCRE2 default)
 * ================================================================ */

static Value fn_regex_replace(int argCount, Value* args) {
    if (argCount < 3) return argCount > 0 ? args[0] : NIL_VAL;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) return args[0];
    int fok;
    uint32_t opts = read_flags_arg(argCount, args, 3, &fok);
    if (!fok) return args[0];

    ObjString* subj = AS_STRING(args[0]);
    ObjString* repl = AS_STRING(args[2]);
    pcre2_code* re = compile_pattern(AS_STRING(args[1]), opts);
    if (!re) return args[0];

    /* First call to get required output size */
    PCRE2_SIZE out_len = 0;
    uint32_t sub_opts = PCRE2_SUBSTITUTE_GLOBAL
                      | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    int rc = pcre2_substitute(re,
                              (PCRE2_SPTR)subj->chars, (PCRE2_SIZE)subj->length,
                              0, sub_opts, NULL, NULL,
                              (PCRE2_SPTR)repl->chars, (PCRE2_SIZE)repl->length,
                              NULL, &out_len);
    if (rc != PCRE2_ERROR_NOMEMORY && rc < 0) {
        pcre2_code_free(re);
        return args[0];
    }

    char* buf = malloc(out_len + 1);
    if (!buf) { pcre2_code_free(re); return args[0]; }

    sub_opts = PCRE2_SUBSTITUTE_GLOBAL;
    rc = pcre2_substitute(re,
                          (PCRE2_SPTR)subj->chars, (PCRE2_SIZE)subj->length,
                          0, sub_opts, NULL, NULL,
                          (PCRE2_SPTR)repl->chars, (PCRE2_SIZE)repl->length,
                          (PCRE2_UCHAR*)buf, &out_len);
    pcre2_code_free(re);
    if (rc < 0) { free(buf); return args[0]; }

    Value result = OBJ_VAL(obj_copy_string(buf, (int)out_len));
    free(buf);
    return result;
}

/* ================================================================
 * INIT
 * ================================================================ */

/* ================================================================
 * Registry declarations — Regex family (canon only).
 * RegexVérifie, RegexCherche, RegexRemplace.
 * ================================================================ */

static const OwlParam p_verifie[] = {
    {"chaîne", "chaîne", "chaîne à tester"},
    {"motif",  "chaîne", "expression régulière PCRE"},
    {"flags",  "chaîne", "drapeaux optionnels: i (insensible à la casse), s (dot-all), m (multi-ligne)"},
    {0}
};
static const OwlParam p_cherche[] = {
    {"source",  "chaîne",  "chaîne source"},
    {"format",  "chaîne",  "expression régulière PCRE"},
    {"trouvé",  "tableau", "tableau de chaînes rempli avec les sous-chaînes trouvées"},
    {0}
};
static const OwlParam p_remplace[] = {
    {"chaîne",   "chaîne", "chaîne source"},
    {"motif",    "chaîne", "expression régulière"},
    {"remplace", "chaîne", "chaîne de remplacement"},
    {0}
};
static const OwlParam p_remplace_flags[] = {
    {"chaîne",   "chaîne", "chaîne source"},
    {"motif",    "chaîne", "expression régulière"},
    {"remplace", "chaîne", "chaîne de remplacement"},
    {"flags",    "chaîne", "drapeaux optionnels: i (insensible à la casse), s (dot-all), m (multi-ligne)"},
    {0}
};

static const OwlSignature sig_bool[]    = { {-1, p_verifie,  "booléen", "Vrai si correspondance"}, {0} };
static const OwlSignature sig_cherche[] = { {-1, p_cherche,  "entier",  "nombre de correspondances trouvées"}, {0} };
/* Formes discretes {3,4} : active la validation d'arite compile-time. */
static const OwlSignature sig_repl[]    = {
    {3, p_remplace,       "chaîne", "chaîne avec remplacements effectués"},
    {4, p_remplace_flags, "chaîne", "chaîne avec remplacements effectués (avec drapeaux)"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical      = "RegexV\xc3\xa9" "rifie",   /* RegexVérifie */
        .canonicalAscii = "RegexVerifie",
        .nameEN         = "RegexMatch",
        .category       = "Regex",
        .descFR         = "Vérifie si une chaîne correspond à une expression régulière. Forme variadic : args[2..] = variables qui reçoivent les groupes capturés (out-params).",
        .descEN         = "Check whether a string matches a regular expression. Variadic form: args[2..] are variables receiving captured groups (out-params).",
        .signatures     = sig_bool,
        .impl           = fn_regex,
        .flags          = OWL_NAT_OUT_PARAMS,
        .firstOutParamArg = 2,
        .lastOutParamArg  = -1,  /* variadic : tous les args >= 2 sont out (groupes capturés) */
    },
    {
        .canonical  = "RegexCherche",
        .nameEN     = "RegexSearch",
        .category   = "Regex",
        .descFR     = "Cherche toutes les sous-chaînes d'une chaîne correspondant à une expression régulière. Remplit le tableau passé en 3e argument et retourne le nombre de correspondances.",
        .descEN     = "Find all substrings matching a regular expression. Fills the array passed as the 3rd argument and returns the match count.",
        .signatures = sig_cherche,
        .impl       = fn_regex_cherche,
    },
    {
        .canonical  = "RegexRemplace",
        .nameEN     = "RegexReplace",
        .category   = "Regex",
        .descFR     = "Remplace toutes les occurrences d'une expression régulière dans une chaîne.",
        .descEN     = "Replace all matches of a regular expression in a string.",
        .signatures = sig_repl,
        .impl       = fn_regex_replace,
    },
};

void stdlib_regex_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
}
