/*
 * BigInt runtime: arbitrary-precision integer arithmetic with optional
 * GMP backend, tagged-value boxing, and NyValue conversion helpers.
 */
#ifdef NYTRIX_USE_GMP
#include <gmp.h>
#endif
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rt/runtime.h"
#include "rt/shared.h"

extern int64_t rt_tagof(int64_t v);
extern int64_t rt_list_new(int64_t n_v);
extern int64_t rt_append(int64_t lst, int64_t val);
extern int64_t rt_alloc_string(const char *s);

static inline int64_t _bi_alloc(size_t word_count) {
  size_t bytes = 16 + word_count * sizeof(uint64_t);
  int64_t p = rt_malloc((int64_t)bytes);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p - 8) = TAG_BIGINT;
  *(int64_t *)(uintptr_t)((char *)p + 8) = rt_tag_v((int64_t)word_count);
  return p;
}

static int64_t _bi_from_i64(int64_t v);

#ifdef NYTRIX_USE_GMP

void _bi_mpz_set_i64(mpz_t out, int64_t v) {
  uint64_t mag = 0;
  if (v < 0) {
    mag = (uint64_t)(-(v + 1)) + 1u;
    mpz_import(out, 1, -1, sizeof(mag), 0, 0, &mag);
    mpz_neg(out, out);
  } else {
    mag = (uint64_t)v;
    mpz_import(out, 1, -1, sizeof(mag), 0, 0, &mag);
  }
}

bool _bi_mpz_fits_small_int(const mpz_t v) {
  mpz_t limit;
  mpz_init(limit);
  if (mpz_sgn(v) >= 0) {
    _bi_mpz_set_i64(limit, NY_SMALL_INT_MAX);
    bool ok = mpz_cmp(v, limit) <= 0;
    mpz_clear(limit);
    return ok;
  }
  mpz_t tmp;
  mpz_init(tmp);
  mpz_neg(tmp, v);
  _bi_mpz_set_i64(limit, NY_SMALL_INT_MAX);
  mpz_add_ui(limit, limit, 1);
  bool ok = mpz_cmp(tmp, limit) <= 0;
  mpz_clear(tmp);
  mpz_clear(limit);
  return ok;
}

int64_t _bi_mpz_get_i64(const mpz_t v) {
  uint64_t mag = 0;
  size_t count = 0;
  mpz_t tmp;
  mpz_init(tmp);
  if (mpz_sgn(v) < 0)
    mpz_neg(tmp, v);
  else
    mpz_set(tmp, v);
  mpz_export(&mag, &count, -1, sizeof(mag), 0, 0, tmp);
  mpz_clear(tmp);
  if (mpz_sgn(v) < 0)
    return -(int64_t)(mag - 1u) - 1;
  return (int64_t)mag;
}

void _bi_val_to_mpz(int64_t v, mpz_t result) {
  mpz_init(result);
  if (is_int(v)) {
    _bi_mpz_set_i64(result, rt_untag_v(v));
    return;
  }
  if (!is_ptr(v))
    return;
  int64_t tag = *(int64_t *)(uintptr_t)((char *)v - 8);
  if (tag != TAG_BIGINT)
    return;
  int64_t sign = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 0));
  int64_t words = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 8));
  if (words > 0) {
    mpz_import(result, (size_t)words, -1, sizeof(uint64_t), 0, 0,
               (const void *)((char *)v + 16));
    if (sign < 0)
      mpz_neg(result, result);
  }
}

int64_t _bi_from_mpz(const mpz_t val) {
  int sign = mpz_sgn(val);
  if (sign == 0) {
    int64_t p = _bi_alloc(0);
    if (!p)
      return 0;
    *(int64_t *)(uintptr_t)((char *)p + 0) = rt_tag_v(0);
    return p;
  }
  size_t word_count = 0;
  void *words = mpz_export(NULL, &word_count, -1, sizeof(uint64_t), 0, 0, val);
  int64_t p = _bi_alloc(word_count);
  if (!p) {
    free(words);
    return 0;
  }
  *(int64_t *)(uintptr_t)((char *)p + 0) =
      rt_tag_v((int64_t)(sign > 0 ? 1 : -1));
  if (word_count > 0 && words)
    memcpy((void *)((char *)p + 16), words, word_count * sizeof(uint64_t));
  free(words);
  return p;
}

#else /* !NYTRIX_USE_GMP — native bigint library */

typedef struct {
  int sign;
  size_t count;
  uint64_t *d;
} _bn_t;

static size_t _bn_bitlen(const _bn_t *a);
static void _bn_shl1(_bn_t *a);
static void _bn_shr(_bn_t *r, const _bn_t *a, size_t bits);

static inline void _bn_init(_bn_t *a) {
  a->sign = 0;
  a->count = 0;
  a->d = NULL;
}
static inline void _bn_clear(_bn_t *a) {
  free(a->d);
  a->d = NULL;
  a->count = 0;
  a->sign = 0;
}
static void _bn_set_zero(_bn_t *a) {
  free(a->d);
  a->d = NULL;
  a->count = 0;
  a->sign = 0;
}
static void _bn_grow(_bn_t *a, size_t n) {
  if (n <= a->count)
    return;
  uint64_t *nd = (uint64_t *)realloc(a->d, n * sizeof(uint64_t));
  if (!nd)
    return;
  memset(nd + a->count, 0, (n - a->count) * sizeof(uint64_t));
  a->d = nd;
  a->count = n;
}
static void _bn_trim(_bn_t *a) {
  while (a->count > 0 && a->d[a->count - 1] == 0)
    a->count--;
  if (a->count == 0)
    a->sign = 0;
}
static void _bn_set_i64(_bn_t *a, int64_t v) {
  if (v == 0) {
    _bn_set_zero(a);
    return;
  }
  uint64_t *nd = (uint64_t *)realloc(a->d, sizeof(uint64_t));
  if (!nd)
    return;
  a->d = nd;
  if (v < 0) {
    a->sign = -1;
    a->d[0] = (uint64_t)(-(v + 1)) + 1u;
  } else {
    a->sign = 1;
    a->d[0] = (uint64_t)v;
  }
  a->count = 1;
}
static void _bn_copy(_bn_t *r, const _bn_t *a) {
  if (r == a)
    return;
  if (a->count == 0 || a->sign == 0) {
    _bn_set_zero(r);
    return;
  }
  uint64_t *nd = (uint64_t *)malloc(a->count * sizeof(uint64_t));
  if (!nd)
    return;
  memcpy(nd, a->d, a->count * sizeof(uint64_t));
  free(r->d);
  r->d = nd;
  r->sign = a->sign;
  r->count = a->count;
}
static inline bool _bn_is_zero(const _bn_t *a) {
  return a->count == 0 || a->sign == 0;
}
static int _bn_cmp_mag(const _bn_t *a, const _bn_t *b) {
  if (a->count != b->count)
    return a->count > b->count ? 1 : -1;
  for (size_t i = a->count; i > 0; i--) {
    if (a->d[i - 1] != b->d[i - 1])
      return a->d[i - 1] > b->d[i - 1] ? 1 : -1;
  }
  return 0;
}
static int _bn_cmp(const _bn_t *a, const _bn_t *b) {
  if (a->sign != b->sign) {
    if (a->sign == 0)
      return b->sign > 0 ? -1 : 1;
    if (b->sign == 0)
      return a->sign > 0 ? 1 : -1;
    return a->sign > b->sign ? 1 : -1;
  }
  int r = _bn_cmp_mag(a, b);
  return a->sign < 0 ? -r : r;
}
static void _bn_abs(_bn_t *r, const _bn_t *a) {
  _bn_copy(r, a);
  if (r->sign < 0)
    r->sign = 1;
}
static void _bn_add_mag(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  size_t n = a->count > b->count ? a->count : b->count;
  _bn_grow(r, n + 1);
  uint64_t carry = 0;
  for (size_t i = 0; i < n; i++) {
    __uint128_t s = (__uint128_t)(i < a->count ? a->d[i] : 0) +
                    (i < b->count ? b->d[i] : 0) + carry;
    r->d[i] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
  }
  r->d[n] = carry;
  r->count = n + 1;
  r->sign = 1;
  _bn_trim(r);
}
static void _bn_sub_mag(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_grow(r, a->count);
  uint64_t borrow = 0;
  for (size_t i = 0; i < a->count; i++) {
    uint64_t av = a->d[i];
    uint64_t bv = i < b->count ? b->d[i] : 0;
    uint64_t sub = bv + borrow;
    uint64_t next_borrow = (borrow && sub == 0) || av < sub;
    r->d[i] = av - sub;
    borrow = next_borrow;
  }
  r->count = a->count;
  r->sign = 1;
  _bn_trim(r);
}
static void _bn_add(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  if (_bn_is_zero(a)) {
    _bn_copy(r, b);
    return;
  }
  if (_bn_is_zero(b)) {
    _bn_copy(r, a);
    return;
  }
  if (a->sign == b->sign) {
    _bn_add_mag(r, a, b);
    r->sign = a->sign;
  } else {
    int cmp = _bn_cmp_mag(a, b);
    if (cmp == 0) {
      _bn_set_zero(r);
      return;
    }
    if (cmp > 0) {
      _bn_sub_mag(r, a, b);
      r->sign = a->sign;
    } else {
      _bn_sub_mag(r, b, a);
      r->sign = b->sign;
    }
  }
}
static void _bn_neg(_bn_t *r, const _bn_t *a) {
  _bn_copy(r, a);
  if (r->sign != 0)
    r->sign = -r->sign;
}
static void _bn_sub(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_t tmp = {0};
  _bn_neg(&tmp, b);
  _bn_add(r, a, &tmp);
  _bn_clear(&tmp);
}
static inline size_t _bn_max2(size_t a, size_t b) { return a > b ? a : b; }

#define KARATSUBA_THRESHOLD 32

static void _bn_mul_karatsuba(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  if (_bn_is_zero(a) || _bn_is_zero(b)) {
    _bn_set_zero(r);
    return;
  }
  size_t max_n = _bn_max2(a->count, b->count);
  if (max_n <= KARATSUBA_THRESHOLD) {
    size_t n = a->count + b->count;
    _bn_grow(r, n);
    memset(r->d, 0, n * sizeof(uint64_t));
    for (size_t i = 0; i < a->count; i++) {
      __uint128_t carry = 0;
      for (size_t j = 0; j < b->count; j++) {
        __uint128_t cur = (__uint128_t)a->d[i] * b->d[j] + r->d[i + j] + carry;
        r->d[i + j] = (uint64_t)cur;
        carry = cur >> 64;
      }
      size_t k = i + b->count;
      while (carry && k < n) {
        __uint128_t cur = (__uint128_t)r->d[k] + carry;
        r->d[k] = (uint64_t)cur;
        carry = cur >> 64;
        k++;
      }
    }
    r->count = n;
    r->sign = (a->sign == b->sign) ? 1 : -1;
    _bn_trim(r);
    return;
  }
  size_t m = max_n / 2;
  _bn_t a_lo = {0}, a_hi = {0}, b_lo = {0}, b_hi = {0};
  a_lo.d = a->d;
  a_lo.count = a->count < m ? a->count : m;
  a_lo.sign = 1;
  a_hi.d = a->count > m ? a->d + m : NULL;
  a_hi.count = a->count > m ? a->count - m : 0;
  a_hi.sign = 1;
  b_lo.d = b->d;
  b_lo.count = b->count < m ? b->count : m;
  b_lo.sign = 1;
  b_hi.d = b->count > m ? b->d + m : NULL;
  b_hi.count = b->count > m ? b->count - m : 0;
  b_hi.sign = 1;
  _bn_t z0 = {0}, z2 = {0}, t = {0}, s = {0}, tps = {0};
  _bn_mul_karatsuba(&z0, &a_lo, &b_lo);
  _bn_mul_karatsuba(&z2, &a_hi, &b_hi);
  _bn_add(&t, &a_lo, &a_hi);
  _bn_add(&s, &b_lo, &b_hi);
  _bn_mul_karatsuba(&tps, &t, &s);
  _bn_sub(&t, &tps, &z0);
  _bn_sub(&s, &t, &z2);
  _bn_clear(&t);
  _bn_clear(&tps);
  size_t rlen = 2 * m + _bn_max2(z2.count, _bn_max2(s.count, 0)) + 1;
  _bn_grow(r, rlen);
  memset(r->d, 0, rlen * sizeof(uint64_t));
  for (size_t i = 0; i < z0.count; i++)
    r->d[i] = z0.d[i];
  __uint128_t carry = 0;
  for (size_t i = 0; i < s.count; i++) {
    carry += (__uint128_t)r->d[m + i] + s.d[i];
    r->d[m + i] = (uint64_t)carry;
    carry >>= 64;
  }
  for (size_t i = m + s.count; carry && i < rlen; i++) {
    carry += (__uint128_t)r->d[i];
    r->d[i] = (uint64_t)carry;
    carry >>= 64;
  }
  carry = 0;
  for (size_t i = 0; i < z2.count; i++) {
    carry += (__uint128_t)r->d[2 * m + i] + z2.d[i];
    r->d[2 * m + i] = (uint64_t)carry;
    carry >>= 64;
  }
  for (size_t i = 2 * m + z2.count; carry && i < rlen; i++) {
    carry += (__uint128_t)r->d[i];
    r->d[i] = (uint64_t)carry;
    carry >>= 64;
  }
  r->count = rlen;
  r->sign = (a->sign == b->sign) ? 1 : -1;
  _bn_trim(r);
  _bn_clear(&z0);
  _bn_clear(&z2);
  _bn_clear(&s);
}

static void _bn_mul(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  if (_bn_is_zero(a) || _bn_is_zero(b)) {
    _bn_set_zero(r);
    return;
  }
  _bn_t aa = {0}, bb = {0};
  const _bn_t *ap = a, *bp = b;
  if (r == a) {
    _bn_copy(&aa, a);
    ap = &aa;
  }
  if (r == b) {
    _bn_copy(&bb, b);
    bp = &bb;
  }
  _bn_mul_karatsuba(r, ap, bp);
  _bn_clear(&aa);
  _bn_clear(&bb);
}

/*
 * Binary fallback retained for allocation failure in the normalized divider.
 */
static void _bn_divmod_binary(_bn_t *q, _bn_t *r, const _bn_t *a,
                              const _bn_t *b) {
  size_t abits = _bn_bitlen(a);
  _bn_set_zero(q);
  _bn_set_zero(r);
  for (size_t bit = abits; bit-- > 0;) {
    _bn_shl1(r);
    size_t word = bit / 64, off = bit % 64;
    if (word < a->count && ((a->d[word] >> off) & 1u)) {
      if (r->count == 0) {
        _bn_grow(r, 1);
        if (r->count == 0)
          return;
        r->d[0] = 0;
      }
      r->sign = 1;
      r->d[0] |= 1u;
    }
    _bn_shl1(q);
    if (!_bn_is_zero(r) && _bn_cmp_mag(r, b) >= 0) {
      _bn_t tmp = {0};
      _bn_sub_mag(&tmp, r, b);
      _bn_clear(r);
      *r = tmp;
      if (q->count == 0) {
        _bn_grow(q, 1);
        if (q->count == 0)
          return;
        q->d[0] = 0;
      }
      q->sign = 1;
      q->d[0] |= 1u;
    }
  }
  _bn_trim(q);
  _bn_trim(r);
  q->sign = _bn_is_zero(q) ? 0 : 1;
  r->sign = _bn_is_zero(r) ? 0 : 1;
}

