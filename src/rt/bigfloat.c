#include "base/common.h"
#include "rt/runtime.h"
#include "rt/shared.h"

#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t sign;
  int64_t exp2;
  uint32_t precision;
  uint32_t words;
  uint64_t limb[];
} ny_bigfloat_t;

typedef struct {
  uint64_t *d;
  size_t n;
  size_t cap;
} bf_mag_t;

static uint32_t bf_precision_clamp(int64_t p) {
  if (p < 32)
    p = 32;
  if (p > 1048576)
    p = 1048576;
  return (uint32_t)p;
}

static uint32_t bf_default_precision(void) {
  static _Atomic uint32_t cached = 0;
  uint32_t p = atomic_load_explicit(&cached, memory_order_relaxed);
  if (p)
    return p;
  const char *s = getenv("NYTRIX_BIGFLOAT_PRECISION");
  long v = 256;
  if (s && *s) {
    char *end = NULL;
    long x = strtol(s, &end, 10);
    if (end != s && !*end)
      v = x;
  }
  p = bf_precision_clamp(v);
  atomic_store_explicit(&cached, p, memory_order_relaxed);
  return p;
}

static ny_bigfloat_t *bf_ptr(int64_t v) {
  if (!is_ptr(v) || !is_heap_ptr(v))
    return NULL;
  if (*(int64_t *)((char *)(uintptr_t)v - 8) != TAG_BIGFLOAT)
    return NULL;
  return (ny_bigfloat_t *)(uintptr_t)v;
}

static void mag_free(bf_mag_t *a) {
  if (!a)
    return;
  free(a->d);
  a->d = NULL;
  a->n = a->cap = 0;
}

static bool mag_reserve(bf_mag_t *a, size_t cap) {
  if (cap <= a->cap)
    return true;
  size_t nc = a->cap ? a->cap : 4;
  while (nc < cap) {
    if (nc > SIZE_MAX / 2)
      return false;
    nc *= 2;
  }
  uint64_t *p = realloc(a->d, nc * sizeof(*p));
  if (!p)
    return false;
  memset(p + a->cap, 0, (nc - a->cap) * sizeof(*p));
  a->d = p;
  a->cap = nc;
  return true;
}

static void mag_trim(bf_mag_t *a) {
  while (a->n && a->d[a->n - 1] == 0)
    a->n--;
}

static unsigned bf_clz64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return x ? (unsigned)__builtin_clzll(x) : 64u;
#else
  unsigned n = 0;
  if (!x)
    return 64;
  while (!(x & (UINT64_C(1) << 63))) {
    x <<= 1;
    n++;
  }
  return n;
#endif
}

static size_t mag_bitlen(const bf_mag_t *a) {
  if (!a || !a->n)
    return 0;
  return (a->n - 1) * 64 + 64 - bf_clz64(a->d[a->n - 1]);
}

static bool mag_copy(bf_mag_t *dst, const uint64_t *src, size_t n) {
  if (!mag_reserve(dst, n + 2))
    return false;
  if (n)
    memcpy(dst->d, src, n * sizeof(*src));
  dst->n = n;
  mag_trim(dst);
  return true;
}

static int mag_cmp(const bf_mag_t *a, const bf_mag_t *b) {
  if (a->n != b->n)
    return a->n < b->n ? -1 : 1;
  for (size_t i = a->n; i-- > 0;) {
    if (a->d[i] != b->d[i])
      return a->d[i] < b->d[i] ? -1 : 1;
  }
  return 0;
}

static bool mag_get_bit(const bf_mag_t *a, size_t bit) {
  size_t w = bit >> 6;
  return w < a->n && ((a->d[w] >> (bit & 63)) & 1u);
}

static bool mag_any_low(const bf_mag_t *a, size_t bits) {
  if (!bits)
    return false;
  size_t whole = bits >> 6;
  size_t rem = bits & 63;
  size_t lim = whole < a->n ? whole : a->n;
  for (size_t i = 0; i < lim; ++i)
    if (a->d[i])
      return true;
  if (rem && whole < a->n) {
    uint64_t mask = rem == 64 ? UINT64_MAX : ((UINT64_C(1) << rem) - 1);
    if (a->d[whole] & mask)
      return true;
  }
  return false;
}

