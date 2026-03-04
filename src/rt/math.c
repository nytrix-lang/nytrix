#include "base/common.h"
#include "rt/runtime.h"
#include "rt/shared.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline double get_flt(int64_t v) {
  if (v & 1)
    return (double)(v >> 1);
  if (v == 0)
    return 0.0;

  if (is_v_flt(v)) {
    double d;
    memcpy(&d, (const void *)(uintptr_t)v, 8);
    return d;
  }

  if (!is_ptr(v)) {
    double d;
    memcpy(&d, &v, 8);
    if (isfinite(d))
      return d;
  }

  return 0.0;
}

static uint64_t rt_rng_state = 0x123456789ABCDEF0ULL;
static int rt_rng_forced_prng = 0;

int64_t rt_srand(int64_t i) {
  rt_rng_state = (uint64_t)(i >> 1);
  rt_rng_forced_prng = 1;
  return i;
}

int64_t rt_rand64(void) {
  uint64_t val = 0;
  int ok = 0;
#if defined(rt_x86_64__)
  if (!rt_rng_forced_prng) {
    rt_asm__ volatile("rdrand %0; setc %b1" : "=r"(val), "=q"(ok));
  }
#endif
  if (!ok) {
    rt_rng_state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = rt_rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    val = z ^ (z >> 31);
  }
  uint64_t res = ((uint64_t)(val & 0x3FFFFFFFFFFFFFFFULL) << 1) | 1ULL;
  return (int64_t)res;
}

static __thread void *g_flt_cache = NULL;

static void *rt_flt_alloc_slot(void) {
  if (g_flt_cache) {
    void *p = g_flt_cache;
    g_flt_cache = *(void **)p;
    return p;
  }
  size_t chunk_size = 4096;
  char *chunk = (char *)rt_malloc((int64_t)((chunk_size << 1) | 1));
  if (!chunk)
    return NULL;
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

void rt_flt_free(int64_t v) {
  if (!v)
    return;
  void *slot = (void *)((char *)(uintptr_t)v - 8);
  *(void **)slot = g_flt_cache;
  g_flt_cache = slot;
}

int64_t rt_flt_box_val(int64_t bits) {
  void *slot = rt_flt_alloc_slot();
  if (!slot)
    return 0;
  *(int64_t *)slot = TAG_FLOAT;
  memcpy((char *)slot + 8, &bits, 8);
  return (int64_t)(uintptr_t)((char *)slot + 8);
}

int64_t rt_flt_box_val32(int64_t bits32) {
  uint32_t raw = (uint32_t)rt_untag_v(bits32);
  float f;
  memcpy(&f, &raw, 4);
  double d = (double)f;
  int64_t b;
  memcpy(&b, &d, 8);
  return rt_flt_box_val(b);
}

int64_t rt_flt_unbox_val32(int64_t v) {
  double d = get_flt(v);
  float f = (float)d;
  uint32_t b;
  memcpy(&b, &f, 4);
  return rt_tag_v((int64_t)b);
}

int64_t rt_flt_from_int(int64_t v) {
  if (v & 1) {
    double d = (double)(v >> 1);
    int64_t b;
    memcpy(&b, &d, 8);
    return b;
  }
  return 0;
}

int64_t rt_flt_to_int(int64_t v) {
  double d = get_flt(v);
  return rt_tag_v((int64_t)d);
}

int64_t rt_flt_trunc(int64_t v) { return rt_flt_to_int(v); }

#define FLT_OP(name, op)                                                       \
  int64_t rt_flt_##name(int64_t a, int64_t b) {                                \
    double da = get_flt(a);                                                    \
    double db = get_flt(b);                                                    \
    double r = da op db;                                                       \
    int64_t rr;                                                                \
    memcpy(&rr, &r, 8);                                                        \
    return rt_flt_box_val(rr);                                                 \
  }

FLT_OP(add, +)
FLT_OP(sub, -)
FLT_OP(mul, *)
FLT_OP(div, /)

#define FLT_CMP(name, op)                                                      \
  int64_t rt_flt_##name(int64_t a, int64_t b) {                                \
    double da = get_flt(a);                                                    \
    double db = get_flt(b);                                                    \
    return (da op db) ? 2 : 4;                                                 \
  }

FLT_CMP(lt, <)
FLT_CMP(gt, >)
FLT_CMP(le, <=)
FLT_CMP(ge, >=)
FLT_CMP(eq, ==)

int64_t rt_add(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return (int64_t)((uint64_t)a + (uint64_t)b - 1);
  // Handle ptr+int BEFORE is_v_flt to avoid reading guard pages at ptr-8
  // for non-heap pointers (e.g., Vulkan-mapped memory). Use is_v_flt_mapped
  // (uncached mincore) so stale page-cache entries can't cause SIGSEGV.
  if (is_any_ptr(a) && (b & 1)) {
    if (is_v_flt_mapped(a))
      return rt_flt_add(a, b);
    return a + (b >> 1);
  }
  if ((a & 1) && is_any_ptr(b)) {
    if (is_v_flt_mapped(b))
      return rt_flt_add(a, b);
    return b + (a >> 1);
  }
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_add(a, b);
  if (is_v_str(a) && is_v_str(b))
    return rt_str_concat(a, b);
  return 1;
}

int64_t rt_sub(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return (int64_t)((uint64_t)a - (uint64_t)b + 1);
  if (is_any_ptr(a) && (b & 1)) {
    if (is_v_flt_mapped(a))
      return rt_flt_sub(a, b);
    return a - (b >> 1);
  }
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_sub(a, b);
  return 1;
}

int64_t rt_mul(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return rt_tag_v((a >> 1) * (b >> 1));
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_mul(a, b);
  return 1;
}

