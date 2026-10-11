// SPDX-License-Identifier: AGPL-3.0-only
#ifndef owl_lsp_catalog_h
#define owl_lsp_catalog_h

/* ================================================================
 * NATIVE FUNCTION CATALOG
 * Now uses the unified catalog from owl_catalog.h.
 * This file provides aliases and the keyword catalog.
 * ================================================================ */

#include "../src/owl_catalog.h"

/* Alias: LSP code uses NativeInfo, which is now OwlNativeInfo */
typedef OwlNativeInfo NativeInfo;

#define NATIVE_CATALOG      OWL_NATIVE_CATALOG
#define NATIVE_CATALOG_COUNT OWL_NATIVE_CATALOG_COUNT

/* Keyword documentation */
typedef struct {
    const char* keyword;
    const char* syntax;       /* Usage syntax example */
    const char* description;  /* FR description */
} KeywordInfo;

extern const KeywordInfo KEYWORD_CATALOG[];
extern const int KEYWORD_CATALOG_COUNT;

/* Lookup by name (case-insensitive) */
const NativeInfo* catalog_find_native(const char* name);
const KeywordInfo* catalog_find_keyword(const char* name);

#endif
