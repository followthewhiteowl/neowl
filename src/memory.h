// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_memory_h
#define owl_memory_h

#include "common.h"

#define GROW_CAPACITY(capacity) \
    ((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(type, pointer, oldCount, newCount) \
    (type*)owl_realloc(pointer, sizeof(type) * (oldCount), \
                       sizeof(type) * (newCount))

#define FREE_ARRAY(type, pointer, oldCount) \
    owl_realloc(pointer, sizeof(type) * (oldCount), 0)

#define ALLOCATE(type, count) \
    (type*)owl_realloc(NULL, 0, sizeof(type) * (count))

#define FREE(type, pointer) \
    owl_realloc(pointer, sizeof(type), 0)

void* owl_realloc(void* pointer, size_t oldSize, size_t newSize);
void owl_free_all(void);

/*  : comptabilise dans gc_bytesAllocated la memoire allouee HORS
 * owl_realloc (les buffers double[] des Tenseurs Vecteur/Matrice, malloc
 * bruts). Sans ca le GC ne voit que les en-tetes Obj (dizaines d'octets) et
 * jamais les Go de doubles → le declencheur ne franchit jamais le seuil.
 * NE declenche PAS gc_collect (appelee depuis le site de malloc brut ou l'objet
 * n'est pas encore enracine : collecter la serait GC-unsafe). Le prochain
 * owl_realloc collectera au bon moment avec le compteur a jour. delta<0 est
 * borne contre l'underflow comme dans owl_realloc. */
void owl_gc_account_ext(int64_t delta);

/* Safe strdup: exits with OOM error if allocation fails.
 * Use this instead of strdup() everywhere to avoid NULL dereferences. */
char* safe_strdup(const char* s);

/*  : stress-GC toggle runtime. Activé via env OWL_STRESS_GC=1
 * pour la chasse  et le hook pré-commit. Si défini compile-time
 * via DEBUG_STRESS_GC (src/common.h), démarre à true. */
extern bool g_stress_gc;
extern bool g_gc_poison;  /*  : poison-and-leak debug UAF */
void owl_gc_init_stress_from_env(void);

/*  : mode inference « sans gradient » (facon torch.no_grad). Arme par
 * `.Infère` autour de son forward. Sous ce flag, les constructeurs de Tenseur
 * n'allouent PAS de buffer grad et les constructeurs de noeud d'op ne posent NI
 * `op` NI `parents` : chaque intermediaire reste une feuille -> collectable au
 * fil de l'eau, pic memoire d'inference coupe. L'entrainement ne l'arme JAMAIS
 * (il a besoin du graphe backward). Defaut = faux. */
extern bool g_no_grad;

#endif