static bool mag_shl(bf_mag_t *a, size_t bits) {
  if (!a->n || !bits)
    return true;
  size_t ws = bits >> 6;
  unsigned bs = (unsigned)(bits & 63);
  size_t old = a->n;
  size_t need = old + ws + (bs ? 1 : 0);
  if (!mag_reserve(a, need + 1))
    return false;
  if (ws) {
    memmove(a->d + ws, a->d, old * sizeof(*a->d));
    memset(a->d, 0, ws * sizeof(*a->d));
  }
  a->n = old + ws;
  if (bs) {
    uint64_t carry = 0;
    for (size_t i = ws; i < a->n; ++i) {
      uint64_t x = a->d[i];
      a->d[i] = (x << bs) | carry;
      carry = x >> (64 - bs);
    }
    if (carry)
      a->d[a->n++] = carry;
  }
  return true;
}

static bool mag_shr_jam(bf_mag_t *a, size_t bits) {
  if (!a->n || !bits)
    return false;
  bool jam = mag_any_low(a, bits);
  size_t ws = bits >> 6;
  unsigned bs = (unsigned)(bits & 63);
  if (ws >= a->n) {
    a->n = jam ? 1 : 0;
    if (jam)
      a->d[0] = 1;
    return jam;
  }
  if (ws) {
    memmove(a->d, a->d + ws, (a->n - ws) * sizeof(*a->d));
    a->n -= ws;
  }
  if (bs) {
    uint64_t carry = 0;
    for (size_t i = a->n; i-- > 0;) {
      uint64_t x = a->d[i];
      a->d[i] = (x >> bs) | carry;
      carry = x << (64 - bs);
    }
  }
  mag_trim(a);
  if (jam) {
    if (!a->n) {
      a->n = 1;
      a->d[0] = 1;
    } else {
      a->d[0] |= 1;
    }
  }
  return jam;
}

static void mag_shr_plain(bf_mag_t *a, size_t bits) {
  if (!a->n || !bits)
    return;
  size_t ws = bits >> 6;
  unsigned bs = (unsigned)(bits & 63);
  if (ws >= a->n) {
    a->n = 0;
    return;
  }
  if (ws) {
    memmove(a->d, a->d + ws, (a->n - ws) * sizeof(*a->d));
    a->n -= ws;
  }
  if (bs) {
    uint64_t carry = 0;
    for (size_t i = a->n; i-- > 0;) {
      uint64_t x = a->d[i];
      a->d[i] = (x >> bs) | carry;
      carry = x << (64 - bs);
    }
  }
  mag_trim(a);
}

static bool mag_add_one(bf_mag_t *a) {
  if (!a->n) {
    if (!mag_reserve(a, 1))
      return false;
    a->d[0] = 1;
    a->n = 1;
    return true;
  }
  for (size_t i = 0; i < a->n; ++i) {
    if (++a->d[i])
      return true;
  }
  if (!mag_reserve(a, a->n + 1))
    return false;
  a->d[a->n++] = 1;
  return true;
}

static bool mag_add(bf_mag_t *out, const bf_mag_t *a, const bf_mag_t *b) {
  size_t n = a->n > b->n ? a->n : b->n;
  if (!mag_reserve(out, n + 1))
    return false;
  uint64_t carry = 0;
  for (size_t i = 0; i < n; ++i) {
    uint64_t av = i < a->n ? a->d[i] : 0;
    uint64_t bv = i < b->n ? b->d[i] : 0;
    uint64_t s = av + bv;
    uint64_t c1 = s < av;
    uint64_t t = s + carry;
    uint64_t c2 = t < s;
    out->d[i] = t;
    carry = c1 | c2;
  }
  out->n = n;
  if (carry)
    out->d[out->n++] = carry;
  return true;
}

static bool mag_sub(bf_mag_t *out, const bf_mag_t *a, const bf_mag_t *b) {
  if (mag_cmp(a, b) < 0)
    return false;
  if (!mag_reserve(out, a->n + 1))
    return false;
  uint64_t borrow = 0;
  for (size_t i = 0; i < a->n; ++i) {
    uint64_t av = a->d[i];
    uint64_t bv = i < b->n ? b->d[i] : 0;
    uint64_t t = av - bv;
    uint64_t b1 = av < bv;
    uint64_t r = t - borrow;
    uint64_t b2 = t < borrow;
    out->d[i] = r;
    borrow = b1 | b2;
  }
  out->n = a->n;
  mag_trim(out);
  return borrow == 0;
}

