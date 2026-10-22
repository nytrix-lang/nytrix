/*
 * Memory runtime: allocator, deallocator, realloc, zero-init, and
 * heap management primitives with ownership and GC integration hooks.
 */
#include "code/lang/runtime/shared.h"
void rt_str_forget_pointer(int64_t v);
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern int64_t rt_copy_mem(int64_t dst, int64_t src, int64_t n);

/*
 * The float wrapper is defined before the general indexed store below.
 */
int64_t rt_store32_idx(int64_t addr, int64_t idx, int64_t val);

int g_rt_zero_init = 1; /* default: always zero (current behavior) */
static int g_rt_zero_init_cached = 0;
static inline bool rt_zero_init_enabled(void) {
  if (g_rt_zero_init_cached)
    return g_rt_zero_init != 0;
  const char *env = getenv("NYTRIX_ZERO_INIT");
  g_rt_zero_init = !(env && (*env == '0' || strcmp(env, "false") == 0));
  g_rt_zero_init_cached = 1;
  return g_rt_zero_init != 0;
}

#ifdef _WIN32
#include <malloc.h>
#endif

static int g_mem_trace = -1;
static inline bool mem_trace_enabled(void) {
  if (g_mem_trace >= 0)
    return g_mem_trace != 0;
  const char *env = getenv("NYTRIX_MEM_TRACE");
  g_mem_trace = (env && (*env == '1' || strcmp(env, "true") == 0)) ? 1 : 0;
  return g_mem_trace != 0;
}

#ifndef NDEBUG
static uint64_t g_alloc = 0;
static uint64_t g_free = 0;
static uint64_t g_pool_hits = 0;

__attribute__((destructor)) static void rt_stats(void) {
  const char *env = getenv("NYTRIX_MEM_STATS");
  if (env && *env == '1') {
    fprintf(stderr, "\n━━━ Nytrix Runtime Stats ━━━\n");
    fprintf(stderr, "Allocated: %" PRIu64 " bytes\n", g_alloc);
    fprintf(stderr, "Freed:     %" PRIu64 " bytes\n", g_free);
    fprintf(stderr, "Leaked:    %" PRId64 " bytes\n",
            (int64_t)(g_alloc - g_free));
    fprintf(stderr, "Pool hits: %" PRIu64 " (%.1f%%)\n", g_pool_hits,
            g_alloc ? 100.0 * g_pool_hits / g_alloc : 0.0);
  }
}
#endif

atomic_uint_fast64_t g_ny_alloc_count = 0;
atomic_uint_fast64_t g_ny_realloc_count = 0;

static inline void *ny_aligned_alloc(size_t alignment, size_t size) {
#ifdef _WIN32
  return _aligned_malloc(size, alignment);
#else
  if (alignment <= _Alignof(max_align_t))
    return malloc(size);
  void *p = NULL;
  if (posix_memalign(&p, alignment, size) != 0)
    return NULL;
  return p;
#endif
}

static inline void ny_aligned_free(void *p) {
#ifdef _WIN32
  _aligned_free(p);
#else
  free(p);
#endif
}

/*
 * Finer segregated classes keep the 32-byte header while avoiding the former
 * power-of-two fragmentation cliff.  All entries retain 16-byte alignment.
 */
static const size_t g_pool_sizes[] = {32,   48,   64,   80,   96,   112,  128,
                                      160,  192,  224,  256,  320,  384,  448,
                                      512,  640,  768,  896,  1024, 1280, 1536,
                                      1792, 2048, 2560, 3072, 3584, 4096};
#define NUM_POOLS (sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]))
typedef struct mem_pool_node {
  struct mem_pool_node *next;
} mem_pool_node_t;
_Thread_local mem_pool_node_t *g_mem_pools[NUM_POOLS] = {0};

/*
 * Exact provenance for objects returned by the managed allocator.  GC objects
 * are tracked by the collector's object descriptors; this table covers the
 * separate 32-byte-header allocator without reading user-controlled memory.
 */
#define RT_MANAGED_PTR_TABLE_BITS 22u
#define RT_MANAGED_PTR_TABLE_SIZE (1u << RT_MANAGED_PTR_TABLE_BITS)
#define RT_MANAGED_PTR_TABLE_MASK (RT_MANAGED_PTR_TABLE_SIZE - 1u)
#define RT_MANAGED_PTR_TOMBSTONE UINTPTR_MAX
static _Atomic(uintptr_t) rt_managed_ptr_table[RT_MANAGED_PTR_TABLE_SIZE];
static _Atomic(uint64_t) rt_managed_ptr_epoch;
#define RT_MANAGED_PTR_CACHE_SIZE 8u
typedef struct rt_managed_ptr_cache_entry {
  uintptr_t value;
  uint64_t epoch;
} rt_managed_ptr_cache_entry_t;
static _Thread_local rt_managed_ptr_cache_entry_t
    rt_managed_ptr_cache[RT_MANAGED_PTR_CACHE_SIZE];

static size_t rt_managed_ptr_slot(uintptr_t value) {
  /*
   * Managed payloads are aligned and commonly adjacent. Keep their low
   * address bits local in the table, folding higher regions into the slot
   * instead of scattering every allocation onto an unrelated cache line.
   * Exact keys and open-addressing probes still resolve collisions.
   */
  uint64_t x = (uint64_t)value >> 4;
  x ^= x >> RT_MANAGED_PTR_TABLE_BITS;
  x ^= x >> (2u * RT_MANAGED_PTR_TABLE_BITS);
  return (size_t)x & RT_MANAGED_PTR_TABLE_MASK;
}

