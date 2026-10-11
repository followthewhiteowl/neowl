// SPDX-License-Identifier: AGPL-3.0-only
/* Simplified memory allocator for owl-lsp.
 * No GC needed -- the LSP server uses standard malloc/free. */
#include "../src/common.h"

/* Stub: gc globals referenced by memory.h macros */
size_t gc_bytesAllocated = 0;
size_t gc_nextGC = 1024 * 1024;

void* owl_realloc(void* pointer, size_t oldSize, size_t newSize) {
    (void)oldSize;
    if (newSize == 0) {
        free(pointer);
        return NULL;
    }
    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        fprintf(stderr, "owl-lsp: out of memory\n");
        exit(74);
    }
    return result;
}
