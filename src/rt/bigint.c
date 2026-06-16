#include <gmp.h>
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

static inline bool _bi_read_limb2(int64_t v, int *sign, uint64_t limbs[2], size_t *words) {
  limbs[0] = 0;
  limbs[1] = 0;
  *words = 0;
  *sign = 0;
  if (is_int(v)) {
    int64_t raw = rt_untag_v(v);
    if (raw == 0)
      return true;
    *sign = raw < 0 ? -1 : 1;
    limbs[0] = raw < 0 ? (uint64_t)(-(raw + 1)) + 1u : (uint64_t)raw;
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

static inline size_t _bi_norm_words_const(const uint64_t *limbs, size_t words) {
  while (words > 0 && limbs[words - 1] == 0)
    words--;
  return words;
}

static inline int _bi_cmp_mag(const uint64_t *a, size_t aw, const uint64_t *b, size_t bw) {
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

static int64_t _bi_from_limbs(int sign, const uint64_t *limbs, size_t words) {
  words = _bi_norm_words_const(limbs, words);
  if (words == 0)
    sign = 0;
  int64_t p = _bi_alloc(words);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p + 0) = rt_tag_v((int64_t)(sign < 0 ? -1 : (sign > 0 ? 1 : 0)));
  if (words > 0)
    memcpy((void *)((char *)p + 16), limbs, words * sizeof(uint64_t));
  return p;
}

static inline int64_t _bi_from_limbs_or_small_int(int sign, const uint64_t *limbs, size_t words);

static inline int64_t _bi_from_i128_value(__int128 v, int compact) {
  if (compact && v >= (__int128)NY_SMALL_INT_MIN && v <= (__int128)NY_SMALL_INT_MAX)
    return rt_tag_v((int64_t)v);
  int sign = v < 0 ? -1 : (v > 0 ? 1 : 0);
  unsigned __int128 mag = v < 0 ? (unsigned __int128)(-v) : (unsigned __int128)v;
  uint64_t limbs[2] = {(uint64_t)mag, (uint64_t)(mag >> 64)};
  size_t words = limbs[1] ? 2 : (limbs[0] ? 1 : 0);
  return compact ? _bi_from_limbs_or_small_int(sign, limbs, words)
                 : _bi_from_limbs(sign, limbs, words);
}

static inline bool _bi_try_submul_i64_int_pair(int64_t a, int64_t q_raw, int64_t b,
                                               int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  __int128 av = (__int128)rt_untag_v(a);
  __int128 bv = (__int128)rt_untag_v(b);
  *out = _bi_from_i128_value(av - (__int128)q_raw * bv, 1);
  return true;
}

static inline bool _bi_try_add_int_pair(int64_t a, int64_t b, int sub_b, int64_t *out) {
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

static inline bool _bi_try_submul_i64_int_pair_small(int64_t a, int64_t q_raw,
                                                     int64_t b,
                                                     int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  return _bi_try_i128_small_tag((__int128)rt_untag_v(a) -
                                    (__int128)q_raw * (__int128)rt_untag_v(b),
                                out);
}

static inline bool _bi_try_add_int_pair_small(int64_t a, int64_t b, int sub_b,
                                              int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  __int128 av = (__int128)rt_untag_v(a);
  __int128 bv = (__int128)rt_untag_v(b);
  return _bi_try_i128_small_tag(sub_b ? av - bv : av + bv, out);
}

static inline int64_t _bi_from_limbs_or_small_int(int sign, const uint64_t *limbs, size_t words) {
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

static inline size_t _bi_mag_add4(uint64_t out[4], const uint64_t *a, size_t aw, const uint64_t *b,
                                  size_t bw) {
  uint64_t carry = 0;
  size_t n = aw > bw ? aw : bw;
  for (size_t i = 0; i < n; i++) {
    __uint128_t s = (__uint128_t)(i < aw ? a[i] : 0) + (i < bw ? b[i] : 0) + carry;
    out[i] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
  }
  if (carry && n < 4)
    out[n++] = carry;
  return _bi_norm_words(out, n);
}

static inline size_t _bi_mag_sub4(uint64_t out[4], const uint64_t *a, size_t aw, const uint64_t *b,
                                  size_t bw) {
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

static inline bool _bi_try_add_limb2_kind(int64_t a, int64_t b, int sub_b, int compact,
                                          int64_t *out) {
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
  if (!_bi_read_limb2(a, &as, al, &aw) || !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (sub_b)
    bs = -bs;
  if (aw <= 1 && bw <= 1) {
    __int128 av = as < 0 ? -(__int128)al[0] : (as > 0 ? (__int128)al[0] : 0);
    __int128 bv = bs < 0 ? -(__int128)bl[0] : (bs > 0 ? (__int128)bl[0] : 0);
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
  *out = compact ? _bi_from_limbs_or_small_int(rs, rl, rw) : _bi_from_limbs(rs, rl, rw);
  return true;
}

static inline bool _bi_try_add_limb2(int64_t a, int64_t b, int sub_b, int64_t *out) {
  return _bi_try_add_limb2_kind(a, b, sub_b, 0, out);
}

static inline bool _bi_try_mul_limb2(int64_t a, int64_t b, int64_t *out) {
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t al[2], bl[2], rl[4] = {0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) || !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (as == 0 || bs == 0) {
    *out = _bi_from_limbs(0, rl, 0);
    return true;
  }
  for (size_t i = 0; i < aw; i++) {
    __uint128_t carry = 0;
    for (size_t j = 0; j < bw; j++) {
      __uint128_t cur = (__uint128_t)al[i] * bl[j] + rl[i + j] + carry;
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

static inline bool _bi_try_submul_limb2(int64_t a, int64_t q, int64_t b, int64_t *out) {
  int as = 0, qs = 0, bs = 0;
  size_t aw = 0, qw = 0, bw = 0;
  uint64_t al[2], ql[2], bl[2], pl[4] = {0, 0, 0, 0}, rl[5] = {0, 0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) || !_bi_read_limb2(q, &qs, ql, &qw) ||
      !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (qs == 0 || bs == 0) {
    *out = _bi_from_limbs_or_small_int(as, al, aw);
    return true;
  }

  for (size_t i = 0; i < qw; i++) {
    __uint128_t carry = 0;
    for (size_t j = 0; j < bw; j++) {
      __uint128_t cur = (__uint128_t)ql[i] * bl[j] + pl[i + j] + carry;
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
      __uint128_t s = (__uint128_t)(i < aw ? al[i] : 0) + (i < pw ? pl[i] : 0) + carry;
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
        uint64_t av = hi[i];
        uint64_t bv = i < low ? lo[i] : 0;
        uint64_t sub = bv + borrow;
        uint64_t next_borrow = (borrow && sub == 0) || av < sub;
        rl[i] = av - sub;
        borrow = next_borrow;
      }
      rs = cmp > 0 ? as : rhs_sign;
      rw = _bi_norm_words(rl, hiw);
    }
  }
  *out = _bi_from_limbs_or_small_int(rs, rl, rw);
  return true;
}

static inline bool _bi_try_submul_i64_limb2(int64_t a, int64_t q_raw, int64_t b,
                                            int64_t *out) {
  if (is_int(a) && is_int(b)) {
    _bi_try_submul_i64_int_pair(a, q_raw, b, out);
    return true;
  }
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t al[2], bl[2], pl[4] = {0, 0, 0, 0}, rl[5] = {0, 0, 0, 0, 0};
  if (!_bi_read_limb2(a, &as, al, &aw) || !_bi_read_limb2(b, &bs, bl, &bw))
    return false;
  if (q_raw == 0 || bs == 0) {
    *out = _bi_from_limbs_or_small_int(as, al, aw);
    return true;
  }
  if (aw <= 1 && bw <= 1) {
    __int128 av = as < 0 ? -(__int128)al[0] : (as > 0 ? (__int128)al[0] : 0);
    __int128 bv = bs < 0 ? -(__int128)bl[0] : (bs > 0 ? (__int128)bl[0] : 0);
    *out = _bi_from_i128_value(av - (__int128)q_raw * bv, 1);
    return true;
  }

  int qs = q_raw < 0 ? -1 : 1;
  uint64_t qmag = q_raw < 0 ? (uint64_t)(-(q_raw + 1)) + 1u : (uint64_t)q_raw;
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
      __uint128_t s = (__uint128_t)(i < aw ? al[i] : 0) + (i < pw ? pl[i] : 0) + add_carry;
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
        uint64_t av = hi[i];
        uint64_t bv = i < low ? lo[i] : 0;
        uint64_t sub = bv + borrow;
        uint64_t next_borrow = (borrow && sub == 0) || av < sub;
        rl[i] = av - sub;
        borrow = next_borrow;
      }
      rs = cmp > 0 ? as : rhs_sign;
      rw = _bi_norm_words(rl, hiw);
    }
  }
  *out = _bi_from_limbs_or_small_int(rs, rl, rw);
  return true;
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
    mpz_import(result, (size_t)words, -1, sizeof(uint64_t), 0, 0, (const void *)((char *)v + 16));
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

  *(int64_t *)(uintptr_t)((char *)p + 0) = rt_tag_v((int64_t)(sign > 0 ? 1 : -1));
  if (word_count > 0 && words)
    memcpy((void *)((char *)p + 16), words, word_count * sizeof(uint64_t));
  free(words);

  return p;
}

int64_t rt_bigint_add(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_add_limb2(a, b, 0, &fast))
    return fast;
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
}

int64_t rt_bigint_sub(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_add_limb2(a, b, 1, &fast))
    return fast;
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
}

int64_t rt_bigint_mul(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_mul_limb2(a, b, &fast))
    return fast;
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
}

int64_t rt_bigint_submul(int64_t a, int64_t q, int64_t b) {
  if (q == rt_tag_v(0) || _bi_is_zero_fast(b))
    return a;
  int64_t fast = 0;
  if (q == rt_tag_v(1) && _bi_try_add_limb2_kind(a, b, 1, 1, &fast))
    return fast;
  if (q == rt_tag_v(-1) && _bi_try_add_limb2_kind(a, b, 0, 1, &fast))
    return fast;
  if (is_int(q) && _bi_try_submul_i64_limb2(a, rt_untag_v(q), b, &fast))
    return fast;
  if (_bi_try_submul_limb2(a, q, b, &fast))
    return fast;
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
}

int64_t rt_bigint_row_submul(int64_t row_k, int64_t row_j, int64_t q, int64_t limit_v) {
  if (!is_ptr(row_k) || !is_ptr(row_j))
    return row_k;
  int64_t limit = is_int(limit_v) ? rt_untag_v(limit_v) : limit_v;
  if (limit < 0 || q == rt_tag_v(0))
    return row_k;
  int64_t *rk = (int64_t *)(uintptr_t)((char *)row_k + 16);
  const int64_t *rj = (const int64_t *)(uintptr_t)((char *)row_j + 16);
  if (is_int(q)) {
    int64_t q_raw = rt_untag_v(q);
    if (q_raw == 1 || q_raw == -1) {
      int sub_b = q_raw == 1;
      for (int64_t c = 0; c <= limit; c++) {
        int64_t b = rj[c];
        if (_bi_is_zero_fast(b))
          continue;
        int64_t fast = 0;
        rk[c] = (_bi_try_add_int_pair_small(rk[c], b, sub_b, &fast) ||
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
      rk[c] = (_bi_try_submul_i64_int_pair_small(rk[c], q_raw, b, &fast) ||
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

int64_t rt_bigint_row_submul_auto(int64_t row_k, int64_t row_j, int64_t q) {
  if (!is_ptr(row_k) || !is_ptr(row_j) || q == rt_tag_v(0))
    return row_k;
  int64_t tagged_len = *(int64_t *)(uintptr_t)((char *)row_j + 0);
  int64_t limit = is_int(tagged_len) ? rt_untag_v(tagged_len) - 1 : tagged_len - 1;
  if (limit < 0)
    return row_k;
  const int64_t *rj = (const int64_t *)(uintptr_t)((char *)row_j + 16);
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
        rk[c] = (_bi_try_add_int_pair_small(rk[c], b, sub_b, &fast) ||
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
      rk[c] = (_bi_try_submul_i64_int_pair_small(rk[c], q_raw, b, &fast) ||
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
}

int64_t rt_bigint_mod(int64_t a, int64_t b) {
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
}

static int64_t _bi_binary_bitop(int64_t a, int64_t b,
                                void (*op)(mpz_t, const mpz_t, const mpz_t)) {
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

int64_t rt_bigint_or(int64_t a, int64_t b) { return _bi_binary_bitop(a, b, mpz_ior); }

int64_t rt_bigint_xor(int64_t a, int64_t b) { return _bi_binary_bitop(a, b, mpz_xor); }

int64_t rt_bigint_cmp(int64_t a, int64_t b) {
  mpz_t ma, mb;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  int r = mpz_cmp(ma, mb);
  mpz_clear(ma);
  mpz_clear(mb);
  return rt_tag_v((int64_t)(r > 0 ? 1 : (r < 0 ? -1 : 0)));
}

int64_t rt_bigint_to_str(int64_t a) {
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  char *s = mpz_get_str(NULL, 10, ma);
  int64_t r = rt_alloc_string(s);
  free(s);
  mpz_clear(ma);
  return r;
}

int64_t rt_bigint_from_str(int64_t str_ptr) {
  if (!is_v_str(str_ptr))
    return 0;
  const char *s = (const char *)(uintptr_t)str_ptr;
  mpz_t val;
  mpz_init(val);
  if (mpz_set_str(val, s, 10) != 0) {
    mpz_clear(val);
    return 0;
  }
  int64_t r = _bi_from_mpz(val);
  mpz_clear(val);
  return r;
}

int64_t rt_bigint_to_bytes(int64_t a) {
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  if (mpz_sgn(ma) == 0) {
    mpz_clear(ma);
    int64_t res = rt_list_new(1);
    res = rt_append(res, rt_tag_v(0));
    return res;
  }

  size_t count = 0;
  unsigned char *buf = (unsigned char *)mpz_export(NULL, &count, 1, 1, 1, 0, ma);
  int64_t res = rt_list_new((count << 1) | 1);
  for (size_t i = 0; i < count; ++i) {
    res = rt_append(res, rt_tag_v((int64_t)buf[i]));
  }
  free(buf);
  mpz_clear(ma);
  return res;
}

int64_t rt_bigint_powmod(int64_t base, int64_t exp, int64_t mod) {
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
}

int64_t rt_bigint_modinv(int64_t a, int64_t m) {
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
}

int64_t rt_bigint_gcd(int64_t a, int64_t b) {
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
}

int64_t rt_bigint_bitlen(int64_t a) {
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  size_t r = mpz_sizeinbase(ma, 2);
  mpz_clear(ma);
  return rt_tag_v((int64_t)r);
}

int64_t rt_bigint_popcount(int64_t a) {
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  size_t r = mpz_popcount(ma);
  mpz_clear(ma);
  return rt_tag_v((int64_t)r);
}

int64_t rt_bigint_to_int(int64_t a) {
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  int64_t v = _bi_mpz_get_i64(ma);
  mpz_clear(ma);
  return rt_tag_v(v);
}

static double _bi_to_double_fast(int64_t a) {
  if (is_int(a)) {
    return (double)rt_untag_v(a);
  } else if (is_ptr(a)) {
    int64_t tag = *(int64_t *)(uintptr_t)((char *)a - 8);
    if (tag == TAG_BIGINT) {
      int64_t sign = rt_untag_v(*(int64_t *)(uintptr_t)((char *)a + 0));
      int64_t words = rt_untag_v(*(int64_t *)(uintptr_t)((char *)a + 8));
      if (sign != 0 && words > 0) {
        const uint64_t *src = (const uint64_t *)((char *)a + 16);
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

int64_t rt_bigint_from_int(int64_t n) { return _bi_from_i64(rt_untag_v(n)); }

static int64_t rt_bigint_from_bytes_be(const uint8_t *bytes, size_t len) {
  mpz_t val;
  mpz_init(val);
  if (bytes && len > 0)
    mpz_import(val, len, 1, 1, 1, 0, bytes);
  int64_t out = _bi_from_mpz(val);
  mpz_clear(val);
  return out;
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
  return rt_bigint_from_bytes_be((const uint8_t *)(uintptr_t)v, (size_t)len);
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

  if (tag == TAG_BIGINT)
    return v;
  if (tag == TAG_BYTES)
    return rt_long_bytes_like(v);
  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    if (!rt_addr_readable((uintptr_t)v, 16))
      return _bi_from_i64(0);
    int64_t tagged_len = *(int64_t *)((char *)(uintptr_t)v + 0);
    int64_t len = is_int(tagged_len) ? (tagged_len >> 1) : tagged_len;
    if (len <= 0)
      return _bi_from_i64(0);
    size_t need = 16u + (size_t)len * sizeof(int64_t);
    if (!rt_addr_readable((uintptr_t)v, need))
      return _bi_from_i64(0);
    mpz_t out;
    mpz_init(out);
    for (int64_t i = 0; i < len; ++i) {
      int64_t item = *(int64_t *)((char *)(uintptr_t)v + 16 + i * 8);
      unsigned long byte = is_int(item) ? (unsigned long)(rt_untag_v(item) & 255) : 0ul;
      mpz_mul_ui(out, out, 256);
      mpz_add_ui(out, out, byte);
    }
    int64_t boxed = _bi_from_mpz(out);
    mpz_clear(out);
    return boxed;
  }

  return _bi_from_i64(0);
}

int64_t rt_bigint_pow(int64_t b, int64_t e) {
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
}
int64_t rt_bigint_isqrt(int64_t a) {
  mpz_t ma, mr;
  _bi_val_to_mpz(a, ma);
  mpz_init(mr);
  mpz_sqrt(mr, ma);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mr);
  return r;
}
int64_t rt_bigint_legendre(int64_t a, int64_t p) {
  mpz_t ma, mp;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(p, mp);
  int r = mpz_legendre(ma, mp);
  mpz_clear(ma);
  mpz_clear(mp);
  return rt_tag_v(r);
}
int64_t rt_bigint_jacobi(int64_t a, int64_t n) {
  mpz_t ma, mn;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(n, mn);
  int r = mpz_jacobi(ma, mn);
  mpz_clear(ma);
  mpz_clear(mn);
  return rt_tag_v(r);
}
int64_t rt_bigint_kronecker(int64_t a, int64_t n) {
  mpz_t ma, mn;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(n, mn);
  int r = mpz_kronecker(ma, mn);
  mpz_clear(ma);
  mpz_clear(mn);
  return rt_tag_v(r);
}
int64_t rt_bigint_iroot(int64_t n, int64_t k) {
  mpz_t mn, mr;
  _bi_val_to_mpz(n, mn);
  mpz_init(mr);
  mpz_root(mr, mn, (unsigned long)rt_untag_v(k));
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(mn);
  mpz_clear(mr);
  return r;
}
int64_t rt_bigint_is_perfect_square(int64_t n) {
  mpz_t mn;
  _bi_val_to_mpz(n, mn);
  int r = mpz_perfect_square_p(mn);
  mpz_clear(mn);
  return rt_tag_v(r);
}
int64_t rt_bigint_xgcd(int64_t a, int64_t b) {
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
}
int64_t rt_bigint_clz(int64_t a) {
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
}
int64_t rt_bigint_ctz(int64_t a) {
  mpz_t ma;
  _bi_val_to_mpz(a, ma);
  size_t r = mpz_scan1(ma, 0);
  mpz_clear(ma);
  return rt_tag_v((int64_t)r);
}

static long _bi_gf2_deg(const mpz_t a) {
  if (mpz_sgn(a) == 0)
    return -1;
  return (long)mpz_sizeinbase(a, 2) - 1;
}

static void _bi_gf2_mod_mpz(mpz_t out, const mpz_t a, const mpz_t m) {
  mpz_set(out, a);
  long m_deg = _bi_gf2_deg(m);
  if (m_deg < 0) {
    mpz_set_ui(out, 0);
    return;
  }
  mpz_t shifted;
  mpz_init(shifted);
  for (;;) {
    long a_deg = _bi_gf2_deg(out);
    if (a_deg < m_deg)
      break;
    mpz_mul_2exp(shifted, m, (mp_bitcnt_t)(a_deg - m_deg));
    mpz_xor(out, out, shifted);
  }
  mpz_clear(shifted);
}

static void _bi_gf2_div_qr_mpz(mpz_t q, mpz_t r, const mpz_t a, const mpz_t b) {
  mpz_set_ui(q, 0);
  mpz_set(r, a);
  long b_deg = _bi_gf2_deg(b);
  if (b_deg < 0)
    return;
  mpz_t shifted;
  mpz_init(shifted);
  for (;;) {
    long r_deg = _bi_gf2_deg(r);
    if (r_deg < b_deg)
      break;
    mp_bitcnt_t shift = (mp_bitcnt_t)(r_deg - b_deg);
    mpz_setbit(q, shift);
    mpz_mul_2exp(shifted, b, shift);
    mpz_xor(r, r, shifted);
  }
  mpz_clear(shifted);
}

static void _bi_gf2_mulmod_mpz(mpz_t out, const mpz_t a, const mpz_t b, const mpz_t m) {
  long m_deg = _bi_gf2_deg(m);
  if (m_deg < 0) {
    mpz_set_ui(out, 0);
    return;
  }
  mpz_t va, vb;
  mpz_init(va);
  mpz_init_set(vb, b);
  _bi_gf2_mod_mpz(va, a, m);
  mpz_set_ui(out, 0);
  while (mpz_sgn(vb) != 0) {
    if (mpz_tstbit(vb, 0))
      mpz_xor(out, out, va);
    mpz_fdiv_q_2exp(vb, vb, 1);
    if (mpz_sgn(vb) != 0) {
      mpz_mul_2exp(va, va, 1);
      if (mpz_tstbit(va, (mp_bitcnt_t)m_deg))
        mpz_xor(va, va, m);
    }
  }
  mpz_clear(va);
  mpz_clear(vb);
}

int64_t rt_bigint_gf2_mod(int64_t a, int64_t m) {
  mpz_t ma, mm, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(m, mm);
  mpz_init(mr);
  _bi_gf2_mod_mpz(mr, ma, mm);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mm);
  mpz_clear(mr);
  return r;
}

int64_t rt_bigint_gf2_mulmod(int64_t a, int64_t b, int64_t m) {
  mpz_t ma, mb, mm, mr;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(b, mb);
  _bi_val_to_mpz(m, mm);
  mpz_init(mr);
  _bi_gf2_mulmod_mpz(mr, ma, mb, mm);
  int64_t r = _bi_from_mpz(mr);
  mpz_clear(ma);
  mpz_clear(mb);
  mpz_clear(mm);
  mpz_clear(mr);
  return r;
}

int64_t rt_bigint_gf2_inv(int64_t a, int64_t m) {
  mpz_t ma, mm, r0, r1, t0, t1, q, rem, prod, next_t;
  _bi_val_to_mpz(a, ma);
  _bi_val_to_mpz(m, mm);
  mpz_init_set(r0, mm);
  mpz_init(r1);
  _bi_gf2_mod_mpz(r1, ma, mm);
  mpz_init_set_ui(t0, 0);
  mpz_init_set_ui(t1, 1);
  mpz_inits(q, rem, prod, next_t, NULL);

  while (mpz_sgn(r1) != 0) {
    _bi_gf2_div_qr_mpz(q, rem, r0, r1);
    mpz_set(r0, r1);
    mpz_set(r1, rem);
    _bi_gf2_mulmod_mpz(prod, q, t1, mm);
    mpz_xor(next_t, t0, prod);
    _bi_gf2_mod_mpz(next_t, next_t, mm);
    mpz_set(t0, t1);
    mpz_set(t1, next_t);
  }

  int64_t out;
  if (_bi_gf2_deg(r0) > 0)
    out = _bi_from_i64(0);
  else
    out = _bi_from_mpz(t0);

  mpz_clears(ma, mm, r0, r1, t0, t1, q, rem, prod, next_t, NULL);
  return out;
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
  mpz_t val;
  mpz_init(val);
  _bi_mpz_set_i64(val, v);
  int64_t r = _bi_from_mpz(val);
  mpz_clear(val);
  return r;
}
