/*
 * BigInt runtime: native arbitrary-precision integer arithmetic with
 * tagged-value boxing, and NyValue conversion helpers.
 *
 * References:
 *  Brent & Zimmermann — Modern Computer Arithmetic, Ch.1–4:
 *   schoolbook, Karatsuba, Toom-Cook, FFT/NTT multiplication.
 *  Menezes, van Oorschot, Vanstone — Handbook of Applied
 *   Cryptography, Ch.14 §14.3: multi-limb arithmetic.
 */
#include "code/lang/runtime/runtime.h"
#include "code/lang/runtime/shared.h"
#include "base/ascii.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int64_t rt_tagof(int64_t v);
extern int64_t rt_list_new(int64_t n_v);
extern int64_t rt_append(int64_t lst, int64_t val);
extern int64_t rt_alloc_string(const char *s);
int64_t rt_bigint_from_i64_raw(int64_t v);

/*
 * Validate an entire address range before pointer arithmetic or dereference.
 * Several public helpers accept raw native pointers, so guarding only the
 * first byte is not enough: start + len may wrap uintptr_t before the runtime
 * readability probe sees it.
 */
static inline bool _bi_readable_range(uintptr_t start, size_t len) {
  if (len == 0)
    return true;
  if (start == 0 || len > (size_t)(UINTPTR_MAX - start))
    return false;
  return rt_addr_readable(start, len);
}

static inline int64_t _bi_i64_from_u64_bits(uint64_t bits) {
  int64_t out = 0;
  memcpy(&out, &bits, sizeof(out));
  return out;
}

static inline bool _bi_tag_size(size_t n, int64_t *tagged) {
  if (!tagged || n > (size_t)NY_SMALL_INT_MAX)
    return false;
  *tagged = rt_tag_v((int64_t)n);
  return true;
}

static inline int64_t _bi_alloc(size_t word_count) {
  if (word_count > (SIZE_MAX - 16u) / sizeof(uint64_t))
    return 0;
  size_t bytes = 16u + word_count * sizeof(uint64_t);
  if (bytes > (size_t)INT64_MAX)
    return 0;
  /*
   * Constructors initialize sign, count and every live limb before the
   * object escapes. Avoid zeroing that payload only to overwrite it.
   */
  int64_t p = rt_malloc_uninit((int64_t)bytes);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p - 8) = TAG_BIGINT;
  *(int64_t *)(uintptr_t)((char *)p + 8) = rt_tag_v((int64_t)word_count);
  return p;
}

static int64_t _bi_from_i64(int64_t v);
static inline bool _bi_read_heap_limbs(int64_t v, int *sign,
                                       const uint64_t **limbs, size_t *words,
                                       uint64_t small[1]);

typedef struct {
  int sign;
  size_t count;
  uint64_t *d;
} _bn_t;

static size_t _bn_bitlen(const _bn_t *a);
static void _bn_shl1(_bn_t *a);
static void _bn_shl(_bn_t *r, const _bn_t *a, size_t bits);
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
static int _bn_grow_checked(_bn_t *a, size_t n) {
  if (n <= a->count)
    return n == 0 || a->d != NULL;
  if (n > SIZE_MAX / sizeof(uint64_t))
    return 0;
  uint64_t *nd = (uint64_t *)realloc(a->d, n * sizeof(uint64_t));
  if (!nd)
    return 0;
  memset(nd + a->count, 0, (n - a->count) * sizeof(uint64_t));
  a->d = nd;
  a->count = n;
  return 1;
}

/*
 * Legacy shape for read-mostly callers; see _bn_grow_checked for OOM.
 */
