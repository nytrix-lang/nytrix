#include "rt/shared.h"
#include <inttypes.h>
#include <stdlib.h>

static void rt_val_to_str_info(int64_t v, char *buf, size_t bsize, const char **out_s,
                               int *out_len) {
  if (is_v_str(v)) {
    *out_s = (const char *)(uintptr_t)v;
    *out_len = (int)rt_tagged_str_len(v);
  } else if (rt_is_nil_imm(v)) {
    *out_s = "none";
    *out_len = 4;
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
        extern int64_t rt_bigint_to_str(int64_t a);
        int64_t s_obj = rt_bigint_to_str(v);
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
    *out_s = "none";
    *out_len = 4;
  }
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

typedef struct rt_string_builder_t {
  char *buf;
  size_t len;
  size_t cap;
} rt_string_builder_t;

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
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_v;
  if (!b || !b->buf || b->len == 0)
    return rt_alloc_string_len("", 0);
  return rt_alloc_string_len(b->buf, b->len);
}

int64_t rt_str_builder_free(int64_t builder_v) {
  rt_string_builder_t *b = (rt_string_builder_t *)(uintptr_t)builder_v;
  if (!b)
    return 0;
  free(b->buf);
  free(b);
  return 0;
}

int64_t rt_str_hash(int64_t v) {
  if (!is_v_str(v))
    return rt_tag_v(0);
  size_t n = rt_tagged_str_len(v);
  const unsigned char *s = (const unsigned char *)(uintptr_t)v;
  uint64_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h = ((h ^ (uint64_t)s[i]) * 16777619u) & 2147483647u;
  }
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
} _str_none = {((4ULL << 1) | 1), TAG_STR_CONST, "none"},
  _str_true = {((4ULL << 1) | 1), TAG_STR_CONST, "true"},
  _str_false = {((5ULL << 1) | 1), TAG_STR_CONST, "false"};

int64_t rt_to_str(int64_t v) {
  if (is_v_str(v))
    return v;
  if (rt_is_nil_imm(v))
    return (int64_t)(uintptr_t)_str_none.s;
  /* Immediate booleans must be checked before is_int() because the current
     ABI uses even immediates for bool while small ints use odd tags. */
  if (rt_is_true_imm(v))
    return (int64_t)(uintptr_t)_str_true.s;
  if (rt_is_false_imm(v))
    return (int64_t)(uintptr_t)_str_false.s;
  char buf[128];
  const char *s;
  int len;
  rt_val_to_str_info(v, buf, sizeof(buf), &s, &len);
  if (len > (int)sizeof(buf) && s == buf)
    len = sizeof(buf);
  return rt_alloc_string_len(s, len);
}
