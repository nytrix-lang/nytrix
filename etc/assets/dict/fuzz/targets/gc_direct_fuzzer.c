#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef NYTRIX_GC_C
#error "compile with -DNYTRIX_GC_C=\\\"/path/to/nytrix/src/rt/gc.c\\\""
#endif

__thread uintptr_t rt_heap_ptr_cache_keys[1u << 13] = {0};
__thread uint64_t rt_heap_ptr_cache_epoch = 0;
_Atomic uint64_t rt_heap_ptr_global_epoch = 1;

bool ny_env_enabled(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0 && strcmp(v, "false") != 0 &&
         strcmp(v, "FALSE") != 0 && strcmp(v, "no") != 0;
}

int64_t rt_malloc(int64_t size) {
  int64_t n = (size & 1) ? (size >> 1) : size;
  if (n < 0) return 0;
  size_t body = ((size_t)n + 15u) & ~15u;
  unsigned char *base = (unsigned char *)calloc(1, body + 32u);
  if (!base) return 0;
  uint64_t *size_slot = (uint64_t *)(void *)(base + 16u);
  *size_slot = ((uint64_t)body << 1u) | 1u;
  return (int64_t)(uintptr_t)(base + 32u);
}

#include NYTRIX_GC_C

enum {
  GC_FUZZ_ROOTS = 256,
  GC_FUZZ_MAX_THREADS = 64,
  GC_FUZZ_SCRATCH_ROOTS = 64,
  GC_FUZZ_TAG_DICT = TAG_DICT,
  GC_FUZZ_TAG_LIST = TAG_LIST,
  GC_FUZZ_TAG_TUPLE = TAG_TUPLE,
  GC_FUZZ_TAG_OK = TAG_OK,
  GC_FUZZ_TAG_ERR = TAG_ERR,
  GC_FUZZ_TAG_CLOSURE = TAG_CLOSURE,
  GC_FUZZ_TAG_STR = TAG_STR,
  GC_FUZZ_TAG_STR_CONST = TAG_STR_CONST
};

typedef enum {
  GC_MODE_ALLOC = 0,
  GC_MODE_ROOT_CHURN,
  GC_MODE_REMEMBERED_CHURN,
  GC_MODE_MINOR_STORM,
  GC_MODE_MAJOR_STORM,
  GC_MODE_DEEP_GRAPH,
  GC_MODE_WIDE_GRAPH,
  GC_MODE_DICT_HEAVY,
  GC_MODE_CLOSURE_RESULT,
  GC_MODE_STRING_HEAVY,
  GC_MODE_RESULT_NEST,
  GC_MODE_CYCLE_GRAPH,
  GC_MODE_PROMOTION_LADDER,
  GC_MODE_MIXED_RUNTIME,
  GC_MODE_COUNT
} gc_mode_t;

typedef struct {
  uint64_t state;
} gc_rng_t;

typedef struct {
  int id;
  int threads;
  uint64_t iterations;
  uint64_t deadline_ns;
  uint64_t seed;
  int explicit_minor_every;
  int explicit_major_every;
  bool stop_on_deadline;
  _Atomic int *stop;
  int64_t *roots;
  int64_t *scratch_roots;
  int scratch_count;
  uint64_t ops;
  uint64_t remembered_events;
  uint64_t mode_counts[GC_MODE_COUNT];
  int max_depth;
  int max_roots_live;
  gc_mode_t forced_mode;
} gc_worker_t;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t rng_next(gc_rng_t *r) {
  uint64_t x = r->state ? r->state : UINT64_C(0x9e3779b97f4a7c15);
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->state = x;
  return x * UINT64_C(2685821657736338717);
}

static int rng_range(gc_rng_t *r, int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int)(rng_next(r) % (uint64_t)(hi - lo + 1));
}

static int64_t tagged_int(int64_t v) { return (v << 1) | 1; }

static _Atomic uint64_t g_tag_mask = 0;
static __thread int64_t *tls_scratch_roots = NULL;
static __thread int tls_scratch_count = 0;
static __thread int tls_scratch_next = 0;
static pthread_mutex_t g_harness_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t gc_tag_bit(int tag) {
  switch (tag) {
  case TAG_LIST: return UINT64_C(1) << 0;
  case TAG_DICT: return UINT64_C(1) << 1;
  case TAG_TUPLE: return UINT64_C(1) << 2;
  case TAG_OK: return UINT64_C(1) << 3;
  case TAG_ERR: return UINT64_C(1) << 4;
  case TAG_CLOSURE: return UINT64_C(1) << 5;
  case TAG_STR: return UINT64_C(1) << 6;
  case TAG_STR_CONST: return UINT64_C(1) << 7;
  default: return UINT64_C(1) << 31;
  }
}

static int gc_popcount64(uint64_t v) {
  int n = 0;
  while (v) {
    n += (int)(v & 1u);
    v >>= 1;
  }
  return n;
}

static void gc_set_scratch_roots(int64_t *roots, int count) {
  tls_scratch_roots = roots;
  tls_scratch_count = count;
  tls_scratch_next = 0;
}

static void gc_scratch_protect_unlocked(int64_t obj) {
  if (!obj || !tls_scratch_roots || tls_scratch_count <= 0) return;
  tls_scratch_roots[tls_scratch_next++ % tls_scratch_count] = obj;
}

static int64_t alloc_tagged_body_unlocked(int tag) {
  size_t body = (size_t)tag;
  int64_t obj = nyGcAllocFastUnlocked(body);
  if (!obj) {
    nyGcMinorCollectUnlocked();
    obj = nyGcAllocFastUnlocked(body);
  }
  if (!obj) {
    obj = nyGcAllocTenuredUnlocked(body);
    if (!obj) {
      nyGcMajorCollectUnlocked();
      obj = nyGcAllocTenuredUnlocked(body);
    }
  }
  if (!obj) return 0;
  *(int64_t *)((uint8_t *)(uintptr_t)obj - 8) = tag;
  atomic_fetch_or_explicit(&g_tag_mask, gc_tag_bit(tag), memory_order_relaxed);
  gc_scratch_protect_unlocked(obj);
  return obj;
}

static int64_t alloc_tagged_body(int tag) {
  if (!gNyGc.initialized) nyGcInit();
  if (!gNyGc.enable_nursery) return nyGcAlloc((size_t)tag);
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(tag);
  nyGcUnlock();
  return obj;
}

