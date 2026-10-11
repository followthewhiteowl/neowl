// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "memory.h"
#include "gc.h"
#include <string.h>

/*  : toggle runtime du stress-GC. Permet d'activer le mode via
 * env var OWL_STRESS_GC=1 sans rebuild — utilisé par le hook pré-commit
 * et la chasse aux bugs  occasionnelle. Si la macro
 * DEBUG_STRESS_GC compile-time reste définie (cas debug strict), le
 * toggle démarre déjà à true. Coût branche `if (g_stress_gc)` négligeable
 * vs cost du heap allocator lui-même. */
#ifdef DEBUG_STRESS_GC
bool g_stress_gc = true;
#else
bool g_stress_gc = false;
#endif

/*  : poison-and-leak debug toggle (OWL_GC_POISON=1).
 * Memset 0xDD sur tout bloc libere puis LEAK (pas de free system) :
 * tout use-after-free crashe instantanement sur un Obj poisonne au
 * lieu de fluctuer selon la loterie ASLR. Cout memoire enorme,
 * exclusivement debug Heisenbug stress-GC, jamais en prod. */
bool g_gc_poison = false;

/*  : mode inference sans gradient (cf. memory.h). Toggle interne, pose
 * uniquement autour du forward de `.Infère` (sauvegarde/restauration exception-
 * safe cote appelant). Pas de pilotage par env : c'est un etat transitoire du
 * chemin inference, pas un mode de debug. */
bool g_no_grad = false;

void owl_gc_init_stress_from_env(void) {
    const char* env = getenv("OWL_STRESS_GC");
    if (env != NULL && env[0] != '\0' && env[0] != '0') {
        g_stress_gc = true;
        fprintf(stderr, "[owl] stress-GC actif (OWL_STRESS_GC=%s)\n", env);
    }
    const char* poison = getenv("OWL_GC_POISON");
    if (poison != NULL && poison[0] != '\0' && poison[0] != '0') {
        g_gc_poison = true;
        fprintf(stderr, "[owl] GC poison-and-leak actif (OWL_GC_POISON=%s) -- "
                        "UAF crashe instantanement, fuite memoire intentionnelle\n",
                poison);
    }
}

void* owl_realloc(void* pointer, size_t oldSize, size_t newSize) {
    /* Track net allocation change (safely handle underflow) */
    if (newSize > oldSize) {
        gc_bytesAllocated += (newSize - oldSize);
    } else {
        size_t freed = oldSize - newSize;
        if (freed > gc_bytesAllocated) {
            gc_bytesAllocated = 0;
        } else {
            gc_bytesAllocated -= freed;
        }
    }

    if (newSize > oldSize) {
        if (g_stress_gc) {
            gc_collect();
        }
        if (gc_bytesAllocated > gc_nextGC) {
            gc_collect();
        }
    }

    if (newSize == 0) {
        /*  : poison-and-leak. Ecrit 0xDD sur toute la zone
         * et NE LIBERE PAS. Toute UAF lit du 0xDD = type Obj invalide,
         * pointeurs invalides → segfault deterministe + signature
         * reconnaissable (0xDDDDDDDD dans cores/disassembly). */
        if (g_gc_poison && pointer != NULL && oldSize > 0) {
            memset(pointer, 0xDD, oldSize);
            return NULL;
        }
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        fprintf(stderr, "owl: out of memory\n");
        exit(74);
    }
    return result;
}

void owl_gc_account_ext(int64_t delta) {
    if (delta >= 0) {
        gc_bytesAllocated += (size_t)delta;
    } else {
        size_t freed = (size_t)(-delta);
        if (freed > gc_bytesAllocated) {
            gc_bytesAllocated = 0;
        } else {
            gc_bytesAllocated -= freed;
        }
    }
}

char* safe_strdup(const char* s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)malloc(len);
    if (copy == NULL) {
        fprintf(stderr, "owl: out of memory in strdup\n");
        exit(74);
    }
    memcpy(copy, s, len);
    return copy;
}
