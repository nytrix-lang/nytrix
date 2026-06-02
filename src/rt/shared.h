#ifndef RT_COMMON_H
#define RT_COMMON_H

#include "base/compat.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NY_WITH_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define NY_WITH_ASAN 1
#endif
#ifndef NY_WITH_ASAN
#define NY_WITH_ASAN 0
#endif
#if NY_WITH_ASAN
#include <sanitizer/asan_interface.h>
#endif
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
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

#define NY_VALUE_INT_TAG_BIT UINT64_C(1)
#define NY_VALUE_INT_SHIFT 1
#define NY_VALUE_PTR_TAG_MASK UINT64_C(7)
#define NY_VALUE_PTR_MIN_ADDR ((uintptr_t)0x1000)

#if UINTPTR_MAX == 0xffffffff
#define NY_NATIVE_TAG_MASK UINT64_C(3)
#define NY_NATIVE_TAG UINT64_C(2)
#define NY_NATIVE_SHIFT 2
#define NY_NATIVE_MARK (UINT64_C(1) << 63)
#define NY_NATIVE_IS(v)                                                                            \
  (((((uint64_t)(v) & NY_NATIVE_MARK) != 0ULL) &&                                                  \
    (((uint64_t)(v) & NY_NATIVE_TAG_MASK) == NY_NATIVE_TAG)))
#else
#define NY_NATIVE_TAG_MASK NY_VALUE_PTR_TAG_MASK
#define NY_NATIVE_TAG UINT64_C(6)
#define NY_NATIVE_SHIFT 3
#define NY_NATIVE_IS(v) ((((uint64_t)(v)) & NY_NATIVE_TAG_MASK) == NY_NATIVE_TAG)
#endif

#define is_int(v) ((((uint64_t)(v)) & NY_VALUE_INT_TAG_BIT) != 0)
#define is_ptr(v)                                                                                    \
  (((((uint64_t)(v)) & NY_VALUE_INT_TAG_BIT) == 0) && (uintptr_t)(v) > NY_VALUE_PTR_MIN_ADDR)

static inline bool rt_env_is_truthy(const char *v) {
  if (!v || !*v)
    return false;
  return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "False") != 0 &&
         strcmp(v, "FALSE") != 0 && strcmp(v, "off") != 0 && strcmp(v, "OFF") != 0 &&
         strcmp(v, "no") != 0 && strcmp(v, "NO") != 0;
}

static inline bool rt_env_enabled(const char *name) {
  if (!name || !*name)
    return false;
  return rt_env_is_truthy(getenv(name));
}

static inline bool rt_env_enabled_default_on(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return true;
  return rt_env_is_truthy(v);
}

static inline int rt_addr_mapped(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0)
    return 0;
  if (p > UINTPTR_MAX - n)
    return 0;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {0};
  if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)))
    return 0;
  if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
    return 0;
  return 1;
#elif defined(__APPLE__)
  mach_vm_address_t region = (mach_vm_address_t)p;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  kern_return_t kr =
      mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                     (vm_region_info_t)&info, &count, &object_name);
  if (object_name != MACH_PORT_NULL)
    mach_port_deallocate(mach_task_self(), object_name);
  if (kr != KERN_SUCCESS || region_size == 0)
    return 0;
  mach_vm_address_t begin = (mach_vm_address_t)p;
  mach_vm_address_t end = begin + (mach_vm_size_t)n;
  mach_vm_address_t region_end = region + region_size;
  if (end < begin || region_end < region || begin < region || end > region_end)
    return 0;
  return (info.protection & VM_PROT_READ) != 0;
#else
  static long ps = 0;
  if (ps == 0)
    ps = ny_page_size();
  if (ps <= 0)
    ps = 4096;
  uintptr_t mask = (uintptr_t)ps - 1;
  uintptr_t start = p & ~mask;
  uintptr_t end = (p + n - 1) & ~mask;
  unsigned char vec = 0;
  for (uintptr_t cur = start; cur <= end; cur += (uintptr_t)ps) {
    if (mincore((void *)cur, (size_t)ps, (void *)&vec) != 0)
      return 0;
  }
  return 1;
#endif
}

#define RT_PAGE_CACHE_BITS 11
#define RT_PAGE_CACHE_SIZE (1 << RT_PAGE_CACHE_BITS)
#define RT_PAGE_CACHE_MASK (RT_PAGE_CACHE_SIZE - 1)

