#include "rt/shared.h"
#include <math.h>
#include <stdio.h>

static inline double get_flt(int64_t v) {
  if (v & 1) return (double)(v >> 1);
  if (v == 0) return 0.0;
  
  if (is_v_flt(v)) {
    double d;
    memcpy(&d, (const void *)(uintptr_t)v, 8);
    return d;
  }
  
  if (!is_ptr(v)) {
    double d;
    memcpy(&d, &v, 8);
    if (isfinite(d)) return d;
  }
  
  return 0.0;
}

static uint64_t __rng_state = 0x123456789ABCDEF0ULL;
static int __rng_forced_prng = 0;

int64_t __srand(int64_t i) {
  __rng_state = (uint64_t)(i >> 1);
  __rng_forced_prng = 1;
  return i;
}

int64_t __rand64(void) {
  uint64_t val = 0;
  int ok = 0;
#if defined(__x86_64__)
  if (!__rng_forced_prng) {
    __asm__ volatile("rdrand %0; setc %b1" : "=r"(val), "=q"(ok));
  }
#endif
  if (!ok) {
    __rng_state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = __rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    val = z ^ (z >> 31);
  }
  uint64_t res = ((uint64_t)(val & 0x3FFFFFFFFFFFFFFFULL) << 1) | 1ULL;
  return (int64_t)res;
}

static __thread void *g_flt_cache = NULL;

static void *__flt_alloc_slot(void) {
  if (g_flt_cache) {
    void *p = g_flt_cache;
    g_flt_cache = *(void **)p;
    return p;
  }
  size_t chunk_size = 4096;
  char *chunk = (char *)__malloc((int64_t)((chunk_size << 1) | 1));
  if (!chunk) return NULL;
  size_t slot_count = chunk_size / 16;
  for (size_t i = 0; i < slot_count - 1; i++) {
    void *curr = chunk + (i * 16);
    void *next = chunk + ((i + 1) * 16);
    *(void **)curr = next;
  }
  *(void **)(chunk + ((slot_count - 1) * 16)) = NULL;
  g_flt_cache = (void *)chunk;
  void *p = g_flt_cache;
  g_flt_cache = *(void **)p;
  return p;
}

void __flt_free(int64_t v) {
  if (!v) return;
  void *slot = (void *)((char *)(uintptr_t)v - 8);
  *(void **)slot = g_flt_cache;
  g_flt_cache = slot;
}

int64_t __flt_box_val(int64_t bits) {
  void *slot = __flt_alloc_slot();
  if (!slot) return 0;
  *(int64_t *)slot = TAG_FLOAT;
  memcpy((char *)slot + 8, &bits, 8);
  return (int64_t)(uintptr_t)((char *)slot + 8);
}

int64_t __flt_box_val32(int64_t bits32) {
  uint32_t raw = (uint32_t)rt_untag_v(bits32);
  float f; memcpy(&f, &raw, 4);
  double d = (double)f;
  int64_t b; memcpy(&b, &d, 8);
  return __flt_box_val(b);
}

int64_t __flt_unbox_val32(int64_t v) {
  double d = get_flt(v);
  float f = (float)d;
  uint32_t b; memcpy(&b, &f, 4);
  return rt_tag_v((int64_t)b);
}

int64_t __flt_from_int(int64_t v) {
  if (v & 1) {
    double d = (double)(v >> 1);
    int64_t b; memcpy(&b, &d, 8);
    return b;
  }
  return 0;
}

int64_t __flt_to_int(int64_t v) {
  double d = get_flt(v);
  return rt_tag_v((int64_t)d);
}

int64_t __flt_trunc(int64_t v) { return __flt_to_int(v); }

#define FLT_OP(name, op) \
  int64_t __flt_##name(int64_t a, int64_t b) { \
    double da = get_flt(a); \
    double db = get_flt(b); \
    double r = da op db; \
    int64_t rr; memcpy(&rr, &r, 8); \
    return __flt_box_val(rr); \
  }

