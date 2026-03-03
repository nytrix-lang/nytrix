#ifndef RT_COMMON_H
#define RT_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define NY_MAGIC1 0x545249584E5954ULL
#define NY_MAGIC2 0x4E59545249584EULL
#define NY_MAGIC3 0xDEADBEEFCAFEBABEULL

#define is_int(v) ((v) & 1)
#define is_ptr(v) (((v) & 1) == 0 && (uintptr_t)(v) > 0x1000)

static inline int rt_addr_mapped(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0) return 0;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {0};
  if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return 0;
  if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return 0;
  return 1;
#else
  static long ps = 0;
  if (ps == 0) ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 4096;
  uintptr_t mask = (uintptr_t)ps - 1;
  uintptr_t start = p & ~mask;
  uintptr_t end = (p + n - 1) & ~mask;
#ifdef __APPLE__
  char vec = 0;
#else
  unsigned char vec = 0;
#endif
  for (uintptr_t cur = start; cur <= end; cur += (uintptr_t)ps) {
    if (mincore((void *)cur, (size_t)ps, &vec) != 0) return 0;
  }
  return 1;
#endif
}

#define RT_PAGE_CACHE_BITS 10
#define RT_PAGE_CACHE_SIZE (1 << RT_PAGE_CACHE_BITS)
#define RT_PAGE_CACHE_MASK (RT_PAGE_CACHE_SIZE - 1)

static inline int rt_addr_readable(uintptr_t p, size_t n) {
  static __thread uintptr_t last_pg = 0;
  static __thread uintptr_t cache[RT_PAGE_CACHE_SIZE];
  if (p < 0x1000 || n == 0) return 0;
  if (p > UINTPTR_MAX - n) return 0;
  
  uintptr_t pg1 = p & ~4095ULL;
  if (pg1 == last_pg) {
    uintptr_t pg2 = (p + n - 1) & ~4095ULL;
    if (pg1 == pg2) return 1;
  }
  
  uintptr_t h1 = (pg1 >> 12) & RT_PAGE_CACHE_MASK;
  if (cache[h1] == pg1) {
    uintptr_t pg2 = (p + n - 1) & ~4095ULL;
    if (pg1 == pg2) { last_pg = pg1; return 1; }
    uintptr_t h2 = (pg2 >> 12) & RT_PAGE_CACHE_MASK;
    if (cache[h2] == pg2) { last_pg = pg1; return 1; }
  }
  
  if (rt_addr_mapped(p, n)) {
    cache[h1] = pg1;
    last_pg = pg1;
    uintptr_t pg2 = (p + n - 1) & ~4095ULL;
    if (pg2 != pg1) {
      uintptr_t h2 = (pg2 >> 12) & RT_PAGE_CACHE_MASK;
      cache[h2] = pg2;
    }
    return 1;
  }
  return 0;
}

static inline bool is_valid_heap_ptr(int64_t v) {
  if (!(v > 0x1000 && (v & 15) == 0)) return false;
  uintptr_t p = (uintptr_t)v;
  if (!rt_addr_readable(p - 32, 32)) return false;
  uint64_t m1 = *(uint64_t *)(p - 32);
  uint64_t m2 = *(uint64_t *)(p - 24);
  return (m1 == NY_MAGIC1 && m2 == NY_MAGIC2);
}

#define is_heap_ptr(v) is_valid_heap_ptr(v)
#define is_any_ptr(v) (((v) != 0 && !((v) & 1) && (uintptr_t)(v) > 0x1000))

static inline int64_t rt_tag_v(int64_t v) { return (int64_t)(((uint64_t)v << 1) | 1); }
static inline int64_t rt_untag_v(int64_t v) {
  if (v & 1) return (v >> 1);
  if ((v & 7) == 6) return (v >> 3);
  return v;
}

#if UINTPTR_MAX == 0xffffffff
static inline int64_t __mask_ptr(int64_t v) { return (int64_t)(v & ~2ULL); }
#define NY_NATIVE_TAG 2
#define NY_NATIVE_MARK (1ULL << 63)
#define NY_NATIVE_IS(v) (((((uint64_t)(v) & NY_NATIVE_MARK) != 0ULL) && (((v) & 3) == NY_NATIVE_TAG)))
#define NY_NATIVE_ENCODE(p) ((int64_t)(NY_NATIVE_MARK | (((uint64_t)(uintptr_t)(p) << 2) | (uint64_t)NY_NATIVE_TAG)))
#define NY_NATIVE_DECODE(v) ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_NATIVE_MARK) >> 2))
#else
static inline int64_t __mask_ptr(int64_t v) { return (int64_t)(v & ~7ULL); }
#define NY_NATIVE_TAG 6
#define NY_NATIVE_IS(v) (((v) & 7) == NY_NATIVE_TAG)
#define NY_NATIVE_ENCODE(p) ((int64_t)(((uint64_t)(uintptr_t)(p) << 3) | (uint64_t)NY_NATIVE_TAG))
#define NY_NATIVE_DECODE(v) ((void *)(uintptr_t)(((uint64_t)(v)) >> 3))
#endif

