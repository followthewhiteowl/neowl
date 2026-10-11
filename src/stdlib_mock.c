// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_mock.c - Universal Mock System */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "vm.h"
#include "mock.h"
#include "memory.h"
#include "object.h"

/* Builder state for fluent API */
typedef struct {
    int mockIndex;
    ArgPattern patterns[MAX_PATTERNS_PER_RULE];
    int patternCount;
    bool hasRule;
} MockBuilder;

static MockBuilder g_currentBuilder;

static void builder_reset(void) {
    g_currentBuilder.mockIndex = -1;
    g_currentBuilder.patternCount = 0;
    g_currentBuilder.hasRule = false;
}

/* MockCree - Create a mock.
 *
 * Two accepted forms:
 *   MockCrée("HTTPRequete")   — lookup by string (useful for reflection)
 *   MockCrée(HTTPRequete)     — bare function reference (compile-time checked)
 *
 * The bare form is recommended: the compiler resolves the identifier, so
 * typos fail immediately and the LSP autocompletes. The string form stays
 * for cases where the target name is computed at runtime. */
static Value fn_mock_cree(int argCount, Value* args) {
    if (argCount != 1) return NIL_VAL;

    ObjNative* native = NULL;
    ObjString* name   = NULL;

    if (IS_STRING(args[0])) {
        name = AS_STRING(args[0]);
        Value nativeVal;
        if (!table_get(&vm.globals, name, &nativeVal)) return NIL_VAL;
        if (!IS_OBJ(nativeVal) || OBJ_TYPE(nativeVal) != OBJ_NATIVE) return NIL_VAL;
        native = (ObjNative*)AS_OBJ(nativeVal);
    } else if (IS_OBJ(args[0]) && OBJ_TYPE(args[0]) == OBJ_NATIVE) {
        native = (ObjNative*)AS_OBJ(args[0]);
        /* Recover the canonical display name via the registry. Falls back to
         * "<native>" if the function wasn't registered through OwlNativeDef
         * (shouldn't happen for user-visible functions, but is defensive). */
        const OwlNativeDef* d = owl_registry_find_by_fn(native->function);
        const char* displayName = (d && d->canonical) ? d->canonical : "<native>";
        name = obj_copy_string(displayName, (int)strlen(displayName));
    } else {
        return NIL_VAL;
    }

    int mockIdx = mock_registry_create(name, native->function, native->arity);
    if (mockIdx < 0) return NIL_VAL;

    return OBJ_VAL(obj_new_mock_native(mockIdx));
}