FLT_OP(add, +)
FLT_OP(sub, -)
FLT_OP(mul, *)
FLT_OP(div, /)

#define FLT_CMP(name, op) \
  int64_t __flt_##name(int64_t a, int64_t b) { \
    double da = get_flt(a); \
    double db = get_flt(b); \
    return (da op db) ? 2 : 4; \
  }

FLT_CMP(lt, <)
FLT_CMP(gt, >)
FLT_CMP(le, <=)
FLT_CMP(ge, >=)
FLT_CMP(eq, ==)

int64_t __add(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (int64_t)((uint64_t)a + (uint64_t)b - 1);
  if (is_v_flt(a) || is_v_flt(b)) return __flt_add(a, b);
  if (is_v_str(a) && is_v_str(b)) return __str_concat(a, b);
  if (is_any_ptr(a) && (b & 1)) return a + (b >> 1);
  if ((a & 1) && is_any_ptr(b)) return b + (a >> 1);
  return 1;
}

int64_t __sub(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (int64_t)((uint64_t)a - (uint64_t)b + 1);
  if (is_v_flt(a) || is_v_flt(b)) return __flt_sub(a, b);
  if (is_any_ptr(a) && (b & 1)) return a - (b >> 1);
  return 1;
}

int64_t __mul(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return rt_tag_v((a >> 1) * (b >> 1));
  if (is_v_flt(a) || is_v_flt(b)) return __flt_mul(a, b);
  return 1;
}

int64_t __div(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0) return 1;
    return rt_tag_v((a >> 1) / bv);
  }
  if (is_v_flt(a) || is_v_flt(b)) return __flt_div(a, b);
  return 1;
}

int64_t __mod(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0) return 1;
    return rt_tag_v((a >> 1) % bv);
  }
  return 1;
}

int64_t __eq(int64_t a, int64_t b) {
  if (a == b) return 2;
  if ((a == 0 && b == 1) || (a == 1 && b == 0)) return 2;
  if ((a & 1) != (b & 1)) return 4;
  if (is_ptr(a) && is_ptr(b)) {
    if (a <= 4 || b <= 4) return 4;
    if (is_v_flt(a) || is_v_flt(b)) return __flt_eq(a, b);
    if (is_v_str(a) && is_v_str(b)) {
      uintptr_t la_p = (uintptr_t)a - 16;
      uintptr_t lb_p = (uintptr_t)b - 16;
      int64_t la_tagged = *(int64_t*)la_p;
      int64_t lb_tagged = *(int64_t*)lb_p;
      if (la_tagged != lb_tagged) return 4;
      size_t la = (size_t)(la_tagged >> 1);
      if (la == 0) return 2;
      return memcmp((const void *)(uintptr_t)a, (const void *)(uintptr_t)b, la) == 0 ? 2 : 4;
    }
  }
  return 4;
}

int64_t __lt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) < (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_lt(a, b);
  if (is_ptr(a) && is_ptr(b)) return a < b ? 2 : 4;
  return 4;
}
int64_t __le(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) <= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_le(a, b);
  if (is_ptr(a) && is_ptr(b)) return a <= b ? 2 : 4;
  return 4;
}
int64_t __gt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) > (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_gt(a, b);
  if (is_ptr(a) && is_ptr(b)) return a > b ? 2 : 4;
  return 4;
}
int64_t __ge(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) return (a >> 1) >= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b)) return __flt_ge(a, b);
  if (is_ptr(a) && is_ptr(b)) return a >= b ? 2 : 4;
  return 4;
}

int64_t __and(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) & (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __or(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) | (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __xor(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) ^ (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __shl(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) << (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __shr(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) >> (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t __not(int64_t a) {
  return (int64_t)(((~(uint64_t)(a & 1 ? a >> 1 : a)) << 1) | 1);
}

int64_t __flt_unbox_val(int64_t v) {
  return __rt_flt_unbox_val(v);
}
