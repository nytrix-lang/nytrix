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

// Core Tags and Predicates
#define is_int(v) ((v) & 1)
#define is_ptr(v) ((v) != 0 && ((v) & 7) == 0 && (uintptr_t)(v) > 0x1000)

static inline int rt_addr_mapped(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0)
    return 0;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {0};
  uintptr_t end = p + n - 1;
  if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)))
    return 0;
  if (mbi.State != MEM_COMMIT)
    return 0;
  if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))
    return 0;
  uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize - 1;
  if (end > region_end) {
    MEMORY_BASIC_INFORMATION mbi2 = {0};
    if (!VirtualQuery((LPCVOID)end, &mbi2, sizeof(mbi2)))
      return 0;
    if (mbi2.State != MEM_COMMIT)
      return 0;
    if (mbi2.Protect == PAGE_NOACCESS || (mbi2.Protect & PAGE_GUARD))
      return 0;
  }
  return 1;
#else
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0)
    return 1;
  uintptr_t mask = (uintptr_t)ps - 1;
  uintptr_t start = p & ~mask;
  uintptr_t end = (p + n - 1) & ~mask;
#if defined(__linux__)
  unsigned char vec = 0;
#else
  char vec = 0;
#endif
  for (uintptr_t cur = start;; cur += (uintptr_t)ps) {
    if (mincore((void *)cur, (size_t)ps, &vec) != 0)
      return 0;
    if (cur == end)
      break;
  }
  return 1;
#endif
}

// Stronger check for locations we dereference directly in pointer/tag
// predicates. On Linux, mincore(2) only proves mapping presence, not
// readability.
static inline int rt_addr_readable(uintptr_t p, size_t n) {
  if (p < 0x1000 || n == 0)
    return 0;
  if (p > UINTPTR_MAX - n)
    return 0;
#ifdef __linux__
  uintptr_t end = p + n; // exclusive
  enum {
    RT_READABLE_CACHE_SLOTS = 64,
    RT_READABLE_MAP_CACHE_CAP = 1024,
    RT_READABLE_MAP_REFRESH_INTERVAL = 16384
  };
  typedef struct {
    uintptr_t lo;
    uintptr_t hi;
    unsigned char readable;
  } rt_readable_map_entry_t;

  static __thread uintptr_t cache_lo[RT_READABLE_CACHE_SLOTS] = {0};
  static __thread uintptr_t cache_hi[RT_READABLE_CACHE_SLOTS] = {0};
  static __thread unsigned char cache_valid[RT_READABLE_CACHE_SLOTS] = {0};
  static __thread rt_readable_map_entry_t map_cache[RT_READABLE_MAP_CACHE_CAP];
  static __thread size_t map_cache_len = 0;
  static __thread unsigned query_count = 0;

  unsigned cache_slot =
      (unsigned)((p >> 12) & (RT_READABLE_CACHE_SLOTS - 1u));
  if (cache_valid[cache_slot] && p >= cache_lo[cache_slot] &&
      end <= cache_hi[cache_slot]) {
    return 1;
  }

  bool need_refresh = (map_cache_len == 0);
  if (!need_refresh) {
    query_count++;
    if (query_count >= RT_READABLE_MAP_REFRESH_INTERVAL) {
      need_refresh = true;
      query_count = 0;
    }
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    if (need_refresh) {
      FILE *fp = fopen("/proc/self/maps", "r");
      if (!fp)
        return rt_addr_mapped(p, n);
      map_cache_len = 0;
      char line[256];
      while (fgets(line, sizeof(line), fp)) {
        if (map_cache_len >= RT_READABLE_MAP_CACHE_CAP)
          break;
        unsigned long long lo = 0, hi = 0;
        char perms[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) != 3)
          continue;
        uintptr_t map_lo = (uintptr_t)lo;
        uintptr_t map_hi = (uintptr_t)hi;
        if (map_hi <= map_lo)
          continue;
        map_cache[map_cache_len].lo = map_lo;
        map_cache[map_cache_len].hi = map_hi;
        map_cache[map_cache_len].readable = (perms[0] == 'r') ? 1 : 0;
        map_cache_len++;
      }
      fclose(fp);
      need_refresh = false;
    }

    uintptr_t start = p;
    uintptr_t cur = p;
    size_t lo_idx = 0;
    size_t hi_idx = map_cache_len;
    while (lo_idx < hi_idx) {
      size_t mid = lo_idx + ((hi_idx - lo_idx) >> 1);
      if (map_cache[mid].hi <= cur) {
        lo_idx = mid + 1;
      } else {
        hi_idx = mid;
      }
    }

    uintptr_t covered_lo = start;
    bool covered_set = false;
    for (size_t i = lo_idx; i < map_cache_len; i++) {
      uintptr_t map_lo = map_cache[i].lo;
      uintptr_t map_hi = map_cache[i].hi;
      if (map_hi <= cur)
        continue;
      if (map_lo > cur)
        break;
      if (!covered_set) {
        covered_lo = (map_lo < start) ? map_lo : start;
        covered_set = true;
      }
      if (!map_cache[i].readable) {
        cur = 0;
        break;
      }
      if (map_hi >= end) {
        cache_lo[cache_slot] = covered_lo;
        cache_hi[cache_slot] = map_hi;
        cache_valid[cache_slot] = 1;
        return 1;
      }
      cur = map_hi;
    }
    if (cur == 0)
      return 0;
    if (attempt == 0) {
      need_refresh = true;
      continue;
    }
    return 0;
  }
  return 0;