/* MockQuand - Start building a rule */
static Value fn_mock_quand(int argCount, Value* args) {
    if (argCount != 1 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;

    builder_reset();
    g_currentBuilder.mockIndex = AS_MOCK_NATIVE(args[0])->mockIndex;
    g_currentBuilder.hasRule = true;

    return args[0];
}

/* MockAvec - Add patterns */
static Value fn_mock_avec(int argCount, Value* args) {
    if (argCount < 1 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;
    if (!g_currentBuilder.hasRule) return NIL_VAL;

    for (int i = 1; i < argCount && g_currentBuilder.patternCount < MAX_PATTERNS_PER_RULE; i++) {
        ArgPattern pattern;
        Value arg = args[i];

        /* If arg is a 0-arity native (e.g. MockAny without parentheses),
         * call it to materialise the Pattern object it produces. */
        if (IS_OBJ(arg) && OBJ_TYPE(arg) == OBJ_NATIVE) {
            ObjNative* nat = (ObjNative*)AS_OBJ(arg);
            if (nat->arity == 0) {
                arg = nat->function(0, NULL);
            }
        }

        if (IS_PATTERN(arg)) {
            ObjPattern* p = AS_PATTERN(arg);
            pattern.type = (PatternType)p->patternType;
            pattern.compareValue = p->value;
            pattern.regexPattern = NULL;
        } else {
            pattern.type = PATTERN_EXACT;
            pattern.compareValue = arg;
            pattern.regexPattern = NULL;
        }
        g_currentBuilder.patterns[g_currentBuilder.patternCount++] = pattern;
    }

    return args[0];
}

/* MockRetourne - Set return value */
static Value fn_mock_retourne(int argCount, Value* args) {
    if (argCount != 2 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;
    if (!g_currentBuilder.hasRule) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL || mock->ruleCount >= MAX_RULES_PER_MOCK) return NIL_VAL;

    MockRule* rule = &mock->rules[mock->ruleCount++];
    memcpy(rule->patterns, g_currentBuilder.patterns, sizeof(ArgPattern) * g_currentBuilder.patternCount);
    rule->patternCount = g_currentBuilder.patternCount;
    rule->action.type = MOCK_RETURN_VALUE;
    rule->action.returnValue = args[1];
    rule->action.sequence = NULL;
    rule->action.sequenceLen = 0;
    rule->action.sequenceIndex = 0;
    rule->action.callback = NULL;
    rule->action.exceptionMsg = NULL;
    rule->action.httpStatus = 0;
    table_init(&rule->action.httpHeaders);
    rule->matchCount = 0;

    builder_reset();
    return args[0];
}

/* MockRetourneSequence - Set sequence */
static Value fn_mock_retourne_sequence(int argCount, Value* args) {
    if (argCount != 2 || !IS_MOCK_NATIVE(args[0]) || !IS_ARRAY(args[1])) return NIL_VAL;
    if (!g_currentBuilder.hasRule) return NIL_VAL;

    ObjArray* array = AS_ARRAY(args[1]);
    if (array->elements.count == 0) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL || mock->ruleCount >= MAX_RULES_PER_MOCK) return NIL_VAL;

    Value* sequence = ALLOCATE(Value, array->elements.count);
    memcpy(sequence, array->elements.values, sizeof(Value) * array->elements.count);

    MockRule* rule = &mock->rules[mock->ruleCount++];
    memcpy(rule->patterns, g_currentBuilder.patterns, sizeof(ArgPattern) * g_currentBuilder.patternCount);
    rule->patternCount = g_currentBuilder.patternCount;
    rule->action.type = MOCK_RETURN_SEQUENCE;
    rule->action.returnValue = NIL_VAL;
    rule->action.sequence = sequence;
    rule->action.sequenceLen = array->elements.count;
    rule->action.sequenceIndex = 0;
    rule->action.callback = NULL;
    rule->action.exceptionMsg = NULL;
    rule->action.httpStatus = 0;
    table_init(&rule->action.httpHeaders);
    rule->matchCount = 0;

    builder_reset();
    return args[0];
}

/* MockActive
 * Retourne le mock (args[0]) pour permettre le chainage fluent comme
 * `mockInfo = MockCrée(Info).Quand().Active()` où l'appelant veut
 * récupérer la référence MockNative. Avant ce fix, fn retournait NIL,
 * cassant la chaîne et rendant un Null à mockInfo. */
static Value fn_mock_active(int argCount, Value* args) {
    if (argCount != 1 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL) return NIL_VAL;

    mock->isActive = true;
    g_mockRegistry.anyActive = true;

    return args[0];
}

/* MockDesactive — idem, retourne args[0] pour chainage. */
static Value fn_mock_desactive(int argCount, Value* args) {
    if (argCount != 1 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL) return NIL_VAL;

    mock->isActive = false;

    bool anyActive = false;
    for (int i = 0; i < g_mockRegistry.mockCount; i++) {
        if (g_mockRegistry.mocks[i].isActive) {
            anyActive = true;
            break;
        }
    }
    g_mockRegistry.anyActive = anyActive;

    return args[0];
}

/* MockVerifieAppele */
static Value fn_mock_verifie_appele(int argCount, Value* args) {
    if (argCount < 1 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL) return NIL_VAL;

    int expectedCount = -1;
    if (argCount >= 3 && IS_STRING(args[1]) && IS_INTEGER(args[2])) {
        ObjString* paramName = AS_STRING(args[1]);
        if (strcmp(paramName->chars, "fois") == 0 || strcmp(paramName->chars, "times") == 0) {
            expectedCount = (int)AS_INTEGER(args[2]);
        }
    } else if (argCount == 2 && IS_INTEGER(args[1])) {
        expectedCount = (int)AS_INTEGER(args[1]);
    }

    bool success = (expectedCount == -1) ? (mock->totalCalls > 0) : (mock->totalCalls == expectedCount);
    return BOOL_VAL(success);
}

/* MockReinitialise — idem chainable. */
static Value fn_mock_reinitialise(int argCount, Value* args) {
    if (argCount != 1 || !IS_MOCK_NATIVE(args[0])) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL) return NIL_VAL;

    mock->totalCalls = 0;
    for (int i = 0; i < mock->ruleCount; i++) {
        mock->rules[i].matchCount = 0;
        if (mock->rules[i].action.type == MOCK_RETURN_SEQUENCE) {
            mock->rules[i].action.sequenceIndex = 0;
        }
    }

    return args[0];
}

