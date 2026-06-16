#include "base/common.h"
#include "rt/runtime.h"
#include "rt/shared.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t rt_division_by_zero(void) { return rt_panic(rt_alloc_string("division by zero")); }

int64_t rt_modulo_by_zero(void) { return rt_panic(rt_alloc_string("modulo by zero")); }

static inline int64_t rt_flt_box_double(double d);

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

static inline int rt_is_complex_raw(int64_t v) {
  return is_ptr(v) && is_heap_ptr(v) &&
         *(int64_t *)((char *)(uintptr_t)v - 8) == TAG_COMPLEX;
}

static inline double rt_complex_re_raw(int64_t v) {
  if (rt_is_complex_raw(v)) {
    double d = 0.0;
    memcpy(&d, (const void *)(uintptr_t)v, 8);
    return d;
  }
  return get_flt(v);
}

static inline double rt_complex_im_raw(int64_t v) {
  if (rt_is_complex_raw(v)) {
    double d = 0.0;
    memcpy(&d, (const void *)((uintptr_t)v + 8), 8);
    return d;
  }
  return 0.0;
}

static int64_t rt_complex_box_double(double re, double im) {
  int64_t p = rt_malloc((int64_t)((16u << 1) | 1u));
  if (!p)
    return 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_COMPLEX;
  memcpy((void *)(uintptr_t)p, &re, 8);
  memcpy((void *)((uintptr_t)p + 8), &im, 8);
  return p;
}