static int64_t make_scalar(gc_rng_t *rng, int tag) {
  int64_t v = tagged_int((int64_t)(rng_next(rng) & 0x7fffff));
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(tag);
  if (!obj) {
    nyGcUnlock();
    return 0;
  }
  if (tag == GC_FUZZ_TAG_OK || tag == GC_FUZZ_TAG_ERR) {
    *(int64_t *)(uintptr_t)obj = v;
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = v;
  }
  nyGcUnlock();
  return obj;
}

static int64_t make_list(gc_rng_t *rng, int64_t *roots, int root_count, bool tuple) {
  int len = rng_range(rng, 0, 10);
  int64_t values[10] = {0};
  for (int i = 0; i < len; ++i) {
    if (root_count > 0 && (rng_next(rng) & 3u) == 0)
      values[i] = roots[rng_range(rng, 0, root_count - 1)];
    else
      values[i] = tagged_int((int64_t)(rng_next(rng) & 0xffff));
  }
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(tuple ? GC_FUZZ_TAG_TUPLE : GC_FUZZ_TAG_LIST);
  if (!obj) {
    nyGcUnlock();
    return 0;
  }
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(len);
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = tagged_int(10);
  int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)obj + 16);
  for (int i = 0; i < len; ++i) items[i] = values[i];
  nyGcUnlock();
  return obj;
}

static int64_t make_dict(gc_rng_t *rng, int64_t *roots, int root_count) {
  int cap = rng_range(rng, 1, 3);
  int64_t keys[3] = {0};
  int64_t values[3] = {0};
  int64_t states[3] = {0};
  int filled = 0;
  for (int i = 0; i < cap; ++i) {
    bool live = rng_range(rng, 0, 99) < 72;
    states[i] = live ? ((rng_next(rng) & 1u) ? 1 : tagged_int(1)) : 0;
    if (!live) continue;
    ++filled;
    keys[i] = (root_count > 0 && (rng_next(rng) & 1u))
                  ? roots[rng_range(rng, 0, root_count - 1)]
                  : tagged_int((int64_t)(rng_next(rng) & 0xffff));
    values[i] = (root_count > 0 && (rng_next(rng) & 3u) != 0)
                    ? roots[rng_range(rng, 0, root_count - 1)]
                    : tagged_int((int64_t)(rng_next(rng) & 0xffff));
  }
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(GC_FUZZ_TAG_DICT);
  if (!obj) {
    nyGcUnlock();
    return 0;
  }
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(0);
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = tagged_int(cap);
  for (int i = 0; i < cap; ++i) {
    uint8_t *slot = (uint8_t *)(uintptr_t)obj + 16u + (size_t)i * 24u;
    *(int64_t *)slot = keys[i];
    *(int64_t *)(slot + 8) = values[i];
    *(int64_t *)(slot + 16) = states[i];
  }
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(filled);
  nyGcUnlock();
  return obj;
}

static int64_t make_closure(gc_rng_t *rng, int64_t *roots, int root_count) {
  int64_t fn = tagged_int((int64_t)(rng_next(rng) & 0xffff));
  int64_t arity = tagged_int((int64_t)(rng_next(rng) & 0xffff));
  int64_t env = root_count > 0 ? roots[rng_range(rng, 0, root_count - 1)] : 0;
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(GC_FUZZ_TAG_CLOSURE);
  if (obj) {
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = fn;
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = arity;
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 16) = env;
  }
  nyGcUnlock();
  return obj;
}

static int64_t make_result_ref(gc_rng_t *rng, int64_t ref) {
  int tag = (rng_next(rng) & 1u) ? GC_FUZZ_TAG_OK : GC_FUZZ_TAG_ERR;
  int64_t first = ref ? ref : tagged_int((int64_t)(rng_next(rng) & 0xffff));
  int64_t second = tagged_int((int64_t)(rng_next(rng) & 0xffff));
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(tag);
  if (obj) {
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = first;
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = second;
  }
  nyGcUnlock();
  return obj;
}

static int64_t make_deep_graph(gc_rng_t *rng, int *depth_out) {
  int depth = rng_range(rng, 2, 12);
  int64_t tail = make_scalar(rng, GC_FUZZ_TAG_OK);
  if (!tail) return 0;
  for (int i = 0; i < depth; ++i) {
    int tag = (rng_next(rng) & 1u) ? GC_FUZZ_TAG_LIST : GC_FUZZ_TAG_TUPLE;
    nyGcLock();
    int64_t obj = alloc_tagged_body_unlocked(tag);
    if (!obj) {
      nyGcUnlock();
      return 0;
    }
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(2);
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = tagged_int(2);
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 16) = tail;
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 24) = tagged_int(i);
    nyGcUnlock();
    tail = obj;
  }
  if (depth_out && depth > *depth_out) *depth_out = depth;
  return tail;
}

static int64_t make_wide_graph(gc_rng_t *rng, int64_t *roots, int root_count, int *depth_out) {
  enum { WIDE_SELF = 1, WIDE_ROOT = 2, WIDE_SCALAR = 3, WIDE_INT = 4 };
  int kind[10] = {0};
  int64_t values[10] = {0};
  for (int i = 0; i < 10; ++i) {
    if (i == 0 && (rng_next(rng) & 1u)) {
      kind[i] = WIDE_SELF;
    } else if (root_count > 0 && (rng_next(rng) & 1u)) {
      kind[i] = WIDE_ROOT;
      values[i] = roots[rng_range(rng, 0, root_count - 1)];
    } else if ((rng_next(rng) & 3u) == 0) {
      kind[i] = WIDE_SCALAR;
      values[i] = make_scalar(rng, GC_FUZZ_TAG_OK);
      if (!values[i]) return 0;
    } else {
      kind[i] = WIDE_INT;
      values[i] = tagged_int((int64_t)(rng_next(rng) & 0xffff));
    }
  }
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(GC_FUZZ_TAG_LIST);
  if (!obj) {
    nyGcUnlock();
    return 0;
  }
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(10);
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = tagged_int(10);
  int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)obj + 16);
  for (int i = 0; i < 10; ++i) {
    items[i] = kind[i] == WIDE_SELF ? obj : values[i];
  }
  nyGcUnlock();
  if (depth_out && *depth_out < 2) *depth_out = 2;
  return obj;
}