static void rt_managed_ptr_register(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return;
  size_t start = rt_managed_ptr_slot(value);
  size_t tombstone = SIZE_MAX;
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    size_t slot = (start + probe) & RT_MANAGED_PTR_TABLE_MASK;
    uintptr_t seen =
        atomic_load_explicit(&rt_managed_ptr_table[slot], memory_order_acquire);
    if (seen == value)
      return;
    if (seen == RT_MANAGED_PTR_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = slot;
    if (!seen) {
      size_t target = tombstone == SIZE_MAX ? slot : tombstone;
      uintptr_t expected = tombstone == SIZE_MAX ? 0 : RT_MANAGED_PTR_TOMBSTONE;
      if (atomic_compare_exchange_strong_explicit(
              &rt_managed_ptr_table[target], &expected, value,
              memory_order_release, memory_order_relaxed))
        return;
      probe = (size_t)-1;
      tombstone = SIZE_MAX;
    }
  }
  if (tombstone != SIZE_MAX) {
    uintptr_t expected = RT_MANAGED_PTR_TOMBSTONE;
    (void)atomic_compare_exchange_strong_explicit(
        &rt_managed_ptr_table[tombstone], &expected, value,
        memory_order_release, memory_order_relaxed);
  }
}

static void rt_managed_ptr_unregister(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return;
  size_t start = rt_managed_ptr_slot(value);
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    size_t slot = (start + probe) & RT_MANAGED_PTR_TABLE_MASK;
    uintptr_t seen =
        atomic_load_explicit(&rt_managed_ptr_table[slot], memory_order_acquire);
    if (!seen)
      return;
    if (seen == value) {
      if (atomic_compare_exchange_strong_explicit(
              &rt_managed_ptr_table[slot], &seen, RT_MANAGED_PTR_TOMBSTONE,
              memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&rt_managed_ptr_epoch, 1,
                                  memory_order_release);
        return;
      }
      probe = (size_t)-1;
    }
  }
}

bool rt_heap_ptr_registry_has(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return false;
  size_t start = rt_managed_ptr_slot(value);
  uint64_t epoch =
      atomic_load_explicit(&rt_managed_ptr_epoch, memory_order_acquire);
  rt_managed_ptr_cache_entry_t *cached =
      &rt_managed_ptr_cache[start & (RT_MANAGED_PTR_CACHE_SIZE - 1u)];
  if (cached->value == value && cached->epoch == epoch)
    return true;
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    uintptr_t seen = atomic_load_explicit(
        &rt_managed_ptr_table[(start + probe) & RT_MANAGED_PTR_TABLE_MASK],
        memory_order_acquire);
    if (seen == value) {
      cached->value = value;
      cached->epoch = epoch;
      return true;
    }
    if (!seen)
      return false;
  }
  return false;
}

typedef struct ny_rc_entry {
  uintptr_t ptr;
  uint64_t count;
  struct ny_rc_entry *next;
} ny_rc_entry_t;

#define NY_RC_BUCKETS 4096u
static atomic_flag g_rc_lock = ATOMIC_FLAG_INIT;
static ny_rc_entry_t *g_rc_table[NY_RC_BUCKETS] = {0};
static int g_rc_enabled = -1;

void rt_rc_set_enabled(int enabled) { g_rc_enabled = enabled != 0; }

static inline bool rt_rc_enabled(void) {
  if (g_rc_enabled >= 0)
    return g_rc_enabled != 0;
  const char *policy = getenv("NYTRIX_HEAP_POLICY");
  const char *rc = getenv("NYTRIX_RC_GC");
  g_rc_enabled = ((policy && strcmp(policy, "rc") == 0) ||
                  (rc && (*rc == '1' || strcmp(rc, "true") == 0)))
                     ? 1
                     : 0;
  return g_rc_enabled != 0;
}

static inline void rt_rc_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_rc_lock, memory_order_acquire)) {
  }
}

static inline void rt_rc_unlock(void) {
  atomic_flag_clear_explicit(&g_rc_lock, memory_order_release);
}

static inline size_t rt_rc_bucket(uintptr_t ptr) {
  return (ptr >> 4) & (NY_RC_BUCKETS - 1u);
}

static ny_rc_entry_t *rt_rc_find_locked(uintptr_t ptr,
                                        ny_rc_entry_t ***prev_next) {
  size_t b = rt_rc_bucket(ptr);
  ny_rc_entry_t **slot = &g_rc_table[b];
  while (*slot) {
    if ((*slot)->ptr == ptr) {
      if (prev_next)
        *prev_next = slot;
      return *slot;
    }
    slot = &(*slot)->next;
  }
  if (prev_next)
    *prev_next = slot;
  return NULL;
}

static void rt_rc_adopt_new(int64_t ptr) {
  if (!rt_rc_enabled() || !ptr)
    return;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (entry) {
    entry->count = 1;
  } else {
    entry = (ny_rc_entry_t *)calloc(1, sizeof(*entry));
    if (entry) {
      entry->ptr = p;
      entry->count = 1;
      entry->next = *slot;
      *slot = entry;
    }
  }
  rt_rc_unlock();
}

static void rt_rc_forget(int64_t ptr) {
  if (!ptr)
    return;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (entry) {
    *slot = entry->next;
    free(entry);
  }
  rt_rc_unlock();
}

static inline int ny_mem_pool_slot(size_t total) {
  for (int i = 0; i < (int)NUM_POOLS; i++) {
    if (total <= g_pool_sizes[i])
      return i;
  }
  return -1;
}

