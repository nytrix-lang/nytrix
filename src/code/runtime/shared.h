#ifndef RT_COMMON_H
#define RT_COMMON_H

#include "base/compat.h"
#include "base/util.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native sequence payloads follow magic, count, element size, capacity. */
enum { RT_NATIVE_TBUF_HEADER = 32 };
#define NY_NATIVE_TBUF_MAGIC UINT64_C(0x4e59544255464d47)
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
#if defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
#define NY_WITH_UBSAN 1
#endif
#endif
#if defined(__SANITIZE_UNDEFINED__)
#define NY_WITH_UBSAN 1
#endif
#ifndef NY_WITH_UBSAN
#define NY_WITH_UBSAN 0
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

#ifndef _WIN32
extern char **environ;
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
#define NY_NATIVE_IS(v)                                                        \
  (((((uint64_t)(v) & NY_NATIVE_MARK) != 0ULL) &&                              \
    (((uint64_t)(v) & NY_NATIVE_TAG_MASK) == NY_NATIVE_TAG)))
#else
#define NY_NATIVE_TAG_MASK NY_VALUE_PTR_TAG_MASK
#define NY_NATIVE_TAG UINT64_C(6)
#define NY_NATIVE_SHIFT 3
#define NY_NATIVE_IS(v)                                                        \
  ((((uint64_t)(v)) & NY_NATIVE_TAG_MASK) == NY_NATIVE_TAG)
#endif

/* A zero-capture Nytrix lambda has a raw code pointer but must keep the
 * tagged dynamic callback ABI when it is stored in `fnptr`/`any`.  Keep that
 * fact in the callable value instead of guessing from the callback's result
 * or from pointer alignment. */
#if UINTPTR_MAX == 0xffffffff
#define NY_DYNAMIC_CALLABLE_TAG_MASK UINT64_C(3)
#define NY_DYNAMIC_CALLABLE_TAG UINT64_C(0)
#define NY_DYNAMIC_CALLABLE_MARK (UINT64_C(1) << 63)
#define NY_DYNAMIC_CALLABLE_IS(v)                                              \
  (((((uint64_t)(v)) & NY_DYNAMIC_CALLABLE_MARK) != 0ULL) &&                   \
   (((uint64_t)(v) & NY_DYNAMIC_CALLABLE_TAG_MASK) ==                          \
    NY_DYNAMIC_CALLABLE_TAG))
#define NY_DYNAMIC_CALLABLE_ENCODE(p)                                          \
  ((int64_t)(NY_DYNAMIC_CALLABLE_MARK | ((uint64_t)(uintptr_t)(p) << 2)))
#define NY_DYNAMIC_CALLABLE_BOOL_ENCODE(p)                                     \
  (NY_DYNAMIC_CALLABLE_ENCODE(p) | NY_DYNAMIC_CALLABLE_BOOL_MARK)
#define NY_DYNAMIC_CALLABLE_DECODE(v)                                          \
  ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_DYNAMIC_CALLABLE_MARKS) >> 2))
#else
#define NY_DYNAMIC_CALLABLE_TAG_MASK NY_VALUE_PTR_TAG_MASK
#define NY_DYNAMIC_CALLABLE_TAG UINT64_C(4)
#define NY_DYNAMIC_CALLABLE_MARK (UINT64_C(1) << 63)
#define NY_DYNAMIC_CALLABLE_IS(v)                                              \
  (((((uint64_t)(v)) & NY_DYNAMIC_CALLABLE_MARK) != 0ULL) &&                   \
   (((uint64_t)(v) & NY_DYNAMIC_CALLABLE_TAG_MASK) ==                          \
    NY_DYNAMIC_CALLABLE_TAG))
#define NY_DYNAMIC_CALLABLE_ENCODE(p)                                          \
  ((int64_t)(NY_DYNAMIC_CALLABLE_MARK | ((uint64_t)(uintptr_t)(p) << 3) |      \
             NY_DYNAMIC_CALLABLE_TAG))
#define NY_DYNAMIC_CALLABLE_BOOL_ENCODE(p)                                     \
  (NY_DYNAMIC_CALLABLE_ENCODE(p) | NY_DYNAMIC_CALLABLE_BOOL_MARK)
#define NY_DYNAMIC_CALLABLE_DECODE(v)                                          \
  ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_DYNAMIC_CALLABLE_MARKS) >> 3))
#endif

/* All callable encodings remain even. An odd callable tag would overlap
 * negative tagged integers, whose high bits are set by sign extension. */
#define NY_DYNAMIC_CALLABLE_BOOL_MARK (UINT64_C(1) << 62)
/* A bool-marked callable whose body consumes its parameters through the
 * dynamic (tagged) ABI must not have its integer arguments unboxed at the
 * dispatch.  The mark keeps the raw 0/1 result handling while opting out of
 * the raw-scalar argument untagging that plain bool marking implies. */
#define NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK (UINT64_C(1) << 61)
/* The callback body receives canonical arguments but returns a raw scalar
 * i64. Result normalization must box even odd values; parity alone cannot
 * distinguish those from tagged integers. */
#define NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK (UINT64_C(1) << 60)
#define NY_DYNAMIC_CALLABLE_MARKS                                              \
  (NY_DYNAMIC_CALLABLE_MARK | NY_DYNAMIC_CALLABLE_BOOL_MARK |                 \
   NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK)
#define NY_DYNAMIC_CALLABLE_BOOL_IS(v)                                         \
  (NY_DYNAMIC_CALLABLE_IS(v) &&                                                \
   (((uint64_t)(v) & NY_DYNAMIC_CALLABLE_BOOL_MARK) != 0))
#define NY_DYNAMIC_CALLABLE_TAGGED_ARGS_IS(v)                                  \
  (NY_DYNAMIC_CALLABLE_IS(v) &&                                                \
   (((uint64_t)(v) & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK) != 0))
#define NY_DYNAMIC_CALLABLE_RAW_RESULT_IS(v)                                   \
  (NY_DYNAMIC_CALLABLE_IS(v) &&                                                \
   (((uint64_t)(v) & NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK) != 0))