/* MockDernierArgument(mock, indice) — valeur typée du Nᵉ argument (1-based)
 * du DERNIER appel intercepté sur ce mock. La valeur revient dans son type
 * d'origine (chaîne, entier, httpRequête, JSON…) : on la copie telle quelle
 * depuis la capture faite dans mock_try_intercept. Erreur soft (ErreurDetectee)
 * si aucun appel n'a été intercepté ou si l'indice sort des bornes. */
static Value fn_mock_dernier_argument(int argCount, Value* args) {
    if (argCount != 2 || !IS_MOCK_NATIVE(args[0]) || !IS_INTEGER(args[1])) return NIL_VAL;

    MockNative* mock = mock_registry_get(AS_MOCK_NATIVE(args[0])->mockIndex);
    if (mock == NULL) return NIL_VAL;

    if (mock->lastArgCount == 0) {
        owl_emit_native_error(E_NATIVE_MOCK_NO_CALL, "MockDernierArgument", NULL);
        return NIL_VAL;
    }

    int indice = (int)AS_INTEGER(args[1]);
    if (indice < 1 || indice > mock->lastArgCount) {
        char idxBuf[16], maxBuf[16];
        snprintf(idxBuf, sizeof idxBuf, "%d", indice);
        snprintf(maxBuf, sizeof maxBuf, "%d", mock->lastArgCount);
        owl_emit_native_error(E_NATIVE_MOCK_ARG_OOB, "MockDernierArgument",
                              "index", idxBuf, "max", maxBuf, NULL);
        return NIL_VAL;
    }

    return mock->lastArgs[indice - 1];
}

/* Pattern helpers */
static Value fn_commence(int argCount, Value* args) {
    if (argCount != 1) return NIL_VAL;
    return OBJ_VAL(obj_new_pattern(PATTERN_STARTS, args[0]));
}

static Value fn_contient(int argCount, Value* args) {
    if (argCount != 1) return NIL_VAL;
    return OBJ_VAL(obj_new_pattern(PATTERN_CONTAINS, args[0]));
}

static Value fn_termine_par(int argCount, Value* args) {
    if (argCount != 1) return NIL_VAL;
    return OBJ_VAL(obj_new_pattern(PATTERN_ENDS, args[0]));
}

static Value fn_quelconque(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_pattern(PATTERN_ANY, NIL_VAL));
}

/* Registry definitions */
static const OwlParam p_name[] = {{"fonction",
                                   "cha\xc3\xae" "ne|Proc\xc3\xa9" "dure",
                                   "Fonction native \xc3\xa0 mocker : r\xc3\xa9" "f\xc3\xa9" "rence bare (ex: HTTPRequ\xc3\xaa" "te) ou nom string (ex: \"HTTPRequ\xc3\xaa" "te\")"},
                                  {0}};
static const OwlParam p_mock[] = {{"mock", "MockNative", "Le mock"}, {0}};
static const OwlParam p_mock_val[] = {{"mock", "MockNative", ""}, {"val", "tout", ""}, {0}};
static const OwlParam p_mock_arr[] = {{"mock", "MockNative", ""}, {"arr", "tableau", ""}, {0}};
static const OwlParam p_pattern[] = {{"val", "tout", ""}, {0}};
static const OwlParam p_mock_idx[] = {{"mock", "MockNative", "Le mock"},
                                      {"indice", "entier", "Position de l'argument, 1-based"}, {0}};
/* MockVérifieAppelé — formes discrètes. 1 arg (mock seul → au moins
 * un appel), 2 args (mock + nombre attendu), 3 args (mock + "fois"/"times" +
 * nombre). L'impl ne lit jamais au-delà de args[2]. */
static const OwlParam p_verif_n[] = {{"mock", "MockNative", "Le mock"}, {"nombre", "entier", "nombre d'appels attendu"}, {0}};
static const OwlParam p_verif_fn[] = {{"mock", "MockNative", "Le mock"}, {"param", "cha\xc3\xae" "ne", "\"fois\" / \"times\""}, {"nombre", "entier", "nombre d'appels attendu"}, {0}};