int64_t rt_is_complex_obj(int64_t v) { return rt_is_complex_raw(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }

int64_t rt_complex_new(int64_t re, int64_t im) {
  return rt_complex_box_double(get_flt(re), get_flt(im));
}

int64_t rt_complex_new_bits(int64_t re_bits, int64_t im_bits) {
  double re = 0.0, im = 0.0;
  memcpy(&re, &re_bits, 8);
  memcpy(&im, &im_bits, 8);
  return rt_complex_box_double(re, im);
}

int64_t rt_complex_real(int64_t z) { return rt_flt_box_double(rt_complex_re_raw(z)); }

int64_t rt_complex_imag(int64_t z) { return rt_flt_box_double(rt_complex_im_raw(z)); }

int64_t rt_complex_re_bits(int64_t z) {
  double d = rt_complex_re_raw(z);
  int64_t bits = 0;
  memcpy(&bits, &d, 8);
  return bits;
}

int64_t rt_complex_im_bits(int64_t z) {
  double d = rt_complex_im_raw(z);
  int64_t bits = 0;
  memcpy(&bits, &d, 8);
  return bits;
}

extern int64_t rt_bigint_add(int64_t a, int64_t b);
extern int64_t rt_bigint_sub(int64_t a, int64_t b);
extern int64_t rt_bigint_mul(int64_t a, int64_t b);
extern int64_t rt_bigint_div(int64_t a, int64_t b);
extern int64_t rt_bigint_mod(int64_t a, int64_t b);
extern int64_t rt_bigint_cmp(int64_t a, int64_t b);
extern int64_t rt_list_new(int64_t n);
extern void _bi_val_to_mpz(int64_t v, mpz_t result);
extern int64_t _bi_from_mpz(const mpz_t val);
extern bool _bi_mpz_fits_small_int(const mpz_t v);
extern int64_t _bi_mpz_get_i64(const mpz_t v);

static inline int rt_is_bigint_obj(int64_t v) {
  if (!is_ptr(v) || !is_heap_ptr(v))
    return 0;
  return *(int64_t *)((char *)(uintptr_t)v - 8) == TAG_BIGINT;
}

static inline bool rt_heap_tag_raw(int64_t v, int64_t *tag) {
  if (!is_ptr(v) || !is_heap_ptr(v))
    return false;
  *tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  return true;
}

static inline bool rt_is_list_tuple_tag(int64_t tag) {
  return tag == TAG_LIST || tag == TAG_TUPLE;
}

static inline int64_t rt_seq_len_raw(int64_t v) {
  int64_t n = *(int64_t *)((char *)(uintptr_t)v + 0);
  return is_int(n) ? (n >> 1) : n;
}

static inline int64_t rt_seq_item_raw(int64_t v, int64_t i) {
  return *(int64_t *)((char *)(uintptr_t)v + 16 + i * 8);
}

static inline int64_t rt_range_field_raw(int64_t v, int64_t off) {
  int64_t raw = *(int64_t *)((char *)(uintptr_t)v + off);
  return is_int(raw) ? (raw >> 1) : raw;
}

static inline bool rt_is_range_obj(int64_t v, int64_t tag) {
  if (tag != TAG_RANGE)
    return false;
  size_t hsz = rt_get_heap_size(v);
  return hsz >= 24 && hsz <= 32;
}

static inline int64_t rt_math_range_len_raw(int64_t v) {
  int64_t start = rt_range_field_raw(v, 0);
  int64_t stop = rt_range_field_raw(v, 8);
  int64_t step = rt_range_field_raw(v, 16);
  if (step == 0)
    return 0;
  if (step > 0) {
    if (start >= stop)
      return 0;
    return ((stop - start - 1) / step) + 1;
  }
  if (start <= stop)
    return 0;
  return ((start - stop - 1) / -step) + 1;
}

static inline int64_t rt_range_item_raw(int64_t v, int64_t i) {
  int64_t start = rt_range_field_raw(v, 0);
  int64_t step = rt_range_field_raw(v, 16);
  return rt_tag_v(start + i * step);
}

static inline bool rt_is_seq_or_range(int64_t v, int64_t tag) {
  return rt_is_list_tuple_tag(tag) || rt_is_range_obj(v, tag);
}

static inline int64_t rt_seq_or_range_len_raw(int64_t v, int64_t tag) {
  return rt_is_range_obj(v, tag) ? rt_math_range_len_raw(v) : rt_seq_len_raw(v);
}

static inline int64_t rt_seq_or_range_item_raw(int64_t v, int64_t tag, int64_t i) {
  return rt_is_range_obj(v, tag) ? rt_range_item_raw(v, i) : rt_seq_item_raw(v, i);
}

static int64_t rt_seq_concat(int64_t a, int64_t b, int64_t a_tag, int64_t b_tag) {
  int64_t na = rt_seq_len_raw(a);
  int64_t nb = rt_seq_len_raw(b);
  if (na < 0 || nb < 0 || na > (INT64_MAX - nb))
    return 0;
  int64_t total = na + nb;
  int64_t out = rt_list_new(rt_tag_v(total));
  if (!out)
    return 0;
  if (a_tag == TAG_TUPLE && b_tag == TAG_TUPLE)
    *(int64_t *)((char *)(uintptr_t)out - 8) = TAG_TUPLE;
  if (na > 0)
    memcpy((char *)(uintptr_t)out + 16, (char *)(uintptr_t)a + 16, (size_t)na * sizeof(int64_t));
  if (nb > 0)
    memcpy((char *)(uintptr_t)out + 16 + (size_t)na * sizeof(int64_t),
           (char *)(uintptr_t)b + 16, (size_t)nb * sizeof(int64_t));
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(total);
  return out;
}

typedef struct {
  uint64_t add_int_fast;
  uint64_t add_bigint;
  uint64_t add_float;
  uint64_t add_ptr_int;
  uint64_t add_str;
  uint64_t add_other;
  uint64_t sub_int_fast;
  uint64_t sub_bigint;
  uint64_t sub_float;
  uint64_t sub_ptr_int;
  uint64_t sub_other;
  uint64_t mul_int_fast;
  uint64_t mul_bigint;
  uint64_t mul_float;
  uint64_t mul_str_repeat;
  uint64_t mul_list_repeat;
  uint64_t mul_other;
  uint64_t div_int_fast;
  uint64_t div_bigint;
  uint64_t div_float;
  uint64_t div_zero;
  uint64_t div_other;
  uint64_t mod_int_fast;
  uint64_t mod_bigint;
  uint64_t mod_zero;
  uint64_t mod_other;
  uint64_t eq_same;
  uint64_t eq_bigint;
  uint64_t eq_int_mixed;
  uint64_t eq_float;
  uint64_t eq_str;
  uint64_t eq_ptr;
  uint64_t eq_other;
  uint64_t cmp_int_fast;
  uint64_t cmp_bigint;
  uint64_t cmp_float;
  uint64_t cmp_str;
  uint64_t cmp_ptr;
  uint64_t cmp_other;
  uint64_t bigint_promotions_add;
  uint64_t bigint_promotions_sub;
  uint64_t bigint_promotions_mul;
} ny_math_stats_t;

static ny_math_stats_t g_math_stats = {0};
static int g_math_stats_enabled = -1;

static inline bool math_stats_enabled(void) {
  if (g_math_stats_enabled >= 0)
    return g_math_stats_enabled != 0;
  const char *env = getenv("NYTRIX_MATH_STATS");
  g_math_stats_enabled = (env && (*env == '1' || strcmp(env, "true") == 0)) ? 1 : 0;
  return g_math_stats_enabled != 0;
}

#define MATH_STAT_INC(field)                                                                       \
  do {                                                                                             \
    if (__builtin_expect(g_math_stats_enabled == 1, 0))                                            \
      g_math_stats.field++;                                                                        \
    else if (__builtin_expect(g_math_stats_enabled < 0, 0) && math_stats_enabled())                \
      g_math_stats.field++;                                                                        \
  } while (0)

__attribute__((destructor)) static void rt_math_stats_dump(void) {
  if (!math_stats_enabled())
    return;
  fprintf(stderr, "\nNytrix math stats\n");
  fprintf(stderr,
          "  add: int=%" PRIu64 " bigint=%" PRIu64 " float=%" PRIu64 " ptr_int=%" PRIu64
          " str=%" PRIu64 " other=%" PRIu64 "\n",
          g_math_stats.add_int_fast, g_math_stats.add_bigint, g_math_stats.add_float,
          g_math_stats.add_ptr_int, g_math_stats.add_str, g_math_stats.add_other);
  fprintf(stderr,
          "  sub: int=%" PRIu64 " bigint=%" PRIu64 " float=%" PRIu64 " ptr_int=%" PRIu64
          " other=%" PRIu64 "\n",
          g_math_stats.sub_int_fast, g_math_stats.sub_bigint, g_math_stats.sub_float,
          g_math_stats.sub_ptr_int, g_math_stats.sub_other);
  fprintf(stderr,
          "  mul: int=%" PRIu64 " bigint=%" PRIu64 " float=%" PRIu64 " str=%" PRIu64
          " list=%" PRIu64 " other=%" PRIu64 "\n",
          g_math_stats.mul_int_fast, g_math_stats.mul_bigint, g_math_stats.mul_float,
          g_math_stats.mul_str_repeat, g_math_stats.mul_list_repeat, g_math_stats.mul_other);
  fprintf(stderr,
          "  div: int=%" PRIu64 " bigint=%" PRIu64 " float=%" PRIu64 " zero=%" PRIu64
          " other=%" PRIu64 "\n",
          g_math_stats.div_int_fast, g_math_stats.div_bigint, g_math_stats.div_float,
          g_math_stats.div_zero, g_math_stats.div_other);
  fprintf(stderr, "  mod: int=%" PRIu64 " bigint=%" PRIu64 " zero=%" PRIu64 " other=%" PRIu64 "\n",
          g_math_stats.mod_int_fast, g_math_stats.mod_bigint, g_math_stats.mod_zero,
          g_math_stats.mod_other);
  fprintf(stderr,
          "  eq: same=%" PRIu64 " bigint=%" PRIu64 " int_mixed=%" PRIu64 " float=%" PRIu64
          " str=%" PRIu64 " ptr=%" PRIu64 " other=%" PRIu64 "\n",
          g_math_stats.eq_same, g_math_stats.eq_bigint, g_math_stats.eq_int_mixed,
          g_math_stats.eq_float, g_math_stats.eq_str, g_math_stats.eq_ptr, g_math_stats.eq_other);
  fprintf(stderr,
          "  cmp: int=%" PRIu64 " bigint=%" PRIu64 " float=%" PRIu64 " str=%" PRIu64 " ptr=%" PRIu64
          " other=%" PRIu64 "\n",
          g_math_stats.cmp_int_fast, g_math_stats.cmp_bigint, g_math_stats.cmp_float,
          g_math_stats.cmp_str, g_math_stats.cmp_ptr, g_math_stats.cmp_other);
  fprintf(stderr, "  bigint promotions: add=%" PRIu64 " sub=%" PRIu64 " mul=%" PRIu64 "\n",
          g_math_stats.bigint_promotions_add, g_math_stats.bigint_promotions_sub,
          g_math_stats.bigint_promotions_mul);
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
  rt_float_cache_forget((uintptr_t)v);
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
  int64_t out = (int64_t)(uintptr_t)((char *)slot + 8);
  rt_float_cache_store((uintptr_t)out);
  return out;
}

static inline int64_t rt_flt_box_double(double d) {
  int64_t bits;
  memcpy(&bits, &d, 8);
  return rt_flt_box_val(bits);
}

int64_t rt_flt_box_val32(int64_t bits32) {
  uint32_t raw = (uint32_t)rt_untag_v(bits32);
  float f;
  memcpy(&f, &raw, 4);
  return rt_flt_box_double((double)f);
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
    int64_t bits;
    memcpy(&bits, &d, 8);
    return bits;
  }
  return 0;
}