static bool mag_mul(bf_mag_t *out, const bf_mag_t *a, const bf_mag_t *b) {
  if (!a->n || !b->n) {
    out->n = 0;
    return true;
  }
  size_t n = a->n + b->n;
  if (!mag_reserve(out, n + 1))
    return false;
  memset(out->d, 0, (n + 1) * sizeof(*out->d));
#if defined(__SIZEOF_INT128__)
  for (size_t i = 0; i < a->n; ++i) {
    __uint128_t carry = 0;
    for (size_t j = 0; j < b->n; ++j) {
      __uint128_t z = (__uint128_t)a->d[i] * b->d[j] +
                      out->d[i + j] + carry;
      out->d[i + j] = (uint64_t)z;
      carry = z >> 64;
    }
    size_t k = i + b->n;
    while (carry) {
      __uint128_t z = (__uint128_t)out->d[k] + carry;
      out->d[k++] = (uint64_t)z;
      carry = z >> 64;
    }
  }
#else
  for (size_t i = 0; i < a->n; ++i) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b->n; ++j) {
      uint64_t a0 = (uint32_t)a->d[i], a1 = a->d[i] >> 32;
      uint64_t b0 = (uint32_t)b->d[j], b1 = b->d[j] >> 32;
      uint64_t p0 = a0 * b0;
      uint64_t p1 = a0 * b1;
      uint64_t p2 = a1 * b0;
      uint64_t p3 = a1 * b1;
      uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
      uint64_t lo = (p0 & UINT32_MAX) | (mid << 32);
      uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
      uint64_t x = out->d[i + j];
      uint64_t y = lo + x;
      hi += y < lo;
      uint64_t z = y + carry;
      hi += z < y;
      out->d[i + j] = z;
      carry = hi;
    }
    out->d[i + b->n] += carry;
  }
#endif
  out->n = n + 1;
  mag_trim(out);
  return true;
}

static size_t mag_to_u32(const bf_mag_t *a, uint32_t *out) {
  size_t n = 0;
  for (size_t i = 0; i < a->n; ++i) {
    out[n++] = (uint32_t)a->d[i];
    out[n++] = (uint32_t)(a->d[i] >> 32);
  }
  while (n && out[n - 1] == 0)
    n--;
  return n;
}

static unsigned bf_clz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
  return x ? (unsigned)__builtin_clz(x) : 32u;
#else
  unsigned n = 0;
  if (!x)
    return 32;
  while (!(x & UINT32_C(0x80000000))) {
    x <<= 1;
    n++;
  }
  return n;
#endif
}

