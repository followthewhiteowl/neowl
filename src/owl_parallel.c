// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_parallel.c — pool de threads COMPUTE fork-join "parallel-for".
 *
 * Modele : N-1 threads workers PERSISTANTS + le thread appelant. Chaque
 * owl_parallel_for decoupe [0, n) en N chunks CONTIGUS deterministes
 * (fonction de n et N seulement), en assigne un a chaque worker et un a
 * l'appelant, puis l'appelant execute son chunk et attend la fin de tous les
 * workers (join logique via variable de condition). Comme chaque indice i est
 * traite par exactement un chunk et que l'ordre d'accumulation INTERNE a un i
 * est preserve par les kernels, le resultat est bit-a-bit identique quel que
 * soit OWL_THREADS.
 *
 * Isolation VM/GC : les workers n'executent qu'un corps numerique sur des
 * `double*` bruts. Aucun Obj*, aucune allocation OWL, aucun etat VM ne franchit
 * la frontiere de thread. La VM reste mono-thread ; ce pool est un simple
 * fork-join declenche depuis le thread VM (unique appelant, jamais reentrant :
 * aucun corps de kernel n'appelle owl_parallel_for a son tour).
 *
 * Abstraction OS : #ifdef _WIN32 (CONDITION_VARIABLE + CRITICAL_SECTION,
 * CreateThread) / sinon pthread. API OS uniquement -> zero dependance nouvelle,
 * binaire statique preserve. */

#ifdef _WIN32
  /* CONDITION_VARIABLE / InitializeConditionVariable sont apparus avec Vista :
   * doit preceder l'include windows.h. */
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
#endif

#include "owl_parallel.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  typedef CRITICAL_SECTION   par_mutex_t;
  typedef CONDITION_VARIABLE par_cond_t;
  typedef HANDLE             par_thread_t;
  static void par_mutex_init(par_mutex_t* m) { InitializeCriticalSection(m); }
  static void par_lock(par_mutex_t* m)       { EnterCriticalSection(m); }
  static void par_unlock(par_mutex_t* m)     { LeaveCriticalSection(m); }
  static void par_cond_init(par_cond_t* c)   { InitializeConditionVariable(c); }
  static void par_wait(par_cond_t* c, par_mutex_t* m) {
      SleepConditionVariableCS(c, m, INFINITE);
  }
  static void par_signal(par_cond_t* c)      { WakeConditionVariable(c); }
  static void par_broadcast(par_cond_t* c)   { WakeAllConditionVariable(c); }
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_mutex_t par_mutex_t;
  typedef pthread_cond_t  par_cond_t;
  typedef pthread_t       par_thread_t;
  static void par_mutex_init(par_mutex_t* m) { pthread_mutex_init(m, NULL); }
  static void par_lock(par_mutex_t* m)       { pthread_mutex_lock(m); }
  static void par_unlock(par_mutex_t* m)     { pthread_mutex_unlock(m); }
  static void par_cond_init(par_cond_t* c)   { pthread_cond_init(c, NULL); }
  static void par_wait(par_cond_t* c, par_mutex_t* m) { pthread_cond_wait(c, m); }
  static void par_signal(par_cond_t* c)      { pthread_cond_signal(c); }
  static void par_broadcast(par_cond_t* c)   { pthread_cond_broadcast(c); }
#endif

#define OWL_PAR_MAX_THREADS 32
/* En dessous de ce volume de calcul estime, on reste en serie : le cout de
 * reveil des workers depasserait le gain. N'affecte QUE la perf. */
#define OWL_PAR_MIN_WORK    2048L

typedef struct {
    int nthreads;                       /* total (workers + appelant), >= 1 */
    int nworkers;                       /* nthreads - 1 */
    par_thread_t workers[OWL_PAR_MAX_THREADS];

    par_mutex_t m;
    par_cond_t  cv_work;                /* les workers attendent une tache */
    par_cond_t  cv_done;               /* l'appelant attend la fin de la tache */

    OwlParallelBody body;
    void*           ctx;
    int             starts[OWL_PAR_MAX_THREADS + 1]; /* bornes de chunks */

    unsigned generation;               /* incremente a chaque dispatch */
    int      pending;                  /* workers encore actifs sur la tache */
    int      shutdown;
} OwlParPool;

typedef struct { OwlParPool* p; int idx; } ParWorkerArg;

static OwlParPool   g_pool;
static ParWorkerArg g_wargs[OWL_PAR_MAX_THREADS];
static int          g_pool_ready = 0;  /* 0 = non initialise, 1 = pret */