int64_t rt_flt_to_int(int64_t v) {
  double d = get_flt(v);
  return rt_tag_v((int64_t)d);
}

int64_t rt_flt_trunc(int64_t v) { return rt_flt_to_int(v); }

int64_t rt_complex_add(int64_t a, int64_t b) {
  return rt_complex_box_double(rt_complex_re_raw(a) + rt_complex_re_raw(b),
                               rt_complex_im_raw(a) + rt_complex_im_raw(b));
}

int64_t rt_complex_sub(int64_t a, int64_t b) {
  return rt_complex_box_double(rt_complex_re_raw(a) - rt_complex_re_raw(b),
                               rt_complex_im_raw(a) - rt_complex_im_raw(b));
}

int64_t rt_complex_mul(int64_t a, int64_t b) {
  double ar = rt_complex_re_raw(a), ai = rt_complex_im_raw(a);
  double br = rt_complex_re_raw(b), bi = rt_complex_im_raw(b);
  return rt_complex_box_double(ar * br - ai * bi, ar * bi + ai * br);
}

int64_t rt_complex_div(int64_t a, int64_t b) {
  double ar = rt_complex_re_raw(a), ai = rt_complex_im_raw(a);
  double br = rt_complex_re_raw(b), bi = rt_complex_im_raw(b);
  double den = br * br + bi * bi;
  if (den == 0.0)
    return rt_division_by_zero();
  return rt_complex_box_double((ar * br + ai * bi) / den, (ai * br - ar * bi) / den);
}