#define is_int(v) ((((uint64_t)(v)) & NY_VALUE_INT_TAG_BIT) != 0)
#define is_ptr(v)                                                              \
  (!NY_DYNAMIC_CALLABLE_IS(v) &&                                               \
   ((((uint64_t)(v)) & NY_VALUE_INT_TAG_BIT) == 0) &&                          \
   (uintptr_t)(v) > NY_VALUE_PTR_MIN_ADDR)

#define memset_manual(p, v, n)                                                 \
  do {                                                                         \
    unsigned char *_p = (unsigned char *)(p);                                  \
    unsigned char _v = (unsigned char)(v);                                     \
    size_t _n = (n);                                                           \
    while (_n-- > 0)                                                           \
      *_p++ = _v;                                                              \
  } while (0)

static inline bool rt_env_is_truthy(const char *v) {
  return ny_env_is_truthy(v);
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

extern volatile uint64_t g_dbg_mincore_calls;
extern volatile uint64_t g_dbg_readable_entries;
extern volatile uint64_t g_dbg_readable_fast;
extern volatile uint64_t g_dbg_readable_array_hit;
extern volatile uint64_t g_dbg_enabled;
void dbg_maybe_register(void);

#define DBG_CALLER_SLOTS 64
extern volatile uint64_t g_dbg_caller_ret[DBG_CALLER_SLOTS];
extern volatile uintptr_t g_dbg_caller_ips[DBG_CALLER_SLOTS];
#define RT_DBG_NOTE_CALLER()                                        \
  do {                                                              \
    uintptr_t dbg_ra = (uintptr_t)__builtin_return_address(0);      \
    uintptr_t dbg_lo = (uintptr_t)dbg_ra & 0xfff;                   \
    for (unsigned dbg_i = 0; dbg_i < DBG_CALLER_SLOTS; ++dbg_i) {   \
      if (g_dbg_caller_ips[dbg_i] == dbg_ra) {                      \
        g_dbg_caller_ret[dbg_i]++;                                  \
        break;                                                      \
      }                                                             \
      if (g_dbg_caller_ips[dbg_i] == 0) {                           \
        g_dbg_caller_ips[dbg_i] = dbg_ra;                           \
        g_dbg_caller_ret[dbg_i] = 1;                                \
        (void)dbg_lo;                                               \
        break;                                                      \
      }                                                             \
    }                                                               \
  } while (0)

/*
 * Known-address-space oracle.  A lazy snapshot of /proc/self/maps plus ranges
 * registered by every runtime allocation provides a syscall-free answer to
 * "is [p, p+n) mapped and readable?"  Genuine objects are always inside a
 * registered range, so a probe that lands nowhere near any known mapping can
 * only be an arbitrary integer masquerading as a pointer (the mincore storm in
 * `acc += a[i]` loops).  Rejecting those without a syscall is safe: their old
 * path failed mincore (unmapped) or failed the magic checks (garbage) and
 * converged on the same non-object classification.  Anything indecisive still
 * falls through to the real mincore probe for correctness.  Linux only.
 */
#define RT_ORACLE_SNAPSHOT_MAX 4096
#define RT_ORACLE_EXTRA_MAX 256
#define RT_ORACLE_DEDUP 8
#define RT_ORACLE_GAP_DEFAULT ((uintptr_t)1 << 30)

typedef struct rt_oracle_range {
  uintptr_t start; /* inclusive */
  uintptr_t end;   /* exclusive */
} rt_oracle_range_t;

extern rt_oracle_range_t g_rt_oracle_snapshot[RT_ORACLE_SNAPSHOT_MAX];
extern uint32_t g_rt_oracle_snapshot_count;
extern rt_oracle_range_t g_rt_oracle_extra[RT_ORACLE_EXTRA_MAX];
extern _Atomic uint32_t g_rt_oracle_extra_count;
extern _Atomic uintptr_t g_rt_oracle_lo;
extern _Atomic uintptr_t g_rt_oracle_hi;
extern _Atomic uint32_t g_rt_oracle_ready;
extern _Atomic uint32_t g_rt_oracle_full;
extern uintptr_t g_rt_oracle_gap;

void rt_map_oracle_init(void);
void rt_map_oracle_add(uintptr_t start, size_t size);

/* 1 = mapped+readable (authoritative), -1 = not mapped (authoritative),
 * 0 = unknown, caller must verify with a real probe. */
static inline int rt_map_oracle_check(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0)
    return 0;
  if (p > UINTPTR_MAX - n)
    return 0;
  uintptr_t e = p + n - 1;
  if (!atomic_load_explicit(&g_rt_oracle_ready, memory_order_relaxed))
    rt_map_oracle_init();
  int reject_ok =
      !atomic_load_explicit(&g_rt_oracle_full, memory_order_relaxed);
  uintptr_t gap = reject_ok ? g_rt_oracle_gap : 0;
  uintptr_t lo = atomic_load_explicit(&g_rt_oracle_lo, memory_order_acquire);
  uintptr_t hi = atomic_load_explicit(&g_rt_oracle_hi, memory_order_acquire);
  if (reject_ok) {
    if (e < lo && lo - e > gap)
      return -1;
    if (p > hi && p - hi > gap)
      return -1;
  }
  uint32_t extra_n =
      atomic_load_explicit(&g_rt_oracle_extra_count, memory_order_acquire);
  for (uint32_t i = 0; i < extra_n; ++i) {
    if (p >= g_rt_oracle_extra[i].start && e <= g_rt_oracle_extra[i].end)
      return 1;
  }
  uint32_t cnt = g_rt_oracle_snapshot_count;
  if (cnt == 0)
    return 0;
  uint32_t lo2 = 0, hi2 = cnt;
  while (lo2 < hi2) {
    uint32_t mid = lo2 + (hi2 - lo2) / 2;
    if (g_rt_oracle_snapshot[mid].start <= p)
      lo2 = mid + 1;
    else
      hi2 = mid;
  }
  if (lo2 > 0) {
    const rt_oracle_range_t *r = &g_rt_oracle_snapshot[lo2 - 1];
    if (e <= r->end)
      return 1;
    if (!reject_ok || lo2 >= cnt || p <= r->end)
      return 0;
    uintptr_t gap_prev = p - r->end;
    uintptr_t gap_next = g_rt_oracle_snapshot[lo2].start - e;
    if (gap_prev > gap && gap_next > gap)
      return -1;
  }
  return 0;
}