/*
 * Two independent heap systems, NOT interconvertible:
 *
 *   GC-traced heap: rt_malloc / rt_malloc_uninit / rt_free / rt_realloc.
 *     Each allocation carries a 32-byte header (NY_MAGIC1 + body size); the
 *     returned pointer is base+32.  rt_free inspects the header, recycles
 *     small allocations through per-size-class pools, and frees large ones.
 *
 *   Raw heap: rt_malloc_raw / rt_free_raw.
 *     A plain malloc/free with no header and no GC participation, used for
 *     unmanaged FFI/native buffers.
 *
 * Crossing the boundary is an error: free_raw on a traced pointer would free
 * the wrong address (base+32), and rt_free on a raw pointer is a silent no-op
 * (leak).  rt_free_raw detects and re-routes traced pointers; callers must
 * keep the two heaps separate.
 */
static int64_t rt_malloc_impl(int64_t size, int zero_fill) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  if (n < 0)
    return 0;
  size_t body = (size_t)n;
  body = (body + 15) & ~15ULL;
  size_t total = body + 32;

  int slot = ny_mem_pool_slot(total);
  void *p = NULL;
  if (slot >= 0 && g_mem_pools[slot]) {
    p = g_mem_pools[slot];
    g_mem_pools[slot] = g_mem_pools[slot]->next;
  } else {
    p = ny_aligned_alloc(16, (slot >= 0) ? g_pool_sizes[slot] : total);
  }

  if (__builtin_expect(!p, 0))
    return 0;

  size_t fill_size = (slot >= 0) ? g_pool_sizes[slot] : total;
  if (zero_fill && rt_zero_init_enabled())
    memset(p, 0, fill_size);

  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = (uint64_t)((body << 1) | 1);
  /*
   * Object metadata must be initialized even when payload zeroing is off.
   * A recycled string header otherwise makes a fresh byte buffer look like
   * the previous string, including its old length.
   */
  *(uint64_t *)((char *)p + 16) = 0;
  *(uint64_t *)((char *)p + 24) = 0;

  int64_t res = (int64_t)(uintptr_t)((char *)p + 32);
  atomic_fetch_add_explicit(&g_ny_alloc_count, 1, memory_order_relaxed);
  rt_map_oracle_add((uintptr_t)p, (size_t)total);
  rt_managed_ptr_register((uintptr_t)res);
  rt_rc_adopt_new(res);
  if (mem_trace_enabled() && total > 1024 * 1024) {
    fprintf(stderr, "[mem] large alloc %p (body=%zu, total=%zu)\n",
            (void *)(uintptr_t)res, body, total);
  }
  return res;
}

int64_t rt_malloc(int64_t size) { return rt_malloc_impl(size, 1); }

int64_t rt_malloc_uninit(int64_t size) { return rt_malloc_impl(size, 0); }

int64_t rt_malloc_i64(int64_t size) {
  if (size < 0)
    return 0;
  size_t body = (size_t)size;
  body = (body + 15) & ~15ULL;
  size_t total = body + 32;

  int slot = ny_mem_pool_slot(total);
  void *p = NULL;
  if (slot >= 0 && g_mem_pools[slot]) {
    p = g_mem_pools[slot];
    g_mem_pools[slot] = g_mem_pools[slot]->next;
  } else {
    p = ny_aligned_alloc(16, (slot >= 0) ? g_pool_sizes[slot] : total);
  }

  if (__builtin_expect(!p, 0))
    return 0;

  size_t fill_size = (slot >= 0) ? g_pool_sizes[slot] : total;
  if (rt_zero_init_enabled())
    memset(p, 0, fill_size);

  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = (uint64_t)((body << 1) | 1);
  /*
   * Object metadata must be initialized even when payload zeroing is off.
   * A recycled string header otherwise makes a fresh byte buffer look like
   * the previous string, including its old length.
   */
  *(uint64_t *)((char *)p + 16) = 0;
  *(uint64_t *)((char *)p + 24) = 0;

  int64_t res = (int64_t)(uintptr_t)((char *)p + 32);
  atomic_fetch_add_explicit(&g_ny_alloc_count, 1, memory_order_relaxed);
  rt_map_oracle_add((uintptr_t)p, (size_t)total);
  rt_managed_ptr_register((uintptr_t)res);
  rt_rc_adopt_new(res);
  if (mem_trace_enabled() && total > 1024 * 1024) {
    fprintf(stderr, "[mem] large alloc %p (body=%zu, total=%zu)\n",
            (void *)(uintptr_t)res, body, total);
  }
  return res;
}

/*
 * The interpreter passes tagged integer offsets to runtime primitives.
 */
int64_t rt_ptr_add(int64_t ptr, int64_t offset) {
  return ptr + (is_int(offset) ? rt_untag_v(offset) : offset);
}

int64_t rt_ptr_sub(int64_t ptr, int64_t offset) {
  return ptr - (is_int(offset) ? rt_untag_v(offset) : offset);
}

/*
 * NYIR passes typed integer offsets unboxed, so it needs an explicit bridge.
 */
int64_t rt_ptr_add_i64(int64_t ptr, int64_t offset) { return ptr + offset; }
int64_t rt_ptr_sub_i64(int64_t ptr, int64_t offset) { return ptr - offset; }
typedef struct raw_ptr_node {
  uintptr_t ptr;
  size_t size;
  struct raw_ptr_node *next;
  struct raw_ptr_node *range_next;
} raw_ptr_node_t;

#define RAW_PTR_HASH_BUCKETS 2048
static atomic_flag g_raw_ptr_lock = ATOMIC_FLAG_INIT;
static raw_ptr_node_t *g_raw_ptr_table[RAW_PTR_HASH_BUCKETS] = {0};
static raw_ptr_node_t *g_raw_ptr_ranges = NULL;

static inline void raw_ptr_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_raw_ptr_lock,
                                           memory_order_acquire)) {
  }
}