/*
 * Knuth Algorithm D over the native base-2^64 limbs.  Normalizing the divisor
 * makes the two-limb quotient estimate exact or at most two too large; the
 * correction step then avoids the bit-at-a-time allocation/copy loop.
 */
static void _bn_divmod_knuth(_bn_t *q, _bn_t *r, const _bn_t *a,
                             const _bn_t *b) {
  if (_bn_is_zero(b)) {
    _bn_set_zero(q);
    _bn_copy(r, a);
    return;
  }
  int cmp = _bn_cmp_mag(a, b);
  if (cmp < 0) {
    _bn_set_zero(q);
    _bn_copy(r, a);
    r->sign = _bn_is_zero(r) ? 0 : 1;
    return;
  }
  if (cmp == 0) {
    _bn_set_i64(q, 1);
    _bn_set_zero(r);
    return;
  }

  size_t n = b->count;
  if (n == 1) {
    uint64_t divisor = b->d[0], rem = 0;
    _bn_grow(q, a->count);
    if (q->count < a->count) {
      _bn_divmod_binary(q, r, a, b);
      return;
    }
    for (size_t i = a->count; i-- > 0;) {
      __uint128_t cur = ((__uint128_t)rem << 64) | a->d[i];
      q->d[i] = (uint64_t)(cur / divisor);
      rem = (uint64_t)(cur % divisor);
    }
    q->count = a->count;
    q->sign = 1;
    _bn_trim(q);
    if (rem)
      _bn_set_i64(r, (int64_t)rem), r->d[0] = rem, r->sign = 1;
    else
      _bn_set_zero(r);
    return;
  }

  size_t m = a->count - n;
  unsigned shift = (unsigned)__builtin_clzll(b->d[n - 1]);
  uint64_t *vn = calloc(n, sizeof(*vn));
  uint64_t *un = calloc(a->count + 1, sizeof(*un));
  uint64_t *qd = calloc(m + 1, sizeof(*qd));
  if (!vn || !un || !qd) {
    free(vn); free(un); free(qd);
    _bn_divmod_binary(q, r, a, b);
    return;
  }
  if (shift == 0) {
    memcpy(vn, b->d, n * sizeof(*vn));
    memcpy(un, a->d, a->count * sizeof(*un));
  } else {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
      uint64_t x = b->d[i];
      vn[i] = (x << shift) | carry;
      carry = x >> (64 - shift);
    }
    carry = 0;
    for (size_t i = 0; i < a->count; ++i) {
      uint64_t x = a->d[i];
      un[i] = (x << shift) | carry;
      carry = x >> (64 - shift);
    }
    un[a->count] = carry;
  }

  for (size_t jj = m + 1; jj-- > 0;) {
    size_t j = jj;
    uint64_t qhat, rhat;
    bool rhat_carried = false;
    if (un[j + n] == vn[n - 1]) {
      qhat = UINT64_MAX;
      rhat = un[j + n - 1] + vn[n - 1];
      /*
       * rhat may carry past 2^64 here (un[j+n-1] + vn[n-1] >= B).  A carried
       * rhat makes B*rhat + un[j+n-2] >= B^2 > qhat*vn[n-2], so qhat = B-1 is
       * already exact and the correction loop must be skipped: evaluating it
       * against the wrapped rhat would wrongly decrement qhat.
       */
      rhat_carried = rhat < vn[n - 1];
    } else {
      __uint128_t top = ((__uint128_t)un[j + n] << 64) | un[j + n - 1];
      qhat = (uint64_t)(top / vn[n - 1]);
      rhat = (uint64_t)(top % vn[n - 1]);
    }
    if (!rhat_carried) {
      while ((__uint128_t)qhat * vn[n - 2] >
             ((__uint128_t)rhat << 64) + un[j + n - 2]) {
        --qhat;
        uint64_t old = rhat;
        rhat += vn[n - 1];
        if (rhat < old)
          break;
      }
    }

    uint64_t borrow = 0;
    for (size_t i = 0; i < n; ++i) {
      __uint128_t product = (__uint128_t)qhat * vn[i] + borrow;
      uint64_t low = (uint64_t)product, old = un[j + i];
      un[j + i] = old - low;
      borrow = (uint64_t)(product >> 64) + (old < low);
    }
    bool negative = un[j + n] < borrow;
    un[j + n] -= borrow;
    if (negative) {
      --qhat;
      uint64_t carry = 0;
      for (size_t i = 0; i < n; ++i) {
        __uint128_t sum = (__uint128_t)un[j + i] + vn[i] + carry;
        un[j + i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
      }
      un[j + n] += carry;
    }
    qd[j] = qhat;
  }

  _bn_grow(q, m + 1);
  _bn_grow(r, n);
  if (q->count < m + 1 || r->count < n) {
    free(vn); free(un); free(qd);
    _bn_divmod_binary(q, r, a, b);
    return;
  }
  memcpy(q->d, qd, (m + 1) * sizeof(*qd));
  q->count = m + 1;
  q->sign = 1;
  if (shift == 0) {
    memcpy(r->d, un, n * sizeof(*un));
  } else {
    uint64_t carry = 0;
    for (size_t i = n; i-- > 0;) {
      uint64_t x = un[i];
      r->d[i] = (x >> shift) | carry;
      carry = x << (64 - shift);
    }
  }
  r->count = n;
  r->sign = 1;
  _bn_trim(q);
  _bn_trim(r);
  q->sign = _bn_is_zero(q) ? 0 : 1;
  r->sign = _bn_is_zero(r) ? 0 : 1;
  free(vn); free(un); free(qd);
}

#ifndef BURNIKEL_ZIEGLER_THRESHOLD
#define BURNIKEL_ZIEGLER_THRESHOLD 32u
#endif

/*
 * Copy limbs [first, first + count) without changing their base-2^64 value.
 */
static bool _bn_limb_slice(_bn_t *r, const _bn_t *a, size_t first,
                           size_t count) {
  if (first >= a->count || count == 0) {
    _bn_set_zero(r);
    return true;
  }
  if (count > a->count - first)
    count = a->count - first;
  _bn_grow(r, count);
  if (r->count < count)
    return false;
  memcpy(r->d, a->d + first, count * sizeof(*r->d));
  r->count = count;
  r->sign = 1;
  _bn_trim(r);
  return true;
}

/*
 * r = high * B^low.count + low, where B=2^64 and both inputs are positive.
 */
static bool _bn_join_limbs(_bn_t *r, const _bn_t *high, const _bn_t *low,
                           size_t low_count) {
  size_t n = high->count + low_count;
  if (n < low_count)
    return false;
  _bn_grow(r, n);
  if (r->count < n)
    return false;
  memset(r->d, 0, n * sizeof(*r->d));
  if (low->count)
    memcpy(r->d, low->d, low->count * sizeof(*r->d));
  if (high->count)
    memcpy(r->d + low_count, high->d, high->count * sizeof(*r->d));
  r->count = n;
  r->sign = n ? 1 : 0;
  _bn_trim(r);
  return true;
}

/*
 * Burnikel-Ziegler block division.  Large dividends are consumed in divisor-
 * sized base-B blocks, reducing each 2n/n subproblem with normalized Algorithm
 * D.  This keeps temporary operands bounded to two divisor blocks and is the
 * dedicated large-operand path; the threshold below prevents small values from
 * paying its block assembly cost.
 */
static void _bn_divmod_burnikel_ziegler(_bn_t *q, _bn_t *r, const _bn_t *a,
                                        const _bn_t *b) {
  size_t block = b->count;
  size_t blocks = (a->count + block - 1) / block;
  _bn_t rem = {0}, quotient = {0};
  _bn_grow(&quotient, blocks * block);
  if (quotient.count < blocks * block) {
    _bn_clear(&quotient);
    _bn_divmod_knuth(q, r, a, b);
    return;
  }
  memset(quotient.d, 0, quotient.count * sizeof(*quotient.d));
  for (size_t bi = blocks; bi-- > 0;) {
    _bn_t chunk = {0}, partial = {0}, digit = {0}, next_rem = {0};
    if (!_bn_limb_slice(&chunk, a, bi * block, block) ||
        !_bn_join_limbs(&partial, &rem, &chunk, block)) {
      _bn_clear(&chunk); _bn_clear(&partial); _bn_clear(&digit);
      _bn_clear(&next_rem); _bn_clear(&rem); _bn_clear(&quotient);
      _bn_divmod_knuth(q, r, a, b);
      return;
    }
    _bn_divmod_knuth(&digit, &next_rem, &partial, b);
    if (digit.count > block) {
      _bn_clear(&chunk); _bn_clear(&partial); _bn_clear(&digit);
      _bn_clear(&next_rem); _bn_clear(&rem); _bn_clear(&quotient);
      _bn_divmod_knuth(q, r, a, b);
      return;
    }
    if (digit.count)
      memcpy(quotient.d + bi * block, digit.d,
             digit.count * sizeof(*quotient.d));
    _bn_clear(&rem);
    rem = next_rem;
    _bn_clear(&chunk); _bn_clear(&partial); _bn_clear(&digit);
  }
  quotient.sign = 1;
  _bn_trim(&quotient);
  _bn_copy(q, &quotient);
  _bn_copy(r, &rem);
  _bn_clear(&quotient);
  _bn_clear(&rem);
}

static void _bn_divmod_unsigned(_bn_t *q, _bn_t *r, const _bn_t *a,
                                const _bn_t *b) {
  if (b->count >= BURNIKEL_ZIEGLER_THRESHOLD &&
      a->count >= b->count + BURNIKEL_ZIEGLER_THRESHOLD)
    _bn_divmod_burnikel_ziegler(q, r, a, b);
  else
    _bn_divmod_knuth(q, r, a, b);
}

/*
 * Truncated division: q = trunc(a/b), r = a - q*b, |r| < |b|
 */
static void _bn_tdiv_qr(_bn_t *q, _bn_t *r, const _bn_t *a,
                         const _bn_t *b) {
  _bn_t abs_a = {0};
  _bn_t abs_b = {0};
  _bn_t rq = {0};
  _bn_t rr = {0};
  _bn_abs(&abs_a, a);
  _bn_abs(&abs_b, b);
  _bn_init(&rq);
  _bn_init(&rr);
  _bn_divmod_unsigned(&rq, &rr, &abs_a, &abs_b);
  /*
   * quotient sign
   */
  if (_bn_is_zero(&rq))
    rq.sign = 0;
  else
    rq.sign = (a->sign == b->sign || a->sign == 0 || b->sign == 0) ? 1 : -1;
  /*
   * remainder sign follows dividend
   */
  rr.sign = _bn_is_zero(&rr) ? 0 : a->sign;
  _bn_copy(q, &rq);
  _bn_copy(r, &rr);
  _bn_clear(&abs_a);
  _bn_clear(&abs_b);
  _bn_clear(&rq);
  _bn_clear(&rr);
}

/*
 * Floor division: q = floor(a/b), r = a - q*b, 0 <= r < |b|
 */
static void _bn_fdiv_qr(_bn_t *q, _bn_t *r, const _bn_t *a,
                         const _bn_t *b) {
  _bn_t abs_a = {0};
  _bn_t abs_b = {0};
  _bn_t rq = {0};
  _bn_t rr = {0};
  _bn_abs(&abs_a, a);
  _bn_abs(&abs_b, b);
  _bn_init(&rq);
  _bn_init(&rr);
  _bn_divmod_unsigned(&rq, &rr, &abs_a, &abs_b);
  /*
   * Floor adjust: if signs differ and r != 0, q -= 1, r += |b|
   */
  int need_adj = (!_bn_is_zero(&rr)) &&
                 ((a->sign < 0 && b->sign > 0) ||
                  (a->sign > 0 && b->sign < 0));
  if (need_adj) {
    _bn_t one = {0};
    _bn_set_i64(&one, 1);
    _bn_t tq = {0};
    _bn_sub(&tq, &rq, &one);
    _bn_clear(&rq);
    _bn_copy(&rq, &tq);
    _bn_clear(&tq);
    _bn_clear(&one);
    _bn_t tr2 = {0};
    _bn_add(&tr2, &rr, &abs_b);
    _bn_clear(&rr);
    _bn_copy(&rr, &tr2);
    _bn_clear(&tr2);
  }
  if (_bn_is_zero(&rq))
    rq.sign = 0;
  else
    rq.sign = (a->sign == b->sign || a->sign == 0 || b->sign == 0) ? 1 : -1;
  rr.sign = _bn_is_zero(&rr) ? 0 : 1;
  _bn_copy(q, &rq);
  _bn_copy(r, &rr);
  _bn_clear(&abs_a);
  _bn_clear(&abs_b);
  _bn_clear(&rq);
  _bn_clear(&rr);
}


/*
 * In-place left shift by 1 bit (safe).
 */
static void _bn_shl1(_bn_t *a) {
  if (_bn_is_zero(a))
    return;
  size_t n = a->count;
  uint64_t carry = 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t next = a->d[i] >> 63;
    a->d[i] = (a->d[i] << 1) | carry;
    carry = next;
  }
  if (carry) {
    _bn_grow(a, n + 1);
    if (a->count >= n + 1)
      a->d[n] = carry;
    a->count = n + 1;
  }
}

/*
 * Left shift by arbitrary bits
 */
static void _bn_shl(_bn_t *r, const _bn_t *a, size_t bits) {
  if (_bn_is_zero(a) || bits == 0) {
    _bn_copy(r, a);
    return;
  }
  _bn_t src = {0};
  const _bn_t *ap = a;
  if (r == a) {
    _bn_copy(&src, a);
    ap = &src;
  }
  size_t words = bits / 64;
  size_t remain = bits % 64;
  size_t n = ap->count + words + (remain > 0 ? 1 : 0);
  _bn_grow(r, n);
  memset(r->d, 0, n * sizeof(uint64_t));
  if (remain > 0) {
    uint64_t carry = 0;
    for (size_t i = 0; i < ap->count; i++) {
      r->d[i + words] = (ap->d[i] << remain) | carry;
      carry = ap->d[i] >> (64 - remain);
    }
    if (carry)
      r->d[ap->count + words] = carry;
  } else {
    for (size_t i = 0; i < ap->count; i++)
      r->d[i + words] = ap->d[i];
  }
  r->count = n;
  r->sign = ap->sign;
  _bn_trim(r);
  _bn_clear(&src);
}
/*
 * Right shift (unsigned floor division by 2^bits)
 */
static void _bn_shr(_bn_t *r, const _bn_t *a, size_t bits) {
  if (_bn_is_zero(a) || bits == 0) {
    _bn_copy(r, a);
    return;
  }
  size_t words = bits / 64;
  size_t remain = bits % 64;
  if (words >= a->count) {
    _bn_set_zero(r);
    return;
  }
  size_t n = a->count - words;
  _bn_grow(r, n);
  if (remain > 0) {
    uint64_t carry = 0;
    for (size_t i = n; i > 0; i--) {
      uint64_t high = a->d[i - 1 + words];
      r->d[i - 1] = (high >> remain) | carry;
      carry = high << (64 - remain);
    }
  } else {
    for (size_t i = 0; i < n; i++)
      r->d[i] = a->d[i + words];
  }
  r->count = n;
  r->sign = a->sign;
  _bn_trim(r);
}

