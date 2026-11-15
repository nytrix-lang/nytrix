/* Shared generic helpers for Nytrix fuzz and comparison subcommands. */
#ifndef NYTRIX_TEST_FUZZ_SHARED_H
#define NYTRIX_TEST_FUZZ_SHARED_H

#include "core.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef void (*fuzz_shape_visit_t)(const char *path, void *context);

static inline bool fuzz_parse_long(const char **cursor, long *value) {
  const char *p = *cursor;
  char *end = NULL;
  long parsed = strtol(p, &end, 10);
  if (end == p)
    return false;
  *cursor = end;
  *value = parsed;
  return true;
}

static inline void fuzz_walk_shape_files(const char *dir,
                                         fuzz_shape_visit_t visit,
                                         void *context) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (n <= 0 || (size_t)n >= sizeof(path))
      continue;
    struct stat st;
    if (stat(path, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      fuzz_walk_shape_files(path, visit, context);
    else if (S_ISREG(st.st_mode) && ny_has_suffix(path, ".nshape"))
      visit(path, context);
  }
  closedir(d);
}

static inline bool fuzz_contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle)
    return false;
  size_t n = strlen(needle);
  for (const char *p = haystack; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      ++i;
    if (i == n)
      return true;
  }
  return false;
}

static inline void fuzz_file_hash_hex(const char *path, char *out,
                                      size_t out_sz) {
  if (out_sz)
    out[0] = '\0';
  if (!path || !*path || !out_sz)
    return;
  file_buf_t f = {0};
  if (!read_file(path, &f))
    return;
  snprintf(out, out_sz, "%016" PRIx64, fnv1a64(f.data, f.len));
  free(f.data);
}

static inline const char *
fuzz_canonical_generator_method(const char *generator) {
  if (!generator || !*generator || strcmp(generator, "mixed") == 0 ||
      strcmp(generator, "auto") == 0)
    return "mixed";
  if (strcmp(generator, "generate") == 0 || strcmp(generator, "typed") == 0)
    return "typed";
  if (strcmp(generator, "ir") == 0)
    return "ir";
  if (strcmp(generator, "stress") == 0)
    return "stress";
  return generator;
}

static inline int fuzz_double_cmp(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

#endif