int64_t rt_div(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0)
      return 1;
    return rt_tag_v((a >> 1) / bv);
  }
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_div(a, b);
  return 1;
}

int64_t rt_mod(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0)
      return 1;
    return rt_tag_v((a >> 1) % bv);
  }
  return 1;
}

int64_t rt_eq(int64_t a, int64_t b) {
  if (a == b)
    return 2;
  if ((a == 0 && b == 1) || (a == 1 && b == 0))
    return 2;
  if ((a & 1) != (b & 1))
    return 4;
  if (is_ptr(a) && is_ptr(b)) {
    if (a <= 4 || b <= 4)
      return 4;
    if (is_v_flt(a) || is_v_flt(b))
      return rt_flt_eq(a, b);
    if (is_v_str(a) && is_v_str(b)) {
      uintptr_t la_p = (uintptr_t)a - 16;
      uintptr_t lb_p = (uintptr_t)b - 16;
      int64_t la_tagged = *(int64_t *)la_p;
      int64_t lb_tagged = *(int64_t *)lb_p;
      if (la_tagged != lb_tagged)
        return 4;
      size_t la = (size_t)(la_tagged >> 1);
      if (la == 0)
        return 2;
      return memcmp((const void *)(uintptr_t)a, (const void *)(uintptr_t)b,
                    la) == 0
                 ? 2
                 : 4;
    }
  }
  return 4;
}

int64_t rt_lt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return (a >> 1) < (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_lt(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a < b ? 2 : 4;
  return 4;
}
int64_t rt_le(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return (a >> 1) <= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_le(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a <= b ? 2 : 4;
  return 4;
}
int64_t rt_gt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return (a >> 1) > (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_gt(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a > b ? 2 : 4;
  return 4;
}
int64_t rt_ge(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1))
    return (a >> 1) >= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return rt_flt_ge(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a >= b ? 2 : 4;
  return 4;
}

int64_t rt_and(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) &
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t rt_or(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) |
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t rt_xor(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) ^
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t rt_shl(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a)
                     << (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t rt_shr(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) >>
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t rt_not(int64_t a) {
  return (int64_t)(((~(uint64_t)(a & 1 ? a >> 1 : a)) << 1) | 1);
}

int64_t rt_flt_unbox_val(int64_t v) { return _rt_flt_unbox_val(v); }

extern int64_t rt_list_new(int64_t n);
extern int64_t rt_append(int64_t lst, int64_t val);

static inline int64_t raw_i(int64_t v) { return (v & 1) ? (v >> 1) : v; }

static inline int64_t list_len(int64_t lst) {
  if (!is_ptr(lst))
    return 0;
  int64_t lv = *(int64_t *)((char *)(uintptr_t)lst + 0);
  return raw_i(lv);
}

static inline int64_t list_get(int64_t lst, int64_t i) {
  int64_t v = *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8);
  return raw_i(v);
}

int64_t rt_big_add_abs(int64_t a, int64_t b) {
  int64_t na = list_len(a);
  int64_t nb = list_len(b);
  int64_t nmax = na > nb ? na : nb;
  int64_t out = rt_list_new((nmax + 1) << 1 | 1);
  // rt_list_new sets length=cap. Reset length to 0 to use rt_append
  *(int64_t *)((char *)(uintptr_t)out + 0) = 1;

  int64_t carry = 0;
  for (int64_t i = 0; i < nmax; i++) {
    int64_t va = (i < na) ? list_get(a, i) : 0;
    int64_t vb = (i < nb) ? list_get(b, i) : 0;
    int64_t sum = va + vb + carry;
    if (sum >= 1000000000) {
      sum -= 1000000000;
      carry = 1;
    } else {
      carry = 0;
    }
    rt_append(out, (sum << 1) | 1);
  }
  if (carry > 0) {
    rt_append(out, (1 << 1) | 1);
  }
  return out;
}

int64_t rt_big_sub_abs(int64_t a, int64_t b) {
  int64_t na = list_len(a);
  int64_t nb = list_len(b);
  int64_t out = rt_list_new(na << 1 | 1);
  *(int64_t *)((char *)(uintptr_t)out + 0) = 1;

  int64_t borrow = 0;
  for (int64_t i = 0; i < na; i++) {
    int64_t va = list_get(a, i) - borrow;
    int64_t vb = (i < nb) ? list_get(b, i) : 0;
    if (va < vb) {
      va += 1000000000;
      borrow = 1;
    } else {
      borrow = 0;
    }
    rt_append(out, ((va - vb) << 1) | 1);
  }
  return out;
}

int64_t rt_big_mul_abs(int64_t a, int64_t b) {
  int64_t na = list_len(a);
  int64_t nb = list_len(b);
  if (na == 0 || nb == 0)
    return rt_list_new(1);

  int64_t nout = na + nb;
  int64_t out = rt_list_new(nout << 1 | 1);
  int64_t *raw_out = (int64_t *)((char *)(uintptr_t)out + 16);
  // Initialize with tagged 0
  for (int64_t k = 0; k < nout; k++)
    raw_out[k] = 1;

  for (int64_t i = 0; i < na; i++) {
    int64_t carry = 0;
    intptr_t va = (intptr_t)list_get(a, i);
    for (int64_t j = 0; j < nb; j++) {
      int64_t idx = i + j;
      intptr_t cur = (intptr_t)raw_i(raw_out[idx]);
      intptr_t prod = cur + va * (intptr_t)list_get(b, j) + (intptr_t)carry;
      carry = (int64_t)(prod / 1000000000);
      raw_out[idx] = ((prod % 1000000000) << 1) | 1;
    }
    if (carry > 0) {
      int64_t idx2 = i + nb;
      raw_out[idx2] = ((raw_i(raw_out[idx2]) + carry) << 1) | 1;
    }
  }
  return out;
}