static bool mag_div(bf_mag_t *q, bf_mag_t *r, const bf_mag_t *u,
                    const bf_mag_t *v) {
  if (!v->n)
    return false;
  if (mag_cmp(u, v) < 0) {
    q->n = 0;
    return r ? mag_copy(r, u->d, u->n) : true;
  }
  size_t un_cap = u->n * 2 + 2;
  size_t vn_cap = v->n * 2 + 2;
  uint32_t *uu = calloc(un_cap + 2, sizeof(*uu));
  uint32_t *vv = calloc(vn_cap + 1, sizeof(*vv));
  if (!uu || !vv) {
    free(uu); free(vv);
    return false;
  }
  size_t un = mag_to_u32(u, uu);
  size_t vn = mag_to_u32(v, vv);
  if (!vn) {
    free(uu); free(vv);
    return false;
  }
  size_t qn = un - vn + 1;
  uint32_t *qq = calloc(qn + 1, sizeof(*qq));
  uint32_t *vnorm = calloc(vn, sizeof(*vnorm));
  uint32_t *unorm = calloc(un + 1, sizeof(*unorm));
  if (!qq || !vnorm || !unorm) {
    free(uu); free(vv); free(qq); free(vnorm); free(unorm);
    return false;
  }
  unsigned s = bf_clz32(vv[vn - 1]);
  uint32_t carry = 0;
  for (size_t i = 0; i < vn; ++i) {
    uint64_t x = ((uint64_t)vv[i] << s) | carry;
    vnorm[i] = (uint32_t)x;
    carry = (uint32_t)(x >> 32);
  }
  carry = 0;
  for (size_t i = 0; i < un; ++i) {
    uint64_t x = ((uint64_t)uu[i] << s) | carry;
    unorm[i] = (uint32_t)x;
    carry = (uint32_t)(x >> 32);
  }
  unorm[un] = carry;
  const uint64_t B = UINT64_C(1) << 32;
  for (size_t jj = qn; jj-- > 0;) {
    size_t j = jj;
    uint64_t num = ((uint64_t)unorm[j + vn] << 32) | unorm[j + vn - 1];
    uint64_t qhat = num / vnorm[vn - 1];
    uint64_t rhat = num % vnorm[vn - 1];
    if (qhat >= B) {
      qhat = B - 1;
      rhat += vnorm[vn - 1];
    }
    if (vn > 1) {
      while (rhat < B && qhat * vnorm[vn - 2] >
             (rhat << 32) + unorm[j + vn - 2]) {
        qhat--;
        rhat += vnorm[vn - 1];
        if (rhat >= B)
          break;
      }
    }
    uint64_t k = 0;
    uint64_t borrow = 0;
    for (size_t i = 0; i < vn; ++i) {
      uint64_t p = qhat * vnorm[i] + k;
      k = p >> 32;
      uint64_t sub = (uint32_t)p + borrow;
      uint32_t old = unorm[j + i];
      unorm[j + i] = (uint32_t)(old - sub);
      borrow = old < sub;
    }
    uint64_t subtop = k + borrow;
    uint32_t oldtop = unorm[j + vn];
    unorm[j + vn] = (uint32_t)(oldtop - subtop);
    bool negative = oldtop < subtop;
    if (negative) {
      qhat--;
      uint64_t addcarry = 0;
      for (size_t i = 0; i < vn; ++i) {
        uint64_t z = (uint64_t)unorm[j + i] + vnorm[i] + addcarry;
        unorm[j + i] = (uint32_t)z;
        addcarry = z >> 32;
      }
      unorm[j + vn] = (uint32_t)(unorm[j + vn] + addcarry);
    }
    qq[j] = (uint32_t)qhat;
  }
  size_t qwords = (qn + 1) / 2;
  if (!mag_reserve(q, qwords + 1)) {
    free(uu); free(vv); free(qq); free(vnorm); free(unorm);
    return false;
  }
  memset(q->d, 0, (qwords + 1) * sizeof(*q->d));
  for (size_t i = 0; i < qn; ++i)
    q->d[i / 2] |= (uint64_t)qq[i] << ((i & 1) * 32);
  q->n = qwords;
  mag_trim(q);
  if (r) {
    size_t rn = vn;
    uint32_t *rr = calloc(rn + 1, sizeof(*rr));
    if (!rr || !mag_reserve(r, (rn + 1) / 2 + 1)) {
      free(rr); free(uu); free(vv); free(qq); free(vnorm); free(unorm);
      return false;
    }
    if (!s) {
      memcpy(rr, unorm, rn * sizeof(*rr));
    } else {
      uint32_t c = 0;
      for (size_t i = rn; i-- > 0;) {
        uint32_t x = unorm[i];
        rr[i] = (x >> s) | c;
        c = x << (32 - s);
      }
    }
    memset(r->d, 0, r->cap * sizeof(*r->d));
    for (size_t i = 0; i < rn; ++i)
      r->d[i / 2] |= (uint64_t)rr[i] << ((i & 1) * 32);
    r->n = (rn + 1) / 2;
    mag_trim(r);
    free(rr);
  }
  free(uu); free(vv); free(qq); free(vnorm); free(unorm);
  return true;
}

