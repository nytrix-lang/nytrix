/*
 * String runtime: UTF-8 string operations, formatting, parsing,
 * concatenation, slicing, and StringBuilder-backed construction.
 */
#include "rt/shared.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static void rt_val_to_str_info(int64_t v, char *buf, size_t bsize, const char **out_s,
                               int *out_len) {
  if (is_v_str(v)) {
    *out_s = (const char *)(uintptr_t)v;
    *out_len = (int)rt_tagged_str_len(v);
  } else if (rt_is_nil_imm(v)) {
    *out_s = "nil";
    *out_len = 3;
  } else if (rt_is_true_imm(v)) {
    *out_s = "true";
    *out_len = 4;
  } else if (rt_is_false_imm(v)) {
    *out_s = "false";
    *out_len = 5;
  } else if (is_int(v)) {
    int64_t val = (int64_t)(v >> 1);
    char *p = buf + bsize - 1;
    *p = '\0';
    int len = 0;
    uint64_t abs_v = (val < 0) ? (uint64_t)-val : (uint64_t)val;
    do {
      *--p = (char)('0' + (abs_v % 10));
      abs_v /= 10;
      len++;
    } while (abs_v);
    if (val < 0) {
      *--p = '-';
      len++;
    }
    *out_len = len;
    *out_s = p;
  } else if (is_ptr(v)) {
    if (is_v_flt(v)) {
      double d;
      memcpy(&d, (void *)(uintptr_t)v, 8);
      *out_len = snprintf(buf, bsize, "%g", d);
      *out_s = buf;
      return;
    } else if (is_heap_ptr(v)) {
      int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
      if (tag == TAG_COMPLEX) {
        double re = 0.0, im = 0.0;
        memcpy(&re, (const void *)(uintptr_t)v, 8);
        memcpy(&im, (const void *)((uintptr_t)v + 8), 8);
        *out_len = snprintf(buf, bsize, "%g%+gi", re, im);
        *out_s = buf;
        return;
      }
      if (tag == TAG_BIGINT) {
        int64_t s_obj = rt_bigint_to_str(v);
        *out_s = (const char *)(uintptr_t)s_obj;
        *out_len = (int)rt_tagged_str_len(s_obj);
        return;
      }
      if (tag == TAG_BIGFLOAT) {
        int64_t s_obj = rt_bigfloat_to_str(v);
        *out_s = (const char *)(uintptr_t)s_obj;
        *out_len = (int)rt_tagged_str_len(s_obj);
        return;
      }
      *out_len = snprintf(buf, bsize, "<ptr 0x%lx tag=%ld>", (unsigned long)v, (long)tag);
      *out_s = buf;
    } else if ((v & 3) == 2) {
      *out_len = snprintf(buf, bsize, "<fn 0x%lx>", (unsigned long)(v & ~3ULL));
      *out_s = buf;
    } else {
      *out_len = snprintf(buf, bsize, "<ptr 0x%lx>", (unsigned long)v);
      *out_s = buf;
    }
  } else {
    *out_s = "nil";
    *out_len = 3;
  }
}

/*
 * Raw C-string helpers used by the NYIR native-only path.  Native literals are
 * emitted directly into the object, so this path deliberately keeps them as
 * NUL-terminated pointers instead of constructing tagged runtime strings.
 */
int64_t rt_native_cstr_concat(int64_t a_ptr, int64_t b_ptr) {
  const char *a = (const char *)(uintptr_t)a_ptr;
  const char *b = (const char *)(uintptr_t)b_ptr;
  if (!a)
    a = "";
  if (!b)
    b = "";
  size_t a_len = strlen(a), b_len = strlen(b);
  if (a_len > SIZE_MAX - b_len - 1)
    return 0;
  char *joined = malloc(a_len + b_len + 1);
  if (!joined)
    return 0;
  memcpy(joined, a, a_len);
  memcpy(joined + a_len, b, b_len + 1);
  return (int64_t)(uintptr_t)joined;
}
typedef struct rt_string_builder_t {
  char *buf;
  size_t len;
  size_t cap;
} rt_string_builder_t;

static int rt_str_builder_reserve(rt_string_builder_t *b, size_t need);

