/*
 * Parallel-work dispatcher.
 *
 * The default backend is an internal persistent thread pool (POSIX threads,
 * with a C11-thread fallback where pthreads is unavailable). OpenMP is an
 * explicit opt-in oracle build (NYTRIX_ENABLE_OPENMP=ON); the shipped
 * compiler neither requires nor links libomp.
 */
#include "base/parallel.h"
#include "base/common.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_OPENMP) && !defined(NYTRIX_NO_OPENMP)
#include <omp.h>
#define NY_PAR_HAVE_OPENMP 1
#else
#define NY_PAR_HAVE_OPENMP 0
#endif

#if !NY_PAR_HAVE_OPENMP && !defined(_WIN32)
#include <pthread.h>
#define NY_PAR_HAVE_PTHREAD 1
#else
#define NY_PAR_HAVE_PTHREAD 0
#endif

#if !NY_PAR_HAVE_OPENMP && !NY_PAR_HAVE_PTHREAD && !defined(_WIN32) && \
    !defined(__STDC_NO_THREADS__)
#include <threads.h>
#define NY_PAR_HAVE_C11 1
#else
#define NY_PAR_HAVE_C11 0
#endif

static _Atomic uint64_t g_regions;
static _Atomic uint64_t g_tasks;
static _Atomic uint64_t g_serial_regions;
static _Atomic uint64_t g_openmp_regions;
static _Atomic uint64_t g_c11_regions;
static _Atomic uint64_t g_pool_regions;
static _Atomic bool g_report_registered;

static void ny_parallel_report(void) {
  const char *s = getenv("NYTRIX_PARALLEL_REPORT");
  if (!s || !*s || strcmp(s, "0") == 0 || strcmp(s, "off") == 0)
    return;
  ny_parallel_stats_t st;
  ny_parallel_stats(&st);
  fprintf(stderr,
          "[parallel] threads=%d regions=%" PRIu64 " tasks=%" PRIu64
          " serial=%" PRIu64 " openmp=%" PRIu64 " c11=%" PRIu64
          " pool=%" PRIu64 "\n",
          ny_parallel_threads(), st.regions, st.tasks, st.serial_regions,
          st.openmp_regions, st.c11_regions, st.pool_regions);
}

static void ny_parallel_register_report(void) {
  bool expected = false;
  if (atomic_compare_exchange_strong_explicit(
          &g_report_registered, &expected, true, memory_order_relaxed,
          memory_order_relaxed))
    atexit(ny_parallel_report);
}

static long ny_parallel_parse_long(const char *s, long fallback) {
  if (!s || !*s)
    return fallback;
  errno = 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (errno || end == s || *end || v <= 0)
    return fallback;
  return v;
}

static bool ny_parallel_mode_enabled(void) {
  const char *mode = getenv("NYTRIX_PARALLEL_MODE");
  if (!mode || !*mode || strcmp(mode, "auto") == 0 ||
      strcmp(mode, "threads") == 0 || strcmp(mode, "all") == 0)
    return true;
  return strcmp(mode, "off") != 0 && strcmp(mode, "modules") != 0;
}

int ny_parallel_threads(void) {
  /*
   * Every parallel-capable pass asks; the answer cannot change mid-compile.
   */
  static _Atomic int cached = 0;
  int known = atomic_load_explicit(&cached, memory_order_relaxed);
  if (known)
    return known;
  long requested = ny_parallel_parse_long(getenv("NYTRIX_PARALLEL_THREADS"), 0);
  if (requested <= 0)
    requested = ny_parallel_parse_long(getenv("OMP_NUM_THREADS"), 0);
#if NY_PAR_HAVE_OPENMP
  if (requested <= 0)
    requested = omp_get_max_threads();
#else
  if (requested <= 0)
    requested = ny_cpu_count();
#endif
  if (requested < 1)
    requested = 1;
  if (requested > 256)
    requested = 256;
  atomic_store_explicit(&cached, (int)requested, memory_order_relaxed);
  return (int)requested;
}