#define TAG_LIST 100
#define TAG_DICT 101
#define TAG_TUPLE 103
#define TAG_OK 104
#define TAG_ERR 105
#define TAG_FLOAT 110
#define TAG_STR 120
#define TAG_STR_CONST 121

static inline int is_v_flt(int64_t v) {
  if (!is_ptr(v) || (v & 7) != 0) return 0;
  if (!rt_addr_readable((uintptr_t)v - 8, 8)) return 0;
  return (*(int64_t *)((char *)(uintptr_t)v - 8) == TAG_FLOAT);
}

static inline int is_ny_obj(int64_t v) {
  if (!is_ptr(v) || ((v) & 7) != 0) return 0;
  if (is_heap_ptr(v)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    return (tag >= 100 && tag <= 255);
  }
  if (rt_addr_readable((uintptr_t)v - 8, 8)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    return (tag == TAG_STR || tag == TAG_STR_CONST);
  }
  return 0;
}

static inline int is_v_str(int64_t v) {
  if (!is_ptr(v) || ((v) & 7) != 0) return 0;
  if (is_heap_ptr(v)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    return (tag == TAG_STR || tag == TAG_STR_CONST);
  }
  if (rt_addr_readable((uintptr_t)v - 8, 8)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    return (tag == TAG_STR || tag == TAG_STR_CONST);
  }
  return 0;
}

static inline int is_v_ok(int64_t v) {
  if (!is_ptr(v) || ((v) & 7) != 0) return 0;
  if (!is_heap_ptr(v)) return 0;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  return tag == TAG_OK;
}

static inline int is_v_err(int64_t v) {
  if (!is_ptr(v) || ((v) & 7) != 0) return 0;
  if (!is_heap_ptr(v)) return 0;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  return tag == TAG_ERR;
}

static inline int64_t __rt_flt_unbox_val(int64_t v) {
  if (v & 1) {
    double d = (double)(v >> 1);
    int64_t res;
    memcpy(&res, &d, 8);
    return res;
  }
  if (is_v_flt(v)) {
    int64_t bits;
    memcpy(&bits, (const void *)(uintptr_t)v, 8);
    return bits;
  }
  return 0;
}

static inline int64_t __rt_load_item_fast(int64_t lst, int64_t i_v) {
  if (!is_ptr(lst)) return 0;
  int64_t i = is_int(i_v) ? (i_v >> 1) : i_v;
  return *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8);
}

static inline int64_t __rt_store_item_fast(int64_t lst, int64_t i_v, int64_t val) {
  if (!is_ptr(lst)) return 0;
  int64_t i = is_int(i_v) ? (i_v >> 1) : i_v;
  *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8) = val;
  return val;
}

void __cleanup_args(void);
int64_t __set_args(int64_t argc, int64_t argv, int64_t envp);
int64_t __malloc(int64_t n);
int64_t __free(int64_t ptr);
int64_t __runtime_cleanup(void);
int64_t __flt_box_val(int64_t bits);
int64_t __str_concat(int64_t a, int64_t b);
int64_t __trace_last_file(void);
int64_t __trace_last_line(void);
int64_t __trace_last_col(void);
int64_t __trace_last_func(void);
void print_trace_entry(int64_t file, int64_t line, int64_t col, int64_t func, const char *prefix);

static inline size_t __get_heap_size(int64_t v) {
  if (!is_heap_ptr(v)) return (size_t)-1;
  int64_t raw = *(int64_t *)((char *)(uintptr_t)v - 16);
  if (raw & 1) raw >>= 1;
  return (size_t)raw;
}

static inline int __check_oob(const char *op, int64_t addr, int64_t idx, size_t access_sz) {
  (void)op;
  if ((intptr_t)idx < 0) return 0;
  size_t hsz = __get_heap_size(addr);
  if (hsz == (size_t)-1) return 1;
  if ((size_t)idx + access_sz > hsz) return 0;
  return 1;
}

int64_t __rt_alloc_string(const char *s);
int64_t __rt_alloc_string_len(const char *s, size_t len);
int64_t __list_len(int64_t lst);
int64_t __list_set_len(int64_t lst, int64_t n);
int64_t __load_item(int64_t lst, int64_t i);
int64_t __load_item_fast(int64_t lst, int64_t i);
int64_t __flt_unbox_val(int64_t v);
void __flt_free(int64_t v);

#endif