static const OwlSignature sig_cree[] = {{1, p_name, "MockNative", ""}, {0}};
static const OwlSignature sig_mock[] = {{1, p_mock, "MockNative", ""}, {0}};
static const OwlSignature sig_avec[] = {{-1, p_mock, "MockNative", ""}, {0}};
static const OwlSignature sig_ret[] = {{2, p_mock_val, "MockNative", ""}, {0}};
static const OwlSignature sig_seq[] = {{2, p_mock_arr, "MockNative", ""}, {0}};
static const OwlSignature sig_void[] = {{1, p_mock, "", ""}, {0}};
static const OwlSignature sig_verif[] = {
    {1, p_mock,     "bool\xc3\xa9" "en", "vrai si le mock a \xc3\xa9" "t\xc3\xa9 appel\xc3\xa9 au moins une fois"},
    {2, p_verif_n,  "bool\xc3\xa9" "en", "vrai si le nombre d'appels correspond exactement"},
    {3, p_verif_fn, "bool\xc3\xa9" "en", "vrai si le nombre d'appels correspond exactement"},
    {0}};
static const OwlSignature sig_dernarg[] = {{2, p_mock_idx, "tout",
    "valeur du N-i\xc3\xa8" "me argument du dernier appel, dans son type d'origine"}, {0}};
static const OwlSignature sig_pattern[] = {{1, p_pattern, "Pattern", ""}, {0}};
static const OwlSignature sig_any[] = {{0, NULL, "Pattern", ""}, {0}};

/* Prefix method bindings on MockNative for fluent API.
 * Each DEF below can have its own bindings block to declare .Method() forms. */