#elif defined(__APPLE__)
  uintptr_t cur = p;
  uintptr_t end = p + n;
  while (cur < end) {
    mach_vm_address_t region_addr = (mach_vm_address_t)cur;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info = {0};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr =
        mach_vm_region(mach_task_self(), &region_addr, &region_size,
                       VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
                       &object);
    if (object != MACH_PORT_NULL)
      mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS)
      return 0;
    if ((uintptr_t)region_addr > cur)
      return 0;
    if (!(info.protection & VM_PROT_READ))
      return 0;
    uintptr_t region_end = (uintptr_t)region_addr + (uintptr_t)region_size;
    if (region_end <= cur)
      return 0;
    if (end <= region_end)
      return 1;
    cur = region_end;
  }
  return 1;
#else
  return rt_addr_mapped(p, n);
#endif
}

static inline bool is_valid_heap_ptr(int64_t v) {
  if (!is_ptr(v) || (v & 63) != 0)
    return false;
  uintptr_t raw_p_start = (uintptr_t)v - 64;
  uintptr_t raw_p_magic2 = raw_p_start + 16;
  if (!rt_addr_readable(raw_p_start, 24))
    return false;
  uint64_t m1 = 0, m2 = 0;
  memcpy(&m1, (const void *)raw_p_start, sizeof(m1));
  memcpy(&m2, (const void *)raw_p_magic2, sizeof(m2));
  bool match = (m1 == NY_MAGIC1) && (m2 == NY_MAGIC2);
  return match;
}

#define is_heap_ptr(v) is_valid_heap_ptr(v)

#define is_any_ptr(v) (((v) != 0 && !((v) & 1) && (uintptr_t)(v) > 0x1000))

static inline int64_t rt_tag_v(int64_t v) {
  return (int64_t)(((uint64_t)v << 1) | 1);
}
static inline int64_t rt_untag_v(int64_t v) { return (v & 1) ? (v >> 1) : v; }
#if UINTPTR_MAX == 0xffffffff
/*
 * On 32-bit ARM, function pointers may carry the Thumb mode bit in bit0.
 * Preserve bit0 and clear only bit1 (Ny/native marker lane).
 */
static inline int64_t __mask_ptr(int64_t v) { return (int64_t)(v & ~2ULL); }
#define NY_NATIVE_TAG 2
#define NY_NATIVE_MARK (1ULL << 63)
/* 32-bit: require explicit high-bit marker to avoid collisions with raw fn ptrs. */
#define NY_NATIVE_IS(v)                                                        \
  ((((uint64_t)(v) & NY_NATIVE_MARK) != 0ULL) && (((v) & 3) == NY_NATIVE_TAG))
#define NY_NATIVE_ENCODE(p)                                                    \
  ((int64_t)(NY_NATIVE_MARK |                                                 \
             (((uint64_t)(uintptr_t)(p) << 2) | (uint64_t)NY_NATIVE_TAG)))
#define NY_NATIVE_DECODE(v)                                                    \
  ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_NATIVE_MARK) >> 2))
#else
static inline int64_t __mask_ptr(int64_t v) { return (int64_t)(v & ~7ULL); }
#define NY_NATIVE_TAG 6
#define NY_NATIVE_IS(v) (((v) & 7) == NY_NATIVE_TAG)
/*
 * Preserve all function-pointer bits when tagging native symbols:
 * some platforms expose callable addresses that are not 8-byte aligned.
 */
#define NY_NATIVE_ENCODE(p)                                                    \
  ((int64_t)(((uint64_t)(uintptr_t)(p) << 3) | (uint64_t)NY_NATIVE_TAG))
#define NY_NATIVE_DECODE(v) ((void *)(uintptr_t)(((uint64_t)(v)) >> 3))
#endif

#define TAG_FLOAT 221     // (110 << 1) | 1
#define TAG_STR 241       // (120 << 1) | 1
#define TAG_STR_CONST 243 // (121 << 1) | 1
#define TAG_OK 201
#define TAG_ERR 202