static size_t _bn_bitlen(const _bn_t *a) {
  if (_bn_is_zero(a))
    return 0;
  return (a->count - 1) * 64 + (size_t)(64 - __builtin_clzll(a->d[a->count - 1]));
}
static size_t _bn_popcount(const _bn_t *a) {
  size_t r = 0;
  for (size_t i = 0; i < a->count; i++)
    r += (size_t)__builtin_popcountll(a->d[i]);
  return r;
}

/*
 * Two's complement bitwise AND
 */
static void _bn_and(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  size_t n = a->count > b->count ? a->count : b->count;
  _bn_grow(r, n);
  int aneg = a->sign < 0, bneg = b->sign < 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t av = i < a->count ? a->d[i] : (aneg ? ~(uint64_t)0 : 0);
    uint64_t bv = i < b->count ? b->d[i] : (bneg ? ~(uint64_t)0 : 0);
    r->d[i] = av & bv;
  }
  r->count = n;
  r->sign = (aneg && bneg) ? -1 : 1;
  _bn_trim(r);
}
/*
 * Two's complement bitwise OR
 */
static void _bn_or(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  size_t n = a->count > b->count ? a->count : b->count;
  _bn_grow(r, n);
  int aneg = a->sign < 0, bneg = b->sign < 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t av = i < a->count ? a->d[i] : (aneg ? ~(uint64_t)0 : 0);
    uint64_t bv = i < b->count ? b->d[i] : (bneg ? ~(uint64_t)0 : 0);
    r->d[i] = av | bv;
  }
  r->count = n;
  r->sign = (aneg || bneg) ? -1 : 1;
  _bn_trim(r);
}
/*
 * Two's complement bitwise XOR
 */
static void _bn_xor(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  size_t n = a->count > b->count ? a->count : b->count;
  _bn_grow(r, n);
  int aneg = a->sign < 0, bneg = b->sign < 0;
  for (size_t i = 0; i < n; i++) {
    uint64_t av = i < a->count ? a->d[i] : (aneg ? ~(uint64_t)0 : 0);
    uint64_t bv = i < b->count ? b->d[i] : (bneg ? ~(uint64_t)0 : 0);
    r->d[i] = av ^ bv;
  }
  r->count = n;
  r->sign = (aneg != bneg) ? -1 : 1;
  _bn_trim(r);
}
/*
 * Bitwise complement: ~a = -(a+1)
 */
static void _bn_com(_bn_t *r, const _bn_t *a) {
  _bn_t one = {0};
  _bn_t neg_a = {0};
  _bn_set_i64(&one, 1);
  _bn_neg(&neg_a, a);
  _bn_sub(r, &neg_a, &one);
  _bn_clear(&one);
  _bn_clear(&neg_a);
}

/*
 * GCD (always returns non-negative)
 */

/*
 * GCD (always returns non-negative)
 */
static size_t _bn_ctz_mag(const _bn_t *a) {
  size_t bits = 0;
  for (size_t i = 0; i < a->count; ++i) {
    if (a->d[i] != 0)
      return bits + (size_t)__builtin_ctzll(a->d[i]);
    bits += 64;
  }
  return bits;
}

/*
 * Stein's binary GCD: shifts and subtraction replace repeated division.
 */
static void _bn_gcd(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_t u = {0}, v = {0};
  _bn_abs(&u, a);
  _bn_abs(&v, b);
  if (_bn_is_zero(&u)) {
    _bn_copy(r, &v);
    _bn_clear(&u); _bn_clear(&v);
    return;
  }
  if (_bn_is_zero(&v)) {
    _bn_copy(r, &u);
    _bn_clear(&u); _bn_clear(&v);
    return;
  }
  size_t uz = _bn_ctz_mag(&u), vz = _bn_ctz_mag(&v);
  size_t common = uz < vz ? uz : vz;
  _bn_shr(&u, &u, uz);
  _bn_shr(&v, &v, vz);
  while (!_bn_is_zero(&v)) {
    if (_bn_cmp_mag(&u, &v) > 0) {
      _bn_t swap = u; u = v; v = swap;
    }
    _bn_t diff = {0};
    _bn_sub_mag(&diff, &v, &u);
    _bn_clear(&v);
    v = diff;
    if (!_bn_is_zero(&v))
      _bn_shr(&v, &v, _bn_ctz_mag(&v));
  }
  _bn_shl(r, &u, common);
  r->sign = _bn_is_zero(r) ? 0 : 1;
  _bn_clear(&u); _bn_clear(&v);
}

/*
 * String conversion: base 10
 */
static char *_bn_to_str(const _bn_t *a) {
  if (_bn_is_zero(a)) {
    char *s = (char *)malloc(2);
    s[0] = '0';
    s[1] = '\0';
    return s;
  }
  _bn_t tmp = {0};
  _bn_copy(&tmp, a);
  tmp.sign = 1;
  char *buf = (char *)malloc(256);
  size_t pos = 0;
  while (!_bn_is_zero(&tmp)) {
    _bn_t rq = {0};
    _bn_t rr = {0};
    _bn_init(&rq);
    _bn_init(&rr);
    /*
     * divide by 10
     */
    {
      _bn_t ten = {0};
      _bn_set_i64(&ten, 10);
      _bn_divmod_unsigned(&rq, &rr, &tmp, &ten);
      _bn_clear(&ten);
    }
    uint64_t digit = (rr.count > 0) ? rr.d[0] : 0;
    if (pos >= 255)
      buf = (char *)realloc(buf, pos + 256);
    buf[pos++] = (char)('0' + digit);
    _bn_clear(&tmp);
    _bn_copy(&tmp, &rq);
    _bn_clear(&rq);
    _bn_clear(&rr);
  }
  _bn_clear(&tmp);
  buf[pos] = '\0';
  for (size_t i = 0; i < pos / 2; i++) {
    char t = buf[i];
    buf[i] = buf[pos - 1 - i];
    buf[pos - 1 - i] = t;
  }
  if (a->sign < 0) {
    memmove(buf + 1, buf, pos + 1);
    buf[0] = '-';
  }
  return buf;
}

/*
 * Parse decimal string
 */
static void _bn_from_str(_bn_t *r, const char *s) {
  _bn_set_zero(r);
  if (!s || !*s)
    return;
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s == '0')
    s++;
  if (!*s) {
    _bn_set_zero(r);
    return;
  }
  _bn_t ten = {0};
  _bn_set_i64(&ten, 10);
  while (*s) {
    if (*s < '0' || *s > '9')
      break;
    int digit = *s - '0';
    /*
     * r = r * 10 + digit
     */
    _bn_t tmp = {0};
    _bn_mul(&tmp, r, &ten);
    if (digit) {
      _bn_t dv = {0};
      _bn_set_i64(&dv, digit);
      _bn_t sum = {0};
      _bn_add(&sum, &tmp, &dv);
      _bn_clear(&tmp);
      _bn_clear(&dv);
      _bn_clear(r);
      _bn_copy(r, &sum);
      _bn_clear(&sum);
    } else {
      _bn_clear(r);
      _bn_copy(r, &tmp);
      _bn_clear(&tmp);
    }
    s++;
  }
  _bn_clear(&ten);
  if (neg && !_bn_is_zero(r))
    r->sign = -1;
  _bn_trim(r);
}

/*
 * Convert native to tagged int64 (small int or heap BigInt)
 */
static int64_t _bi_from_native(_bn_t *a) {
  _bn_trim(a);
  if (_bn_is_zero(a))
    return rt_tag_v(0);
  if (a->count == 1) {
    uint64_t mag = a->d[0];
    if (a->sign >= 0 && mag <= (uint64_t)NY_SMALL_INT_MAX)
      return rt_tag_v((int64_t)mag);
    if (a->sign < 0 && mag <= (uint64_t)NY_SMALL_INT_MAX + 1u)
      return rt_tag_v(-((int64_t)mag));
  }
  int64_t p = _bi_alloc(a->count);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p + 0) =
      rt_tag_v((int64_t)a->sign);
  memcpy((void *)((char *)p + 16), a->d, a->count * sizeof(uint64_t));
  return p;
}

/*
 * Convert tagged int64 to native bigint
 */
static void _bi_val_to_native(int64_t v, _bn_t *out) {
  if (is_int(v)) {
    _bn_set_i64(out, rt_untag_v(v));
    return;
  }
  if (!is_ptr(v)) {
    _bn_set_zero(out);
    return;
  }
  int64_t tag = *(int64_t *)(uintptr_t)((char *)v - 8);
  if (tag != TAG_BIGINT) {
    _bn_set_zero(out);
    return;
  }
  int64_t sign = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 0));
  int64_t words = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 8));
  if (words <= 0) {
    _bn_set_zero(out);
    return;
  }
  uint64_t *nd = (uint64_t *)malloc((size_t)words * sizeof(uint64_t));
  if (!nd) {
    _bn_set_zero(out);
    return;
  }
  memcpy(nd, (const void *)((char *)v + 16),
         (size_t)words * sizeof(uint64_t));
  free(out->d);
  out->d = nd;
  out->sign = (int)sign;
  out->count = (size_t)words;
}

/*
 * Integer square root (floor) via Newton's method
 */
static void _bn_isqrt(_bn_t *r, const _bn_t *a) {
  if (_bn_is_zero(a) || a->sign < 0) {
    _bn_set_zero(r);
    return;
  }
  /*
   * Initial guess: 2^(bitlen/2)
   */
  size_t bits = _bn_bitlen(a);
  _bn_set_i64(r, 1);
  if (bits > 1)
    _bn_shl(r, r, bits / 2);
  for (int iter = 0; iter < (int)(bits + 2); iter++) {
    /*
     * q = a / r
     */
    _bn_t rq = {0};
    _bn_t rr = {0};
    _bn_init(&rq);
    _bn_init(&rr);
    _bn_divmod_unsigned(&rq, &rr, a, r);
    _bn_clear(&rr);
    /*
     * new_r = (r + q) / 2
     */
    _bn_t sum = {0};
    _bn_add(&sum, r, &rq);
    _bn_clear(&rq);
    _bn_shr(r, &sum, 1);
    _bn_clear(&sum);
    _bn_trim(r);
  }
  /*
   * Newton converges to floor(sqrt(a)) from below, check result
   */
  _bn_t sq = {0};
  _bn_mul(&sq, r, r);
  /*
   * if r*r > a, decrement
   */
  while (_bn_cmp(&sq, a) > 0) {
    _bn_t one = {0};
    _bn_set_i64(&one, 1);
    _bn_t tmp = {0};
    _bn_sub(&tmp, r, &one);
    _bn_clear(r);
    _bn_copy(r, &tmp);
    _bn_clear(&tmp);
    _bn_clear(&one);
    _bn_clear(&sq);
    _bn_mul(&sq, r, r);
  }
  _bn_clear(&sq);
}

/*
 * Integer k-th root (floor) via Newton's method
 */
static void _bn_iroot(_bn_t *r, const _bn_t *a, unsigned long k) {
  if (k == 0 || _bn_is_zero(a)) {
    _bn_set_zero(r);
    return;
  }
  if (k == 1) {
    _bn_copy(r, a);
    return;
  }
  if (k == 2) {
    _bn_isqrt(r, a);
    return;
  }
  size_t bits = _bn_bitlen(a);
  _bn_set_i64(r, 1);
  if (bits > k)
    _bn_shl(r, r, (bits + k - 1) / k);
  for (int iter = 0; iter < (int)(bits / k + 10); iter++) {
    /*
     * Compute r^k and r^(k-1)
     */
    _bn_t rk = {0};
    _bn_t rk1 = {0};
    _bn_set_i64(&rk, 1);
    for (unsigned long i = 0; i < k; i++) {
      _bn_t tmp = {0};
      _bn_mul(&tmp, &rk, r);
      _bn_clear(&rk);
      _bn_copy(&rk, &tmp);
      _bn_clear(&tmp);
    }
    _bn_set_i64(&rk1, 1);
    for (unsigned long i = 1; i < k; i++) {
      _bn_t tmp = {0};
      _bn_mul(&tmp, &rk1, r);
      _bn_clear(&rk1);
      _bn_copy(&rk1, &tmp);
      _bn_clear(&tmp);
    }
    /*
     * a / r^(k-1)
     */
    _bn_t q = {0};
    _bn_t rem = {0};
    _bn_init(&q);
    _bn_init(&rem);
    _bn_divmod_unsigned(&q, &rem, a, &rk1);
    _bn_clear(&rem);
    /*
     * new_r = ((k-1)*r + a/r^(k-1)) / k
     */
    _bn_t km1 = {0};
    _bn_set_i64(&km1, (int64_t)(k - 1));
    _bn_t t1 = {0};
    _bn_mul(&t1, r, &km1); /* (k-1)*r */
    _bn_clear(&km1);
    _bn_t sum = {0};
    _bn_add(&sum, &t1, &q);
    _bn_clear(&t1);
    _bn_clear(&q);
    _bn_clear(&rk);
    _bn_clear(&rk1);
    /*
     * divide by k
     */
    {
      _bn_t kbn = {0};
      _bn_set_i64(&kbn, (int64_t)k);
      _bn_t rem2 = {0};
      _bn_init(&rem2);
      _bn_set_zero(&q);
      _bn_divmod_unsigned(&q, &rem2, &sum, &kbn);
      _bn_clear(&rem2);
      _bn_clear(&kbn);
    }
    _bn_clear(&sum);
    q.sign = 1;
    _bn_trim(&q);
    if (_bn_cmp(&q, r) >= 0) {
      _bn_clear(&q);
      return;
    }
    _bn_clear(r);
    _bn_copy(r, &q);
    _bn_clear(&q);
  }
}

/*
 * Perfect square test
 */
static bool _bn_perfect_square_p(const _bn_t *a) {
  if (_bn_is_zero(a))
    return true;
  if (a->sign < 0)
    return false;
  _bn_t root = {0};
  _bn_isqrt(&root, a);
  _bn_t sq = {0};
  _bn_mul(&sq, &root, &root);
  bool ok = (_bn_cmp(&sq, a) == 0);
  _bn_clear(&root);
  _bn_clear(&sq);
  return ok;
}

/*
 * Modular inverse via extended Euclidean (unsigned)
 */