static inline void raw_ptr_unlock(void) {
  atomic_flag_clear_explicit(&g_raw_ptr_lock, memory_order_release);
}

static inline size_t raw_ptr_bucket(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12)) & (RAW_PTR_HASH_BUCKETS - 1);
}

static void raw_ptr_register(uintptr_t p, size_t size) {
  if (!p)
    return;
  size_t b = raw_ptr_bucket(p);
  raw_ptr_lock();
  for (raw_ptr_node_t *old = g_raw_ptr_table[b]; old; old = old->next) {
    if (old->ptr == p) {
      if (size > old->size) {
        old->size = size;
        old->range_next = g_raw_ptr_ranges;
        g_raw_ptr_ranges = old;
      }
      raw_ptr_unlock();
      return;
    }
  }
  raw_ptr_node_t *node = (raw_ptr_node_t *)malloc(sizeof(raw_ptr_node_t));
  if (node) {
    node->ptr = p;
    node->size = size;
    node->next = g_raw_ptr_table[b];
    node->range_next = NULL;
    g_raw_ptr_table[b] = node;
    if (size) {
      node->range_next = g_raw_ptr_ranges;
      g_raw_ptr_ranges = node;
    }
  }
  raw_ptr_unlock();
}

void rt_raw_ptr_register(int64_t ptr) { raw_ptr_register((uintptr_t)ptr, 0); }

void rt_raw_ptr_register_range(int64_t ptr, size_t size) {
  raw_ptr_register((uintptr_t)ptr, size);
}

static bool raw_ptr_unregister(uintptr_t p) {
  if (!p)
    return false;
  size_t b = raw_ptr_bucket(p);
  raw_ptr_lock();
  raw_ptr_node_t **slot = &g_raw_ptr_table[b];
  while (*slot) {
    if ((*slot)->ptr == p) {
      raw_ptr_node_t *match = *slot;
      *slot = match->next;
      if (match->size) {
        raw_ptr_node_t **range = &g_raw_ptr_ranges;
        while (*range && *range != match)
          range = &(*range)->range_next;
        if (*range)
          *range = match->range_next;
      }
      free(match);
      raw_ptr_unlock();
      return true;
    }
    slot = &(*slot)->next;
  }
  raw_ptr_unlock();
  return false;
}

bool rt_raw_ptr_unregister(int64_t ptr) {
  return raw_ptr_unregister((uintptr_t)ptr);
}

bool rt_raw_ptr_registered(int64_t ptr) {
  uintptr_t p = (uintptr_t)ptr;
  if (!p)
    return false;
  size_t b = raw_ptr_bucket(p);
  bool found = false;
  raw_ptr_lock();
  for (raw_ptr_node_t *node = g_raw_ptr_table[b]; node; node = node->next) {
    if (p == node->ptr) {
      found = true;
      break;
    }
  }
  if (!found) {
    for (raw_ptr_node_t *node = g_raw_ptr_ranges; node;
         node = node->range_next) {
      if (p >= node->ptr && p - node->ptr < node->size) {
        found = true;
        break;
      }
    }
  }
  raw_ptr_unlock();
  return found;
}

static bool rt_raw_ptr_is_allocation_base(int64_t ptr) {
  uintptr_t p = (uintptr_t)ptr;
  if (!p)
    return false;
  size_t b = raw_ptr_bucket(p);
  bool found = false;
  raw_ptr_lock();
  for (raw_ptr_node_t *node = g_raw_ptr_table[b]; node; node = node->next) {
    if (p == node->ptr) {
      found = node->size != 0;
      break;
    }
  }
  raw_ptr_unlock();
  return found;
}

int64_t rt_malloc_raw(int64_t size) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  return rt_malloc_raw_i64(n);
}

/*
 * Native NYIR passes unboxed integer arguments, so raw allocation sizes must
 * not be classified from their low bit: an odd byte count is still raw.
 */
int64_t rt_malloc_raw_i64(int64_t size) {
  int64_t n = size;
  if (n <= 0)
    n = 1;
  void *p = malloc((size_t)n);
  if (!p)
    return 0;
  rt_map_oracle_add((uintptr_t)p, (size_t)n);
  rt_raw_ptr_register_range((int64_t)(uintptr_t)p, (size_t)n);
  return (int64_t)(uintptr_t)p;
}

int64_t rt_ptr_key(int64_t ptr) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "0x%016" PRIx64, (uint64_t)(uintptr_t)ptr);
  if (n <= 0)
    return rt_alloc_string("0x0000000000000000");
  if ((size_t)n >= sizeof(buf))
    n = (int)sizeof(buf) - 1;
  return rt_alloc_string_len(buf, (size_t)n);
}

static int64_t rt_free_direct(int64_t ptr) {
  if (is_heap_ptr(ptr)) {
    rt_managed_ptr_unregister((uintptr_t)ptr);
    void *base = (char *)(uintptr_t)ptr - 32;
    if (mem_trace_enabled()) {
      size_t body = *(uint64_t *)((char *)base + 8);
      if (body & 1)
        body >>= 1;
      if (body > 1024 * 1024) {
        fprintf(stderr, "[mem] large free  %p (body=%zu)\n",
                (void *)(uintptr_t)ptr, body);
      }
    }
    size_t body = *(uint64_t *)((char *)base + 8);
    if (body & 1)
      body >>= 1;
    size_t total = ((body + 15) & ~15ULL) + 32;
    int slot = ny_mem_pool_slot(total);
    if (slot >= 0) {
      mem_pool_node_t *node = (mem_pool_node_t *)base;
      node->next = g_mem_pools[slot];
      g_mem_pools[slot] = node;
      return 1;
    }
    ny_aligned_free(base);
    return 1;
  } else if (raw_ptr_unregister((uintptr_t)ptr)) {
    free((void *)(uintptr_t)ptr);
    return 1;
  } else if (is_v_flt(ptr)) {
    rt_flt_free(ptr);
    return 1;
  }
  return 0;
}