#define RT_HEAP_PTR_CACHE_BITS 13
#define RT_HEAP_PTR_CACHE_SIZE (1 << RT_HEAP_PTR_CACHE_BITS)
#define RT_HEAP_PTR_CACHE_MASK (RT_HEAP_PTR_CACHE_SIZE - 1)

#define RT_HEAP_PTR_NEG_CACHE_BITS 12
#define RT_HEAP_PTR_NEG_CACHE_SIZE (1 << RT_HEAP_PTR_NEG_CACHE_BITS)
#define RT_HEAP_PTR_NEG_CACHE_MASK (RT_HEAP_PTR_NEG_CACHE_SIZE - 1)

#define RT_CONST_STR_CACHE_BITS 12
#define RT_CONST_STR_CACHE_SIZE (1 << RT_CONST_STR_CACHE_BITS)
#define RT_CONST_STR_CACHE_MASK (RT_CONST_STR_CACHE_SIZE - 1)

#define RT_NON_STR_CACHE_BITS 12
#define RT_NON_STR_CACHE_SIZE (1 << RT_NON_STR_CACHE_BITS)
#define RT_NON_STR_CACHE_MASK (RT_NON_STR_CACHE_SIZE - 1)

#define RT_FLOAT_CACHE_BITS 12
#define RT_FLOAT_CACHE_SIZE (1 << RT_FLOAT_CACHE_BITS)
#define RT_FLOAT_CACHE_MASK (RT_FLOAT_CACHE_SIZE - 1)

#define RT_READABLE_HDR_PAGE_CACHE_BITS 11
#define RT_READABLE_HDR_PAGE_CACHE_SIZE (1 << RT_READABLE_HDR_PAGE_CACHE_BITS)
#define RT_READABLE_HDR_PAGE_CACHE_MASK (RT_READABLE_HDR_PAGE_CACHE_SIZE - 1)

#ifndef NY_STRICT_HEAP_EPOCH
#define NY_STRICT_HEAP_EPOCH 0
#endif

extern __thread uintptr_t rt_heap_ptr_cache_keys[RT_HEAP_PTR_CACHE_SIZE];
extern __thread uint64_t rt_heap_ptr_cache_epoch;
extern _Atomic uint64_t rt_heap_ptr_global_epoch;
static __thread uintptr_t rt_heap_ptr_neg_cache_keys[RT_HEAP_PTR_NEG_CACHE_SIZE];
static __thread uintptr_t rt_const_str_cache_keys[RT_CONST_STR_CACHE_SIZE];
static __thread uintptr_t rt_non_str_cache_keys[RT_NON_STR_CACHE_SIZE];
static __thread uintptr_t rt_float_cache_keys[RT_FLOAT_CACHE_SIZE];
static __thread uintptr_t rt_non_float_cache_keys[RT_FLOAT_CACHE_SIZE];
static __thread uintptr_t rt_readable_hdr_page_cache[RT_READABLE_HDR_PAGE_CACHE_SIZE];

static inline uintptr_t rt_heap_ptr_cache_slot(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12) ^ (p >> 21)) & RT_HEAP_PTR_CACHE_MASK;
}

static inline uintptr_t rt_heap_ptr_neg_cache_slot(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12) ^ (p >> 21)) & RT_HEAP_PTR_NEG_CACHE_MASK;
}

static inline int rt_heap_ptr_cache_hit(uintptr_t p) {
  uintptr_t slot = rt_heap_ptr_cache_slot(p);
  if (rt_heap_ptr_cache_keys[slot] != p)
    return 0;
#if NY_STRICT_HEAP_EPOCH
  uint64_t epoch = atomic_load_explicit(&rt_heap_ptr_global_epoch, memory_order_relaxed);
  if (rt_heap_ptr_cache_epoch == epoch)
    return 1;
  rt_heap_ptr_cache_keys[slot] = 0;
  rt_heap_ptr_cache_epoch = epoch;
  return 0;
#else
  return 1;
#endif
}