static void _bn_grow(_bn_t *a, size_t n) {
  if (!_bn_grow_checked(a, n)) {
    fprintf(stderr, "nytrix: out of memory growing bigint\n");
    abort();
  }
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
  if (!nd) {
    fprintf(stderr, "nytrix: out of memory setting bigint\n");
    abort();
  }
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
  if (!nd) {
    fprintf(stderr, "nytrix: out of memory copying bigint\n");
    abort();
  }
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
#define KARATSUBA_THRESHOLD 176

/*
 * Two carry chains accumulate the low product and the previous high
 * product independently. Runtime dispatch keeps the baseline portable.
 */
#if defined(__x86_64__) && !defined(_WIN32) &&                                 \
    (defined(__GNUC__) || defined(__clang__))
static inline bool _bn_has_adx(void) {
  return __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("adx");
}
static uint64_t _bn_addmul_adx(uint64_t *r, const uint64_t *a, size_t n,
                               uint64_t q) {
  uint64_t carry = 0;
  size_t peel = n & 3u;
  for (size_t i = 0; i < peel; ++i) {
    __uint128_t product = (__uint128_t)a[i] * q + carry;
    product += r[i];
    r[i] = (uint64_t)product;
    carry = (uint64_t)(product >> 64);
  }
  r += peel;
  a += peel;
  n -= peel;
  if (!n)
    return carry;
  __asm__ volatile("xorq %%rax, %%rax\n\t"
                   "1:\n\t"
                   "mulxq (%[a]), %%r9, %%r10\n\t"
                   "adcxq (%[r]), %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "movq %%r9, (%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "mulxq 8(%[a]), %%r9, %%r10\n\t"
                   "adcxq 8(%[r]), %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "movq %%r9, 8(%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "mulxq 16(%[a]), %%r9, %%r10\n\t"
                   "adcxq 16(%[r]), %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "movq %%r9, 16(%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "mulxq 24(%[a]), %%r9, %%r10\n\t"
                   "adcxq 24(%[r]), %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "movq %%r9, 24(%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "leaq 32(%[a]), %[a]\n\t"
                   "leaq 32(%[r]), %[r]\n\t"
                   "leaq -4(%%rcx), %%rcx\n\t"
                   "jrcxz 2f\n\t"
                   "jmp 1b\n\t"
                   "2: movq $0, %%rax\n\t"
                   "adcxq %%rax, %[carry]\n\t"
                   "adoxq %%rax, %[carry]"
                   : [r] "+&r"(r), [a] "+&r"(a), "+&c"(n), [carry] "+&r"(carry)
                   : "d"(q)
                   : "rax", "r9", "r10", "cc", "memory");
  return carry;
}
static uint64_t _bn_submul_adx(uint64_t *r, const uint64_t *a, size_t n,
                               uint64_t q) {
  uint64_t carry = 0;
  size_t peel = n & 3u;
  for (size_t i = 0; i < peel; ++i) {
    __uint128_t product = (__uint128_t)a[i] * q + carry;
    uint64_t low = (uint64_t)product, old = r[i];
    r[i] = old - low;
    carry = (uint64_t)(product >> 64) + (old < low);
  }
  r += peel;
  a += peel;
  n -= peel;
  if (!n)
    return carry;
  __asm__ volatile("xorq %%rax, %%rax\n\t"
                   "1:\n\t"
                   "mulxq (%[a]), %%r9, %%r10\n\t"
                   "movq (%[r]), %%rax\n\t"
                   "notq %%rax\n\t"
                   "adcxq %%rax, %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "notq %%r9\n\t"
                   "movq %%r9, (%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "mulxq 8(%[a]), %%r9, %%r10\n\t"
                   "movq 8(%[r]), %%rax\n\t"
                   "notq %%rax\n\t"
                   "adcxq %%rax, %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "notq %%r9\n\t"
                   "movq %%r9, 8(%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "mulxq 16(%[a]), %%r9, %%r10\n\t"
                   "movq 16(%[r]), %%rax\n\t"
                   "notq %%rax\n\t"
                   "adcxq %%rax, %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "notq %%r9\n\t"
                   "movq %%r9, 16(%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "mulxq 24(%[a]), %%r9, %%r10\n\t"
                   "movq 24(%[r]), %%rax\n\t"
                   "notq %%rax\n\t"
                   "adcxq %%rax, %%r9\n\t"
                   "adoxq %[carry], %%r9\n\t"
                   "notq %%r9\n\t"
                   "movq %%r9, 24(%[r])\n\t"
                   "movq %%r10, %[carry]\n\t"
                   "leaq 32(%[a]), %[a]\n\t"
                   "leaq 32(%[r]), %[r]\n\t"
                   "leaq -4(%%rcx), %%rcx\n\t"
                   "jrcxz 2f\n\t"
                   "jmp 1b\n\t"
                   "2: movq $0, %%rax\n\t"
                   "adcxq %%rax, %[carry]\n\t"
                   "adoxq %%rax, %[carry]"
                   : [r] "+&r"(r), [a] "+&r"(a), "+&c"(n), [carry] "+&r"(carry)
                   : "d"(q)
                   : "rax", "r9", "r10", "cc", "memory");
  return carry;
}
#else
static inline bool _bn_has_adx(void) { return false; }
#endif

/*
 * A single workspace serves the entire recursion. Inputs are read-only;
 * the low and high products occupy their final output ranges.
 */
static size_t _bn_words_trim(const uint64_t *a, size_t n) {
  while (n && a[n - 1] == 0)
    --n;
  return n;
}

static size_t _bn_words_sum(uint64_t *r, const uint64_t *a, size_t an,
                            const uint64_t *b, size_t bn) {
  size_t n = an > bn ? an : bn;
  uint64_t carry = 0;
  for (size_t i = 0; i < n; ++i) {
    __uint128_t v =
        (__uint128_t)(i < an ? a[i] : 0) + (i < bn ? b[i] : 0) + carry;
    r[i] = (uint64_t)v;
    carry = (uint64_t)(v >> 64);
  }
  r[n] = carry;
  return n + (carry != 0);
}

static void _bn_words_sub(uint64_t *r, size_t rn, const uint64_t *a,
                          size_t an) {
  uint64_t borrow = 0;
  for (size_t i = 0; i < rn; ++i) {
    uint64_t v = i < an ? a[i] : 0;
    uint64_t sub = v + borrow;
    uint64_t next = (borrow && sub == 0) || r[i] < sub;
    r[i] -= sub;
    borrow = next;
  }
}

/*
 * Column accumulation keeps a two-limb carry and evaluates each
 * off-diagonal square product once.
 */
static void _bn_words_square(uint64_t *r, const uint64_t *a, size_t n) {
  uint64_t c0 = 0, c1 = 0, c2 = 0;
  for (size_t k = 0; k < 2 * n - 1; ++k) {
    size_t first = k < n ? 0 : k - n + 1;
    size_t last = k / 2;
    for (size_t i = first; i <= last; ++i) {
      size_t j = k - i;
      __uint128_t product = (__uint128_t)a[i] * a[j];
      uint64_t lo = (uint64_t)product, hi = (uint64_t)(product >> 64);
      unsigned top = 0;
      if (i != j) {
        top = (unsigned)(hi >> 63);
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
      }
      __uint128_t sum = (__uint128_t)c0 + lo;
      c0 = (uint64_t)sum;
      sum = (__uint128_t)c1 + hi + (uint64_t)(sum >> 64);
      c1 = (uint64_t)sum;
      c2 += top + (uint64_t)(sum >> 64);
    }
    r[k] = c0;
    c0 = c1;
    c1 = c2;
    c2 = 0;
  }
  r[2 * n - 1] = c0;
}

static void _bn_words_mul(uint64_t *r, const uint64_t *a, size_t an,
                          const uint64_t *b, size_t bn, uint64_t *work) {
  size_t n = an + bn;
  memset(r, 0, n * sizeof(*r));
  an = _bn_words_trim(a, an);
  bn = _bn_words_trim(b, bn);
  n = an + bn;
  if (!an || !bn)
    return;
  size_t max = an > bn ? an : bn;
  size_t k = max / 2;
  if (an <= KARATSUBA_THRESHOLD || bn <= KARATSUBA_THRESHOLD || an <= k ||
      bn <= k) {
#if defined(__x86_64__) && !defined(_WIN32) &&                                 \
    (defined(__GNUC__) || defined(__clang__))
    if (_bn_has_adx()) {
      if (a == b && an == bn) {
        for (size_t i = 0; i + 1 < an; ++i)
          r[i + an] =
              _bn_addmul_adx(r + 2 * i + 1, a + i + 1, an - i - 1, a[i]);
        uint64_t carry = 0;
        for (size_t i = 0; i < 2 * an; ++i) {
          uint64_t next = r[i] >> 63;
          r[i] = (r[i] << 1) | carry;
          carry = next;
        }
        for (size_t i = 0; i < an; ++i) {
          __uint128_t product = (__uint128_t)a[i] * a[i];
          __uint128_t sum = (__uint128_t)r[2 * i] + (uint64_t)product;
          r[2 * i] = (uint64_t)sum;
          sum = (__uint128_t)r[2 * i + 1] + (uint64_t)(product >> 64) +
                (uint64_t)(sum >> 64);
          r[2 * i + 1] = (uint64_t)sum;
          carry = (uint64_t)(sum >> 64);
          for (size_t j = 2 * i + 2; carry && j < 2 * an; ++j) {
            uint64_t old = r[j];
            r[j] += carry;
            carry = r[j] < old;
          }
        }
        return;
      }
      for (size_t i = 0; i < an; ++i)
        r[i + bn] = _bn_addmul_adx(r + i, b, bn, a[i]);
      return;
    }
#endif
    if (a == b && an == bn) {
      _bn_words_square(r, a, an);
      return;
    }
    for (size_t i = 0; i < an; ++i) {
      uint64_t carry = 0;
      for (size_t j = 0; j < bn; ++j) {
        __uint128_t v = (__uint128_t)a[i] * b[j] + r[i + j] + carry;
        r[i + j] = (uint64_t)v;
        carry = (uint64_t)(v >> 64);
      }
      r[i + bn] = carry;
    }
    return;
  }
  size_t ah = an - k, bh = bn - k;
  size_t sa_cap = (k > ah ? k : ah) + 1;
  size_t sb_cap = (k > bh ? k : bh) + 1;
  uint64_t *sa = work, *sb = sa + sa_cap;
  uint64_t *mid = sb + sb_cap;
  uint64_t *next = mid + sa_cap + sb_cap;
  bool square = a == b && an == bn;
  size_t san, sbn;
  if (square) {
    const uint64_t *hi = a + k, *lo = a;
    size_t hn = ah, ln = k;
    size_t low_n = _bn_words_trim(a, k);
    size_t high_n = _bn_words_trim(a + k, ah);
    bool high_greater = high_n > low_n;
    if (high_n == low_n) {
      for (size_t i = high_n; i-- > 0;)
        if (a[k + i] != a[i]) {
          high_greater = a[k + i] > a[i];
          break;
        }
    }
    if (!high_greater) {
      hi = a;
      lo = a + k;
      hn = k;
      ln = ah;
    }
    memcpy(sa, hi, hn * sizeof(*sa));
    _bn_words_sub(sa, hn, lo, ln);
    san = _bn_words_trim(sa, hn);
    sb = sa;
    sbn = san;
  } else {
    san = _bn_words_sum(sa, a, k, a + k, ah);
    sbn = _bn_words_sum(sb, b, k, b + k, bh);
  }
  _bn_words_mul(r, a, k, b, k, next);
  _bn_words_mul(r + 2 * k, a + k, ah, b + k, bh, next);
  _bn_words_mul(mid, sa, san, sb, sbn, next);
  size_t mn = san + sbn;
  if (square) {
    size_t cross_n = _bn_words_sum(next, r, 2 * k, r + 2 * k, ah + bh);
    _bn_words_sub(next, cross_n, mid, mn);
    mid = next;
    mn = cross_n;
  } else {
    _bn_words_sub(mid, mn, r, 2 * k);
    _bn_words_sub(mid, mn, r + 2 * k, ah + bh);
  }
  mn = _bn_words_trim(mid, mn);
  uint64_t carry = 0;
  size_t i = 0;
  for (; i < mn; ++i) {
    __uint128_t v = (__uint128_t)r[k + i] + mid[i] + carry;
    r[k + i] = (uint64_t)v;
    carry = (uint64_t)(v >> 64);
  }
  for (i += k; carry && i < n; ++i) {
    uint64_t old = r[i];
    r[i] += carry;
    carry = r[i] < old;
  }
}

static void _bn_mul_karatsuba(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  if (_bn_is_zero(a) || _bn_is_zero(b)) {
    _bn_set_zero(r);
    return;
  }
  size_t n = a->count + b->count;
  if (n < a->count || n > (SIZE_MAX / sizeof(uint64_t) - 64) / 8) {
    fprintf(stderr, "nytrix: bigint product too large\n");
    abort();
  }
  uint64_t stack_work[1024];
  size_t wn = 8 * n + 64;
  uint64_t *work = wn <= 1024 ? stack_work : malloc(wn * sizeof(*work));
  if (!work) {
    fprintf(stderr, "nytrix: out of memory multiplying bigint\n");
    abort();
  }
  _bn_grow(r, n);
  _bn_words_mul(r->d, a->d, a->count, b->d, b->count, work);
  r->count = n;
  r->sign = a->sign == b->sign ? 1 : -1;
  _bn_trim(r);
  if (work != stack_work)
    free(work);
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
  if (a == b) {
    bp = ap;
  } else if (r == b) {
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
static void _bn_divmod_knuth_impl(_bn_t *q, _bn_t *r, const _bn_t *a,
                                  const _bn_t *b, bool want_remainder) {
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
    if (rem) {
      _bn_grow(r, 1);
      r->d[0] = rem;
      r->count = 1;
      r->sign = 1;
    } else {
      _bn_set_zero(r);
    }
    return;
  }

  size_t m = a->count - n;
  unsigned shift = (unsigned)__builtin_clzll(b->d[n - 1]);
  /*
   * Short operands need only bounded scratch; retain heap storage for large
   * division. Every consumed word is assigned below, including un's extra
   * normalization word when no shift is required.
   */
  uint64_t vn_stack[128], un_stack[257];
  bool short_operands = n <= 128 && a->count <= 256;
  bool alloc_ok = n <= SIZE_MAX / sizeof(uint64_t) &&
                  a->count < SIZE_MAX / sizeof(uint64_t);
  uint64_t *vn = short_operands ? vn_stack
                               : alloc_ok ? malloc(n * sizeof(*vn)) : NULL;
  uint64_t *un = short_operands
                     ? un_stack
                     : alloc_ok ? malloc((a->count + 1) * sizeof(*un)) : NULL;
  _bn_grow(q, m + 1);
  uint64_t *qd = q->d;
  if (!vn || !un || !qd) {
    if (!short_operands) {
      free(vn);
      free(un);
    }
    _bn_divmod_binary(q, r, a, b);
    return;
  }
  if (shift == 0) {
    memcpy(vn, b->d, n * sizeof(*vn));
    memcpy(un, a->d, a->count * sizeof(*un));
    un[a->count] = 0;
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

  uint64_t divisor = vn[n - 1];
  uint64_t inverse = (uint64_t)((~(__uint128_t)0) / divisor);
#if defined(__x86_64__) && !defined(_WIN32) &&                                 \
    (defined(__GNUC__) || defined(__clang__))
  bool use_adx = _bn_has_adx();
#endif
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
      uint64_t high = un[j + n], low = un[j + n - 1];
      /*
       * With B=2^64 and R=floor((B^2-1)/d),
       * floor((high*R+low)/B) underestimates (high*B+low)/d
       * by at most two when B/2 <= d < B and high < d.
       */
      __uint128_t estimate = (__uint128_t)high * inverse + low;
      qhat = high + (uint64_t)(estimate >> 64);
      __uint128_t rem =
          (((__uint128_t)high << 64) | low) - (__uint128_t)qhat * divisor;
      while (rem >= divisor) {
        ++qhat;
        rem -= divisor;
      }
      rhat = (uint64_t)rem;
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
#if defined(__x86_64__) && !defined(_WIN32) &&                                 \
    (defined(__GNUC__) || defined(__clang__))
    if (use_adx) {
      borrow = _bn_submul_adx(un + j, vn, n, qhat);
    } else
#endif
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
  if (want_remainder)
    _bn_grow(r, n);
  if (q->count < m + 1 || (want_remainder && r->count < n)) {
    if (!short_operands) {
      free(vn);
      free(un);
    }
    _bn_divmod_binary(q, r, a, b);
    return;
  }
  q->count = m + 1;
  q->sign = 1;
  _bn_trim(q);
  if (want_remainder) {
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
    _bn_trim(r);
    r->sign = _bn_is_zero(r) ? 0 : 1;
  }
  if (!short_operands) {
    free(vn);
    free(un);
  }
}

static void _bn_divmod_knuth(_bn_t *q, _bn_t *r, const _bn_t *a,
                             const _bn_t *b) {
  _bn_divmod_knuth_impl(q, r, a, b, true);
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

static int _bn_words_cmp(const uint64_t *a, const uint64_t *b, size_t n) {
  for (size_t i = n; i-- > 0;)
    if (a[i] != b[i])
      return a[i] > b[i] ? 1 : -1;
  return 0;
}
static uint64_t _bn_words_sub_n(uint64_t *r, const uint64_t *a, size_t n) {
  uint64_t borrow = 0;
  for (size_t i = 0; i < n; ++i) {
    uint64_t sub = a[i] + borrow;
    uint64_t next = (borrow && sub == 0) || r[i] < sub;
    r[i] -= sub;
    borrow = next;
  }
  return borrow;
}
static uint64_t _bn_words_add_n(uint64_t *r, const uint64_t *a, size_t n) {
  uint64_t carry = 0;
  for (size_t i = 0; i < n; ++i) {
    __uint128_t sum = (__uint128_t)r[i] + a[i] + carry;
    r[i] = (uint64_t)sum;
    carry = (uint64_t)(sum >> 64);
  }
  return carry;
}
static void _bn_div_words_base(uint64_t *q, uint64_t *r, const uint64_t *a,
                               const uint64_t *b, size_t n, uint64_t *u) {
  memcpy(u, a, 2 * n * sizeof(*u));
  uint64_t d = b[n - 1];
  uint64_t inv = (uint64_t)((~(__uint128_t)0) / d);
  for (size_t j = n; j-- > 0;) {
    uint64_t qhat, rhat;
    bool carried = false;
    uint64_t high = u[j + n], low = u[j + n - 1];
    if (high == d) {
      qhat = UINT64_MAX;
      rhat = low + d;
      carried = rhat < d;
    } else {
      __uint128_t estimate = (__uint128_t)high * inv + low;
      qhat = high + (uint64_t)(estimate >> 64);
      __uint128_t rem =
          (((__uint128_t)high << 64) | low) - (__uint128_t)qhat * d;
      while (rem >= d) {
        ++qhat;
        rem -= d;
      }
      rhat = (uint64_t)rem;
    }
    if (n > 1 && !carried) {
      while ((__uint128_t)qhat * b[n - 2] >
             (((__uint128_t)rhat << 64) | u[j + n - 2])) {
        --qhat;
        uint64_t old = rhat;
        rhat += d;
        if (rhat < old)
          break;
      }
    }
    if (!qhat) {
      q[j] = 0;
      continue;
    }
    uint64_t borrow = 0;
#if defined(__x86_64__) && !defined(_WIN32) &&                                 \
    (defined(__GNUC__) || defined(__clang__))
    if (_bn_has_adx())
      borrow = _bn_submul_adx(u + j, b, n, qhat);
    else
#endif
      for (size_t i = 0; i < n; ++i) {
        __uint128_t p = (__uint128_t)qhat * b[i] + borrow;
        uint64_t lo = (uint64_t)p, old = u[j + i];
        u[j + i] = old - lo;
        borrow = (uint64_t)(p >> 64) + (old < lo);
      }
    bool negative = u[j + n] < borrow;
    u[j + n] -= borrow;
    if (negative) {
      --qhat;
      u[j + n] += _bn_words_add_n(u + j, b, n);
    }
    q[j] = qhat;
  }
  memcpy(r, u, n * sizeof(*r));
}
static void _bn_div_words_2n1n(uint64_t *q, uint64_t *r, const uint64_t *a,
                               const uint64_t *b, size_t n, uint64_t *work);
static void _bn_div_words_3n2n(uint64_t *q, uint64_t *r, const uint64_t *a,
                               const uint64_t *b, size_t h, uint64_t *work) {
  uint64_t *rh = work, *joined = rh + h + 1, *product = joined + 2 * h + 1;
  uint64_t *next = product + 2 * h;
  memset(joined, 0, (2 * h + 1) * sizeof(*joined));
  if (_bn_words_cmp(a + 2 * h, b + h, h) < 0) {
    _bn_div_words_2n1n(q, rh, a + h, b + h, h, next);
    memcpy(joined + h, rh, h * sizeof(*rh));
  } else {
    for (size_t i = 0; i < h; ++i)
      q[i] = UINT64_MAX;
    uint64_t carry = 0;
    for (size_t i = 0; i < h; ++i) {
      __uint128_t sum = (__uint128_t)a[h + i] + b[h + i] + carry;
      joined[h + i] = (uint64_t)sum;
      carry = (uint64_t)(sum >> 64);
    }
    joined[2 * h] = carry;
  }
  memcpy(joined, a, h * sizeof(*a));
  _bn_words_mul(product, q, h, b, h, next);
  uint64_t borrow = _bn_words_sub_n(joined, product, 2 * h);
  bool negative = joined[2 * h] < borrow;
  joined[2 * h] -= borrow;
  while (negative) {
    uint64_t carry = _bn_words_add_n(joined, b, 2 * h);
    uint64_t old = joined[2 * h];
    joined[2 * h] += carry;
    negative = !(carry && joined[2 * h] < old);
    size_t i = 0;
    while (i < h && q[i] == 0)
      q[i++] = UINT64_MAX;
    if (i < h)
      --q[i];
  }
  memcpy(r, joined, 2 * h * sizeof(*r));
}
static void _bn_div_words_2n1n(uint64_t *q, uint64_t *r, const uint64_t *a,
                               const uint64_t *b, size_t n, uint64_t *work) {
  bool upper_zero = true;
  for (size_t i = n; i < 2 * n; ++i)
    if (a[i]) {
      upper_zero = false;
      break;
    }
  if (upper_zero && _bn_words_cmp(a, b, n) < 0) {
    memset(q, 0, n * sizeof(*q));
    memcpy(r, a, n * sizeof(*r));
    return;
  }
  if (n <= BURNIKEL_ZIEGLER_THRESHOLD || (n & 1u)) {
    _bn_div_words_base(q, r, a, b, n, work);
    return;
  }
  size_t h = n / 2;
  uint64_t *rem = work, *partial = rem + n, *next = partial + 3 * h;
  _bn_div_words_3n2n(q + h, rem, a + h, b, h, next);
  memcpy(partial, a, h * sizeof(*a));
  memcpy(partial + h, rem, n * sizeof(*rem));
  _bn_div_words_3n2n(q, r, partial, b, h, next);
}

static void _bn_divmod_burnikel_ziegler(_bn_t *q, _bn_t *r, const _bn_t *a,
                                        const _bn_t *b) {
  size_t unit = 1;
  while (unit < b->count / BURNIKEL_ZIEGLER_THRESHOLD)
    unit *= 2;
  size_t block = (b->count + unit - 1) / unit * unit;
  size_t shift =
      (block - b->count) * 64 + (unsigned)__builtin_clzll(b->d[b->count - 1]);
  _bn_t dividend = {0}, divisor = {0};
  _bn_shl(&dividend, a, shift);
  _bn_shl(&divisor, b, shift);
  size_t blocks = (dividend.count + block - 1) / block;
  if (block > (SIZE_MAX / sizeof(uint64_t) - 64) / 32) {
    _bn_clear(&dividend);
    _bn_clear(&divisor);
    _bn_divmod_knuth(q, r, a, b);
    return;
  }
  uint64_t *arena = calloc(32 * block + 64, sizeof(*arena));
  if (!arena) {
    _bn_clear(&dividend);
    _bn_clear(&divisor);
    _bn_divmod_knuth(q, r, a, b);
    return;
  }
  uint64_t *rem = arena, *partial = rem + block, *digit = partial + 2 * block;
  uint64_t *work = digit + block;
  _bn_t quotient = {0};
  _bn_grow(&quotient, blocks * block);
  for (size_t bi = blocks; bi-- > 0;) {
    memset(partial, 0, block * sizeof(*partial));
    size_t start = bi * block;
    size_t count = dividend.count - start;
    if (count > block)
      count = block;
    memcpy(partial, dividend.d + start, count * sizeof(*partial));
    memcpy(partial + block, rem, block * sizeof(*rem));
    _bn_div_words_2n1n(digit, rem, partial, divisor.d, block, work);
    memcpy(quotient.d + bi * block, digit, block * sizeof(*digit));
  }
  quotient.sign = 1;
  _bn_trim(&quotient);
  _bn_clear(q);
  *q = quotient;
  _bn_t remainder = {.sign = 1, .count = block, .d = rem};
  _bn_trim(&remainder);
  _bn_shr(r, &remainder, shift);
  free(arena);
  _bn_clear(&dividend);
  _bn_clear(&divisor);
}

static void _bn_divmod_unsigned(_bn_t *q, _bn_t *r, const _bn_t *a,
                                const _bn_t *b) {
  if (b->count >= 128 && a->count >= b->count + BURNIKEL_ZIEGLER_THRESHOLD)
    _bn_divmod_burnikel_ziegler(q, r, a, b);
  else
    _bn_divmod_knuth(q, r, a, b);
}

/*
 * Truncated division: q = trunc(a/b), r = a - q*b, |r| < |b|
 */
static void _bn_tdiv_qr(_bn_t *q, _bn_t *r, const _bn_t *a, const _bn_t *b) {
  /*
   * Unsigned division reads its inputs. Borrow their magnitudes until both
   * outputs are complete, then transfer the result storage to the caller.
   */
  _bn_t abs_a = *a;
  _bn_t abs_b = *b;
  _bn_t rq = {0};
  _bn_t rr = {0};
  abs_a.sign = _bn_is_zero(a) ? 0 : 1;
  abs_b.sign = _bn_is_zero(b) ? 0 : 1;
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
  _bn_clear(q);
  *q = rq;
  _bn_clear(r);
  *r = rr;
}

/*
 * Floor division: q = floor(a/b), r = a - q*b; nonzero r has b's sign
 */
static void _bn_fdiv_qr(_bn_t *q, _bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_t abs_a = *a;
  _bn_t abs_b = *b;
  _bn_t rq = {0};
  _bn_t rr = {0};
  abs_a.sign = _bn_is_zero(a) ? 0 : 1;
  abs_b.sign = _bn_is_zero(b) ? 0 : 1;
  _bn_divmod_unsigned(&rq, &rr, &abs_a, &abs_b);

  bool signs_differ = (a->sign < 0) != (b->sign < 0);
  if (signs_differ && !_bn_is_zero(&rr)) {
    /*
     * floor(|a|/-|b|) is one step below truncation; the remainder moves to
     * |b|-r and takes the divisor's sign, matching floor-division semantics.
     */
    _bn_t one = {0};
    _bn_t adjusted_q = {0};
    _bn_t adjusted_r = {0};
    _bn_set_i64(&one, 1);
    _bn_add_mag(&adjusted_q, &rq, &one);
    _bn_sub_mag(&adjusted_r, &abs_b, &rr);
    _bn_clear(&rq);
    _bn_clear(&rr);
    rq = adjusted_q;
    rr = adjusted_r;
    _bn_clear(&one);
  }

  rq.sign = _bn_is_zero(&rq) ? 0 : (signs_differ ? -1 : 1);
  rr.sign = _bn_is_zero(&rr) ? 0 : b->sign;
  _bn_clear(q);
  _bn_clear(r);
  *q = rq;
  *r = rr;
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
    if (n == SIZE_MAX) {
      fprintf(stderr, "nytrix: bigint shift is too large\n");
      abort();
    }
    _bn_grow(a, n + 1);
    a->d[n] = carry;
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
  size_t extra = remain > 0 ? 1u : 0u;
  if (words > SIZE_MAX - ap->count || extra > SIZE_MAX - ap->count - words) {
    _bn_clear(&src);
    fprintf(stderr, "nytrix: bigint shift is too large\n");
    abort();
  }
  size_t n = ap->count + words + extra;
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
 * Arithmetic right shift (floor division by 2^bits)
 */
static void _bn_shr(_bn_t *r, const _bn_t *a, size_t bits) {
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
  bool discarded = false;
  size_t discarded_words = words < ap->count ? words : ap->count;
  for (size_t i = 0; i < discarded_words && !discarded; ++i)
    discarded = ap->d[i] != 0;
  if (remain > 0 && words < ap->count) {
    uint64_t mask = (UINT64_C(1) << remain) - 1u;
    discarded = discarded || (ap->d[words] & mask) != 0;
  }
  if (words >= ap->count) {
    if (ap->sign < 0)
      _bn_set_i64(r, -1);
    else
      _bn_set_zero(r);
    _bn_clear(&src);
    return;
  }
  size_t n = ap->count - words;
  _bn_grow(r, n);
  if (remain > 0) {
    uint64_t carry = 0;
    for (size_t i = n; i > 0; i--) {
      uint64_t high = ap->d[i - 1 + words];
      r->d[i - 1] = (high >> remain) | carry;
      carry = high << (64 - remain);
    }
  } else {
    for (size_t i = 0; i < n; i++)
      r->d[i] = ap->d[i + words];
  }
  r->count = n;
  r->sign = ap->sign;
  _bn_trim(r);

  /*
   * Arithmetic right shifts of negative values round toward
   * negative infinity, so any discarded magnitude bit increments |q|.
   */
  if (ap->sign < 0 && discarded) {
    size_t i = 0;
    uint64_t carry = 1;
    while (i < r->count && carry) {
      uint64_t next = r->d[i] + carry;
      carry = next == 0;
      r->d[i++] = next;
    }
    if (carry) {
      size_t old_count = r->count;
      if (old_count == SIZE_MAX) {
        _bn_clear(&src);
        fprintf(stderr, "nytrix: bigint shift is too large\n");
        abort();
      }
      _bn_grow(r, old_count + 1);
      r->d[old_count] = 1;
    }
    r->sign = -1;
  }
  _bn_trim(r);
  _bn_clear(&src);
}

static size_t _bn_bitlen(const _bn_t *a) {
  if (_bn_is_zero(a))
    return 0;
  return (a->count - 1) * 64 +
         (size_t)(64 - __builtin_clzll(a->d[a->count - 1]));
}
static size_t _bn_popcount(const _bn_t *a) {
  size_t r = 0;
  for (size_t i = 0; i < a->count; i++)
    r += (size_t)__builtin_popcountll(a->d[i]);
  return r;
}

/*
 * Bitwise operations use an explicit finite two's-complement view with one
 * guard limb.  The previous sign-magnitude limb-wise implementation disagreed
 * for negative inputs (for example, -1 & 2 incorrectly produced 0).
 */
typedef enum {
  _BN_BIT_AND,
  _BN_BIT_OR,
  _BN_BIT_XOR,
} _bn_bitop_kind_t;

static inline uint64_t _bn_twos_limb(const _bn_t *a, size_t i,
                                     uint64_t *carry) {
  uint64_t mag = i < a->count ? a->d[i] : 0;
  if (a->sign >= 0)
    return mag;
  uint64_t word = ~mag + *carry;
  *carry = *carry && word == 0;
  return word;
}

static void _bn_bitop(_bn_t *r, const _bn_t *a, const _bn_t *b,
                      _bn_bitop_kind_t op) {
  size_t max_count = a->count > b->count ? a->count : b->count;
  if (max_count == SIZE_MAX) {
    fprintf(stderr, "nytrix: bigint bit operation is too large\n");
    abort();
  }
  size_t n = max_count + 1;
  _bn_t out = {0};
  _bn_grow(&out, n);
  uint64_t acarry = a->sign < 0 ? 1u : 0u;
  uint64_t bcarry = b->sign < 0 ? 1u : 0u;
  for (size_t i = 0; i < n; ++i) {
    uint64_t av = _bn_twos_limb(a, i, &acarry);
    uint64_t bv = _bn_twos_limb(b, i, &bcarry);
    out.d[i] = op == _BN_BIT_AND  ? (av & bv)
               : op == _BN_BIT_OR ? (av | bv)
                                  : (av ^ bv);
  }
  out.count = n;
  if ((out.d[n - 1] >> 63) != 0) {
    uint64_t carry = 1;
    for (size_t i = 0; i < n; ++i) {
      uint64_t word = ~out.d[i] + carry;
      carry = carry && word == 0;
      out.d[i] = word;
    }
    out.sign = -1;
  } else {
    out.sign = 1;
  }
  _bn_trim(&out);
  _bn_clear(r);
  *r = out;
}

static void _bn_and(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_bitop(r, a, b, _BN_BIT_AND);
}

static void _bn_or(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_bitop(r, a, b, _BN_BIT_OR);
}

static void _bn_xor(_bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_bitop(r, a, b, _BN_BIT_XOR);
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
    _bn_clear(&u);
    _bn_clear(&v);
    return;
  }
  if (_bn_is_zero(&v)) {
    _bn_copy(r, &u);
    _bn_clear(&u);
    _bn_clear(&v);
    return;
  }
  size_t uz = _bn_ctz_mag(&u), vz = _bn_ctz_mag(&v);
  size_t common = uz < vz ? uz : vz;
  _bn_shr(&u, &u, uz);
  _bn_shr(&v, &v, vz);
  while (!_bn_is_zero(&v)) {
    if (_bn_cmp_mag(&u, &v) > 0) {
      _bn_t swap = u;
      u = v;
      v = swap;
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
  _bn_clear(&u);
  _bn_clear(&v);
}

/*
 * Small-word helpers used by decimal conversion.  Base 10^9 keeps every
 * quotient step allocation-free and replaces nine full bigint divisions or
 * multiplications from the old digit-at-a-time conversion path.
 */
static uint32_t _bn_divmod_u32_inplace(_bn_t *a, uint32_t divisor) {
  if (_bn_is_zero(a) || divisor == 0)
    return 0;
  uint64_t rem = 0;
  for (size_t i = a->count; i-- > 0;) {
    __uint128_t cur = ((__uint128_t)rem << 64) | a->d[i];
    a->d[i] = (uint64_t)(cur / divisor);
    rem = (uint64_t)(cur % divisor);
  }
  _bn_trim(a);
  if (!_bn_is_zero(a))
    a->sign = 1;
  return (uint32_t)rem;
}

static void _bn_mul_u32_inplace(_bn_t *a, uint32_t multiplier) {
  if (_bn_is_zero(a) || multiplier == 1)
    return;
  if (multiplier == 0) {
    _bn_set_zero(a);
    return;
  }
  size_t old_count = a->count;
  if (old_count == SIZE_MAX) {
    fprintf(stderr, "nytrix: bigint decimal conversion is too large\n");
    abort();
  }
  _bn_grow(a, old_count + 1u);
  __uint128_t carry = 0;
  for (size_t i = 0; i < old_count; ++i) {
    __uint128_t product = (__uint128_t)a->d[i] * multiplier + carry;
    a->d[i] = (uint64_t)product;
    carry = product >> 64;
  }
  a->d[old_count] = (uint64_t)carry;
  a->count = old_count + (carry != 0);
  a->sign = a->count ? 1 : 0;
}

static void _bn_add_u32_inplace(_bn_t *a, uint32_t addend) {
  if (addend == 0)
    return;
  if (_bn_is_zero(a)) {
    _bn_set_i64(a, (int64_t)addend);
    return;
  }
  size_t i = 0;
  uint64_t carry = addend;
  while (carry && i < a->count) {
    uint64_t old = a->d[i];
    a->d[i] = old + carry;
    carry = a->d[i] < old;
    ++i;
  }
  if (carry) {
    if (a->count == SIZE_MAX) {
      fprintf(stderr, "nytrix: bigint decimal conversion is too large\n");
      abort();
    }
    size_t old_count = a->count;
    _bn_grow(a, old_count + 1u);
    a->d[old_count] = carry;
    a->count = old_count + 1u;
  }
  a->sign = 1;
}

/*
 * String conversion: base 10
 */
static char *_bn_to_str(const _bn_t *a) {
  if (_bn_is_zero(a)) {
    char *s = (char *)malloc(2);
    if (!s) {
      fprintf(stderr, "nytrix: out of memory converting bigint to string\n");
      abort();
    }
    s[0] = '0';
    s[1] = '\0';
    return s;
  }

  if (a->count > (SIZE_MAX - 3u) / 20u) {
    fprintf(stderr, "nytrix: bigint string conversion is too large\n");
    abort();
  }
  size_t cap = a->count * 20u + 3u;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    fprintf(stderr, "nytrix: out of memory converting bigint to string\n");
    abort();
  }

  size_t bitlen = _bn_bitlen(a);
  if (bitlen > SIZE_MAX - 28u) {
    free(buf);
    fprintf(stderr, "nytrix: bigint string conversion is too large\n");
    abort();
  }
  size_t chunk_cap = bitlen / 29u + 2u;
  if (chunk_cap > SIZE_MAX / sizeof(uint32_t)) {
    free(buf);
    fprintf(stderr, "nytrix: bigint string conversion is too large\n");
    abort();
  }
  uint32_t *chunks = (uint32_t *)malloc(chunk_cap * sizeof(*chunks));
  if (!chunks) {
    free(buf);
    fprintf(stderr, "nytrix: out of memory converting bigint to string\n");
    abort();
  }

  _bn_t tmp = {0};
  _bn_abs(&tmp, a);
  size_t chunk_count = 0;
  while (!_bn_is_zero(&tmp)) {
    if (chunk_count == chunk_cap) {
      if (chunk_cap > SIZE_MAX / 2u ||
          chunk_cap * 2u > SIZE_MAX / sizeof(*chunks)) {
        free(chunks);
        free(buf);
        _bn_clear(&tmp);
        fprintf(stderr, "nytrix: bigint string conversion is too large\n");
        abort();
      }
      size_t next_cap = chunk_cap * 2u;
      uint32_t *grown = (uint32_t *)realloc(chunks, next_cap * sizeof(*chunks));
      if (!grown) {
        free(chunks);
        free(buf);
        _bn_clear(&tmp);
        fprintf(stderr, "nytrix: out of memory converting bigint to string\n");
        abort();
      }
      chunks = grown;
      chunk_cap = next_cap;
    }
    chunks[chunk_count++] = _bn_divmod_u32_inplace(&tmp, 1000000000u);
  }
  _bn_clear(&tmp);

  size_t pos = 0;
  if (a->sign < 0)
    buf[pos++] = '-';
  int written = snprintf(buf + pos, cap - pos, "%u", chunks[chunk_count - 1]);
  if (written < 0 || (size_t)written >= cap - pos) {
    free(chunks);
    free(buf);
    fprintf(stderr, "nytrix: bigint string conversion failed\n");
    abort();
  }
  pos += (size_t)written;
  for (size_t i = chunk_count - 1; i-- > 0;) {
    written = snprintf(buf + pos, cap - pos, "%09u", chunks[i]);
    if (written != 9 || (size_t)written >= cap - pos) {
      free(chunks);
      free(buf);
      fprintf(stderr, "nytrix: bigint string conversion failed\n");
      abort();
    }
    pos += 9u;
  }
  free(chunks);
  buf[pos] = '\0';
  return buf;
}

/*
 * Parse decimal string
 */
static bool _bn_from_str(_bn_t *r, const char *s) {
  _bn_set_zero(r);
  if (!s || !*s)
    return false;
  bool neg = false;
  if (*s == '-' || *s == '+') {
    neg = *s == '-';
    ++s;
  }
  if (!*s)
    return false;

  const char *digits = s;
  size_t digit_count = 0;
  for (const char *p = digits; *p; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    if (digit_count == SIZE_MAX)
      return false;
    ++digit_count;
  }
  while (*digits == '0') {
    ++digits;
    --digit_count;
  }
  if (digit_count == 0)
    return true;

  size_t first_group = digit_count % 9u;
  if (first_group == 0)
    first_group = 9u;
  const char *p = digits;
  size_t group = first_group;
  while (digit_count != 0) {
    uint32_t chunk = 0;
    for (size_t i = 0; i < group; ++i)
      chunk = chunk * 10u + (uint32_t)(*p++ - '0');
    uint32_t scale = group == 9u ? 1000000000u : 1u;
    if (group != 9u) {
      for (size_t i = 0; i < group; ++i)
        scale *= 10u;
    }
    _bn_mul_u32_inplace(r, scale);
    _bn_add_u32_inplace(r, chunk);
    digit_count -= group;
    group = 9u;
  }
  if (neg && !_bn_is_zero(r))
    r->sign = -1;
  return true;
}

/*
 * Convert native to tagged int64 (small int or heap BigInt)
 */
static int64_t _bi_from_native(_bn_t *a) {
  _bn_trim(a);
  /*
   * Every public BigInt operation returns a heap-backed TAG_BIGINT object,
   * even when the
   * numerical result would fit in a tagged small integer.  Returning a tagged
   * int here violates the static `bigint` representation expected by compiled
   * callers (a later bigint_cmp can then see an untagged scalar as neither an
   * int nor a pointer and silently compare it as zero).
   */
  size_t words = _bn_is_zero(a) ? 0 : a->count;
  int sign = words == 0 ? 0 : a->sign;
  int64_t p = _bi_alloc(words);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p + 0) = rt_tag_v((int64_t)sign);
  if (words > 0)
    memcpy((void *)((char *)p + 16), a->d, words * sizeof(uint64_t));
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
  if (words <= 0 || (uint64_t)words > SIZE_MAX / sizeof(uint64_t)) {
    _bn_set_zero(out);
    return;
  }
  uint64_t *nd = (uint64_t *)malloc((size_t)words * sizeof(uint64_t));
  if (!nd) {
    _bn_set_zero(out);
    return;
  }
  memcpy(nd, (const void *)((char *)v + 16), (size_t)words * sizeof(uint64_t));
  free(out->d);
  out->d = nd;
  out->sign = (int)sign;
  out->count = (size_t)words;
}

/*
 * Exponentiation helpers keep the result independent from the input storage,
 * so they are safe when r aliases base.  Squaring stops once no exponent bits
 * remain; the previous public pow path always built one unused final square.
 */
static void _bn_pow_ui(_bn_t *r, const _bn_t *base, unsigned long exp) {
  _bn_t result = {0}, factor = {0};
  _bn_set_i64(&result, 1);
  _bn_copy(&factor, base);
  while (exp != 0) {
    if (exp & 1u) {
      _bn_t product = {0};
      _bn_mul(&product, &result, &factor);
      _bn_clear(&result);
      result = product;
    }
    exp >>= 1;
    if (exp != 0) {
      _bn_t square = {0};
      _bn_mul(&square, &factor, &factor);
      _bn_clear(&factor);
      factor = square;
    }
  }
  _bn_clear(r);
  *r = result;
  _bn_clear(&factor);
}

static void _bn_pow_bigexp(_bn_t *r, const _bn_t *base, const _bn_t *exp) {
  _bn_t result = {0}, factor = {0};
  _bn_set_i64(&result, 1);
  _bn_copy(&factor, base);
  size_t bits = _bn_bitlen(exp);
  for (size_t i = 0; i < bits; ++i) {
    size_t word = i / 64, bit = i % 64;
    if (word < exp->count && ((exp->d[word] >> bit) & 1u)) {
      _bn_t product = {0};
      _bn_mul(&product, &result, &factor);
      _bn_clear(&result);
      result = product;
    }
    if (i + 1u < bits) {
      _bn_t square = {0};
      _bn_mul(&square, &factor, &factor);
      _bn_clear(&factor);
      factor = square;
    }
  }
  _bn_clear(r);
  *r = result;
  _bn_clear(&factor);
}

/*
 * Integer square root (floor) via Newton's method
 */
static void _bn_isqrt_newton(_bn_t *r, const _bn_t *a) {
  if (_bn_is_zero(a) || a->sign < 0) {
    _bn_set_zero(r);
    return;
  }
  /*
   * Start above sqrt(a), then Newton's update decreases monotonically.
   */
  size_t bits = _bn_bitlen(a);
  _bn_set_i64(r, 1);
  if (bits > 1)
    _bn_shl(r, r, bits / 2 + bits % 2);
  for (;;) {
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
    _bn_t next = {0};
    _bn_shr(&next, &sum, 1);
    _bn_clear(&sum);
    _bn_trim(&next);
    if (_bn_cmp(&next, r) >= 0) {
      _bn_clear(&next);
      break;
    }
    _bn_clear(r);
    *r = next;
  }
  /*
   * Defensive correction in case integer Newton stops one step high.
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
 * Keep the low n bits of a positive operand.
 */
static void _bn_low_bits(_bn_t *r, const _bn_t *a, size_t n) {
  size_t words = n / 64 + (n % 64 != 0);
  if (!_bn_limb_slice(r, a, 0, words))
    return;
  if (words && n % 64)
    r->d[words - 1] &= (UINT64_C(1) << (n % 64)) - 1;
  _bn_trim(r);
}

/*
 * Recursive square root with remainder. Splitting at a quarter of the
 * input bits reduces each division and multiplication to roughly half size.
 */
static void _bn_sqrtrem(_bn_t *root, _bn_t *rem, const _bn_t *a) {
  size_t bits = _bn_bitlen(a);
  if (bits <= 256) {
    _bn_isqrt_newton(root, a);
    _bn_t square = {0};
    _bn_mul(&square, root, root);
    _bn_sub(rem, a, &square);
    _bn_clear(&square);
    return;
  }
  size_t split = (bits - 1) / 4;
  _bn_t high = {0}, middle_shift = {0}, middle = {0}, low = {0};
  _bn_shr(&high, a, 2 * split);
  _bn_shr(&middle_shift, a, split);
  _bn_low_bits(&middle, &middle_shift, split);
  _bn_low_bits(&low, a, split);
  _bn_t high_root = {0}, high_rem = {0};
  _bn_sqrtrem(&high_root, &high_rem, &high);
  _bn_t partial = {0}, denom = {0};
  _bn_shl(&partial, &high_rem, split);
  _bn_add(&partial, &partial, &middle);
  _bn_add(&denom, &high_root, &high_root);
  _bn_t q = {0}, qrem = {0};
  _bn_divmod_unsigned(&q, &qrem, &partial, &denom);
  _bn_t shifted_root = {0};
  _bn_shl(&shifted_root, &high_root, split);
  _bn_add(root, &shifted_root, &q);
  _bn_t qdenom = {0}, lower_rem = {0};
  _bn_mul(&qdenom, &q, &denom);
  _bn_sub(&lower_rem, &partial, &qdenom);
  _bn_shl(rem, &lower_rem, split);
  _bn_add(rem, rem, &low);
  _bn_t qsquare = {0};
  _bn_mul(&qsquare, &q, &q);
  _bn_sub(rem, rem, &qsquare);
  _bn_t one = {0}, twice = {0}, adjust = {0}, next_root = {0}, next_rem = {0};
  _bn_set_i64(&one, 1);
  for (;;) {
    _bn_add(&twice, root, root);
    _bn_sub(&adjust, &twice, &one);
    if (rem->sign >= 0)
      break;
    _bn_add(&next_rem, rem, &adjust);
    _bn_sub(&next_root, root, &one);
    _bn_clear(rem);
    *rem = next_rem;
    memset(&next_rem, 0, sizeof(next_rem));
    _bn_clear(root);
    *root = next_root;
    memset(&next_root, 0, sizeof(next_root));
  }
  for (;;) {
    _bn_add(&twice, root, root);
    _bn_add(&adjust, &twice, &one);
    if (_bn_cmp(rem, &adjust) < 0)
      break;
    _bn_sub(&next_rem, rem, &adjust);
    _bn_add(&next_root, root, &one);
    _bn_clear(rem);
    *rem = next_rem;
    memset(&next_rem, 0, sizeof(next_rem));
    _bn_clear(root);
    *root = next_root;
    memset(&next_root, 0, sizeof(next_root));
  }
  _bn_clear(&high);
  _bn_clear(&middle_shift);
  _bn_clear(&middle);
  _bn_clear(&low);
  _bn_clear(&high_root);
  _bn_clear(&high_rem);
  _bn_clear(&partial);
  _bn_clear(&denom);
  _bn_clear(&q);
  _bn_clear(&qrem);
  _bn_clear(&shifted_root);
  _bn_clear(&qdenom);
  _bn_clear(&lower_rem);
  _bn_clear(&qsquare);
  _bn_clear(&one);
  _bn_clear(&twice);
  _bn_clear(&adjust);
  _bn_clear(&next_root);
  _bn_clear(&next_rem);
}

static void _bn_isqrt(_bn_t *r, const _bn_t *a) {
  if (_bn_is_zero(a) || a->sign < 0) {
    _bn_set_zero(r);
    return;
  }
  _bn_t root = {0}, rem = {0};
  _bn_sqrtrem(&root, &rem, a);
  _bn_clear(r);
  *r = root;
  _bn_clear(&rem);
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
  if (a->sign < 0) {
    if ((k & 1u) == 0) {
      _bn_set_zero(r);
      return;
    }
    _bn_t magnitude = {0};
    _bn_abs(&magnitude, a);
    _bn_iroot(r, &magnitude, k);
    if (!_bn_is_zero(r))
      r->sign = -1;
    _bn_clear(&magnitude);
    return;
  }
  if (k == 2) {
    _bn_isqrt(r, a);
    return;
  }
  size_t bits = _bn_bitlen(a);
  if (k >= bits) {
    _bn_set_i64(r, 1);
    return;
  }
  _bn_set_i64(r, 1);
  _bn_shl(r, r, bits / k + (bits % k != 0));
  /*
   * Integer Newton decreases strictly until it reaches the floor root.
   * A bit-length-derived iteration cap can stop early for large k.
   */
  for (;;) {
    /*
     * Compute only r^(k-1); exponentiation by squaring avoids O(k)
     * multiplications for large root indices.
     */
    _bn_t rk1 = {0};
    _bn_pow_ui(&rk1, r, k - 1u);
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
 * Modular inverse via extended Euclidean arithmetic
 */
static bool _bn_modinv(_bn_t *r, const _bn_t *a, const _bn_t *m) {
  if (_bn_is_zero(m)) {
    _bn_set_zero(r);
    return false;
  }

  _bn_t modulus = {0};
  _bn_t a_mod = {0};
  _bn_t discard_q = {0};
  _bn_abs(&modulus, m);
  _bn_fdiv_qr(&discard_q, &a_mod, a, &modulus);
  _bn_clear(&discard_q);

  /*
   * Extended Euclid on positive residues: r0 = modulus, r1 = a mod modulus.
   * t0/t1 track the coefficient of a, which is the inverse when gcd == 1.
   */
  _bn_t r0 = {0}, r1 = {0}, t0 = {0}, t1 = {0};
  _bn_copy(&r0, &modulus);
  _bn_copy(&r1, &a_mod);
  _bn_set_i64(&t0, 0);
  _bn_set_i64(&t1, 1);
  while (!_bn_is_zero(&r1)) {
    _bn_t q = {0}, rem = {0};
    _bn_divmod_unsigned(&q, &rem, &r0, &r1);
    _bn_clear(&r0);
    r0 = r1;
    r1 = rem;

    _bn_t prod = {0}, next_t = {0};
    _bn_mul(&prod, &q, &t1);
    _bn_sub(&next_t, &t0, &prod);
    _bn_clear(&q);
    _bn_clear(&prod);
    _bn_clear(&t0);
    t0 = t1;
    t1 = next_t;
  }

  _bn_t one = {0};
  _bn_set_i64(&one, 1);
  bool ok = _bn_cmp(&r0, &one) == 0;
  _bn_clear(&one);
  if (ok) {
    _bn_t normalized_q = {0}, normalized = {0};
    _bn_fdiv_qr(&normalized_q, &normalized, &t0, &modulus);
    _bn_copy(r, &normalized);
    _bn_clear(&normalized_q);
    _bn_clear(&normalized);
  } else {
    _bn_set_zero(r);
  }

  _bn_clear(&modulus);
  _bn_clear(&a_mod);
  _bn_clear(&r0);
  _bn_clear(&r1);
  _bn_clear(&t0);
  _bn_clear(&t1);
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
  if (!_bn_limb_slice(&q1, x, k - 1, x->count) || !_bn_low_limbs(&r1, x, k + 1))
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
  _bn_clear(&q1);
  _bn_clear(&q2);
  _bn_clear(&q3);
  _bn_clear(&r1);
  _bn_clear(&product);
  _bn_clear(&r2);
  _bn_clear(&reduced);
  return true;
fail:
  _bn_clear(&q1);
  _bn_clear(&q2);
  _bn_clear(&q3);
  _bn_clear(&r1);
  _bn_clear(&product);
  _bn_clear(&r2);
  _bn_clear(&reduced);
  return false;
}

static void _bn_reduce_product(_bn_t *out, const _bn_t *product,
                               const _bn_t *mod, const _bn_barrett_t *barrett) {
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
  if (_bn_is_zero(mod) || (mod->count == 1 && mod->d[0] == 1)) {
    _bn_set_zero(r);
    return;
  }
  if (exp->sign < 0) {
    _bn_t inverse = {0}, positive_exp = {0};
    if (!_bn_modinv(&inverse, base, mod)) {
      _bn_set_zero(r);
      return;
    }
    _bn_abs(&positive_exp, exp);
    _bn_powmod(r, &inverse, &positive_exp, mod);
    _bn_clear(&inverse);
    _bn_clear(&positive_exp);
    return;
  }
  _bn_t modulus = {0};
  _bn_abs(&modulus, mod);
  _bn_t result = {0};
  _bn_set_i64(&result, 1);
  _bn_barrett_t barrett = {0};
  bool have_barrett = _bn_barrett_init(&barrett, &modulus);
  /*
   * Reduce base mod mod
   */
  _bn_t b = {0};
  {
    _bn_t rq = {0};
    _bn_t rr = {0};
    _bn_init(&rq);
    _bn_init(&rr);
    _bn_fdiv_qr(&rq, &rr, base, &modulus);
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
      _bn_reduce_product(&reduced, &prod, &modulus,
                         have_barrett ? &barrett : NULL);
      _bn_clear(&result);
      _bn_copy(&result, &reduced);
      result.sign = _bn_is_zero(&result) ? 0 : 1;
      _bn_clear(&reduced);
      _bn_clear(&prod);
    }
    if (i + 1u < expbits) {
      _bn_t sq = {0}, reduced = {0};
      _bn_mul(&sq, &b, &b);
      _bn_reduce_product(&reduced, &sq, &modulus,
                         have_barrett ? &barrett : NULL);
      _bn_clear(&b);
      b = reduced;
      b.sign = _bn_is_zero(&b) ? 0 : 1;
      _bn_clear(&sq);
    }
  }
  _bn_copy(r, &result);
  _bn_clear(&result);
  _bn_clear(&b);
  _bn_clear(&modulus);
  _bn_barrett_clear(&barrett);
}

/*
 * xgcd: g = gcd(a,b), g = ax + by
 */
static void _bn_xgcd(_bn_t *g, _bn_t *x, _bn_t *y, const _bn_t *a,
                     const _bn_t *b) {
  if (_bn_is_zero(a) && _bn_is_zero(b)) {
    _bn_set_zero(g);
    _bn_set_zero(x);
    _bn_set_zero(y);
    return;
  }
  _bn_t old_r = {0};
  _bn_t new_r = {0};
  _bn_t old_s = {0};
  _bn_t new_s = {0};
  _bn_t old_t = {0};
  _bn_t new_t = {0};
  _bn_abs(&old_r, a);
  _bn_abs(&new_r, b);
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
  if (a->sign < 0)
    _bn_neg(&old_s, &old_s);
  if (b->sign < 0)
    _bn_neg(&old_t, &old_t);
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
  if (_bn_is_zero(n) || n->sign < 0 || !(n->d[0] & 1u))
    return 0;

  _bn_t v = {0}, u = {0}, q = {0};
  _bn_abs(&v, n);
  _bn_t abs_a = {0};
  _bn_abs(&abs_a, a);
  _bn_divmod_unsigned(&q, &u, &abs_a, &v);
  _bn_clear(&q);
  _bn_clear(&abs_a);

  int result = 1;
  if (a->sign < 0 && (v.d[0] & 3u) == 3u)
    result = -result;

  while (!_bn_is_zero(&u)) {
    size_t twos = _bn_ctz_mag(&u);
    if (twos) {
      if (twos & 1u) {
        uint64_t vmod8 = v.d[0] & 7u;
        if (vmod8 == 3u || vmod8 == 5u)
          result = -result;
      }
      _bn_shr(&u, &u, twos);
    }

    if (u.count == 1 && u.d[0] == 1u) {
      _bn_clear(&u);
      _bn_clear(&v);
      return result;
    }

    if ((u.d[0] & 3u) == 3u && (v.d[0] & 3u) == 3u)
      result = -result;

    _bn_t rem = {0}, quotient = {0};
    _bn_divmod_unsigned(&quotient, &rem, &v, &u);
    _bn_clear(&quotient);
    _bn_clear(&v);
    v = u;
    u = rem;
  }

  bool coprime = v.count == 1 && v.d[0] == 1u;
  _bn_clear(&u);
  _bn_clear(&v);
  return coprime ? result : 0;
}

/*
 * Kronecker extends Jacobi to even, zero, and negative denominators.
 */
static int _bn_kronecker_val(const _bn_t *a, const _bn_t *n) {
  if (_bn_is_zero(n))
    return a->count == 1 && a->d[0] == 1u ? 1 : 0;

  int result = 1;
  if (n->sign < 0 && a->sign < 0)
    result = -result;

  _bn_t odd = {0};
  _bn_abs(&odd, n);
  size_t twos = _bn_ctz_mag(&odd);
  if (twos) {
    if (_bn_is_zero(a) || (a->d[0] & 1u) == 0u) {
      _bn_clear(&odd);
      return 0;
    }
    if (twos & 1u) {
      uint64_t amod8 = a->d[0] & 7u;
      if (a->sign < 0)
        amod8 = (8u - amod8) & 7u;
      if (amod8 == 3u || amod8 == 5u)
        result = -result;
    }
    _bn_shr(&odd, &odd, twos);
  }

  if (odd.count == 1 && odd.d[0] == 1u) {
    _bn_clear(&odd);
    return result;
  }
  int jacobi = _bn_jacobi_val(a, &odd);
  _bn_clear(&odd);
  return result * jacobi;
}

/*
 * GF(2) values are non-negative coefficient bitsets.  Polynomial long
 * division repeatedly XORs shifted copies of the divisor; doing that directly
 * in limb storage avoids allocating a temporary bigint for every reduction
 * step.  `source` must not alias `target` (all callers keep a stable modulus
 * or multiplicand copy).
 */
static void _bn_gf2_xor_shifted_inplace(_bn_t *target, const _bn_t *source,
                                        size_t shift) {
  if (_bn_is_zero(source))
    return;
  size_t word_shift = shift / 64u;
  unsigned bit_shift = (unsigned)(shift % 64u);
  size_t extra = bit_shift != 0 ? 1u : 0u;
  if (word_shift > SIZE_MAX - source->count ||
      extra > SIZE_MAX - word_shift - source->count) {
    fprintf(stderr, "nytrix: GF(2) shift is too large\n");
    abort();
  }
  size_t needed = word_shift + source->count + extra;
  size_t old_count = target->count;
  if (needed > target->count)
    _bn_grow(target, needed);
  if (bit_shift == 0) {
    for (size_t i = 0; i < source->count; ++i)
      target->d[word_shift + i] ^= source->d[i];
  } else {
    for (size_t i = 0; i < source->count; ++i) {
      uint64_t limb = source->d[i];
      target->d[word_shift + i] ^= limb << bit_shift;
      target->d[word_shift + i + 1u] ^= limb >> (64u - bit_shift);
    }
  }
  if (target->count < old_count)
    target->count = old_count;
  target->sign = 1;
  _bn_trim(target);
}

static void _bn_gf2_toggle_bit(_bn_t *value, size_t bit_index) {
  size_t word = bit_index / 64u;
  unsigned bit = (unsigned)(bit_index % 64u);
  if (word == SIZE_MAX) {
    fprintf(stderr, "nytrix: GF(2) quotient is too large\n");
    abort();
  }
  if (word >= value->count)
    _bn_grow(value, word + 1u);
  value->d[word] ^= UINT64_C(1) << bit;
  value->sign = 1;
  _bn_trim(value);
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
  _bn_t mod = {0};
  _bn_abs(&mod, m);
  _bn_abs(out, a);
  long m_deg = _bn_gf2_deg(&mod);
  if (m_deg < 0) {
    _bn_set_zero(out);
    _bn_clear(&mod);
    return;
  }
  for (;;) {
    long a_deg = _bn_gf2_deg(out);
    if (a_deg < m_deg)
      break;
    _bn_gf2_xor_shifted_inplace(out, &mod, (size_t)(a_deg - m_deg));
  }
  out->sign = _bn_is_zero(out) ? 0 : 1;
  _bn_clear(&mod);
}
/*
 * GF(2) carryless multiply mod m
 */
static void _bn_gf2_mulmod(_bn_t *out, const _bn_t *a, const _bn_t *b,
                           const _bn_t *m) {
  _bn_t mod = {0};
  _bn_abs(&mod, m);
  long m_deg = _bn_gf2_deg(&mod);
  if (m_deg < 0) {
    _bn_set_zero(out);
    _bn_clear(&mod);
    return;
  }
  _bn_t va = {0};
  _bn_t vb = {0};
  _bn_gf2_mod(&va, a, &mod);
  _bn_abs(&vb, b);
  _bn_set_zero(out);
  size_t b_bits = _bn_bitlen(&vb);
  for (size_t bit_index = 0; bit_index < b_bits; ++bit_index) {
    size_t word = bit_index / 64u;
    unsigned bit = (unsigned)(bit_index % 64u);
    if (((vb.d[word] >> bit) & 1u) != 0)
      _bn_gf2_xor_shifted_inplace(out, &va, 0);
    if (bit_index + 1u < b_bits) {
      /*
       * va is reduced, so x*va can exceed the modulus degree by at most one;
       * one conditional XOR is therefore a complete reduction.
       */
      _bn_shl1(&va);
      if (_bn_gf2_deg(&va) >= m_deg)
        _bn_gf2_xor_shifted_inplace(&va, &mod, 0);
    }
  }
  out->sign = _bn_is_zero(out) ? 0 : 1;
  _bn_clear(&va);
  _bn_clear(&vb);
  _bn_clear(&mod);
}

/*
 * GF(2) polynomial division: a = q*b + r in GF(2)
 */
static void _bn_gf2_div_qr(_bn_t *q, _bn_t *r, const _bn_t *a, const _bn_t *b) {
  _bn_t divisor = {0};
  _bn_set_zero(q);
  _bn_abs(r, a);
  _bn_abs(&divisor, b);
  long b_deg = _bn_gf2_deg(&divisor);
  if (b_deg < 0) {
    _bn_clear(&divisor);
    return;
  }
  for (;;) {
    long r_deg = _bn_gf2_deg(r);
    if (r_deg < b_deg)
      break;
    size_t shift = (size_t)(r_deg - b_deg);
    _bn_gf2_xor_shifted_inplace(r, &divisor, shift);
    _bn_gf2_toggle_bit(q, shift);
  }
  q->sign = _bn_is_zero(q) ? 0 : 1;
  r->sign = _bn_is_zero(r) ? 0 : 1;
  _bn_clear(&divisor);
}

/*
 * Convert big-endian bytes to native
 */
static void _bn_from_bytes_be(_bn_t *r, const uint8_t *bytes, size_t len) {
  _bn_set_zero(r);
  if (!bytes || len == 0)
    return;
  size_t words =
      len / sizeof(uint64_t) + (len % sizeof(uint64_t) != 0 ? 1u : 0u);
  _bn_grow(r, words);
  memset(r->d, 0, words * sizeof(*r->d));
  for (size_t i = 0; i < len; ++i) {
    size_t src = len - 1 - i;
    r->d[i / 8] |= (uint64_t)bytes[src] << ((i % 8) * 8);
  }
  r->count = words;
  r->sign = 1;
  _bn_trim(r);
}

/*
 * Convert to big-endian bytes
 */
static uint8_t *_bn_to_bytes_be(const _bn_t *a, size_t *count) {
  if (!count)
    return NULL;
  *count = 0;
  if (_bn_is_zero(a)) {
    uint8_t *buf = (uint8_t *)malloc(1);
    if (!buf) {
      fprintf(stderr, "nytrix: out of memory converting bigint to bytes\n");
      abort();
    }
    buf[0] = 0;
    *count = 1;
    return buf;
  }

  uint64_t top = a->d[a->count - 1];
  size_t top_bytes = (size_t)(64 - __builtin_clzll(top) + 7) / 8;
  size_t byte_count = (a->count - 1) * sizeof(uint64_t) + top_bytes;
  uint8_t *buf = (uint8_t *)malloc(byte_count);
  if (!buf) {
    fprintf(stderr, "nytrix: out of memory converting bigint to bytes\n");
    abort();
  }
  for (size_t i = 0; i < byte_count; ++i) {
    size_t mag_index = byte_count - 1 - i;
    buf[i] = (uint8_t)(a->d[mag_index / 8] >> ((mag_index % 8) * 8));
  }
  *count = byte_count;
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

/*
 * Fast-path helpers
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

static inline int _bi_cmp_mag(const uint64_t *a, size_t aw, const uint64_t *b,
                              size_t bw) {
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
  *(int64_t *)(uintptr_t)((char *)p + 0) =
      rt_tag_v((int64_t)(sign < 0 ? -1 : (sign > 0 ? 1 : 0)));
  if (words > 0)
    memcpy((void *)((char *)p + 16), limbs, words * sizeof(uint64_t));
  return p;
}

static inline int64_t
_bi_from_limbs_or_small_int(int sign, const uint64_t *limbs, size_t words);

static inline int64_t _bi_from_i128_value(__int128 v, int compact) {
  if (compact && v >= (__int128)NY_SMALL_INT_MIN &&
      v <= (__int128)NY_SMALL_INT_MAX)
    return rt_tag_v((int64_t)v);
  int sign = v < 0 ? -1 : (v > 0 ? 1 : 0);
  unsigned __int128 mag =
      v < 0 ? (unsigned __int128)(-(v + 1)) + 1u : (unsigned __int128)v;
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

static inline bool _bi_try_submul_i64_int_pair_small(int64_t a, int64_t q_raw,
                                                     int64_t b, int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  return _bi_try_i128_small_tag(
      (__int128)rt_untag_v(a) - (__int128)q_raw * (__int128)rt_untag_v(b), out);
}

static inline bool _bi_try_add_int_pair_small(int64_t a, int64_t b, int sub_b,
                                              int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  __int128 av = (__int128)rt_untag_v(a);
  __int128 bv = (__int128)rt_untag_v(b);
  return _bi_try_i128_small_tag(sub_b ? av - bv : av + bv, out);
}

static inline int64_t
_bi_from_limbs_or_small_int(int sign, const uint64_t *limbs, size_t words) {
  words = _bi_norm_words_const(limbs, words);
  if (words == 0)
    return rt_tag_v(0);
  if (words == 1) {
    uint64_t mag = limbs[0];
    if (sign >= 0 && mag <= (uint64_t)NY_SMALL_INT_MAX)
      return rt_tag_v((int64_t)mag);
    if (sign < 0 && mag <= (uint64_t)NY_SMALL_INT_MAX + 1u)
      return rt_tag_v(-(int64_t)(mag - 1u) - 1);
  }
  return _bi_from_limbs(sign, limbs, words);
}

static inline size_t _bi_mag_add4(uint64_t out[4], const uint64_t *a, size_t aw,
                                  const uint64_t *b, size_t bw) {
  uint64_t carry = 0;
  size_t n = aw > bw ? aw : bw;
  for (size_t i = 0; i < n; i++) {
    __uint128_t s =
        (__uint128_t)(i < aw ? a[i] : 0) + (i < bw ? b[i] : 0) + carry;
    out[i] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
  }
  if (carry && n < 4)
    out[n++] = carry;
  return _bi_norm_words(out, n);
}

static inline size_t _bi_mag_sub4(uint64_t out[4], const uint64_t *a, size_t aw,
                                  const uint64_t *b, size_t bw) {
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
  *out = compact ? _bi_from_limbs_or_small_int(rs, rl, rw)
                 : _bi_from_limbs(rs, rl, rw);
  return true;
}

static inline bool _bi_try_add_limb2(int64_t a, int64_t b, int sub_b,
                                     int64_t *out) {
  return _bi_try_add_limb2_kind(a, b, sub_b, 0, out);
}

static inline bool _bi_read_heap_limbs(int64_t v, int *sign,
                                       const uint64_t **limbs, size_t *words,
                                       uint64_t small[1]) {
  if (!sign || !limbs || !words || !small)
    return false;
  *sign = 0;
  *limbs = small;
  *words = 0;
  small[0] = 0;
  if (is_int(v)) {
    int64_t raw = rt_untag_v(v);
    if (raw == 0)
      return true;
    *sign = raw < 0 ? -1 : 1;
    small[0] = raw < 0 ? (uint64_t)(-(raw + 1)) + 1u : (uint64_t)raw;
    *words = 1;
    return true;
  }
  if (!is_ptr(v))
    return false;
  uintptr_t base = (uintptr_t)v;
  size_t heap_size = 0;
  bool managed = rt_heap_size_fast(v, &heap_size);
  if (base < 8u ||
      (managed ? heap_size < 16u : !_bi_readable_range(base - 8u, 24u)))
    return false;
  int64_t tag = 0, sign_v = 0, words_v = 0;
  memcpy(&tag, (const void *)(base - 8u), sizeof(tag));
  memcpy(&sign_v, (const void *)base, sizeof(sign_v));
  memcpy(&words_v, (const void *)(base + 8u), sizeof(words_v));
  if (tag != TAG_BIGINT || !is_int(sign_v) || !is_int(words_v))
    return false;
  int64_t s = rt_untag_v(sign_v);
  int64_t w = rt_untag_v(words_v);
  if (w < 0 || (uint64_t)w > SIZE_MAX / sizeof(uint64_t))
    return false;
  size_t n = (size_t)w;
  size_t bytes = n * sizeof(uint64_t);
  /*
   * Live allocator bounds prove the limb span without a mapping syscall.
   * Foreign callers retain the range probe and all overflow checks.
   */
  if (base > UINTPTR_MAX - 16u ||
      (managed ? bytes > heap_size - 16u
               : !_bi_readable_range(base + 16u, bytes)))
    return false;
  *sign = s < 0 ? -1 : (s > 0 ? 1 : 0);
  *words = n;
  *limbs = (const uint64_t *)(base + 16u);
  while (*words > 0 && (*limbs)[*words - 1] == 0)
    (*words)--;
  if (*words == 0)
    *sign = 0;
  return true;
}

/*
 * Decode a bigint used as a non-negative bit/count operand without silently
 * truncating high limbs.  Return -1 for a negative value, 0 for a value that
 * fits uint64_t, and 1 for a larger positive value.  Invalid dynamic values
 * retain the runtime's historical zero-coercion behavior.
 */
static inline int _bi_decode_u64_count(int64_t v, uint64_t *out) {
  int sign = 0;
  size_t words = 0;
  const uint64_t *limbs = NULL;
  uint64_t small[1] = {0};
  if (out)
    *out = 0;
  if (!_bi_read_heap_limbs(v, &sign, &limbs, &words, small))
    return 0;
  if (sign < 0)
    return -1;
  if (words > 1)
    return 1;
  if (out && words == 1)
    *out = limbs[0];
  return 0;
}

static bool _bi_row_payload_len(int64_t row, size_t *out_len) {
  if (out_len)
    *out_len = 0;
  if (!is_ptr(row) || !out_len || !_bi_readable_range((uintptr_t)row, 16u))
    return false;
  int64_t stored_len = 0;
  memcpy(&stored_len, (const void *)(uintptr_t)row, sizeof(stored_len));
  int64_t raw_len = is_int(stored_len) ? rt_untag_v(stored_len) : stored_len;
  if (raw_len < 0 || (uint64_t)raw_len > (SIZE_MAX - 16u) / sizeof(int64_t))
    return false;
  size_t len = (size_t)raw_len;
  size_t bytes = 16u + len * sizeof(int64_t);
  if (!_bi_readable_range((uintptr_t)row, bytes))
    return false;
  *out_len = len;
  return true;
}

/*
 * Common BigInt workloads repeatedly divide a short multi-limb value by a
 * machine-sized positive/negative divisor.  Avoid materializing two temporary
 * _bn_t allocations and running general Knuth division for that case.
 */
#define NY_BI_SHORT_LIMBS 8u

/*
 * Add or subtract validated limb views directly into the final heap object.
 * This preserves the BigInt ABI without temporary operand or result copies.
 */
static inline bool _bi_try_add_heap(int64_t a, int64_t b, int sub_b,
                                    int64_t *out) {
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t asmall[1], bsmall[1];
  const uint64_t *al = NULL, *bl = NULL;
  if (!_bi_read_heap_limbs(a, &as, &al, &aw, asmall) ||
      !_bi_read_heap_limbs(b, &bs, &bl, &bw, bsmall))
    return false;
  if (sub_b)
    bs = -bs;
  size_t capacity = aw > bw ? aw : bw;
  if (capacity && as == bs && as != 0) {
    __uint128_t top = (__uint128_t)(aw == capacity ? al[aw - 1] : 0) +
                      (bw == capacity ? bl[bw - 1] : 0);
    if (top >= UINT64_MAX)
      ++capacity;
  }
  int64_t result = _bi_alloc(capacity);
  if (!result)
    return false;
  uint64_t *rl = (uint64_t *)(uintptr_t)(result + 16);
  int rs = 0;
  size_t rw = 0;
  if (as == 0) {
    rs = bs;
    rw = bw;
    if (bw)
      memcpy(rl, bl, bw * sizeof(*bl));
  } else if (bs == 0) {
    rs = as;
    rw = aw;
    if (aw)
      memcpy(rl, al, aw * sizeof(*al));
  } else if (as == bs) {
    uint64_t carry = 0;
    size_t n = aw > bw ? aw : bw;
    for (size_t i = 0; i < n; ++i) {
      __uint128_t sum =
          (__uint128_t)(i < aw ? al[i] : 0) + (i < bw ? bl[i] : 0) + carry;
      rl[i] = (uint64_t)sum;
      carry = (uint64_t)(sum >> 64);
    }
    rw = n;
    if (carry)
      rl[rw++] = carry;
    rs = as;
  } else {
    int cmp = _bi_cmp_mag(al, aw, bl, bw);
    if (cmp == 0) {
      rs = 0;
      rw = 0;
    } else {
      const uint64_t *hi = cmp > 0 ? al : bl;
      const uint64_t *lo = cmp > 0 ? bl : al;
      size_t hiw = cmp > 0 ? aw : bw;
      size_t low = cmp > 0 ? bw : aw;
      uint64_t borrow = 0;
      for (size_t i = 0; i < hiw; ++i) {
        uint64_t hv = hi[i];
        uint64_t lv = i < low ? lo[i] : 0;
        uint64_t sub = lv + borrow;
        uint64_t next = (borrow && sub == 0) || hv < sub;
        rl[i] = hv - sub;
        borrow = next;
      }
      rw = _bi_norm_words(rl, hiw);
      rs = cmp > 0 ? as : bs;
    }
  }
  *(int64_t *)(uintptr_t)result = rt_tag_v(rs);
  *(int64_t *)(uintptr_t)(result + 8) = rt_tag_v((int64_t)rw);
  *out = result;
  return true;
}

static inline bool _bi_try_mul_small_heap(int64_t a, int64_t b, int64_t *out) {
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t asmall[1], bsmall[1];
  const uint64_t *al = NULL, *bl = NULL;
  if (!_bi_read_heap_limbs(a, &as, &al, &aw, asmall) ||
      !_bi_read_heap_limbs(b, &bs, &bl, &bw, bsmall))
    return false;
  if (aw == 0 || bw == 0) {
    *out = _bi_from_limbs(0, asmall, 0);
    return *out != 0;
  }
  /*
   * BigInts are immutable; multiplication by positive one can retain
   * an existing heap object without changing its representation.
   */
  if (bw == 1 && bl[0] == 1 && bs > 0 && is_ptr(a)) {
    *out = a;
    return true;
  }
  if (aw == 1 && al[0] == 1 && as > 0 && is_ptr(b)) {
    *out = b;
    return true;
  }
  const uint64_t *wide = NULL;
  size_t ww = 0;
  uint64_t factor = 0;
  if (bw == 1) {
    wide = al;
    ww = aw;
    factor = bl[0];
  } else if (aw == 1) {
    wide = bl;
    ww = bw;
    factor = al[0];
  } else {
    if (aw > NY_BI_SHORT_LIMBS || bw > NY_BI_SHORT_LIMBS)
      return false;
    /*
     * Both magnitudes fit bounded native limbs: multiply directly into
     * scratch instead of copying each input into a general bigint. The
     * final carry word is untouched by earlier rows.
     */
    int64_t result = _bi_alloc(aw + bw);
    if (!result)
      return false;
    uint64_t *product = (uint64_t *)(uintptr_t)(result + 16);
    _bn_words_mul(product, al, aw, bl, bw, NULL);
    size_t words = _bi_norm_words(product, aw + bw);
    *(int64_t *)(uintptr_t)result = rt_tag_v(as == bs ? 1 : -1);
    *(int64_t *)(uintptr_t)(result + 8) = rt_tag_v((int64_t)words);
    *out = result;
    return true;
  }
  int64_t result = _bi_alloc(ww + 1);
  if (!result)
    return false;
  uint64_t *rl = (uint64_t *)(uintptr_t)(result + 16);
  uint64_t carry = 0;
  for (size_t i = 0; i < ww; ++i) {
    __uint128_t prod = (__uint128_t)wide[i] * factor + carry;
    rl[i] = (uint64_t)prod;
    carry = (uint64_t)(prod >> 64);
  }
  size_t rw = ww;
  if (carry)
    rl[rw++] = carry;
  *(int64_t *)(uintptr_t)result = rt_tag_v(as == bs ? 1 : -1);
  *(int64_t *)(uintptr_t)(result + 8) = rt_tag_v((int64_t)rw);
  *out = result;
  return true;
}

static inline bool _bi_try_cmp_heap(int64_t a, int64_t b, int *out) {
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t asmall[1], bsmall[1];
  const uint64_t *al = NULL, *bl = NULL;
  if (!_bi_read_heap_limbs(a, &as, &al, &aw, asmall) ||
      !_bi_read_heap_limbs(b, &bs, &bl, &bw, bsmall))
    return false;
  if (as != bs) {
    *out = as > bs ? 1 : -1;
    return true;
  }
  if (as == 0) {
    *out = 0;
    return true;
  }
  int cmp = _bi_cmp_mag(al, aw, bl, bw);
  *out = as < 0 ? -cmp : cmp;
  return true;
}

static inline bool _bi_try_div_small_heap(int64_t a, int64_t b,
                                          int want_remainder, int64_t *out) {
  int as = 0, bs = 0;
  size_t aw = 0, bw = 0;
  uint64_t asmall[1], bsmall[1];
  const uint64_t *al = NULL, *bl = NULL;
  /* Most large division operands have a multi-limb divisor. Reject those
   * before validating the dividend: the general path will read it once after
   * this helper returns. */
  if (!_bi_read_heap_limbs(b, &bs, &bl, &bw, bsmall) || bw != 1 || bl[0] == 0)
    return false;
  if (!_bi_read_heap_limbs(a, &as, &al, &aw, asmall))
    return false;
  if (aw == 0) {
    *out = _bi_from_limbs(0, asmall, 0);
    return true;
  }
  uint64_t rem = 0;
  uint64_t d = bl[0];
  if (want_remainder) {
    for (size_t i = aw; i > 0; --i) {
      __uint128_t cur = ((__uint128_t)rem << 64) | al[i - 1];
      rem = (uint64_t)(cur % d);
    }
    uint64_t floor_rem = rem;
    int rem_sign = 0;
    if (rem != 0) {
      rem_sign = bs;
      if (as != bs)
        floor_rem = d - rem;
    }
    *out = _bi_from_limbs(rem_sign, &floor_rem, floor_rem != 0 ? 1u : 0u);
    return *out != 0;
  }
  if (aw > SIZE_MAX / sizeof(uint64_t))
    return false;
  uint64_t stack_q[8];
  uint64_t *q = aw <= (sizeof(stack_q) / sizeof(stack_q[0]))
                    ? stack_q
                    : (uint64_t *)malloc(aw * sizeof(uint64_t));
  if (!q)
    return false;
  for (size_t i = aw; i > 0; --i) {
    __uint128_t cur = ((__uint128_t)rem << 64) | al[i - 1];
    q[i - 1] = (uint64_t)(cur / d);
    rem = (uint64_t)(cur % d);
  }
  *out = _bi_from_limbs(as == 0 ? 0 : (as == bs ? 1 : -1), q, aw);
  if (q != stack_q)
    free(q);
  return *out != 0;
}

/*
 * BigInt entry points intentionally keep their heap-backed result contract,
 * even when both inputs are tagged small integers.  Check the native
 * operation before widening so the common one-limb case avoids the temporary
 * limb arrays used by the general compact path.  Values outside the tagged
 * range fall through and are promoted by the existing limb implementation.
 */
static inline int64_t _bi_alloc_small_result(int64_t raw) {
  size_t words = raw == 0 ? 0 : 1;
  int64_t p = _bi_alloc(words);
  if (!p)
    return 0;
  *(int64_t *)(uintptr_t)((char *)p + 0) = rt_tag_v(raw == 0  ? 0
                                                    : raw < 0 ? -1
                                                              : 1);
  if (words > 0) {
    uint64_t mag = raw < 0 ? (uint64_t)(-(raw + 1)) + 1u : (uint64_t)raw;
    *(uint64_t *)((char *)p + 16) = mag;
  }
  return p;
}

static inline bool _bi_try_small_add_alloc(int64_t a, int64_t b, int sub_b,
                                           int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  int64_t av = rt_untag_v(a);
  int64_t bv = rt_untag_v(b);
  int64_t raw = 0;
  bool overflow = sub_b ? __builtin_sub_overflow(av, bv, &raw)
                        : __builtin_add_overflow(av, bv, &raw);
  if (overflow || !ny_small_int_fits_i64(raw))
    return false;
  *out = rt_bigint_from_i64_raw(raw);
  return true;
}

static inline bool _bi_try_small_mul_alloc(int64_t a, int64_t b, int64_t *out) {
  if (!is_int(a) || !is_int(b))
    return false;
  int64_t raw = 0;
  if (__builtin_mul_overflow(rt_untag_v(a), rt_untag_v(b), &raw) ||
      !ny_small_int_fits_i64(raw))
    return false;
  *out = _bi_alloc_small_result(raw);
  return true;
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

static inline bool _bi_try_submul_limb2(int64_t a, int64_t q, int64_t b,
                                        int64_t *out) {
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
      __uint128_t s =
          (__uint128_t)(i < aw ? al[i] : 0) + (i < pw ? pl[i] : 0) + carry;
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
      __uint128_t s =
          (__uint128_t)(i < aw ? al[i] : 0) + (i < pw ? pl[i] : 0) + add_carry;
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
  if (_bi_try_small_add_alloc(a, b, 0, &fast))
    return fast;
  if (_bi_try_add_heap(a, b, 0, &fast))
    return fast;
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  uint64_t small_a[1], small_b[1];
  const uint64_t *al = NULL, *bl = NULL;
  (void)_bi_read_heap_limbs(a, &na.sign, &al, &na.count, small_a);
  (void)_bi_read_heap_limbs(b, &nb.sign, &bl, &nb.count, small_b);
  na.d = (uint64_t *)al;
  nb.d = (uint64_t *)bl;
  _bn_add(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_sub(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_small_add_alloc(a, b, 1, &fast))
    return fast;
  if (_bi_try_add_heap(a, b, 1, &fast))
    return fast;
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  uint64_t small_a[1], small_b[1];
  const uint64_t *al = NULL, *bl = NULL;
  (void)_bi_read_heap_limbs(a, &na.sign, &al, &na.count, small_a);
  (void)_bi_read_heap_limbs(b, &nb.sign, &bl, &nb.count, small_b);
  na.d = (uint64_t *)al;
  nb.d = (uint64_t *)bl;
  _bn_sub(&nr, &na, &nb);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_mul(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_small_mul_alloc(a, b, &fast))
    return fast;
  if (_bi_try_mul_limb2(a, b, &fast))
    return fast;
  if (_bi_try_mul_small_heap(a, b, &fast))
    return fast;
  _bn_t na, nb;
  _bn_init(&na);
  _bn_init(&nb);
  uint64_t small_a[1], small_b[1];
  const uint64_t *al = NULL, *bl = NULL;
  (void)_bi_read_heap_limbs(a, &na.sign, &al, &na.count, small_a);
  (void)_bi_read_heap_limbs(b, &nb.sign, &bl, &nb.count, small_b);
  na.d = (uint64_t *)al;
  nb.d = (uint64_t *)bl;
  size_t words = na.count + nb.count;
  if (words < na.count || words > (SIZE_MAX / sizeof(uint64_t) - 64) / 8)
    return 0;
  int64_t result = _bi_alloc(words);
  if (!result)
    return 0;
  size_t scratch_words = 8 * words + 64;
  uint64_t stack_work[1024];
  uint64_t *work = scratch_words <= 1024
                       ? stack_work
                       : malloc(scratch_words * sizeof(*work));
  if (!work)
    return 0;
  uint64_t *product = (uint64_t *)(uintptr_t)(result + 16);
  _bn_words_mul(product, na.d, na.count, nb.d, nb.count, work);
  if (work != stack_work)
    free(work);
  words = _bi_norm_words(product, words);
  *(int64_t *)(uintptr_t)result =
      rt_tag_v(words ? (na.sign == nb.sign ? 1 : -1) : 0);
  *(int64_t *)(uintptr_t)(result + 8) = rt_tag_v((int64_t)words);
  return result;
}

static int64_t _bi_submul_owned_result(int64_t value) {
  /*
   * BigInt entry points keep a heap-backed result even when the arithmetic
   * fits the tagged small-int range.  This lets callers retain the declared
   * BigInt representation instead of receiving an int only on fast paths.
   */
  return is_int(value) ? rt_bigint_from_i64_raw(rt_untag_v(value)) : value;
}

int64_t rt_bigint_submul(int64_t a, int64_t q, int64_t b) {
  if (q == rt_tag_v(0) || _bi_is_zero_fast(b))
    return _bi_submul_owned_result(a);
  int64_t fast = 0;
  if (q == rt_tag_v(1) && _bi_try_add_limb2_kind(a, b, 1, 1, &fast))
    return _bi_submul_owned_result(fast);
  if (q == rt_tag_v(-1) && _bi_try_add_limb2_kind(a, b, 0, 1, &fast))
    return _bi_submul_owned_result(fast);
  if (is_int(q) && _bi_try_submul_i64_limb2(a, rt_untag_v(q), b, &fast))
    return _bi_submul_owned_result(fast);
  if (_bi_try_submul_limb2(a, q, b, &fast))
    return _bi_submul_owned_result(fast);
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
}

int64_t rt_bigint_row_submul(int64_t row_k, int64_t row_j, int64_t q,
                             int64_t limit_v) {
  size_t row_k_len = 0, row_j_len = 0;
  if (!_bi_row_payload_len(row_k, &row_k_len) ||
      !_bi_row_payload_len(row_j, &row_j_len))
    return row_k;
  int64_t limit = is_int(limit_v) ? rt_untag_v(limit_v) : limit_v;
  if (limit < 0 || q == rt_tag_v(0))
    return row_k;
  size_t common_len = row_k_len < row_j_len ? row_k_len : row_j_len;
  if (common_len == 0)
    return row_k;
  if ((uint64_t)limit >= (uint64_t)common_len)
    limit = (int64_t)(common_len - 1u);
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
  if (q == rt_tag_v(0))
    return row_k;
  size_t row_k_len = 0, row_j_len = 0;
  if (!_bi_row_payload_len(row_k, &row_k_len) ||
      !_bi_row_payload_len(row_j, &row_j_len))
    return row_k;
  size_t common_len = row_k_len < row_j_len ? row_k_len : row_j_len;
  if (common_len == 0 || common_len - 1u > (size_t)INT64_MAX)
    return row_k;
  int64_t limit = (int64_t)(common_len - 1u);
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
  int64_t fast = 0;
  if (_bi_try_div_small_heap(a, b, 0, &fast))
    return fast;
  _bn_t na, nb, nq, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nq);
  _bn_init(&nr);
  /*
   * The quotient engine never mutates its operands. Keep validated limb
   * views alive through division rather than allocating two input copies.
   */
  uint64_t small_a[1], small_b[1];
  const uint64_t *limbs_a = NULL, *limbs_b = NULL;
  (void)_bi_read_heap_limbs(a, &na.sign, &limbs_a, &na.count, small_a);
  (void)_bi_read_heap_limbs(b, &nb.sign, &limbs_b, &nb.count, small_b);
  na.d = (uint64_t *)limbs_a;
  nb.d = (uint64_t *)limbs_b;
  /*
   * Most runtime BigInts fit in a handful of limbs. The quotient is copied
   * once into its managed result below, so keep its temporary limbs on the
   * stack instead of malloc/realloc/free for every division. The Knuth path
   * writes every quotient limb before reading it, and its bounded operand
   * scratch is stack-backed as well.
   */
  uint64_t quotient_stack[8];
  bool stack_quotient = na.count <= sizeof(quotient_stack) /
                                        sizeof(quotient_stack[0]);
  if (stack_quotient) {
    nq.d = quotient_stack;
    nq.count = na.count;
  }
  if (_bn_is_zero(&nb)) {
    return rt_division_by_zero();
  }
  int magnitude_cmp = _bn_cmp_mag(&na, &nb);
  if (magnitude_cmp < 0)
    return _bi_from_i64(0);
  /*
   * A quotient of magnitude one needs no normalized division or remainder.
   */
  size_t half_words = na.count;
  if (half_words && na.d[half_words - 1] == 1)
    --half_words;
  int half_cmp = half_words > nb.count ? 1 : half_words < nb.count ? -1 : 0;
  if (!half_cmp) {
    for (size_t i = half_words; i-- > 0;) {
      uint64_t half =
          (na.d[i] >> 1) | (i + 1 < na.count ? na.d[i + 1] << 63 : 0);
      if (half != nb.d[i]) {
        half_cmp = half > nb.d[i] ? 1 : -1;
        break;
      }
    }
  }
  if (half_cmp < 0)
    return _bi_from_i64(na.sign == nb.sign ? 1 : -1);
  if (nb.count >= 128 && na.count >= nb.count + BURNIKEL_ZIEGLER_THRESHOLD)
    _bn_tdiv_qr(&nq, &nr, &na, &nb);
  else
    _bn_divmod_knuth_impl(&nq, &nr, &na, &nb, false);
  if (!_bn_is_zero(&nq))
    nq.sign = na.sign == nb.sign ? 1 : -1;
  int64_t r = _bi_from_native(&nq);
  if (!stack_quotient)
    _bn_clear(&nq);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_mod(int64_t a, int64_t b) {
  int64_t fast = 0;
  if (_bi_try_div_small_heap(a, b, 1, &fast))
    return fast;
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
}

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

int64_t rt_bigint_cmp(int64_t a, int64_t b) {
  int fast = 0;
  if (_bi_try_cmp_heap(a, b, &fast))
    return rt_tag_v((int64_t)fast);
  _bn_t na, nb;
  _bn_init(&na);
  _bn_init(&nb);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  int r = _bn_cmp(&na, &nb);
  _bn_clear(&na);
  _bn_clear(&nb);
  return rt_tag_v((int64_t)(r > 0 ? 1 : (r < 0 ? -1 : 0)));
}

int64_t rt_bigint_cmp_raw(int64_t a, int64_t b) {
  return rt_untag_v(rt_bigint_cmp(a, b));
}

int64_t rt_bigint_to_str(int64_t a) {
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  char *s = _bn_to_str(&na);
  if (!s) {
    _bn_clear(&na);
    return 0;
  }
  int64_t r = rt_alloc_string(s);
  free(s);
  _bn_clear(&na);
  return r;
}

/*
 * Accept one optional sign followed by at least one ASCII decimal digit.
 * Validate once at the public boundary and strip a leading '+'.
 */
static const char *_bi_decimal_parse_start(const char *s) {
  if (!s)
    return NULL;
  uintptr_t p = (uintptr_t)s;
  if (!_bi_readable_range(p, 1u))
    return NULL;
  char c = *(const char *)p;
  bool plus = c == '+';
  if (c == '+' || c == '-') {
    if (p == UINTPTR_MAX || !_bi_readable_range(p + 1u, 1u))
      return NULL;
    p++;
    c = *(const char *)p;
  }
  if (c < '0' || c > '9')
    return NULL;
  for (;;) {
    if (c == '\0')
      break;
    if (c < '0' || c > '9')
      return NULL;
    if (p == UINTPTR_MAX)
      return NULL;
    p++;
    if (!_bi_readable_range(p, 1u))
      return NULL;
    c = *(const char *)p;
  }
  return plus ? s + 1 : s;
}

static bool _bi_readable_cstr_len(const char *s, size_t *out_len) {
  if (out_len)
    *out_len = 0;
  if (!s || !out_len)
    return false;
  uintptr_t p = (uintptr_t)s;
  size_t len = 0;
  for (;;) {
    if (!_bi_readable_range(p, 1u))
      return false;
    if (*(const char *)p == '\0') {
      *out_len = len;
      return true;
    }
    if (p == UINTPTR_MAX || len == SIZE_MAX)
      return false;
    p++;
    len++;
  }
}

/*
 * Parse the exact source spelling retained by the AST.  The literal's i64
 * field is intentionally saturated when the source value exceeds INT64_MAX,
 * so using that field here would make large unsigned and radix-prefixed
 * literals silently become INT64_MAX.
 */
int64_t rt_bigint_from_literal(int64_t str_ptr) {
  if (!str_ptr)
    return 0;
  const char *s = (const char *)(uintptr_t)str_ptr;
  size_t len = 0;
  if (!_bi_readable_cstr_len(s, &len) || len == 0)
    return 0;
  size_t i = 0;
  bool neg = false;
  if (s[i] == '+' || s[i] == '-') {
    neg = s[i] == '-';
    if (++i == len)
      return 0;
  }
  unsigned base = 10;
  if (i + 2 <= len && s[i] == '0') {
    if (s[i + 1] == 'x' || s[i + 1] == 'X') {
      base = 16;
      i += 2;
    } else if (s[i + 1] == 'b' || s[i + 1] == 'B') {
      base = 2;
      i += 2;
    } else if (s[i + 1] == 'o' || s[i + 1] == 'O') {
      base = 8;
      i += 2;
    }
  }
  if (i == len)
    return 0;
  _bn_t value;
  _bn_init(&value);
  for (; i < len; ++i) {
    int digit_value = ny_ascii_hex_digit_value((unsigned char)s[i]);
    if (digit_value < 0 || (unsigned)digit_value >= base) {
      _bn_clear(&value);
      return 0;
    }
    unsigned digit = (unsigned)digit_value;
    _bn_mul_u32_inplace(&value, base);
    _bn_add_u32_inplace(&value, digit);
  }
  if (neg && !_bn_is_zero(&value))
    value.sign = -1;
  int64_t result = _bi_from_native(&value);
  _bn_clear(&value);
  return result;
}

int64_t rt_bigint_from_str(int64_t str_ptr) {
  if (!str_ptr)
    return 0;
  /*
   * The interpreter/LLVM tiers pass tagged Ny strings, while direct native
   * NYIR uses raw NUL-terminated C strings.  Both representations point at
   * the first character, so parsing is identical once NULL is rejected.
   */
  const char *s = _bi_decimal_parse_start((const char *)(uintptr_t)str_ptr);
  if (!s)
    return 0;
  _bn_t val;
  _bn_init(&val);
  if (!_bn_from_str(&val, s)) {
    _bn_clear(&val);
    return 0;
  }
  int64_t r = _bi_from_native(&val);
  _bn_clear(&val);
  return r;
}

int64_t rt_bigint_to_bytes(int64_t a) {
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  if (_bn_is_zero(&na)) {
    _bn_clear(&na);
    int64_t res = rt_list_new(rt_tag_v(0));
    res = rt_append(res, rt_tag_v(0));
    return res;
  }
  size_t count = 0;
  uint8_t *buf = _bn_to_bytes_be(&na, &count);
  int64_t tagged_count = 0;
  if (!_bi_tag_size(count, &tagged_count)) {
    free(buf);
    _bn_clear(&na);
    return 0;
  }
  int64_t res = rt_list_new(tagged_count);
  for (size_t i = 0; i < count; ++i)
    res = rt_append(res, rt_tag_v((int64_t)buf[i]));
  free(buf);
  _bn_clear(&na);
  return res;
}

int64_t rt_bigint_powmod(int64_t base, int64_t exp, int64_t mod) {
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
}

int64_t rt_bigint_modinv(int64_t a, int64_t m) {
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
}

int64_t rt_bigint_gcd(int64_t a, int64_t b) {
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
}

int64_t rt_bigint_bitlen(int64_t a) {
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
}

/*
 * Native NYIR keeps scalar results untagged.  The interpreter-facing
 * bit-length intrinsic above returns the ordinary tagged integer ABI.
 */
int64_t rt_bigint_bitlen_raw(int64_t a) {
  return rt_untag_v(rt_bigint_bitlen(a));
}

int64_t rt_bigint_popcount(int64_t a) { return _bi_popcount_native(a); }

int64_t rt_bigint_to_i64_raw(int64_t a) {
  if (is_int(a))
    return rt_untag_v(a);
  _bn_t na;
  _bn_init(&na);
  _bi_val_to_native(a, &na);
  uint64_t bits = _bn_is_zero(&na)
                      ? UINT64_C(0)
                      : (na.sign >= 0 ? na.d[0] : UINT64_C(0) - na.d[0]);
  int64_t v = _bi_i64_from_u64_bits(bits);
  _bn_clear(&na);
  return v;
}

bool rt_bigint_to_i64_checked(int64_t value, int64_t *out) {
  if (!out)
    return false;
  if (is_int(value)) {
    *out = rt_untag_v(value);
    return true;
  }
  if (rt_value_tag(value) != TAG_BIGINT)
    return false;
  _bn_t integer;
  _bn_init(&integer);
  _bi_val_to_native(value, &integer);
  bool fits = integer.count == 0 ||
              (integer.count == 1 &&
               (integer.sign < 0 ? integer.d[0] <= (UINT64_C(1) << 63)
                                 : integer.d[0] <= (uint64_t)INT64_MAX));
  if (fits) {
    uint64_t bits = integer.count == 0 ? UINT64_C(0)
                    : integer.sign < 0 ? UINT64_C(0) - integer.d[0]
                                       : integer.d[0];
    *out = _bi_i64_from_u64_bits(bits);
  }
  _bn_clear(&integer);
  return fits;
}

int64_t rt_bigint_to_int(int64_t a) {
  return ny_value_box_i64(rt_bigint_to_i64_raw(a));
}

static double _bi_to_double_fast(int64_t a) {
  if (is_int(a))
    return (double)rt_untag_v(a);

  int sign = 0;
  size_t words = 0;
  const uint64_t *limbs = NULL;
  uint64_t small[1] = {0};
  if (!_bi_read_heap_limbs(a, &sign, &limbs, &words, small) || sign == 0 ||
      words == 0)
    return 0.0;

  /*
   * A finite IEEE-754 binary64 has at most 1024 significant integer bits.
   * More limbs cannot round back into the finite range.
   */
  if (words > 16u)
    return sign < 0 ? -INFINITY : INFINITY;

  uint64_t top = limbs[words - 1u];
  unsigned top_bits = 64u - (unsigned)__builtin_clzll(top);
  size_t bitlen = (words - 1u) * 64u + (size_t)top_bits;
  uint64_t q = 0;

  if (bitlen <= 53u) {
    q = limbs[0];
    double d = (double)q;
    return sign < 0 ? -d : d;
  }

  size_t shift = bitlen - 53u;
  size_t word_shift = shift / 64u;
  unsigned bit_shift = (unsigned)(shift % 64u);
  q = limbs[word_shift] >> bit_shift;
  if (bit_shift != 0u && word_shift + 1u < words)
    q |= limbs[word_shift + 1u] << (64u - bit_shift);
  q &= UINT64_C(0x001fffffffffffff); /* exactly the top 53 bits */

  /*
   * Round N / 2^shift to nearest, ties to even.
   */
  size_t guard_index = shift - 1u;
  size_t guard_word = guard_index / 64u;
  unsigned guard_bit = (unsigned)(guard_index % 64u);
  bool guard = ((limbs[guard_word] >> guard_bit) & 1u) != 0;
  bool sticky = false;
  for (size_t i = 0; i < guard_word && !sticky; ++i)
    sticky = limbs[i] != 0;
  if (!sticky && guard_bit != 0u) {
    uint64_t mask = (UINT64_C(1) << guard_bit) - 1u;
    sticky = (limbs[guard_word] & mask) != 0;
  }
  if (guard && (sticky || (q & 1u) != 0))
    q++;

  double d = ldexp((double)q, (int)shift);
  return sign < 0 ? -d : d;
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
  uintptr_t base = (uintptr_t)buf;
  if ((uint64_t)i > (uint64_t)UINTPTR_MAX / sizeof(double))
    return buf;
  uintptr_t offset = (uintptr_t)i * sizeof(double);
  if (base > UINTPTR_MAX - offset)
    return buf;
  uintptr_t p = base + offset;
  if (p < 0x1000 || !_bi_readable_range(p, sizeof(double)))
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
}

int64_t rt_bigint_from_int(int64_t n) {
  /*
   * JIT calls carry tagged ints; native NYIR calls carry raw scalar slots.
   */
  int64_t raw = is_int(n) ? rt_untag_v(n) : n;
  return rt_bigint_from_i64_raw(raw);
}

static int64_t rt_bigint_from_bytes_be_stored(const uint8_t *bytes,
                                              size_t len) {
  _bn_t val;
  _bn_init(&val);
  _bn_from_bytes_be(&val, bytes, len);
  int64_t out = _bi_from_native(&val);
  _bn_clear(&val);
  return out;
}

static int64_t rt_bigint_from_element_bytes(uintptr_t base, size_t count,
                                            size_t stride, size_t scalar_bytes,
                                            bool tagged_items) {
  if (count == 0)
    return _bi_from_i64(0);
  if (stride == 0 || scalar_bytes == 0 || scalar_bytes > sizeof(int64_t) ||
      count - 1u > (UINTPTR_MAX - base) / stride)
    return _bi_from_i64(0);

  uint8_t stack_bytes[256];
  uint8_t *bytes =
      count <= sizeof(stack_bytes) ? stack_bytes : (uint8_t *)malloc(count);
  if (!bytes)
    return 0;
  for (size_t i = 0; i < count; ++i) {
    int64_t item = 0;
    memcpy(&item, (const void *)(base + i * stride), scalar_bytes);
    int64_t raw = tagged_items ? (is_int(item) ? rt_untag_v(item) : 0) : item;
    bytes[i] = (uint8_t)((uint64_t)raw & UINT64_C(0xff));
  }
  int64_t out = rt_bigint_from_bytes_be_stored(bytes, count);
  if (bytes != stack_bytes)
    free(bytes);
  return out;
}

static int64_t rt_long_bytes_like(int64_t v) {
  if ((uintptr_t)v < 16u)
    return _bi_from_i64(0);
  uintptr_t len_ptr = (uintptr_t)v - 16u;
  if (!_bi_readable_range(len_ptr, sizeof(int64_t)))
    return _bi_from_i64(0);
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)len_ptr, sizeof(tagged_len));
  int64_t len = is_int(tagged_len) ? (tagged_len >> 1) : tagged_len;
  if (len <= 0)
    return _bi_from_i64(0);
  if (!_bi_readable_range((uintptr_t)v, (size_t)len))
    return _bi_from_i64(0);
  return rt_bigint_from_bytes_be_stored((const uint8_t *)(uintptr_t)v,
                                        (size_t)len);
}

/*
 * Native lists are compact typed buffers rather than managed TAG_LIST
 * objects.  Their scalar payload is deliberately raw (the 24-byte variant
 * stores the value in the first word), so the ordinary `is_int` test used by
 * the legacy list path cannot decode them.  Keep this adapter here because
 * `.long` is also reachable through `any` and must accept either ABI.
 */
static int64_t rt_long_tbuf(int64_t v) {
  enum { RT_LONG_TBUF_HEADER = 32 };
  const uint64_t magic = UINT64_C(0x4e59544255464d47);
  if (!v || (uintptr_t)v < RT_LONG_TBUF_HEADER ||
      !_bi_readable_range((uintptr_t)v - RT_LONG_TBUF_HEADER,
                          RT_LONG_TBUF_HEADER))
    return 0;
  int64_t hdr[4] = {0, 0, 0, 0};
  memcpy(hdr, (const void *)((uintptr_t)v - RT_LONG_TBUF_HEADER), sizeof(hdr));
  int64_t count = hdr[1], elem_size = hdr[2];
  if ((uint64_t)hdr[0] != magic || count <= 0 || elem_size <= 0 ||
      count > INT64_MAX / elem_size ||
      (uint64_t)count > SIZE_MAX / (uint64_t)elem_size)
    return 0;
  size_t payload_bytes = (size_t)count * (size_t)elem_size;
  if (!_bi_readable_range((uintptr_t)v, payload_bytes))
    return 0;
  if (elem_size == 1)
    return rt_bigint_from_bytes_be_stored((const uint8_t *)(uintptr_t)v,
                                          (size_t)count);
  size_t scalar_bytes =
      (size_t)elem_size < sizeof(int64_t) ? (size_t)elem_size : sizeof(int64_t);
  return rt_bigint_from_element_bytes((uintptr_t)v, (size_t)count,
                                      (size_t)elem_size, scalar_bytes, false);
}

/*
 * A typed list parameter is passed as its raw element pointer plus a hidden
 * count, not as a native tbuf handle.  Keep that ABI explicit at the bigint
 * boundary instead of making rt_long guess from an unowned pointer.
 */
int64_t rt_long_list_raw(int64_t data, int64_t count) {
  if (!data || count <= 0 || (uint64_t)count > SIZE_MAX / sizeof(int64_t) ||
      !_bi_readable_range((uintptr_t)data, (size_t)count * sizeof(int64_t)))
    return _bi_from_i64(0);
  return rt_bigint_from_element_bytes((uintptr_t)data, (size_t)count,
                                      sizeof(int64_t), sizeof(int64_t), false);
}

static int64_t _bi_from_f64_trunc(double d) {
  if (!isfinite(d))
    return _bi_from_i64(0);
  uint64_t bits = 0;
  memcpy(&bits, &d, sizeof(bits));
  bool negative = (bits >> 63) != 0;
  unsigned exponent_bits = (unsigned)((bits >> 52) & 0x7ffu);
  uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);
  if (exponent_bits == 0)
    return _bi_from_i64(0);

  int exponent = (int)exponent_bits - 1023;
  if (exponent < 0)
    return _bi_from_i64(0);
  uint64_t mantissa = fraction | UINT64_C(0x0010000000000000);

  _bn_t value = {0};
  if (exponent <= 52) {
    _bn_set_i64(&value, (int64_t)(mantissa >> (52 - exponent)));
  } else {
    _bn_set_i64(&value, (int64_t)mantissa);
    _bn_shl(&value, &value, (size_t)(exponent - 52));
  }
  if (negative && !_bn_is_zero(&value))
    value.sign = -1;
  int64_t out = _bi_from_native(&value);
  _bn_clear(&value);
  return out;
}

int64_t rt_long(int64_t v) {
  int64_t tbuf = rt_long_tbuf(v);
  if (tbuf)
    return tbuf;
  if (is_v_flt(v)) {
    int64_t bits = _rt_flt_unbox_val(v);
    double d = 0.0;
    memcpy(&d, &bits, sizeof(d));
    return _bi_from_f64_trunc(d);
  }
  if (is_v_str(v) || rt_native_is_str(v)) {
    size_t len = 0;
    if (is_v_str(v)) {
      len = rt_tagged_str_len(v);
      if (!_bi_readable_range((uintptr_t)v, len))
        return _bi_from_i64(0);
    } else if (!_bi_readable_cstr_len((const char *)(uintptr_t)v, &len)) {
      return _bi_from_i64(0);
    }
    if (len == 0)
      return _bi_from_i64(0);
    return rt_bigint_from_bytes_be_stored((const uint8_t *)(uintptr_t)v, len);
  }
  /*
   * Dynamic Ny integers are tagged.  `rt_bigint_from_int` is the raw/native
   * scalar bridge; `rt_long` is the generic normalization boundary and must
   * decode an immediate integer before constructing a heap BigInt.
   */
  if (is_int(v))
    return _bi_from_i64(rt_untag_v(v));
  if (!is_ptr(v))
    return _bi_from_i64(v);
  uintptr_t base = (uintptr_t)v;
  if (base < 8u || !_bi_readable_range(base - 8u, sizeof(int64_t)))
    return _bi_from_i64(0);
  int64_t tag = 0;
  memcpy(&tag, (const void *)(base - 8u), sizeof(tag));
  if (tag == TAG_FLOAT) {
    if (!_bi_readable_range(base, sizeof(int64_t)))
      return _bi_from_i64(0);
    int64_t bits = 0;
    memcpy(&bits, (const void *)base, sizeof(bits));
    double d = 0.0;
    memcpy(&d, &bits, sizeof(d));
    return _bi_from_f64_trunc(d);
  }
  if (tag == TAG_BIGINT) {
    int sign = 0;
    size_t words = 0;
    const uint64_t *limbs = NULL;
    uint64_t small[1] = {0};
    return _bi_read_heap_limbs(v, &sign, &limbs, &words, small)
               ? v
               : _bi_from_i64(0);
  }
  if (tag == TAG_BYTES)
    return rt_long_bytes_like(v);
  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    if (!_bi_readable_range((uintptr_t)v, 16u))
      return _bi_from_i64(0);
    int64_t tagged_len = 0;
    memcpy(&tagged_len, (const void *)(uintptr_t)v, sizeof(tagged_len));
    int64_t len = is_int(tagged_len) ? rt_untag_v(tagged_len) : tagged_len;
    if (len <= 0 || (uint64_t)len > (SIZE_MAX - 16u) / sizeof(int64_t))
      return _bi_from_i64(0);
    size_t need = 16u + (size_t)len * sizeof(int64_t);
    if (!_bi_readable_range((uintptr_t)v, need))
      return _bi_from_i64(0);
    return rt_bigint_from_element_bytes((uintptr_t)v + 16u, (size_t)len,
                                        sizeof(int64_t), sizeof(int64_t), true);
  }
  return _bi_from_i64(0);
}

int64_t rt_bigint_pow(int64_t b, int64_t e) {
  _bn_t nb, ne, nr;
  _bn_init(&nb);
  _bn_init(&ne);
  _bn_init(&nr);
  _bi_val_to_native(b, &nb);
  _bi_val_to_native(e, &ne);
  _bn_pow_bigexp(&nr, &nb, &ne);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nb);
  _bn_clear(&ne);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_isqrt(int64_t a) {
  _bn_t na, nr;
  _bn_init(&na);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bn_isqrt(&nr, &na);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_legendre(int64_t a, int64_t p) {
  _bn_t na, np;
  _bn_init(&na);
  _bn_init(&np);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(p, &np);
  int r = _bn_jacobi_val(&na, &np);
  _bn_clear(&na);
  _bn_clear(&np);
  return rt_tag_v(r);
}

int64_t rt_bigint_jacobi(int64_t a, int64_t n) {
  _bn_t na, nn;
  _bn_init(&na);
  _bn_init(&nn);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(n, &nn);
  int r = _bn_jacobi_val(&na, &nn);
  _bn_clear(&na);
  _bn_clear(&nn);
  return rt_tag_v(r);
}

int64_t rt_bigint_kronecker(int64_t a, int64_t n) {
  _bn_t na, nn;
  _bn_init(&na);
  _bn_init(&nn);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(n, &nn);
  int r = _bn_kronecker_val(&na, &nn);
  _bn_clear(&na);
  _bn_clear(&nn);
  return rt_tag_v(r);
}

int64_t rt_bigint_iroot(int64_t n, int64_t k) {
  _bn_t nn, nk, nr;
  _bn_init(&nn);
  _bn_init(&nk);
  _bn_init(&nr);
  _bi_val_to_native(n, &nn);
  _bi_val_to_native(k, &nk);
  bool k_fits = nk.sign > 0 && !_bn_is_zero(&nk) && nk.count == 1 &&
                nk.d[0] <= (uint64_t)ULONG_MAX;
  if (k_fits) {
    _bn_iroot(&nr, &nn, (unsigned long)nk.d[0]);
  } else if (nk.sign > 0 && !_bn_is_zero(&nk)) {
    if (_bn_is_zero(&nn) || (nn.sign < 0 && (nk.d[0] & 1u) == 0))
      _bn_set_zero(&nr);
    else
      _bn_set_i64(&nr, nn.sign < 0 ? -1 : 1);
  } else {
    _bn_set_zero(&nr);
  }
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&nn);
  _bn_clear(&nk);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_is_perfect_square(int64_t n) {
  _bn_t nn;
  _bn_init(&nn);
  _bi_val_to_native(n, &nn);
  bool r = _bn_perfect_square_p(&nn);
  _bn_clear(&nn);
  return rt_tag_v(r ? 1 : 0);
}

int64_t rt_bigint_xgcd(int64_t a, int64_t b) {
  _bn_t na, nb, ng, nx, ny;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&ng);
  _bn_init(&nx);
  _bn_init(&ny);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bn_xgcd(&ng, &nx, &ny, &na, &nb);
  int64_t res = rt_list_new(rt_tag_v(3));
  res = rt_append(res, _bi_from_native(&ng));
  res = rt_append(res, _bi_from_native(&nx));
  res = rt_append(res, _bi_from_native(&ny));
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&ng);
  _bn_clear(&nx);
  _bn_clear(&ny);
  return res;
}

int64_t rt_bigint_clz(int64_t a) {
  if (is_int(a)) {
    int64_t v = rt_untag_v(a);
    if (v == 0)
      return rt_tag_v(64);
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    size_t bits = (size_t)(64 - __builtin_clzll(mag));
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
}

int64_t rt_bigint_ctz(int64_t a) {
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
  int64_t r = _bn_is_zero(&na) ? 64 : (int64_t)_bn_ctz_mag(&na);
  _bn_clear(&na);
  return rt_tag_v(r);
}

int64_t rt_bigint_not(int64_t a) {
  _bn_t na, nr;
  _bn_init(&na);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bn_com(&nr, &na);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_shl(int64_t a, int64_t b) {
  uint64_t shift = 0;
  int shift_state = _bi_decode_u64_count(b, &shift);
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  if (shift_state < 0 || shift == 0) {
    _bn_copy(&nr, &na);
  } else if (shift_state > 0 || shift > (uint64_t)SIZE_MAX) {
    if (_bn_is_zero(&na))
      _bn_set_zero(&nr);
    else {
      _bn_clear(&na);
      _bn_clear(&nb);
      _bn_clear(&nr);
      return 0;
    }
  } else {
    _bn_shl(&nr, &na, (size_t)shift);
  }
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_shr(int64_t a, int64_t b) {
  uint64_t shift = 0;
  int shift_state = _bi_decode_u64_count(b, &shift);
  _bn_t na, nb, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  if (shift_state < 0 || shift == 0) {
    _bn_copy(&nr, &na);
  } else if (shift_state > 0 || shift > (uint64_t)SIZE_MAX) {
    _bn_set_i64(&nr, na.sign < 0 ? -1 : 0);
  } else {
    _bn_shr(&nr, &na, (size_t)shift);
  }
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nr);
  return r;
}

int64_t rt_bigint_gf2_mod(int64_t a, int64_t m) {
  _bn_t na, nm, nr;
  _bn_init(&na);
  _bn_init(&nm);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(m, &nm);
  na.sign = _bn_is_zero(&na) ? 0 : 1;
  nm.sign = _bn_is_zero(&nm) ? 0 : 1;
  _bn_gf2_mod(&nr, &na, &nm);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nm);
  _bn_clear(&nr);
  return r;
}
int64_t rt_bigint_gf2_mulmod(int64_t a, int64_t b, int64_t m) {
  _bn_t na, nb, nm, nr;
  _bn_init(&na);
  _bn_init(&nb);
  _bn_init(&nm);
  _bn_init(&nr);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(b, &nb);
  _bi_val_to_native(m, &nm);
  na.sign = _bn_is_zero(&na) ? 0 : 1;
  nb.sign = _bn_is_zero(&nb) ? 0 : 1;
  nm.sign = _bn_is_zero(&nm) ? 0 : 1;
  _bn_gf2_mulmod(&nr, &na, &nb, &nm);
  int64_t r = _bi_from_native(&nr);
  _bn_clear(&na);
  _bn_clear(&nb);
  _bn_clear(&nm);
  _bn_clear(&nr);
  return r;
}
int64_t rt_bigint_gf2_inv(int64_t a, int64_t m) {
  _bn_t na, nm;
  _bn_init(&na);
  _bn_init(&nm);
  _bi_val_to_native(a, &na);
  _bi_val_to_native(m, &nm);
  na.sign = _bn_is_zero(&na) ? 0 : 1;
  nm.sign = _bn_is_zero(&nm) ? 0 : 1;

  _bn_t r0 = {0}, r1 = {0}, t0 = {0}, t1 = {0};
  _bn_copy(&r0, &nm);
  _bn_gf2_mod(&r1, &na, &nm);
  _bn_set_i64(&t0, 0);
  _bn_set_i64(&t1, 1);
  while (!_bn_is_zero(&r1)) {
    _bn_t q = {0}, rem = {0};
    _bn_gf2_div_qr(&q, &rem, &r0, &r1);
    _bn_clear(&r0);
    _bn_copy(&r0, &r1);
    _bn_clear(&r1);
    _bn_copy(&r1, &rem);
    _bn_clear(&rem);
    _bn_t prod = {0};
    _bn_gf2_mulmod(&prod, &q, &t1, &nm);
    _bn_t next_t = {0};
    _bn_copy(&next_t, &t0);
    _bn_gf2_xor_shifted_inplace(&next_t, &prod, 0);
    _bn_gf2_mod(&next_t, &next_t, &nm);
    _bn_clear(&q);
    _bn_clear(&prod);
    _bn_clear(&t0);
    _bn_copy(&t0, &t1);
    _bn_clear(&t1);
    _bn_copy(&t1, &next_t);
    _bn_clear(&next_t);
  }
  int64_t out = _bn_gf2_deg(&r0) == 0 ? _bi_from_native(&t0) : _bi_from_i64(0);
  _bn_clear(&na);
  _bn_clear(&nm);
  _bn_clear(&r0);
  _bn_clear(&r1);
  _bn_clear(&t0);
  _bn_clear(&t1);
  return out;
}
int64_t rt_ct_compare(int64_t a_ptr, int64_t b_ptr, int64_t len_val) {
  const uint8_t *a = (const uint8_t *)(uintptr_t)a_ptr;
  const uint8_t *b = (const uint8_t *)(uintptr_t)b_ptr;
  int64_t raw_len = len_val >> 1;
  if (raw_len <= 0)
    return rt_tag_v(0);
  if (!a || !b)
    return rt_tag_v(1);
  size_t len = (size_t)raw_len;
  if (!_bi_readable_range((uintptr_t)a, len) ||
      !_bi_readable_range((uintptr_t)b, len))
    return rt_tag_v(1);
  uint8_t result = 0;
  for (size_t i = 0; i < len; ++i)
    result |= (uint8_t)(a[i] ^ b[i]);
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
  int sign = v < 0 ? -1 : (v > 0 ? 1 : 0);
  uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
  return _bi_from_limbs(sign, &mag, sign == 0 ? 0u : 1u);
}