size_t ny_parallel_min_work(void) {
  static _Atomic long cached = 0;
  long v = atomic_load_explicit(&cached, memory_order_relaxed);
  if (!v) {
    v = ny_parallel_parse_long(getenv("NYTRIX_PARALLEL_MIN_WORK"), 2048);
    if (v <= 0)
      v = 2048;
    atomic_store_explicit(&cached, v, memory_order_relaxed);
  }
  return (size_t)v;
}

/*
 * Team size for one region: a worker per min-work grain, capped by the task
 * count and the configured threads. Sizing the team from the work keeps a
 * medium region parallel on a wide machine; dividing the work by every
 * hardware thread instead made a 96-thread host run regions serially that an
 * 8-thread host parallelized.
 */
static size_t ny_parallel_team(size_t count, size_t work_items) {
  int threads = ny_parallel_threads();
  if (!ny_parallel_mode_enabled() || threads < 2)
    return 1;
  size_t team = count < (size_t)threads ? count : (size_t)threads;
  size_t by_work = work_items / ny_parallel_min_work();
  if (team > by_work)
    team = by_work;
  return team ? team : 1;
}

bool ny_parallel_enabled(size_t count, size_t work_items) {
  return ny_parallel_team(count, work_items) >= 2;
}

#if NY_PAR_HAVE_PTHREAD
/*
 * Persistent pool. One job runs at a time; the main thread participates and
 * waits until every worker reports the job finished, so the job struct needs
 * no per-region allocation and no thread is created per region.
 */
typedef struct {
  _Atomic size_t next;
  _Atomic int failed;
  size_t count;
  size_t active_workers;
  ny_parallel_task_fn fn;
  void *ctx;
} ny_parallel_job_t;

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_pool_done = PTHREAD_COND_INITIALIZER;
static ny_parallel_job_t g_pool_job;
static pthread_t *g_pool_threads;
static size_t g_pool_workers;
static unsigned long g_pool_epoch;
static size_t g_pool_done_count;
static bool g_pool_stop;
static bool g_pool_atexit;
static _Atomic bool g_region_active;

/*
 * One shared work queue: whichever participant is free takes the next index,
 * which keeps uneven encode/lowering jobs balanced without per-region thread
 * creation. Callbacks are independent, so completion order does not affect
 * the emitted object.
 */
static void ny_parallel_job_run(ny_parallel_job_t *job) {
  for (;;) {
    if (atomic_load_explicit(&job->failed, memory_order_relaxed))
      return;
    size_t i =
        atomic_fetch_add_explicit(&job->next, 1, memory_order_relaxed);
    if (i >= job->count)
      return;
    if (!job->fn(i, job->ctx)) {
      atomic_store_explicit(&job->failed, 1, memory_order_relaxed);
      return;
    }
  }
}

static void *ny_parallel_pool_worker(void *opaque) {
  size_t worker_id = (size_t)(uintptr_t)opaque;
  unsigned long seen = 0;
  pthread_mutex_lock(&g_pool_mu);
  for (;;) {
    while (!g_pool_stop && seen == g_pool_epoch)
      pthread_cond_wait(&g_pool_work, &g_pool_mu);
    if (g_pool_stop) {
      pthread_mutex_unlock(&g_pool_mu);
      return NULL;
    }
    seen = g_pool_epoch;
    bool active = worker_id < g_pool_job.active_workers;
    pthread_mutex_unlock(&g_pool_mu);
    if (!active) {
      pthread_mutex_lock(&g_pool_mu);
      continue;
    }
    ny_parallel_job_run(&g_pool_job);
    pthread_mutex_lock(&g_pool_mu);
    ++g_pool_done_count;
    if (g_pool_done_count >= g_pool_job.active_workers)
      pthread_cond_signal(&g_pool_done);
  }
}

static void ny_parallel_pool_shutdown(void) {
  pthread_mutex_lock(&g_pool_mu);
  g_pool_stop = true;
  pthread_cond_broadcast(&g_pool_work);
  pthread_mutex_unlock(&g_pool_mu);
  for (size_t i = 0; i < g_pool_workers; ++i)
    pthread_join(g_pool_threads[i], NULL);
  free(g_pool_threads);
  g_pool_threads = NULL;
  g_pool_workers = 0;
}