/*
 * Native NYIR owns these builders exclusively until finalize.  Finalize
 * transfers the backing C string and frees only the builder wrapper.
 */
int64_t rt_native_cstr_builder_new(int64_t initial_ptr) {
  const char *initial = (const char *)(uintptr_t)initial_ptr;
  if (!initial)
    initial = "";
  size_t len = strlen(initial);
  rt_string_builder_t *b = calloc(1, sizeof(*b));
  if (!b)
    return 0;
  b->cap = len > 63 ? len : 64;
  b->buf = malloc(b->cap + 1);
  if (!b->buf) {
    free(b);
    return 0;
  }
  memcpy(b->buf, initial, len + 1);
  b->len = len;
  return (int64_t)(uintptr_t)b;
}

int64_t rt_native_cstr_builder_append(int64_t builder_ptr, int64_t suffix_ptr) {
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_ptr;
  const char *suffix = (const char *)(uintptr_t)suffix_ptr;
  if (!b || !suffix)
    return 0;
  size_t suffix_len = strlen(suffix);
  if (suffix_len > SIZE_MAX - b->len - 1 ||
      !rt_str_builder_reserve(b, b->len + suffix_len))
    return 0;
  memcpy(b->buf + b->len, suffix, suffix_len + 1);
  b->len += suffix_len;
  return builder_ptr;
}

int64_t rt_native_cstr_builder_finalize(int64_t builder_ptr) {
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_ptr;
  if (!b)
    return 0;
  char *buf = b->buf;
  free(b);
  return (int64_t)(uintptr_t)buf;
}

int64_t rt_native_cstr_replace(int64_t s_ptr, int64_t old_ptr, int64_t new_ptr) {
  const char *s = (const char *)(uintptr_t)s_ptr;
  const char *old = (const char *)(uintptr_t)old_ptr;
  const char *new_s = (const char *)(uintptr_t)new_ptr;
  if (!s)
    s = "";
  if (!old)
    old = "";
  if (!new_s)
    new_s = "";
  size_t s_len = strlen(s), old_len = strlen(old), new_len = strlen(new_s);
  if (old_len == 0) {
    char *copy = malloc(s_len + 1);
    if (!copy)
      return 0;
    memcpy(copy, s, s_len + 1);
    return (int64_t)(uintptr_t)copy;
  }
  size_t matches = 0;
  for (const char *p = s; (p = strstr(p, old)) != NULL; p += old_len)
    matches++;
  size_t out_len = s_len;
  if (matches > 0 && new_len > old_len) {
    size_t growth = new_len - old_len;
    if (matches > (SIZE_MAX - s_len - 1) / growth)
      return 0;
    out_len = s_len + matches * growth;
  } else if (matches > 0) {
    out_len = s_len - matches * (old_len - new_len);
  }
  char *out = malloc(out_len + 1);
  if (!out)
    return 0;
  char *dst = out;
  const char *cur = s;
  const char *hit = NULL;
  while ((hit = strstr(cur, old)) != NULL) {
    size_t prefix = (size_t)(hit - cur);
    memcpy(dst, cur, prefix);
    dst += prefix;
    memcpy(dst, new_s, new_len);
    dst += new_len;
    cur = hit + old_len;
  }
  strcpy(dst, cur);
  return (int64_t)(uintptr_t)out;
}

int64_t rt_native_i64_min(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t rt_native_i64_max(int64_t a, int64_t b) { return a > b ? a : b; }
double rt_native_f64_min(double a, double b) { return a < b ? a : b; }
double rt_native_f64_max(double a, double b) { return a > b ? a : b; }


/*
 * Format a raw signed 64-bit integer as a freshly-allocated C string.
 * Used by the native-only path for f-string interpolation of integer
 * expressions (mirrors rt_print_i64_raw's formatting but returns a string).
 */
int64_t rt_native_i64_to_cstr(int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
  if (n < 0 || (size_t)n >= sizeof(buf))
    return 0;
  char *s = malloc((size_t)n + 1);
  if (!s)
    return 0;
  memcpy(s, buf, (size_t)n + 1);
  return (int64_t)(uintptr_t)s;
}

/*
 * Format a raw f64 as a freshly-allocated C string (mirrors the runtime's
 * %g formatting so to_str(f) agrees with print(f) and the JIT path).
 */
int64_t rt_native_f64_to_cstr(double v) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", v);
  if (n < 0 || (size_t)n >= sizeof(buf))
    return 0;
  char *s = malloc((size_t)n + 1);
  if (!s)
    return 0;
  memcpy(s, buf, (size_t)n + 1);
  return (int64_t)(uintptr_t)s;
}