static inline int is_v_flt(int64_t v) {
  if (!is_ptr(v))
    return 0;
  uintptr_t tp = (uintptr_t)v - 8;
  if (!rt_addr_readable(tp, sizeof(int64_t)))
    return 0;
  int64_t tag = 0;
  memcpy(&tag, (const void *)tp, sizeof(tag));
  return tag == TAG_FLOAT;
}

static inline int is_ny_obj(int64_t v) {
  if (!is_heap_ptr(v))
    return 0;
  uintptr_t tp = (uintptr_t)v - 8;
  if (!rt_addr_readable(tp, sizeof(int64_t)))
    return 0;
  int64_t tag = 0;
  memcpy(&tag, (const void *)tp, sizeof(tag));
  if (is_int(tag)) {
    int64_t norm = tag >> 1;
    return (norm >= 100 && norm <= 125);
  }
  return (tag >= 100 && tag <= 125) || (tag >= 200 && tag <= 250);
}

static inline int is_v_str(int64_t v) {
  if (!is_ptr(v))
    return 0;

  // String tag is always at v - 8.
  uintptr_t tp = (uintptr_t)v - 8;
  if (!rt_addr_readable(tp, sizeof(int64_t)))
    return 0;
  int64_t tag = 0;
  memcpy(&tag, (const void *)tp, sizeof(tag));
  if (tag != TAG_STR && tag != TAG_STR_CONST)
    return 0;

  // Length is expected at v - 16 and must be non-negative.
  uintptr_t lp = (uintptr_t)v - 16;
  if (!rt_addr_readable(lp, sizeof(int64_t)))
    return 0;
  int64_t len_raw = 0;
  memcpy(&len_raw, (const void *)lp, sizeof(len_raw));
  if (!is_int(len_raw) || len_raw < 0)
    return 0;

  uint64_t len_u = (uint64_t)(len_raw >> 1);
  // Reject obviously bogus lengths to avoid classifying random pointers as
  // strings (especially for non-heap constant pointers).
  if (len_u > (1ULL << 40))
    return 0;
  if (len_u >= UINTPTR_MAX - (uintptr_t)v)
    return 0;
  if (!rt_addr_readable((uintptr_t)v, (size_t)len_u + 1))
    return 0;
  unsigned char nul = 1;
  memcpy(&nul, (const void *)((uintptr_t)v + (uintptr_t)len_u), sizeof(nul));
  if (nul != 0)
    return 0;
  return 1;
}

static inline int is_v_ok(int64_t v) {
  if (!is_ptr(v))
    return 0;
  uintptr_t tp = (uintptr_t)v - 8;
  if (!rt_addr_readable(tp, sizeof(int64_t)))
    return 0;
  int64_t tag = 0;
  memcpy(&tag, (const void *)tp, sizeof(tag));
  return tag == TAG_OK;
}

static inline int is_v_err(int64_t v) {
  if (!is_ptr(v))
    return 0;
  uintptr_t tp = (uintptr_t)v - 8;
  if (!rt_addr_readable(tp, sizeof(int64_t)))
    return 0;
  int64_t tag = 0;
  memcpy(&tag, (const void *)tp, sizeof(tag));
  return tag == TAG_ERR;
}

// Global declarations needed across runtime
void __cleanup_args(void);
int64_t __set_args(int64_t argc, int64_t argv, int64_t envp);
int64_t __malloc(int64_t n);
int64_t __free(int64_t ptr);
int64_t __runtime_cleanup(void);
int64_t __flt_unbox_val(int64_t v);
int64_t __flt_box_val(int64_t bits);
int64_t __str_concat(int64_t a, int64_t b);
int64_t __trace_last_file(void);
int64_t __trace_last_line(void);
int64_t __trace_last_col(void);
int64_t __trace_last_func(void);
// Helper for memory OOB checks
static inline size_t __get_heap_size(int64_t v) {
  if (!is_heap_ptr(v))
    return (size_t)-1;
  uintptr_t sp = (uintptr_t)v - 56;
  if (!rt_addr_readable(sp, sizeof(uint64_t)))
    return (size_t)-1;
  uint64_t sz = 0;
  memcpy(&sz, (const void *)sp, sizeof(sz));
  return (size_t)sz;
}

static inline int __check_oob(const char *op, int64_t addr, int64_t idx,
                              size_t access_sz) {
  (void)op;
  // Header-relative accesses (negative offsets) are used for Nytrix object
  // metadata (including constant strings that may not carry heap magics).
  // Allow only if the target header bytes are mapped and within header window.
  if ((intptr_t)idx < 0) {
    if ((intptr_t)idx < -64)
      return 0;
    uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
    return rt_addr_readable(p, access_sz) ? 1 : 0;
  }
  if (!is_heap_ptr(addr))
    return 1;
  size_t sz = __get_heap_size(addr);
  // Normal body access
  if ((size_t)idx + access_sz > sz) {
    return 0;
  }
  return 1;
}

int64_t __copy_mem(int64_t dst, int64_t src, int64_t n);

#endif