static int64_t make_stringish(gc_rng_t *rng, int tag) {
  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(tag);
  if (!obj) {
    nyGcUnlock();
    return 0;
  }
  uint8_t *bytes = (uint8_t *)(uintptr_t)obj;
  size_t body_len = (size_t)(tag == GC_FUZZ_TAG_STR_CONST ? GC_FUZZ_TAG_STR_CONST : GC_FUZZ_TAG_STR);
  int len = rng_range(rng, 8, 48);
  if ((size_t)len >= body_len) len = (int)body_len - 1;
  for (int i = 0; i < len; ++i) bytes[i] = (uint8_t)('a' + (int)((rng_next(rng) + (uint64_t)i) % 26u));
  bytes[len] = 0;
  nyGcUnlock();
  return obj;
}

static int64_t make_string_heavy(gc_rng_t *rng, int64_t *roots, int root_count, int *depth_out) {
  int64_t a = make_stringish(rng, GC_FUZZ_TAG_STR);
  int64_t b = make_stringish(rng, (rng_next(rng) & 1u) ? GC_FUZZ_TAG_STR : GC_FUZZ_TAG_STR_CONST);
  int64_t c = root_count > 0 && (rng_next(rng) & 1u) ? roots[rng_range(rng, 0, root_count - 1)]
                                                       : make_stringish(rng, GC_FUZZ_TAG_STR);
  if (!a || !b || !c) return 0;
  int64_t values[5] = {a, b, c, tagged_int((int64_t)(rng_next(rng) & 0xffff)), 0};
  if (rng_next(rng) & 1u) {
    nyGcLock();
    int64_t obj = alloc_tagged_body_unlocked(GC_FUZZ_TAG_LIST);
    if (!obj) {
      nyGcUnlock();
      return 0;
    }
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(4);
    *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = tagged_int(5);
    int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)obj + 16);
    for (int i = 0; i < 4; ++i) items[i] = values[i];
    items[4] = obj;
    nyGcUnlock();
    if (depth_out && *depth_out < 3) *depth_out = 3;
    return obj;
  }

  nyGcLock();
  int64_t obj = alloc_tagged_body_unlocked(GC_FUZZ_TAG_DICT);
  if (!obj) {
    nyGcUnlock();
    return 0;
  }
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 0) = tagged_int(3);
  *(int64_t *)((uint8_t *)(uintptr_t)obj + 8) = tagged_int(3);
  for (int i = 0; i < 3; ++i) {
    uint8_t *slot = (uint8_t *)(uintptr_t)obj + 16u + (size_t)i * 24u;
    *(int64_t *)slot = i == 0 ? a : tagged_int((int64_t)(rng_next(rng) & 0xffff));
    *(int64_t *)(slot + 8) = values[i];
    *(int64_t *)(slot + 16) = tagged_int(1);
  }
  nyGcUnlock();
  if (depth_out && *depth_out < 3) *depth_out = 3;
  return obj;
}

static int64_t make_result_nest(gc_rng_t *rng, int64_t *roots, int root_count, int *depth_out) {
  int depth = rng_range(rng, 3, 9);
  int64_t leaf = (rng_next(rng) & 1u) ? make_wide_graph(rng, roots, root_count, depth_out)
                                      : make_string_heavy(rng, roots, root_count, depth_out);
  if (!leaf) leaf = make_dict(rng, roots, root_count);
  if (!leaf) return 0;
  int64_t cur = leaf;
  for (int i = 0; i < depth; ++i) {
    cur = make_result_ref(rng, cur);
    if (!cur) return 0;
  }
  if (depth_out && depth + 2 > *depth_out) *depth_out = depth + 2;
  return cur;
}

static int64_t make_cycle_graph(gc_rng_t *rng, int *depth_out) {
  int64_t marker = tagged_int((int64_t)(rng_next(rng) & 0xffff));
  nyGcLock();
  int64_t a = alloc_tagged_body_unlocked(GC_FUZZ_TAG_LIST);
  int64_t b = alloc_tagged_body_unlocked(GC_FUZZ_TAG_LIST);
  int64_t d = alloc_tagged_body_unlocked(GC_FUZZ_TAG_DICT);
  if (!a || !b || !d) {
    nyGcUnlock();
    return 0;
  }
  *(int64_t *)((uint8_t *)(uintptr_t)a + 0) = tagged_int(4);
  *(int64_t *)((uint8_t *)(uintptr_t)a + 8) = tagged_int(4);
  int64_t *ai = (int64_t *)((uint8_t *)(uintptr_t)a + 16);
  ai[0] = a;
  ai[1] = b;
  ai[2] = d;
  ai[3] = marker;

  *(int64_t *)((uint8_t *)(uintptr_t)b + 0) = tagged_int(4);
  *(int64_t *)((uint8_t *)(uintptr_t)b + 8) = tagged_int(4);
  int64_t *bi = (int64_t *)((uint8_t *)(uintptr_t)b + 16);
  bi[0] = b;
  bi[1] = a;
  bi[2] = d;
  bi[3] = tagged_int((int64_t)(rng_next(rng) & 0xffff));

  *(int64_t *)((uint8_t *)(uintptr_t)d + 0) = tagged_int(3);
  *(int64_t *)((uint8_t *)(uintptr_t)d + 8) = tagged_int(3);
  for (int i = 0; i < 3; ++i) {
    uint8_t *slot = (uint8_t *)(uintptr_t)d + 16u + (size_t)i * 24u;
    *(int64_t *)slot = i == 0 ? a : tagged_int((int64_t)(rng_next(rng) & 0xffff));
    *(int64_t *)(slot + 8) = i == 0 ? b : (i == 1 ? d : a);
    *(int64_t *)(slot + 16) = tagged_int(1);
  }
  nyGcUnlock();
  if (depth_out && *depth_out < 4) *depth_out = 4;
  return a;
}

static int64_t make_object(gc_rng_t *rng, int64_t *roots, int root_count);

static int64_t make_mixed_runtime_graph(gc_rng_t *rng, int64_t *roots, int root_count, int *depth_out) {
  int pick = rng_range(rng, 0, 99);
  if (pick < 18) return make_string_heavy(rng, roots, root_count, depth_out);
  if (pick < 34) return make_result_nest(rng, roots, root_count, depth_out);
  if (pick < 48) return make_cycle_graph(rng, depth_out);
  if (pick < 62) return make_wide_graph(rng, roots, root_count, depth_out);
  if (pick < 76) return make_dict(rng, roots, root_count);
  if (pick < 88) return make_closure(rng, roots, root_count);
  return make_object(rng, roots, root_count);
}

