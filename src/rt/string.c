#include "rt/shared.h"

int64_t __str_concat(int64_t a, int64_t b) {
  char buf_a[512], buf_b[512];
  const char *sa = NULL, *sb = NULL;
  if (is_v_str(a))
    sa = (const char *)(uintptr_t)a;
  else if (is_int(a)) {
    snprintf(buf_a, sizeof(buf_a), "%ld", a >> 1);
    sa = buf_a;
  } else if (is_v_flt(a)) {
    double d;
    memcpy(&d, (void *)(uintptr_t)a, 8);
    snprintf(buf_a, sizeof(buf_a), "%g", d);
    sa = buf_a;
  } else if (a == 2)
    sa = "true";
  else if (a == 4)
    sa = "false";
  else if (a == 0)
    sa = "none";
  else {
    snprintf(buf_a, sizeof(buf_a), "<ptr 0x%lx>", (unsigned long)a);
    sa = buf_a;
  }
  if (is_v_str(b))
    sb = (const char *)(uintptr_t)b;
  else if (is_int(b)) {
    snprintf(buf_b, sizeof(buf_b), "%ld", b >> 1);
    sb = buf_b;
  } else if (is_v_flt(b)) {
    double d;
    memcpy(&d, (void *)(uintptr_t)b, 8);
    snprintf(buf_b, sizeof(buf_b), "%g", d);
    sb = buf_b;
  } else if (b == 2)
    sb = "true";
  else if (b == 4)
    sb = "false";
  else if (b == 0)
    sb = "none";
  else {
    snprintf(buf_b, sizeof(buf_b), "<ptr 0x%lx>", (unsigned long)b);
    sb = buf_b;
  }
  if (!sa || !sb)
    return 0;
  size_t la = strlen(sa);
  size_t lb = strlen(sb);
  int64_t res = __malloc((int64_t)((la + lb + 1) << 1 | 1));
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = (int64_t)(((la + lb) << 1) | 1);
  char *s = (char *)(uintptr_t)res;
  __copy_mem(s, sa, la);
  __copy_mem(s + la, sb, lb);
  s[la + lb] = '\0';
  return res;
}

int64_t __to_str(int64_t v) {
  if (v == 0) {
    int64_t res = __malloc((5 << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = (4 << 1) | 1;
    strcpy((char *)(uintptr_t)res, "none");
    return res;
  }
  if (v == 2) {
    int64_t res = __malloc((5 << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = (4 << 1) | 1;
    strcpy((char *)(uintptr_t)res, "true");
    return res;
  }
  if (v == 4) {
    int64_t res = __malloc((6 << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = (5 << 1) | 1;
    strcpy((char *)(uintptr_t)res, "false");
    return res;
  }
  if (v & 1) { // is_int
    int64_t val = v >> 1;
    char buf[64];
    int len = sprintf(buf, "%ld", val);
    int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((void *)(uintptr_t)res, buf, (size_t)len + 1);
    return res;
  }
  if ((v & 3) == 2) {
    char buf[64];
    int len = sprintf(buf, "<fn 0x%lx>", (unsigned long)(v & ~3ULL));
    int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((void *)(uintptr_t)res, buf, (size_t)len + 1);
    return res;
  }

  if (is_v_str(v)) {
    return v;
  }

  if (is_heap_ptr(v)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    if (tag == TAG_FLOAT) {
      double d;
      memcpy(&d, (void *)(uintptr_t)v, 8);
      char buf[64];
      int len = sprintf(buf, "%g", d);
      int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
      *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
      *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
      __copy_mem((void *)(uintptr_t)res, buf, (size_t)len + 1);
      return res;
    }
    // Generic Nytrix heap object
    char buf[64];
    int len = sprintf(buf, "<ptr 0x%lx tag=%ld>", (unsigned long)v, (long)tag);
    int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((void *)(uintptr_t)res, buf, (size_t)len + 1);
    return res;
  }
  if (is_ptr(v)) {
    char buf[64];
    int len = sprintf(buf, "<ptr 0x%lx>", (unsigned long)v);
    int64_t res = __malloc(((int64_t)(len + 1) << 1) | 1);
    *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
    *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((void *)(uintptr_t)res, buf, (size_t)len + 1);
    return res;
  }
  return __to_str(0);
}