static bool _bn_modinv(_bn_t *r, const _bn_t *a, const _bn_t *m) {
  _bn_t s0 = {0};
  _bn_t s1 = {0};
  _bn_t r0 = {0};
  _bn_t r1 = {0};
  _bn_set_i64(&s0, 1);
  _bn_set_i64(&s1, 0);
  _bn_copy(&r0, a);
  _bn_trim(&r0);
  r0.sign = r0.sign == 0 ? 0 : 1;
  _bn_copy(&r1, m);
  _bn_trim(&r1);

  while (!_bn_is_zero(&r1)) {
    _bn_t q = {0};
    _bn_t rr = {0};
    _bn_init(&q);
    _bn_init(&rr);
    _bn_divmod_unsigned(&q, &rr, &r0, &r1);
    _bn_clear(&r0);
    _bn_copy(&r0, &r1);
    _bn_clear(&r1);
    _bn_copy(&r1, &rr);
    _bn_clear(&rr);
    _bn_t prod = {0};
    _bn_mul(&prod, &q, &s1);
    _bn_clear(&q);
    _bn_t ns = {0};
    _bn_sub(&ns, &s0, &prod);
    _bn_clear(&prod);
    _bn_clear(&s0);
    _bn_copy(&s0, &s1);
    _bn_clear(&s1);
    _bn_copy(&s1, &ns);
    _bn_clear(&ns);
  }
  /*
   * r0 is GCD
   */
  _bn_t one = {0};
  _bn_set_i64(&one, 1);
  bool ok = (_bn_cmp(&r0, &one) == 0);
  _bn_clear(&one);
  _bn_clear(&r0);
  _bn_clear(&r1);
  if (!ok) {
    _bn_set_zero(r);
    _bn_clear(&s0);
    _bn_clear(&s1);
    return false;
  }
  _bn_copy(r, &s0);
  r->sign = _bn_is_zero(r) ? 0 : 1;
  /*
   * Ensure result is in [0, |m|)
   */
  if (r->sign < 0) {
    _bn_t abs_m = {0};
    _bn_abs(&abs_m, m);
    _bn_t tmp = {0};
    _bn_add(&tmp, r, &abs_m);
    _bn_clear(r);
    _bn_copy(r, &tmp);
    _bn_clear(&tmp);
    _bn_clear(&abs_m);
    r->sign = _bn_is_zero(r) ? 0 : 1;
  }
  _bn_clear(&s0);
  _bn_clear(&s1);
  return ok;
}

#ifndef BARRETT_REDUCTION_THRESHOLD
#define BARRETT_REDUCTION_THRESHOLD 8u
#endif

typedef struct {
  _bn_t modulus;
  _bn_t mu;
  size_t limbs;
  bool ready;
} _bn_barrett_t;

static void _bn_barrett_clear(_bn_barrett_t *ctx) {
  if (!ctx)
    return;
  _bn_clear(&ctx->modulus);
  _bn_clear(&ctx->mu);
  memset(ctx, 0, sizeof(*ctx));
}

/*
 * Precompute mu=floor(B^(2k)/m), B=2^64, for one powmod modulus.
 */
static bool _bn_barrett_init(_bn_barrett_t *ctx, const _bn_t *mod) {
  if (!ctx || !mod || mod->count < BARRETT_REDUCTION_THRESHOLD)
    return false;
  memset(ctx, 0, sizeof(*ctx));
  _bn_abs(&ctx->modulus, mod);
  ctx->limbs = ctx->modulus.count;
  _bn_t power = {0}, rem = {0};
  _bn_grow(&power, 2 * ctx->limbs + 1);
  if (power.count < 2 * ctx->limbs + 1) {
    _bn_clear(&power);
    _bn_barrett_clear(ctx);
    return false;
  }
  memset(power.d, 0, power.count * sizeof(*power.d));
  power.d[2 * ctx->limbs] = 1;
  power.sign = 1;
  _bn_divmod_unsigned(&ctx->mu, &rem, &power, &ctx->modulus);
  _bn_clear(&power);
  _bn_clear(&rem);
  ctx->ready = !_bn_is_zero(&ctx->mu);
  if (!ctx->ready)
    _bn_barrett_clear(ctx);
  return ctx->ready;
}

static bool _bn_low_limbs(_bn_t *out, const _bn_t *a, size_t count) {
  return _bn_limb_slice(out, a, 0, count);
}

/*
 * Barrett reduction for nonnegative x < B^(2k).
 */
static bool _bn_barrett_reduce(_bn_t *out, const _bn_t *x,
                               const _bn_barrett_t *ctx) {
  if (!out || !x || !ctx || !ctx->ready || x->sign < 0 ||
      x->count > 2 * ctx->limbs)
    return false;
  if (_bn_cmp_mag(x, &ctx->modulus) < 0) {
    _bn_copy(out, x);
    out->sign = _bn_is_zero(out) ? 0 : 1;
    return true;
  }
  size_t k = ctx->limbs;
  _bn_t q1 = {0}, q2 = {0}, q3 = {0};
  _bn_t r1 = {0}, product = {0}, r2 = {0}, reduced = {0};
  if (!_bn_limb_slice(&q1, x, k - 1, x->count) ||
      !_bn_low_limbs(&r1, x, k + 1))
    goto fail;
  _bn_mul(&q2, &q1, &ctx->mu);
  if (!_bn_limb_slice(&q3, &q2, k + 1, q2.count))
    goto fail;
  _bn_mul(&product, &q3, &ctx->modulus);
  if (!_bn_low_limbs(&r2, &product, k + 1))
    goto fail;
  if (_bn_cmp_mag(&r1, &r2) >= 0) {
    _bn_sub_mag(&reduced, &r1, &r2);
  } else {
    _bn_t base = {0}, sum = {0};
    _bn_grow(&base, k + 2);
    if (base.count < k + 2) {
      _bn_clear(&base);
      goto fail;
    }
    memset(base.d, 0, base.count * sizeof(*base.d));
    base.d[k + 1] = 1;
    base.sign = 1;
    _bn_add_mag(&sum, &r1, &base);
    _bn_sub_mag(&reduced, &sum, &r2);
    _bn_clear(&base);
    _bn_clear(&sum);
  }
  while (_bn_cmp_mag(&reduced, &ctx->modulus) >= 0) {
    _bn_t next = {0};
    _bn_sub_mag(&next, &reduced, &ctx->modulus);
    _bn_clear(&reduced);
    reduced = next;
  }
  _bn_copy(out, &reduced);
  out->sign = _bn_is_zero(out) ? 0 : 1;
  _bn_clear(&q1); _bn_clear(&q2); _bn_clear(&q3);
  _bn_clear(&r1); _bn_clear(&product); _bn_clear(&r2);
  _bn_clear(&reduced);
  return true;
fail:
  _bn_clear(&q1); _bn_clear(&q2); _bn_clear(&q3);
  _bn_clear(&r1); _bn_clear(&product); _bn_clear(&r2);
  _bn_clear(&reduced);
  return false;
}

static void _bn_reduce_product(_bn_t *out, const _bn_t *product,
                               const _bn_t *mod,
                               const _bn_barrett_t *barrett) {
  if (barrett && _bn_barrett_reduce(out, product, barrett))
    return;
  _bn_t q = {0};
  _bn_divmod_unsigned(&q, out, product, mod);
  _bn_clear(&q);
}

/*
 * Modular exponentiation. Large repeated products use one modulus-specific
 * Barrett reciprocal; unsupported sizes retain exact division reduction.
 */
static void _bn_powmod(_bn_t *r, const _bn_t *base, const _bn_t *exp,
                       const _bn_t *mod) {
  if (_bn_is_zero(mod)) {
    _bn_set_zero(r);
    return;
  }
  _bn_t result = {0};
  _bn_set_i64(&result, 1);
  _bn_barrett_t barrett = {0};
  bool have_barrett = _bn_barrett_init(&barrett, mod);
  /*
   * Reduce base mod mod
   */
  _bn_t b = {0};
  {
    _bn_t rq = {0};
    _bn_t rr = {0};
    _bn_init(&rq);
    _bn_init(&rr);
    _bn_divmod_unsigned(&rq, &rr, base, mod);
    _bn_clear(&rq);
    _bn_copy(&b, &rr);
    _bn_clear(&rr);
  }
  b.sign = _bn_is_zero(&b) ? 0 : 1;
  size_t expbits = _bn_bitlen(exp);
  for (size_t i = 0; i < expbits; i++) {
    size_t word = i / 64;
    size_t bit = i % 64;
    if (word < exp->count && (exp->d[word] >> bit & 1)) {
      _bn_t prod = {0};
      _bn_mul(&prod, &result, &b);
      _bn_t reduced = {0};
      _bn_reduce_product(&reduced, &prod, mod,
                         have_barrett ? &barrett : NULL);
      _bn_clear(&result);
      _bn_copy(&result, &reduced);
      result.sign = _bn_is_zero(&result) ? 0 : 1;
      _bn_clear(&reduced);
      _bn_clear(&prod);
    }
    /*
     * square b
     */
    _bn_t sq = {0};
    _bn_mul(&sq, &b, &b);
    _bn_t reduced = {0};
    _bn_reduce_product(&reduced, &sq, mod,
                       have_barrett ? &barrett : NULL);
    _bn_clear(&b);
    _bn_copy(&b, &reduced);
    b.sign = _bn_is_zero(&b) ? 0 : 1;
    _bn_clear(&reduced);
    _bn_clear(&sq);
  }
  _bn_copy(r, &result);
  _bn_clear(&result);
  _bn_clear(&b);
  _bn_barrett_clear(&barrett);
}

/*
 * xgcd: g = gcd(a,b), g = ax + by
 */
static void _bn_xgcd(_bn_t *g, _bn_t *x, _bn_t *y, const _bn_t *a,
                      const _bn_t *b) {
  _bn_t old_r = {0};
  _bn_t new_r = {0};
  _bn_t old_s = {0};
  _bn_t new_s = {0};
  _bn_t old_t = {0};
  _bn_t new_t = {0};
  _bn_copy(&old_r, a);
  _bn_copy(&new_r, b);
  _bn_set_i64(&old_s, 1);
  _bn_set_i64(&new_s, 0);
  _bn_set_i64(&old_t, 0);
  _bn_set_i64(&new_t, 1);
  while (!_bn_is_zero(&new_r)) {
    _bn_t q = {0};
    _bn_t rr = {0};
    _bn_init(&q);
    _bn_init(&rr);
    _bn_divmod_unsigned(&q, &rr, &old_r, &new_r);
    _bn_clear(&old_r);
    _bn_copy(&old_r, &new_r);
    _bn_clear(&new_r);
    _bn_copy(&new_r, &rr);
    _bn_clear(&rr);
    _bn_t prod = {0};
    _bn_t ns = {0};
    _bn_mul(&prod, &q, &new_s);
    _bn_sub(&ns, &old_s, &prod);
    _bn_clear(&prod);
    _bn_clear(&old_s);
    _bn_copy(&old_s, &new_s);
    _bn_clear(&new_s);
    _bn_copy(&new_s, &ns);
    _bn_clear(&ns);
    _bn_t prod2 = {0};
    _bn_t nt = {0};
    _bn_mul(&prod2, &q, &new_t);
    _bn_sub(&nt, &old_t, &prod2);
    _bn_clear(&prod2);
    _bn_clear(&old_t);
    _bn_copy(&old_t, &new_t);
    _bn_clear(&new_t);
    _bn_copy(&new_t, &nt);
    _bn_clear(&nt);
    _bn_clear(&q);
  }
  _bn_copy(g, &old_r);
  g->sign = _bn_is_zero(g) ? 0 : 1;
  _bn_copy(x, &old_s);
  _bn_copy(y, &old_t);
  _bn_clear(&old_r);
  _bn_clear(&new_r);
  _bn_clear(&old_s);
  _bn_clear(&new_s);
  _bn_clear(&old_t);
  _bn_clear(&new_t);
}

/*
 * Jacobi symbol (a/n) for n > 0, odd
 */
static int _bn_jacobi_val(const _bn_t *a, const _bn_t *n) {
  _bn_t u = {0};
  _bn_t v = {0};
  _bn_copy(&u, a);
  _bn_trim(&u);
  if (_bn_is_zero(&u)) {
    _bn_clear(&u);
    return 0;
  }
  u.sign = 1;
  _bn_copy(&v, n);
  _bn_trim(&v);
  int result = 1;
  for (;;) {
    _bn_trim(&u);
    if (_bn_is_zero(&u))
      break;
    /*
     * Remove factors of 2 from u
     */
    while (u.count > 0 && (u.d[0] & 1) == 0) {
      _bn_shr(&u, &u, 1);
      int vmod8 = v.count > 0 ? (int)(v.d[0] & 7) : 0;
      if (vmod8 == 3 || vmod8 == 5)
        result = -result;
    }
    /*
     * Swap u, v
     */
    _bn_t tmp = {0};
    _bn_copy(&tmp, &u);
    _bn_clear(&u);
    _bn_copy(&u, &v);
    _bn_clear(&v);
    _bn_copy(&v, &tmp);
    _bn_clear(&tmp);
    int umod4 = u.count > 0 ? (int)(u.d[0] & 3) : 0;
    if (umod4 == 3) {
      int vmod4 = v.count > 0 ? (int)(v.d[0] & 3) : 0;
      if (vmod4 == 3)
        result = -result;
    }
    /*
     * u = u mod v
     */
    {
      _bn_t rq = {0};
      _bn_t rr = {0};
      _bn_init(&rq);
      _bn_init(&rr);
      _bn_divmod_unsigned(&rq, &rr, &u, &v);
      _bn_clear(&rq);
      _bn_clear(&u);
      _bn_copy(&u, &rr);
      u.sign = _bn_is_zero(&u) ? 0 : 1;
      _bn_clear(&rr);
    }
  }
  _bn_clear(&u);
  _bn_clear(&v);
  return result;
}

/*
 * GF(2) polynomial degree
 */
static long _bn_gf2_deg(const _bn_t *a) {
  if (_bn_is_zero(a))
    return -1;
  return (long)(_bn_bitlen(a) - 1);
}
/*
 * GF(2) reduce: out = a mod m
 */
static void _bn_gf2_mod(_bn_t *out, const _bn_t *a, const _bn_t *m) {
  _bn_copy(out, a);
  long m_deg = _bn_gf2_deg(m);
  if (m_deg < 0) {
    _bn_set_zero(out);
    return;
  }
  for (;;) {
    long a_deg = _bn_gf2_deg(out);
    if (a_deg < m_deg)
      break;
    _bn_t shifted = {0};
    _bn_shl(&shifted, m, (size_t)(a_deg - m_deg));
    _bn_xor(out, out, &shifted);
    _bn_clear(&shifted);
  }
}
/*
 * GF(2) carryless multiply mod m
 */
static void _bn_gf2_mulmod(_bn_t *out, const _bn_t *a, const _bn_t *b,
                           const _bn_t *m) {
  long m_deg = _bn_gf2_deg(m);
  if (m_deg < 0) {
    _bn_set_zero(out);
    return;
  }
  _bn_t va = {0};
  _bn_t vb = {0};
  _bn_gf2_mod(&va, a, m);
  _bn_copy(&vb, b);
  _bn_set_zero(out);
  while (!_bn_is_zero(&vb)) {
    if (vb.count > 0 && (vb.d[0] & 1))
      _bn_xor(out, out, &va);
    _bn_shr(&vb, &vb, 1);
    if (!_bn_is_zero(&vb)) {
      _bn_t shifted = {0};
      _bn_shl(&shifted, &va, 1);
      _bn_t reduced = {0};
      if (_bn_gf2_deg(&shifted) >= m_deg) {
        _bn_gf2_mod(&reduced, &shifted, m);
        _bn_copy(&va, &reduced);
        _bn_clear(&reduced);
      } else {
        _bn_copy(&va, &shifted);
      }
      _bn_clear(&shifted);
    }
  }
  _bn_clear(&va);
  _bn_clear(&vb);
}

/*
 * GF(2) polynomial division: a = q*b + r in GF(2)
 */