static int64_t make_object(gc_rng_t *rng, int64_t *roots, int root_count) {
  int pick = rng_range(rng, 0, 99);
  int depth = 0;
  if (pick < 30) return make_dict(rng, roots, root_count);
  if (pick < 55) return make_list(rng, roots, root_count, false);
  if (pick < 67) return make_list(rng, roots, root_count, true);
  if (pick < 76) return make_closure(rng, roots, root_count);
  if (pick < 83) return make_result_ref(rng, root_count > 0 ? roots[rng_range(rng, 0, root_count - 1)] : 0);
  if (pick < 90) return make_stringish(rng, (rng_next(rng) & 1u) ? GC_FUZZ_TAG_STR : GC_FUZZ_TAG_STR_CONST);
  if (pick < 96) return make_scalar(rng, GC_FUZZ_TAG_OK);
  if (pick < 99) return make_scalar(rng, GC_FUZZ_TAG_ERR);
  return make_cycle_graph(rng, &depth);
}

static gc_mode_t choose_mode(gc_rng_t *rng, gc_mode_t forced) {
  if (forced >= 0 && forced < GC_MODE_COUNT) return forced;
  int pick = rng_range(rng, 0, 99);
  if (pick < 16) return GC_MODE_ALLOC;
  if (pick < 28) return GC_MODE_ROOT_CHURN;
  if (pick < 40) return GC_MODE_REMEMBERED_CHURN;
  if (pick < 50) return GC_MODE_DEEP_GRAPH;
  if (pick < 60) return GC_MODE_WIDE_GRAPH;
  if (pick < 69) return GC_MODE_DICT_HEAVY;
  if (pick < 77) return GC_MODE_CLOSURE_RESULT;
  if (pick < 84) return GC_MODE_STRING_HEAVY;
  if (pick < 90) return GC_MODE_RESULT_NEST;
  if (pick < 95) return GC_MODE_CYCLE_GRAPH;
  if (pick < 98) return GC_MODE_PROMOTION_LADDER;
  return (pick & 1) ? GC_MODE_MINOR_STORM : GC_MODE_MAJOR_STORM;
}

static int64_t make_mode_object(gc_rng_t *rng, int64_t *roots, int root_count,
                                gc_mode_t mode, int *max_depth) {
  switch (mode) {
  case GC_MODE_DEEP_GRAPH:
    return make_deep_graph(rng, max_depth);
  case GC_MODE_WIDE_GRAPH:
    return make_wide_graph(rng, roots, root_count, max_depth);
  case GC_MODE_DICT_HEAVY:
    return make_dict(rng, roots, root_count);
  case GC_MODE_CLOSURE_RESULT: {
    int64_t ref = root_count > 0 ? roots[rng_range(rng, 0, root_count - 1)] : 0;
    return (rng_next(rng) & 1u) ? make_closure(rng, roots, root_count) : make_result_ref(rng, ref);
  }
  case GC_MODE_STRING_HEAVY:
    return make_string_heavy(rng, roots, root_count, max_depth);
  case GC_MODE_RESULT_NEST:
    return make_result_nest(rng, roots, root_count, max_depth);
  case GC_MODE_CYCLE_GRAPH:
    return make_cycle_graph(rng, max_depth);
  case GC_MODE_PROMOTION_LADDER:
  case GC_MODE_MIXED_RUNTIME:
    return make_mixed_runtime_graph(rng, roots, root_count, max_depth);
  default:
    return make_object(rng, roots, root_count);
  }
}

static bool run_remembered_churn(gc_worker_t *w, gc_rng_t *rng, int slot, int visible_roots) {
  if (w->threads != 1) return false;
  int64_t key = tagged_int((int64_t)(rng_next(rng) & 0xffff));
  nyGcLock();
  int64_t parent = alloc_tagged_body_unlocked(GC_FUZZ_TAG_DICT);
  if (!parent) {
    nyGcUnlock();
    return false;
  }
  *(int64_t *)((uint8_t *)(uintptr_t)parent + 0) = tagged_int(1);
  *(int64_t *)((uint8_t *)(uintptr_t)parent + 8) = tagged_int(1);
  uint8_t *dict_slot = (uint8_t *)(uintptr_t)parent + 16u;
  *(int64_t *)dict_slot = key;
  *(int64_t *)(dict_slot + 8) = tagged_int(0);
  *(int64_t *)(dict_slot + 16) = tagged_int(1);
  nyGcUnlock();
  nyGcWriteBarrier(&w->roots[slot], parent);
  nyGcTriggerMinor();
  parent = w->roots[slot];
  dict_slot = (uint8_t *)(uintptr_t)parent + 16u;
  int64_t child = make_mode_object(rng, w->roots, visible_roots, GC_MODE_WIDE_GRAPH, &w->max_depth);
  if (!child) return false;
  size_t remembered_before = gNyGc.remembered_count;
  nyGcWriteBarrier((int64_t *)(dict_slot + 8), child);
  if (gNyGc.remembered_count > remembered_before) ++w->remembered_events;
  return true;
}

static bool header_is_tenured(int64_t obj);

