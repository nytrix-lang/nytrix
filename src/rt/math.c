#include "rt/shared.h"

static uint64_t ___rng_state = 0x123456789ABCDEF0ULL;
static int ___rng_forced_prng = 0;

int64_t __srand(int64_t s) {
  ___rng_state = (uint64_t)(s >> 1);
  ___rng_forced_prng = 1;
  return s;
}

int64_t __rand64(void) {
  uint64_t val;
  int ok = 0;
#if defined(__x86_64__)
  if (!___rng_forced_prng) {
    __asm__ volatile("rdrand %0; setc %b1" : "=r"(val), "=q"(ok));
  }
#endif
  if (!ok) {
    ___rng_state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = ___rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    val = z ^ (z >> 31);
  }
  uint64_t res = ((uint64_t)(val & 0x3FFFFFFFFFFFFFFFULL) << 1) | 1ULL;
  return (int64_t)res;
}

// Float Primitives
int64_t __flt_box_val(int64_t bits) {
  int64_t res = __malloc(17);
  *(int64_t *)((char *)(uintptr_t)res - 8) = TAG_FLOAT;
  memcpy((void *)(uintptr_t)res, &bits, 8);
  return res;
}

int64_t __flt_unbox_val(int64_t v) {
  if (v & 1) { // is_int
    double d = (double)(v >> 1);
    int64_t res;
    memcpy(&res, &d, 8);
    return res;
  }
  if (is_ptr(v)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    if (tag == TAG_FLOAT) {
      int64_t bits;
      memcpy(&bits, (void *)(uintptr_t)v, 8);
      return bits;
    }
  }
  return 0;
}

int64_t __flt_from_int(int64_t v) {
  if (is_int(v)) {
    double d = (double)(v >> 1);
    int64_t res;
    memcpy(&res, &d, 8);
    return res;
  }
  return 0;
}

int64_t __flt_to_int(int64_t v) {
  int64_t b = __flt_unbox_val(v);
  double d;
  memcpy(&d, &b, 8);
  return (int64_t)(((uint64_t)d << 1) | 1);
}

int64_t __flt_trunc(int64_t v) { return __flt_to_int(v); }

// Float Ops
#define FLT_OP(name, op)                                                       \
  int64_t __flt_##name(int64_t a, int64_t b) {                                 \
    double da, db;                                                             \
    int64_t ba = __flt_unbox_val(a);                                           \
    int64_t bb = __flt_unbox_val(b);                                           \
    memcpy(&da, &ba, 8);                                                       \
    memcpy(&db, &bb, 8);                                                       \
    double r = da op db;                                                       \
    int64_t rr;                                                                \
    memcpy(&rr, &r, 8);                                                        \
    return __flt_box_val(rr);                                                  \
  }

FLT_OP(add, +)
FLT_OP(sub, -)
FLT_OP(mul, *)
FLT_OP(div, /)

#define FLT_CMP(name, op)                                                      \
  int64_t __flt_##name(int64_t a, int64_t b) {                                 \
    double da, db;                                                             \
    int64_t ba = __flt_unbox_val(a);                                           \
    int64_t bb = __flt_unbox_val(b);                                           \
    memcpy(&da, &ba, 8);                                                       \
    memcpy(&db, &bb, 8);                                                       \
    return (da op db) ? 2 : 4;                                                 \
  }

FLT_CMP(lt, <)
FLT_CMP(gt, >)
FLT_CMP(le, <=)
FLT_CMP(ge, >=)
FLT_CMP(eq, ==)

// Mixed Math
int64_t __add(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return a + b - 1;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_add(a, b);
  if (is_any_ptr(a) && is_int(b))
    return a + (b >> 1);
  if (is_int(a) && is_any_ptr(b))
    return b + (a >> 1);
  if (is_v_str(a) && is_v_str(b))
    return __str_concat(a, b);
  return 1;
}

int64_t __sub(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return a - b + 1;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_sub(a, b);
  if (is_any_ptr(a) && is_int(b))
    return a - (b >> 1);
  if (is_any_ptr(a) && is_any_ptr(b))
    return (int64_t)(((uint64_t)(a - b) << 1) | 1);
  return 1;
}

int64_t __mul(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (int64_t)((((uint64_t)(a >> 1) * (uint64_t)(b >> 1)) << 1) | 1);
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_mul(a, b);
  return 0;
}

int64_t __div(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b)) {
    int64_t vb = b >> 1;
    if (vb == 0)
      return 0;
    return (int64_t)((((uint64_t)(a >> 1) / (uint64_t)vb) << 1) | 1);
  }
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_div(a, b);
  return 0;
}

int64_t __mod(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b)) {
    int64_t vb = b >> 1;
    if (vb == 0)
      return 1;
    return (int64_t)((((uint64_t)(a >> 1) % (uint64_t)vb) << 1) | 1);
  }
  return b ? a % b : 1;
}

// Logic
int64_t __eq(int64_t a, int64_t b) {
  if (a == b)
    return 2; // True
  if ((a == 0 && b == 1) || (a == 1 && b == 0))
    return 2; // NONE == 0
  if ((a & 1) != (b & 1))
    return 4;
  if (is_ptr(a) && is_ptr(b)) {
    if (a <= 4 || b <= 4)
      return 4;
    if (is_v_flt(a) || is_v_flt(b))
      return __flt_eq(a, b);
    int64_t ta = *(int64_t *)((char *)(uintptr_t)a - 8);
    int64_t tb = *(int64_t *)((char *)(uintptr_t)b - 8);
    int a_is_str = (ta == TAG_STR || ta == TAG_STR_CONST);
    int b_is_str = (tb == TAG_STR || tb == TAG_STR_CONST);
    if (a_is_str && b_is_str) {
      int res =
          (strcmp((const char *)(uintptr_t)a, (const char *)(uintptr_t)b) == 0);
      return res ? 2 : 4;
    }
  }
  return 4;
}

int64_t __lt(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) < (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_lt(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a < b ? 2 : 4;
  return 4;
}
int64_t __le(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) <= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_le(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a <= b ? 2 : 4;
  return 4;
}
int64_t __gt(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) > (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_gt(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a > b ? 2 : 4;
  return 4;
}
int64_t __ge(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) >= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_ge(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a >= b ? 2 : 4;
  return 4;
}

// Bitwise
int64_t __and(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) &
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __or(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) |
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __xor(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) ^
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __shl(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a)
                     << (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __shr(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) >>
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __not(int64_t a) {
  return (int64_t)(((~(uint64_t)(a & 1 ? a >> 1 : a)) << 1) | 1);
}
