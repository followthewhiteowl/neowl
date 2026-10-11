// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_mock_h
#define owl_mock_h

#include "common.h"
#include "value.h"
#include "object.h"

/* ================================================================
 * MOCK SYSTEM - Universal native function interception
 * ================================================================
 *
 * Architecture:
 * - MockRegistry: global registry of active mocks
 * - MockNative: one entry per mocked native function
 * - MockRule: matching patterns + action to execute
 * - Injection point: vm.c call_value() before native invocation
 *
 * Usage:
 *   mock := MockCree("HTTPRequete")
 *   MockQuand(mock).Avec(url := Commence("http://")).Retourne("response")
 *   MockActive(mock)
 *   // ... tests ...
 *   MockDesactive(mock)
 */

#define MAX_MOCKS 64
#define MAX_RULES_PER_MOCK 32
#define MAX_PATTERNS_PER_RULE 8

/* ================================================================
 * PATTERN TYPES - For argument matching
 * ================================================================ */

typedef enum {
    PATTERN_EXACT,      /* arg == compareValue */
    PATTERN_STARTS,     /* string starts with compareValue */
    PATTERN_CONTAINS,   /* string contains compareValue */
    PATTERN_ENDS,       /* string ends with compareValue */
    PATTERN_ANY,        /* matches anything */
    PATTERN_REGEX,      /* regex match (phase 2) */
} PatternType;

typedef struct {
    PatternType type;
    Value compareValue;  /* The value to compare against (for EXACT, STARTS, etc.) */
    char* regexPattern;  /* For PATTERN_REGEX (NULL otherwise) */
} ArgPattern;

/* ================================================================
 * MOCK ACTION TYPES - What to do when a rule matches
 * ================================================================ */

typedef enum {
    MOCK_RETURN_VALUE,      /* Return a fixed value */
    MOCK_RETURN_SEQUENCE,   /* Return values from a sequence (cycles through) */
    MOCK_CALLBACK,          /* Call a custom function */
    MOCK_THROW,             /* Throw an exception */
} MockActionType;

typedef struct {
    MockActionType type;

    /* Return value (for RETURN_VALUE) */
    Value returnValue;

    /* Sequence (for RETURN_SEQUENCE) */
    Value* sequence;
    int sequenceLen;
    int sequenceIndex;  /* Current position in sequence */

    /* Callback (for CALLBACK) */
    NativeFn callback;

    /* Exception (for THROW) */
    ObjString* exceptionMsg;

    /* HTTP-specific state (optional, for HTTPRequete mocks) */
    int httpStatus;      /* 0 = not set, otherwise HTTP status code */
    Table httpHeaders;   /* Simulated HTTP headers (ObjString* -> ObjString*) */
} MockAction;

/* ================================================================
 * MOCK RULE - Pattern + Action
 * ================================================================ */

typedef struct {
    ArgPattern patterns[MAX_PATTERNS_PER_RULE];
    int patternCount;
    MockAction action;
    int matchCount;  /* Spy counter: how many times this rule matched */
} MockRule;

/* ================================================================
 * MOCK NATIVE - One entry per mocked function
 * ================================================================ */

typedef struct {
    ObjString* nativeName;   /* Name of the mocked function */
    NativeFn originalFn;     /* Pointer to original implementation */
    int originalArity;       /* Original arity (-1 = variadic) */
    MockRule rules[MAX_RULES_PER_MOCK];
    int ruleCount;
    bool isActive;           /* Whether this mock is currently active */
    int totalCalls;          /* Spy counter: total calls to this mock */

    /* Arguments capturés du DERNIER appel intercepté. Heap-alloués (owl_realloc,
     * suivi GC) et marqués via mock_mark_roots() : une valeur composée capturée
     * (httpRequête, JSON…) doit survivre au-delà de l'appel tant que le mock la
     * référence. lastArgCount = arité du dernier appel, 0 si aucun. */
    Value* lastArgs;
    int lastArgCount;
    int lastArgCapacity;
} MockNative;

/* ================================================================
 * MOCK REGISTRY - Global registry
 * ================================================================ */

typedef struct {
    MockNative mocks[MAX_MOCKS];
    int mockCount;
    bool anyActive;  /* Fast path: true if any mock is active */
} MockRegistry;

/* Global mock registry */
extern MockRegistry g_mockRegistry;

/* ================================================================
 * REGISTRY API
 * ================================================================ */

/* Initialize the mock registry */
void mock_registry_init(void);

/* Free all mock data */
void mock_registry_free(void);

/* Create a new mock entry for a native function.
 * Returns the index in the registry, or -1 on error. */
int mock_registry_create(ObjString* name, NativeFn originalFn, int arity);

/* Find an active mock by its original function pointer.
 * Returns NULL if not found or not active. */
MockNative* mock_registry_find_active(NativeFn fn);

/* Get mock by index (for ObjMockNative to reference) */
MockNative* mock_registry_get(int index);

/* ================================================================
 * INTERCEPTION API - Called from vm.c
 * ================================================================ */

/* Try to intercept a native function call.
 * Returns true if intercepted (outResult contains the mock result).
 * Returns false if no active mock found (call original function). */
bool mock_try_intercept(ObjNative* native, int argCount, Value* args, Value* outResult);

/* Same as mock_try_intercept but takes the raw NativeFn pointer directly.
 * Used by the method-dispatch path (OP_INVOKE/OP_INVOKE_SAFE) which only has
 * the bound NativeFn, not the ObjNative wrapper. */
bool mock_try_intercept_fn(NativeFn fn, int argCount, Value* args, Value* outResult);

/* ================================================================
 * RULE MATCHING
 * ================================================================ */

/* Check if a rule matches the given arguments */
bool mock_rule_matches(MockRule* rule, int argCount, Value* args);

/* Check if a single pattern matches a value */
bool mock_pattern_matches(ArgPattern* pattern, Value arg);

/* Execute a rule's action and return the result */
Value mock_rule_execute(MockNative* mock, MockRule* rule, int argCount, Value* args);

/* ================================================================
 * GC SUPPORT
 * ================================================================ */

/* Mark all Values in the mock registry (for GC) */
void mock_mark_roots(void);

#endif
