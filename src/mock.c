// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock.h"
#include "vm.h"
#include "memory.h"
#include "table.h"
#include "gc.h"

/* Global mock registry */
MockRegistry g_mockRegistry;

/* ================================================================
 * REGISTRY IMPLEMENTATION
 * ================================================================ */

void mock_registry_init(void) {
    g_mockRegistry.mockCount = 0;
    g_mockRegistry.anyActive = false;

    for (int i = 0; i < MAX_MOCKS; i++) {
        g_mockRegistry.mocks[i].nativeName = NULL;
        g_mockRegistry.mocks[i].originalFn = NULL;
        g_mockRegistry.mocks[i].originalArity = 0;
        g_mockRegistry.mocks[i].ruleCount = 0;
        g_mockRegistry.mocks[i].isActive = false;
        g_mockRegistry.mocks[i].totalCalls = 0;
        g_mockRegistry.mocks[i].lastArgs = NULL;
        g_mockRegistry.mocks[i].lastArgCount = 0;
        g_mockRegistry.mocks[i].lastArgCapacity = 0;
    }
}

void mock_registry_free(void) {
    for (int i = 0; i < g_mockRegistry.mockCount; i++) {
        MockNative* mock = &g_mockRegistry.mocks[i];

        /* Free rules */
        for (int j = 0; j < mock->ruleCount; j++) {
            MockRule* rule = &mock->rules[j];
            MockAction* action = &rule->action;

            /* Free sequence array if allocated */
            if (action->type == MOCK_RETURN_SEQUENCE && action->sequence != NULL) {
                FREE_ARRAY(Value, action->sequence, action->sequenceLen);
                action->sequence = NULL;
            }

            /* Free regex pattern if allocated */
            for (int k = 0; k < rule->patternCount; k++) {
                if (rule->patterns[k].regexPattern != NULL) {
                    FREE_ARRAY(char, rule->patterns[k].regexPattern,
                              strlen(rule->patterns[k].regexPattern) + 1);
                    rule->patterns[k].regexPattern = NULL;
                }
            }

            /* Free HTTP headers table */
            if (action->httpStatus != 0) {
                table_free(&action->httpHeaders);
            }
        }

        /* Free le buffer d'arguments capturés. */
        if (mock->lastArgs != NULL) {
            FREE_ARRAY(Value, mock->lastArgs, mock->lastArgCapacity);
            mock->lastArgs = NULL;
            mock->lastArgCount = 0;
            mock->lastArgCapacity = 0;
        }
    }

    g_mockRegistry.mockCount = 0;
    g_mockRegistry.anyActive = false;
}

int mock_registry_create(ObjString* name, NativeFn originalFn, int arity) {
    if (g_mockRegistry.mockCount >= MAX_MOCKS) {
        return -1;  /* Registry full */
    }

    /* Si un mock existe déjà pour cette native, on RECYCLE le slot mais
     * on remet TOUT à zéro : compteurs, rules, état actif. L'intention
     * d'un appelant qui fait `m = MockCrée(X)` est d'avoir un mock vierge,
     * pas une accumulation des cycles précédents. Avant ce fix, le 2e
     * MockCrée gardait totalCalls de la session précédente — toute
     * assertion `MockVérifieAppelé(m, n)` lisait alors un compteur sale. */
    for (int i = 0; i < g_mockRegistry.mockCount; i++) {
        if (g_mockRegistry.mocks[i].originalFn == originalFn) {
            MockNative* existing = &g_mockRegistry.mocks[i];
            /* Free les regex patterns alloués par d'anciennes rules. */
            for (int r = 0; r < existing->ruleCount; r++) {
                MockRule* rule = &existing->rules[r];
                for (int k = 0; k < rule->patternCount; k++) {
                    if (rule->patterns[k].type == PATTERN_REGEX &&
                        rule->patterns[k].regexPattern != NULL) {
                        FREE_ARRAY(char, rule->patterns[k].regexPattern,
                                  strlen(rule->patterns[k].regexPattern) + 1);
                        rule->patterns[k].regexPattern = NULL;
                    }
                }
                /* Free HTTP headers table si présents. */
                if (rule->action.httpStatus != 0) {
                    table_free(&rule->action.httpHeaders);
                }
            }
            existing->ruleCount   = 0;
            existing->isActive    = false;
            existing->totalCalls  = 0;
            /* Repart vierge : oublie les args du cycle précédent. Le buffer
             * heap est conservé (réutilisé au prochain appel), seul le compte
             * retombe à 0 pour que mock_mark_roots n'itère plus d'entrées
             * périmées. */
            existing->lastArgCount = 0;
            /* Recalcule anyActive (on vient peut-être de désactiver le seul
             * mock actif). */
            bool anyActive = false;
            for (int j = 0; j < g_mockRegistry.mockCount; j++) {
                if (g_mockRegistry.mocks[j].isActive) { anyActive = true; break; }
            }
            g_mockRegistry.anyActive = anyActive;
            return i;
        }
    }

    int index = g_mockRegistry.mockCount++;
    MockNative* mock = &g_mockRegistry.mocks[index];

    mock->nativeName = name;
    mock->originalFn = originalFn;
    mock->originalArity = arity;
    mock->ruleCount = 0;
    mock->isActive = false;
    mock->totalCalls = 0;
    mock->lastArgs = NULL;
    mock->lastArgCount = 0;
    mock->lastArgCapacity = 0;

    return index;
}