/*
 * Convert a tagged Nytrix value to an owned raw C string for native-only
 * interpolation.  This preserves the runtime's bool, nil, integer, float,
 * string, bigint, and pointer formatting instead of treating the tagged word
 * as an unboxed integer.
 */
int64_t rt_native_any_to_cstr(int64_t v) {
  char buf[128];
  const char *s = NULL;
  int len = 0;
  rt_val_to_str_info(v, buf, sizeof(buf), &s, &len);
  if (!s || len < 0 || (size_t)len > SIZE_MAX - 1)
    return 0;
  char *out = malloc((size_t)len + 1);
  if (!out)
    return 0;
  memcpy(out, s, (size_t)len);
  out[len] = '\0';
  return (int64_t)(uintptr_t)out;
}

/*
 * Unbox a tagged dynamic value for a native NYIR f64 operation.
 */
double rt_native_any_to_f64(int64_t v) {
  if (is_int(v))
    return (double)(v >> 1);
  if (is_v_flt(v))
    return rt_flt_unbox_double(v);
  return 0.0;
}

int64_t rt_native_cstr_eq(int64_t a_ptr, int64_t b_ptr) {
  const char *a = (const char *)(uintptr_t)a_ptr;
  const char *b = (const char *)(uintptr_t)b_ptr;
  if (!a || !b)
    return a == b;
  return strcmp(a, b) == 0;
}

int64_t rt_native_assert_cstr(int64_t condition, int64_t message_ptr) {
  if (condition)
    return 0;
  const char *message = (const char *)(uintptr_t)message_ptr;
  fprintf(stderr, "Nytrix assertion failed%s%s\n", message ? ": " : "",
          message ? message : "");
  abort();
}

int64_t rt_str_concat(int64_t a, int64_t b) {
  char buf_a[128], buf_b[128];
  const char *sa, *sb;
  int la, lb;

  rt_val_to_str_info(a, buf_a, sizeof(buf_a), &sa, &la);
  rt_val_to_str_info(b, buf_b, sizeof(buf_b), &sb, &lb);

  if (!sa || !sb)
    return 0;

  if (la > (int)sizeof(buf_a) && sa == buf_a)
    la = sizeof(buf_a);
  if (lb > (int)sizeof(buf_b) && sb == buf_b)
    lb = sizeof(buf_b);

  if (la + lb <= 23) {
    char small[24];
    memcpy(small, sa, la);
    memcpy(small + la, sb, lb);
    small[la + lb] = '\0';
    return rt_alloc_string_len(small, (size_t)(la + lb));
  }

  int64_t res = rt_malloc((int64_t)((la + lb + 1) << 1 | 1));
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = (int64_t)(((la + lb) << 1) | 1);
  char *s = (char *)(uintptr_t)res;
  memcpy(s, sa, la);
  memcpy(s + la, sb, lb);
  s[la + lb] = '\0';
  return res;
}

int64_t rt_str_builder_new(int64_t cap_v) {
  int64_t cap_i = is_int(cap_v) ? (cap_v >> 1) : cap_v;
  if (cap_i < 64)
    cap_i = 64;
  rt_string_builder_t *b = (rt_string_builder_t *)calloc(1, sizeof(*b));
  if (!b)
    return 0;
  b->buf = (char *)malloc((size_t)cap_i + 1);
  if (!b->buf) {
    free(b);
    return 0;
  }
  b->cap = (size_t)cap_i;
  b->buf[0] = '\0';
  return (int64_t)(uintptr_t)b;
}

static int rt_str_builder_reserve(rt_string_builder_t *b, size_t need) {
  if (!b)
    return 0;
  if (need + 1 <= b->cap)
    return 1;
  size_t next = b->cap ? b->cap : 64;
  while (need + 1 > next) {
    size_t grown = next * 2;
    if (grown <= next) {
      next = need + 1;
      break;
    }
    next = grown;
  }
  char *nbuf = (char *)realloc(b->buf, next + 1);
  if (!nbuf)
    return 0;
  b->buf = nbuf;
  b->cap = next;
  return 1;
}

