// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_parallel.h — pool de threads COMPUTE fork-join "parallel-for".
 *
 * Distinct du pool TCP (net_tcp_server.c, I/O) : ce pool ne fait AUCUNE I/O,
 * ne touche AUCUN objet OWL ni etat VM. Les workers n'operent que sur des
 * `double*` bruts fournis par l'appelant. La VM reste mono-thread : ce pool
 * est un fork-join purement numerique, declenche depuis le thread VM (unique
 * appelant) au coeur des kernels chauds (produit matriciel, convolution).
 */
#ifndef OWL_PARALLEL_H
#define OWL_PARALLEL_H

/* Corps applique a un sous-intervalle contigu [i0, i1) de l'espace [0, n). */
typedef void (*OwlParallelBody)(int i0, int i1, void* ctx);

/* Decoupe [0, n) en chunks CONTIGUS deterministes (fonction de n et du nombre
 * de threads uniquement), en assigne un a chaque worker et un au thread
 * appelant, execute `body` sur chacun, puis attend la fin de tous les workers
 * (fork-join) avant de rendre la main.
 *
 * `totalWork` = estimation du volume de calcul (n * cout par item). En dessous
 * du seuil interne, execution SERIE stricte (perf uniquement — le resultat est
 * IDENTIQUE serie/parallele : chaque i est traite par exactement un chunk).
 *
 * Contrat GC/VM : `body` ne doit toucher que de la memoire brute fournie via
 * `ctx` (double*), JAMAIS un objet OWL, une allocation OWL ou un etat VM. */
void owl_parallel_for(int n, long totalWork, OwlParallelBody body, void* ctx);

/* Nombre total de threads du pool (appelant compris, >= 1). 1 = serie stricte.
 * Resout OWL_THREADS (defaut = concurrence materielle plafonnee) a la premiere
 * invocation, sur le thread appelant. */
int owl_parallel_num_threads(void);

#endif /* OWL_PARALLEL_H */