int64_t rt_complex_conj(int64_t z) {
  return rt_complex_box_double(rt_complex_re_raw(z), -rt_complex_im_raw(z));
}

int64_t rt_complex_abs2(int64_t z) {
  double re = rt_complex_re_raw(z), im = rt_complex_im_raw(z);
  return rt_flt_box_double(re * re + im * im);
}

int64_t rt_complex_eq(int64_t a, int64_t b) {
  return (rt_complex_re_raw(a) == rt_complex_re_raw(b) &&
          rt_complex_im_raw(a) == rt_complex_im_raw(b))
             ? NY_IMM_TRUE
             : NY_IMM_FALSE;
}

#define FLT_OP(name, op)                                                                           \
  int64_t rt_flt_##name(int64_t a, int64_t b) {                                                    \
    double da = get_flt(a);                                                                        \
    double db = get_flt(b);                                                                        \
    double r = da op db;                                                                           \
    return rt_flt_box_double(r);                                                                   \
  }

FLT_OP(add, +)
FLT_OP(sub, -)
FLT_OP(mul, *)

int64_t rt_flt_div(int64_t a, int64_t b) {
  double da = get_flt(a);
  double db = get_flt(b);
  if (db == 0.0)
    return rt_division_by_zero();
  return rt_flt_box_double(da / db);
}

#define FLT_CMP(name, op)                                                                          \
  int64_t rt_flt_##name(int64_t a, int64_t b) {                                                    \
    double da = get_flt(a);                                                                        \
    double db = get_flt(b);                                                                        \
    return (da op db) ? NY_IMM_TRUE : NY_IMM_FALSE;                                                \
  }

FLT_CMP(lt, <)
FLT_CMP(gt, >)
FLT_CMP(le, <=)
FLT_CMP(ge, >=)
FLT_CMP(eq, ==)

int64_t rt_flt_is_nan(int64_t v) {
  double d = get_flt(v);
  return isnan(d) ? NY_IMM_TRUE : NY_IMM_FALSE;
}

int64_t rt_flt_is_inf(int64_t v) {
  double d = get_flt(v);
  return isinf(d) ? NY_IMM_TRUE : NY_IMM_FALSE;
}

int64_t rt_flt_nan(void) { return rt_flt_box_double(NAN); }

int64_t rt_flt_inf(void) { return rt_flt_box_double(INFINITY); }

int64_t rt_flt_hash(int64_t v) {
  double d = get_flt(v);
  uint64_t bits = 0;
  memcpy(&bits, &d, 8);
  if (bits == 0x8000000000000000ULL)
    bits = 0;
  if (isnan(d))
    bits = 0x7ff8000000000000ULL;
  uint64_t h = bits ^ (bits >> 33);
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return rt_tag_v((int64_t)(h & 0x7fffffffULL));
}