static inline int rt_addr_mapped(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0)
    return 0;
  if (p > UINTPTR_MAX - n)
    return 0;
#if !defined(_WIN32) && !defined(__APPLE__)
  int oracle = rt_map_oracle_check(p, n);
  if (oracle == 1)
    return 1;
  if (oracle == -1)
    return 0;
#endif
  if (g_dbg_enabled) {
    g_dbg_mincore_calls++;
    RT_DBG_NOTE_CALLER();
  }
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {0};
  if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)))
    return 0;
  if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS ||
      (mbi.Protect & PAGE_GUARD))
    return 0;
  return 1;
#elif defined(__APPLE__)
  mach_vm_address_t region = (mach_vm_address_t)p;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(
      mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
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
extern int64_t rt_cstr_to_str(int64_t p);
extern int64_t rt_magic_tbuf_elem_size(int64_t v);
static __thread uintptr_t
    rt_heap_ptr_neg_cache_keys[RT_HEAP_PTR_NEG_CACHE_SIZE];
static __thread uintptr_t rt_const_str_cache_keys[RT_CONST_STR_CACHE_SIZE];
static __thread uintptr_t rt_non_str_cache_keys[RT_NON_STR_CACHE_SIZE];
static __thread uintptr_t rt_float_cache_keys[RT_FLOAT_CACHE_SIZE];
static __thread uintptr_t rt_non_float_cache_keys[RT_FLOAT_CACHE_SIZE];
static __thread uintptr_t
    rt_readable_hdr_page_cache[RT_READABLE_HDR_PAGE_CACHE_SIZE];

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
  uint64_t epoch =
      atomic_load_explicit(&rt_heap_ptr_global_epoch, memory_order_relaxed);
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
  rt_heap_ptr_cache_epoch =
      atomic_load_explicit(&rt_heap_ptr_global_epoch, memory_order_relaxed);
#endif
  rt_heap_ptr_cache_keys[rt_heap_ptr_cache_slot(p)] = p;
  rt_heap_ptr_neg_cache_keys[rt_heap_ptr_neg_cache_slot(p)] = 0;
}

static inline void rt_heap_ptr_cache_forget(uintptr_t p) {
  rt_heap_ptr_cache_keys[rt_heap_ptr_cache_slot(p)] = 0;
  rt_heap_ptr_neg_cache_keys[rt_heap_ptr_neg_cache_slot(p)] = 0;
#if NY_STRICT_HEAP_EPOCH
  rt_heap_ptr_cache_epoch = atomic_fetch_add_explicit(&rt_heap_ptr_global_epoch,
                                                      1, memory_order_relaxed) +
                            1;
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
  rt_non_str_cache_keys[((p >> 4) ^ (p >> 12) ^ (p >> 21)) &
                        RT_NON_STR_CACHE_MASK] = 0;
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

static inline uintptr_t rt_page_base_4k(uintptr_t p) {
  return p & ~(uintptr_t)4095ULL;
}

static inline uintptr_t rt_page_cache_slot(uintptr_t pg, uintptr_t mask) {
  return ((pg >> 12) ^ (pg >> 21) ^ (pg >> 30)) & mask;
}

static inline int rt_addr_readable(uintptr_t p, size_t n) {
  static __thread uintptr_t last_pg = 0;
  static __thread uintptr_t cache[RT_PAGE_CACHE_SIZE];
  if (g_dbg_enabled)
    g_dbg_readable_entries++;
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
    if (g_dbg_enabled)
      g_dbg_readable_fast++;
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
  if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS ||
      (mbi.Protect & PAGE_GUARD))
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
  if (g_dbg_enabled)
    g_dbg_readable_entries++;
  if (p < 0x1000 || n == 0 || p > UINTPTR_MAX - n)
    return 0;
  uintptr_t pg1 = rt_page_base_4k(p);
  uintptr_t pg2 = rt_page_base_4k(p + n - 1);
  if (pg1 == last_pg1 && pg2 == last_pg2) {
    if (g_dbg_enabled)
      g_dbg_readable_fast++;
    return 1;
  }
  uintptr_t h1 = rt_page_cache_slot(pg1, RT_READABLE_HDR_PAGE_CACHE_MASK);
  if (rt_readable_hdr_page_cache[h1] != pg1)
    return 0;
  if (pg2 != pg1) {
    uintptr_t h2 = rt_page_cache_slot(pg2, RT_READABLE_HDR_PAGE_CACHE_MASK);
    if (rt_readable_hdr_page_cache[h2] != pg2)
      return 0;
  }
  if (g_dbg_enabled)
    g_dbg_readable_array_hit++;
  last_pg1 = pg1;
  last_pg2 = pg2;
  return 1;
}

static inline void rt_readable_hdr_cache_store(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0 || p > UINTPTR_MAX - n)
    return;
  uintptr_t pg1 = rt_page_base_4k(p);
  uintptr_t pg2 = rt_page_base_4k(p + n - 1);
  rt_readable_hdr_page_cache[rt_page_cache_slot(
      pg1, RT_READABLE_HDR_PAGE_CACHE_MASK)] = pg1;
  if (pg2 != pg1)
    rt_readable_hdr_page_cache[rt_page_cache_slot(
        pg2, RT_READABLE_HDR_PAGE_CACHE_MASK)] = pg2;
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
#define is_any_ptr(v)                                                          \
  (((v) != 0 && ((((uint64_t)(v)) & NY_VALUE_INT_TAG_BIT) == 0) &&             \
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
  if (NY_DYNAMIC_CALLABLE_IS(v))
    return (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(v);
  if (NY_NATIVE_IS(v))
    return (int64_t)(((uint64_t)v) >> NY_NATIVE_SHIFT);
  return v;
}

static inline int rt_is_nil_imm(int64_t v) { return v == NY_IMM_NIL; }
static inline int rt_is_true_imm(int64_t v) { return v == NY_IMM_TRUE; }
static inline int rt_is_false_imm(int64_t v) { return v == NY_IMM_FALSE; }
static inline int rt_is_bool_imm(int64_t v) {
  return v == NY_IMM_TRUE || v == NY_IMM_FALSE;
}
static inline int rt_is_falsy(int64_t v) {
  return v == NY_IMM_NIL || v == NY_IMM_FALSE || v == rt_tag_v(0);
}
static inline int rt_is_truthy(int64_t v) { return !rt_is_falsy(v); }

#if UINTPTR_MAX == 0xffffffff
static inline int64_t rt_mask_ptr(int64_t v) { return (int64_t)(v & ~2ULL); }
#define NY_NATIVE_ENCODE(p)                                                    \
  ((int64_t)(NY_NATIVE_MARK | (((uint64_t)(uintptr_t)(p) << NY_NATIVE_SHIFT) | \
                               (uint64_t)NY_NATIVE_TAG)))
#define NY_NATIVE_DECODE(v)                                                    \
  ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_NATIVE_MARK) >> NY_NATIVE_SHIFT))