static int64_t bf_alloc_from_mag(int sign, int64_t exp2, bf_mag_t *m,
                                 uint32_t precision) {
  mag_trim(m);
  if (!m->n || sign == 0) {
    size_t bytes = sizeof(ny_bigfloat_t) + sizeof(uint64_t);
    int64_t p = rt_malloc((int64_t)bytes);
    if (!p)
      return 0;
    *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_BIGFLOAT;
    ny_bigfloat_t *z = (ny_bigfloat_t *)(uintptr_t)p;
    z->sign = 0;
    z->exp2 = 0;
    z->precision = precision;
    z->words = 0;
    return p;
  }
  size_t bits = mag_bitlen(m);
  if (bits > precision) {
    size_t drop = bits - precision;
    bool guard = mag_get_bit(m, drop - 1);
    bool sticky = drop > 1 && mag_any_low(m, drop - 1);
    bool lsb = mag_get_bit(m, drop);
    mag_shr_plain(m, drop);
    exp2 += (int64_t)drop;
    if (guard && (sticky || lsb)) {
      if (!mag_add_one(m))
        return 0;
      if (mag_bitlen(m) > precision) {
        mag_shr_plain(m, 1);
        exp2++;
      }
    }
  } else if (bits < precision) {
    size_t shift = precision - bits;
    if (!mag_shl(m, shift))
      return 0;
    exp2 -= (int64_t)shift;
  }
  mag_trim(m);
  size_t bytes = sizeof(ny_bigfloat_t) + m->n * sizeof(uint64_t);
  int64_t p = rt_malloc((int64_t)bytes);
  if (!p)
    return 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_BIGFLOAT;
  ny_bigfloat_t *x = (ny_bigfloat_t *)(uintptr_t)p;
  x->sign = sign < 0 ? -1 : 1;
  x->exp2 = exp2;
  x->precision = precision;
  x->words = (uint32_t)m->n;
  memcpy(x->limb, m->d, m->n * sizeof(uint64_t));
  return p;
}

static int64_t bf_from_u64_raw(uint64_t mag, int sign, int64_t exp2,
                               uint32_t precision) {
  bf_mag_t m = {0};
  if (mag) {
    if (!mag_reserve(&m, 2))
      return 0;
    m.d[0] = mag;
    m.n = 1;
  }
  int64_t out = bf_alloc_from_mag(sign, exp2, &m, precision);
  mag_free(&m);
  return out;
}

static int64_t bf_clone_prec(const ny_bigfloat_t *x, uint32_t precision) {
  bf_mag_t m = {0};
  if (!mag_copy(&m, x->limb, x->words))
    return 0;
  int64_t out = bf_alloc_from_mag((int)x->sign, x->exp2, &m, precision);
  mag_free(&m);
  return out;
}

static int64_t bf_from_value_raw(int64_t v, uint32_t precision) {
  ny_bigfloat_t *bf = bf_ptr(v);
  if (bf)
    return bf_clone_prec(bf, precision);
  if (is_int(v)) {
    int64_t raw = rt_untag_v(v);
    uint64_t mag = raw < 0 ? (uint64_t)(-(raw + 1)) + 1 : (uint64_t)raw;
    return bf_from_u64_raw(mag, raw < 0 ? -1 : raw > 0, 0, precision);
  }
  if (is_v_flt(v)) {
    double d = rt_flt_unbox_double(v);
    if (!isfinite(d) || d == 0.0)
      return bf_from_u64_raw(0, 0, 0, precision);
    int e = 0;
    double f = frexp(fabs(d), &e);
    uint64_t mant = (uint64_t)ldexp(f, 53);
    return bf_from_u64_raw(mant, signbit(d) ? -1 : 1, e - 53,
                           precision);
  }
  if (is_ptr(v) && is_heap_ptr(v) &&
      *(int64_t *)((char *)(uintptr_t)v - 8) == TAG_BIGINT) {
    int sign = (int)rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 0));
    int64_t words = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 8));
    if (words < 0)
      return 0;
    bf_mag_t m = {0};
    if (!mag_copy(&m, (const uint64_t *)((char *)(uintptr_t)v + 16),
                  (size_t)words))
      return 0;
    int64_t out = bf_alloc_from_mag(sign, 0, &m, precision);
    mag_free(&m);
    return out;
  }
  return bf_from_u64_raw(0, 0, 0, precision);
}