static bool ny_parallel_pool_start_locked(int threads) {
  if (threads < 2)
    return false;
  size_t want = (size_t)threads - 1;
  if (g_pool_workers >= want)
    return true;
  pthread_t *threads_buf =
      realloc(g_pool_threads, want * sizeof(*g_pool_threads));
  if (!threads_buf)
    return g_pool_workers > 0;
  g_pool_threads = threads_buf;
  /*
   * Worker stacks must not depend on the inherited RLIMIT_STACK: a child
   * compiler started under a ~3 MB limit overflowed in every worker (the
   * x86-64 encode and lowering recursion needs more), while the caller's
   * thread kept running fine. The pages are reserved lazily, so a generous
   * size costs address space only.
   */
  pthread_attr_t attr;
  bool have_attr = pthread_attr_init(&attr) == 0;
  if (have_attr)
    pthread_attr_setstacksize(&attr, (size_t)64 << 20);
  while (g_pool_workers < want) {
    size_t id = g_pool_workers;
    if (pthread_create(&g_pool_threads[id], have_attr ? &attr : NULL,
                       ny_parallel_pool_worker, (void *)(uintptr_t)id) != 0)
      break;
    ++g_pool_workers;
  }
  if (have_attr)
    pthread_attr_destroy(&attr);
  if (!g_pool_atexit) {
    if (g_pool_workers) {
      atexit(ny_parallel_pool_shutdown);
      g_pool_atexit = true;
    }
  }
  if (!g_pool_workers) {
    free(g_pool_threads);
    g_pool_threads = NULL;
    return false;
  }
  return true;
}
#endif

#if NY_PAR_HAVE_C11
typedef struct {
  _Atomic size_t next;
  _Atomic int failed;
  size_t count;
  ny_parallel_task_fn fn;
  void *ctx;
} ny_parallel_c11_ctx_t;

static int ny_parallel_c11_worker(void *opaque) {
  ny_parallel_c11_ctx_t *p = (ny_parallel_c11_ctx_t *)opaque;
  for (;;) {
    if (atomic_load_explicit(&p->failed, memory_order_relaxed))
      return 0;
    size_t i = atomic_fetch_add_explicit(&p->next, 1, memory_order_relaxed);
    if (i >= p->count)
      return 0;
    if (!p->fn(i, p->ctx)) {
      atomic_store_explicit(&p->failed, 1, memory_order_relaxed);
      return 0;
    }
  }
}
#endif

