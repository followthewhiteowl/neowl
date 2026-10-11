// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_table_h
#define owl_table_h

#include "common.h"
#include "value.h"

/* ================================================================
 * HASH TABLE - Open addressing with linear probing
 * Used for: global variables, string interning, class members
 * ================================================================ */

typedef struct {
    ObjString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
} Table;

void table_init(Table* table);
void table_free(Table* table);
bool table_get(Table* table, ObjString* key, Value* value);
bool table_set(Table* table, ObjString* key, Value value);
bool table_delete(Table* table, ObjString* key);
void table_add_all(Table* from, Table* to);

/* Find a string in the table by raw chars (for string interning) */
ObjString* table_find_string(Table* table, const char* chars,
                             int length, uint32_t hash);

/* Find an existing key matching `chars` case-insensitively (linear scan).
 * Keys are already accent-stripped at compile time, so an ASCII strcasecmp
 * covers invariants 1 & 2 for member access on user-defined names. Returns
 * the stored ObjString* key (so callers can reuse the canonical slot for
 * read or write), or NULL on miss. */
ObjString* table_find_key_case_insensitive(Table* table, const char* chars);

/* GC: remove entries whose key is an unmarked (white) string */
void table_remove_white(Table* table);

#endif