static int bf_cmp_obj(const ny_bigfloat_t *a, const ny_bigfloat_t *b) {
  if (a->sign != b->sign)
    return a->sign < b->sign ? -1 : 1;
  if (!a->sign)
    return 0;
  int dir = a->sign < 0 ? -1 : 1;
  int64_t atop = a->exp2 + (int64_t)a->precision;
  int64_t btop = b->exp2 + (int64_t)b->precision;
  if (atop != btop)
    return (atop < btop ? -1 : 1) * dir;
  uint32_t p = a->precision > b->precision ? a->precision : b->precision;
  bf_mag_t am = {0}, bm = {0};
  if (!mag_copy(&am, a->limb, a->words) ||
      !mag_copy(&bm, b->limb, b->words)) {
    mag_free(&am); mag_free(&bm);
    return 0;
  }
  if (p > a->precision)
    mag_shl(&am, p - a->precision);
  if (p > b->precision)
    mag_shl(&bm, p - b->precision);
  int c = mag_cmp(&am, &bm) * dir;
  mag_free(&am); mag_free(&bm);
  return c;
}

static int64_t bf_addsub_obj(const ny_bigfloat_t *a, const ny_bigfloat_t *b,
                             bool subtract) {
  uint32_t p = a->precision > b->precision ? a->precision : b->precision;
  if (!a->sign)
    return bf_clone_prec(b, p);
  if (!b->sign)
    return bf_clone_prec(a, p);
  int bs = subtract ? -(int)b->sign : (int)b->sign;
  uint32_t qprec = p + 8;
  bf_mag_t am = {0}, bm = {0}, out = {0};
  if (!mag_copy(&am, a->limb, a->words) ||
      !mag_copy(&bm, b->limb, b->words))
    goto fail;
  if (qprec > a->precision && !mag_shl(&am, qprec - a->precision))
    goto fail;
  if (qprec > b->precision && !mag_shl(&bm, qprec - b->precision))
    goto fail;
  int64_t ae = a->exp2 - (int64_t)(qprec - a->precision);
  int64_t be = b->exp2 - (int64_t)(qprec - b->precision);
  int64_t common = ae > be ? ae : be;
  if (common > ae)
    mag_shr_jam(&am, (size_t)(common - ae));
  if (common > be)
    mag_shr_jam(&bm, (size_t)(common - be));
  int sign = 0;
  if ((int)a->sign == bs) {
    if (!mag_add(&out, &am, &bm))
      goto fail;
    sign = (int)a->sign;
  } else {
    int c = mag_cmp(&am, &bm);
    if (!c) {
      mag_free(&am); mag_free(&bm); mag_free(&out);
      return bf_from_u64_raw(0, 0, 0, p);
    }
    if (c > 0) {
      if (!mag_sub(&out, &am, &bm))
        goto fail;
      sign = (int)a->sign;
    } else {
      if (!mag_sub(&out, &bm, &am))
        goto fail;
      sign = bs;
    }
  }
  {
    int64_t result = bf_alloc_from_mag(sign, common, &out, p);
    mag_free(&am); mag_free(&bm); mag_free(&out);
    return result;
  }
fail:
  mag_free(&am); mag_free(&bm); mag_free(&out);
  return 0;
}

static int64_t bf_mul_obj(const ny_bigfloat_t *a, const ny_bigfloat_t *b) {
  uint32_t p = a->precision > b->precision ? a->precision : b->precision;
  if (!a->sign || !b->sign)
    return bf_from_u64_raw(0, 0, 0, p);
  bf_mag_t am = {0}, bm = {0}, out = {0};
  if (!mag_copy(&am, a->limb, a->words) ||
      !mag_copy(&bm, b->limb, b->words) || !mag_mul(&out, &am, &bm)) {
    mag_free(&am); mag_free(&bm); mag_free(&out);
    return 0;
  }
  int64_t result = bf_alloc_from_mag((int)(a->sign * b->sign),
                                     a->exp2 + b->exp2, &out, p);
  mag_free(&am); mag_free(&bm); mag_free(&out);
  return result;
}