int64_t rt_free(int64_t ptr) {
  if (is_heap_ptr(ptr))
    rt_rc_forget(ptr);
  rt_str_forget_pointer(ptr);
  return rt_free_direct(ptr);
}

int64_t rt_free_raw(int64_t ptr) {
  if (!ptr)
    return 1;
  int tbuf_release = rt_tbuf_release_handle(ptr);
  if (tbuf_release > 0) {
    free((void *)((uintptr_t)ptr - RT_NATIVE_TBUF_HEADER));
    return 1;
  }
  if (tbuf_release < 0) {
    fprintf(stderr,
            "[mem] free_raw ignored a statically registered typed buffer\n");
    return 0;
  }
  /*
   * Provenance comes from allocator registries; pointer contents are never
   * consulted to distinguish the two allocation families.
   */
  if (is_heap_ptr(ptr)) {
    fprintf(stderr,
            "[mem] cross-system free: GC-traced pointer passed to free_raw; "
            "rerouting to rt_free\n");
    return rt_free(ptr);
  }
  raw_ptr_unregister((uintptr_t)ptr);
  free((void *)(uintptr_t)ptr);
  return 1;
}

int64_t rt_retain_owned(int64_t ptr) {
  if (!rt_rc_enabled() || !is_heap_ptr(ptr))
    return ptr;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (entry) {
    entry->count++;
  } else {
    entry = (ny_rc_entry_t *)calloc(1, sizeof(*entry));
    if (entry) {
      entry->ptr = p;
      entry->count = 1;
      entry->next = *slot;
      *slot = entry;
    }
  }
  rt_rc_unlock();
  return ptr;
}

int64_t rt_release_owned(int64_t ptr) {
  if (!rt_rc_enabled())
    return rt_free(ptr);
  if (!is_heap_ptr(ptr)) {
    if (is_v_flt(ptr))
      return rt_free(ptr);
    return 0;
  }
  uintptr_t p = (uintptr_t)ptr;
  bool should_free = false;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (!entry) {
    should_free = true;
  } else if (entry->count <= 1) {
    *slot = entry->next;
    free(entry);
    should_free = true;
  } else {
    entry->count--;
  }
  rt_rc_unlock();
  return should_free ? rt_free_direct(ptr) : 1;
}

int64_t rt_rc_count(int64_t ptr) {
  if (!rt_rc_enabled() || !is_heap_ptr(ptr))
    return 0;
  uint64_t count = 0;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t *entry = rt_rc_find_locked(p, NULL);
  if (entry)
    count = entry->count;
  rt_rc_unlock();
  return (int64_t)((count << 1) | 1u);
}

int64_t rt_drop_owned(int64_t ptr) { return rt_release_owned(ptr); }

int64_t rt_drop_owned_slot(int64_t slot_ptr) {
  if (!is_ptr(slot_ptr))
    return 0;
  int64_t *slot = (int64_t *)(uintptr_t)slot_ptr;
  int64_t v = *slot;
  *slot = 0;
  return rt_drop_owned(v);
}

int64_t rt_pool_release(void) {
  int64_t freed_count = 0;
  for (size_t i = 0; i < NUM_POOLS; i++) {
    mem_pool_node_t *node = g_mem_pools[i];
    g_mem_pools[i] = NULL;
    while (node) {
      mem_pool_node_t *next = node->next;
      ny_aligned_free(node);
      freed_count++;
      node = next;
    }
  }
  return rt_tag_v(freed_count);
}

/*
 * Native AOT entries can return directly to the host CRT. Report requested
 * counters at process exit as well as explicit runtime cleanup, once only.
 */
