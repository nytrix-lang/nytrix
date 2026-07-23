#include "base/parallel.h"
#include "base/common.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_OPENMP)
#include <omp.h>
#define NY_PAR_HAVE_OPENMP 1
#else
#define NY_PAR_HAVE_OPENMP 0
#endif

#if !NY_PAR_HAVE_OPENMP && !defined(_WIN32) && !defined(__STDC_NO_THREADS__)
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
static _Atomic bool g_report_registered;

static void ny_parallel_report(void) {
  const char *s = getenv("NYTRIX_PARALLEL_REPORT");
  if (!s || !*s || strcmp(s, "0") == 0 || strcmp(s, "off") == 0)
    return;
  ny_parallel_stats_t st;
  ny_parallel_stats(&st);
  fprintf(stderr,
          "[parallel] threads=%d regions=%" PRIu64 " tasks=%" PRIu64
          " serial=%" PRIu64 " openmp=%" PRIu64 " c11=%" PRIu64 "\n",
          ny_parallel_threads(), st.regions, st.tasks, st.serial_regions,
          st.openmp_regions, st.c11_regions);
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
  return (int)requested;
}

size_t ny_parallel_min_work(void) {
  long v = ny_parallel_parse_long(getenv("NYTRIX_PARALLEL_MIN_WORK"), 2048);
  return v > 0 ? (size_t)v : 2048;
}

bool ny_parallel_enabled(size_t work_items) {
  if (!ny_parallel_mode_enabled() || ny_parallel_threads() < 2)
    return false;
  return work_items >= ny_parallel_min_work();
}

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
  if (count < 2 || !ny_parallel_enabled(work_items)) {
    atomic_fetch_add_explicit(&g_serial_regions, 1, memory_order_relaxed);
    for (size_t i = 0; i < count; ++i)
      if (!fn(i, ctx))
        return false;
    return true;
  }

#if NY_PAR_HAVE_OPENMP
  if (!omp_in_parallel()) {
    _Atomic int failed = 0;
    int threads = ny_parallel_threads();
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

#if NY_PAR_HAVE_C11
  int threads = ny_parallel_threads();
  if ((size_t)threads > count)
    threads = (int)count;
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
}

void ny_parallel_stats_reset(void) {
  atomic_store_explicit(&g_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_tasks, 0, memory_order_relaxed);
  atomic_store_explicit(&g_serial_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_openmp_regions, 0, memory_order_relaxed);
  atomic_store_explicit(&g_c11_regions, 0, memory_order_relaxed);
}