MockNative* mock_registry_find_active(NativeFn fn) {
    /* Fast path: if no mocks are active, skip search */
    if (!g_mockRegistry.anyActive) {
        return NULL;
    }

    for (int i = 0; i < g_mockRegistry.mockCount; i++) {
        MockNative* mock = &g_mockRegistry.mocks[i];
        if (mock->isActive && mock->originalFn == fn) {
            return mock;
        }
    }

    return NULL;
}

MockNative* mock_registry_get(int index) {
    if (index < 0 || index >= g_mockRegistry.mockCount) {
        return NULL;
    }
    return &g_mockRegistry.mocks[index];
}

/* ================================================================
 * INTERCEPTION
 * ================================================================ */

bool mock_try_intercept(ObjNative* native, int argCount, Value* args, Value* outResult) {
    return mock_try_intercept_fn(native->function, argCount, args, outResult);
}

bool mock_try_intercept_fn(NativeFn fn, int argCount, Value* args, Value* outResult) {
    /* Fast path: if no mocks active, return immediately */
    if (!g_mockRegistry.anyActive) {
        return false;
    }

    /* Find active mock for this native */
    MockNative* mock = mock_registry_find_active(fn);
    if (mock == NULL) {
        return false;  /* No mock for this native */
    }

    /* Increment total call counter */
    mock->totalCalls++;

    /* Capture les arguments de CE (donc du dernier) appel intercepté, quel
     * que soit le résultat du matching de règles. Le buffer croît si l'arité
     * dépasse la capacité courante ; il n'est jamais rétréci (réemploi). Les
     * valeurs sont copiées depuis la pile VM (encore rootée à cet instant) et
     * resteront vivantes via mock_mark_roots. owl_realloc peut déclencher un
     * GC : les args sur la pile et les autres captures sont marqués, donc sûr. */
    if (argCount > mock->lastArgCapacity) {
        int newCap = argCount;
        mock->lastArgs = GROW_ARRAY(Value, mock->lastArgs,
                                    mock->lastArgCapacity, newCap);
        mock->lastArgCapacity = newCap;
    }
    for (int a = 0; a < argCount; a++) {
        mock->lastArgs[a] = args[a];
    }
    mock->lastArgCount = argCount;

    /* Try to match rules in order */
    for (int i = 0; i < mock->ruleCount; i++) {
        MockRule* rule = &mock->rules[i];
        if (mock_rule_matches(rule, argCount, args)) {
            rule->matchCount++;
            *outResult = mock_rule_execute(mock, rule, argCount, args);
            return true;
        }
    }

    /* No rule matched - fall back to original function */
    return false;
}

/* ================================================================
 * PATTERN MATCHING
 * ================================================================ */

bool mock_rule_matches(MockRule* rule, int argCount, Value* args) {
    /* Special case: 0 patterns = wildcard, match ANY arity. Permet
     *   MockCrée(Avertissement)..Quand().Retourne(0).Active()
     * d'intercepter tous les appels quel que soit le nombre d'args.
     * Indispensable pour les natives variadiques (Info, Avertissement,
     * Erreur en mode canon : `Avertissement(msg)`, `Avertissement(msg1, msg2)`,
     * `Avertissement(titre, m1, m2)` — tous matchent une seule regle. */
    if (rule->patternCount == 0) {
        return true;
    }

    /* Strict arity check sinon : pattern count doit egaler arg count.
     * Permet de mocker selectivement par arite avec plusieurs regles. */
    if (rule->patternCount != argCount) {
        return false;
    }

    /* Check each pattern */
    for (int i = 0; i < rule->patternCount; i++) {
        if (!mock_pattern_matches(&rule->patterns[i], args[i])) {
            return false;
        }
    }

    return true;
}

/* Helper: check if string starts with prefix */
static bool str_starts_with(const char* str, const char* prefix) {
    size_t prefixLen = strlen(prefix);
    size_t strLen = strlen(str);
    if (strLen < prefixLen) return false;
    return strncmp(str, prefix, prefixLen) == 0;
}