static inline void rt_heap_ptr_cache_store(uintptr_t p) {
#if NY_STRICT_HEAP_EPOCH
  rt_heap_ptr_cache_epoch = atomic_load_explicit(&rt_heap_ptr_global_epoch, memory_order_relaxed);
#endif
  rt_heap_ptr_cache_keys[rt_heap_ptr_cache_slot(p)] = p;
  rt_heap_ptr_neg_cache_keys[rt_heap_ptr_neg_cache_slot(p)] = 0;
}

static inline void rt_heap_ptr_cache_forget(uintptr_t p) {
  rt_heap_ptr_cache_keys[rt_heap_ptr_cache_slot(p)] = 0;
  rt_heap_ptr_neg_cache_keys[rt_heap_ptr_neg_cache_slot(p)] = 0;
#if NY_STRICT_HEAP_EPOCH
  rt_heap_ptr_cache_epoch =
      atomic_fetch_add_explicit(&rt_heap_ptr_global_epoch, 1, memory_order_relaxed) + 1;
#endif
}

static inline int rt_heap_ptr_neg_cache_hit(uintptr_t p) {
  return rt_heap_ptr_neg_cache_keys[rt_heap_ptr_neg_cache_slot(p)] == p;
}

static inline void rt_heap_ptr_neg_cache_store(uintptr_t p) {
  rt_heap_ptr_neg_cache_keys[rt_heap_ptr_neg_cache_slot(p)] = p;
}

static inline uintptr_t rt_const_str_cache_slot(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12) ^ (p >> 21)) & RT_CONST_STR_CACHE_MASK;
}

static inline int rt_const_str_cache_hit(uintptr_t p) {
  return rt_const_str_cache_keys[rt_const_str_cache_slot(p)] == p;
}

static inline void rt_const_str_cache_store(uintptr_t p) {
  rt_const_str_cache_keys[rt_const_str_cache_slot(p)] = p;
  rt_non_str_cache_keys[((p >> 4) ^ (p >> 12) ^ (p >> 21)) & RT_NON_STR_CACHE_MASK] = 0;
}

static inline uintptr_t rt_non_str_cache_slot(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12) ^ (p >> 21)) & RT_NON_STR_CACHE_MASK;
}

static inline int rt_non_str_cache_hit(uintptr_t p) {
  return rt_non_str_cache_keys[rt_non_str_cache_slot(p)] == p;
}

static inline void rt_non_str_cache_store(uintptr_t p) {
  rt_non_str_cache_keys[rt_non_str_cache_slot(p)] = p;
}

static inline uintptr_t rt_float_cache_slot(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12) ^ (p >> 21)) & RT_FLOAT_CACHE_MASK;
}

static inline int rt_float_cache_hit(uintptr_t p) {
  return rt_float_cache_keys[rt_float_cache_slot(p)] == p;
}

static inline int rt_non_float_cache_hit(uintptr_t p) {
  return rt_non_float_cache_keys[rt_float_cache_slot(p)] == p;
}

static inline void rt_float_cache_store(uintptr_t p) {
  uintptr_t slot = rt_float_cache_slot(p);
  rt_float_cache_keys[slot] = p;
  rt_non_float_cache_keys[slot] = 0;
}

static inline void rt_non_float_cache_store(uintptr_t p) {
  uintptr_t slot = rt_float_cache_slot(p);
  rt_non_float_cache_keys[slot] = p;
  rt_float_cache_keys[slot] = 0;
}

static inline void rt_float_cache_forget(uintptr_t p) {
  uintptr_t slot = rt_float_cache_slot(p);
  if (rt_float_cache_keys[slot] == p)
    rt_float_cache_keys[slot] = 0;
  if (rt_non_float_cache_keys[slot] == p)
    rt_non_float_cache_keys[slot] = 0;
}

static inline uintptr_t rt_page_base_4k(uintptr_t p) { return p & ~(uintptr_t)4095ULL; }

static inline uintptr_t rt_page_cache_slot(uintptr_t pg, uintptr_t mask) {
  return ((pg >> 12) ^ (pg >> 21) ^ (pg >> 30)) & mask;
}