#else
static inline int64_t rt_mask_ptr(int64_t v) {
  return (int64_t)((uint64_t)v & ~NY_VALUE_PTR_TAG_MASK);
}
#define NY_NATIVE_ENCODE(p)                                                    \
  ((int64_t)(((uint64_t)(uintptr_t)(p) << NY_NATIVE_SHIFT) |                   \
             (uint64_t)NY_NATIVE_TAG))
#define NY_NATIVE_DECODE(v)                                                    \
  ((void *)(uintptr_t)(((uint64_t)(v)) >> NY_NATIVE_SHIFT))
#endif

#define TAG_LIST 100
#define TAG_DICT 101
#define TAG_SET 102
#define TAG_TUPLE 103
#define TAG_OK 104
#define TAG_ERR 105
#define TAG_RANGE 106
#define TAG_CLOSURE 107
#define TAG_DICT_TBL 108
#define TAG_FLOAT 110
#define TAG_COMPLEX 111
#define TAG_STR 120
#define TAG_STR_CONST 121
#define TAG_BYTES 122
#define TAG_BIGINT 130
#define TAG_BIGFLOAT 131
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
  if (n == 8 && memcmp(s, "dict_tbl", 8) == 0)
    return TAG_DICT_TBL;
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
  if (n == 8 && memcmp(s, "bigfloat", 8) == 0)
    return TAG_BIGFLOAT;
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

static inline int is_v_flt_mapped(int64_t v) { return is_v_flt(v); }

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

/* Return string length for a tagged Ny string-like object; 0 when unavailable.
 */
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
  return *(int64_t *)((char *)(uintptr_t)lst + 16 + i_v * 8);
}

static inline int64_t _rt_store_item_fast(int64_t lst, int64_t i_v,
                                          int64_t val) {
  if (!is_ptr(lst))
    return 0;
  *(int64_t *)((char *)(uintptr_t)lst + 16 + i_v * 8) = val;
  return val;
}

void rt_cleanup_args(void);
void rt_cleanup_small_strings(void);
int64_t rt_set_args(int64_t argc, int64_t argv, int64_t envp);
int64_t rt_set_args_raw(int argc, char **argv, char **envp);
int _ny_aot_set_args(int argc, char **argv, char **envp);
int64_t rt_env_get(int64_t key);
int64_t rt_argv_get(int64_t index);
int64_t rt_envc_raw(void);
int64_t rt_atoi(int64_t value);
double rt_atof(int64_t value);
int64_t rt_getlogin(void);
int64_t rt_gettimeofday(int64_t tv, int64_t tz);
int64_t rt_malloc(int64_t n);
int64_t rt_malloc_i64(int64_t n);
bool rt_raw_ptr_registered(int64_t ptr);
void rt_raw_ptr_register(int64_t ptr);
bool rt_raw_ptr_unregister(int64_t ptr);
int64_t rt_malloc_uninit(int64_t n);
int64_t rt_free(int64_t ptr);
int64_t rt_ptr_add(int64_t ptr, int64_t offset);
int64_t rt_ptr_sub(int64_t ptr, int64_t offset);
int64_t rt_ptr_add_i64(int64_t ptr, int64_t offset);
int64_t rt_ptr_sub_i64(int64_t ptr, int64_t offset);
int64_t rt_pool_release(void);
int64_t rt_ptr_key(int64_t ptr);
int64_t rt_atomic_load64(int64_t addr, int64_t idx);
int64_t rt_atomic_store64(int64_t addr, int64_t idx, int64_t value);
int64_t rt_atomic_add64(int64_t addr, int64_t idx, int64_t delta);
int64_t rt_atomic_sub64(int64_t addr, int64_t idx, int64_t delta);
int64_t rt_atomic_exchange64(int64_t addr, int64_t idx, int64_t value);
int64_t rt_atomic_cas64(int64_t addr, int64_t idx, int64_t expected,
                        int64_t desired);
int64_t rt_drop_owned(int64_t ptr);
int64_t rt_drop_owned_slot(int64_t slot_ptr);
int64_t rt_runtime_cleanup(void);
int64_t rt_fix_fn_ptr(int64_t fn);
int64_t rt_mark_dynamic_callable(int64_t fn);
int64_t rt_mark_dynamic_bool_callable(int64_t fn);
int64_t rt_mark_dynamic_bool_callable_tagged_args(int64_t fn);
int64_t rt_mark_dynamic_callable_tagged_args(int64_t fn);
int64_t rt_mark_dynamic_callable_raw_result(int64_t fn);
int64_t rt_mark_dynamic_callable_tagged_args_raw_result(int64_t fn);
int64_t rt_flt_box_val(int64_t bits);
double rt_flt_unbox_double(int64_t v);
int64_t rt_flt_box_double(double d);
int64_t rt_str_concat(int64_t a, int64_t b);
int64_t rt_str_builder_new(int64_t cap_v);
int64_t rt_str_builder_append(int64_t builder_v, int64_t value);
int64_t rt_str_builder_to_str(int64_t builder_v);
int64_t rt_str_builder_free(int64_t builder_v);
int64_t rt_dict_reserve(int64_t d, int64_t additional);