static atomic_bool g_ny_counter_reported = ATOMIC_VAR_INIT(false);
static void rt_report_runtime_counters(void) {
  const char *counter_report = getenv("NYTRIX_REPORT_RUNTIME_COUNTERS");
  if (counter_report && *counter_report && strcmp(counter_report, "0") != 0 &&
      strcmp(counter_report, "false") != 0 &&
      strcmp(counter_report, "off") != 0) {
    if (atomic_exchange_explicit(&g_ny_counter_reported, true,
                                 memory_order_relaxed))
      return;
    uint_fast64_t allocs =
        atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
    uint_fast64_t reallocs =
        atomic_load_explicit(&g_ny_realloc_count, memory_order_relaxed);
    fprintf(stderr,
            "ny_runtime_counters alloc=%" PRIuFAST64 " realloc=%" PRIuFAST64
            "\n",
            allocs, reallocs);
  }
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void
rt_report_runtime_counters_at_exit(void) {
  rt_report_runtime_counters();
}
#endif

int64_t rt_runtime_cleanup(void) {
  extern int64_t rt_print_flush(void);
  rt_print_flush();
  rt_async_cleanup();
  rt_report_runtime_counters();
  const char *emit_report = getenv("NYTRIX_EMIT_REPORT");
  if (emit_report && *emit_report) {
    FILE *out = fopen(emit_report, "ab");
    if (out) {
      extern atomic_uint_fast64_t g_ny_gc_alloc_count;
      uint_fast64_t allocs =
          atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
      uint_fast64_t gc_allocs =
          atomic_load_explicit(&g_ny_gc_alloc_count, memory_order_relaxed);
      allocs = UINT_FAST64_MAX - allocs < gc_allocs ? UINT_FAST64_MAX
                                                    : allocs + gc_allocs;
      fprintf(out, "runtime.allocation_calls=%" PRIuFAST64 "\n", allocs);
      fprintf(out, "runtime.reallocation_calls=%" PRIuFAST64 "\n",
              atomic_load_explicit(&g_ny_realloc_count, memory_order_relaxed));
      rt_trace_profile_write(out);
      nyGcWriteTelemetry(out);
      fclose(out);
    } else {
      fprintf(stderr, "runtime report: cannot append %s: %s\n", emit_report,
              strerror(errno));
    }
  }
  const char *mem_stats = getenv("NYTRIX_MEM_STATS");
  if (mem_stats && *mem_stats && strcmp(mem_stats, "0") != 0 &&
      strcmp(mem_stats, "false") != 0 && strcmp(mem_stats, "off") != 0)
    nyGcWriteTelemetry(stderr);
  rt_cleanup_args();
  rt_cleanup_small_strings();
  rt_pool_release();
  return 0;
}

int64_t rt_realloc(int64_t p_val, int64_t newsz) {
  if (is_int(newsz))
    newsz >>= 1;
  if (newsz < 0)
    newsz = 0;
  if (!is_heap_ptr(p_val))
    return rt_malloc(newsz << 1 | 1);
  size_t old_cap = *(uint64_t *)((char *)(uintptr_t)p_val - 24);
  if (old_cap & 1)
    old_cap >>= 1;
  if ((size_t)newsz <= old_cap)
    return p_val;

  /*
   * A pooled allocation owns the whole size-class chunk, even though the
   * public heap-size header records only the requested body.  Growth that
   * remains in that same class is therefore genuinely in place: publish the
   * larger logical capacity and zero the newly exposed bytes when required.
   */
  size_t old_total = ((old_cap + 15) & ~15ULL) + 32;
  size_t new_body = ((size_t)newsz + 15) & ~15ULL;
  size_t new_total = new_body + 32;
  int old_slot = ny_mem_pool_slot(old_total);
  int new_slot = ny_mem_pool_slot(new_total);
  if (old_slot >= 0 && old_slot == new_slot) {
    if (rt_zero_init_enabled())
      memset((char *)(uintptr_t)p_val + old_cap, 0, (size_t)newsz - old_cap);
    *(uint64_t *)((char *)(uintptr_t)p_val - 24) =
        (uint64_t)((new_body << 1) | 1u);
    atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
    return p_val;
  }

  int64_t res = rt_malloc(newsz << 1 | 1);
  if (!res)
    return 0;

  memcpy((void *)(uintptr_t)res, (void *)(uintptr_t)p_val, old_cap);

  *(int64_t *)((char *)(uintptr_t)res - 16) =
      *(int64_t *)((char *)(uintptr_t)p_val - 16);
  *(int64_t *)((char *)(uintptr_t)res - 8) =
      *(int64_t *)((char *)(uintptr_t)p_val - 8);

  atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
  rt_free(p_val);
  return res;
}

int64_t rt_realloc_raw(int64_t p, int64_t size) {
  if (size <= 0) {
    if (is_heap_ptr(p))
      rt_free(p);
    else
      rt_free_raw(p);
    return 0;
  }
  /*
   * Explicit __malloc users retain managed allocation ownership.
   */
  if (is_heap_ptr(p))
    return rt_realloc(p, rt_tag_v(size < 0 ? 0 : size));
  /*
   * Native malloc/free own ordinary C allocations, without the managed
   * heap prefix. Keep realloc in that same allocation family and preserve
   * the old bytes; rt_realloc would replace an untracked pointer with a
   * fresh managed allocation that rt_zfree_raw cannot release.
   */
  if (is_int(p) || (p && (p < 4096 || !rt_addr_readable((uintptr_t)p, 1))))
    return 0;
  if (p && !rt_raw_ptr_is_allocation_base(p)) {
    /*
     * An addressable pointer outside both registries is an ordinary libc
     * allocation (e.g. the result of an `extern` calloc). libc realloc
     * accepts it directly; failing here made every realloc of an
     * extern-allocated buffer return NULL on every route
     * (alloc-provenance oracle).
     */
    void *foreign = realloc((void *)(uintptr_t)p, (size_t)size);
    if (foreign)
      rt_raw_ptr_register_range((int64_t)(uintptr_t)foreign, (size_t)size);
    return (int64_t)(uintptr_t)foreign;
  }
  void *grown = realloc((void *)(uintptr_t)p, (size_t)size);
  if (grown) {
    if (p)
      raw_ptr_unregister((uintptr_t)p);
    rt_raw_ptr_register_range((int64_t)(uintptr_t)grown, (size_t)size);
  }
  return (int64_t)(uintptr_t)grown;
}

int64_t rt_memcpy(int64_t dst, int64_t src, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return dst;
  if (!rt_check_oob("memcpy_dst", dst, 0, (size_t)n))
    return dst;
  if (!rt_check_oob("memcpy_src", src, 0, (size_t)n))
    return dst;
  rt_copy_mem(dst, src, (n << 1) | 1);
  return dst;
}

int64_t rt_memset(int64_t dst, int64_t v, int64_t n) {
  if (is_int(v))
    v >>= 1;
  if (is_int(n))
    n >>= 1;
  if (n > 0) {
    unsigned char *p = (unsigned char *)(uintptr_t)dst;
    unsigned char c = (unsigned char)(v & 0xff);
    while (n-- > 0)
      *p++ = c;
  }
  return dst;
}

int64_t rt_memcmp(int64_t a, int64_t b, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return 0;
  int res = 0;
  const char *s1 = (const char *)(uintptr_t)a;
  const char *s2 = (const char *)(uintptr_t)b;
  for (size_t i = 0; i < (size_t)n; ++i) {
    if (s1[i] != s2[i]) {
      res = s1[i] - s2[i];
      break;
    }
  }
  /*
   * Keep the libc-compatible raw comparison result.  This primitive is used
   * as a byte-level predicate and its public contract is the conventional
   * negative/zero/positive result; tagging here makes equal buffers appear as
   * one to native callers while the legacy backend returns zero.
   */
  return (int64_t)res;
}

/*
 * Native NYIR passes scalar arguments unboxed. Keep that ABI explicit rather
 * than guessing whether an odd length is raw or tagged at runtime.
 */
int64_t rt_memcpy_raw(int64_t dst, int64_t src, int64_t n) {
  if (n <= 0)
    return dst;
  if (!rt_check_oob("memcpy_dst", dst, 0, (size_t)n) ||
      !rt_check_oob("memcpy_src", src, 0, (size_t)n))
    return dst;
  rt_copy_mem(dst, src, (n << 1) | 1);
  return dst;
}

int64_t rt_memset_raw(int64_t dst, int64_t v, int64_t n) {
  if (n <= 0)
    return dst;
  unsigned char *p = (unsigned char *)(uintptr_t)dst;
  unsigned char c = (unsigned char)(v & 0xff);
  while (n-- > 0)
    *p++ = c;
  return dst;
}

int64_t rt_memcmp_raw(int64_t a, int64_t b, int64_t n) {
  if (n <= 0)
    return 0;
  const unsigned char *s1 = (const unsigned char *)(uintptr_t)a;
  const unsigned char *s2 = (const unsigned char *)(uintptr_t)b;
  for (size_t i = 0; i < (size_t)n; ++i) {
    if (s1[i] != s2[i])
      return (int64_t)s1[i] - (int64_t)s2[i];
  }
  return 0;
}

static inline int rt_try_load8_str(int64_t addr, int64_t idx, int64_t *out) {
  if (!out || idx < 0 || !is_v_str(addr))
    return 0;
  uintptr_t lp = (uintptr_t)addr - 16u;
  if (!rt_addr_readable(lp, sizeof(int64_t)))
    return 0;
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)lp, sizeof(tagged_len));
  if (!is_int(tagged_len))
    return 0;
  size_t len = (size_t)(tagged_len >> 1);
  if ((size_t)idx >= len)
    return 0;
  *out =
      (((int64_t)*((const uint8_t *)(uintptr_t)addr + (size_t)idx)) << 1) | 1;
  return 1;
}