/* Helper: check if string ends with suffix */
static bool str_ends_with(const char* str, const char* suffix) {
    size_t suffixLen = strlen(suffix);
    size_t strLen = strlen(str);
    if (strLen < suffixLen) return false;
    return strcmp(str + strLen - suffixLen, suffix) == 0;
}

bool mock_pattern_matches(ArgPattern* pattern, Value arg) {
    switch (pattern->type) {
        case PATTERN_ANY:
            return true;

        case PATTERN_EXACT:
            return values_equal(arg, pattern->compareValue);

        case PATTERN_STARTS:
            if (!IS_STRING(arg) || !IS_STRING(pattern->compareValue)) {
                return false;
            }
            return str_starts_with(AS_CSTRING(arg), AS_CSTRING(pattern->compareValue));

        case PATTERN_CONTAINS:
            if (!IS_STRING(arg) || !IS_STRING(pattern->compareValue)) {
                return false;
            }
            return strstr(AS_CSTRING(arg), AS_CSTRING(pattern->compareValue)) != NULL;

        case PATTERN_ENDS:
            if (!IS_STRING(arg) || !IS_STRING(pattern->compareValue)) {
                return false;
            }
            return str_ends_with(AS_CSTRING(arg), AS_CSTRING(pattern->compareValue));

        case PATTERN_REGEX:
            /* Phase 2: implement with PCRE2 */
            return false;
    }

    return false;
}

/* ================================================================
 * ACTION EXECUTION
 * ================================================================ */

Value mock_rule_execute(MockNative* mock, MockRule* rule, int argCount, Value* args) {
    MockAction* action = &rule->action;

    /* HTTP-specific: simulate global state if this is HTTPRequete */
    /* TODO: implement via stdlib_http.h API when we add HTTP-specific mock support */
    (void)mock;  /* Suppress unused warning for now */

    /* Execute action based on type */
    switch (action->type) {
        case MOCK_RETURN_VALUE:
            return action->returnValue;

        case MOCK_RETURN_SEQUENCE: {
            if (action->sequenceLen == 0) {
                return NIL_VAL;
            }
            if (action->sequenceIndex >= action->sequenceLen) {
                /* Sequence exhausted - return last element */
                return action->sequence[action->sequenceLen - 1];
            }
            Value result = action->sequence[action->sequenceIndex];
            action->sequenceIndex++;
            return result;
        }

        case MOCK_CALLBACK:
            if (action->callback != NULL) {
                return action->callback(argCount, args);
            }
            return NIL_VAL;

        case MOCK_THROW: {
            /* Trigger OWL exception */
            /* TODO: implement via vm.h API when we add exception mock support */
            (void)args;  /* Suppress unused warning for now */
            return NIL_VAL;
        }
    }

    return NIL_VAL;
}

/* ================================================================
 * GC SUPPORT
 * ================================================================ */

void mock_mark_roots(void) {
    for (int i = 0; i < g_mockRegistry.mockCount; i++) {
        MockNative* mock = &g_mockRegistry.mocks[i];

        /* Mark native name */
        if (mock->nativeName != NULL) {
            gc_mark_object((Obj*)mock->nativeName);
        }

        /* Mark les arguments capturés du dernier appel : les garder vivants
         * tant que le mock les référence (cœur du contrat MockDernierArgument
         * pour les valeurs composées : httpRequête, JSON, tableaux…). */
        for (int a = 0; a < mock->lastArgCount; a++) {
            gc_mark_value(mock->lastArgs[a]);
        }

        /* Mark all rules */
        for (int j = 0; j < mock->ruleCount; j++) {
            MockRule* rule = &mock->rules[j];

            /* Mark pattern values */
            for (int k = 0; k < rule->patternCount; k++) {
                gc_mark_value(rule->patterns[k].compareValue);
            }

            /* Mark action values */
            MockAction* action = &rule->action;
            switch (action->type) {
                case MOCK_RETURN_VALUE:
                    gc_mark_value(action->returnValue);
                    break;

                case MOCK_RETURN_SEQUENCE:
                    for (int k = 0; k < action->sequenceLen; k++) {
                        gc_mark_value(action->sequence[k]);
                    }
                    break;

                case MOCK_THROW:
                    if (action->exceptionMsg != NULL) {
                        gc_mark_object((Obj*)action->exceptionMsg);
                    }
                    break;

                default:
                    break;
            }

            /* Mark HTTP headers table */
            if (action->httpStatus != 0) {
                Table* headers = &action->httpHeaders;
                for (int k = 0; k < headers->capacity; k++) {
                    Entry* entry = &headers->entries[k];
                    if (entry->key != NULL) {
                        gc_mark_object((Obj*)entry->key);
                        gc_mark_value(entry->value);
                    }
                }
            }
        }
    }
}
