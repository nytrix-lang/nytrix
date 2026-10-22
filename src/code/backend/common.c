#include "code/backend/internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nyr_set_error(char *err, size_t err_len, const char *fmt, ...) {
  if (!err || err_len == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
}

static bool nyr_writer_reserve(nyr_writer_t *w, size_t add) {
  if (!w || add > SIZE_MAX - w->len - 1)
    return false;
  size_t need = w->len + add + 1;
  if (need <= w->cap)
    return true;
  size_t cap = w->cap ? w->cap : 4096;
  while (cap < need) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  char *data = realloc(w->data, cap);
  if (!data)
    return false;
  w->data = data;
  w->cap = cap;
  return true;
}

bool nyr_put(nyr_writer_t *w, const char *s) {
  if (!s)
    return true;
  size_t n = strlen(s);
  if (!nyr_writer_reserve(w, n))
    return false;
  memcpy(w->data + w->len, s, n + 1);
  w->len += n;
  return true;
}

bool nyr_printf(nyr_writer_t *w, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || !nyr_writer_reserve(w, (size_t)n)) {
    va_end(copy);
    return false;
  }
  vsnprintf(w->data + w->len, w->cap - w->len, fmt, copy);
  va_end(copy);
  w->len += (size_t)n;
  return true;
}