bool ny_parallel_for(size_t count, size_t work_items, ny_parallel_task_fn fn,
                     void *ctx) {
  if (!fn)
    return false;
  ny_parallel_register_report();
  atomic_fetch_add_explicit(&g_regions, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_tasks, count, memory_order_relaxed);
  /*
   * The minimum is a per-worker grain size. Checking only total work can
   * start a large team for tiny per-item callbacks, where startup and
   * synchronization cost more than the work being parallelized.
   */
  int threads = (int)ny_parallel_team(count, work_items);
  if (threads < 2) {
    atomic_fetch_add_explicit(&g_serial_regions, 1, memory_order_relaxed);
    for (size_t i = 0; i < count; ++i)
      if (!fn(i, ctx))
        return false;
    return true;
  }

#if NY_PAR_HAVE_OPENMP
  if (!omp_in_parallel()) {
    _Atomic int failed = 0;
    /*
     * OpenMP creates the requested team even when the loop has fewer tasks.
     * Cap the team to useful workers instead of waking the machine-wide
     * default (dozens of idle threads for a two-item region).
     */
    if ((size_t)threads > count)
      threads = (int)count;
#pragma omp parallel for schedule(static) num_threads(threads)
    for (long long i = 0; i < (long long)count; ++i) {
      if (!atomic_load_explicit(&failed, memory_order_relaxed) &&
          !fn((size_t)i, ctx))
        atomic_store_explicit(&failed, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&g_openmp_regions, 1, memory_order_relaxed);
    return atomic_load_explicit(&failed, memory_order_relaxed) == 0;
  }
#endif

#if NY_PAR_HAVE_PTHREAD
  /*
   * Nested or concurrent callers run serially: the pool owns one job at a
   * time, and serial execution preserves the caller's stack for callbacks
   * that touch builder state.
   */
  bool expected = false;
  if (atomic_compare_exchange_strong_explicit(
          &g_region_active, &expected, true, memory_order_acq_rel,
          memory_order_relaxed)) {
    if ((size_t)threads > count)
      threads = (int)count;
    pthread_mutex_lock(&g_pool_mu);
    if (ny_parallel_pool_start_locked(threads)) {
      size_t active_workers = (size_t)threads - 1;
      if (active_workers > g_pool_workers)
        active_workers = g_pool_workers;
      g_pool_job = (ny_parallel_job_t){.next = 0,
                                       .failed = 0,
                                       .count = count,
                                       .active_workers = active_workers,
                                       .fn = fn,
                                       .ctx = ctx};
      g_pool_done_count = 0;
      ++g_pool_epoch;
      pthread_cond_broadcast(&g_pool_work);
      pthread_mutex_unlock(&g_pool_mu);
      ny_parallel_job_run(&g_pool_job);
      pthread_mutex_lock(&g_pool_mu);
      while (g_pool_done_count < active_workers)
        pthread_cond_wait(&g_pool_done, &g_pool_mu);
      bool failed = atomic_load_explicit(&g_pool_job.failed,
                                         memory_order_relaxed) != 0;
      pthread_mutex_unlock(&g_pool_mu);
      atomic_store_explicit(&g_region_active, false, memory_order_release);
      if (!failed) {
        atomic_fetch_add_explicit(&g_pool_regions, 1, memory_order_relaxed);
        return true;
      }
      return false;
    }
    pthread_mutex_unlock(&g_pool_mu);
    atomic_store_explicit(&g_region_active, false, memory_order_release);
  }
#endif

#if NY_PAR_HAVE_C11
  if (threads > 1) {
    ny_parallel_c11_ctx_t p = {.next = 0,
                               .failed = 0,
                               .count = count,
                               .fn = fn,
                               .ctx = ctx};
    thrd_t *workers = calloc((size_t)threads - 1, sizeof(*workers));
    if (workers) {
      int started = 0;
      for (int i = 0; i < threads - 1; ++i) {
        if (thrd_create(&workers[i], ny_parallel_c11_worker, &p) != thrd_success)
          break;
        started++;
      }
      ny_parallel_c11_worker(&p);
      for (int i = 0; i < started; ++i)
        thrd_join(workers[i], NULL);
      free(workers);
      atomic_fetch_add_explicit(&g_c11_regions, 1, memory_order_relaxed);
      return atomic_load_explicit(&p.failed, memory_order_relaxed) == 0;
    }
  }
#endif

  atomic_fetch_add_explicit(&g_serial_regions, 1, memory_order_relaxed);
  for (size_t i = 0; i < count; ++i)
    if (!fn(i, ctx))
      return false;
  return true;
}

void ny_parallel_stats(ny_parallel_stats_t *out) {
  if (!out)
    return;
  out->regions = atomic_load_explicit(&g_regions, memory_order_relaxed);
  out->tasks = atomic_load_explicit(&g_tasks, memory_order_relaxed);
  out->serial_regions =
      atomic_load_explicit(&g_serial_regions, memory_order_relaxed);
  out->openmp_regions =
      atomic_load_explicit(&g_openmp_regions, memory_order_relaxed);
  out->c11_regions = atomic_load_explicit(&g_c11_regions, memory_order_relaxed);
  out->pool_regions = atomic_load_explicit(&g_pool_regions, memory_order_relaxed);
}

void ny_parallel_stats_reset(void) {
  atomic_store_explicit(&g_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_tasks, 0, memory_order_relaxed);
  atomic_store_explicit(&g_serial_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_openmp_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_c11_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_pool_regions, 0, memory_order_relaxed);
}