static inline int rt_addr_readable(uintptr_t p, size_t n) {
  static __thread uintptr_t last_pg = 0;
  static __thread uintptr_t cache[RT_PAGE_CACHE_SIZE];
  if (p < 0x1000 || n == 0)
    return 0;
  if (p > UINTPTR_MAX - n)
    return 0;

#ifdef __APPLE__
  return rt_addr_mapped(p, n);
#endif

  uintptr_t pg1 = rt_page_base_4k(p);
  uintptr_t pg2 = rt_page_base_4k(p + n - 1);
  if (pg1 == last_pg) {
    if (pg1 == pg2)
      return 1;
  }

  uintptr_t h1 = rt_page_cache_slot(pg1, RT_PAGE_CACHE_MASK);
  if (cache[h1] == pg1) {
    if (pg1 == pg2) {
      last_pg = pg1;
      return 1;
    }
    uintptr_t h2 = rt_page_cache_slot(pg2, RT_PAGE_CACHE_MASK);
    if (cache[h2] == pg2) {
      last_pg = pg1;
      return 1;
    }
  }

#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {0};
  if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)))
    return 0;
  if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
    return 0;
#else
  if (!rt_addr_mapped(p, n))
    return 0;
#endif

  cache[h1] = pg1;
  last_pg = pg1;
  if (pg2 != pg1) {
    uintptr_t h2 = rt_page_cache_slot(pg2, RT_PAGE_CACHE_MASK);
    cache[h2] = pg2;
  }
  return 1;
}

static inline int rt_addr_readable_safe(uintptr_t p, size_t n) {
  if (!rt_addr_readable(p, n))
    return 0;
#if NY_WITH_ASAN
  if (__asan_region_is_poisoned((void *)p, n))
    return 0;
#endif
  return 1;
}

static inline int rt_try_read_i64(uintptr_t p, int64_t *out) {
  if (!out)
    return 0;
  if (!rt_addr_readable_safe(p, sizeof(int64_t)))
    return 0;
  memcpy(out, (const void *)p, sizeof(int64_t));
  return 1;
}

static inline int rt_readable_hdr_cache_hit(uintptr_t p, size_t n) {
  static __thread uintptr_t last_pg1 = 0;
  static __thread uintptr_t last_pg2 = 0;
  if (p < 0x1000 || n == 0 || p > UINTPTR_MAX - n)
    return 0;
  uintptr_t pg1 = rt_page_base_4k(p);
  uintptr_t pg2 = rt_page_base_4k(p + n - 1);
  if (pg1 == last_pg1 && pg2 == last_pg2)
    return 1;
  uintptr_t h1 = rt_page_cache_slot(pg1, RT_READABLE_HDR_PAGE_CACHE_MASK);
  if (rt_readable_hdr_page_cache[h1] != pg1)
    return 0;
  if (pg2 != pg1) {
    uintptr_t h2 = rt_page_cache_slot(pg2, RT_READABLE_HDR_PAGE_CACHE_MASK);
    if (rt_readable_hdr_page_cache[h2] != pg2)
      return 0;
  }
  last_pg1 = pg1;
  last_pg2 = pg2;
  return 1;
}

static inline void rt_readable_hdr_cache_store(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0 || p > UINTPTR_MAX - n)
    return;
  uintptr_t pg1 = rt_page_base_4k(p);
  uintptr_t pg2 = rt_page_base_4k(p + n - 1);
  rt_readable_hdr_page_cache[rt_page_cache_slot(pg1, RT_READABLE_HDR_PAGE_CACHE_MASK)] = pg1;
  if (pg2 != pg1)
    rt_readable_hdr_page_cache[rt_page_cache_slot(pg2, RT_READABLE_HDR_PAGE_CACHE_MASK)] = pg2;
}

static inline int rt_header_readable_cached(uintptr_t p, size_t n) {
  if (rt_readable_hdr_cache_hit(p, n))
    return 1;
  if (!rt_addr_readable_safe(p, n))
    return 0;
  rt_readable_hdr_cache_store(p, n);
  return 1;
}