int64_t rt_str_builder_append(int64_t builder_v, int64_t value) {
  if (!is_ptr(builder_v))
    return builder_v;
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_v;
  if (!b)
    return 0;
  char tmp[128];
  const char *s = NULL;
  int len = 0;
  rt_val_to_str_info(value, tmp, sizeof(tmp), &s, &len);
  if (!s || len <= 0)
    return builder_v;
  size_t slen = (size_t)len;
  size_t need = b->len + slen;
  if (!rt_str_builder_reserve(b, need))
    return builder_v;
  memcpy(b->buf + b->len, s, slen);
  b->len = need;
  b->buf[b->len] = '\0';
  return builder_v;
}

int64_t rt_str_builder_to_str(int64_t builder_v) {
  if (!is_ptr(builder_v))
    return rt_alloc_string_len("", 0);
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_v;
  if (!b || !b->buf || b->len == 0)
    return rt_alloc_string_len("", 0);
  return rt_alloc_string_len(b->buf, b->len);
}

int64_t rt_str_builder_free(int64_t builder_v) {
  if (!is_ptr(builder_v))
    return 0;
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_v;
  if (!b)
    return 0;
  free(b->buf);
  free(b);
  return 0;
}

/*
 * Thread-local string interning.  The table owns only canonical pointers; the
 * strings retain their normal runtime lifetime.  It grows at 70% occupancy so
 * repeated long strings get the same pointer just like SSO strings, while
 * lookup remains bounded and equality can use the a == b fast path.
 */
typedef struct {
  uint64_t hash;
  int64_t ptr;
  size_t len;
} rt_intern_slot_t;

static _Thread_local rt_intern_slot_t *g_intern = NULL;
static _Thread_local size_t g_intern_cap = 0;
static _Thread_local size_t g_intern_count = 0;

static uint64_t rt_intern_fnv(const unsigned char *s, size_t n) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < n; i++) {
    h ^= s[i];
    h *= UINT64_C(1099511628211);
  }
  return h ? h : 1;
}

static bool rt_str_intern_resize(size_t cap) {
  rt_intern_slot_t *next = calloc(cap, sizeof(*next));
  if (!next)
    return false;
  for (size_t i = 0; i < g_intern_cap; ++i) {
    if (!g_intern[i].ptr)
      continue;
    size_t at = (size_t)g_intern[i].hash & (cap - 1);
    while (next[at].ptr)
      at = (at + 1) & (cap - 1);
    next[at] = g_intern[i];
  }
  free(g_intern);
  g_intern = next;
  g_intern_cap = cap;
  return true;
}

int64_t rt_str_intern_lookup(const char *s, size_t len) {
  if (!s || !g_intern_cap)
    return 0;
  uint64_t h = rt_intern_fnv((const unsigned char *)s, len);
  size_t idx = (size_t)h & (g_intern_cap - 1);
  for (size_t i = 0; i < g_intern_cap; ++i) {
    rt_intern_slot_t *slot = &g_intern[idx];
    if (!slot->ptr)
      return 0;
    if (slot->hash == h && slot->len == len &&
        memcmp((const char *)(uintptr_t)slot->ptr, s, len) == 0)
      return slot->ptr;
    idx = (idx + 1) & (g_intern_cap - 1);
  }
  return 0;
}

void rt_str_intern_insert(const char *s, size_t len, int64_t str) {
  if (!s || !str)
    return;
  if (!g_intern_cap && !rt_str_intern_resize(256))
    return;
  if ((g_intern_count + 1) * 10 >= g_intern_cap * 7) {
    if (g_intern_cap > SIZE_MAX / 2 || !rt_str_intern_resize(g_intern_cap * 2))
      return;
  }
  uint64_t h = rt_intern_fnv((const unsigned char *)s, len);
  size_t idx = (size_t)h & (g_intern_cap - 1);
  while (g_intern[idx].ptr) {
    if (g_intern[idx].hash == h && g_intern[idx].len == len &&
        memcmp((const char *)(uintptr_t)g_intern[idx].ptr, s, len) == 0)
      return;
    idx = (idx + 1) & (g_intern_cap - 1);
  }
  g_intern[idx] = (rt_intern_slot_t){.hash = h, .ptr = str, .len = len};
  ++g_intern_count;
}

