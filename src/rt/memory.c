#include "rt/shared.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern int64_t rt_copy_mem(int64_t dst, int64_t src, int64_t n);

/* Manual memset to avoid IFUNC issues with Vulkan driver */
#define memset_manual(p, v, n)                                                                     \
  do {                                                                                             \
    unsigned char *_p = (unsigned char *)(p);                                                      \
    unsigned char _v = (unsigned char)(v);                                                         \
    size_t _n = (n);                                                                               \
    while (_n-- > 0)                                                                               \
      *_p++ = _v;                                                                                  \
  } while (0)

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
    fprintf(stderr, "Leaked:    %" PRId64 " bytes\n", (int64_t)(g_alloc - g_free));
    fprintf(stderr, "Pool hits: %" PRIu64 " (%.1f%%)\n", g_pool_hits,
            g_alloc ? 100.0 * g_pool_hits / g_alloc : 0.0);
  }
}
#endif

atomic_uint_fast64_t g_ny_alloc_count = 0;
atomic_uint_fast64_t g_ny_realloc_count = 0;

#if defined(rt_SANITIZE_ADDRESS__)
#define NY_WITH_ASAN 1
#elif defined(rt_has_feature)
#if rt_has_feature(address_sanitizer)
#define NY_WITH_ASAN 1
#endif
#endif
#ifndef NY_WITH_ASAN
#define NY_WITH_ASAN 0
#endif

#if NY_WITH_ASAN
#define NY_TRACK_LIVE_ALLOCS 1
#else
#define NY_TRACK_LIVE_ALLOCS 0
#endif

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

#if NY_TRACK_LIVE_ALLOCS
static atomic_flag g_live_alloc_lock = ATOMIC_FLAG_INIT;
static void *g_live_alloc_head = NULL;

static inline void ny_live_alloc_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_live_alloc_lock, memory_order_acquire)) {
  }
}

static inline void ny_live_alloc_unlock(void) {
  atomic_flag_clear_explicit(&g_live_alloc_lock, memory_order_release);
}

static inline void **ny_live_prev_slot(void *base) { return (void **)((char *)base + 24); }

static inline void **ny_live_next_slot(void *base) { return (void **)((char *)base + 32); }

static inline uint64_t ny_live_size(void *base) { return *(uint64_t *)((char *)base + 8); }

static inline void ny_live_link(void *base) {
  if (!base)
    return;
  void **prev_slot = ny_live_prev_slot(base);
  void **next_slot = ny_live_next_slot(base);
  *prev_slot = NULL;
  *next_slot = g_live_alloc_head;
  if (g_live_alloc_head)
    *ny_live_prev_slot(g_live_alloc_head) = base;
  g_live_alloc_head = base;
}

static inline void ny_live_unlink(void *base) {
  if (!base)
    return;
  void **prev_slot = ny_live_prev_slot(base);
  void **next_slot = ny_live_next_slot(base);
  void *prev = *prev_slot;
  void *next = *next_slot;
  if (prev)
    *ny_live_next_slot(prev) = next;
  else if (g_live_alloc_head == base)
    g_live_alloc_head = next;
  if (next)
    *ny_live_prev_slot(next) = prev;
  *prev_slot = NULL;
  *next_slot = NULL;
}
#endif

#if NY_WITH_ASAN
static void **g_quarantine = NULL;
static size_t g_quarantine_len = 0;
static size_t g_quarantine_cap = 0;

static void quarantine_push(void *p) {
  if (!p)
    return;
  for (size_t i = 0; i < g_quarantine_len; ++i) {
    if (g_quarantine[i] == p)
      return;
  }
  if (g_quarantine_len == g_quarantine_cap) {
    size_t new_cap = g_quarantine_cap ? g_quarantine_cap * 2 : 1024;
    void **next = realloc(g_quarantine, new_cap * sizeof(void *));
    if (!next)
      return;
    g_quarantine = next;
    g_quarantine_cap = new_cap;
  }
  g_quarantine[g_quarantine_len++] = p;
}

__attribute__((destructor)) static void quarantine_drain(void) {
  for (size_t i = 0; i < g_quarantine_len; ++i)
    ny_aligned_free(g_quarantine[i]);
  free(g_quarantine);
  g_quarantine = NULL;
  g_quarantine_len = 0;
  g_quarantine_cap = 0;
}
#endif

static const size_t g_pool_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
#define NUM_POOLS (sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]))
typedef struct mem_pool_node {
  struct mem_pool_node *next;
} mem_pool_node_t;
_Thread_local mem_pool_node_t *g_mem_pools[NUM_POOLS] = {0};
__thread uintptr_t rt_heap_ptr_cache_keys[RT_HEAP_PTR_CACHE_SIZE] = {0};
__thread uint64_t rt_heap_ptr_cache_epoch = 0;
_Atomic uint64_t rt_heap_ptr_global_epoch = 1;

typedef struct ny_rc_entry {
  uintptr_t ptr;
  uint64_t count;
  struct ny_rc_entry *next;
} ny_rc_entry_t;

