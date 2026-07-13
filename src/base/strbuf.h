#ifndef NY_STRBUF_H
#define NY_STRBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} ny_strbuf_t;

static inline void ny_strbuf_init(ny_strbuf_t *b) {
  if (!b)
    return;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static inline void ny_strbuf_free(ny_strbuf_t *b) {
  if (!b)
    return;
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static inline bool ny_strbuf_reserve(ny_strbuf_t *b, size_t need) {
  if (!b)
    return false;
  if (b->len + need <= b->cap)
    return true;
  size_t new_cap = b->cap ? b->cap : 256;
  while (new_cap < b->len + need) {
    if (new_cap > (SIZE_MAX / 2))
      return false;
    new_cap *= 2;
  }
  char *p = realloc(b->data, new_cap + 1);
  if (!p)
    return false;
  b->data = p;
  b->cap = new_cap;
  return true;
}

static inline bool ny_strbuf_append_n(ny_strbuf_t *b, const char *s, size_t n) {
  if (!b || (!s && n > 0))
    return false;
  if (n == 0)
    return true;
  if (!ny_strbuf_reserve(b, n + 1))
    return false;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return true;
}

static inline bool ny_strbuf_append(ny_strbuf_t *b, const char *s) {
  if (!b || !s)
    return false;
  return ny_strbuf_append_n(b, s, strlen(s));
}

static inline bool ny_strbuf_append_c(ny_strbuf_t *b, char c) {
  if (!b)
    return false;
  if (!ny_strbuf_reserve(b, 2))
    return false;
  b->data[b->len++] = c;
  b->data[b->len] = '\0';
  return true;
}

static inline bool ny_strbuf_appendf(ny_strbuf_t *b, const char *fmt, ...) {
  if (!b || !fmt)
    return false;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0)
    return false;
  if (!ny_strbuf_reserve(b, (size_t)n + 1))
    return false;
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return true;
}

static inline bool ny_strbuf_json_str(ny_strbuf_t *b, const char *s) {
  if (!b)
    return false;
  if (!s)
    return ny_strbuf_append(b, "null");
  if (!ny_strbuf_append_c(b, '"'))
    return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    switch (*p) {
    case '"':
      if (!ny_strbuf_append(b, "\\\""))
        return false;
      break;
    case '\\':
      if (!ny_strbuf_append(b, "\\\\"))
        return false;
      break;
    case '\n':
      if (!ny_strbuf_append(b, "\\n"))
        return false;
      break;
    case '\r':
      if (!ny_strbuf_append(b, "\\r"))
        return false;
      break;
    case '\t':
      if (!ny_strbuf_append(b, "\\t"))
        return false;
      break;
    default:
      if (*p < 0x20) {
        if (!ny_strbuf_appendf(b, "\\u%04x", (unsigned)*p))
          return false;
      } else {
        if (!ny_strbuf_append_c(b, (char)*p))
          return false;
      }
      break;
    }
  }
  return ny_strbuf_append_c(b, '"');
}

static inline char *ny_strbuf_take(ny_strbuf_t *b) {
  if (!b)
    return NULL;
  char *r = b->data;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return r;
}

#endif