static int par_hardware_concurrency(void) {
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static int par_resolve_threads(void) {
    const char* e = getenv("OWL_THREADS");
    int nt;
    if (e && *e) {
        nt = atoi(e);
        if (nt < 1) nt = 1;            /* OWL_THREADS=1 -> serie stricte */
    } else {
        nt = par_hardware_concurrency();
    }
    if (nt > OWL_PAR_MAX_THREADS) nt = OWL_PAR_MAX_THREADS;
    if (nt < 1) nt = 1;
    return nt;
}

static void par_worker(OwlParPool* p, int idx) {
    unsigned seen = 0;
    for (;;) {
        par_lock(&p->m);
        while (p->generation == seen && !p->shutdown)
            par_wait(&p->cv_work, &p->m);
        if (p->shutdown) { par_unlock(&p->m); return; }
        seen = p->generation;
        int i0 = p->starts[idx];
        int i1 = p->starts[idx + 1];
        OwlParallelBody body = p->body;
        void* ctx = p->ctx;
        par_unlock(&p->m);

        if (i1 > i0) body(i0, i1, ctx);

        par_lock(&p->m);
        if (--p->pending == 0) par_signal(&p->cv_done);
        par_unlock(&p->m);
    }
}

#ifdef _WIN32
static DWORD WINAPI par_thread_entry(LPVOID a) {
    ParWorkerArg* w = (ParWorkerArg*)a;
    par_worker(w->p, w->idx);
    return 0;
}
#else
static void* par_thread_entry(void* a) {
    ParWorkerArg* w = (ParWorkerArg*)a;
    par_worker(w->p, w->idx);
    return NULL;
}
#endif

/* Init paresseuse sur le thread appelant (unique thread VM, pas de course). */
static OwlParPool* par_pool(void) {
    if (g_pool_ready) return &g_pool;

    int nt = par_resolve_threads();
    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.nthreads = nt;
    g_pool.nworkers = nt - 1;

    if (g_pool.nworkers > 0) {
        par_mutex_init(&g_pool.m);
        par_cond_init(&g_pool.cv_work);
        par_cond_init(&g_pool.cv_done);
        int launched = 0;
        for (int i = 1; i <= g_pool.nworkers; i++) {
            g_wargs[i].p = &g_pool;
            g_wargs[i].idx = i;
#ifdef _WIN32
            g_pool.workers[i - 1] = CreateThread(NULL, 0, par_thread_entry,
                                                 &g_wargs[i], 0, NULL);
            if (g_pool.workers[i - 1] == NULL) break;
#else
            if (pthread_create(&g_pool.workers[i - 1], NULL,
                               par_thread_entry, &g_wargs[i]) != 0) break;
#endif
            launched++;
        }
        /* Un echec de spawn reduit le pool sans casser la correction : moins
         * de threads = toujours le meme resultat, juste moins de parallelisme. */
        g_pool.nworkers = launched;
        g_pool.nthreads = launched + 1;
    }

    g_pool_ready = 1;
    return &g_pool;
}

int owl_parallel_num_threads(void) {
    return par_pool()->nthreads;
}

void owl_parallel_for(int n, long totalWork, OwlParallelBody body, void* ctx) {
    OwlParPool* p = par_pool();
    if (p->nthreads <= 1 || n < 2 || totalWork < OWL_PAR_MIN_WORK) {
        if (n > 0) body(0, n, ctx);    /* serie stricte : aucun thread lance */
        return;
    }

    int T = p->nthreads;

    par_lock(&p->m);
    p->body = body;
    p->ctx  = ctx;
    /* Chunks contigus deterministes : les `rem` premiers recoivent un element
     * de plus. Fonction pure de (n, T) -> partition identique a chaque appel. */
    int base = n / T, rem = n % T, s = 0;
    for (int t = 0; t < T; t++) {
        p->starts[t] = s;
        s += base + (t < rem ? 1 : 0);
    }
    p->starts[T] = s;                  /* == n */
    p->pending = p->nworkers;
    p->generation++;
    par_broadcast(&p->cv_work);
    par_unlock(&p->m);

    /* L'appelant execute le chunk 0 pendant que les workers traitent 1..T-1. */
    int c0 = p->starts[0], c1 = p->starts[1];
    if (c1 > c0) body(c0, c1, ctx);

    par_lock(&p->m);
    while (p->pending > 0) par_wait(&p->cv_done, &p->m);
    par_unlock(&p->m);
}