#define NY_RC_BUCKETS 4096u
static atomic_flag g_rc_lock = ATOMIC_FLAG_INIT;
static ny_rc_entry_t *g_rc_table[NY_RC_BUCKETS] = {0};
static int g_rc_enabled = -1;

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

static inline size_t rt_rc_bucket(uintptr_t ptr) { return (ptr >> 4) & (NY_RC_BUCKETS - 1u); }

static ny_rc_entry_t *rt_rc_find_locked(uintptr_t ptr, ny_rc_entry_t ***prev_next) {
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

int64_t rt_malloc(int64_t size) {
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
  for (size_t i = 0; i < fill_size; i++)
    ((unsigned char *)p)[i] = 0;

  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = (uint64_t)((body << 1) | 1);

  int64_t res = (int64_t)(uintptr_t)((char *)p + 32);
  rt_heap_ptr_cache_store((uintptr_t)res);
  rt_rc_adopt_new(res);
  if (mem_trace_enabled() && total > 1024 * 1024) {
    fprintf(stderr, "[mem] large alloc %p (body=%zu, total=%zu)\n", (void *)(uintptr_t)res, body,
            total);
  }
  return res;
}

int64_t rt_malloc_raw(int64_t size) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  if (n <= 0)
    n = 1;
  void *p = malloc((size_t)n);
  if (!p)
    return 0;
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
    rt_heap_ptr_cache_forget((uintptr_t)ptr);
    void *base = (char *)(uintptr_t)ptr - 32;
    if (mem_trace_enabled()) {
      size_t body = *(uint64_t *)((char *)base + 8);
      if (body & 1)
        body >>= 1;
      if (body > 1024 * 1024) {
        fprintf(stderr, "[mem] large free  %p (body=%zu)\n", (void *)(uintptr_t)ptr, body);
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
  } else if (is_v_flt(ptr)) {
    rt_flt_free(ptr);
    return 1;
  }
  return 0;
}

int64_t rt_free(int64_t ptr) {
  if (is_heap_ptr(ptr))
    rt_rc_forget(ptr);
  return rt_free_direct(ptr);
}

int64_t rt_free_raw(int64_t ptr) {
  if (!ptr)
    return 1;
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
  if (!slot_ptr)
    return 0;
  int64_t *slot = (int64_t *)(uintptr_t)slot_ptr;
  int64_t v = *slot;
  *slot = 0;
  return rt_drop_owned(v);
}

int64_t rt_runtime_cleanup(void) {
  extern int64_t rt_print_flush(void);
  rt_print_flush();
  rt_cleanup_args();
  rt_cleanup_small_strings();
#if NY_TRACK_LIVE_ALLOCS
  while (1) {
    ny_live_alloc_lock();
    void *base = g_live_alloc_head;
    if (base)
      ny_live_unlink(base);
    ny_live_alloc_unlock();
    if (!base)
      break;
#ifndef NDEBUG
    g_free += (size_t)ny_live_size(base) + 128;
#endif
    *(uint64_t *)base = 0;
    *(uint64_t *)((char *)base + 16) = 0;
    ny_aligned_free(base);
  }
#endif
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

  int64_t res = rt_malloc(newsz << 1 | 1);
  if (!res)
    return 0;

  memcpy((void *)(uintptr_t)res, (void *)(uintptr_t)p_val, old_cap);
  // Transfer Nytrix header (length and tag)
  *(int64_t *)((char *)(uintptr_t)res - 16) = *(int64_t *)((char *)(uintptr_t)p_val - 16);
  *(int64_t *)((char *)(uintptr_t)res - 8) = *(int64_t *)((char *)(uintptr_t)p_val - 8);

  atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
  rt_free(p_val);
  return res;
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
    return 1;
  int res = 0;
  const char *s1 = (const char *)(uintptr_t)a;
  const char *s2 = (const char *)(uintptr_t)b;
  for (size_t i = 0; i < (size_t)n; ++i) {
    if (s1[i] != s2[i]) {
      res = s1[i] - s2[i];
      break;
    }
  }
  return (int64_t)(res << 1) | 1;
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
  *out = (((int64_t)*((const uint8_t *)(uintptr_t)addr + (size_t)idx)) << 1) | 1;
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
  return is_int(value) ? ((value >> 1) << 1) : value;
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
  return atomic_fetch_add_explicit(slot, rt_atomic_i64_delta(delta), memory_order_seq_cst);
}

int64_t rt_atomic_sub64(int64_t addr, int64_t idx, int64_t delta) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_fetch_sub_explicit(slot, rt_atomic_i64_delta(delta), memory_order_seq_cst);
}

int64_t rt_atomic_exchange64(int64_t addr, int64_t idx, int64_t value) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_exchange_explicit(slot, value, memory_order_seq_cst);
}

int64_t rt_atomic_cas64(int64_t addr, int64_t idx, int64_t expected, int64_t desired) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return NY_IMM_FALSE;
  int64_t exp = expected;
  return atomic_compare_exchange_strong_explicit(slot, &exp, desired, memory_order_seq_cst,
                                                 memory_order_seq_cst)
             ? NY_IMM_TRUE
             : NY_IMM_FALSE;
}