static int64_t bf_div_obj(const ny_bigfloat_t *a, const ny_bigfloat_t *b) {
  uint32_t p = a->precision > b->precision ? a->precision : b->precision;
  if (!b->sign)
    return bf_from_u64_raw(0, 0, 0, p);
  if (!a->sign)
    return bf_from_u64_raw(0, 0, 0, p);
  bf_mag_t num = {0}, den = {0}, quo = {0}, rem = {0};
  uint32_t extra = p + 10;
  if (!mag_copy(&num, a->limb, a->words) ||
      !mag_copy(&den, b->limb, b->words) || !mag_shl(&num, extra) ||
      !mag_div(&quo, &rem, &num, &den)) {
    mag_free(&num); mag_free(&den); mag_free(&quo); mag_free(&rem);
    return 0;
  }
  if (rem.n) {
    if (!quo.n) {
      mag_reserve(&quo, 1);
      quo.n = 1;
      quo.d[0] = 1;
    } else {
      quo.d[0] |= 1;
    }
  }
  int64_t result = bf_alloc_from_mag((int)(a->sign * b->sign),
                                     a->exp2 - b->exp2 - extra, &quo, p);
  mag_free(&num); mag_free(&den); mag_free(&quo); mag_free(&rem);
  return result;
}

static int64_t bf_scale2_obj(const ny_bigfloat_t *a, int64_t shift) {
  bf_mag_t m = {0};
  if (!mag_copy(&m, a->limb, a->words))
    return 0;
  int64_t out = bf_alloc_from_mag((int)a->sign, a->exp2 + shift, &m,
                                  a->precision);
  mag_free(&m);
  return out;
}

int64_t rt_bigfloat_from_value(int64_t v, int64_t precision_v) {
  int64_t p = is_int(precision_v) ? rt_untag_v(precision_v) : precision_v;
  return bf_from_value_raw(v, bf_precision_clamp(p ? p : bf_default_precision()));
}

int64_t rt_bigfloat_zero(int64_t precision_v) {
  int64_t p = is_int(precision_v) ? rt_untag_v(precision_v) : precision_v;
  return bf_from_u64_raw(0, 0, 0,
                         bf_precision_clamp(p ? p : bf_default_precision()));
}

int64_t rt_bigfloat_one(int64_t precision_v) {
  int64_t p = is_int(precision_v) ? rt_untag_v(precision_v) : precision_v;
  return bf_from_u64_raw(1, 1, 0,
                         bf_precision_clamp(p ? p : bf_default_precision()));
}

int64_t rt_bigfloat_add(int64_t av, int64_t bv) {
  ny_bigfloat_t *a = bf_ptr(av), *b = bf_ptr(bv);
  if (!a || !b)
    return 0;
  return bf_addsub_obj(a, b, false);
}

int64_t rt_bigfloat_sub(int64_t av, int64_t bv) {
  ny_bigfloat_t *a = bf_ptr(av), *b = bf_ptr(bv);
  if (!a || !b)
    return 0;
  return bf_addsub_obj(a, b, true);
}

int64_t rt_bigfloat_mul(int64_t av, int64_t bv) {
  ny_bigfloat_t *a = bf_ptr(av), *b = bf_ptr(bv);
  if (!a || !b)
    return 0;
  return bf_mul_obj(a, b);
}

int64_t rt_bigfloat_div(int64_t av, int64_t bv) {
  ny_bigfloat_t *a = bf_ptr(av), *b = bf_ptr(bv);
  if (!a || !b)
    return 0;
  return bf_div_obj(a, b);
}

int64_t rt_bigfloat_neg(int64_t av) {
  ny_bigfloat_t *a = bf_ptr(av);
  if (!a)
    return 0;
  bf_mag_t m = {0};
  if (!mag_copy(&m, a->limb, a->words))
    return 0;
  int64_t out = bf_alloc_from_mag(-(int)a->sign, a->exp2, &m, a->precision);
  mag_free(&m);
  return out;
}

int64_t rt_bigfloat_abs(int64_t av) {
  ny_bigfloat_t *a = bf_ptr(av);
  if (!a)
    return 0;
  bf_mag_t m = {0};
  if (!mag_copy(&m, a->limb, a->words))
    return 0;
  int64_t out = bf_alloc_from_mag(a->sign ? 1 : 0, a->exp2, &m, a->precision);
  mag_free(&m);
  return out;
}

int64_t rt_bigfloat_cmp(int64_t av, int64_t bv) {
  ny_bigfloat_t *a = bf_ptr(av), *b = bf_ptr(bv);
  if (!a || !b)
    return rt_tag_v(0);
  return rt_tag_v(bf_cmp_obj(a, b));
}

