// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_lsp_json_h
#define owl_lsp_json_h

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Minimal JSON parser/builder for LSP JSON-RPC.
 * Only supports the subset needed by LSP: objects, arrays, strings,
 * integers, booleans, null. No floats needed.
 * ================================================================ */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_INT,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonPair JsonPair;

struct JsonPair {
    char* key;
    JsonValue* value;
};

struct JsonValue {
    JsonType type;
    union {
        bool boolVal;
        int64_t intVal;
        struct { char* str; int len; } stringVal;
        struct { JsonValue** items; int count; int capacity; } arrayVal;
        struct { JsonPair* pairs; int count; int capacity; } objectVal;
    } as;
};

/* Parsing */
JsonValue* json_parse(const char* source, int length);

/* Access helpers */
JsonValue* json_get(JsonValue* obj, const char* key);
const char* json_get_string(JsonValue* obj, const char* key);
int64_t json_get_int(JsonValue* obj, const char* key, int64_t defaultVal);
bool json_get_bool(JsonValue* obj, const char* key, bool defaultVal);

/* Building */
JsonValue* json_new_object(void);
JsonValue* json_new_array(void);
JsonValue* json_new_string(const char* str);
JsonValue* json_new_int(int64_t val);
JsonValue* json_new_bool(bool val);
JsonValue* json_new_null(void);
void json_object_set(JsonValue* obj, const char* key, JsonValue* value);
void json_array_push(JsonValue* arr, JsonValue* value);

/* Serialization */
char* json_serialize(JsonValue* value, int* outLen);

/* Cleanup */
void json_free(JsonValue* value);

#endif