static bool run_promotion_ladder(gc_worker_t *w, gc_rng_t *rng, int slot, int visible_roots) {
  if (w->threads != 1) return false;
  int parent_tag = (rng_next(rng) & 1u) ? GC_FUZZ_TAG_DICT : GC_FUZZ_TAG_LIST;
  nyGcLock();
  int64_t parent = alloc_tagged_body_unlocked(parent_tag);
  if (!parent) {
    nyGcUnlock();
    return false;
  }
  if (parent_tag == GC_FUZZ_TAG_DICT) {
    *(int64_t *)((uint8_t *)(uintptr_t)parent + 0) = tagged_int(1);
    *(int64_t *)((uint8_t *)(uintptr_t)parent + 8) = tagged_int(1);
    uint8_t *dict_slot = (uint8_t *)(uintptr_t)parent + 16u;
    *(int64_t *)dict_slot = tagged_int((int64_t)(rng_next(rng) & 0xffff));
    *(int64_t *)(dict_slot + 8) = tagged_int(0);
    *(int64_t *)(dict_slot + 16) = tagged_int(1);
  } else {
    *(int64_t *)((uint8_t *)(uintptr_t)parent + 0) = tagged_int(3);
    *(int64_t *)((uint8_t *)(uintptr_t)parent + 8) = tagged_int(3);
    int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)parent + 16);
    items[0] = tagged_int(0);
    items[1] = tagged_int((int64_t)(rng_next(rng) & 0xffff));
    items[2] = parent;
  }
  nyGcUnlock();
  nyGcWriteBarrier(&w->roots[slot], parent);
  for (int i = 0; i < NYGC_PROMOTION_AGE + 1; ++i) nyGcTriggerMinor();
  parent = w->roots[slot];
  if (!parent || !header_is_tenured(parent)) return false;

  int depth = 0;
  int64_t child = (rng_next(rng) & 1u) ? make_string_heavy(rng, w->roots, visible_roots, &depth)
                                       : make_result_nest(rng, w->roots, visible_roots, &depth);
  if (!child) return false;
  if (depth > w->max_depth) w->max_depth = depth;

  size_t remembered_before = gNyGc.remembered_count;
  if (parent_tag == GC_FUZZ_TAG_DICT) {
    uint8_t *dict_slot = (uint8_t *)(uintptr_t)parent + 16u;
    nyGcWriteBarrier((int64_t *)(dict_slot + 8), child);
  } else {
    int64_t *items = (int64_t *)((uint8_t *)(uintptr_t)parent + 16);
    nyGcWriteBarrier(&items[(int)(rng_next(rng) % 2u)], child);
  }
  if (gNyGc.remembered_count > remembered_before) ++w->remembered_events;
  if (rng_range(rng, 0, 99) < 70) nyGcTriggerMinor();
  if (rng_range(rng, 0, 99) < 10) nyGcTriggerMajor();
  return true;
}

static void *worker_main(void *arg) {
  gc_worker_t *w = (gc_worker_t *)arg;
  gc_set_scratch_roots(w->scratch_roots, w->scratch_count);
  gc_rng_t rng = {.state = w->seed ^ (UINT64_C(0x9e3779b97f4a7c15) * (uint64_t)(w->id + 1))};
  int root_begin = (GC_FUZZ_ROOTS * w->id) / w->threads;
  int root_end = (GC_FUZZ_ROOTS * (w->id + 1)) / w->threads;
  if (root_end <= root_begin) root_end = root_begin + 1;
  int visible_roots = w->threads == 1 ? GC_FUZZ_ROOTS : 0;

  for (int i = root_begin; i < root_end; ++i) {
    int depth = 0;
    pthread_mutex_lock(&g_harness_mutex);
    int64_t obj = make_mode_object(&rng, w->roots, visible_roots, GC_MODE_WIDE_GRAPH, &depth);
    if (depth > w->max_depth) w->max_depth = depth;
    if (!obj) {
      pthread_mutex_unlock(&g_harness_mutex);
      atomic_store(w->stop, 2);
      return NULL;
    }
    nyGcWriteBarrier(&w->roots[i], obj);
    pthread_mutex_unlock(&g_harness_mutex);
  }

  for (uint64_t i = 0; atomic_load(w->stop) == 0; ++i) {
    if (w->stop_on_deadline && now_ns() >= w->deadline_ns) break;
    if (!w->stop_on_deadline && i >= w->iterations) break;
    int slot = rng_range(&rng, root_begin, root_end - 1);
    gc_mode_t mode = choose_mode(&rng, w->forced_mode);
    ++w->mode_counts[mode];

    pthread_mutex_lock(&g_harness_mutex);
    if (mode == GC_MODE_REMEMBERED_CHURN && run_remembered_churn(w, &rng, slot, visible_roots)) {
      ++w->ops;
      if (w->explicit_minor_every > 0 && (i % (uint64_t)w->explicit_minor_every) == 0)
        nyGcTriggerMinor();
      pthread_mutex_unlock(&g_harness_mutex);
      continue;
    }
    if (mode == GC_MODE_PROMOTION_LADDER && run_promotion_ladder(w, &rng, slot, visible_roots)) {
      ++w->ops;
      pthread_mutex_unlock(&g_harness_mutex);
      continue;
    }

    int64_t obj = make_mode_object(&rng, w->roots, visible_roots, mode, &w->max_depth);
    if (!obj) {
      pthread_mutex_unlock(&g_harness_mutex);
      atomic_store(w->stop, 2);
      break;
    }
    nyGcWriteBarrier(&w->roots[slot], obj);

    if (mode == GC_MODE_MINOR_STORM || (w->explicit_minor_every > 0 && (i % (uint64_t)w->explicit_minor_every) == 0))
      nyGcTriggerMinor();
    if (mode == GC_MODE_MAJOR_STORM || (w->explicit_major_every > 0 && (i % (uint64_t)w->explicit_major_every) == 0))
      nyGcTriggerMajor();
    ++w->ops;
    pthread_mutex_unlock(&g_harness_mutex);
  }
  return NULL;
}

static bool header_is_tenured(int64_t obj) {
  if (!obj || !is_ptr(obj)) return false;
  nyGcHeader_t *header = nyGcHeaderFromObject(obj);
  return header && (header->flags & NYGC_TENURED) != 0;
}

static bool probe_dict_scan(int64_t *roots, uint64_t seed) {
  gc_rng_t rng = {.state = seed ^ UINT64_C(0xd1c710ad5eed)};
  int64_t child = make_list(&rng, roots, GC_FUZZ_ROOTS, false);
  int64_t dict = alloc_tagged_body(GC_FUZZ_TAG_DICT);
  if (!child || !dict) return false;

  *(int64_t *)((uint8_t *)(uintptr_t)dict + 0) = tagged_int(1);
  *(int64_t *)((uint8_t *)(uintptr_t)dict + 8) = tagged_int(1);
  uint8_t *slot = (uint8_t *)(uintptr_t)dict + 16u;
  *(int64_t *)slot = tagged_int(17);
  *(int64_t *)(slot + 8) = child;
  *(int64_t *)(slot + 16) = tagged_int(1);

  nyGcMark(dict);
  bool ok = nyGcIsMarked(child);
  nyGcTriggerMinor();
  return ok;
}