int64_t rt_has_tag(int64_t v, int64_t tag_v);
int64_t rt_raw_truthy(int64_t v);
int64_t rt_store_item_fast(int64_t lst, int64_t i_v, int64_t val);

int64_t rt_eq(int64_t a, int64_t b);
int64_t rt_trace_last_file(void);
int64_t rt_trace_last_line(void);
int64_t rt_trace_last_col(void);
int64_t rt_trace_last_func(void);
void rt_trace_refresh_env(void);
void print_trace_entry(int64_t file, int64_t line, int64_t col, int64_t func,
                       const char *prefix);

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

static inline int rt_check_oob(const char *op, int64_t addr, int64_t idx,
                               size_t access_sz) {
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

/* Small-string optimization threshold and short-string interning. */
#define RT_SSO_MAX 23
int64_t rt_str_intern_lookup(const char *s, size_t len);
void rt_str_intern_insert(const char *s, size_t len, int64_t str);
void rt_str_intern_clear(void);
int64_t rt_str_hash(int64_t v);

int64_t rt_panic(int64_t msg_ptr);
int64_t rt_division_by_zero(void);
int64_t rt_modulo_by_zero(void);
int64_t rt_runtime_tag(int64_t name);
int64_t rt_init_str(int64_t p, int64_t n);
int64_t rt_bytes_new(int64_t n);
int64_t rt_kwarg_new(int64_t key, int64_t value);
int64_t rt_range_new(int64_t start, int64_t stop, int64_t step);
int64_t rt_range_values_raw(int64_t range);
int64_t rt_list_as_tuple(int64_t lst);
int64_t rt_list_new_raw(int64_t capacity);
int64_t rt_list_new_sized(int64_t n);
int64_t rt_list_reserve(int64_t lst, int64_t cap);
int64_t rt_list_sum_int_range(int64_t lst, int64_t start, int64_t stop);
int64_t rt_list_len(int64_t lst);
int64_t rt_list_set_len(int64_t lst, int64_t n);
int64_t rt_load_item(int64_t lst, int64_t i);
int64_t rt_load_item_fast(int64_t lst, int64_t i);
int64_t rt_load_item_any(int64_t lst, int64_t i);
int64_t rt_flt_unbox_val(int64_t v);
void rt_flt_free(int64_t v);
int64_t rt_index_read_probe_enabled(void);
int64_t rt_index_read_probe(int64_t tag, int64_t idx, int64_t path);

int64_t rt_bigint_to_str(int64_t a);
int64_t rt_bigfloat_to_str(int64_t a);
int64_t rt_bigint_from_i64_raw(int64_t v);
int64_t rt_bigint_to_i64_raw(int64_t a);
int64_t rt_bigint_to_int(int64_t a);
int64_t rt_bigint_cmp_raw(int64_t a, int64_t b);
int64_t rt_bigint_bitlen_raw(int64_t a);
int64_t rt_native_has_tag(int64_t value, int64_t tag);
/* Canonical dynamic-value predicates.  The rt_native_* spellings below are
 * ABI compatibility exports for older generated modules only. */
int64_t rt_value_tag(int64_t value);
int64_t rt_is_str(int64_t value);
int64_t rt_async_is_handle(int64_t value);
int64_t rt_tag_or_raw_int(int64_t value);
int64_t rt_raw_word_tag(int64_t value);
int64_t rt_result_unwrap_raw(int64_t value);
int64_t rt_result_unwrap_or_raw(int64_t value, int64_t fallback);
int64_t rt_native_is_str(int64_t value);
int64_t rt_native_is_int(int64_t value);
int64_t rt_bigfloat_from_value_raw(int64_t v, int64_t precision);
int64_t rt_bigfloat_from_f64_bits(double d, int64_t precision);
double rt_bigfloat_to_f64_raw(int64_t a);
int64_t rt_bigfloat_cmp_raw(int64_t a, int64_t b);
int64_t rt_bigfloat_precision_raw(int64_t a);
int64_t rt_bigfloat_pow_int_raw(int64_t a, int64_t exponent);
int64_t rt_bigfloat_zero(int64_t precision_v);
int64_t rt_bigfloat_one(int64_t precision_v);
int64_t rt_bigfloat_add(int64_t av, int64_t bv);
int64_t rt_bigfloat_sub(int64_t av, int64_t bv);
int64_t rt_bigfloat_mul(int64_t av, int64_t bv);
int64_t rt_bigfloat_div(int64_t av, int64_t bv);
int64_t rt_bigfloat_neg(int64_t av);
int64_t rt_bigfloat_abs(int64_t av);
int64_t rt_bigfloat_sqrt(int64_t av);
int64_t rt_bigfloat_is(int64_t v);
int64_t rt_f64_to_i64(double v);
int64_t rt_f64_bits(double v);
int64_t rt_type_name(int64_t value);
int64_t rt_type_name_tagged(int64_t value, int64_t tag);
double rt_sin_f64(double value);
double rt_cos_f64(double value);
double rt_f64_round(double value);
double rt_f64_pow(double base, double exponent);
double rt_f64_floor(double value);
double rt_f64_ceil(double value);
void rt_bounds_check(uint64_t offset, uint64_t len);
void rt_trap(void);
int64_t rt_print_i64_raw(int64_t v);
int64_t rt_print_f64_raw(double d);
/* Raw typed-buffer allocator used by the LLVM-free NYIR lowering. */
int64_t rt_thread_spawn_raw(int64_t fn, int64_t arg);
int64_t rt_zalloc_raw(int64_t size);
int64_t rt_zfree_raw(int64_t ptr);
/* Raw native replacement for std.core.term.get_terminal_size(). */
int64_t rt_terminal_size_raw(void);
int64_t rt_color_raw(int64_t text, int64_t color);
int64_t rt_bytes_get_raw(int64_t buffer, int64_t index, int64_t fallback);
int64_t rt_bytes_set_raw(int64_t buffer, int64_t index, int64_t value);
int64_t rt_bytes_new_raw(int64_t length);
int64_t rt_realloc_raw(int64_t p, int64_t n);
int64_t rt_args_raw(void);
int64_t rt_canvas(int64_t width, int64_t height);
int64_t rt_canvas_clear(int64_t canvas);
int64_t rt_canvas_set(int64_t canvas, int64_t x, int64_t y, int64_t ch,
                             int64_t color, int64_t bold);
int64_t rt_canvas_refresh(int64_t canvas);
int64_t rt_tui_canvas_loop(int64_t draw);
int64_t rt_sound_init(int64_t force_async);
int64_t rt_tui_begin(void);
int64_t rt_tui_end(void);
int64_t rt_poll_key(void);
int64_t rt_is_quit_key(int64_t key);
int64_t rt_file_exists(int64_t path);
int64_t rt_sound_backend_name(void);
int64_t rt_sound_noop(int64_t ignored);
double rt_sqrt_f64(double value);
double rt_load32_f64(int64_t addr, int64_t idx);
int64_t rt_store32_f64(int64_t addr, int64_t idx, double value);
int64_t rt_simmd_rotl32_i64(int64_t v, int64_t k_v);
int64_t rt_simmd_rotr32_i64(int64_t v, int64_t k_v);
int64_t rt_simmd_rotl64_i64(int64_t v, int64_t k_v);
int64_t rt_simmd_rotr64_i64(int64_t v, int64_t k_v);
int64_t rt_simmd_popcnt64_i64(int64_t v);
int64_t rt_simmd_ctz64_i64(int64_t v);
int64_t rt_simmd_clz64_i64(int64_t v);
int64_t rt_simmd_bswap64_i64(int64_t v);
int64_t rt_simmd_popcnt32_i64(int64_t v);
int64_t rt_simmd_ctz32_i64(int64_t v);
int64_t rt_simmd_clz32_i64(int64_t v);
int64_t rt_simmd_bswap32_i64(int64_t v);
int64_t rt_tbuf_new_raw(int64_t count, int64_t elem_size);
int64_t rt_tbuf_concat_raw(int64_t left, int64_t right);
int64_t rt_tbuf_slice(int64_t buffer, int64_t start, int64_t stop);
int64_t rt_any_eq(int64_t left, int64_t right);
int64_t rt_dict_new_raw(int64_t capacity);
int64_t rt_set_new(int64_t capacity);
int64_t rt_set_remove(int64_t set, int64_t key);
int64_t rt_dict_get_raw(int64_t dict, int64_t key, int64_t fallback);
int64_t rt_dict_get_i64_raw(int64_t dict, int64_t key, int64_t fallback);
int64_t rt_dict_set_raw(int64_t dict, int64_t key, int64_t value);
int64_t rt_dict_set_i64_raw(int64_t dict, int64_t key, int64_t value);
int64_t rt_native_dict_set_raw_i64(int64_t dict, int64_t key, int64_t key_len,
                                   int64_t key_tag, int64_t value,
                                   int64_t value_len, int64_t value_tag);
int64_t rt_native_dict_set_native_i64(int64_t dict, int64_t key, int64_t value);
int64_t rt_native_dict_set_fast_i64(int64_t dict, int64_t key, int64_t key_len,
                                    int64_t key_tag, int64_t value,
                                    int64_t value_len, int64_t value_tag);
int64_t rt_native_dict_set_nir_i64(int64_t dict, int64_t key, int64_t value);
int64_t rt_dict_get_str_raw(int64_t dict, int64_t key, int64_t fallback);
int64_t rt_value_get_tagged(int64_t value, int64_t key, int64_t fallback);
/* Dynamic container lookup with an explicitly raw (unboxed) integer index. */
int64_t rt_value_get_index_raw(int64_t value, int64_t index,
                               int64_t fallback);
int64_t rt_value_set_tagged(int64_t value, int64_t key, int64_t item);
int64_t rt_call_any1(int64_t fn, int64_t value);
int64_t rt_call_any2(int64_t fn, int64_t left, int64_t right);
int64_t rt_native_dict_set_str_compact(int64_t dict, int64_t key,
                                       int64_t value);
int64_t rt_dict_set_str_raw(int64_t dict, int64_t key, int64_t value);
int64_t rt_dict_has_str_raw(int64_t dict, int64_t key);
int64_t rt_dict_delete_str_raw(int64_t dict, int64_t key);
int64_t rt_dict_merge_raw(int64_t dict, int64_t other);
int64_t rt_dict_clone_raw(int64_t dict);
int64_t rt_dict_clear_raw(int64_t dict);
int64_t rt_dict_has_raw(int64_t dict, int64_t key);
int64_t rt_dict_len_raw(int64_t dict);
int64_t rt_dict_delete_raw(int64_t dict, int64_t key);
int64_t rt_assert_cstr(int64_t condition, int64_t message_ptr);
int64_t rt_print_cstr(int64_t p);
int64_t rt_print_i64_raw(int64_t val);
int64_t rt_print_f64_raw(double d);
int64_t rt_print_str_raw(int64_t v);
int64_t rt_print_value(int64_t v);
int64_t rt_print_int(int64_t v);
int64_t rt_print_newline(void);
int64_t rt_dict_keys_raw(int64_t dict);
int64_t rt_dict_values_raw(int64_t dict);
int64_t rt_dict_items_raw(int64_t dict);
int64_t rt_runtime_tag_raw_value(int64_t name);
int64_t rt_tbuf_append_raw(int64_t buffer, int64_t value, int64_t is_string);
int64_t rt_tbuf_append_tagged(int64_t buffer, int64_t value,
                                  int64_t is_string);
int64_t rt_tbuf_set_tagged(int64_t buffer, int64_t index, int64_t value);
int64_t rt_tbuf_set_i64_raw(int64_t buffer, int64_t index, int64_t value);
int64_t rt_tbuf_set_f64_bits(int64_t buffer, int64_t index, int64_t bits);
int64_t rt_tbuf_to_cstr(int64_t buffer);
int64_t rt_shl_raw(int64_t a, int64_t b);
int64_t rt_trace_func_raw(int64_t name);
int64_t rt_trace_loc_raw(int64_t file, int64_t line, int64_t col);
int64_t rt_trace_enter_raw(int64_t func, int64_t file, int64_t line);
int64_t rt_print_flush_raw(void);
int64_t rt_trace_ret_void_raw(void);
int64_t rt_trace_exit_raw(void);
int64_t rt_trace_dump_raw(int64_t count);
int64_t rt_tbuf_append_i64_raw(int64_t buffer, int64_t value);
int64_t rt_tbuf_repeat(int64_t buffer, int64_t repeat_count);
int64_t rt_tbuf_extend(int64_t buffer, int64_t other);
int64_t rt_tbuf_len_raw(int64_t buffer);
int64_t rt_tbuf_reserve(int64_t buffer, int64_t capacity);
int64_t rt_tbuf_clear_raw(int64_t buffer);
int64_t rt_tbuf_eq_raw(int64_t left, int64_t right);
int64_t rt_tbuf_clone_raw(int64_t buffer);
int64_t rt_tbuf_swap(int64_t buffer, int64_t left, int64_t right);
int64_t rt_tbuf_get(int64_t buffer, int64_t index, int64_t fallback);
int64_t rt_tbuf_get_any(int64_t buffer, int64_t index, int64_t fallback);
int64_t rt_tbuf_dyn_elem(int64_t buffer, int64_t index,
                         int64_t want_dynamic);
int64_t rt_tbuf_tag(int64_t buffer, int64_t index);
int64_t rt_tbuf_contains(int64_t buffer, int64_t item, int64_t is_string);
int64_t rt_contains_raw(int64_t container, int64_t item);
double rt_value_to_f64(int64_t value, int64_t tag);
int64_t rt_tbuf_pop_raw(int64_t buffer);
int64_t rt_load8_raw(int64_t addr, int64_t idx);
int64_t rt_store8_raw(int64_t addr, int64_t idx, int64_t value);
int64_t rt_cstr_len(int64_t value);
int64_t rt_len(int64_t value);
int64_t rt_len_strict(int64_t value);
int64_t rt_sequence_len_raw(int64_t value);
int64_t rt_sequence_len_safe(int64_t value);
int64_t rt_cstr_builder_new(int64_t initial);
int64_t rt_cstr_builder_append(int64_t builder, int64_t suffix);
int64_t rt_cstr_builder_finalize(int64_t builder);
int64_t rt_cstr_replace(int64_t s, int64_t old_s, int64_t new_s);
int64_t rt_i64_min(int64_t a, int64_t b);
int64_t rt_i64_max(int64_t a, int64_t b);
double rt_f64_min(double a, double b);
double rt_f64_max(double a, double b);
int64_t rt_bool_to_cstr(int64_t value);
int64_t rt_i64_to_cstr_raw(int64_t v);
int64_t rt_f64_to_cstr_raw(double v);
double rt_any_to_f64(int64_t v);
double rt_fmod_f64(double a, double b);
int64_t rt_contains_raw(int64_t collection, int64_t key);
int64_t rt_cstr_eq(int64_t a_ptr, int64_t b_ptr);
int64_t rt_cstr_cmp(int64_t a_ptr, int64_t b_ptr);
int64_t rt_cstr_concat(int64_t a_ptr, int64_t b_ptr);
int64_t rt_cstr_repeat(int64_t str_ptr, int64_t count);
int64_t rt_cstr_slice(int64_t str_ptr, int64_t start, int64_t stop);
int64_t rt_any_to_cstr(int64_t v);
int64_t rt_bigint_to_cstr_raw(int64_t v);
int64_t rt_any_add(int64_t left, int64_t right);
int64_t rt_raw_add(int64_t left, int64_t right);
int64_t rt_any_to_i64(int64_t value);
int64_t rt_bigint_neg_raw(int64_t value);
int64_t rt_tbuf_index_read_raw(int64_t buffer, int64_t index);
int64_t rt_tbuf_index_any(int64_t buffer, int64_t index);
int64_t rt_tbuf_index_any_raw(int64_t buffer, int64_t index);
int64_t rt_index_key_error(void);
int64_t rt_bytes_index_read_raw(int64_t buffer, int64_t index);
int64_t rt_range_index_read_raw(int64_t range, int64_t index);
int64_t rt_cstr_index_read_raw(int64_t str_v, int64_t idx_v);
int64_t rt_value_is_ptr(int64_t v);
int64_t rt_native_is_ptr(int64_t v); /* compatibility alias */
double rt_vec_dot_raw(int64_t left, int64_t right);
int64_t rt_vec_add_raw(int64_t left, int64_t right);
int64_t rt_vec_sub_raw(int64_t left, int64_t right);
int64_t rt_vec_div_component_raw(int64_t left, int64_t right);
int64_t rt_vec_mul_scalar_raw(int64_t v, double s);
int64_t rt_vec_div_scalar_raw(int64_t v, double s);
int64_t rt_any_mul(int64_t left, int64_t right);
int64_t rt_any_div(int64_t left, int64_t right);
int64_t rt_any_mod(int64_t left, int64_t right);

/* GC and FFI gates. */
#include "code/runtime/ffigates.h"
#include "code/runtime/gc.h"

static inline size_t rt_gc_size_arg(int64_t size) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  return n > 0 ? (size_t)n : 0u;
}

