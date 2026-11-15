/* Shape parsing and metadata API for the Nytrix fuzz fixture runner. */
#ifndef NYTRIX_TEST_FUZZ_SHAPE_PARSE_H
#define NYTRIX_TEST_FUZZ_SHAPE_PARSE_H

#include "core.h"

static inline bool fuzz_extract_quoted_after(const char *data, const char *key,
                                             char *out, size_t out_sz) {
  const char *p = strstr(data, key);
  if (!p)
    return false;
  p = strchr(p, '"');
  if (!p)
    return false;
  ++p;
  const char *q = strchr(p, '"');
  if (!q)
    return false;
  size_t n = (size_t)(q - p);
  if (n >= out_sz)
    n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static inline bool fuzz_extract_shape_name(const char *data, char *out,
                                           size_t out_sz) {
  const char *p = strstr(data, "shape ");
  if (!p)
    return false;
  p += 6;
  while (*p == ' ' || *p == '\t')
    ++p;
  const char *q = p;
  while (*q && *q != ' ' && *q != '\t' && *q != '{' && *q != '\n')
    ++q;
  size_t n = (size_t)(q - p);
  if (!n)
    return false;
  if (n >= out_sz)
    n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static inline bool fuzz_extract_features(const char *data, char *out,
                                         size_t out_sz) {
  const char *p = strstr(data, "features ");
  if (!p)
    return false;
  p = strchr(p, '[');
  if (!p)
    return false;
  const char *q = strchr(p, ']');
  if (!q)
    return false;
  size_t n = (size_t)(q - p + 1);
  if (n >= out_sz)
    n = out_sz - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static inline int fuzz_extract_int_after(const char *data, const char *key,
                                         int fallback) {
  const char *p = strstr(data, key);
  if (!p)
    return fallback;
  p += strlen(key);
  while (*p == ' ' || *p == '\t' || *p == ':')
    ++p;
  return atoi(p);
}

static inline int fuzz_extract_weight(const char *data, const char *profile) {
  const char *p = strstr(data, profile);
  if (!p)
    return 1;
  p += strlen(profile);
  while (*p == ' ' || *p == '\t' || *p == ':')
    ++p;
  int v = atoi(p);
  return v > 0 ? v : 0;
}

static inline void fuzz_source_from_path(const char *path, char *out,
                                         size_t out_sz) {
  const char *p = strstr(path, "shapes/");
  if (!p)
    p = path;
  snprintf(out, out_sz, "%.*s", (int)out_sz - 1, p);
}

#endif