static bool probe_list_scan(uint64_t seed) {
  gc_rng_t rng = {.state = seed ^ UINT64_C(0x1157ca11feed)};
  int64_t child = make_scalar(&rng, GC_FUZZ_TAG_OK);
  int64_t list = alloc_tagged_body(GC_FUZZ_TAG_LIST);
  if (!child || !list) return false;

  *(int64_t *)((uint8_t *)(uintptr_t)list + 0) = tagged_int(1);
  *(int64_t *)((uint8_t *)(uintptr_t)list + 8) = tagged_int(1);
  *(int64_t *)((uint8_t *)(uintptr_t)list + 16) = child;

  nyGcMark(list);
  bool ok = nyGcIsMarked(child);
  nyGcTriggerMinor();
  return ok;
}

static bool probe_closure_scan(uint64_t seed) {
  gc_rng_t rng = {.state = seed ^ UINT64_C(0xc105ed5ca11)};
  int64_t child = make_list(&rng, NULL, 0, false);
  int64_t closure = alloc_tagged_body(GC_FUZZ_TAG_CLOSURE);
  if (!child || !closure) return false;

  *(int64_t *)((uint8_t *)(uintptr_t)closure + 0) = tagged_int((int64_t)(seed & 0xffff));
  *(int64_t *)((uint8_t *)(uintptr_t)closure + 8) = child;
  *(int64_t *)((uint8_t *)(uintptr_t)closure + 16) = child;

  nyGcMark(closure);
  bool ok = nyGcIsMarked(child);
  nyGcTriggerMinor();
  return ok;
}

static bool probe_result_scan(uint64_t seed) {
  gc_rng_t rng = {.state = seed ^ UINT64_C(0x0e55a17feed)};
  int64_t child = make_dict(&rng, NULL, 0);
  int64_t result = alloc_tagged_body((seed & 1u) ? GC_FUZZ_TAG_OK : GC_FUZZ_TAG_ERR);
  if (!child || !result) return false;

  *(int64_t *)((uint8_t *)(uintptr_t)result + 0) = child;
  *(int64_t *)((uint8_t *)(uintptr_t)result + 8) = tagged_int(0);

  nyGcMark(result);
  bool ok = nyGcIsMarked(child);
  nyGcTriggerMinor();
  return ok;
}

static bool probe_write_barrier(int64_t *roots, uint64_t seed) {
  gc_rng_t rng = {.state = seed ^ UINT64_C(0xba221e25eed)};
  int64_t parent = alloc_tagged_body(GC_FUZZ_TAG_DICT);
  if (!parent) return false;

  *(int64_t *)((uint8_t *)(uintptr_t)parent + 0) = tagged_int(1);
  *(int64_t *)((uint8_t *)(uintptr_t)parent + 8) = tagged_int(1);
  uint8_t *slot = (uint8_t *)(uintptr_t)parent + 16u;
  *(int64_t *)slot = tagged_int(29);
  *(int64_t *)(slot + 8) = tagged_int(31);
  *(int64_t *)(slot + 16) = tagged_int(1);

  nyGcWriteBarrier(&roots[1], parent);
  nyGcTriggerMinor();
  parent = roots[1];
  if (!header_is_tenured(parent)) return false;

  slot = (uint8_t *)(uintptr_t)parent + 16u;
  int64_t child = make_list(&rng, NULL, 0, false);
  if (!child || header_is_tenured(child)) return false;
  size_t remembered_before = gNyGc.remembered_count;
  nyGcWriteBarrier((int64_t *)(slot + 8), child);
  bool remembered = gNyGc.remembered_count > remembered_before;
  nyGcTriggerMinor();

  int64_t forwarded = *(int64_t *)(slot + 8);
  bool ok = remembered && forwarded && forwarded != child && header_is_tenured(forwarded);
  roots[1] = 0;
  return ok;
}

static size_t probe_promotion_delta(int64_t *roots, uint64_t seed) {
  gc_rng_t rng = {.state = seed ^ UINT64_C(0x9070b01dfeed)};
  roots[0] = make_dict(&rng, roots, GC_FUZZ_ROOTS);
  if (!roots[0]) return 0;
  size_t before = gNyGc.stats.objects_promoted;
  for (int i = 0; i < NYGC_PROMOTION_AGE + 1; ++i) nyGcTriggerMinor();
  return gNyGc.stats.objects_promoted - before;
}

static uint64_t arg_u64(int argc, char **argv, const char *name, uint64_t fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], name) == 0) return strtoull(argv[i + 1], NULL, 10);
  }
  return fallback;
}

static int arg_int(int argc, char **argv, const char *name, int fallback) {
  uint64_t v = arg_u64(argc, argv, name, (uint64_t)fallback);
  if (v > (uint64_t)INT32_MAX) return fallback;
  return (int)v;
}

static bool arg_flag(int argc, char **argv, const char *name) {
  for (int i = 1; i < argc; ++i)
    if (strcmp(argv[i], name) == 0) return true;
  return false;
}

static gc_mode_t arg_mode(int argc, char **argv) {
  const char *mode = NULL;
  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0) {
      mode = argv[i + 1];
      break;
    }
  }
  if (!mode || strcmp(mode, "smart") == 0 || strcmp(mode, "auto") == 0) return (gc_mode_t)-1;
  if (strcmp(mode, "alloc") == 0) return GC_MODE_ALLOC;
  if (strcmp(mode, "root") == 0 || strcmp(mode, "root-churn") == 0) return GC_MODE_ROOT_CHURN;
  if (strcmp(mode, "remembered") == 0 || strcmp(mode, "remembered-churn") == 0) return GC_MODE_REMEMBERED_CHURN;
  if (strcmp(mode, "minor") == 0 || strcmp(mode, "minor-storm") == 0) return GC_MODE_MINOR_STORM;
  if (strcmp(mode, "major") == 0 || strcmp(mode, "major-storm") == 0) return GC_MODE_MAJOR_STORM;
  if (strcmp(mode, "deep") == 0 || strcmp(mode, "deep-graph") == 0) return GC_MODE_DEEP_GRAPH;
  if (strcmp(mode, "wide") == 0 || strcmp(mode, "wide-graph") == 0) return GC_MODE_WIDE_GRAPH;
  if (strcmp(mode, "dict") == 0 || strcmp(mode, "dict-heavy") == 0) return GC_MODE_DICT_HEAVY;
  if (strcmp(mode, "closure") == 0 || strcmp(mode, "closure-result") == 0) return GC_MODE_CLOSURE_RESULT;
  if (strcmp(mode, "string") == 0 || strcmp(mode, "string-heavy") == 0) return GC_MODE_STRING_HEAVY;
  if (strcmp(mode, "result") == 0 || strcmp(mode, "result-nest") == 0) return GC_MODE_RESULT_NEST;
  if (strcmp(mode, "cycle") == 0 || strcmp(mode, "cycle-graph") == 0) return GC_MODE_CYCLE_GRAPH;
  if (strcmp(mode, "promotion") == 0 || strcmp(mode, "promotion-ladder") == 0) return GC_MODE_PROMOTION_LADDER;
  if (strcmp(mode, "mixed") == 0 || strcmp(mode, "mixed-runtime") == 0) return GC_MODE_MIXED_RUNTIME;
  return (gc_mode_t)-1;
}

