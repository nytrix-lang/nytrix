#include "rt/shared.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>

extern int64_t __copy_mem(int64_t dst, int64_t src, int64_t n);

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

__attribute__((destructor)) static void __stats(void) {
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

#if defined(__SANITIZE_ADDRESS__)
#define NY_WITH_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
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
  while (atomic_flag_test_and_set_explicit(&g_live_alloc_lock,
                                           memory_order_acquire)) {
  }
}

static inline void ny_live_alloc_unlock(void) {
  atomic_flag_clear_explicit(&g_live_alloc_lock, memory_order_release);
}

static inline void **ny_live_prev_slot(void *base) {
  return (void **)((char *)base + 24);
}

static inline void **ny_live_next_slot(void *base) {
  return (void **)((char *)base + 32);
}

static inline uint64_t ny_live_size(void *base) {
  return *(uint64_t *)((char *)base + 8);
}

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

int64_t __malloc(int64_t size) {
  if (is_int(size))
    size >>= 1;
  if (size < 0)
    return 0;

  size_t n = (size_t)size;
  size_t body = (n + 15) & ~15ULL;
  size_t total = body + 32;

  void *p = ny_aligned_alloc(16, total);
  if (!p)
    return 0;

  memset(p, 0, total);
  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = NY_MAGIC2;
  *(uint64_t *)((char *)p + 16) = (uint64_t)body;

  int64_t res = (int64_t)(uintptr_t)((char *)p + 32);
  if (mem_trace_enabled() && total > 1024 * 1024) {
    fprintf(stderr, "[mem] large alloc %p (body=%zu, total=%zu)\n",
            (void *)(uintptr_t)res, body, total);
  }
  return res;
}

int64_t __free(int64_t ptr) {
  if (is_heap_ptr(ptr)) {
    void *base = (char *)(uintptr_t)ptr - 32;
    if (mem_trace_enabled()) {
      size_t body = *(uint64_t *)((char *)base + 16);
      if (body > 1024 * 1024) {
        fprintf(stderr, "[mem] large free  %p (body=%zu)\n",
                (void *)(uintptr_t)ptr, body);
      }
    }
    ny_aligned_free(base);
  } else if (is_v_flt(ptr)) {
    // Light float object (slab allocated)
    __flt_free(ptr);
  }
  return 0;
}

int64_t __runtime_cleanup(void) {
  __cleanup_args();
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

int64_t __realloc(int64_t p_val, int64_t newsz) {
  if (is_int(newsz))
    newsz >>= 1;
  if (newsz < 0)
    newsz = 0;
  if (!is_heap_ptr(p_val))
    return __malloc(newsz << 1 | 1);

  char *cap_ptr = (char *)(uintptr_t)p_val - 16;
  size_t old_cap = *(uint64_t *)cap_ptr;
  if ((size_t)newsz <= old_cap)
    return p_val;

  int64_t res = __malloc(newsz << 1 | 1);
  if (!res)
    return 0;

  memcpy((void *)(uintptr_t)res, (void *)(uintptr_t)p_val, old_cap);
  // Transfer tag
  *(int64_t *)((char *)(uintptr_t)res - 8) =
      *(int64_t *)((char *)(uintptr_t)p_val - 8);

  __free(p_val);
  return res;
}

int64_t __memcpy(int64_t dst, int64_t src, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return dst;
  if (!__check_oob("memcpy_dst", dst, 0, (size_t)n))
    return dst;
  if (!__check_oob("memcpy_src", src, 0, (size_t)n))
    return dst;
  __copy_mem(dst, src, (n << 1) | 1);
  return dst;
}

int64_t __memset(int64_t dst, int64_t v, int64_t n) {
  if (is_int(v))
    v >>= 1;
  if (is_int(n))
    n >>= 1;
  if (n > 0)
    memset((void *)(uintptr_t)dst, (int)(v & 0xff), (size_t)n);
  return dst;
}

int64_t __memcmp(int64_t a, int64_t b, int64_t n) {
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

int64_t __load8_idx(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 1 > sz)
      return 1;
  } else if (!__check_oob("load8", addr, idx, 1)) {
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

int64_t __load16_idx(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 2 > sz)
      return 1;
  } else if (!__check_oob("load16", addr, idx, 2)) {
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

int64_t __load32_idx(int64_t addr, int64_t idx) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 4 > sz)
      return 1;
  } else if (!__check_oob("load32", addr, idx, 4)) {
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

int64_t __load64_idx(int64_t addr, int64_t idx) {
  if (is_int(idx) && is_heap_ptr(addr)) {
    intptr_t off = (intptr_t)(idx >> 1);
    if (off >= 0) {
      size_t sz = __get_heap_size(addr);
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
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 8 > sz)
      return 0;
  } else if (!__check_oob("load64", addr, idx, 8)) {
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

int64_t __store8_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 1 > sz)
      return val;
  } else if (!__check_oob("store8", addr, idx, 1)) {
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

int64_t __store16_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 2 > sz)
      return val;
  } else if (!__check_oob("store16", addr, idx, 2)) {
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

int64_t __store32_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx))
    idx >>= 1;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 4 > sz)
      return val;
  } else if (!__check_oob("store32", addr, idx, 4)) {
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

int64_t __store64_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx) && is_heap_ptr(addr)) {
    intptr_t off = (intptr_t)(idx >> 1);
    if (off >= 0) {
      size_t sz = __get_heap_size(addr);
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
    size_t sz = __get_heap_size(addr);
    if ((size_t)idx + 8 > sz)
      return val;
  } else if (!__check_oob("store64", addr, idx, 8)) {
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