static inline int64_t rt_gc_alloc(int64_t size) {
  return nyGcAlloc(rt_gc_size_arg(size));
}

static inline int64_t rt_gc_alloc_fast(int64_t size) {
  return nyGcAllocFast(rt_gc_size_arg(size));
}

static inline int64_t rt_gc_alloc_slow(int64_t size) {
  return nyGcAllocSlow(rt_gc_size_arg(size));
}

static inline void rt_gc_collect(void) { nyGcCollect(); }

static inline void rt_gc_trigger_minor(void) { nyGcTriggerMinor(); }

static inline void rt_gc_trigger_major(void) { nyGcTriggerMajor(); }

static inline void rt_gc_write_barrier(int64_t *slot, int64_t value) {
  nyGcWriteBarrier(slot, value);
}

static inline int64_t rt_ffi_call(void *fn, int64_t *args, size_t argc) {
  return nyFfiCallGeneric(fn, args, argc);
}

static inline int64_t rt_ffi_call_fast_i_i(int64_t fn, int64_t a0) {
  return nyFfiFastII(fn, a0);
}

static inline int64_t rt_ffi_call_fast_i_ii(int64_t fn, int64_t a0,
                                            int64_t a1) {
  return nyFfiFastIIi(fn, a0, a1);
}

static inline int64_t rt_ffi_call_fast_i_iii(int64_t fn, int64_t a0, int64_t a1,
                                             int64_t a2) {
  return nyFfiFastIIii(fn, a0, a1, a2);
}