static inline bool is_valid_heap_ptr(int64_t v) {
  if (!(v > 0x1000 && (v & 15) == 0))
    return false;
  uintptr_t p = (uintptr_t)v;
  if (rt_heap_ptr_cache_hit(p))
    return true;
  if (rt_heap_ptr_neg_cache_hit(p))
    return false;
  uintptr_t hdr = p - 32;
  uintptr_t pg1 = rt_page_base_4k(hdr);
  uintptr_t pg2 = rt_page_base_4k(hdr + 31);
  static __thread uintptr_t last_hdr_pg1 = 0;
  static __thread uintptr_t last_hdr_pg2 = 0;
  if (pg1 != last_hdr_pg1 || pg2 != last_hdr_pg2) {
    if (!rt_header_readable_cached(hdr, 32)) {
      rt_heap_ptr_neg_cache_store(p);
      return false;
    }
    last_hdr_pg1 = pg1;
    last_hdr_pg2 = pg2;
  }
  int64_t m1 = 0;
  if (!rt_try_read_i64(hdr, &m1)) {
    rt_heap_ptr_neg_cache_store(p);
    return false;
  }
  if (m1 != NY_MAGIC1) {
    rt_heap_ptr_neg_cache_store(p);
    return false;
  }
  rt_heap_ptr_cache_store(p);
  return true;
}

#define is_heap_ptr(v) is_valid_heap_ptr(v)
#define is_any_ptr(v)                                                                               \
  (((v) != 0 && ((((uint64_t)(v)) & NY_VALUE_INT_TAG_BIT) == 0) &&                                  \
    (uintptr_t)(v) > NY_VALUE_PTR_MIN_ADDR))

#define NY_IMM_NIL ((int64_t)0)
#define NY_IMM_FALSE ((int64_t)2)
#define NY_IMM_TRUE ((int64_t)8)

static inline int64_t rt_tag_v(int64_t v) {
  return (int64_t)(((uint64_t)v << NY_VALUE_INT_SHIFT) | NY_VALUE_INT_TAG_BIT);
}
static inline int64_t rt_untag_v(int64_t v) {
  if (is_int(v))
    return (v >> NY_VALUE_INT_SHIFT);
  if (NY_NATIVE_IS(v))
    return (int64_t)(((uint64_t)v) >> NY_NATIVE_SHIFT);
  return v;
}

static inline int rt_is_nil_imm(int64_t v) { return v == NY_IMM_NIL; }
static inline int rt_is_true_imm(int64_t v) { return v == NY_IMM_TRUE; }
static inline int rt_is_false_imm(int64_t v) { return v == NY_IMM_FALSE; }
static inline int rt_is_bool_imm(int64_t v) { return v == NY_IMM_TRUE || v == NY_IMM_FALSE; }
static inline int rt_is_falsy(int64_t v) {
  return v == NY_IMM_NIL || v == NY_IMM_FALSE || v == rt_tag_v(0);
}
static inline int rt_is_truthy(int64_t v) { return !rt_is_falsy(v); }

#if UINTPTR_MAX == 0xffffffff
static inline int64_t rt_mask_ptr(int64_t v) { return (int64_t)(v & ~2ULL); }
#define NY_NATIVE_ENCODE(p)                                                                        \
  ((int64_t)(NY_NATIVE_MARK |                                                                      \
             (((uint64_t)(uintptr_t)(p) << NY_NATIVE_SHIFT) | (uint64_t)NY_NATIVE_TAG)))
#define NY_NATIVE_DECODE(v)                                                                        \
  ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_NATIVE_MARK) >> NY_NATIVE_SHIFT))
#else
static inline int64_t rt_mask_ptr(int64_t v) { return (int64_t)((uint64_t)v & ~NY_VALUE_PTR_TAG_MASK); }
#define NY_NATIVE_ENCODE(p)                                                                        \
  ((int64_t)(((uint64_t)(uintptr_t)(p) << NY_NATIVE_SHIFT) | (uint64_t)NY_NATIVE_TAG))
#define NY_NATIVE_DECODE(v) ((void *)(uintptr_t)(((uint64_t)(v)) >> NY_NATIVE_SHIFT))
#endif

#define TAG_LIST 100
#define TAG_DICT 101
#define TAG_SET 102
#define TAG_TUPLE 103
#define TAG_OK 104
#define TAG_ERR 105
#define TAG_RANGE 106
#define TAG_CLOSURE 107
#define TAG_FLOAT 110
#define TAG_COMPLEX 111
#define TAG_STR 120
#define TAG_STR_CONST 121
#define TAG_BYTES 122
#define TAG_BIGINT 130
#define TAG_KWARG 150