static void _bn_gf2_div_qr(_bn_t *q, _bn_t *r, const _bn_t *a,
                           const _bn_t *b) {
  _bn_set_zero(q);
  _bn_copy(r, a);
  long b_deg = _bn_gf2_deg(b);
  if (b_deg < 0)
    return;
  for (;;) {
    long r_deg = _bn_gf2_deg(r);
    if (r_deg < b_deg)
      break;
    size_t shift = (size_t)(r_deg - b_deg);
    _bn_t shifted = {0};
    _bn_shl(&shifted, b, shift);
    _bn_xor(r, r, &shifted);
    _bn_clear(&shifted);
    _bn_t one_at = {0};
    _bn_set_i64(&one_at, 1);
    _bn_shl(&one_at, &one_at, shift);
    _bn_xor(q, q, &one_at);
    _bn_clear(&one_at);
  }
}

/*
 * Convert big-endian bytes to native
 */
static void _bn_from_bytes_be(_bn_t *r, const uint8_t *bytes, size_t len) {
  _bn_set_zero(r);
  if (!bytes || len == 0)
    return;
  _bn_t two56 = {0};
  _bn_set_i64(&two56, 256);
  for (size_t i = 0; i < len; i++) {
    _bn_t tmp = {0};
    _bn_mul(&tmp, r, &two56);
    _bn_clear(r);
    _bn_copy(r, &tmp);
    _bn_clear(&tmp);
    _bn_t dv = {0};
    _bn_set_i64(&dv, (int64_t)bytes[i]);
    _bn_add(r, r, &dv);
    _bn_clear(&dv);
  }
  _bn_clear(&two56);
  r->sign = _bn_is_zero(r) ? 0 : 1;
}

/*
 * Convert to big-endian bytes
 */
static uint8_t *_bn_to_bytes_be(const _bn_t *a, size_t *count) {
  if (_bn_is_zero(a)) {
    *count = 1;
    uint8_t *buf = (uint8_t *)malloc(1);
    buf[0] = 0;
    return buf;
  }
  _bn_t tmp = {0};
  _bn_copy(&tmp, a);
  tmp.sign = 1;
  uint8_t *buf = (uint8_t *)malloc(256);
  size_t pos = 0;
  _bn_t two56 = {0};
  _bn_set_i64(&two56, 256);
  while (!_bn_is_zero(&tmp)) {
    _bn_t rq = {0};
    _bn_t rr = {0};
    _bn_init(&rq);
    _bn_init(&rr);
    _bn_divmod_unsigned(&rq, &rr, &tmp, &two56);
    uint64_t byte_val = (rr.count > 0) ? rr.d[0] : 0;
    if (pos >= 256)
      buf = (uint8_t *)realloc(buf, pos + 256);
    buf[pos++] = (uint8_t)byte_val;
    _bn_clear(&tmp);
    _bn_copy(&tmp, &rq);
    _bn_clear(&rq);
    _bn_clear(&rr);
  }
  _bn_clear(&tmp);
  _bn_clear(&two56);
  for (size_t i = 0; i < pos / 2; i++) {
    uint8_t t = buf[i];
    buf[i] = buf[pos - 1 - i];
    buf[pos - 1 - i] = t;
  }
  *count = pos;
  return buf;
}

static int64_t _bi_popcount_native(int64_t a) {
  if (is_int(a)) {
    int64_t v = rt_untag_v(a);
    return rt_tag_v((int64_t)__builtin_popcountll(
        v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v));
  }
  _bn_t n = {0};
  _bn_init(&n);
  _bi_val_to_native(a, &n);
  size_t r = _bn_popcount(&n);
  _bn_clear(&n);
  return rt_tag_v((int64_t)r);
}

#endif /* NYTRIX_USE_GMP */

/*
 * Fast-path helpers (shared by both backends)
 */

static inline bool _bi_read_limb2(int64_t v, int *sign, uint64_t limbs[2],
                                  size_t *words) {
  limbs[0] = 0;
  limbs[1] = 0;
  *words = 0;
  *sign = 0;
  if (is_int(v)) {
    int64_t raw = rt_untag_v(v);
    if (raw == 0)
      return true;
    *sign = raw < 0 ? -1 : 1;
    limbs[0] =
        raw < 0 ? (uint64_t)(-(raw + 1)) + 1u : (uint64_t)raw;
    *words = 1;
    return true;
  }
  if (!is_ptr(v))
    return false;
  int64_t tag = *(int64_t *)(uintptr_t)((char *)v - 8);
  if (tag != TAG_BIGINT)
    return false;
  int64_t s = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 0));
  int64_t w = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 8));
  if (w < 0 || w > 2)
    return false;
  *sign = s < 0 ? -1 : (s > 0 ? 1 : 0);
  *words = (size_t)w;
  const uint64_t *src = (const uint64_t *)((char *)v + 16);
  if (*words > 0)
    limbs[0] = src[0];
  if (*words > 1)
    limbs[1] = src[1];
  while (*words > 0 && limbs[*words - 1] == 0)
    (*words)--;
  if (*words == 0)
    *sign = 0;
  return true;
}

static inline bool _bi_is_zero_fast(int64_t v) {
  if (is_int(v))
    return rt_untag_v(v) == 0;
  if (!is_ptr(v))
    return false;
  int64_t tag = *(int64_t *)(uintptr_t)((char *)v - 8);
  if (tag != TAG_BIGINT)
    return false;
  int64_t sign = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 0));
  if (sign == 0)
    return true;
  int64_t words = rt_untag_v(*(int64_t *)(uintptr_t)((char *)v + 8));
  return words <= 0;
}

static inline size_t _bi_norm_words(uint64_t *limbs, size_t words) {
  while (words > 0 && limbs[words - 1] == 0)
    words--;
  return words;
}

static inline size_t _bi_norm_words_const(const uint64_t *limbs,
                                          size_t words) {
  while (words > 0 && limbs[words - 1] == 0)
    words--;
  return words;
}

static inline int _bi_cmp_mag(const uint64_t *a, size_t aw,
                               const uint64_t *b, size_t bw) {
  aw = _bi_norm_words_const(a, aw);
  bw = _bi_norm_words_const(b, bw);
  if (aw != bw)
    return aw > bw ? 1 : -1;
  while (aw > 0) {
    aw--;
    if (a[aw] != b[aw])
      return a[aw] > b[aw] ? 1 : -1;
  }
  return 0;
}

static int64_t _bi_from_limbs(int sign, const uint64_t *limbs,
                               size_t words) {
  words = _bi_norm_words_const(limbs, words);
  if (words == 0)
    sign = 0;
  int64_t p = _bi_alloc(words);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p + 0) =
      rt_tag_v((int64_t)(sign < 0 ? -1 : (sign > 0 ? 1 : 0)));
  if (words > 0)
    memcpy((void *)((char *)p + 16), limbs, words * sizeof(uint64_t));
  return p;
}

static inline int64_t _bi_from_limbs_or_small_int(int sign,
                                                    const uint64_t *limbs,
                                                    size_t words);

static inline int64_t _bi_from_i128_value(__int128 v, int compact) {
  if (compact && v >= (__int128)NY_SMALL_INT_MIN &&
      v <= (__int128)NY_SMALL_INT_MAX)
    return rt_tag_v((int64_t)v);
  int sign = v < 0 ? -1 : (v > 0 ? 1 : 0);
  unsigned __int128 mag =
      v < 0 ? (unsigned __int128)(-v) : (unsigned __int128)v;
  uint64_t limbs[2] = {(uint64_t)mag, (uint64_t)(mag >> 64)};
  size_t words = limbs[1] ? 2 : (limbs[0] ? 1 : 0);
  return compact ? _bi_from_limbs_or_small_int(sign, limbs, words)
                 : _bi_from_limbs(sign, limbs, words);
}

static inline bool _bi_try_submul_i64_int_pair(int64_t a, int64_t q_raw,
                                               int64_t b, int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  __int128 av = (__int128)rt_untag_v(a);
  __int128 bv = (__int128)rt_untag_v(b);
  *out = _bi_from_i128_value(av - (__int128)q_raw * bv, 1);
  return true;
}

static inline bool _bi_try_add_int_pair(int64_t a, int64_t b, int sub_b,
                                        int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  __int128 av = (__int128)rt_untag_v(a);
  __int128 bv = (__int128)rt_untag_v(b);
  *out = _bi_from_i128_value(sub_b ? av - bv : av + bv, 1);
  return true;
}

static inline bool _bi_try_i128_small_tag(__int128 v, int64_t *out) {
  if (v < (__int128)NY_SMALL_INT_MIN || v > (__int128)NY_SMALL_INT_MAX)
    return false;
  *out = rt_tag_v((int64_t)v);
  return true;
}

static inline bool _bi_try_submul_i64_int_pair_small(int64_t a,
                                                      int64_t q_raw,
                                                      int64_t b,
                                                      int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  return _bi_try_i128_small_tag(
      (__int128)rt_untag_v(a) -
          (__int128)q_raw * (__int128)rt_untag_v(b),
      out);
}

static inline bool _bi_try_add_int_pair_small(int64_t a, int64_t b,
                                              int sub_b, int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  __int128 av = (__int128)rt_untag_v(a);
  __int128 bv = (__int128)rt_untag_v(b);
  return _bi_try_i128_small_tag(sub_b ? av - bv : av + bv, out);
}

static inline int64_t _bi_from_limbs_or_small_int(int sign,
                                                    const uint64_t *limbs,
                                                    size_t words) {
  words = _bi_norm_words_const(limbs, words);
  if (words == 0)
    return rt_tag_v(0);
  if (words == 1) {
    uint64_t mag = limbs[0];
    if (sign >= 0 && mag <= (uint64_t)NY_SMALL_INT_MAX)
      return rt_tag_v((int64_t)mag);
    if (sign < 0 && mag <= (uint64_t)NY_SMALL_INT_MAX + 1u)
      return rt_tag_v(-((int64_t)mag));
  }
  return _bi_from_limbs(sign, limbs, words);
}

static inline size_t _bi_mag_add4(uint64_t out[4], const uint64_t *a,
                                  size_t aw, const uint64_t *b, size_t bw) {
  uint64_t carry = 0;
  size_t n = aw > bw ? aw : bw;
  for (size_t i = 0; i < n; i++) {
    __uint128_t s = (__uint128_t)(i < aw ? a[i] : 0) +
                    (i < bw ? b[i] : 0) + carry;
    out[i] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
  }
  if (carry && n < 4)
    out[n++] = carry;
  return _bi_norm_words(out, n);
}

static inline size_t _bi_mag_sub4(uint64_t out[4], const uint64_t *a,
                                  size_t aw, const uint64_t *b, size_t bw) {
  uint64_t borrow = 0;
  for (size_t i = 0; i < aw; i++) {
    uint64_t av = a[i];
    uint64_t bv = i < bw ? b[i] : 0;
    uint64_t sub = bv + borrow;
    uint64_t next_borrow = (borrow && sub == 0) || av < sub;
    out[i] = av - sub;
    borrow = next_borrow;
  }
  return _bi_norm_words(out, aw);
}

static inline bool _bi_try_add_limb2_kind(int64_t a, int64_t b, int sub_b,
                                           int compact, int64_t *out) {
  if (is_int(a) && is_int(b)) {
    if (compact)
      _bi_try_add_int_pair(a, b, sub_b, out);
    else {
      __int128 av = (__int128)rt_untag_v(a);
      __int128 bv = (__int128)rt_untag_v(b);
      *out = _bi_from_i128_value(sub_b ? av - bv : av + bv, 0);
    }
    return true;
  }
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t al[2], bl[2], rl[4] = {0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) ||
      !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (sub_b)
    bs = -bs;
  if (aw <= 1 && bw <= 1) {
    __int128 av = as < 0 ? -(__int128)al[0]
                         : (as > 0 ? (__int128)al[0] : 0);
    __int128 bv = bs < 0 ? -(__int128)bl[0]
                         : (bs > 0 ? (__int128)bl[0] : 0);
    *out = _bi_from_i128_value(av + bv, compact);
    return true;
  }
  int rs = 0;
  size_t rw = 0;
  if (as == 0) {
    rs = bs;
    rw = bw;
    memcpy(rl, bl, bw * sizeof(uint64_t));
  } else if (bs == 0) {
    rs = as;
    rw = aw;
    memcpy(rl, al, aw * sizeof(uint64_t));
  } else if (as == bs) {
    rs = as;
    rw = _bi_mag_add4(rl, al, aw, bl, bw);
  } else {
    int cmp = _bi_cmp_mag(al, aw, bl, bw);
    if (cmp == 0) {
      rs = 0;
      rw = 0;
    } else if (cmp > 0) {
      rs = as;
      rw = _bi_mag_sub4(rl, al, aw, bl, bw);
    } else {
      rs = bs;
      rw = _bi_mag_sub4(rl, bl, bw, al, aw);
    }
  }
  *out = compact ? _bi_from_limbs_or_small_int(rs, rl, rw)
                 : _bi_from_limbs(rs, rl, rw);
  return true;
}

static inline bool _bi_try_add_limb2(int64_t a, int64_t b, int sub_b,
                                     int64_t *out) {
  return _bi_try_add_limb2_kind(a, b, sub_b, 0, out);
}

static inline bool _bi_try_mul_limb2(int64_t a, int64_t b, int64_t *out) {
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t al[2], bl[2], rl[4] = {0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) ||
      !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (as == 0 || bs == 0) {
    *out = _bi_from_limbs(0, rl, 0);
    return true;
  }
  for (size_t i = 0; i < aw; i++) {
    __uint128_t carry = 0;
    for (size_t j = 0; j < bw; j++) {
      __uint128_t cur =
          (__uint128_t)al[i] * bl[j] + rl[i + j] + carry;
      rl[i + j] = (uint64_t)cur;
      carry = cur >> 64;
    }
    size_t k = i + bw;
    while (carry && k < 4) {
      __uint128_t cur = (__uint128_t)rl[k] + carry;
      rl[k] = (uint64_t)cur;
      carry = cur >> 64;
      k++;
    }
    if (carry)
      return false;
  }
  *out = _bi_from_limbs(as == bs ? 1 : -1, rl, 4);
  return true;
}