static inline int64_t rt_ffi_call_fast_i_pi(int64_t fn, int64_t ptr,
                                            int64_t idx) {
  return nyFfiFastPII(fn, ptr, idx);
}

/* FFI call dispatch forward declarations */
int64_t rt_call0(int64_t f);
int64_t rt_call0_ptr(int64_t f);
int64_t rt_call1(int64_t f, int64_t a0);
int64_t rt_call1_ptr(int64_t f, int64_t a0);
int64_t rt_call2(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call2_ptr(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call2_ptr_u32(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call3(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr_u64_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr_u32_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr_ptr_u32(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t rt_call4_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t rt_call4_ptr_ptr_ptr_ptr_void(int64_t f, int64_t a0, int64_t a1,
                                      int64_t a2, int64_t a3);
int64_t rt_call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4);
int64_t rt_call5_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4);
int64_t rt_call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5);
int64_t rt_call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6);
int64_t rt_call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7);
int64_t rt_call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8);
int64_t rt_call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9);
int64_t rt_call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10);
int64_t rt_call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11);
int64_t rt_call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11, int64_t a12);
int64_t rt_call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11, int64_t a12,
                  int64_t a13);
int64_t rt_call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11, int64_t a12,
                  int64_t a13, int64_t a14);

/* ny_value_* — the clean boundary between typed and dynamic worlds.
 *
 * TYPED WORLD (compiler, NYIR, native codegen, layout slots):
 *   i64 = raw 64-bit. No tagging. No shifting.
 *   Every register, stack slot, function parameter, layout field, and
 *   ABI slot holds the real value. Arithmetic is raw i64.
 *
 * DYNAMIC WORLD (heterogeneous containers, any boxes, runtime dispatch):
 *   NyValue representation with tag bits for type discrimination.
 *   Boxing: raw i64 -> tagged NyValue when entering any/dict/list.
 *   Unboxing: tagged NyValue -> raw i64 when leaving any.
 *
 * These are the ONLY boundary points where tag/untag happens.
 * All tag masks, shifts, and encoding details are private here.
 */