int64_t rt_flt_sin(int64_t v) { return rt_flt_box_double(sin(get_flt(v))); }
int64_t rt_flt_cos(int64_t v) { return rt_flt_box_double(cos(get_flt(v))); }
int64_t rt_flt_tan(int64_t v) { return rt_flt_box_double(tan(get_flt(v))); }
int64_t rt_flt_asin(int64_t v) { return rt_flt_box_double(asin(get_flt(v))); }
int64_t rt_flt_acos(int64_t v) { return rt_flt_box_double(acos(get_flt(v))); }
int64_t rt_flt_atan(int64_t v) { return rt_flt_box_double(atan(get_flt(v))); }
int64_t rt_flt_atan2(int64_t y, int64_t x) {
  return rt_flt_box_double(atan2(get_flt(y), get_flt(x)));
}
int64_t rt_flt_sqrt(int64_t v) { return rt_flt_box_double(sqrt(get_flt(v))); }
int64_t rt_flt_exp(int64_t v) { return rt_flt_box_double(exp(get_flt(v))); }
int64_t rt_flt_log(int64_t v) { return rt_flt_box_double(log(get_flt(v))); }
int64_t rt_flt_log2(int64_t v) { return rt_flt_box_double(log2(get_flt(v))); }
int64_t rt_flt_log10(int64_t v) { return rt_flt_box_double(log10(get_flt(v))); }
int64_t rt_flt_floor(int64_t v) { return rt_tag_v((int64_t)floor(get_flt(v))); }
int64_t rt_flt_ceil(int64_t v) { return rt_tag_v((int64_t)ceil(get_flt(v))); }
int64_t rt_flt_round(int64_t v) { return rt_tag_v((int64_t)llround(get_flt(v))); }
int64_t rt_flt_fmod(int64_t a, int64_t b) { return rt_flt_box_double(fmod(get_flt(a), get_flt(b))); }
int64_t rt_flt_pow(int64_t a, int64_t b) { return rt_flt_box_double(pow(get_flt(a), get_flt(b))); }

int64_t rt_add(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t av = a >> 1;
    int64_t bv = b >> 1;
    int64_t raw = 0;
    if (__builtin_add_overflow(av, bv, &raw) || !ny_small_int_fits_i64(raw)) {
      MATH_STAT_INC(bigint_promotions_add);
      MATH_STAT_INC(add_bigint);
      return rt_bigint_add(a, b);
    }
    MATH_STAT_INC(add_int_fast);
    return rt_tag_v(raw);
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(add_bigint);
    return rt_bigint_add(a, b);
  }

  if (is_any_ptr(a) && (b & 1)) {
    MATH_STAT_INC(add_ptr_int);
    if (is_v_flt_mapped(a))
      return rt_flt_add(a, b);
    return a + (b >> 1);
  }
  if ((a & 1) && is_any_ptr(b)) {
    MATH_STAT_INC(add_ptr_int);
    if (is_v_flt_mapped(b))
      return rt_flt_add(a, b);
    return b + (a >> 1);
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(add_float);
    return rt_flt_add(a, b);
  }
  if (is_v_str(a) && is_v_str(b)) {
    MATH_STAT_INC(add_str);
    return rt_str_concat(a, b);
  }
  int64_t a_tag = 0, b_tag = 0;
  if (rt_heap_tag_raw(a, &a_tag) && rt_heap_tag_raw(b, &b_tag) &&
      rt_is_list_tuple_tag(a_tag) && rt_is_list_tuple_tag(b_tag)) {
    return rt_seq_concat(a, b, a_tag, b_tag);
  }
  MATH_STAT_INC(add_other);
  return 1;
}

int64_t rt_sub(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t av = a >> 1;
    int64_t bv = b >> 1;
    int64_t raw = 0;
    if (__builtin_sub_overflow(av, bv, &raw) || !ny_small_int_fits_i64(raw)) {
      MATH_STAT_INC(bigint_promotions_sub);
      MATH_STAT_INC(sub_bigint);
      return rt_bigint_sub(a, b);
    }
    MATH_STAT_INC(sub_int_fast);
    return rt_tag_v(raw);
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(sub_bigint);
    return rt_bigint_sub(a, b);
  }
  if (is_any_ptr(a) && (b & 1)) {
    MATH_STAT_INC(sub_ptr_int);
    if (is_v_flt_mapped(a))
      return rt_flt_sub(a, b);
    return a - (b >> 1);
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(sub_float);
    return rt_flt_sub(a, b);
  }
  MATH_STAT_INC(sub_other);
  return 1;
}