static inline bool _bi_try_submul_limb2(int64_t a, int64_t q, int64_t b,
                                        int64_t *out) {
  int as = 0, qs = 0, bs = 0;
  size_t aw = 0, qw = 0, bw = 0;
  uint64_t al[2], ql[2], bl[2], pl[4] = {0, 0, 0, 0},
           rl[5] = {0, 0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) ||
      !_bi_read_limb2(q, &qs, ql, &qw) ||
      !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (qs == 0 || bs == 0) {
    *out = _bi_from_limbs_or_small_int(as, al, aw);
    return true;
  }
  for (size_t i = 0; i < qw; i++) {
    __uint128_t carry = 0;
    for (size_t j = 0; j < bw; j++) {
      __uint128_t cur =
          (__uint128_t)ql[i] * bl[j] + pl[i + j] + carry;
      pl[i + j] = (uint64_t)cur;
      carry = cur >> 64;
    }
    size_t k = i + bw;
    while (carry && k < 4) {
      __uint128_t cur = (__uint128_t)pl[k] + carry;
      pl[k] = (uint64_t)cur;
      carry = cur >> 64;
      k++;
    }
    if (carry)
      return false;
  }
  size_t pw = _bi_norm_words(pl, 4);
  int ps = qs == bs ? 1 : -1;
  int rs = 0;
  size_t rw = 0;
  int rhs_sign = -ps;
  if (as == 0) {
    rs = rhs_sign;
    rw = pw;
    memcpy(rl, pl, pw * sizeof(uint64_t));
  } else if (pw == 0) {
    rs = as;
    rw = aw;
    memcpy(rl, al, aw * sizeof(uint64_t));
  } else if (as == rhs_sign) {
    uint64_t carry = 0;
    size_t n = aw > pw ? aw : pw;
    rs = as;
    for (size_t i = 0; i < n; i++) {
      __uint128_t s =
          (__uint128_t)(i < aw ? al[i] : 0) +
          (i < pw ? pl[i] : 0) + carry;
      rl[i] = (uint64_t)s;
      carry = (uint64_t)(s >> 64);
    }
    if (carry) {
      if (n >= 5)
        return false;
      rl[n++] = carry;
    }
    rw = _bi_norm_words(rl, n);
  } else {
    int cmp = _bi_cmp_mag(al, aw, pl, pw);
    if (cmp == 0) {
      rs = 0;
      rw = 0;
    } else {
      const uint64_t *hi = cmp > 0 ? al : pl;
      const uint64_t *lo = cmp > 0 ? pl : al;
      size_t hiw = cmp > 0 ? aw : pw;
      size_t low = cmp > 0 ? pw : aw;
      uint64_t borrow = 0;
      for (size_t i = 0; i < hiw; i++) {
        uint64_t av2 = hi[i];
        uint64_t bv2 = i < low ? lo[i] : 0;
        uint64_t sub = bv2 + borrow;
        uint64_t next_borrow = (borrow && sub == 0) || av2 < sub;
        rl[i] = av2 - sub;
        borrow = next_borrow;
      }
      rs = cmp > 0 ? as : rhs_sign;
      rw = _bi_norm_words(rl, hiw);
    }
  }
  *out = _bi_from_limbs_or_small_int(rs, rl, rw);
  return true;
}

static inline bool _bi_try_submul_i64_limb2(int64_t a, int64_t q_raw,
                                             int64_t b, int64_t *out) {
  if (is_int(a) && is_int(b)) {
    _bi_try_submul_i64_int_pair(a, q_raw, b, out);
    return true;
  }
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t al[2], bl[2], pl[4] = {0, 0, 0, 0},
           rl[5] = {0, 0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) ||
      !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (q_raw == 0 || bs == 0) {
    *out = _bi_from_limbs_or_small_int(as, al, aw);
    return true;
  }
  if (aw <= 1 && bw <= 1) {
    __int128 av = as < 0 ? -(__int128)al[0]
                         : (as > 0 ? (__int128)al[0] : 0);
    __int128 bv = bs < 0 ? -(__int128)bl[0]
                         : (bs > 0 ? (__int128)bl[0] : 0);
    *out = _bi_from_i128_value(av - (__int128)q_raw * bv, 1);
    return true;
  }
  int qs = q_raw < 0 ? -1 : 1;
  uint64_t qmag =
      q_raw < 0 ? (uint64_t)(-(q_raw + 1)) + 1u : (uint64_t)q_raw;
  __uint128_t carry = 0;
  for (size_t i = 0; i < bw; i++) {
    __uint128_t cur = (__uint128_t)qmag * bl[i] + carry;
    pl[i] = (uint64_t)cur;
    carry = cur >> 64;
  }
  size_t pw = bw;
  if (carry) {
    if (pw >= 4)
      return false;
    pl[pw++] = (uint64_t)carry;
  }
  pw = _bi_norm_words(pl, pw);
  int ps = qs == bs ? 1 : -1;
  int rs = 0;
  size_t rw = 0;
  int rhs_sign = -ps;
  if (as == 0) {
    rs = rhs_sign;
    rw = pw;
    memcpy(rl, pl, pw * sizeof(uint64_t));
  } else if (pw == 0) {
    rs = as;
    rw = aw;
    memcpy(rl, al, aw * sizeof(uint64_t));
  } else if (as == rhs_sign) {
    uint64_t add_carry = 0;
    size_t n = aw > pw ? aw : pw;
    rs = as;
    for (size_t i = 0; i < n; i++) {
      __uint128_t s =
          (__uint128_t)(i < aw ? al[i] : 0) +
          (i < pw ? pl[i] : 0) + add_carry;
      rl[i] = (uint64_t)s;
      add_carry = (uint64_t)(s >> 64);
    }
    if (add_carry) {
      if (n >= 5)
        return false;
      rl[n++] = add_carry;
    }
    rw = _bi_norm_words(rl, n);
  } else {
    int cmp = _bi_cmp_mag(al, aw, pl, pw);
    if (cmp == 0) {
      rs = 0;
      rw = 0;
    } else {
      const uint64_t *hi = cmp > 0 ? al : pl;
      const uint64_t *lo = cmp > 0 ? pl : al;
      size_t hiw = cmp > 0 ? aw : pw;
      size_t low = cmp > 0 ? pw : aw;
      uint64_t borrow = 0;
      for (size_t i = 0; i < hiw; i++) {
        uint64_t av2 = hi[i];
        uint64_t bv2 = i < low ? lo[i] : 0;
        uint64_t sub = bv2 + borrow;
        uint64_t next_borrow = (borrow && sub == 0) || av2 < sub;
        rl[i] = av2 - sub;
        borrow = next_borrow;
      }
      rs = cmp > 0 ? as : rhs_sign;
      rw = _bi_norm_words(rl, hiw);
    }
  }
  *out = _bi_from_limbs_or_small_int(rs, rl, rw);
  return true;
}

/*
 * Public API
 */

int64_t rt_bigint_add(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_add_limb2(a, b, 0, &fast))
    return fast;
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  mpz_add(mr, ma, mb);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_add(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_sub(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_add_limb2(a, b, 1, &fast))
    return fast;
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  mpz_sub(mr, ma, mb);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_sub(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_mul(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_mul_limb2(a, b, &fast))
    return fast;
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  mpz_mul(mr, ma, mb);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_mul(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_submul(int64_t a, int64_t q, int64_t b) {
  if (q == rt_tag_v(0) || _bi_is_zero_fast(b))
    return a;
  int64_t fast = 0;
  if (q == rt_tag_v(1) &&
      _bi_try_add_limb2_kind(a, b, 1, 1, &fast))
    return fast;
  if (q == rt_tag_v(-1) &&
      _bi_try_add_limb2_kind(a, b, 0, 1, &fast))
    return fast;
  if (is_int(q) &&
      _bi_try_submul_i64_limb2(a, rt_untag_v(q), b, &fast))
    return fast;
  if (_bi_try_submul_limb2(a, q, b, &fast))
    return fast;
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mq, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(q, mq);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  mpz_mul(mr, mq, mb);
  mpz_sub(mr, ma, mr);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mq);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nq, nb, nr;
  _bn_init(&na);
  _bn_init(&nq);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(q, &nq);
  _bi_val_to_native(b, &nb);
  _bn_t prod = {0};
  _bn_mul(&prod, &nq, &nb);
  _bn_sub(&nr, &na, &prod);
  _bn_clear(&prod);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nq);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_row_submul(int64_t row_k, int64_t row_j, int64_t q,
                             int64_t limit_v) {
  if (!is_ptr(row_k) || !is_ptr(row_j))
    return row_k;
  int64_t limit = is_int(limit_v) ? rt_untag_v(limit_v) : limit_v;
  if (limit < 0 || q == rt_tag_v(0))
    return row_k;
  int64_t *rk = (int64_t *)(uintptr_t)((char *)row_k + 16);
  const int64_t *rj =
      (const int64_t *)(uintptr_t)((char *)row_j + 16);
  if (is_int(q)) {
    int64_t q_raw = rt_untag_v(q);
    if (q_raw == 1 || q_raw == -1) {
      int sub_b = q_raw == 1;
      for (int64_t c = 0; c <= limit; c++) {
        int64_t b = rj[c];
        if (_bi_is_zero_fast(b))
          continue;
        int64_t fast = 0;
        rk[c] =
            (_bi_try_add_int_pair_small(rk[c], b, sub_b, &fast) ||
             _bi_try_add_limb2_kind(rk[c], b, sub_b, 1, &fast))
                ? fast
                : rt_bigint_submul(rk[c], q, b);
      }
      return row_k;
    }
    for (int64_t c = 0; c <= limit; c++) {
      int64_t b = rj[c];
      if (_bi_is_zero_fast(b))
        continue;
      int64_t fast = 0;
      rk[c] =
          (_bi_try_submul_i64_int_pair_small(rk[c], q_raw, b,
                                             &fast) ||
           _bi_try_submul_i64_limb2(rk[c], q_raw, b, &fast))
              ? fast
              : rt_bigint_submul(rk[c], q, b);
    }
    return row_k;
  }
  for (int64_t c = 0; c <= limit; c++) {
    int64_t b = rj[c];
    if (!_bi_is_zero_fast(b))
      rk[c] = rt_bigint_submul(rk[c], q, b);
  }
  return row_k;
}

int64_t rt_bigint_row_submul_auto(int64_t row_k, int64_t row_j,
                                  int64_t q) {
  if (!is_ptr(row_k) || !is_ptr(row_j) || q == rt_tag_v(0))
    return row_k;
  int64_t tagged_len = *(int64_t *)(uintptr_t)((char *)row_j + 0);
  int64_t limit =
      is_int(tagged_len) ? rt_untag_v(tagged_len) - 1 : tagged_len - 1;
  if (limit < 0)
    return row_k;
  const int64_t *rj =
      (const int64_t *)(uintptr_t)((char *)row_j + 16);
  while (limit >= 0 && _bi_is_zero_fast(rj[limit]))
    limit--;
  if (limit < 0)
    return row_k;
  int64_t *rk = (int64_t *)(uintptr_t)((char *)row_k + 16);
  if (is_int(q)) {
    int64_t q_raw = rt_untag_v(q);
    if (q_raw == 1 || q_raw == -1) {
      int sub_b = q_raw == 1;
      for (int64_t c = 0; c <= limit; c++) {
        int64_t b = rj[c];
        if (_bi_is_zero_fast(b))
          continue;
        int64_t fast = 0;
        rk[c] =
            (_bi_try_add_int_pair_small(rk[c], b, sub_b, &fast) ||
             _bi_try_add_limb2_kind(rk[c], b, sub_b, 1, &fast))
                ? fast
                : rt_bigint_submul(rk[c], q, b);
      }
      return row_k;
    }
    for (int64_t c = 0; c <= limit; c++) {
      int64_t b = rj[c];
      if (_bi_is_zero_fast(b))
        continue;
      int64_t fast = 0;
      rk[c] =
          (_bi_try_submul_i64_int_pair_small(rk[c], q_raw, b,
                                             &fast) ||
           _bi_try_submul_i64_limb2(rk[c], q_raw, b, &fast))
              ? fast
              : rt_bigint_submul(rk[c], q, b);
    }
    return row_k;
  }
  for (int64_t c = 0; c <= limit; c++) {
    int64_t b = rj[c];
    if (!_bi_is_zero_fast(b))
      rk[c] = rt_bigint_submul(rk[c], q, b);
  }
  return row_k;
}

int64_t rt_bigint_div(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mq;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  if (mpz_sgn(mb) == 0) {
    mpz_clear(ma);
    mpz_clear(mb);
    return rt_division_by_zero();
  }
  mpz_init(mq);
  mpz_tdiv_q(mq, ma, mb);
  int64_t r = _bi_from_mpz(mq);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mq);
  return r;
#else
  _bn_t na, nb, nq, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nq);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  if (_bn_is_zero(&nb)) {
    _bn_clear(&na);
    _bn_clear(&nb);
    _bn_clear(&nq);
    _bn_clear(&nr);
    return rt_division_by_zero();
  }
  _bn_tdiv_qr(&nq, &nr, &na, &nb);
  int64_t r = _bi_from_native(&nq);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nq);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_mod(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  if (mpz_sgn(mb) == 0) {
    mpz_clear(ma);
    mpz_clear(mb);
    return rt_modulo_by_zero();
  }
  mpz_init(mr);
  mpz_fdiv_r(mr, ma, mb);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nb, nq, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nq);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  if (_bn_is_zero(&nb)) {
    _bn_clear(&na);
    _bn_clear(&nb);
    _bn_clear(&nq);
    _bn_clear(&nr);
    return rt_modulo_by_zero();
  }
  _bn_fdiv_qr(&nq, &nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nq);
  _bn_clear(&nr);
  return r;
#endif
}