/*
 * Pointer-keyed cache of the 31-bit FNV hash rt_str_hash returns.  Only SSO
 * strings (len <= RT_SSO_MAX) are cached: they are never freed or reused until
 * thread cleanup, so a stale entry can never alias a different live string.
 */
#define RT_STR_HASH_CACHE_BITS 16
#define RT_STR_HASH_CACHE (1u << RT_STR_HASH_CACHE_BITS)

typedef struct {
  int64_t ptr;
  uint64_t hash;
} rt_str_hash_cache_t;

static _Thread_local rt_str_hash_cache_t g_str_hash_cache[RT_STR_HASH_CACHE];

void rt_str_intern_clear(void) {
  free(g_intern);
  g_intern = NULL;
  g_intern_cap = 0;
  g_intern_count = 0;
  memset(g_str_hash_cache, 0, sizeof(g_str_hash_cache));
}

int64_t rt_str_hash(int64_t v) {
  if (!is_v_str(v))
    return rt_tag_v(0);
  uint64_t idx = ((uint64_t)(uintptr_t)v >> 4) & (RT_STR_HASH_CACHE - 1);
  if (g_str_hash_cache[idx].ptr == v)
    return rt_tag_v((int64_t)g_str_hash_cache[idx].hash);
  size_t n = rt_tagged_str_len(v);
  const unsigned char *s = (const unsigned char *)(uintptr_t)v;
  uint64_t h = 2166136261u;
  for (size_t i = 0; i < n; i++)
    h = ((h ^ (uint64_t)s[i]) * 16777619u) & 2147483647u;
  g_str_hash_cache[idx].ptr = v;
  g_str_hash_cache[idx].hash = h;
  return rt_tag_v((int64_t)h);
}

int64_t rt_str_eq(int64_t a, int64_t b) {
  if (a == b)
    return NY_IMM_TRUE;
  if (!is_v_str(a) || !is_v_str(b))
    return NY_IMM_FALSE;
  size_t n = rt_tagged_str_len(a);
  if (n != rt_tagged_str_len(b))
    return NY_IMM_FALSE;
  return memcmp((const void *)(uintptr_t)a, (const void *)(uintptr_t)b, n) == 0 ? NY_IMM_TRUE
                                                                                : NY_IMM_FALSE;
}

static struct {
  uint64_t len_tag;
  uint64_t type_tag;
  char s[8];
} _str_nil = {((3ULL << 1) | 1), TAG_STR_CONST, "nil"},
  _str_true = {((4ULL << 1) | 1), TAG_STR_CONST, "true"},
  _str_false = {((5ULL << 1) | 1), TAG_STR_CONST, "false"};

#define NY_INT_STR_CACHE_SIZE 65536
static int64_t s_cached_int_strs[NY_INT_STR_CACHE_SIZE];

int64_t rt_to_str(int64_t v) {
  if (is_v_str(v))
    return v;
  if (rt_is_nil_imm(v))
    return (int64_t)(uintptr_t)_str_nil.s;

  if (rt_is_true_imm(v))
    return (int64_t)(uintptr_t)_str_true.s;
  if (rt_is_false_imm(v))
    return (int64_t)(uintptr_t)_str_false.s;
  if (is_int(v)) {
    int64_t val = (int64_t)(v >> 1);
    if (val >= 0 && val < NY_INT_STR_CACHE_SIZE) {
      int64_t cached = s_cached_int_strs[val];
      if (cached)
        return cached;
      char ibuf[32];
      int ilen = snprintf(ibuf, sizeof(ibuf), "%lld", (long long)val);
      int64_t s = rt_alloc_string_len(ibuf, ilen);
      s_cached_int_strs[val] = s;
      return s;
    }
  }
  char buf[128];
  const char *s;
  int len;
  rt_val_to_str_info(v, buf, sizeof(buf), &s, &len);
  if (len > (int)sizeof(buf) && s == buf)
    len = sizeof(buf);
  return rt_alloc_string_len(s, len);
}