static inline int64_t rt_runtime_tag_raw_name(const char *s, size_t n) {
  if (!s)
    return 0;
  if (n == 3 && memcmp(s, "nil", 3) == 0)
    return 0;
  if (n == 3 && memcmp(s, "int", 3) == 0)
    return 1;
  if (n == 7 && memcmp(s, "ffi_ptr", 7) == 0)
    return 6;
  if (n == 4 && memcmp(s, "list", 4) == 0)
    return TAG_LIST;
  if (n == 4 && memcmp(s, "dict", 4) == 0)
    return TAG_DICT;
  if (n == 3 && memcmp(s, "set", 3) == 0)
    return TAG_SET;
  if (n == 5 && memcmp(s, "tuple", 5) == 0)
    return TAG_TUPLE;
  if (n == 2 && memcmp(s, "ok", 2) == 0)
    return TAG_OK;
  if (n == 3 && memcmp(s, "err", 3) == 0)
    return TAG_ERR;
  if (n == 5 && memcmp(s, "range", 5) == 0)
    return TAG_RANGE;
  if (n == 7 && memcmp(s, "closure", 7) == 0)
    return TAG_CLOSURE;
  if (n == 3 && memcmp(s, "ptr", 3) == 0)
    return TAG_CLOSURE;
  if (n == 5 && memcmp(s, "float", 5) == 0)
    return TAG_FLOAT;
  if (n == 7 && memcmp(s, "complex", 7) == 0)
    return TAG_COMPLEX;
  if (n == 3 && memcmp(s, "str", 3) == 0)
    return TAG_STR;
  if (n == 9 && memcmp(s, "str_const", 9) == 0)
    return TAG_STR_CONST;
  if (n == 5 && memcmp(s, "bytes", 5) == 0)
    return TAG_BYTES;
  if (n == 6 && memcmp(s, "bigint", 6) == 0)
    return TAG_BIGINT;
  if (n == 5 && memcmp(s, "kwarg", 5) == 0)
    return TAG_KWARG;
  return 0;
}

static inline int64_t rt_heap_object_ptr(int64_t v) {
  if (v == 0 || NY_NATIVE_IS(v))
    return 0;
  if (is_ptr(v) && (((uint64_t)v) & NY_VALUE_PTR_TAG_MASK) == 0)
    return is_heap_ptr(v) ? v : 0;
  uintptr_t p = (uintptr_t)rt_mask_ptr(v);
  if (p <= NY_VALUE_PTR_MIN_ADDR)
    return 0;
  int64_t base = (int64_t)p;
  return is_heap_ptr(base) ? base : 0;
}

#ifndef NY_SMALL_INT_MIN
#define NY_SMALL_INT_MIN INT64_C(-4611686018427387904)
#endif
#ifndef NY_SMALL_INT_MAX
#define NY_SMALL_INT_MAX INT64_C(4611686018427387903)
#endif
static inline bool ny_small_int_fits_i64(int64_t raw) {
  return raw >= NY_SMALL_INT_MIN && raw <= NY_SMALL_INT_MAX;
}

static inline int is_v_flt(int64_t v) {
  if (!is_ptr(v) || (v & 15) != 8)
    return 0;
  uintptr_t p = (uintptr_t)v;
  if (rt_float_cache_hit(p))
    return 1;
  if (rt_non_float_cache_hit(p))
    return 0;
  int64_t tag = 0;
  if (!rt_try_read_i64(p - 8, &tag)) {
    rt_non_float_cache_store(p);
    return 0;
  }
  int ok = (tag == TAG_FLOAT);
  if (ok)
    rt_float_cache_store(p);
  else
    rt_non_float_cache_store(p);
  return ok;
}

static inline int is_v_flt_mapped(int64_t v) {
  return is_v_flt(v);
}

static inline int is_ny_obj(int64_t v) {
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    uintptr_t p = (uintptr_t)heap_v;
    int64_t tag = 0;
    if (!rt_try_read_i64(p - 8, &tag))
      return 0;
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    return (tag >= 100 && tag <= 255);
  }
  if (!is_ptr(v) || ((v) & 7) != 0)
    return 0;
  uintptr_t p = (uintptr_t)v;
  if (rt_const_str_cache_hit(p))
    return 1;
  if (rt_non_str_cache_hit(p))
    return 0;
  int64_t tag = 0;
  if (rt_try_read_i64(p - 8, &tag)) {
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    else if (tag != TAG_STR)
      rt_non_str_cache_store(p);
    return (tag == TAG_STR || tag == TAG_STR_CONST);
  }
  return 0;
}

