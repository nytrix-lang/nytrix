#ifndef RT_COMMON_H
#define RT_COMMON_H

#include "base/common.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NY_MAGIC1 0x545249584E5954ULL
#define NY_MAGIC2 0x4E59545249584EULL
#define NY_MAGIC3 0xDEADBEEFCAFEBABEULL

// Core Tags and Predicates
#define is_int(v) ((v) & 1)
#define is_ptr(v) ((v) != 0 && ((v) & 7) == 0 && (uintptr_t)(v) > 0x1000)

static inline bool is_valid_heap_ptr(int64_t v) {
  if (!is_ptr(v) || (v & 63) != 0)
    return false;
  // This check is the important part
  uintptr_t raw_p_start = (uintptr_t)v - 64;
  uintptr_t raw_p_magic2 = (uintptr_t)v - 48;
  bool match = (*(uint64_t *)raw_p_start == NY_MAGIC1) &&
               (*(uint64_t *)raw_p_magic2 == NY_MAGIC2);
  return match;
}

#define is_heap_ptr(v) is_valid_heap_ptr(v)

#define is_any_ptr(v) (((v) != 0 && !((v) & 1) && (uintptr_t)(v) > 0x1000))

static inline int64_t __tag(int64_t v) {
  return (int64_t)(((uint64_t)v << 1) | 1);
}
static inline int64_t __untag(int64_t v) { return (v & 1) ? (v >> 1) : v; }
static inline int64_t __mask_ptr(int64_t v) { return (int64_t)(v & ~7ULL); }

#define TAG_FLOAT 221     // (110 << 1) | 1
#define TAG_STR 241       // (120 << 1) | 1
#define TAG_STR_CONST 243 // (121 << 1) | 1

static inline int is_v_flt(int64_t v) {
  if (!is_heap_ptr(v))
    return 0;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  return (tag == TAG_FLOAT || tag == 110);
}

static inline int is_ny_obj(int64_t v) {
  if (!is_heap_ptr(v))
    return 0;
  int64_t tag = *(int64_t *)((uintptr_t)v - 8);
  return (tag >= 100 && tag <= 119) || (tag >= 200 && tag <= 250);
}

static inline int is_v_str(int64_t v) {
  if (!is_heap_ptr(v))
    return 0;

  // The tag is always at v - 8
  int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);

  return (tag == TAG_STR || tag == TAG_STR_CONST || tag == 120 || tag == 121);
}

// Global declarations needed across runtime
void __cleanup_args(void);
int64_t __set_args(int64_t argc, int64_t argv, int64_t envp);
int64_t __malloc(int64_t n);
int64_t __free(int64_t ptr);
int64_t __flt_unbox_val(int64_t v);
int64_t __flt_box_val(int64_t bits);
int64_t __str_concat(int64_t a, int64_t b);
// Helper for memory OOB checks
static inline size_t __get_heap_size(int64_t v) {
  if (!is_heap_ptr(v))
    return (size_t)-1;
  return *(uint64_t *)((uintptr_t)v - 56);
}

static inline int __check_oob(const char *op, int64_t addr, int64_t idx,
                              size_t access_sz) {
  (void)op;
  if (!is_heap_ptr(addr))
    return 1;
  size_t sz = __get_heap_size(addr);
  // Handle negative indices (Header access) separately
  if ((intptr_t)idx < 0) {
    // Header is 64 bytes
    if ((intptr_t)idx < -64)
      return 0;
    return 1;
  }
  // Normal body access
  if ((size_t)idx + access_sz > sz) {
    return 0;
  }
  return 1;
}

void __copy_mem(void *dst, const void *src, size_t n);

#endif