int64_t rt_mul(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t av = a >> 1;
    int64_t bv = b >> 1;
    int64_t raw = 0;
    if (__builtin_mul_overflow(av, bv, &raw) || !ny_small_int_fits_i64(raw)) {
      MATH_STAT_INC(bigint_promotions_mul);
      MATH_STAT_INC(mul_bigint);
      return rt_bigint_mul(a, b);
    }
    MATH_STAT_INC(mul_int_fast);
    return rt_tag_v(raw);
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(mul_bigint);
    return rt_bigint_mul(a, b);
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(mul_float);
    return rt_flt_mul(a, b);
  }

  if (is_v_str(a) && (b & 1)) {
    MATH_STAT_INC(mul_str_repeat);
    int64_t count = b >> 1;
    if (count <= 0)
      return rt_alloc_string_len("", 0);
    const char *s_str = (const char *)(uintptr_t)a;
    int64_t s_len = (*(int64_t *)((char *)s_str - 16)) >> 1;
    if (s_len <= 0)
      return a;
    int64_t total_len = s_len * count;
    int64_t res = rt_malloc((int64_t)((total_len + 1) * sizeof(char) << 1) | 1);
    if (!res)
      return 0;
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = (int64_t)((total_len << 1) | 1);
    char *res_str = (char *)(uintptr_t)res;
    for (int64_t i = 0; i < count; i++)
      memcpy(res_str + i * s_len, s_str, s_len);
    res_str[total_len] = '\0';
    return res;
  }
  if ((a & 1) && is_v_str(b)) {
    return rt_mul(b, a);
  }

  if (is_ptr(a) && (b & 1) && is_heap_ptr(a) &&
      *(int64_t *)((char *)(uintptr_t)a - 8) == TAG_LIST) {
    MATH_STAT_INC(mul_list_repeat);
    int64_t count = b >> 1;
    if (count <= 0)
      return rt_list_new(1);
    int64_t len_v = *(int64_t *)((char *)(uintptr_t)a + 0);
    int64_t l_len = is_int(len_v) ? (len_v >> 1) : len_v;
    if (l_len <= 0)
      return rt_list_new(1);
    int64_t total_len = l_len * count;
    int64_t res = rt_list_new((total_len << 1) | 1);
    *(int64_t *)((char *)(uintptr_t)res + 0) = (total_len << 1) | 1;
    for (int64_t i = 0; i < count; i++) {
      memcpy((char *)(uintptr_t)res + 16 + i * l_len * 8, (char *)(uintptr_t)a + 16, l_len * 8);
    }
    return res;
  }
  if ((a & 1) && is_ptr(b) && is_heap_ptr(b) &&
      *(int64_t *)((char *)(uintptr_t)b - 8) == TAG_LIST) {
    return rt_mul(b, a);
  }

  MATH_STAT_INC(mul_other);
  return 1;
}

int64_t rt_div(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0) {
      MATH_STAT_INC(div_zero);
      return rt_division_by_zero();
    }
    MATH_STAT_INC(div_int_fast);
    return rt_tag_v((a >> 1) / bv);
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(div_bigint);
    return rt_bigint_div(a, b);
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(div_float);
    return rt_flt_div(a, b);
  }
  MATH_STAT_INC(div_other);
  return 1;
}

int64_t rt_mod(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    int64_t bv = b >> 1;
    if (bv == 0) {
      MATH_STAT_INC(mod_zero);
      return rt_modulo_by_zero();
    }
    MATH_STAT_INC(mod_int_fast);
    return rt_tag_v((a >> 1) % bv);
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(mod_bigint);
    return rt_bigint_mod(a, b);
  }
  MATH_STAT_INC(mod_other);
  return 1;
}