int64_t rt_load8_idx(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  int64_t str_val = 0;
  if (rt_try_load8_str(addr, idx, &str_val))
    return str_val;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 1 > sz)
      return 1;
  } else if (!rt_check_oob("load8", addr, idx, 1)) {
    return 1;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  if (!hdr && !heap && !rt_addr_readable(p, 1))
    return 1;
  int64_t val = (((int64_t)*(uint8_t *)p) << 1) | 1;
  return val;
}

/*
 * Safe byte access for a dynamic string value. Native lowering passes the
 * string representation directly, while rt_any_to_cstr accepts both the
 * tagged VM representation and native string pointers.
 */
int64_t rt_str_byte(int64_t value, int64_t index) {
  if (index < 0)
    return 0;
  if (!is_v_str(value) && !rt_native_is_str(value))
    return 0;
  const char *s = (const char *)(uintptr_t)value;
  size_t len = (size_t)rt_cstr_len(value);
  if ((size_t)index >= len)
    return 0;
  return (unsigned char)s[index];
}

int64_t rt_load16_idx(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 2 > sz)
      return 1;
  } else if (!rt_check_oob("load16", addr, idx, 2)) {
    return 1;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  if (!hdr && !heap && !rt_addr_readable(p, 2))
    return 1;
  uint16_t v = 0;
  if ((p & (uintptr_t)1u) == 0) {
    v = *(const uint16_t *)p;
  } else {
    memcpy(&v, (const void *)p, sizeof(v));
  }
  return (((int64_t)v) << 1) | 1;
}

int64_t rt_load32_idx(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 4 > sz)
      return 1;
  } else if (!rt_check_oob("load32", addr, idx, 4)) {
    return 1;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  if (!hdr && !heap && !rt_addr_readable(p, 4))
    return 1;
  uint32_t v = 0;
  if ((p & (uintptr_t)3u) == 0) {
    v = *(const uint32_t *)p;
  } else {
    memcpy(&v, (const void *)p, sizeof(v));
  }
  return (((int64_t)v) << 1) | 1;
}

double rt_load32_f64(int64_t addr, int64_t idx) {
  int64_t tagged = rt_load32_idx(addr, idx);
  uint32_t bits = (uint32_t)(rt_untag_v(tagged) & UINT32_MAX);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return (double)value;
}

int64_t rt_store32_f64(int64_t addr, int64_t idx, double value) {
  float narrowed = (float)value;
  uint32_t bits = 0;
  memcpy(&bits, &narrowed, sizeof(bits));
  return rt_store32_idx(addr, idx, rt_tag_v((int64_t)bits));
}

int64_t rt_load64_idx(int64_t addr, int64_t idx) {
  if (is_int(idx) && is_heap_ptr(addr)) {
    intptr_t off = (intptr_t)(idx >> 1);
    if (off >= 0) {
      size_t sz = rt_get_heap_size_known(addr);
      if ((size_t)off + 8 > sz)
        return 0;
    } else if (off < -32) {
      return 0;
    }
    uintptr_t p = (uintptr_t)((intptr_t)addr + off);
    if (p < 0x1000)
      return 0;
    if ((p & (uintptr_t)7u) == 0) {
      return *(const int64_t *)p;
    }
    int64_t v = 0;
    memcpy(&v, (const void *)p, sizeof(v));
    return v;
  }
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 0;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 8 > sz)
      return 0;
  } else if (!rt_check_oob("load64", addr, idx, 8)) {
    return 0;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 0;
  if (!hdr && !heap && !rt_addr_readable(p, 8))
    return 0;
  if ((p & (uintptr_t)7u) == 0) {
    return *(const int64_t *)p;
  }
  int64_t v = 0;
  memcpy(&v, (const void *)p, sizeof(v));
  return v;
}