static const char *mode_name(gc_mode_t mode) {
  switch (mode) {
  case GC_MODE_ALLOC: return "alloc";
  case GC_MODE_ROOT_CHURN: return "root-churn";
  case GC_MODE_REMEMBERED_CHURN: return "remembered-churn";
  case GC_MODE_MINOR_STORM: return "minor-storm";
  case GC_MODE_MAJOR_STORM: return "major-storm";
  case GC_MODE_DEEP_GRAPH: return "deep-graph";
  case GC_MODE_WIDE_GRAPH: return "wide-graph";
  case GC_MODE_DICT_HEAVY: return "dict-heavy";
  case GC_MODE_CLOSURE_RESULT: return "closure-result";
  case GC_MODE_STRING_HEAVY: return "string-heavy";
  case GC_MODE_RESULT_NEST: return "result-nest";
  case GC_MODE_CYCLE_GRAPH: return "cycle-graph";
  case GC_MODE_PROMOTION_LADDER: return "promotion-ladder";
  case GC_MODE_MIXED_RUNTIME: return "mixed-runtime";
  default: return "smart";
  }
}

int main(int argc, char **argv) {
  setenv("NYTRIX_GC", "1", 1);
  uint64_t seed = arg_u64(argc, argv, "--seed", UINT64_C(0xC0FFEE));
  uint64_t iterations = arg_u64(argc, argv, "--iterations", 250000);
  int seconds = arg_int(argc, argv, "--seconds", 0);
  int threads = arg_int(argc, argv, "--threads", 1);
  int minor_every = arg_int(argc, argv, "--minor-every", 97);
  int major_every = arg_int(argc, argv, "--major-every", 4099);
  bool require_promotions = arg_flag(argc, argv, "--require-promotions");
  bool validate_gc = arg_flag(argc, argv, "--validate-gc");
  gc_mode_t forced_mode = arg_mode(argc, argv);
  if (threads < 1) threads = 1;
  if (threads > GC_FUZZ_MAX_THREADS) threads = GC_FUZZ_MAX_THREADS;
  if (validate_gc) setenv("NYTRIX_GC_VALIDATE", "1", 1);
  uint64_t iterations_per_worker = iterations;
  if (seconds <= 0 && threads > 1) {
    iterations_per_worker = (iterations + (uint64_t)threads - 1u) / (uint64_t)threads;
    if (iterations_per_worker == 0) iterations_per_worker = 1;
  }

  nyGcInit();
  static int64_t roots[GC_FUZZ_ROOTS];
  static int64_t main_scratch[GC_FUZZ_SCRATCH_ROOTS];
  static int64_t worker_scratch[GC_FUZZ_MAX_THREADS][GC_FUZZ_SCRATCH_ROOTS];
  for (int i = 0; i < GC_FUZZ_ROOTS; ++i) nyGcAddRoot(&roots[i]);
  for (int i = 0; i < GC_FUZZ_SCRATCH_ROOTS; ++i) nyGcAddRoot(&main_scratch[i]);
  for (int i = 0; i < GC_FUZZ_MAX_THREADS; ++i)
    for (int j = 0; j < GC_FUZZ_SCRATCH_ROOTS; ++j)
      nyGcAddRoot(&worker_scratch[i][j]);
  gc_set_scratch_roots(main_scratch, GC_FUZZ_SCRATCH_ROOTS);
  bool dict_scan_ok = probe_dict_scan(roots, seed);
  bool list_scan_ok = probe_list_scan(seed);
  bool closure_scan_ok = probe_closure_scan(seed);
  bool result_scan_ok = probe_result_scan(seed);
  bool write_barrier_ok = probe_write_barrier(roots, seed);
  size_t promotion_probe_delta = probe_promotion_delta(roots, seed);

  _Atomic int stop = 0;
  pthread_t tids[GC_FUZZ_MAX_THREADS];
  gc_worker_t workers[GC_FUZZ_MAX_THREADS];
  uint64_t start = now_ns();
  uint64_t deadline = seconds > 0 ? start + (uint64_t)seconds * UINT64_C(1000000000) : 0;
  for (int i = 0; i < threads; ++i) {
    workers[i] = (gc_worker_t){
      .id = i,
      .threads = threads,
      .iterations = iterations_per_worker,
      .deadline_ns = deadline,
      .seed = seed,
      .explicit_minor_every = minor_every,
      .explicit_major_every = major_every,
      .stop_on_deadline = seconds > 0,
      .stop = &stop,
      .roots = roots,
      .scratch_roots = worker_scratch[i],
      .scratch_count = GC_FUZZ_SCRATCH_ROOTS,
      .ops = 0,
      .remembered_events = 0,
      .max_depth = 0,
      .max_roots_live = GC_FUZZ_ROOTS / threads,
      .forced_mode = forced_mode,
    };
    int rc = pthread_create(&tids[i], NULL, worker_main, &workers[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
      atomic_store(&stop, 3);
      threads = i;
      break;
    }
  }

  for (int i = 0; i < threads; ++i) pthread_join(tids[i], NULL);
  nyGcTriggerMajor();
  uint64_t ops = 0;
  uint64_t remembered_events = 0;
  uint64_t mode_counts[GC_MODE_COUNT] = {0};
  int max_depth = 0;
  int max_roots_live = 0;
  for (int i = 0; i < threads; ++i) ops += workers[i].ops;
  for (int i = 0; i < threads; ++i) {
    remembered_events += workers[i].remembered_events;
    if (workers[i].max_depth > max_depth) max_depth = workers[i].max_depth;
    max_roots_live += workers[i].max_roots_live;
    for (int m = 0; m < GC_MODE_COUNT; ++m) mode_counts[m] += workers[i].mode_counts[m];
  }
  double elapsed_s = (double)(now_ns() - start) / 1000000000.0;
  double ops_per_s = elapsed_s > 0.0 ? (double)ops / elapsed_s : 0.0;
  uint64_t tag_mask = atomic_load_explicit(&g_tag_mask, memory_order_relaxed);
  int tag_coverage = gc_popcount64(tag_mask);
  int mode_coverage = 0;
  for (int m = 0; m < GC_MODE_COUNT; ++m) {
    if (mode_counts[m] > 0) ++mode_coverage;
  }
  double graph_score = (double)tag_coverage * 35.0 +
                       (double)mode_coverage * 20.0 +
                       (double)max_depth * 10.0 +
                       (double)remembered_events * 5.0 +
                       (double)promotion_probe_delta * 25.0;
  double scheduler_score =
      (double)gNyGc.stats.objects_promoted * 4.0 +
      (double)(gNyGc.stats.nursery_collections + gNyGc.stats.tenured_collections) * 3.0 +
      (double)remembered_events * 25.0 +
      (double)max_depth * 8.0 +
      graph_score +
      (dict_scan_ok && list_scan_ok && closure_scan_ok && result_scan_ok && write_barrier_ok ? 100.0 : 0.0);

  int stop_value = atomic_load(&stop);
  bool ok = stop_value == 0 && dict_scan_ok && list_scan_ok && closure_scan_ok &&
            result_scan_ok && write_barrier_ok;
  if (require_promotions && gNyGc.stats.objects_promoted == 0) ok = false;
  char replay_args[256];
  if (seconds > 0) {
    snprintf(replay_args, sizeof(replay_args),
             "--seed %" PRIu64 " --threads %d --seconds %d --mode %s --minor-every %d --major-every %d%s%s",
             seed, threads, seconds, mode_name(forced_mode), minor_every, major_every,
             require_promotions ? " --require-promotions" : "",
             validate_gc ? " --validate-gc" : "");
  } else {
    snprintf(replay_args, sizeof(replay_args),
             "--seed %" PRIu64 " --threads %d --iterations %" PRIu64 " --mode %s --minor-every %d --major-every %d%s%s",
             seed, threads, iterations, mode_name(forced_mode), minor_every, major_every,
             require_promotions ? " --require-promotions" : "",
             validate_gc ? " --validate-gc" : "");
  }
  printf("{\"ok\":%s,\"seed\":%" PRIu64 ",\"threads\":%d,\"ops\":%" PRIu64
         ",\"elapsed_s\":%.3f,\"iterations\":%" PRIu64
         ",\"iterations_per_worker\":%" PRIu64
         ",\"nursery_allocated\":%zu,\"tenured_allocated\":%zu"
         ",\"minor_collections\":%zu,\"major_collections\":%zu"
         ",\"objects_promoted\":%zu,\"objects_swept\":%zu,\"bytes_freed\":%zu"
         ",\"heap_usage\":%zu,\"remembered_events\":%" PRIu64
         ",\"max_graph_depth\":%d,\"max_roots_live\":%d,\"scheduler_score\":%.2f"
         ",\"ops_per_s\":%.2f,\"graph_score\":%.2f,\"tag_coverage\":%d"
         ",\"tag_mask\":\"0x%llx\",\"mode_coverage\":%d,\"direct_mode\":\"%s\""
         ",\"dict_scan_ok\":%s,\"list_scan_ok\":%s"
         ",\"closure_scan_ok\":%s,\"result_scan_ok\":%s,\"write_barrier_ok\":%s"
         ",\"promotion_probe_delta\":%zu"
         ",\"mode_counts\":{\"alloc\":%" PRIu64 ",\"root_churn\":%" PRIu64
         ",\"remembered_churn\":%" PRIu64 ",\"minor_storm\":%" PRIu64
         ",\"major_storm\":%" PRIu64 ",\"deep_graph\":%" PRIu64
         ",\"wide_graph\":%" PRIu64 ",\"dict_heavy\":%" PRIu64
         ",\"closure_result\":%" PRIu64 ",\"string_heavy\":%" PRIu64
         ",\"result_nest\":%" PRIu64 ",\"cycle_graph\":%" PRIu64
         ",\"promotion_ladder\":%" PRIu64 ",\"mixed_runtime\":%" PRIu64 "}"
         ",\"replay_args\":\"%s\""
         ",\"stop\":%d,\"require_promotions\":%s,\"validation_ok\":%s}\n",
         ok ? "true" : "false", seed, threads, ops, elapsed_s,
         iterations, iterations_per_worker,
         gNyGc.stats.nursery_allocated, gNyGc.stats.tenured_allocated,
         gNyGc.stats.nursery_collections, gNyGc.stats.tenured_collections,
         gNyGc.stats.objects_promoted, gNyGc.stats.objects_swept,
         gNyGc.stats.bytes_freed, nyGcGetHeapUsage(),
         remembered_events, max_depth, max_roots_live, scheduler_score,
         ops_per_s, graph_score, tag_coverage, (unsigned long long)tag_mask,
         mode_coverage, mode_name(forced_mode),
         dict_scan_ok ? "true" : "false", list_scan_ok ? "true" : "false",
         closure_scan_ok ? "true" : "false", result_scan_ok ? "true" : "false",
         write_barrier_ok ? "true" : "false", promotion_probe_delta,
         mode_counts[GC_MODE_ALLOC], mode_counts[GC_MODE_ROOT_CHURN],
         mode_counts[GC_MODE_REMEMBERED_CHURN], mode_counts[GC_MODE_MINOR_STORM],
         mode_counts[GC_MODE_MAJOR_STORM], mode_counts[GC_MODE_DEEP_GRAPH],
         mode_counts[GC_MODE_WIDE_GRAPH], mode_counts[GC_MODE_DICT_HEAVY],
         mode_counts[GC_MODE_CLOSURE_RESULT], mode_counts[GC_MODE_STRING_HEAVY],
         mode_counts[GC_MODE_RESULT_NEST], mode_counts[GC_MODE_CYCLE_GRAPH],
         mode_counts[GC_MODE_PROMOTION_LADDER], mode_counts[GC_MODE_MIXED_RUNTIME],
         replay_args, stop_value,
         require_promotions ? "true" : "false", validate_gc ? "true" : "true");
  nyGcDispose();
  return ok ? 0 : (stop_value ? stop_value : 4);
}