int64_t rt_eq(int64_t a, int64_t b) {
  if (a == b) {
    MATH_STAT_INC(eq_same);
    return NY_IMM_TRUE;
  }
  bool a_big = rt_is_bigint_obj(a);
  bool b_big = rt_is_bigint_obj(b);
  if (a_big || b_big) {
    if ((!a_big && !is_int(a)) || (!b_big && !is_int(b)))
      return NY_IMM_FALSE;
    MATH_STAT_INC(eq_bigint);
    return rt_bigint_cmp(a, b) == rt_tag_v(0) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if ((rt_is_nil_imm(a) && b == rt_tag_v(0)) || (a == rt_tag_v(0) && rt_is_nil_imm(b))) {
    MATH_STAT_INC(eq_int_mixed);
    return NY_IMM_TRUE;
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(eq_float);
    return rt_flt_eq(a, b);
  }
  if ((a & 1) != (b & 1)) {
    MATH_STAT_INC(eq_int_mixed);
    return NY_IMM_FALSE;
  }
  if (is_ptr(a) && is_ptr(b)) {
    if (a <= NY_IMM_FALSE || b <= NY_IMM_FALSE) {
      MATH_STAT_INC(eq_ptr);
      return NY_IMM_FALSE;
    }
    if (rt_is_complex_raw(a) || rt_is_complex_raw(b)) {
      MATH_STAT_INC(eq_float);
      return rt_complex_eq(a, b);
    }
    if (is_v_str(a) && is_v_str(b)) {
      MATH_STAT_INC(eq_str);
      uintptr_t la_p = (uintptr_t)a - 16;
      uintptr_t lb_p = (uintptr_t)b - 16;
      int64_t la_tagged = *(int64_t *)la_p;
      int64_t lb_tagged = *(int64_t *)lb_p;
      if (la_tagged != lb_tagged)
        return NY_IMM_FALSE;
      size_t la = (size_t)(la_tagged >> 1);
      if (la == 0)
        return NY_IMM_TRUE;
      return memcmp((const void *)(uintptr_t)a, (const void *)(uintptr_t)b, la) == 0 ? NY_IMM_TRUE
                                                                                      : NY_IMM_FALSE;
    }
    int64_t a_tag = 0, b_tag = 0;
    if (rt_heap_tag_raw(a, &a_tag) && rt_heap_tag_raw(b, &b_tag) && rt_is_seq_or_range(a, a_tag) &&
        rt_is_seq_or_range(b, b_tag)) {
      int64_t la = rt_seq_or_range_len_raw(a, a_tag);
      int64_t lb = rt_seq_or_range_len_raw(b, b_tag);
      if (la != lb)
        return NY_IMM_FALSE;
      for (int64_t i = 0; i < la; i++) {
        if (rt_eq(rt_seq_or_range_item_raw(a, a_tag, i), rt_seq_or_range_item_raw(b, b_tag, i)) !=
            NY_IMM_TRUE)
          return NY_IMM_FALSE;
      }
      return NY_IMM_TRUE;
    }
    MATH_STAT_INC(eq_ptr);
  }
  MATH_STAT_INC(eq_other);
  return NY_IMM_FALSE;
}

static inline int rt_str_cmp3(int64_t a, int64_t b) {
  const char *sa = (const char *)(uintptr_t)a;
  const char *sb = (const char *)(uintptr_t)b;
  size_t la = (size_t)(*(int64_t *)((char *)sa - 16) >> 1);
  size_t lb = (size_t)(*(int64_t *)((char *)sb - 16) >> 1);
  size_t n = la < lb ? la : lb;
  if (n) {
    int cmp = memcmp(sa, sb, n);
    if (cmp != 0)
      return cmp;
  }
  if (la < lb)
    return -1;
  if (la > lb)
    return 1;
  return 0;
}

int64_t rt_lt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    MATH_STAT_INC(cmp_int_fast);
    return (a >> 1) < (b >> 1) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(cmp_bigint);
    return (rt_bigint_cmp(a, b) >> 1) < 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(cmp_float);
    return rt_flt_lt(a, b);
  }
  if (is_v_str(a) && is_v_str(b)) {
    MATH_STAT_INC(cmp_str);
    return rt_str_cmp3(a, b) < 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_ptr(a) && is_ptr(b)) {
    MATH_STAT_INC(cmp_ptr);
    return a < b ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  MATH_STAT_INC(cmp_other);
  return NY_IMM_FALSE;
}
int64_t rt_le(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    MATH_STAT_INC(cmp_int_fast);
    return (a >> 1) <= (b >> 1) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(cmp_bigint);
    return (rt_bigint_cmp(a, b) >> 1) <= 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(cmp_float);
    return rt_flt_le(a, b);
  }
  if (is_v_str(a) && is_v_str(b)) {
    MATH_STAT_INC(cmp_str);
    return rt_str_cmp3(a, b) <= 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_ptr(a) && is_ptr(b)) {
    MATH_STAT_INC(cmp_ptr);
    return a <= b ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  MATH_STAT_INC(cmp_other);
  return NY_IMM_FALSE;
}
int64_t rt_gt(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    MATH_STAT_INC(cmp_int_fast);
    return (a >> 1) > (b >> 1) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(cmp_bigint);
    return (rt_bigint_cmp(a, b) >> 1) > 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(cmp_float);
    return rt_flt_gt(a, b);
  }
  if (is_v_str(a) && is_v_str(b)) {
    MATH_STAT_INC(cmp_str);
    return rt_str_cmp3(a, b) > 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_ptr(a) && is_ptr(b)) {
    MATH_STAT_INC(cmp_ptr);
    return a > b ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  MATH_STAT_INC(cmp_other);
  return NY_IMM_FALSE;
}
int64_t rt_ge(int64_t a, int64_t b) {
  if ((a & 1) && (b & 1)) {
    MATH_STAT_INC(cmp_int_fast);
    return (a >> 1) >= (b >> 1) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    MATH_STAT_INC(cmp_bigint);
    return (rt_bigint_cmp(a, b) >> 1) >= 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_v_flt(a) || is_v_flt(b)) {
    MATH_STAT_INC(cmp_float);
    return rt_flt_ge(a, b);
  }
  if (is_v_str(a) && is_v_str(b)) {
    MATH_STAT_INC(cmp_str);
    return rt_str_cmp3(a, b) >= 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (is_ptr(a) && is_ptr(b)) {
    MATH_STAT_INC(cmp_ptr);
    return a >= b ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  MATH_STAT_INC(cmp_other);
  return NY_IMM_FALSE;
}

int64_t rt_and(int64_t a, int64_t b) {
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    mpz_t ma, mb, mr;
    _bi_val_to_mpz(a, ma);
    _bi_val_to_mpz(b, mb);
    mpz_init(mr);
    mpz_and(mr, ma, mb);
    int64_t r;
    if (_bi_mpz_fits_small_int(mr))
      r = rt_tag_v(_bi_mpz_get_i64(mr));
    else
      r = _bi_from_mpz(mr);
    mpz_clear(ma);
    mpz_clear(mb);
    mpz_clear(mr);
    return r;
  }
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) & (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t rt_or(int64_t a, int64_t b) {
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    mpz_t ma, mb, mr;
    _bi_val_to_mpz(a, ma);
    _bi_val_to_mpz(b, mb);
    mpz_init(mr);
    mpz_ior(mr, ma, mb);
    int64_t r;
    if (_bi_mpz_fits_small_int(mr))
      r = rt_tag_v(_bi_mpz_get_i64(mr));
    else
      r = _bi_from_mpz(mr);
    mpz_clear(ma);
    mpz_clear(mb);
    mpz_clear(mr);
    return r;
  }
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) | (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t rt_xor(int64_t a, int64_t b) {
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    mpz_t ma, mb, mr;
    _bi_val_to_mpz(a, ma);
    _bi_val_to_mpz(b, mb);
    mpz_init(mr);
    mpz_xor(mr, ma, mb);
    int64_t r;
    if (_bi_mpz_fits_small_int(mr))
      r = rt_tag_v(_bi_mpz_get_i64(mr));
    else
      r = _bi_from_mpz(mr);
    mpz_clear(ma);
    mpz_clear(mb);
    mpz_clear(mr);
    return r;
  }
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) ^ (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t rt_shl(int64_t a, int64_t b) {
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    mpz_t ma, mb, mr;
    _bi_val_to_mpz(a, ma);
    _bi_val_to_mpz(b, mb);
    mpz_init(mr);
    if (mpz_sgn(mb) < 0) {
      mpz_set(mr, ma);
    } else {
      mp_bitcnt_t shift = (mp_bitcnt_t)mpz_get_ui(mb);
      mpz_mul_2exp(mr, ma, shift);
    }
    int64_t r;
    if (_bi_mpz_fits_small_int(mr))
      r = rt_tag_v(_bi_mpz_get_i64(mr));
    else
      r = _bi_from_mpz(mr);
    mpz_clear(ma);
    mpz_clear(mb);
    mpz_clear(mr);
    return r;
  }
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) << (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t rt_shr(int64_t a, int64_t b) {
  if (rt_is_bigint_obj(a) || rt_is_bigint_obj(b)) {
    mpz_t ma, mb, mr;
    _bi_val_to_mpz(a, ma);
    _bi_val_to_mpz(b, mb);
    mpz_init(mr);
    if (mpz_sgn(mb) < 0) {
      mpz_set(mr, ma);
    } else {
      mp_bitcnt_t shift = (mp_bitcnt_t)mpz_get_ui(mb);
      mpz_fdiv_q_2exp(mr, ma, shift);
    }
    int64_t r;
    if (_bi_mpz_fits_small_int(mr))
      r = rt_tag_v(_bi_mpz_get_i64(mr));
    else
      r = _bi_from_mpz(mr);
    mpz_clear(ma);
    mpz_clear(mb);
    mpz_clear(mr);
    return r;
  }
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) >> (uint64_t)(b & 1 ? b >> 1 : b))) << 1 | 1);
}
int64_t rt_not(int64_t a) {
  if (rt_is_bigint_obj(a)) {
    mpz_t ma, mr;
    _bi_val_to_mpz(a, ma);
    mpz_init(mr);
    mpz_com(mr, ma);
    int64_t r;
    if (_bi_mpz_fits_small_int(mr))
      r = rt_tag_v(_bi_mpz_get_i64(mr));
    else
      r = _bi_from_mpz(mr);
    mpz_clear(ma);
    mpz_clear(mr);
    return r;
  }
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
