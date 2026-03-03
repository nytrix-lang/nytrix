#include "rt/shared.h"
#include <inttypes.h>

static void rt_val_to_str_info(int64_t v, char *buf, size_t bsize,
                               const char **out_s, int *out_len) {
  if (is_v_str(v)) {
    *out_s = (const char *)(uintptr_t)v;
    uintptr_t lp = (uintptr_t)v - 16;
    int64_t tagged_len = 0;
    memcpy(&tagged_len, (const void *)lp, sizeof(tagged_len));
    *out_len = (int)(tagged_len >> 1);
  } else if (is_int(v)) {
    *out_len = snprintf(buf, bsize, "%" PRId64, (int64_t)(v >> 1));
    *out_s = buf;
  } else if (is_ptr(v)) {
    if (is_v_flt(v)) {
      double d;
      memcpy(&d, (void *)(uintptr_t)v, 8);
      *out_len = snprintf(buf, bsize, "%g", d);
      *out_s = buf;
      return;
    } else if (is_heap_ptr(v)) {
      int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
      *out_len = snprintf(buf, bsize, "<ptr 0x%lx tag=%ld>", (unsigned long)v,
                          (long)tag);
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

int64_t __str_concat(int64_t a, int64_t b) {
  char buf_a[128], buf_b[128];
  const char *sa, *sb;
  int la, lb;

  rt_val_to_str_info(a, buf_a, sizeof(buf_a), &sa, &la);
  rt_val_to_str_info(b, buf_b, sizeof(buf_b), &sb, &lb);

  if (!sa || !sb)
    return 0;

  if (la > (int)sizeof(buf_a) && sa == buf_a) la = sizeof(buf_a);
  if (lb > (int)sizeof(buf_b) && sb == buf_b) lb = sizeof(buf_b);

  int64_t res = __malloc((int64_t)((la + lb + 1) << 1 | 1));
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

static struct {
  uint64_t len_tag;
  uint64_t type_tag;
  char s[8];
} _str_none = {((4ULL << 1) | 1), TAG_STR_CONST, "none"},
  _str_true = {((4ULL << 1) | 1), TAG_STR_CONST, "true"},
  _str_false = {((5ULL << 1) | 1), TAG_STR_CONST, "false"};

int64_t __to_str(int64_t v) {
  if (is_v_str(v))
    return v;
  if (v == 0)
    return (int64_t)(uintptr_t)_str_none.s;
  if (v == 2)
    return (int64_t)(uintptr_t)_str_true.s;
  if (v == 4)
    return (int64_t)(uintptr_t)_str_false.s;
  char buf[128];
  const char *s;
  int len;
  rt_val_to_str_info(v, buf, sizeof(buf), &s, &len);
  if (len > (int)sizeof(buf) && s == buf) len = sizeof(buf);
  return __rt_alloc_string_len(s, len);
}