static inline int is_v_str(int64_t v) {
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    uintptr_t p = (uintptr_t)heap_v;
    int64_t tag = 0;
    if (!rt_try_read_i64(p - 8, &tag))
      return 0;
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    return (tag == TAG_STR || tag == TAG_STR_CONST);
  }
  if (!is_ptr(v) || ((v) & 7) != 0)
    return 0;
  uintptr_t p = (uintptr_t)v;
  if (rt_const_str_cache_hit(p))
    return 1;
  if (rt_non_str_cache_hit(p))
    return 0;
  int64_t tag = 0;
  if (rt_try_read_i64(p - 8, &tag)) {
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    else if (tag != TAG_STR)
      rt_non_str_cache_store(p);
    return (tag == TAG_STR || tag == TAG_STR_CONST);
  }
  return 0;
}

/* Return string length for a tagged Ny string-like object; 0 when unavailable. */
static inline size_t rt_tagged_str_len(int64_t v) {
  if (!is_v_str(v))
    return 0;
  uintptr_t lp = (uintptr_t)v - 16;
  if (!rt_addr_readable(lp, sizeof(int64_t)))
    return 0;
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)lp, sizeof(tagged_len));
  if (!is_int(tagged_len))
    return 0;
  return (size_t)(tagged_len >> 1);
}

static inline int is_v_ok(int64_t v) {
  int64_t heap_v = rt_heap_object_ptr(v);
  if (!heap_v)
    return 0;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
  return tag == TAG_OK;
}

static inline int is_v_err(int64_t v) {
  int64_t heap_v = rt_heap_object_ptr(v);
  if (!heap_v)
    return 0;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
  return tag == TAG_ERR;
}

static inline int64_t _rt_flt_unbox_val(int64_t v) {
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

static inline int64_t _rt_load_item_fast(int64_t lst, int64_t i_v) {
  if (!is_ptr(lst))
    return 0;
  int64_t i = is_int(i_v) ? (i_v >> 1) : i_v;
  return *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8);
}

static inline int64_t _rt_store_item_fast(int64_t lst, int64_t i_v, int64_t val) {
  if (!is_ptr(lst))
    return 0;
  int64_t i = is_int(i_v) ? (i_v >> 1) : i_v;
  *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8) = val;
  return val;
}

void rt_cleanup_args(void);
void rt_cleanup_small_strings(void);
int64_t rt_set_args(int64_t argc, int64_t argv, int64_t envp);
int _ny_aot_set_args(int argc, char **argv, char **envp);
int64_t rt_malloc(int64_t n);
int64_t rt_malloc_uninit(int64_t n);
int64_t rt_free(int64_t ptr);
int64_t rt_ptr_key(int64_t ptr);
int64_t rt_atomic_load64(int64_t addr, int64_t idx);
int64_t rt_atomic_store64(int64_t addr, int64_t idx, int64_t value);
int64_t rt_atomic_add64(int64_t addr, int64_t idx, int64_t delta);
int64_t rt_atomic_sub64(int64_t addr, int64_t idx, int64_t delta);
int64_t rt_atomic_exchange64(int64_t addr, int64_t idx, int64_t value);
int64_t rt_atomic_cas64(int64_t addr, int64_t idx, int64_t expected, int64_t desired);
int64_t rt_drop_owned(int64_t ptr);
int64_t rt_drop_owned_slot(int64_t slot_ptr);
int64_t rt_runtime_cleanup(void);
int64_t rt_fix_fn_ptr(int64_t fn);
int64_t rt_flt_box_val(int64_t bits);
int64_t rt_str_concat(int64_t a, int64_t b);
int64_t rt_str_builder_new(int64_t cap_v);
int64_t rt_str_builder_append(int64_t builder_v, int64_t value);
int64_t rt_str_builder_to_str(int64_t builder_v);
int64_t rt_str_builder_free(int64_t builder_v);
int64_t rt_dict_reserve(int64_t d, int64_t additional);
int64_t rt_dict_write_fast(int64_t d, int64_t key, int64_t value);
int64_t rt_eq(int64_t a, int64_t b);
int64_t rt_trace_last_file(void);
int64_t rt_trace_last_line(void);
int64_t rt_trace_last_col(void);
int64_t rt_trace_last_func(void);
void rt_trace_refresh_env(void);
void print_trace_entry(int64_t file, int64_t line, int64_t col, int64_t func, const char *prefix);

