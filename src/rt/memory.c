#include "rt/shared.h"

#ifndef NDEBUG
static uint64_t g_alloc = 0;
static uint64_t g_free = 0;
static uint64_t g_pool_hits = 0;

__attribute__((destructor)) static void __stats(void) {
  const char *env = getenv("NYTRIX_MEM_STATS");
  if (env && *env == '1') {
    fprintf(stderr, "\n━━━ Nytrix Runtime Stats ━━━\n");
    fprintf(stderr, "Allocated: %lu bytes\n", g_alloc);
    fprintf(stderr, "Freed:     %lu bytes\n", g_free);
    fprintf(stderr, "Leaked:    %ld bytes\n", (long)(g_alloc - g_free));
    fprintf(stderr, "Pool hits: %lu (%.1f%%)\n", g_pool_hits,
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
    free(g_quarantine[i]);
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
  if (size < 64)
    size = 64;
  size = (size + 63) & ~63ULL;
  size_t total = (size_t)size + 128;
  void *p = aligned_alloc(64, total);
  if (!p)
    return 0;
  // memset(p, 0, total);
  char *z = (char *)p;
  for (size_t i = 0; i < total; ++i)
    z[i] = 0;
#ifndef NDEBUG
  g_alloc += total;
#endif
  int64_t res = (int64_t)(uintptr_t)((char *)p + 64);
  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = (uint64_t)size;
  *(uint64_t *)((char *)p + 16) = NY_MAGIC2;
  *(uint64_t *)((char *)p + 64 + size) = NY_MAGIC3;
  return res;
}

int64_t __free(int64_t ptr) {
  if (is_heap_ptr(ptr)) {
    size_t sz = __get_heap_size(ptr);
    (void)sz;
#ifndef NDEBUG
    size_t total = sz + 128;
    g_free += total;
    if (*(uint64_t *)((char *)(uintptr_t)ptr + sz) != NY_MAGIC3) {
      // fprintf(stderr, "[MEM] Overflow detected at 0x%lx\n", (unsigned
      // long)ptr);
    }
#endif
    void *base = (char *)(uintptr_t)ptr - 64;
    // Clear allocation sentinels so repeated free attempts are ignored.
    *(uint64_t *)base = 0;
    *(uint64_t *)((char *)base + 16) = 0;
#if NY_WITH_ASAN
    // Avoid ASAN false-positive cascades from runtime pointer probes after
    // free.
    quarantine_push(base);
#else
    free(base);
#endif
  }
  return 0;
}

int64_t __realloc(int64_t p_val, int64_t newsz) {
  if (is_int(newsz))
    newsz >>= 1;
  if (newsz < 0)
    newsz = 0;
  if (!is_heap_ptr(p_val))
    return __malloc(
        newsz << 1 |
        1); // was checking if p_val is not heap, treat as new malloc? yes
  char *op = (char *)(uintptr_t)p_val - 64;
  size_t old_size = *(uint64_t *)(op + 8);
  if ((size_t)newsz <= old_size) {
    return p_val;
  }
  int64_t res = __malloc(newsz << 1 | 1);
  if (!res)
    return 0;
  // Copy user data
  memcpy((void *)(uintptr_t)res, (void *)(uintptr_t)p_val, old_size);
  // Copy metadata from the header (p-16 and p-8)
  *(int64_t *)((char *)(uintptr_t)res - 16) =
      *(int64_t *)((char *)(uintptr_t)p_val - 16);
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
  __copy_mem((void *)(uintptr_t)dst, (void *)(uintptr_t)src, (size_t)n);
  return dst;
}

int64_t __memset(int64_t dst, int64_t v, int64_t n) {
  if (is_int(v))
    v >>= 1;
  if (is_int(n))
    n >>= 1;
  if (n > 0) {
    char *d = (char *)(uintptr_t)dst;
    for (size_t i = 0; i < (size_t)n; ++i)
      d[i] = (char)v;
  }
  return dst;
}

int64_t __memcmp(int64_t a, int64_t b, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return 1; // tagged 0
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
  if (!is_any_ptr(addr))
    return 1;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("load8", addr, idx, 1))
    return 1;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  int64_t val = (((int64_t)*(uint8_t *)p) << 1) | 1;
  return val;
}

int64_t __load16_idx(int64_t addr, int64_t idx) {
  if (!is_any_ptr(addr))
    return 1;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("load16", addr, idx, 2))
    return 1;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  return (((int64_t)*(uint16_t *)p) << 1) | 1;
}

int64_t __load32_idx(int64_t addr, int64_t idx) {
  if (!is_any_ptr(addr))
    return 1;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("load32", addr, idx, 4))
    return 1;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  return (((int64_t)*(uint32_t *)p) << 1) | 1;
}

int64_t __load64_idx(int64_t addr, int64_t idx) {
  if (!is_any_ptr(addr))
    return 0;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("load64", addr, idx, 8))
    return 0;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 0;
  return *(int64_t *)p;
}

int64_t __store8_idx(int64_t addr, int64_t idx, int64_t val) {
  if (!is_any_ptr(addr))
    return val;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("store8", addr, idx, 1))
    return val;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  *(uint8_t *)p = (uint8_t)v;
  return val;
}

int64_t __store16_idx(int64_t addr, int64_t idx, int64_t val) {
  if (!is_any_ptr(addr))
    return val;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("store16", addr, idx, 2))
    return val;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  *(uint16_t *)p = (uint16_t)v;
  return val;
}

int64_t __store32_idx(int64_t addr, int64_t idx, int64_t val) {
  if (!is_any_ptr(addr))
    return val;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("store32", addr, idx, 4))
    return val;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  *(uint32_t *)p = (uint32_t)v;
  return val;
}

int64_t __store64_idx(int64_t addr, int64_t idx, int64_t val) {
  // if (idx == 0) printf("DEBUG: __store64_idx addr=%lx val=%lx\n", addr, val);
  if (!is_any_ptr(addr))
    return val;
  if (is_int(idx))
    idx >>= 1;
  if (!__check_oob("store64", addr, idx, 8))
    return val;
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  *(int64_t *)p = val;
  return val;
}