static const OwlMethodBinding MB_QUAND[] = {
    {"MockNative", "Quand", 1}, {"MockNative", "When", 1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_AVEC[] = {
    {"MockNative", "Avec", -1}, {"MockNative", "With", -1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_RETOURNE[] = {
    {"MockNative", "Retourne", 2}, {"MockNative", "Returns", 2}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_RETSEQ[] = {
    {"MockNative", "RetourneSequence", 2}, {"MockNative", "ReturnsSequence", 2}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_ACTIVE[] = {
    {"MockNative", "Active", 1}, {"MockNative", "Activate", 1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_DESACTIVE[] = {
    {"MockNative", "Desactive", 1}, {"MockNative", "Deactivate", 1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_VERIFIE[] = {
    {"MockNative", "VerifieAppele", -1}, {"MockNative", "VerifyCount", -1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_REINIT[] = {
    {"MockNative", "Reinitialise", 1}, {"MockNative", "Reset", 1}, {NULL, NULL, 0}
};
static const OwlMethodBinding MB_DERNARG[] = {
    {"MockNative", "DernierArgument", 2}, {"MockNative", "LastArgument", 2}, {NULL, NULL, 0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical      = "MockCr\xc3\xa9" "e",
        .canonicalAscii = "MockCree",
        .nameEN         = "MockCreate",
        .category   = "Test",
        .descFR     = "Cr\xc3\xa9" "e un mock pour une fonction native. Intercepte n'importe quelle native, y compris bloquantes (ConsoleLit, Avertissement) ou destructives (VersPressePapier, HTTPRequ\xc3\xaa" "te). Cha\xc3\xae" "ner avec .Quand(), .Retourne() ou .RetourneS\xc3\xa9" "quence(), puis .Active().",
        .descEN     = "Create a mock for a native function. Intercepts any native, including blocking ones (ConsoleRead, Warning) or destructive ones (ToClipboard, HTTPRequest). Chain with .When(), .Returns() or .ReturnsSequence(), then .Activate().",
        .signatures = sig_cree,
        .impl       = fn_mock_cree,
    },
    {
        .canonical  = "MockQuand",
        .nameEN     = "MockWhen",
        .category   = "Test",
        .descFR     = "D\xc3\xa9" "marre la construction d'une r\xc3\xa8" "gle.",
        .descEN     = "Start building a rule.",
        .signatures = sig_mock,
        .impl       = fn_mock_quand,
        .methodBindings = MB_QUAND,
    },
    {
        .canonical  = "MockAvec",
        .nameEN     = "MockWith",
        .category   = "Test",
        .descFR     = "Ajoute des patterns d'arguments.",
        .descEN     = "Add argument patterns.",
        .signatures = sig_avec,
        .impl       = fn_mock_avec,
        .methodBindings = MB_AVEC,
    },
    {
        .canonical  = "MockRetourne",
        .nameEN     = "MockReturns",
        .category   = "Test",
        .descFR     = "D\xc3\xa9" "finit la valeur de retour.",
        .descEN     = "Set return value.",
        .signatures = sig_ret,
        .impl       = fn_mock_retourne,
        .methodBindings = MB_RETOURNE,
    },
    {
        .canonical      = "MockRetourneS\xc3\xa9" "quence",
        .canonicalAscii = "MockRetourneSequence",
        .nameEN         = "MockReturnsSequence",
        .category   = "Test",
        .descFR     = "D\xc3\xa9" "finit une s\xc3\xa9" "quence de retour.",
        .descEN     = "Set return sequence.",
        .signatures = sig_seq,
        .impl       = fn_mock_retourne_sequence,
        .methodBindings = MB_RETSEQ,
    },
    {
        .canonical  = "MockActive",
        .nameEN     = "MockActivate",
        .category   = "Test",
        .descFR     = "Active un mock.",
        .descEN     = "Activate a mock.",
        .signatures = sig_void,
        .impl       = fn_mock_active,
        .methodBindings = MB_ACTIVE,
    },
    {
        .canonical      = "MockD\xc3\xa9" "sactive",
        .canonicalAscii = "MockDesactive",
        .nameEN         = "MockDeactivate",
        .category   = "Test",
        .descFR     = "D\xc3\xa9" "sactive un mock.",
        .descEN     = "Deactivate a mock.",
        .signatures = sig_void,
        .impl       = fn_mock_desactive,
        .methodBindings = MB_DESACTIVE,
    },
    {
        .canonical      = "MockV\xc3\xa9" "rifieAppel\xc3\xa9",
        .canonicalAscii = "MockVerifieAppele",
        .nameEN         = "MockVerifyCount",
        .category   = "Test",
        .descFR     = "V\xc3\xa9" "rifie le nombre d'appels.",
        .descEN     = "Verify call count.",
        .signatures = sig_verif,
        .impl       = fn_mock_verifie_appele,
        .methodBindings = MB_VERIFIE,
    },
    {
        .canonical      = "MockR\xc3\xa9" "initialise",
        .canonicalAscii = "MockReinitialise",
        .nameEN         = "MockReset",
        .category   = "Test",
        .descFR     = "R\xc3\xa9" "initialise les compteurs.",
        .descEN     = "Reset counters.",
        .signatures = sig_void,
        .impl       = fn_mock_reinitialise,
        .methodBindings = MB_REINIT,
    },
    {
        .canonical      = "MockDernierArgument",
        .nameEN         = "MockLastArgument",
        .category   = "Test",
        .descFR     = "Retourne la valeur du N-i\xc3\xa8" "me argument (1-based) du dernier appel intercept\xc3\xa9 sur le mock, dans son type d'origine (cha\xc3\xae" "ne, entier, structure, etc.). Erreur si aucun appel n'a \xc3\xa9" "t\xc3\xa9 intercept\xc3\xa9 ou si l'indice est hors bornes.",
        .descEN     = "Return the value of the Nth argument (1-based) of the last intercepted call on the mock, in its original type (string, integer, structure, etc.). Errors if no call was intercepted or the index is out of bounds.",
        .signatures = sig_dernarg,
        .impl       = fn_mock_dernier_argument,
        .methodBindings = MB_DERNARG,
    },
    {
        .canonical  = "MockCommence",
        .nameEN     = "MockStartsWith",
        .category   = "Test",
        .descFR     = "Pattern mock: d\xc3\xa9" "but de cha\xc3\xae" "ne.",
        .descEN     = "Mock pattern: string starts with.",
        .signatures = sig_pattern,
        .impl       = fn_commence,
    },
    {
        .canonical  = "MockContient",
        .nameEN     = "MockContains",
        .category   = "Test",
        .descFR     = "Pattern mock: contient sous-cha\xc3\xae" "ne.",
        .descEN     = "Mock pattern: contains substring.",
        .signatures = sig_pattern,
        .impl       = fn_contient,
    },
    {
        .canonical  = "MockTerminePar",
        .nameEN     = "MockEndsWith",
        .category   = "Test",
        .descFR     = "Pattern mock: fin de cha\xc3\xae" "ne.",
        .descEN     = "Mock pattern: string ends with.",
        .signatures = sig_pattern,
        .impl       = fn_termine_par,
    },
    {
        .canonical  = "MockTout",
        .nameEN     = "MockAny",
        .category   = "Test",
        .descFR     = "Pattern mock: accepte toute valeur.",
        .descEN     = "Mock pattern: accept any value.",
        .signatures = sig_any,
        .impl       = fn_quelconque,
    },
};

void stdlib_mock_init(void) {
    /* Les méthodes prefix (.Quand(), .Avec(), .Retourne(), etc.) sont
     * déclarées via OwlNativeDef.methodBindings sur chaque DEF ci-dessus.
     * owl_registry_register_module les auto-enregistre. */
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
}