int64_t rt_bigfloat_precision(int64_t av) {
  ny_bigfloat_t *a = bf_ptr(av);
  return rt_tag_v(a ? (int64_t)a->precision : 0);
}

int64_t rt_bigfloat_to_f64(int64_t av) {
  ny_bigfloat_t *a = bf_ptr(av);
  double d = 0.0;
  if (a && a->sign && a->words) {
    size_t bits = a->precision;
    size_t take = bits > 63 ? 63 : bits;
    bf_mag_t m = {0};
    if (mag_copy(&m, a->limb, a->words)) {
      if (bits > take)
        mag_shr_jam(&m, bits - take);
      uint64_t top = m.n ? m.d[0] : 0;
      int64_t e = a->exp2 + (int64_t)(bits - take);
      if (e > INT_MAX)
        d = INFINITY;
      else if (e < INT_MIN)
        d = 0.0;
      else
        d = ldexp((double)top, (int)e);
      if (a->sign < 0)
        d = -d;
    }
    mag_free(&m);
  }
  return rt_flt_box_double(d);
}

int64_t rt_bigfloat_sqrt(int64_t av) {
  ny_bigfloat_t *a = bf_ptr(av);
  if (!a || a->sign <= 0)
    return bf_from_u64_raw(0, 0, 0, a ? a->precision : bf_default_precision());
  int64_t top = a->exp2 + (int64_t)a->precision - 1;
  int64_t guess_exp = top >= 0 ? top / 2 : -(((-top) + 1) / 2);
  int64_t xv = bf_from_u64_raw(1, 1, guess_exp, a->precision);
  if (!xv)
    return 0;
  unsigned iterations = 4;
  for (uint32_t p = a->precision; p > 1; p >>= 1)
    iterations++;
  for (unsigned i = 0; i < iterations; ++i) {
    ny_bigfloat_t *x = bf_ptr(xv);
    int64_t qv = bf_div_obj(a, x);
    ny_bigfloat_t *q = bf_ptr(qv);
    int64_t sv = q ? bf_addsub_obj(x, q, false) : 0;
    ny_bigfloat_t *sum = bf_ptr(sv);
    int64_t nv = sum ? bf_scale2_obj(sum, -1) : 0;
    if (!nv)
      return xv;
    ny_bigfloat_t *next = bf_ptr(nv);
    if (next && bf_cmp_obj(x, next) == 0)
      return nv;
    xv = nv;
  }
  return xv;
}

int64_t rt_bigfloat_pow_int(int64_t av, int64_t ev) {
  ny_bigfloat_t *a = bf_ptr(av);
  if (!a)
    return 0;
  int64_t e = is_int(ev) ? rt_untag_v(ev) : ev;
  bool negative = e < 0;
  uint64_t n = negative ? (uint64_t)(-(e + 1)) + 1 : (uint64_t)e;
  int64_t result = bf_from_u64_raw(1, 1, 0, a->precision);
  int64_t base = bf_clone_prec(a, a->precision);
  while (n) {
    if (n & 1) {
      ny_bigfloat_t *r = bf_ptr(result), *b = bf_ptr(base);
      result = bf_mul_obj(r, b);
    }
    n >>= 1;
    if (n) {
      ny_bigfloat_t *b = bf_ptr(base);
      base = bf_mul_obj(b, b);
    }
  }
  if (negative) {
    int64_t one = bf_from_u64_raw(1, 1, 0, a->precision);
    return bf_div_obj(bf_ptr(one), bf_ptr(result));
  }
  return result;
}

int64_t rt_bigfloat_to_str(int64_t av) {
  ny_bigfloat_t *a = bf_ptr(av);
  if (!a || !a->sign)
    return rt_alloc_string("0.0");
  int64_t fv = rt_bigfloat_to_f64(av);
  double d = rt_flt_unbox_double(fv);
  char buf[96];
  if (isfinite(d))
    snprintf(buf, sizeof(buf), "%.17g", d);
  else {
    int64_t top = a->exp2 + (int64_t)a->precision - 1;
    snprintf(buf, sizeof(buf), "%s0x1p%" PRId64,
             a->sign < 0 ? "-" : "", top);
  }
  return rt_alloc_string(buf);
}

int64_t rt_bigfloat_is(int64_t v) {
  return bf_ptr(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
}