static inline size_t rt_get_heap_size_known(int64_t v) {
  int64_t raw = *(int64_t *)((char *)(uintptr_t)v - 24);
  if (raw & 1)
    raw >>= 1;
  return (size_t)raw;
}

static inline size_t rt_get_heap_size(int64_t v) {
  if (!is_heap_ptr(v))
    return (size_t)-1;
  return rt_get_heap_size_known(v);
}

static inline bool rt_heap_size_fast(int64_t v, size_t *out_size) {
  if (!out_size)
    return false;
  if (!is_heap_ptr(v))
    return false;
  *out_size = rt_get_heap_size_known(v);
  return true;
}

static inline int rt_check_oob(const char *op, int64_t addr, int64_t idx, size_t access_sz) {
  (void)op;
  if ((intptr_t)idx < 0)
    return 0;
  size_t hsz = rt_get_heap_size(addr);
  if (hsz == (size_t)-1)
    return 1;
  if ((size_t)idx + access_sz > hsz)
    return 0;
  return 1;
}

int64_t rt_alloc_string(const char *s);
int64_t rt_alloc_string_len(const char *s, size_t len);
int64_t rt_panic(int64_t msg_ptr);
int64_t rt_division_by_zero(void);
int64_t rt_modulo_by_zero(void);
int64_t rt_runtime_tag(int64_t name);
int64_t rt_init_str(int64_t p, int64_t n);
int64_t rt_bytes_new(int64_t n);
int64_t rt_kwarg_new(int64_t key, int64_t value);
int64_t rt_range_new(int64_t start, int64_t stop, int64_t step);
int64_t rt_list_as_tuple(int64_t lst);
int64_t rt_list_reserve(int64_t lst, int64_t cap);
int64_t rt_list_sum_int_range(int64_t lst, int64_t start, int64_t stop);
int64_t rt_list_len(int64_t lst);
int64_t rt_list_set_len(int64_t lst, int64_t n);
int64_t rt_load_item(int64_t lst, int64_t i);
int64_t rt_load_item_fast(int64_t lst, int64_t i);
int64_t rt_flt_unbox_val(int64_t v);
void rt_flt_free(int64_t v);
int64_t rt_index_read_probe_enabled(void);
int64_t rt_index_read_probe(int64_t tag, int64_t idx, int64_t path);

/* Phase 4: GC and FFI Gates */
#include "rt/ffigates.h"
#include "rt/gc.h"

static inline size_t rt_gc_size_arg(int64_t size) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  return n > 0 ? (size_t)n : 0u;
}

static inline int64_t rt_gc_alloc(int64_t size) { return nyGcAlloc(rt_gc_size_arg(size)); }

static inline int64_t rt_gc_alloc_fast(int64_t size) { return nyGcAllocFast(rt_gc_size_arg(size)); }

static inline int64_t rt_gc_alloc_slow(int64_t size) { return nyGcAllocSlow(rt_gc_size_arg(size)); }

static inline void rt_gc_collect(void) { nyGcCollect(); }

static inline void rt_gc_trigger_minor(void) { nyGcTriggerMinor(); }

static inline void rt_gc_trigger_major(void) { nyGcTriggerMajor(); }

static inline void rt_gc_write_barrier(int64_t *slot, int64_t value) {
  nyGcWriteBarrier(slot, value);
}

static inline int64_t rt_ffi_call(void *fn, int64_t *args, size_t argc) {
  return nyFfiCallGeneric(fn, args, argc);
}

static inline int64_t rt_ffi_call_fast_i_i(int64_t fn, int64_t a0) { return nyFfiFastII(fn, a0); }

static inline int64_t rt_ffi_call_fast_i_ii(int64_t fn, int64_t a0, int64_t a1) {
  return nyFfiFastIIi(fn, a0, a1);
}

static inline int64_t rt_ffi_call_fast_i_iii(int64_t fn, int64_t a0, int64_t a1, int64_t a2) {
  return nyFfiFastIIii(fn, a0, a1, a2);
}

static inline int64_t rt_ffi_call_fast_i_pi(int64_t fn, int64_t ptr, int64_t idx) {
  return nyFfiFastPII(fn, ptr, idx);
}

#endif