/* Value kind constants for ny_value_kind() */
#define NY_VALUE_KIND_NIL 0
#define NY_VALUE_KIND_FALSE 1
#define NY_VALUE_KIND_TRUE 2
#define NY_VALUE_KIND_INT 3
#define NY_VALUE_KIND_PTR 4
#define NY_VALUE_KIND_NATIVE 5

/* Typed world: raw i64 operations (NO tagging, NO allocation) */

/* Identity: raw i64 is already raw i64. The typed world never tags. */
static inline int64_t ny_value_from_i64_raw(int64_t raw) { return raw; }
static inline int64_t ny_value_to_i64_raw(int64_t v) { return v; }

/* Dynamic world: tagged NyValue operations */

/* Box a raw i64 into the tagged NyValue representation. */
static inline int64_t ny_value_from_i64(int64_t raw) {
  return ny_small_int_fits_i64(raw) ? rt_tag_v(raw)
                                    : rt_bigint_from_i64_raw(raw);
}

/* Unbox a tagged NyValue to raw i64. Caller must ensure is_int(v). */
static inline int64_t ny_value_to_i64(int64_t v) {
  return is_int(v) ? (v >> NY_VALUE_INT_SHIFT) : rt_bigint_to_i64_raw(v);
}

/* Try to unbox a tagged NyValue. Returns 1 and writes *out on success,
 * returns 0 if v is not a tagged integer. */
static inline int ny_value_try_i64(int64_t v, int64_t *out) {
  if (is_int(v)) {
    *out = v >> NY_VALUE_INT_SHIFT;
    return 1;
  }
  return 0;
}

/* Check if a raw i64 fits in the tagged representation (62-bit signed range).
 */
static inline int ny_value_is_small_int(int64_t raw) {
  return ny_small_int_fits_i64(raw);
}

/* Classify a NyValue without extracting it. */
static inline int ny_value_kind(int64_t v) {
  if (v == NY_IMM_NIL)
    return NY_VALUE_KIND_NIL;
  if (v == NY_IMM_FALSE)
    return NY_VALUE_KIND_FALSE;
  if (v == NY_IMM_TRUE)
    return NY_VALUE_KIND_TRUE;
  if (is_int(v))
    return NY_VALUE_KIND_INT;
  if (NY_NATIVE_IS(v))
    return NY_VALUE_KIND_NATIVE;
  if (is_ptr(v))
    return NY_VALUE_KIND_PTR;
  return NY_VALUE_KIND_NIL; /* fallback */
}

/* Check if a NyValue is a heap object (not immediate, not native). */
static inline int ny_value_is_heap_object(int64_t v) {
  return is_ptr(v) && !NY_NATIVE_IS(v) && rt_heap_object_ptr(v) != 0;
}

/* Get the heap tag of a NyValue (0 if not a heap object). */
static inline int64_t ny_value_heap_tag(int64_t v) {
  int64_t hp = rt_heap_object_ptr(v);
  if (!hp)
    return 0;
  return *(int64_t *)((char *)(uintptr_t)hp - 8);
}

int64_t rt_adt_alloc(int64_t nfields, int64_t tag);
int64_t rt_adt_tag(int64_t v);
int64_t rt_cstr_get_raw(int64_t str_v, int64_t idx_v);

/* Boundary operations (the only places tagging happens) */

/* Box a raw i64 into a dynamic context. Values outside the tagged range use
 * the existing explicit bigint heap representation until a dedicated boxed
 * i64 object is introduced. */
static inline int64_t ny_value_box_i64(int64_t raw) {
  return ny_value_from_i64(raw);
}

/* Unbox a dynamic integer to raw i64. Non-integer values retain the legacy
 * zero fallback through the bigint conversion helper. */
static inline int64_t ny_value_unbox_i64(int64_t v) {
  return ny_value_to_i64(v);
}

#endif