int64_t rt_store8_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 1 > sz)
      return val;
  } else if (!rt_check_oob("store8", addr, idx, 1)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_readable(p, 1))
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  *(uint8_t *)p = (uint8_t)v;
  return val;
}

int64_t rt_store16_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 2 > sz)
      return val;
  } else if (!rt_check_oob("store16", addr, idx, 2)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_readable(p, 2))
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  uint16_t raw = (uint16_t)v;
  if ((p & (uintptr_t)1u) == 0) {
    *(uint16_t *)p = raw;
  } else {
    memcpy((void *)p, &raw, sizeof(raw));
  }
  return val;
}

int64_t rt_store32_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 4 > sz)
      return val;
  } else if (!rt_check_oob("store32", addr, idx, 4)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_readable(p, 4))
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  uint32_t raw = (uint32_t)v;
  if ((p & (uintptr_t)3u) == 0) {
    *(uint32_t *)p = raw;
  } else {
    memcpy((void *)p, &raw, sizeof(raw));
  }
  return val;
}

int64_t rt_store64_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx) && is_heap_ptr(addr)) {
    intptr_t off = (intptr_t)(idx >> 1);
    if (off >= 0) {
      size_t sz = rt_get_heap_size_known(addr);
      if ((size_t)off + 8 > sz)
        return val;
    } else if (off < -32) {
      return val;
    }
    uintptr_t p = (uintptr_t)((intptr_t)addr + off);
    if (p < 0x1000)
      return val;
    int64_t raw = val;
    if (off == -8 && is_int(raw)) {
      int64_t u = raw >> 1;
      if (u >= 100 && u <= 255)
        raw = u;
    } else if (off == -16 && !is_int(raw)) {
      raw = rt_tag_v(raw);
    }
    if ((p & (uintptr_t)7u) == 0) {
      *(int64_t *)p = raw;
    } else {
      memcpy((void *)p, &raw, sizeof(raw));
    }
    return val;
  }
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 8 > sz)
      return val;
  } else if (!rt_check_oob("store64", addr, idx, 8)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_readable(p, 8))
    return val;
  int64_t raw = val;
  if (idx == -8 && is_int(raw)) {
    int64_t u = raw >> 1;
    if (u >= 100 && u <= 255)
      raw = u;
  } else if (idx == -16 && !is_int(raw)) {
    raw = rt_tag_v(raw);
  }
  if ((p & (uintptr_t)7u) == 0) {
    *(int64_t *)p = raw;
  } else {
    memcpy((void *)p, &raw, sizeof(raw));
  }
  return val;
}

int64_t rt_load64_h(int64_t p, int64_t i) {
  int64_t raw = rt_load64_idx(p, i);
  return rt_tag_v(raw);
}

int64_t rt_load32_h(int64_t p, int64_t i) { return rt_load32_idx(p, i); }

int64_t rt_store64_h(int64_t p, int64_t i, int64_t v) {
  int64_t raw = rt_untag_v(v);
  return rt_store64_idx(p, i, raw);
}

static _Atomic int64_t *rt_atomic_i64_slot(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  if ((intptr_t)idx < 0)
    return NULL;
  bool heap = is_heap_ptr(addr);
  if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 8 > sz)
      return NULL;
  } else if (!rt_check_oob("atomic64", addr, idx, 8)) {
    return NULL;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000 || (p & (uintptr_t)7u) != 0)
    return NULL;
  if (!heap && !rt_addr_readable(p, 8))
    return NULL;
  return (_Atomic int64_t *)p;
}

static inline int64_t rt_atomic_i64_delta(int64_t value) {
  /*
   * Cells hold RAW i64 words (rt_atomic_load/store are untyped), so a
   * tagged-integer argument must be untagged before use. The previous
   * `(v >> 1) << 1` dropped the tag bit but kept the DOUBLED magnitude
   * (tagged(5)=11 -> 10 instead of 5).
   */
  return is_int(value) ? rt_untag_v(value) : value;
}

int64_t rt_atomic_load64(int64_t addr, int64_t idx) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_load_explicit(slot, memory_order_seq_cst);
}

int64_t rt_atomic_store64(int64_t addr, int64_t idx, int64_t value) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return value;
  atomic_store_explicit(slot, value, memory_order_seq_cst);
  return value;
}

int64_t rt_atomic_add64(int64_t addr, int64_t idx, int64_t delta) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_fetch_add_explicit(slot, rt_atomic_i64_delta(delta),
                                   memory_order_seq_cst);
}

int64_t rt_atomic_sub64(int64_t addr, int64_t idx, int64_t delta) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_fetch_sub_explicit(slot, rt_atomic_i64_delta(delta),
                                   memory_order_seq_cst);
}

int64_t rt_atomic_exchange64(int64_t addr, int64_t idx, int64_t value) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_exchange_explicit(slot, value, memory_order_seq_cst);
}

int64_t rt_atomic_cas64(int64_t addr, int64_t idx, int64_t expected,
                        int64_t desired) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return NY_IMM_FALSE;
  int64_t exp = expected;
  return atomic_compare_exchange_strong_explicit(
             slot, &exp, desired, memory_order_seq_cst, memory_order_seq_cst)
             ? NY_IMM_TRUE
             : NY_IMM_FALSE;
}