#ifdef NYTRIX_USE_GMP
static int64_t _bi_binary_bitop(int64_t a, int64_t b,
                                void (*op)(mpz_t, const mpz_t,
                                           const mpz_t)) {
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  op(mr, ma, mb);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
}
int64_t rt_bigint_and(int64_t a, int64_t b) {
  return _bi_binary_bitop(a, b, mpz_and);
}
int64_t rt_bigint_or(int64_t a, int64_t b) {
  return _bi_binary_bitop(a, b, mpz_ior);
}
int64_t rt_bigint_xor(int64_t a, int64_t b) {
  return _bi_binary_bitop(a, b, mpz_xor);
}
#else
int64_t rt_bigint_and(int64_t a, int64_t b) {
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_and(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
}
int64_t rt_bigint_or(int64_t a, int64_t b) {
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_or(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
}
int64_t rt_bigint_xor(int64_t a, int64_t b) {
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_xor(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
}
#endif

int64_t rt_bigint_cmp(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  int r = mpz_cmp(ma, mb);
  mpz_clear(ma);
  mpz_clear(mb);
  return rt_tag_v((int64_t)(r > 0 ? 1 : (r < 0 ? -1 : 0)));
#else
  _bn_t na, nb;
  _bn_init(&na);
  _bn_init(&nb);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  int r = _bn_cmp(&na, &nb);
  _bn_clear(&na);
  _bn_clear(&nb);
  return rt_tag_v((int64_t)(r > 0 ? 1 : (r < 0 ? -1 : 0)));
#endif
}

int64_t rt_native_bigint_cmp(int64_t a, int64_t b) {
  return rt_untag_v(rt_bigint_cmp(a, b));
}

int64_t rt_bigint_to_str(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  char *s = mpz_get_str(NULL, 10, ma);
  int64_t r = rt_alloc_string(s);
  free(s);
  mpz_clear(ma);
  return r;
#else
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  char *s = _bn_to_str(&na);
  int64_t r = rt_alloc_string(s);
  free(s);
  _bn_clear(&na);
  return r;
#endif
}

int64_t rt_bigint_from_str(int64_t str_ptr) {
  if (!str_ptr)
    return 0;
  /*
   * The interpreter/LLVM tiers pass tagged Ny strings, while direct native
   * NYIR uses raw NUL-terminated C strings.  Both representations point at
   * the first character, so parsing is identical once NULL is rejected.
   */
  const char *s = (const char *)(uintptr_t)str_ptr;
#ifdef NYTRIX_USE_GMP
  mpz_t val;
  mpz_init(val);
  if (mpz_set_str(val, s, 10) != 0) {
    mpz_clear(val);
    return 0;
  }
  int64_t r = _bi_from_mpz(val);
  mpz_clear(val);
  return r;
#else
  _bn_t val;
  _bn_init(&val);
  _bn_from_str(&val, s);
  int64_t r = _bi_from_native(&val);
  _bn_clear(&val);
  return r;
#endif
}

int64_t rt_bigint_to_bytes(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  if (mpz_sgn(ma) == 0) {
    mpz_clear(ma);
    int64_t res = rt_list_new(1);
    res = rt_append(res, rt_tag_v(0));
    return res;
  }
  size_t count = 0;
  unsigned char *buf =
      (unsigned char *)mpz_export(NULL, &count, 1, 1, 1, 0, ma);
  int64_t res = rt_list_new((count << 1) | 1);
  for (size_t i = 0; i < count; ++i)
    res = rt_append(res, rt_tag_v((int64_t)buf[i]));
  free(buf);
  mpz_clear(ma);
  return res;
#else
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  if (_bn_is_zero(&na)) {
    _bn_clear(&na);
    int64_t res = rt_list_new(1);
    res = rt_append(res, rt_tag_v(0));
    return res;
  }
  size_t count = 0;
  uint8_t *buf = _bn_to_bytes_be(&na, &count);
  int64_t res = rt_list_new((count << 1) | 1);
  for (size_t i = 0; i < count; ++i)
    res = rt_append(res, rt_tag_v((int64_t)buf[i]));
  free(buf);
  _bn_clear(&na);
  return res;
#endif
}

int64_t rt_bigint_powmod(int64_t base, int64_t exp, int64_t mod) {
#ifdef NYTRIX_USE_GMP
  mpz_t mb, me, mm, mr;
  _bi_val_to_mpz(base, mb);
  _bi_val_to_mpz(exp, me);
  _bi_val_to_mpz(mod, mm);
  mpz_init(mr);
  mpz_powm(mr, mb, me, mm);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(mb);
  mpz_clear(me);
  mpz_clear(mm);
  mpz_clear(mr);
  return r;
#else
  _bn_t nb, ne, nm, nr;
  _bn_init(&nb);
  _bn_init(&ne);
  _bn_init(&nm);
  _bn_init(&nr);
  _bi_val_to_native(base, &nb);
  _bi_val_to_native(exp, &ne);
  _bi_val_to_native(mod, &nm);
  _bn_powmod(&nr, &nb, &ne, &nm);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nb);
  _bn_clear(&ne);
  _bn_clear(&nm);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_modinv(int64_t a, int64_t m) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mm, mi;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(m, mm);
  mpz_init(mi);
  if (mpz_invert(mi, ma, mm)) {
    int64_t r = _bi_from_mpz(mi);
    mpz_clear(ma);
    mpz_clear(mm);
    mpz_clear(mi);
    return r;
  }
  mpz_clear(ma);
  mpz_clear(mm);
  mpz_clear(mi);
  return _bi_from_i64(0);
#else
  _bn_t na, nm, nr;
  _bn_init(&na);
  _bn_init(&nm);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(m, &nm);
  if (_bn_modinv(&nr, &na, &nm)) {
    int64_t r = _bi_from_native(&nr);
    _bn_clear(&na);
    _bn_clear(&nm);
    _bn_clear(&nr);
    return r;
  }
  _bn_clear(&na);
  _bn_clear(&nm);
  _bn_clear(&nr);
  return _bi_from_i64(0);
#endif
}

int64_t rt_bigint_gcd(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  mpz_gcd(mr, ma, mb);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_gcd(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_bitlen(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  size_t r = mpz_sizeinbase(ma, 2);
  mpz_clear(ma);
  return rt_tag_v((int64_t)r);
#else
  if (is_int(a)) {
    int64_t v = rt_untag_v(a);
    if (v == 0)
      return rt_tag_v(0);
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    return rt_tag_v((int64_t)(64 - __builtin_clzll(mag)));
  }
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  size_t r = _bn_bitlen(&na);
  _bn_clear(&na);
  return rt_tag_v((int64_t)r);
#endif
}

int64_t rt_bigint_popcount(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  size_t r = mpz_popcount(ma);
  mpz_clear(ma);
  return rt_tag_v((int64_t)r);
#else
  return _bi_popcount_native(a);
#endif
}

int64_t rt_bigint_to_i64_raw(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  int64_t v = _bi_mpz_get_i64(ma);
  mpz_clear(ma);
  return v;
#else
  if (is_int(a))
    return rt_untag_v(a);
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  int64_t v = 0;
  if (_bn_is_zero(&na))
    v = 0;
  else
    v = na.sign >= 0 ? (int64_t)na.d[0] : -(int64_t)na.d[0];
  _bn_clear(&na);
  return v;
#endif
}

int64_t rt_bigint_to_int(int64_t a) {
  return ny_value_box_i64(rt_bigint_to_i64_raw(a));
}

static double _bi_to_double_fast(int64_t a) {
  if (is_int(a)) {
    return (double)rt_untag_v(a);
  } else if (is_ptr(a)) {
    int64_t tag = *(int64_t *)(uintptr_t)((char *)a - 8);
    if (tag == TAG_BIGINT) {
      int64_t sign = rt_untag_v(*(int64_t *)(uintptr_t)((char *)a + 0));
      int64_t words =
          rt_untag_v(*(int64_t *)(uintptr_t)((char *)a + 8));
      if (sign != 0 && words > 0) {
        const uint64_t *src =
            (const uint64_t *)((char *)a + 16);
        int64_t hi = words - 1;
        while (hi >= 0 && src[hi] == 0)
          hi--;
        if (hi >= 0) {
          long double acc = 0.0L;
          int take = 0;
          for (int64_t i = hi; i >= 0 && take < 4; i--, take++)
            acc = acc * 18446744073709551616.0L + (long double)src[i];
          long double scaled = ldexpl(acc, (int)((hi + 1 - take) * 64));
          return (double)(sign < 0 ? -scaled : scaled);
        }
      }
    }
  }
  return 0.0;
}

int64_t rt_bigint_to_f64(int64_t a) {
  double d = _bi_to_double_fast(a);
  int64_t bits = 0;
  memcpy(&bits, &d, sizeof(bits));
  return rt_flt_box_val(bits);
}

int64_t rt_bigint_f64buf_store(int64_t buf, int64_t i_v, int64_t a) {
  int64_t i = is_int(i_v) ? rt_untag_v(i_v) : i_v;
  if (i < 0)
    return buf;
  uintptr_t p = (uintptr_t)((intptr_t)buf + (intptr_t)(i * 8));
  if (p < 0x1000)
    return buf;
  double d = _bi_to_double_fast(a);
  if ((p & (uintptr_t)7u) == 0) {
    *(double *)p = d;
  } else {
    memcpy((void *)p, &d, sizeof(d));
  }
  return buf;
}

int64_t rt_bigint_from_i64_raw(int64_t v) {
#ifdef NYTRIX_USE_GMP
  return _bi_from_i64(v);
#else
  int sign = v < 0 ? -1 : (v == 0 ? 0 : 1);
  uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
  size_t wc = (sign == 0) ? 0 : 1;
  int64_t p = _bi_alloc(wc);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p + 0) = rt_tag_v((int64_t)sign);
  if (wc > 0)
    *(uint64_t *)((char *)p + 16) = mag;
  return p;
#endif
}

int64_t rt_bigint_from_int(int64_t n) { return rt_bigint_from_i64_raw(rt_untag_v(n)); }

static int64_t rt_bigint_from_bytes_be_stored(const uint8_t *bytes,
                                               size_t len) {
#ifdef NYTRIX_USE_GMP
  mpz_t val;
  mpz_init(val);
  if (bytes && len > 0)
    mpz_import(val, len, 1, 1, 1, 0, bytes);
  int64_t out = _bi_from_mpz(val);
  mpz_clear(val);
  return out;
#else
  _bn_t val;
  _bn_init(&val);
  _bn_from_bytes_be(&val, bytes, len);
  int64_t out = _bi_from_native(&val);
  _bn_clear(&val);
  return out;
#endif
}

static int64_t rt_long_bytes_like(int64_t v) {
  uintptr_t len_ptr = (uintptr_t)v - 16u;
  if (!rt_addr_readable(len_ptr, sizeof(int64_t)))
    return _bi_from_i64(0);
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)len_ptr, sizeof(tagged_len));
  int64_t len = is_int(tagged_len) ? (tagged_len >> 1) : tagged_len;
  if (len <= 0)
    return _bi_from_i64(0);
  if (!rt_addr_readable((uintptr_t)v, (size_t)len))
    return _bi_from_i64(0);
  return rt_bigint_from_bytes_be_stored(
      (const uint8_t *)(uintptr_t)v, (size_t)len);
}

int64_t rt_long(int64_t v) {
  if (is_int(v))
    return _bi_from_i64(rt_untag_v(v));
  if (is_v_flt(v)) {
    int64_t bits = _rt_flt_unbox_val(v);
    double d = 0.0;
    memcpy(&d, &bits, sizeof(d));
    return _bi_from_i64((int64_t)d);
  }
  if (is_v_str(v))
    return rt_long_bytes_like(v);
  if (!is_ptr(v))
    return _bi_from_i64(0);
  int64_t tag = 0;
  if (is_heap_ptr(v)) {
    tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  } else {
    uintptr_t tag_ptr = (uintptr_t)v - 8u;
    if (!rt_addr_readable(tag_ptr, sizeof(int64_t)))
      return _bi_from_i64(0);
    memcpy(&tag, (const void *)tag_ptr, sizeof(tag));
  }
  if (tag == TAG_FLOAT) {
    int64_t bits = *(int64_t *)((char *)(uintptr_t)v + 0);
    double d = 0.0;
    memcpy(&d, &bits, sizeof(d));
    return _bi_from_i64((int64_t)d);
  }
  if (tag == TAG_BIGINT)
    return v;
  if (tag == TAG_BYTES)
    return rt_long_bytes_like(v);
  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    if (!rt_addr_readable((uintptr_t)v, 16))
      return _bi_from_i64(0);
    int64_t tagged_len = *(int64_t *)((char *)(uintptr_t)v + 0);
    int64_t len =
        is_int(tagged_len) ? (tagged_len >> 1) : tagged_len;
    if (len <= 0)
      return _bi_from_i64(0);
    size_t need = 16u + (size_t)len * sizeof(int64_t);
    if (!rt_addr_readable((uintptr_t)v, need))
      return _bi_from_i64(0);
#ifdef NYTRIX_USE_GMP
    mpz_t out;
    mpz_init(out);
    for (int64_t i = 0; i < len; ++i) {
      int64_t item =
          *(int64_t *)((char *)(uintptr_t)v + 16 + i * 8);
      unsigned long byte =
          is_int(item) ? (unsigned long)(rt_untag_v(item) & 255)
                       : 0ul;
      mpz_mul_ui(out, out, 256);
      mpz_add_ui(out, out, byte);
    }
    int64_t boxed = _bi_from_mpz(out);
    mpz_clear(out);
    return boxed;
#else
    _bn_t out;
    _bn_init(&out);
    _bn_t two56 = {0};
    _bn_set_i64(&two56, 256);
    for (int64_t i = 0; i < len; ++i) {
      int64_t item =
          *(int64_t *)((char *)(uintptr_t)v + 16 + i * 8);
      unsigned long byte =
          is_int(item) ? (unsigned long)(rt_untag_v(item) & 255)
                       : 0ul;
      _bn_t tmp = {0};
      _bn_mul(&tmp, &out, &two56);
      _bn_clear(&out);
      _bn_copy(&out, &tmp);
      _bn_clear(&tmp);
      _bn_t dv = {0};
      _bn_set_i64(&dv, (int64_t)byte);
      _bn_add(&out, &out, &dv);
      _bn_clear(&dv);
    }
    _bn_clear(&two56);
    int64_t boxed = _bi_from_native(&out);
    _bn_clear(&out);
    return boxed;
#endif
  }
  return _bi_from_i64(0);
}

int64_t rt_bigint_pow(int64_t b, int64_t e) {
#ifdef NYTRIX_USE_GMP
  mpz_t mb, me, mr;
  _bi_val_to_mpz(b, mb);
  _bi_val_to_mpz(e, me);
  mpz_init(mr);
  mpz_pow_ui(mr, mb, mpz_get_ui(me));
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(mb);
  mpz_clear(me);
  mpz_clear(mr);
  return r;
#else
  _bn_t nb, ne, nr;
  _bn_init(&nb);
  _bn_init(&ne);
  _bn_init(&nr);
  _bi_val_to_native(b, &nb);
  _bi_val_to_native(e, &ne);
  /*
   * Repeated squaring
   */
  _bn_set_i64(&nr, 1);
  size_t ebits = _bn_bitlen(&ne);
  for (size_t i = 0; i < ebits; i++) {
    size_t word = i / 64;
    size_t bit = i % 64;
    if (word < ne.count && (ne.d[word] >> bit & 1)) {
      _bn_t tmp = {0};
      _bn_mul(&tmp, &nr, &nb);
      _bn_clear(&nr);
      _bn_copy(&nr, &tmp);
      _bn_clear(&tmp);
    }
    _bn_t sq = {0};
    _bn_mul(&sq, &nb, &nb);
    _bn_clear(&nb);
    _bn_copy(&nb, &sq);
    _bn_clear(&sq);
  }
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nb);
  _bn_clear(&ne);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_isqrt(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mr;
  _bi_val_to_mpz(a, ma);
  mpz_init(mr);
  mpz_sqrt(mr, ma);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nr;
  _bn_init(&na);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bn_isqrt(&nr, &na);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_legendre(int64_t a, int64_t p) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mp;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(p, mp);
  int r = mpz_legendre(ma, mp);
  mpz_clear(ma);
  mpz_clear(mp);
  return rt_tag_v(r);
#else
  _bn_t na, np;
  _bn_init(&na);
  _bn_init(&np);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(p, &np);
  int r = _bn_jacobi_val(&na, &np);
  _bn_clear(&na);
  _bn_clear(&np);
  return rt_tag_v(r);
#endif
}

int64_t rt_bigint_jacobi(int64_t a, int64_t n) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mn;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(n, mn);
  int r = mpz_jacobi(ma, mn);
  mpz_clear(ma);
  mpz_clear(mn);
  return rt_tag_v(r);
#else
  _bn_t na, nn;
  _bn_init(&na);
  _bn_init(&nn);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(n, &nn);
  int r = _bn_jacobi_val(&na, &nn);
  _bn_clear(&na);
  _bn_clear(&nn);
  return rt_tag_v(r);
#endif
}

int64_t rt_bigint_kronecker(int64_t a, int64_t n) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mn;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(n, mn);
  int r = mpz_kronecker(ma, mn);
  mpz_clear(ma);
  mpz_clear(mn);
  return rt_tag_v(r);
#else
  _bn_t na, nn;
  _bn_init(&na);
  _bn_init(&nn);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(n, &nn);
  int r = _bn_jacobi_val(&na, &nn);
  _bn_clear(&na);
  _bn_clear(&nn);
  return rt_tag_v(r);
#endif
}

int64_t rt_bigint_iroot(int64_t n, int64_t k) {
#ifdef NYTRIX_USE_GMP
  mpz_t mn, mr;
  _bi_val_to_mpz(n, mn);
  mpz_init(mr);
  mpz_root(mr, mn, (unsigned long)rt_untag_v(k));
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(mn);
  mpz_clear(mr);
  return r;
#else
  _bn_t nn, nk, nr;
  _bn_init(&nn);
  _bn_init(&nk);
  _bn_init(&nr);
  _bi_val_to_native(n, &nn);
  _bi_val_to_native(k, &nk);
  unsigned long ku = _bn_is_zero(&nk) ? 0 : (unsigned long)nk.d[0];
  _bn_iroot(&nr, &nn, ku);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nn);
  _bn_clear(&nk);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_is_perfect_square(int64_t n) {
#ifdef NYTRIX_USE_GMP
  mpz_t mn;
  _bi_val_to_mpz(n, mn);
  int r = mpz_perfect_square_p(mn);
  mpz_clear(mn);
  return rt_tag_v(r);
#else
  _bn_t nn;
  _bn_init(&nn);
  _bi_val_to_native(n, &nn);
  bool r = _bn_perfect_square_p(&nn);
  _bn_clear(&nn);
  return rt_tag_v(r ? 1 : 0);
#endif
}

int64_t rt_bigint_xgcd(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mg, mx, my;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mg);
  mpz_init(mx);
  mpz_init(my);
  mpz_gcdext(mg, mx, my, ma, mb);
  int64_t res = rt_list_new((3 << 1) | 1);
  res = rt_append(res, _bi_from_mpz(mg));
  res = rt_append(res, _bi_from_mpz(mx));
  res = rt_append(res, _bi_from_mpz(my));
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mg);
  mpz_clear(mx);
  mpz_clear(my);
  return res;
#else
  _bn_t na, nb, ng, nx, ny;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&ng);
  _bn_init(&nx);
  _bn_init(&ny);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_xgcd(&ng, &nx, &ny, &na, &nb);
  int64_t res = rt_list_new((3 << 1) | 1);
  res = rt_append(res, _bi_from_native(&ng));
  res = rt_append(res, _bi_from_native(&nx));
  res = rt_append(res, _bi_from_native(&ny));
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&ng);
  _bn_clear(&nx);
  _bn_clear(&ny);
  return res;
#endif
}

int64_t rt_bigint_clz(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  if (mpz_sgn(ma) == 0) {
    mpz_clear(ma);
    return rt_tag_v(64);
  }
  size_t bits = mpz_sizeinbase(ma, 2);
  mpz_clear(ma);
  size_t limb_bits = bits % 64;
  return rt_tag_v((int64_t)(limb_bits ? 64 - limb_bits : 0));
#else
  if (is_int(a)) {
    int64_t v = rt_untag_v(a);
    if (v == 0)
      return rt_tag_v(64);
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    size_t bits = 64 - __builtin_clzll(mag);
    size_t limb_bits = bits % 64;
    return rt_tag_v((int64_t)(limb_bits ? 64 - limb_bits : 0));
  }
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  if (_bn_is_zero(&na)) {
    _bn_clear(&na);
    return rt_tag_v(64);
  }
  size_t bits = _bn_bitlen(&na);
  _bn_clear(&na);
  size_t limb_bits = bits % 64;
  return rt_tag_v((int64_t)(limb_bits ? 64 - limb_bits : 0));
#endif
}

int64_t rt_bigint_ctz(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  size_t r = mpz_scan1(ma, 0);
  mpz_clear(ma);
  return rt_tag_v((int64_t)r);
#else
  if (is_int(a)) {
    int64_t v = rt_untag_v(a);
    if (v == 0)
      return rt_tag_v(64);
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    return rt_tag_v((int64_t)__builtin_ctzll(mag));
  }
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  int64_t r = 0;
  if (!_bn_is_zero(&na)) {
    for (size_t i = 0; i < na.count; i++) {
      if (na.d[i]) {
        r = (int64_t)(i * 64 + __builtin_ctzll(na.d[i]));
        break;
      }
    }
    if (r == 0 && na.count > 0) {
      /*
       * all zero — shouldn't happen after trim
       */
    }
  }
  _bn_clear(&na);
  return rt_tag_v(r);
#endif
}

int64_t rt_bigint_not(int64_t a) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mr;
  _bi_val_to_mpz(a, ma);
  mpz_init(mr);
  mpz_com(mr, ma);
  int64_t r = _bi_from_mpz(mr);
  mpz_clears(ma, mr, NULL);
  return r;
#else
  _bn_t na, nr;
  _bn_init(&na);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bn_com(&nr, &na);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_shl(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  if (mpz_sgn(mb) < 0)
    mpz_set(mr, ma);
  else
    mpz_mul_2exp(mr, ma, mpz_get_ui(mb));
  int64_t r = _bi_from_mpz(mr);
  mpz_clears(ma, mb, mr, NULL);
  return r;
#else
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  if (_bn_is_zero(&nb) || nb.sign < 0) {
    _bn_copy(&nr, &na);
  } else {
    size_t bits = nb.count > 0 ? (size_t)nb.d[0] : 0;
    _bn_shl(&nr, &na, bits);
  }
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_shr(int64_t a, int64_t b) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  mpz_init(mr);
  if (mpz_sgn(mb) < 0)
    mpz_set(mr, ma);
  else
    mpz_fdiv_q_2exp(mr, ma, mpz_get_ui(mb));
  int64_t r = _bi_from_mpz(mr);
  mpz_clears(ma, mb, mr, NULL);
  return r;
#else
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  if (_bn_is_zero(&nb) || nb.sign < 0) {
    _bn_copy(&nr, &na);
  } else {
    size_t bits = nb.count > 0 ? (size_t)nb.d[0] : 0;
    _bn_shr(&nr, &na, bits);
  }
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_gf2_mod(int64_t a, int64_t m) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mm, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(m, mm);
  mpz_init(mr);
  mpz_set(mr, ma);
  {
    mpz_t shifted;
    mpz_init(shifted);
    for (;;) {
      long a_deg = mpz_sgn(mr) == 0 ? -1 : (long)mpz_sizeinbase(mr, 2) - 1;
      long md = mpz_sgn(mm) == 0 ? -1 : (long)mpz_sizeinbase(mm, 2) - 1;
      if (a_deg < md)
        break;
      mpz_mul_2exp(shifted, mm, (mp_bitcnt_t)(a_deg - md));
      mpz_xor(mr, mr, shifted);
    }
    mpz_clear(shifted);
  }
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mm);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nm, nr;
  _bn_init(&na);
  _bn_init(&nm);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(m, &nm);
  _bn_gf2_mod(&nr, &na, &nm);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nm);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_gf2_mulmod(int64_t a, int64_t b, int64_t m) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mb, mm, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  _bi_val_to_mpz(m, mm);
  mpz_init(mr);
  {
    long m_deg = (long)mpz_sizeinbase(mm, 2) - 1;
    if (mpz_sgn(mm) == 0)
      m_deg = -1;
    if (m_deg < 0) {
      mpz_set_ui(mr, 0);
    } else {
      mpz_t va, vb2;
      mpz_init(va);
      mpz_init_set(vb2, mb);
      /*
       * reduce va
       */
      mpz_set(va, ma);
      {
        mpz_t shifted;
        mpz_init(shifted);
        for (;;) {
          long a_deg = (long)mpz_sizeinbase(va, 2) - 1;
          if (mpz_sgn(va) == 0)
            a_deg = -1;
          if (a_deg < m_deg)
            break;
          mpz_mul_2exp(shifted, mm, (mp_bitcnt_t)(a_deg - m_deg));
          mpz_xor(va, va, shifted);
        }
        mpz_clear(shifted);
      }
      mpz_set_ui(mr, 0);
      while (mpz_sgn(vb2) != 0) {
        if (mpz_tstbit(vb2, 0))
          mpz_xor(mr, mr, va);
        mpz_fdiv_q_2exp(vb2, vb2, 1);
        if (mpz_sgn(vb2) != 0) {
          mpz_mul_2exp(va, va, 1);
          if (mpz_tstbit(va, (mp_bitcnt_t)m_deg))
            mpz_xor(va, va, mm);
        }
      }
      mpz_clear(va);
      mpz_clear(vb2);
    }
  }
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mm);
  mpz_clear(mr);
  return r;
#else
  _bn_t na, nb, nm, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nm);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bi_val_to_native(m, &nm);
  _bn_gf2_mulmod(&nr, &na, &nb, &nm);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nm);
  _bn_clear(&nr);
  return r;
#endif
}

int64_t rt_bigint_gf2_inv(int64_t a, int64_t m) {
#ifdef NYTRIX_USE_GMP
  mpz_t ma, mm;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(m, mm);
  /*
   * Extended GCD over GF(2)
   */
  mpz_t r0, r1, t0, t1, q, rem, prod, next_t;
  mpz_init_set(r0, mm);
  mpz_init(r1);
  mpz_set(r1, ma);
  /*
   * reduce r1 mod m
   */
  {
    mpz_t shifted;
    mpz_init(shifted);
    long m_deg = (long)mpz_sizeinbase(mm, 2) - 1;
    if (mpz_sgn(mm) == 0)
      m_deg = -1;
    for (;;) {
      long a_deg = (long)mpz_sizeinbase(r1, 2) - 1;
      if (mpz_sgn(r1) == 0)
        a_deg = -1;
      if (a_deg < m_deg)
        break;
      mpz_mul_2exp(shifted, mm, (mp_bitcnt_t)(a_deg - m_deg));
      mpz_xor(r1, r1, shifted);
    }
    mpz_clear(shifted);
  }
  mpz_init_set_ui(t0, 0);
  mpz_init_set_ui(t1, 1);
  mpz_inits(q, rem, prod, next_t, NULL);
  while (mpz_sgn(r1) != 0) {
    /*
     * div_qr over GF(2)
     */
    mpz_set_ui(q, 0);
    mpz_set(rem, r0);
    long b_deg = (long)mpz_sizeinbase(r1, 2) - 1;
    if (mpz_sgn(r1) == 0)
      b_deg = -1;
    if (b_deg >= 0) {
      mpz_t shifted;
      mpz_init(shifted);
      for (;;) {
        long r_deg = (long)mpz_sizeinbase(rem, 2) - 1;
        if (mpz_sgn(rem) == 0)
          r_deg = -1;
        if (r_deg < b_deg)
          break;
        mp_bitcnt_t shift = (mp_bitcnt_t)(r_deg - b_deg);
        mpz_setbit(q, shift);
        mpz_mul_2exp(shifted, r1, shift);
        mpz_xor(rem, rem, shifted);
      }
      mpz_clear(shifted);
    }
    mpz_set(r0, r1);
    mpz_set(r1, rem);
    /*
     * prod = q * t1 mod m
     */
    mpz_mul(prod, q, t1);
    /*
     * reduce prod mod m
     */
    {
      mpz_t shifted;
      mpz_init(shifted);
      long m_deg = (long)mpz_sizeinbase(mm, 2) - 1;
      if (mpz_sgn(mm) == 0)
        m_deg = -1;
      for (;;) {
        long a_deg = (long)mpz_sizeinbase(prod, 2) - 1;
        if (mpz_sgn(prod) == 0)
          a_deg = -1;
        if (a_deg < m_deg)
          break;
        mpz_mul_2exp(shifted, mm, (mp_bitcnt_t)(a_deg - m_deg));
        mpz_xor(prod, prod, shifted);
      }
      mpz_clear(shifted);
    }
    mpz_xor(next_t, t0, prod);
    /*
     * reduce next_t mod m
     */
    {
      mpz_t shifted;
      mpz_init(shifted);
      long m_deg = (long)mpz_sizeinbase(mm, 2) - 1;
      if (mpz_sgn(mm) == 0)
        m_deg = -1;
      for (;;) {
        long a_deg = (long)mpz_sizeinbase(next_t, 2) - 1;
        if (mpz_sgn(next_t) == 0)
          a_deg = -1;
        if (a_deg < m_deg)
          break;
        mpz_mul_2exp(shifted, mm, (mp_bitcnt_t)(a_deg - m_deg));
        mpz_xor(next_t, next_t, shifted);
      }
      mpz_clear(shifted);
    }
    mpz_set(t0, t1);
    mpz_set(t1, next_t);
  }
  long r0_deg = (long)mpz_sizeinbase(r0, 2) - 1;
  if (mpz_sgn(r0) == 0)
    r0_deg = -1;
  int64_t out;
  if (r0_deg > 0)
    out = _bi_from_i64(0);
  else
    out = _bi_from_mpz(t0);
  mpz_clears(ma, mm, r0, r1, t0, t1, q, rem, prod, next_t, NULL);
  return out;
#else
  _bn_t na, nm;
  _bn_init(&na);
  _bn_init(&nm);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(m, &nm);
  /*
   * GF(2) extended GCD
   */
  _bn_t r0, r1, t0, t1;
  _bn_copy(&r0, &nm);
  _bn_gf2_mod(&r1, &na, &nm);
  _bn_set_i64(&t0, 0);
  _bn_set_i64(&t1, 1);
  while (!_bn_is_zero(&r1)) {
    /*
     * GF(2) div_qr
     */
    _bn_t q, rem;
    _bn_set_zero(&q);
    _bn_gf2_div_qr(&q, &rem, &r0, &r1);
    _bn_clear(&r0);
    _bn_copy(&r0, &r1);
    _bn_clear(&r1);
    _bn_copy(&r1, &rem);
    _bn_clear(&rem);
    _bn_t prod = {0};
    _bn_gf2_mulmod(&prod, &q, &t1, &nm);
    _bn_t next_t = {0};
    _bn_xor(&next_t, &t0, &prod);
    _bn_gf2_mod(&next_t, &next_t, &nm);
    _bn_clear(&q);
    _bn_clear(&prod);
    _bn_clear(&t0);
    _bn_copy(&t0, &t1);
    _bn_clear(&t1);
    _bn_copy(&t1, &next_t);
    _bn_clear(&next_t);
  }
  long r0_deg = _bn_gf2_deg(&r0);
  int64_t out;
  if (r0_deg > 0)
    out = _bi_from_i64(0);
  else
    out = _bi_from_native(&t0);
  _bn_clear(&na);
  _bn_clear(&nm);
  _bn_clear(&r0);
  _bn_clear(&r1);
  _bn_clear(&t0);
  _bn_clear(&t1);
  return out;
#endif
}

int64_t rt_ct_compare(int64_t a_ptr, int64_t b_ptr, int64_t len_val) {
  const uint8_t *a = (const uint8_t *)(uintptr_t)a_ptr;
  const uint8_t *b = (const uint8_t *)(uintptr_t)b_ptr;
  size_t len = (size_t)(len_val >> 1);
  if (!a || !b || len == 0)
    return rt_tag_v(0);
  uint8_t result = 0;
  size_t i = 0;
  while (i < len) {
    result |= a[i] ^ b[i];
    i++;
  }
  return rt_tag_v((int64_t)result);
}

int64_t rt_ct_select(int64_t a, int64_t b, int64_t condition) {
  uint64_t c = (uint64_t)condition;
  uint64_t is_0 = ((c | (0ULL - c)) >> 63) ^ 1ULL;
  uint64_t d1 = c ^ 1ULL;
  uint64_t is_1 = ((d1 | (0ULL - d1)) >> 63) ^ 1ULL;
  uint64_t dfalse = c ^ (uint64_t)NY_IMM_FALSE;
  uint64_t is_false = ((dfalse | (0ULL - dfalse)) >> 63) ^ 1ULL;
  uint64_t is_falsy = is_0 | is_1 | is_false;
  uint64_t mask = 0ULL - (1ULL - is_falsy);
  return (int64_t)(((uint64_t)a & mask) | ((uint64_t)b & ~mask));
}

static int64_t _bi_from_i64(int64_t v) {
#ifdef NYTRIX_USE_GMP
  mpz_t val;
  mpz_init(val);
  _bi_mpz_set_i64(val, v);
  int64_t r = _bi_from_mpz(val);
  mpz_clear(val);
  return r;
#else
  return rt_tag_v(v);
#endif
}
